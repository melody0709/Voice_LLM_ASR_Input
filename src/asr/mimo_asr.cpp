#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mimo_asr.h"

#include "audio_diagnostics.h"
#include "cloud_asr_common.h"
#include "cloud_http_common.h"
#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <sstream>
#include <wincrypt.h>
#include <winhttp.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "winhttp.lib")

namespace mimo_asr {
namespace {

constexpr size_t kMaxBase64Bytes = 10 * 1024 * 1024;

struct ParsedEndpoint {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    bool useSsl = true;
    std::wstring error;
};

struct RecognizeAttempt {
    bool ok = false;
    bool retryable = false;
    std::wstring text;
    std::wstring errorText;
};

void PutLe16(std::vector<BYTE>& out, size_t offset, uint16_t value) {
    out[offset] = static_cast<BYTE>(value & 0xff);
    out[offset + 1] = static_cast<BYTE>((value >> 8) & 0xff);
}

void PutLe32(std::vector<BYTE>& out, size_t offset, uint32_t value) {
    out[offset] = static_cast<BYTE>(value & 0xff);
    out[offset + 1] = static_cast<BYTE>((value >> 8) & 0xff);
    out[offset + 2] = static_cast<BYTE>((value >> 16) & 0xff);
    out[offset + 3] = static_cast<BYTE>((value >> 24) & 0xff);
}

std::vector<BYTE> BuildWavFromPcm16Mono16k(const std::vector<BYTE>& pcm) {
    if (pcm.empty()) return {};
    if (pcm.size() > 0xffffffffu - 44u) return {};

    std::vector<BYTE> wav(44 + pcm.size());
    std::memcpy(wav.data() + 0, "RIFF", 4);
    PutLe32(wav, 4, static_cast<uint32_t>(36 + pcm.size()));
    std::memcpy(wav.data() + 8, "WAVE", 4);
    std::memcpy(wav.data() + 12, "fmt ", 4);
    PutLe32(wav, 16, 16);
    PutLe16(wav, 20, 1);
    PutLe16(wav, 22, 1);
    PutLe32(wav, 24, 16000);
    PutLe32(wav, 28, 16000 * 2);
    PutLe16(wav, 32, 2);
    PutLe16(wav, 34, 16);
    std::memcpy(wav.data() + 36, "data", 4);
    PutLe32(wav, 40, static_cast<uint32_t>(pcm.size()));
    std::memcpy(wav.data() + 44, pcm.data(), pcm.size());
    return wav;
}

std::string Base64EncodeNoCrLf(const std::vector<BYTE>& data) {
    if (data.empty() || data.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        return {};
    }

    DWORD chars = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), flags, nullptr, &chars) || chars == 0) {
        return {};
    }

    std::string encoded(chars, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()), flags, encoded.data(), &chars)) {
        return {};
    }

    if (chars > 0 && encoded[chars - 1] == '\0') {
        encoded.resize(chars - 1);
    } else {
        encoded.resize(chars);
    }
    return encoded;
}

std::wstring NormalizedLanguage(std::wstring language) {
    language = Trim(language);
    if (language == L"zh" || language == L"en") return language;
    return kDefaultLanguage;
}

ParsedEndpoint ParseEndpoint(std::wstring baseUrl) {
    ParsedEndpoint endpoint;
    baseUrl = Trim(baseUrl);
    if (baseUrl.empty()) baseUrl = kDefaultBaseUrl;
    if (baseUrl.find(L"://") == std::wstring::npos) {
        baseUrl = L"https://" + baseUrl;
    }
    const size_t queryPos = baseUrl.find_first_of(L"?#");
    if (queryPos != std::wstring::npos) {
        baseUrl.resize(queryPos);
    }
    while (!baseUrl.empty() && baseUrl.back() == L'/') {
        baseUrl.pop_back();
    }

    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(baseUrl.c_str(), 0, 0, &parts)) {
        endpoint.error = L"invalid Base URL";
        return endpoint;
    }

    const std::wstring scheme(parts.lpszScheme, parts.dwSchemeLength);
    if (scheme != L"https" && scheme != L"http") {
        endpoint.error = L"Base URL must use http or https";
        return endpoint;
    }
    endpoint.useSsl = (scheme == L"https");
    endpoint.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    endpoint.port = parts.nPort;
    endpoint.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (endpoint.path.empty()) endpoint.path = L"";
    while (!endpoint.path.empty() && endpoint.path.back() == L'/') {
        endpoint.path.pop_back();
    }
    endpoint.path += L"/chat/completions";

    if (endpoint.host.empty()) {
        endpoint.error = L"Base URL host is empty";
    }
    return endpoint;
}

