#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <string>
#include <vector>

#include "utils.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace llm {

using ::WideToUtf8;
using ::Utf8ToWide;
using ::EscapeJson;
using ::Trim;

constexpr wchar_t kSystemPrompt[] =
    L"语音识别纠错助手。修正ASR明显错误，不改写润色。\n"
    L"可修正：明确的同音错字、英文术语大小写、数字规范化、补充标点。\n"
    L"禁止：改写、增删、改变语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

constexpr wchar_t kPresetBasicFix[] =
    L"语音识别纠错助手。修正ASR明显错误，不改写润色。\n"
    L"可修正：明确的同音错字（根据语境）、英文术语大小写、数字规范化、标点。\n"
    L"禁止：改写、增删、改变语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

constexpr wchar_t kPresetDeepFix[] =
    L"语音识别纠错助手。修正ASR错误，不改写润色。\n"
    L"可修正：同音错字（根据语境）、英文术语大小写、数字规范化、标点、语法错误。\n"
    L"禁止：改写、增删、改变语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

constexpr wchar_t kPresetPolish[] =
    L"语音识别纠错助手。修正ASR错误并润色表达。\n"
    L"可修正：同音错字、英文术语大小写、数字、标点，保留中英文混合,并润色语句。\n"
    L"保持原意和语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

struct PromptPreset {
    const wchar_t* name;
    const wchar_t* prompt;
    const wchar_t* description;
};

constexpr PromptPreset kPromptPresets[] = {
    {L"Basic Fix",  kPresetBasicFix,  L"Fix homophones, terms, numbers, and add missing punctuation"},
    {L"Deep Fix",   kPresetDeepFix,   L"Fix typos, terms, grammar, punctuation, and normalize numbers"},
    {L"Polish",     kPresetPolish,    L"Fix errors and polish expression while preserving original meaning"},
};
constexpr int kPromptPresetCount = sizeof(kPromptPresets) / sizeof(kPromptPresets[0]);

struct ProviderPreset {
    const wchar_t* name;
    const wchar_t* url;
    const wchar_t* defaultModel;
    const wchar_t* extraParams;
};

constexpr ProviderPreset kProviderPresets[] = {
    {L"DeepSeek",    L"https://api.deepseek.com",      L"deepseek-v4-flash",           L"\"thinking\":{\"type\":\"disabled\"}"},
    {L"OpenRouter",  L"https://openrouter.ai/api/v1",  L"qwen/qwen3-4b",              L"\"reasoning\":{\"effort\":\"none\"}"},
    {L"SiliconFlow", L"https://api.siliconflow.cn/v1", L"Qwen/Qwen3.6-35B-A3B",       L"\"chat_template_kwargs\":{\"enable_thinking\":false}"},
};
constexpr int kProviderPresetCount = sizeof(kProviderPresets) / sizeof(kProviderPresets[0]);

inline std::wstring EncryptString(const std::wstring& plain) {
    if (plain.empty()) return L"";
    std::string utf8 = WideToUtf8(plain);
    DATA_BLOB input = { static_cast<DWORD>(utf8.size()), reinterpret_cast<BYTE*>(utf8.data()) };
    DATA_BLOB output = {};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return L"";
    }
    const char* base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring result;
    const BYTE* data = output.pbData;
    size_t len = output.cbData;
    for (size_t i = 0; i < len; i += 3) {
        DWORD n = static_cast<DWORD>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<DWORD>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<DWORD>(data[i + 2]);
        result += base64[(n >> 18) & 0x3F];
        result += base64[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? base64[(n >> 6) & 0x3F] : L'=';
        result += (i + 2 < len) ? base64[n & 0x3F] : L'=';
    }
    LocalFree(output.pbData);
    return result;
}

inline std::wstring DecryptString(const std::wstring& enc) {
    if (enc.empty()) return L"";
    auto base64Decode = [](wchar_t c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<BYTE> data;
    for (size_t i = 0; i < enc.size(); i += 4) {
        int a = base64Decode(enc[i]);
        int b = (i + 1 < enc.size()) ? base64Decode(enc[i + 1]) : -1;
        int c2 = (i + 2 < enc.size()) ? base64Decode(enc[i + 2]) : -1;
        int d = (i + 3 < enc.size()) ? base64Decode(enc[i + 3]) : -1;
        if (a < 0) break;
        data.push_back(static_cast<BYTE>((a << 2) | (b >> 4)));
        if (b < 0 || enc[i + 2] == L'=') break;
        data.push_back(static_cast<BYTE>(((b & 0xF) << 4) | (c2 >> 2)));
        if (c2 < 0 || enc[i + 3] == L'=') break;
        data.push_back(static_cast<BYTE>(((c2 & 3) << 6) | d));
    }
    DATA_BLOB input = { static_cast<DWORD>(data.size()), data.data() };
    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return L"";
    }
    std::wstring result = Utf8ToWide(std::string(reinterpret_cast<char*>(output.pbData), output.cbData));
    LocalFree(output.pbData);
    return result;
}

struct RequestConfig {
    std::wstring endpoint;
    std::wstring apiKey;
    std::wstring model;
    std::wstring systemPrompt;
    std::wstring extraParams;
};

inline std::string BuildRequestBody(const std::wstring& userMsg, const RequestConfig& cfg) {
    const std::wstring& prompt = cfg.systemPrompt.empty() ? std::wstring(kSystemPrompt) : cfg.systemPrompt;
    std::string body = "{\"model\":\"" + EscapeJson(cfg.model)
        + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + EscapeJson(prompt)
        + "\"},{\"role\":\"user\",\"content\":\"" + EscapeJson(userMsg)
        + "\"}],\"max_tokens\":1024,\"temperature\":0.1";
    if (!cfg.extraParams.empty()) {
        body += "," + WideToUtf8(cfg.extraParams);
    }
    body += "}";
    return body;
}

