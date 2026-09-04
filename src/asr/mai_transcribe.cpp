#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "mai_transcribe.h"

#include "asr_runtime_log.h"
#include "audio_diagnostics.h"
#include "utils.h"

#include <chrono>
#include <cstdarg>
#include <limits>
#include <rpc.h>
#include <string_view>
#include <utility>
#include <wincrypt.h>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "rpcrt4.lib")
#pragma comment(lib, "winhttp.lib")

namespace mai_transcribe {
namespace {

constexpr wchar_t kOpenRouterHost[] = L"openrouter.ai";
constexpr wchar_t kOpenRouterPath[] = L"/api/v1/audio/transcriptions";
constexpr char kOpenRouterModel[] = "microsoft/mai-transcribe-2";
constexpr char kAzureModel[] = "MAI-Transcribe-2";
constexpr wchar_t kAzurePath[] =
    L"/speechtotext/transcriptions:transcribe?api-version=2025-10-15";
constexpr size_t kMaxOpenRouterJsonBytes = 16u * 1024u * 1024u;
constexpr size_t kMaxAzureWavBytes = 250u * 1024u * 1024u;

void MaiDebugLog(const char* format, ...) {
#if defined(VOXTYPE_MAI_PROTOCOL_TEST)
    (void)format;
#else
    if (!format) return;
    va_list args;
    va_start(args, format);
    asr_runtime_log::WriteNamedV(L"mai_asr_debug.log", format, args);
    va_end(args);
#endif
}

bool IsSupportedLanguage(const std::wstring& language) {
    return language.empty() || language == L"auto" || language == L"zh" ||
           language == L"en" || language == L"yue";
}

std::wstring EffectiveLanguage(const std::wstring& language) {
    return language == L"auto" ? std::wstring() : language;
}

bool IsSafeHeaderValue(const std::wstring& value) {
    for (wchar_t ch : value) {
        if (ch == L'\r' || ch == L'\n' || ch < 0x20) return false;
    }
    return true;
}

std::string Base64Encode(const std::vector<BYTE>& data) {
    if (data.empty() ||
        data.size() > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        return {};
    }
    DWORD chars = 0;
    const DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                              flags, nullptr, &chars) || chars == 0) {
        return {};
    }
    std::string output(chars, '\0');
    if (!CryptBinaryToStringA(data.data(), static_cast<DWORD>(data.size()),
                              flags, output.data(), &chars)) {
        return {};
    }
    if (chars > 0 && output[chars - 1] == '\0') --chars;
    output.resize(chars);
    return output;
}

std::string BuildOpenRouterJson(const std::string& audioBase64,
                                const std::wstring& language) {
    std::string json = "{\"model\":\"" + std::string(kOpenRouterModel) +
        "\",\"input_audio\":{\"data\":\"" + audioBase64 +
        "\",\"format\":\"wav\"}";
    const std::wstring effective = EffectiveLanguage(language);
    if (!effective.empty()) {
        json += ",\"language\":\"" + EscapeJson(effective) + "\"";
    }
    json += '}';
    return json;
}

std::string BuildAzureDefinition(const std::wstring& language) {
    std::string json = "{";
    const std::wstring effective = EffectiveLanguage(language);
    if (!effective.empty()) {
        json += "\"locales\":[\"" + EscapeJson(effective) + "\"],";
    }
    json +=
        "\"enhancedMode\":{\"enabled\":true,\"model\":\"" +
        std::string(kAzureModel) + "\","
        "\"modelOptions\":{\"transcribeStyle\":\"clean\"}}}";
    return json;
}

void AppendAscii(std::vector<BYTE>& output, std::string_view text) {
    output.insert(output.end(), text.begin(), text.end());
}