void AppendUtf8Codepoint(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseJsonUnicodeEscape(const std::string& json, size_t pos, uint32_t& value) {
    if (pos + 4 > json.size()) return false;
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int hex = HexValue(json[pos + i]);
        if (hex < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(hex);
    }
    return true;
}

std::wstring ExtractJsonStringDecoded(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return L"";
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return L"";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return L"";
    ++pos;

    std::string out;
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') break;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= json.size()) break;
        const char esc = json[pos++];
        switch (esc) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!ParseJsonUnicodeEscape(json, pos, cp)) break;
            pos += 4;
            if (cp >= 0xd800 && cp <= 0xdbff &&
                pos + 6 <= json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
                uint32_t low = 0;
                if (ParseJsonUnicodeEscape(json, pos + 2, low) && low >= 0xdc00 && low <= 0xdfff) {
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    pos += 6;
                }
            }
            AppendUtf8Codepoint(out, cp);
            break;
        }
        default:
            out.push_back(esc);
            break;
        }
    }

    return Utf8ToWide(out);
}

std::string BuildRequestJson(const MimoConfig& cfg, const std::string& base64Audio) {
    const std::wstring model = Trim(cfg.model).empty() ? kDefaultModel : Trim(cfg.model);
    const std::wstring language = NormalizedLanguage(cfg.language);

    std::string json;
    json.reserve(base64Audio.size() + 512);
    json += "{\"model\":\"";
    json += EscapeJson(model);
    json += "\",\"messages\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_audio\",\"input_audio\":{\"data\":\"data:audio/wav;base64,";
    json += base64Audio;
    json += "\"}}]}],\"asr_options\":{\"language\":\"";
    json += EscapeJson(language);
    json += "\"}}";
    return json;
}

RecognizeAttempt RecognizePrepared(const std::string& requestJson,
                                   const ParsedEndpoint& endpoint,
                                   const MimoConfig& cfg,
                                   DWORD timeoutMs) {
    RecognizeAttempt result;

    CloudHttpRequest req;
    req.method = L"POST";
    req.host = endpoint.host;
    req.port = endpoint.port;
    req.path = endpoint.path;
    req.useSsl = endpoint.useSsl;
    req.timeoutMs = timeoutMs;
    req.headers = L"Content-Type: application/json\r\napi-key: " + Trim(cfg.apiKey) + L"\r\n";
    req.body.assign(requestJson.begin(), requestJson.end());

    CloudHttpResponse response = SendCloudHttpRequest(req);
    if (!response.ok) {
        result.retryable = IsTransientCloudHttpError(response.winhttpError);
        result.errorText = L"MiMo ASR error: " + response.failedStep +
            L" failed (err=" + std::to_wstring(response.winhttpError) + L")";
        return result;
    }

    if (response.statusCode < 200 || response.statusCode >= 300) {
        result.retryable = IsRetryableCloudHttpStatus(response.statusCode);
        std::wstring message = ExtractJsonStringDecoded(response.body, "message");
        if (message.empty()) message = ExtractJsonStringDecoded(response.body, "error");
        result.errorText = L"MiMo ASR error: HTTP " + std::to_wstring(response.statusCode);
        if (!message.empty()) result.errorText += L": " + message;
        return result;
    }

    result.ok = true;
    result.text = ExtractJsonStringDecoded(response.body, "content");
    return result;
}

