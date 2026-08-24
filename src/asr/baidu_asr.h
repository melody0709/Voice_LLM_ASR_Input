#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include "cloud_http_common.h"
#include "audio_diagnostics.h"
#include "utils.h"

#pragma comment(lib, "winhttp.lib")

namespace baidu_asr {

struct BaiduConfig {
    std::wstring apiKey;
    std::wstring secretKey;
    int devPid = 1537;
    uint64_t diagnosticAttemptId = 0;
    audio_diagnostics::StageKind diagnosticStageKind =
        audio_diagnostics::StageKind::Primary;
    unsigned diagnosticStageIndex = 0;
};

inline int ExtractJsonInt(const std::string& json, const std::string& key, int fallback = 0) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return fallback;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
    if (pos >= json.size()) return fallback;
    std::string numStr;
    while (pos < json.size() && (json[pos] == '-' || (json[pos] >= '0' && json[pos] <= '9'))) {
        numStr += json[pos++];
    }
    if (numStr.empty()) return fallback;
    return std::stoi(numStr);
}

inline std::wstring GetMachineCuid() {
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD size = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(name, &size)) {
        return L"voxtype_" + std::wstring(name);
    }
    return L"voxtype_unknown";
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
    const size_t hostPos = url.find(host);
    if (hostPos == std::wstring::npos) return L"";
    std::wstring path = url.substr(hostPos + host.size());

    for (int attempt = 0; attempt < 2; ++attempt) {
        CloudHttpRequest req;
        req.method = L"GET";
        req.host = host;
        req.port = port;
        req.path = path;
        req.useSsl = useSsl;
        req.timeoutMs = 5000;

        CloudHttpResponse response = SendCloudHttpRequest(req);
        if (response.ok && response.statusCode >= 200 && response.statusCode < 300) {
            return Utf8ToWide(response.body);
        }

        const bool retryable = (!response.ok && IsTransientCloudHttpError(response.winhttpError)) ||
                               (response.ok && IsRetryableCloudHttpStatus(response.statusCode));
        if (!retryable || attempt == 1) break;
        SleepCloudHttpRetryBackoff(attempt);
    }

    return L"";
}

inline std::mutex& TokenMutex() {
    static std::mutex s_tokenMutex;
    return s_tokenMutex;
}

inline std::wstring& CachedToken() {
    static std::wstring s_cachedToken;
    return s_cachedToken;
}

inline ULONGLONG& CachedTokenExpiresAt() {
    static ULONGLONG s_tokenExpiresAt = 0;
    return s_tokenExpiresAt;
}

inline void ClearCachedToken() {
    std::lock_guard<std::mutex> lk(TokenMutex());
    CachedToken().clear();
    CachedTokenExpiresAt() = 0;
}

inline std::wstring GetAccessToken(const BaiduConfig& cfg) {
    std::lock_guard<std::mutex> lk(TokenMutex());

    std::wstring& cachedToken = CachedToken();
    ULONGLONG& tokenExpiresAt = CachedTokenExpiresAt();

    ULONGLONG now = GetTickCount64();
    if (!cachedToken.empty() && now < tokenExpiresAt) {
        return cachedToken;
    }

    std::wstring tokUrl = L"https://aip.baidubce.com/oauth/2.0/token"
                          L"?grant_type=client_credentials"
                          L"&client_id=" + UrlEncode(cfg.apiKey) +
                          L"&client_secret=" + UrlEncode(cfg.secretKey);

    std::wstring response = HttpGet(tokUrl, L"aip.baidubce.com",
                                    INTERNET_DEFAULT_HTTPS_PORT, true);
    if (response.empty()) {
        cachedToken.clear();
        tokenExpiresAt = 0;
        return L"";
    }

    std::string respUtf8 = WideToUtf8(response);
    std::wstring token = ExtractJsonStr(respUtf8, "access_token");
    std::wstring expiresInStr = ExtractJsonStr(respUtf8, "expires_in");

    if (token.empty()) {
        cachedToken.clear();
        tokenExpiresAt = 0;
        return L"";
    }

    int expiresIn = 2592000;
    if (!expiresInStr.empty()) {
        expiresIn = _wtoi(expiresInStr.c_str());
        if (expiresIn <= 0) expiresIn = 2592000;
    }

    cachedToken = token;
    tokenExpiresAt = now + static_cast<ULONGLONG>(expiresIn) * 1000 - 60000; // 提前1分钟刷新
    return token;
}