std::vector<BYTE> BuildAzureMultipart(const std::vector<BYTE>& wav,
                                      const std::wstring& language,
                                      const std::string& boundary) {
    if (boundary.empty() || boundary.find_first_of("\r\n") != std::string::npos ||
        wav.size() > kMaxAzureWavBytes) {
        return {};
    }
    const std::string definition = BuildAzureDefinition(language);
    std::vector<BYTE> body;
    body.reserve(wav.size() + definition.size() + boundary.size() * 3u + 320u);
    AppendAscii(body, "--" + boundary + "\r\n");
    AppendAscii(body,
        "Content-Disposition: form-data; name=\"audio\"; filename=\"recording.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n");
    body.insert(body.end(), wav.begin(), wav.end());
    AppendAscii(body, "\r\n--" + boundary + "\r\n");
    AppendAscii(body,
        "Content-Disposition: form-data; name=\"definition\"\r\n"
        "Content-Type: application/json\r\n\r\n");
    AppendAscii(body, definition);
    AppendAscii(body, "\r\n--" + boundary + "--\r\n");
    return body;
}

std::string MakeBoundary() {
    UUID uuid = {};
    const RPC_STATUS created = UuidCreate(&uuid);
    if (created != RPC_S_OK && created != RPC_S_UUID_LOCAL_ONLY) return {};
    RPC_CSTR text = nullptr;
    if (UuidToStringA(&uuid, &text) != RPC_S_OK || !text) return {};
    std::string boundary = "----VoxTypeMAI" +
        std::string(reinterpret_cast<const char*>(text));
    RpcStringFreeA(&text);
    return boundary;
}

struct AzureEndpoint {
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring error;
};

AzureEndpoint ParseAzureEndpoint(const std::wstring& raw) {
    AzureEndpoint endpoint;
    const std::wstring url = Trim(raw);
    if (url.empty()) {
        endpoint.error = L"Azure Endpoint is empty";
        return endpoint;
    }
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUserNameLength = static_cast<DWORD>(-1);
    parts.dwPasswordLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        endpoint.error = L"invalid Azure Endpoint";
        return endpoint;
    }
    if (parts.nScheme != INTERNET_SCHEME_HTTPS) {
        endpoint.error = L"Azure Endpoint requires HTTPS";
        return endpoint;
    }
    if (parts.dwHostNameLength == 0) {
        endpoint.error = L"Azure Endpoint host is empty";
        return endpoint;
    }
    if (parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) {
        endpoint.error = L"Azure Endpoint must not contain user information";
        return endpoint;
    }
    const std::wstring path = parts.dwUrlPathLength == 0
        ? std::wstring()
        : std::wstring(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (!path.empty() && path != L"/") {
        endpoint.error = L"Azure Endpoint must be the resource root URL";
        return endpoint;
    }
    if (parts.dwExtraInfoLength != 0) {
        endpoint.error = L"Azure Endpoint must not contain a query or fragment";
        return endpoint;
    }
    endpoint.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    endpoint.port = parts.nPort ? parts.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    return endpoint;
}

bool TryExtractStringField(const std::string& json,
                           const std::string& key,
                           std::wstring& value) {
    value.clear();
    if (!json_detail::IsValidDocument(json)) return false;
    const size_t pos = json_detail::FindValueForKey(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '"') {
        return false;
    }
    return json_detail::DecodeString(json, pos, value);
}

bool TryParseOpenRouterText(const std::string& body, std::wstring& text) {
    return TryExtractStringField(body, "text", text);
}

bool TryParseAzureText(const std::string& body, std::wstring& text) {
    text.clear();
    if (!json_detail::IsValidDocument(body)) return false;
    size_t pos = json_detail::FindValueForKey(body, "combinedPhrases");
    if (pos == std::string::npos || pos >= body.size() || body[pos] != '[') {
        return false;
    }
    ++pos;
    for (;;) {
        json_detail::SkipWhitespace(body, pos);
        if (pos >= body.size()) return false;
        if (body[pos] == ']') return true;
        if (body[pos] != '{') return false;
        const size_t objectStart = pos;
        if (!json_detail::SkipValue(body, pos)) return false;
        std::wstring phrase;
        if (!TryExtractStringField(
                body.substr(objectStart, pos - objectStart), "text", phrase)) {
            return false;
        }
        if (!phrase.empty()) {
            if (!text.empty()) text += L' ';
            text += phrase;
        }
        json_detail::SkipWhitespace(body, pos);
        if (pos >= body.size()) return false;
        if (body[pos] == ']') return true;
        if (body[pos] != ',') return false;
        ++pos;
    }
}