RecognizeAttempt RecognizeInternal(const std::vector<BYTE>& pcm16k16Mono, const MimoConfig& cfg) {
    RecognizeAttempt result;
    if (pcm16k16Mono.empty()) {
        result.ok = true;
        return result;
    }

    if (Trim(cfg.apiKey).empty()) {
        result.errorText = L"MiMo ASR error: missing API key";
        return result;
    }

    ParsedEndpoint endpoint = ParseEndpoint(cfg.baseUrl);
    if (!endpoint.error.empty()) {
        result.errorText = L"MiMo ASR error: " + endpoint.error;
        return result;
    }

    std::vector<BYTE> wav = BuildWavFromPcm16Mono16k(pcm16k16Mono);
    if (wav.empty()) {
        result.errorText = L"MiMo ASR error: failed to build WAV audio";
        return result;
    }

    std::string base64Audio = Base64EncodeNoCrLf(wav);
    if (base64Audio.empty()) {
        result.errorText = L"MiMo ASR error: failed to encode audio";
        return result;
    }
    if (base64Audio.size() > kMaxBase64Bytes) {
        result.errorText = L"MiMo ASR error: audio exceeds 10MB base64 limit";
        return result;
    }

    const std::string requestJson = BuildRequestJson(cfg, base64Audio);
    const DWORD timeoutMs = std::clamp<DWORD>(
        ComputeCloudAsrRecordedRequestTimeoutMs(0.0, pcm16k16Mono.size()) + 12000,
        15000,
        60000);

    std::wstring lastError;
    for (int attempt = 0; attempt < 2; ++attempt) {
        audio_diagnostics::StageMetadata diagnostic;
        diagnostic.kind = attempt == 0
            ? cfg.diagnosticStageKind
            : audio_diagnostics::RetryStageKind(cfg.diagnosticStageKind);
        diagnostic.index = attempt == 0
            ? cfg.diagnosticStageIndex
            : audio_diagnostics::RetryStageIndex(
                cfg.diagnosticStageKind, cfg.diagnosticStageIndex,
                static_cast<unsigned>(attempt));
        diagnostic.backend = L"mimo";
        diagnostic.model = cfg.model;
        diagnostic.transport = L"batch_http_wav_json";
        diagnostic.reason = attempt == 0 ? L"" : L"transient_http_retry";
        diagnostic.encoding = L"wav_base64_json";
        diagnostic.sentBytes = pcm16k16Mono.size();
        audio_diagnostics::RegisterStageInput(
            cfg.diagnosticAttemptId, diagnostic, pcm16k16Mono);
        RecognizeAttempt r = RecognizePrepared(requestJson, endpoint, cfg, timeoutMs);
        audio_diagnostics::StageTerminal terminal;
        if (r.ok && r.text.empty()) {
            terminal.terminal = "http_success_empty";
            terminal.reason = "no_speech";
        } else if (r.ok) {
            terminal.terminal = "http_success";
            terminal.textChars = r.text.size();
        } else if (r.retryable) {
            terminal.terminal = "transient_http_error";
            terminal.reason = "network";
        } else {
            terminal.terminal = "provider_error";
            terminal.reason = "provider_error";
        }
        audio_diagnostics::CompleteStage(
            cfg.diagnosticAttemptId, diagnostic.kind, diagnostic.index, terminal);
        if (r.ok) return r;
        lastError = r.errorText;
        if (!r.retryable || attempt == 1) break;
        SleepCloudHttpRetryBackoff(attempt);
    }

    result.errorText = lastError.empty() ? L"MiMo ASR error: request failed" : lastError;
    return result;
}

} // namespace

std::wstring Recognize(const std::vector<BYTE>& pcm16k16Mono, const MimoConfig& cfg) {
    RecognizeAttempt result = RecognizeInternal(pcm16k16Mono, cfg);
    if (result.ok) return result.text;
    return result.errorText.empty() ? L"MiMo ASR error: request failed" : result.errorText;
}

TestResult TestConnection(const MimoConfig& cfg) {
    TestResult result;
    if (Trim(cfg.apiKey).empty()) {
        result.message = L"Please fill in MiMo API Key.";
        return result;
    }
    if (Trim(cfg.baseUrl).empty()) {
        result.message = L"Please fill in MiMo Base URL.";
        return result;
    }
    if (Trim(cfg.model).empty()) {
        result.message = L"Please fill in MiMo model.";
        return result;
    }

    std::vector<BYTE> silence(16000 * sizeof(int16_t), 0);
    RecognizeAttempt attempt = RecognizeInternal(silence, cfg);
    if (!attempt.ok) {
        result.message = attempt.errorText.empty() ? L"MiMo ASR request failed." : attempt.errorText;
        return result;
    }

    result.ok = true;
    result.message = L"Connection OK. MiMo ASR endpoint is reachable.";
    return result;
}

} // namespace mimo_asr
