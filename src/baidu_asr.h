#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace baidu_asr {

struct BaiduConfig {
    std::wstring apiKey;
    std::wstring secretKey;
    int devPid = 1537;
};

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    return result;
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    return result;
}

inline std::wstring ExtractJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return L"";
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return L"";
    size_t start = pos + 1;
    size_t end = json.find('"', start);
    if (end == std::string::npos) return L"";
    return Utf8ToWide(json.substr(start, end - start));
}

inline std::wstring GetMachineCuid() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &size)) {
        return L"voice_llm_" + std::wstring(name);
    }
    return L"voice_llm_unknown";
}

inline std::wstring UrlEncode(const std::wstring& src) {
    std::string utf8 = WideToUtf8(src);
    std::string out;
    for (char c : utf8) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += c;
        } else {
            const char* hex = "0123456789ABCDEF";
            out += '%';
            out += hex[(static_cast<unsigned char>(c) >> 4) & 0xF];
            out += hex[static_cast<unsigned char>(c) & 0xF];
        }
    }
    return Utf8ToWide(out);
}

inline std::wstring HttpGet(const std::wstring& url, const std::wstring& host, INTERNET_PORT port, bool useSsl) {
    HINTERNET hSession = WinHttpOpen(L"VoiceLLMASRInput/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return L"";

    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return L"";
    }

    DWORD flags = useSsl ? WINHTTP_FLAG_SECURE : 0;
    size_t pathStart = url.find(host) + host.size();
    std::wstring path = url.substr(pathStart);

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"";
    }

    if (useSsl) {
        DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                         SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                         SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    }

    WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 5000);

    if (!WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"";
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"";
    }

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

    return Utf8ToWide(responseBody);
}