std::wstring ErrorMessage(const std::string& body) {
    std::wstring message = ExtractJsonStringDecoded(body, "message");
    if (message.empty()) message = ExtractJsonStringDecoded(body, "detail");
    if (message.size() > 512) message.resize(512);
    return message;
}

std::string ProviderCode(const std::string& body, DWORD statusCode) {
    const std::wstring code = ExtractJsonStringDecoded(body, "code");
    return code.empty() ? std::to_string(static_cast<unsigned long>(statusCode))
                        : WideToUtf8(code);
}

} // namespace

std::string EncodeBase64ForTest(const std::vector<BYTE>& data) {
    return Base64Encode(data);
}

std::string BuildOpenRouterJsonForTest(const std::string& audioBase64,
                                       const std::wstring& language) {
    return BuildOpenRouterJson(audioBase64, language);
}

std::vector<BYTE> BuildAzureMultipartForTest(const std::vector<BYTE>& wav,
                                             const std::wstring& language,
                                             const std::string& boundary) {
    return BuildAzureMultipart(wav, language, boundary);
}

std::wstring ParseOpenRouterTextForTest(const std::string& responseBody) {
    std::wstring text;
    return TryParseOpenRouterText(responseBody, text) ? text : std::wstring();
}

std::wstring ParseAzureTextForTest(const std::string& responseBody) {
    std::wstring text;
    return TryParseAzureText(responseBody, text) ? text : std::wstring();
}

bool ValidateAzureEndpointForTest(const std::wstring& endpoint,
                                  std::wstring& error) {
    AzureEndpoint parsed = ParseAzureEndpoint(endpoint);
    error = std::move(parsed.error);
    return error.empty();
}