inline bool ContainsCaseInsensitive(std::wstring text, const wchar_t* needle) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    std::wstring n = needle;
    std::transform(n.begin(), n.end(), n.begin(),
                   [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return text.find(n) != std::wstring::npos;
}

inline bool IsTokenError(int errNo, const std::wstring& errMsg) {
    return errNo == 110 ||
           errNo == 111 ||
           ContainsCaseInsensitive(errMsg, L"token") ||
           ContainsCaseInsensitive(errMsg, L"access_token");
}

inline std::wstring ExtractBaiduResultText(const std::string& responseBody) {
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

    if (resultText.empty()) {
        printf("[Baidu diag] err_no=0 but result empty, body (%zu bytes): %s\n", responseBody.size(), responseBody.c_str());
    }

    return resultText;
}

struct BaiduRecognizeAttempt {
    bool ok = false;
    bool retryable = false;
    bool tokenError = false;
    std::wstring text;
    std::wstring errorText;
    int providerCode = 0;
};

inline BaiduRecognizeAttempt RecognizeOnce(const std::vector<BYTE>& pcm,
                                           const BaiduConfig& cfg,
                                           const std::wstring& token) {
    BaiduRecognizeAttempt result;

    const std::wstring cuid = GetMachineCuid();
    const std::wstring host = L"vop.baidu.com";
    std::wstring path = L"/server_api?cuid=" + UrlEncode(cuid) +
                        L"&token=" + UrlEncode(token) +
                        L"&dev_pid=" + std::to_wstring(cfg.devPid);

    CloudHttpRequest req;
    req.method = L"POST";
    req.host = host;
    req.port = INTERNET_DEFAULT_HTTPS_PORT;
    req.path = path;
    req.headers = L"Content-Type: audio/pcm;rate=16000\r\n";
    req.body = pcm;
    req.useSsl = true;
    req.timeoutMs = 8000;

    CloudHttpResponse response = SendCloudHttpRequest(req);
    if (!response.ok) {
        result.retryable = IsTransientCloudHttpError(response.winhttpError);
        result.errorText = L"Baidu ASR error: " + response.failedStep +
            L" failed (" + std::to_wstring(response.winhttpError) + L")";
        return result;
    }

    if (response.statusCode < 200 || response.statusCode >= 300) {
        result.retryable = IsRetryableCloudHttpStatus(response.statusCode);
        result.errorText = L"Baidu ASR error: HTTP " + std::to_wstring(response.statusCode);
        printf("[Baidu diag] HTTP status=%lu body (%zu bytes): %s\n",
               response.statusCode, response.body.size(), response.body.c_str());
        return result;
    }

    int errNo = ExtractJsonInt(response.body, "err_no", -1);
    result.providerCode = errNo;
    if (errNo < 0) {
        result.retryable = response.body.empty();
        result.errorText = L"Baidu ASR error: Empty response";
        printf("[Baidu diag] Empty/parse error response body (%zu bytes): %s\n",
               response.body.size(), response.body.c_str());
        return result;
    }

    if (errNo != 0) {
        std::wstring errMsg = ExtractJsonStr(response.body, "err_msg");
        result.tokenError = IsTokenError(errNo, errMsg);
        result.errorText = L"Baidu ASR error " + std::to_wstring(errNo) +
            L": " + (errMsg.empty() ? L"unknown" : errMsg);
        printf("[Baidu diag] err_no=%d err_msg='%ls' dev_pid=%d body: %s\n",
               errNo, errMsg.c_str(), cfg.devPid, response.body.c_str());
        return result;
    }

    result.ok = true;
    result.text = ExtractBaiduResultText(response.body);
    return result;
}

inline std::wstring Recognize(const std::vector<BYTE>& pcm, const BaiduConfig& cfg) {
    if (pcm.empty() || cfg.apiKey.empty() || cfg.secretKey.empty()) {
        return L"";
    }

    std::wstring lastError;
    bool refreshedToken = false;
    bool retriedTransient = false;
    unsigned requestIndex = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        std::wstring token = GetAccessToken(cfg);
        if (token.empty()) {
            lastError = L"Baidu ASR error: Failed to get access token";
            if (!retriedTransient) {
                retriedTransient = true;
                SleepCloudHttpRetryBackoff(attempt);
                continue;
            }
            break;
        }

        audio_diagnostics::StageMetadata diagnostic;
        diagnostic.kind = requestIndex == 0
            ? cfg.diagnosticStageKind
            : audio_diagnostics::RetryStageKind(cfg.diagnosticStageKind);
        diagnostic.index = requestIndex == 0
            ? cfg.diagnosticStageIndex
            : audio_diagnostics::RetryStageIndex(
                cfg.diagnosticStageKind, cfg.diagnosticStageIndex, requestIndex);
        diagnostic.backend = L"baidu";
        diagnostic.model = L"dev_pid_" + std::to_wstring(cfg.devPid);
        diagnostic.transport = L"batch_http_pcm";
        diagnostic.reason = requestIndex == 0 ? L"" : L"token_or_transient_retry";
        diagnostic.sentBytes = pcm.size();
        audio_diagnostics::RegisterStageInput(
            cfg.diagnosticAttemptId, diagnostic, pcm);

        BaiduRecognizeAttempt r = RecognizeOnce(pcm, cfg, token);
        audio_diagnostics::StageTerminal terminal;
        if (r.ok && r.text.empty()) {
            terminal.terminal = "http_success_empty";
            terminal.reason = "no_speech";
        } else if (r.ok) {
            terminal.terminal = "http_success";
            terminal.textChars = r.text.size();
        } else if (r.tokenError) {
            terminal.terminal = "auth_error";
            terminal.reason = "auth_or_config";
        } else if (r.retryable) {
            terminal.terminal = "transient_http_error";
            terminal.reason = "network";
        } else {
            terminal.terminal = "provider_error";
            terminal.reason = "provider_error";
        }
        terminal.providerCode = std::to_string(r.providerCode);
        audio_diagnostics::CompleteStage(
            cfg.diagnosticAttemptId, diagnostic.kind, diagnostic.index, terminal);
        ++requestIndex;
        if (r.ok) return r.text;
        lastError = r.errorText;

        if (r.tokenError && !refreshedToken) {
            refreshedToken = true;
            ClearCachedToken();
            SleepCloudHttpRetryBackoff(attempt);
            continue;
        }

        if (r.retryable && !retriedTransient) {
            retriedTransient = true;
            printf("[Baidu diag] retrying same PCM after transient failure: %ls\n", lastError.c_str());
            SleepCloudHttpRetryBackoff(attempt);
            continue;
        }

        break;
    }

    return lastError.empty() ? L"Baidu ASR error: request failed" : lastError;
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

    CloudHttpRequest req;
    req.method = L"POST";
    req.host = host;
    req.port = INTERNET_DEFAULT_HTTPS_PORT;
    req.path = path;
    req.headers = L"Content-Type: audio/pcm;rate=16000\r\n";
    req.body.assign(3200, 0);
    req.useSsl = true;
    req.timeoutMs = 5000;

    CloudHttpResponse response;
    for (int attempt = 0; attempt < 2; ++attempt) {
        response = SendCloudHttpRequest(req);
        const bool retryable = (!response.ok && IsTransientCloudHttpError(response.winhttpError)) ||
                               (response.ok && IsRetryableCloudHttpStatus(response.statusCode));
        if (!retryable || attempt == 1) break;
        SleepCloudHttpRetryBackoff(attempt);
    }

    if (!response.ok) {
        res.message = response.failedStep + L" failed (error " +
            std::to_wstring(response.winhttpError) + L").";
        return res;
    }
    if (response.statusCode < 200 || response.statusCode >= 300) {
        res.message = L"HTTP " + std::to_wstring(response.statusCode) + L" from server.";
        return res;
    }

    int errNo = ExtractJsonInt(response.body, "err_no", -1);
    if (errNo < 0) {
        res.message = L"Invalid response from server.";
        return res;
    }

    if (errNo == 0) {
        res.ok = true;
        res.message = L"Connection OK. Token valid, API reachable.";
    } else {
        std::wstring errMsg = ExtractJsonStr(response.body, "err_msg");
        res.message = L"API error " + std::to_wstring(errNo) + L": " +
            (errMsg.empty() ? L"unknown" : errMsg);
    }

    return res;
}

} // namespace baidu_asr