inline std::wstring GetAccessToken(const BaiduConfig& cfg) {
    static std::wstring s_cachedToken;
    static ULONGLONG s_tokenExpiresAt = 0;

    ULONGLONG now = GetTickCount64();
    if (!s_cachedToken.empty() && now < s_tokenExpiresAt) {
        return s_cachedToken;
    }

    std::wstring tokUrl = L"https://aip.baidubce.com/oauth/2.0/token"
                          L"?grant_type=client_credentials"
                          L"&client_id=" + UrlEncode(cfg.apiKey) +
                          L"&client_secret=" + UrlEncode(cfg.secretKey);

    std::wstring response = HttpGet(tokUrl, L"aip.baidubce.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, true);
    if (response.empty()) {
        s_cachedToken.clear();
        s_tokenExpiresAt = 0;
        return L"";
    }

    std::string respUtf8 = WideToUtf8(response);
    std::wstring token = ExtractJsonStr(respUtf8, "access_token");
    std::wstring expiresInStr = ExtractJsonStr(respUtf8, "expires_in");

    if (token.empty()) {
        s_cachedToken.clear();
        s_tokenExpiresAt = 0;
        return L"";
    }

    int expiresIn = 2592000;
    if (!expiresInStr.empty()) {
        expiresIn = _wtoi(expiresInStr.c_str());
        if (expiresIn <= 0) expiresIn = 2592000;
    }

    s_cachedToken = token;
    s_tokenExpiresAt = now + static_cast<ULONGLONG>(expiresIn) * 1000 - 60000; // 提前1分钟刷新
    return token;
}

inline std::wstring Recognize(const std::vector<BYTE>& pcm, const BaiduConfig& cfg) {
    if (pcm.empty() || cfg.apiKey.empty() || cfg.secretKey.empty()) {
        return L"";
    }

    std::wstring token = GetAccessToken(cfg);
    if (token.empty()) return L"Baidu ASR error: Failed to get access token";

    const std::wstring cuid = GetMachineCuid();
    const std::wstring host = L"vop.baidu.com";

    std::wstring path = L"/server_api?cuid=" + UrlEncode(cuid) +
                        L"&token=" + UrlEncode(token) +
                        L"&dev_pid=" + std::to_wstring(cfg.devPid);

    HINTERNET hSession = WinHttpOpen(L"VoiceLLMASRInput/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return L"Baidu ASR error: WinHttpOpen failed";

    WinHttpSetTimeouts(hSession, 8000, 8000, 8000, 8000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return L"Baidu ASR error: WinHttpConnect failed";
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"Baidu ASR error: WinHttpOpenRequest failed";
    }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    WinHttpSetTimeouts(hRequest, 8000, 8000, 8000, 8000);

    std::wstring headers = L"Content-Type: audio/pcm;rate=16000\r\n";
    DWORD bodySize = static_cast<DWORD>(pcm.size());

    if (!WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
                            const_cast<BYTE*>(pcm.data()), bodySize, bodySize, 0)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"Baidu ASR error: SendRequest failed (" + std::to_wstring(err) + L")";
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return L"Baidu ASR error: ReceiveResponse failed (" + std::to_wstring(err) + L")";
    }

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

    std::wstring errNoStr = ExtractJsonStr(responseBody, "err_no");
    if (errNoStr.empty()) return L"Baidu ASR error: Empty response";

    int errNo = _wtoi(errNoStr.c_str());
    if (errNo != 0) {
        std::wstring errMsg = ExtractJsonStr(responseBody, "err_msg");
        return L"Baidu ASR error " + errNoStr + L": " + (errMsg.empty() ? L"unknown" : errMsg);
    }

    std::wstring resultText = ExtractJsonStr(responseBody, "result");
    if (resultText.empty()) {
        size_t arrPos = responseBody.find("\"result\"");
        if (arrPos != std::string::npos) {
            size_t bracketPos = responseBody.find('[', arrPos);
            if (bracketPos != std::string::npos) {
                size_t quotePos = responseBody.find('"', bracketPos + 1);
                if (quotePos != std::string::npos) {
                    size_t endQuotePos = responseBody.find('"', quotePos + 1);
                    if (endQuotePos != std::string::npos) {
                        resultText = Utf8ToWide(responseBody.substr(quotePos + 1, endQuotePos - quotePos - 1));
                    }
                }
            }
        }
    }

    return resultText;
}

struct TestResult {
    bool ok = false;
    std::wstring message;
};

inline TestResult TestConnection(const BaiduConfig& cfg) {
    TestResult res;
    if (cfg.apiKey.empty() || cfg.secretKey.empty()) {
        res.message = L"Please fill in API Key and Secret Key.";
        return res;
    }

    std::wstring token = GetAccessToken(cfg);
    if (token.empty()) {
        res.message = L"Failed to get access_token. Check API Key / Secret Key.";
        return res;
    }

    const std::wstring cuid = GetMachineCuid();
    const std::wstring host = L"vop.baidu.com";
    std::wstring path = L"/server_api?cuid=" + UrlEncode(cuid) +
                        L"&token=" + UrlEncode(token) +
                        L"&dev_pid=" + std::to_wstring(cfg.devPid);

    HINTERNET hSession = WinHttpOpen(L"VoiceLLMASRInput/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        res.message = L"WinHttpOpen failed.";
        return res;
    }
    WinHttpSetTimeouts(hSession, 5000, 5000, 5000, 5000);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpConnect failed.";
        return res;
    }

    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpOpenRequest failed.";
        return res;
    }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    WinHttpSetTimeouts(hRequest, 5000, 5000, 5000, 5000);

    std::wstring headers = L"Content-Type: audio/pcm;rate=16000\r\n";
    std::vector<BYTE> silencePcm(3200, 0);

    if (!WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
                            silencePcm.data(), static_cast<DWORD>(silencePcm.size()),
                            static_cast<DWORD>(silencePcm.size()), 0)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"SendRequest failed (error " + std::to_wstring(err) + L")";
        return res;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        WinHttpCloseHandle(hRequest);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"No response from server.";
        return res;
    }

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

    std::wstring errNoStr = ExtractJsonStr(responseBody, "err_no");
    if (errNoStr.empty()) {
        res.message = L"Invalid response from server.";
        return res;
    }

    int errNo = _wtoi(errNoStr.c_str());
    if (errNo == 0) {
        res.ok = true;
        res.message = L"Connection OK. Token valid, API reachable.";
    } else {
        std::wstring errMsg = ExtractJsonStr(responseBody, "err_msg");
        res.message = L"API error " + errNoStr + L": " + (errMsg.empty() ? L"unknown" : errMsg);
    }

    return res;
}

} // namespace baidu_asr