Result Recognize(const std::vector<BYTE>& pcm,
                 const Config& config,
                 DWORD timeoutMs,
                 CloudHttpCancellation* cancellation) {
    Result result;
    if (pcm.empty()) {
        result.ok = true;
        return result;
    }
    if ((pcm.size() & 1u) != 0) {
        result.error = L"MAI ASR error: PCM byte count must be even";
        return result;
    }
    const std::wstring apiKey = Trim(config.apiKey);
    if (apiKey.empty()) {
        result.error = L"MAI ASR error: missing API key";
        return result;
    }
    if (!IsSafeHeaderValue(apiKey)) {
        result.error = L"MAI ASR error: API key contains invalid characters";
        return result;
    }
    if (!IsSupportedLanguage(config.language)) {
        result.error = L"MAI ASR error: unsupported language";
        return result;
    }

    const std::vector<BYTE> wav = audio_diagnostics::BuildPcm16MonoWav(
        pcm.data(), pcm.size());
    if (wav.empty()) {
        result.error = L"MAI ASR error: audio exceeds WAV size limit";
        return result;
    }

    CloudHttpRequest request;
    request.timeoutMs = timeoutMs;
    request.cancellation = cancellation;

    if (config.apiProvider == ApiProvider::OpenRouter) {
        const std::string encoded = Base64Encode(wav);
        if (encoded.empty()) {
            result.error = L"MAI ASR error: audio Base64 encoding failed";
            return result;
        }
        const std::string body = BuildOpenRouterJson(encoded, config.language);
        if (body.size() > kMaxOpenRouterJsonBytes) {
            result.error = L"MAI ASR error: audio exceeds OpenRouter request size limit";
            return result;
        }
        request.host = kOpenRouterHost;
        request.path = kOpenRouterPath;
        request.headers = L"Content-Type: application/json\r\nAuthorization: Bearer " +
                          apiKey + L"\r\n";
        request.body.assign(body.begin(), body.end());
    } else {
        const AzureEndpoint endpoint = ParseAzureEndpoint(config.azureEndpoint);
        if (!endpoint.error.empty()) {
            result.error = L"MAI ASR error: " + endpoint.error;
            return result;
        }
        const std::string boundary = MakeBoundary();
        if (boundary.empty()) {
            result.error = L"MAI ASR error: multipart boundary generation failed";
            return result;
        }
        request.host = endpoint.host;
        request.port = endpoint.port;
        request.path = kAzurePath;
        request.headers = L"Content-Type: multipart/form-data; boundary=" +
                          Utf8ToWide(boundary) +
                          L"\r\nOcp-Apim-Subscription-Key: " + apiKey + L"\r\n";
        request.body = BuildAzureMultipart(wav, config.language, boundary);
        if (request.body.empty()) {
            result.error = L"MAI ASR error: Azure multipart request is too large";
            return result;
        }
    }

    result.networkBytes = request.body.size();
    MaiDebugLog("event=request_start provider=%s model=%s host=%s path=%s pcm_bytes=%zu network_bytes=%zu",
                config.apiProvider == ApiProvider::OpenRouter ? "openrouter" : "azure",
                config.apiProvider == ApiProvider::OpenRouter
                    ? kOpenRouterModel : kAzureModel,
                WideToUtf8(request.host).c_str(), WideToUtf8(request.path).c_str(),
                pcm.size(), result.networkBytes);
    const auto started = std::chrono::steady_clock::now();
    const CloudHttpResponse response = SendCloudHttpRequest(request);
    result.elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    result.statusCode = response.statusCode;
    result.winhttpError = response.winhttpError;
    MaiDebugLog("event=response provider=%s status=%lu winhttp_error=%lu ok=%d elapsed_ms=%.1f body_bytes=%zu",
                config.apiProvider == ApiProvider::OpenRouter ? "openrouter" : "azure",
                static_cast<unsigned long>(response.statusCode),
                static_cast<unsigned long>(response.winhttpError),
                response.ok ? 1 : 0, result.elapsedMs, response.body.size());

    if (!response.ok) {
        result.retryable = IsTransientCloudHttpError(response.winhttpError);
        result.error = L"MAI ASR error: " +
            (response.failedStep.empty() ? std::wstring(L"request failed")
                                         : response.failedStep);
        if (response.winhttpError != 0) {
            result.error += L" (err=" + std::to_wstring(response.winhttpError) + L")";
        }
        return result;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        result.retryable = IsRetryableCloudHttpStatus(response.statusCode);
        result.providerCode = ProviderCode(response.body, response.statusCode);
        result.error = L"MAI ASR error: HTTP " +
                       std::to_wstring(response.statusCode);
        const std::wstring message = ErrorMessage(response.body);
        if (!message.empty()) result.error += L": " + message;
        return result;
    }

    const bool parsed = config.apiProvider == ApiProvider::OpenRouter
        ? TryParseOpenRouterText(response.body, result.text)
        : TryParseAzureText(response.body, result.text);
    if (!parsed) {
        result.error = L"MAI ASR error: malformed success response";
        return result;
    }
    result.ok = true;
    return result;
}

TestResult TestConnection(const Config& config) {
    TestResult result;
    const std::vector<BYTE> silence(16000u * sizeof(int16_t), 0);
    const Result response = Recognize(silence, config, 30000, nullptr);
    result.ok = response.ok;
    if (response.ok) {
        result.message = config.apiProvider == ApiProvider::OpenRouter
            ? L"Connection OK. OpenRouter MAI-Transcribe-2 is reachable."
            : L"Connection OK. Azure MAI-Transcribe-2 is reachable.";
    } else {
        result.message = response.error.empty()
            ? L"MAI-Transcribe-2 connection test failed."
            : response.error;
    }
    return result;
}

} // namespace mai_transcribe