inline std::string BuildTestBody(const RequestConfig& cfg) {
    return "{\"model\":\"" + EscapeJson(cfg.model)
        + "\",\"messages\":[{\"role\":\"user\",\"content\":\"ping\"}],\"max_tokens\":5}";
}

inline std::wstring ParseResponse(const std::string& response) {
    size_t pos = response.find("\"content\"");
    if (pos == std::string::npos) return L"";
    pos = response.find(':', pos);
    if (pos == std::string::npos) return L"";
    pos = response.find('"', pos + 1);
    if (pos == std::string::npos) return L"";
    std::string value;
    bool escape = false;
    for (++pos; pos < response.size(); ++pos) {
        const char c = response[pos];
        if (escape) {
            switch (c) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(c); break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            break;
        } else {
            value.push_back(c);
        }
    }
    return Trim(Utf8ToWide(value));
}

inline bool ParseEndpoint(const std::wstring& endpoint, std::wstring& host, std::wstring& path, bool& useSsl, INTERNET_PORT& port) {
    std::wstring url = endpoint;
    const std::wstring httpsP = L"https://";
    const std::wstring httpP = L"http://";
    if (url.compare(0, httpsP.size(), httpsP) == 0) {
        url = url.substr(httpsP.size());
        useSsl = true;
        port = INTERNET_DEFAULT_HTTPS_PORT;
    } else if (url.compare(0, httpP.size(), httpP) == 0) {
        url = url.substr(httpP.size());
        useSsl = false;
        port = INTERNET_DEFAULT_HTTP_PORT;
    } else {
        useSsl = true;
        port = INTERNET_DEFAULT_HTTPS_PORT;
    }
    size_t colon = url.find(L':');
    size_t slash = url.find(L'/');
    if (colon != std::wstring::npos && (slash == std::wstring::npos || colon < slash)) {
        host = url.substr(0, colon);
        std::wstring portStr = (slash != std::wstring::npos) ? url.substr(colon + 1, slash - colon - 1) : url.substr(colon + 1);
        int p = _wtoi(portStr.c_str());
        if (p > 0 && p < 65536) port = static_cast<INTERNET_PORT>(p);
        if (slash != std::wstring::npos) path = url.substr(slash); else path = L"/";
    } else {
        if (slash != std::wstring::npos) {
            host = url.substr(0, slash);
            path = url.substr(slash);
        } else {
            host = url;
            path = L"/";
        }
    }
    path += L"/chat/completions";
    return !host.empty();
}

struct RequestResult {
    bool success = false;
    DWORD statusCode = 0;
    std::wstring error;
    std::wstring responseText;
};

inline RequestResult SendRequestRaw(const RequestConfig& cfg, const std::string& body, DWORD timeoutMs = 5000) {
    RequestResult res;
    std::wstring host, path;
    bool useSsl = true;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!ParseEndpoint(cfg.endpoint, host, path, useSsl, port)) {
        res.error = L"Invalid endpoint URL";
        return res;
    }

    HINTERNET hSession = WinHttpOpen(L"VoxType/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { res.error = L"WinHttpOpen failed"; return res; }
    WinHttpSetTimeouts(hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        res.error = L"WinHttpConnect failed to " + host + L":" + std::to_wstring(port);
        WinHttpCloseHandle(hSession); return res;
    }

    DWORD flags = useSsl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!hRequest) {
        res.error = L"WinHttpOpenRequest failed";
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return res;
    }

    WinHttpSetTimeouts(hRequest, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\nAuthorization: Bearer " + cfg.apiKey;
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
        const_cast<char*>(body.c_str()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        res.error = L"WinHttpSendRequest failed (error " + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return res;
    }

    BOOL received = WinHttpReceiveResponse(hRequest, nullptr);
    if (!received) {
        res.error = L"WinHttpReceiveResponse failed (error " + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return res;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    res.statusCode = statusCode;

    std::string responseBody;
    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) break;
        if (bytesAvailable == 0) break;
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead);
        responseBody += chunk;
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    res.responseText = Utf8ToWide(responseBody);
    if (statusCode == 200) {
        res.success = true;
        res.responseText = ParseResponse(responseBody);
    } else {
        res.error = L"HTTP " + std::to_wstring(statusCode) + L": " + res.responseText.substr(0, 200);
    }
    return res;
}

inline std::wstring Refine(const std::wstring& asrText, const RequestConfig& cfg) {
    std::string body = BuildRequestBody(asrText, cfg);
    RequestResult res = SendRequestRaw(cfg, body, 5000);
    return (res.success && !res.responseText.empty()) ? res.responseText : asrText;
}

struct TestResult {
    bool ok = false;
    DWORD elapsed = 0;
    std::wstring message;
};

inline TestResult TestConnection(const RequestConfig& cfg) {
    TestResult result;
    DWORD startTime = GetTickCount();

    std::string body = BuildTestBody(cfg);
    RequestResult res = SendRequestRaw(cfg, body, 8000);
    result.elapsed = GetTickCount() - startTime;

    if (res.success) {
        result.ok = true;
        result.message = L"Connection OK (" + std::to_wstring(result.elapsed) + L" ms)";
    } else {
        result.message = res.error;
    }
    return result;
}

} // namespace llm
