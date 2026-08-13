#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "qwen_audio_http.h"

#include "asr_runtime_log.h"
#include "cloud_asr_common.h"
#include "cloud_http_common.h"
#include "qwen_audio_json.h"
#include "qwen_context.h"
#include "utils.h"

#include <algorithm>
#include <cctype>
#include <cstdarg>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <vector>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace qwen_audio_http {
namespace {

void QwenHttpDebugLog(const char* format, ...) {
#if defined(VOXTYPE_QWEN_AUDIO_PROTOCOL_TEST)
    (void)format;
    return;
#else
    if (!format) return;
    va_list args;
    va_start(args, format);
    asr_runtime_log::WriteNamedV(L"qwen_audio_debug.log", format, args);
    va_end(args);
#endif
}

// The Audio 3 HTTP documentation caps the Base64/Data-URL input at 10 MiB.
// Reserve the Data-URL prefix before calculating the PCM budget.
constexpr size_t kMaxDataUrlBytes = 10u * 1024u * 1024u;
constexpr size_t kDataUrlPrefixBytes = sizeof("data:audio/wav;base64,") - 1;
constexpr size_t kMaxBase64Bytes = kMaxDataUrlBytes - kDataUrlPrefixBytes;
// Base64 rounds the WAV payload up to a multiple of four. Use a conservative
// multiple-of-three WAV budget so the complete Data URL can never exceed the
// documented 10 MiB cap after padding is added.
constexpr size_t kMaxWavBytes = (kMaxBase64Bytes / 4u) * 3u;
constexpr size_t kMaxPcmBytes = kMaxWavBytes > 44u ? kMaxWavBytes - 44u : 0u;

void PutLe16(std::vector<BYTE>& out, size_t pos, uint16_t value) {
    out[pos] = static_cast<BYTE>(value & 0xff);
    out[pos + 1] = static_cast<BYTE>((value >> 8) & 0xff);
}

void PutLe32(std::vector<BYTE>& out, size_t pos, uint32_t value) {
    out[pos] = static_cast<BYTE>(value & 0xff);
    out[pos + 1] = static_cast<BYTE>((value >> 8) & 0xff);
    out[pos + 2] = static_cast<BYTE>((value >> 16) & 0xff);
    out[pos + 3] = static_cast<BYTE>((value >> 24) & 0xff);
}

std::vector<BYTE> BuildWavImpl(const std::vector<BYTE>& pcm) {
    if (pcm.empty() || pcm.size() > 0xffffffffu - 44u) return {};
    std::vector<BYTE> wav(44 + pcm.size());
    std::memcpy(wav.data(), "RIFF", 4);
    PutLe32(wav, 4, static_cast<uint32_t>(36 + pcm.size()));
    std::memcpy(wav.data() + 8, "WAVEfmt ", 8);
    PutLe32(wav, 16, 16);
    PutLe16(wav, 20, 1);
    PutLe16(wav, 22, 1);
    PutLe32(wav, 24, 16000);
    PutLe32(wav, 28, 32000);
    PutLe16(wav, 32, 2);
    PutLe16(wav, 34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    PutLe32(wav, 40, static_cast<uint32_t>(pcm.size()));
    std::memcpy(wav.data() + 44, pcm.data(), pcm.size());
    return wav;
}

std::string Base64Impl(const std::vector<BYTE>& data) {
    if (data.empty() || data.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) return {};
    DWORD chars = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), flags, nullptr, &chars) || chars == 0) return {};
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), flags, out.data(), &chars)) return {};
    if (chars && out[chars - 1] == '\0') --chars;
    out.resize(chars);
    return out;
}

std::string JsonEscape(const std::wstring& value) {
    return EscapeJson(value);
}

std::vector<std::wstring> SplitHints(const std::wstring& raw) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= raw.size() && result.size() < 4) {
        size_t end = raw.find_first_of(L",;", start);
        std::wstring item = Trim(raw.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        bool valid = !item.empty() && item.size() <= 16;
        for (wchar_t ch : item) valid = valid && ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-');
        if (valid) result.push_back(item == L"fil" ? L"tl" : item);
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

void AppendHints(std::string& json, const std::wstring& raw) {
    const auto hints = SplitHints(raw);
    if (hints.empty()) return;
    json += ",\"language_hints\": [";
    for (size_t i = 0; i < hints.size(); ++i) {
        if (i) json += ',';
        json += '"' + JsonEscape(hints[i]) + '"';
    }
    json += ']';
}

struct Endpoint {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool ssl = true;
    std::wstring error;
};

Endpoint ParseEndpoint(std::wstring url) {
    Endpoint e;
    url = Trim(url);
    if (url.empty()) { e.error = L"HTTP Base URL is empty"; return e; }
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { e.error = L"invalid HTTP Base URL"; return e; }
    std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
    if (scheme != L"https") { e.error = L"Qwen Audio HTTP requires HTTPS"; return e; }
    e.ssl = true;
    e.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    e.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (e.path.empty() || e.path == L"/") e.path = L"/api/v1/services/aigc/multimodal-generation/generation";
    while (e.path.size() > 1 && e.path.back() == L'/') e.path.pop_back();
    if (e.path != L"/api/v1/services/aigc/multimodal-generation/generation") {
        e.error = L"Qwen Audio HTTP endpoint path must be /api/v1/services/aigc/multimodal-generation/generation";
        return e;
    }
    if (e.host.empty()) e.error = L"HTTP Base URL host is empty";
    e.port = parts.nPort ? parts.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    return e;
}

std::string BuildRequestImpl(const Config& cfg, const std::string& audio) {
    std::string json = "{\"model\":\"" + JsonEscape(cfg.model) + "\",\"input\":{\"messages\":[";
    std::wstring context = Trim(cfg.inputContextText);
    if (context.size() > qwen_context::kMaxContextCharacters) {
        context = input_context::TakeFirstN(context, qwen_context::kMaxContextCharacters);
    }
    if (!context.empty()) {
        json += "{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"" +
            JsonEscape(context) + "\"}]},";
    }
    json += "{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64," + audio + "\"}}]}],\"parameters\":{\"format\":\"wav\",\"sample_rate\":\"16000\"";
    AppendHints(json, cfg.languageHints);
    if (!Trim(cfg.vocabularyId).empty()) json += ",\"vocabulary_id\":\"" + JsonEscape(cfg.vocabularyId) + "\"";
    if (qwen_audio_json::HasValidVocabulary(cfg.vocabulary)) {
        json += ",\"vocabulary\":" + WideToUtf8(Trim(cfg.vocabulary));
    }
    json += "}}}";
    return json;
}

bool IsNoSpeechResponseImpl(DWORD statusCode, const std::string& responseBody) {
    // Audio 3 HTTP uses a provider error envelope for silence instead of a
    // successful empty transcript. This is a recognition outcome, not an
    // operational failure, so it must enter VoxType's shared no-speech path.
    return statusCode == 400 &&
           responseBody.find("ASR_RESPONSE_HAVE_NO_WORDS") != std::string::npos;
}

} // namespace

std::vector<BYTE> BuildWavForPcm(const std::vector<BYTE>& pcm16k16Mono) {
    return BuildWavImpl(pcm16k16Mono);
}

std::string EncodeBase64ForTest(const std::vector<BYTE>& data) {
    return Base64Impl(data);
}

std::string BuildRequestJsonForTest(const Config& config, const std::string& audioBase64) {
    return BuildRequestImpl(config, audioBase64);
}

std::wstring ParseResponseTextForTest(const std::string& responseBody) {
    static constexpr std::string_view kSentencePath[] = {
        "output", "output", "sentence", "text"
    };
    static constexpr std::string_view kOutputTextPath[] = {"output", "text"};
    std::wstring text = qwen_audio_json::ExtractStringAtPath(responseBody, kSentencePath);
    if (text.empty()) {
        text = qwen_audio_json::ExtractStringAtPath(responseBody, kOutputTextPath);
    }
    return text;
}

bool IsNoSpeechResponseForTest(DWORD statusCode, const std::string& responseBody) {
    return IsNoSpeechResponseImpl(statusCode, responseBody);
}

Result Recognize(const std::vector<BYTE>& pcm,
                 const Config& cfg,
                 DWORD timeoutMs,
                 CloudHttpCancellation* cancellation) {
    Result result;
    if (pcm.empty()) { result.ok = true; return result; }
    if ((pcm.size() & 1u) != 0) {
        result.error = L"Qwen Audio ASR error: PCM byte count must be even";
        return result;
    }
    if (Trim(cfg.apiKey).empty()) { result.error = L"Qwen Audio ASR error: missing API key"; return result; }
    if (!qwen_audio_json::IsValidVocabulary(cfg.vocabulary)) {
        result.error = L"Qwen Audio ASR error: vocabulary must be a valid JSON object with weights 1-5 or 50";
        return result;
    }
    Endpoint endpoint = ParseEndpoint(cfg.baseUrl);
    if (!endpoint.error.empty()) { result.error = L"Qwen Audio ASR error: " + endpoint.error; return result; }
    if (pcm.size() > kMaxPcmBytes) {
        QwenHttpDebugLog("event=http_rejected reason=audio_too_large pcm_bytes=%zu limit_bytes=%zu",
                         pcm.size(), kMaxPcmBytes);
        result.error = L"Qwen Audio ASR error: audio exceeds the maximum request size";
        return result;
    }
    std::vector<BYTE> wav = BuildWavImpl(pcm);
    const std::string encoded = Base64Impl(wav);
    if (encoded.empty() || encoded.size() > kMaxBase64Bytes) {
        result.error = L"Qwen Audio ASR error: audio is empty or exceeds request size limit";
        return result;
    }
    CloudHttpRequest request;
    request.host = endpoint.host;
    request.port = endpoint.port;
    request.path = endpoint.path;
    request.useSsl = endpoint.ssl;
    request.timeoutMs = timeoutMs;
    request.cancellation = cancellation;
    request.headers = L"Content-Type: application/json\r\nAuthorization: Bearer " + Trim(cfg.apiKey) + L"\r\nX-DashScope-SSE: disable\r\n";
    const std::string body = BuildRequestImpl(cfg, encoded);
    request.body.assign(body.begin(), body.end());
    QwenHttpDebugLog("event=http_request_start model=%s host=%s path=%s pcm_bytes=%zu context=%d",
                     WideToUtf8(cfg.model).c_str(),
                     WideToUtf8(endpoint.host).c_str(),
                     WideToUtf8(endpoint.path).c_str(),
                     pcm.size(), cfg.inputContextText.empty() ? 0 : 1);
    const auto started = std::chrono::steady_clock::now();
    CloudHttpResponse response = SendCloudHttpRequest(request);
    result.elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    result.statusCode = response.statusCode;
    QwenHttpDebugLog("event=http_response status=%lu winhttp_error=%lu ok=%d elapsed_ms=%.1f",
                     static_cast<unsigned long>(response.statusCode),
                     static_cast<unsigned long>(response.winhttpError),
                     response.ok ? 1 : 0, result.elapsedMs);
    if (!response.ok) {
        result.retryable = IsTransientCloudHttpError(response.winhttpError);
        result.error = L"Qwen Audio ASR error: " + response.failedStep;
        if (response.winhttpError != 0) {
            result.error += L" (err=" + std::to_wstring(response.winhttpError) + L")";
        }
        return result;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        if (IsNoSpeechResponseImpl(response.statusCode, response.body)) {
            // Keep ok=true with empty text so QwenAudioAsrSession's existing
            // NormalizeAsrText()/ClassifyAsrResult() path reports exactly
            // "No speech detected" and never starts fallback.
            result.ok = true;
            return result;
        }
        result.retryable = IsRetryableCloudHttpStatus(response.statusCode);
        result.error = L"Qwen Audio ASR error: HTTP " + std::to_wstring(response.statusCode);
        std::wstring msg = qwen_audio_json::ExtractString(response.body, "message");
        if (msg.empty()) msg = qwen_audio_json::ExtractString(response.body, "error_message");
        if (!msg.empty()) result.error += L": " + msg;
        return result;
    }
    result.text = ParseResponseTextForTest(response.body);
    result.ok = true;
    return result;
}

TestResult TestConnection(const Config& cfg) {
    TestResult result;
    std::vector<BYTE> silence(16000 * sizeof(int16_t), 0);
    Result r = Recognize(silence, cfg, 30000);
    result.ok = r.ok;
    result.message = r.ok ? L"Connection OK. Qwen Audio HTTP endpoint is reachable."
                          : (r.error.empty() ? L"Qwen Audio HTTP request failed." : r.error);
    return result;
}

} // namespace qwen_audio_http
