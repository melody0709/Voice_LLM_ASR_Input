#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <mutex>
#include <string>
#include <vector>

#include "cloud_http_common.h"
#include "cloud_asr_common.h"
#include "audio_diagnostics.h"
#include "asr_runtime_log.h"
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
    if (!json_detail::IsValidDocument(json)) return fallback;
    size_t pos = json_detail::FindValueForKey(json, key);
    if (pos == std::string::npos || pos >= json.size()) return fallback;

    bool negative = false;
    if (json[pos] == '-') {
        negative = true;
        ++pos;
    }
    if (pos >= json.size() || json[pos] < '0' || json[pos] > '9') return fallback;

    const uint64_t limit = negative
        ? static_cast<uint64_t>(INT_MAX) + 1ULL
        : static_cast<uint64_t>(INT_MAX);
    uint64_t value = 0;
    while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
        const unsigned digit = static_cast<unsigned>(json[pos] - '0');
        if (value > (limit - digit) / 10ULL) return fallback;
        value = value * 10ULL + digit;
        ++pos;
    }
    json_detail::SkipWhitespace(json, pos);
    if (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != ']') {
        return fallback;  // Reject fractions/exponents instead of truncating them.
    }
    if (negative && value == static_cast<uint64_t>(INT_MAX) + 1ULL) return INT_MIN;
    const int signedValue = static_cast<int>(value);
    return negative ? -signedValue : signedValue;
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

inline std::wstring& CachedTokenApiKey() {
    static std::wstring s_cachedApiKey;
    return s_cachedApiKey;
}

inline std::wstring& CachedTokenSecretKey() {
    static std::wstring s_cachedSecretKey;
    return s_cachedSecretKey;
}

inline ULONGLONG& CachedTokenExpiresAt() {
    static ULONGLONG s_tokenExpiresAt = 0;
    return s_tokenExpiresAt;
}

inline void ClearCachedToken() {
    std::lock_guard<std::mutex> lk(TokenMutex());
    CachedToken().clear();
    CachedTokenApiKey().clear();
    CachedTokenSecretKey().clear();
    CachedTokenExpiresAt() = 0;
}

inline ULONGLONG TokenCacheLifetimeMs(int expiresIn) {
    const int effectiveExpiresIn = expiresIn > 0 ? expiresIn : 2592000;
    const ULONGLONG lifetimeMs = static_cast<ULONGLONG>(effectiveExpiresIn) * 1000ULL;
    const ULONGLONG refreshMarginMs = (std::min)(60000ULL, lifetimeMs / 10ULL);
    return lifetimeMs - refreshMarginMs;
}

inline std::wstring GetAccessToken(const BaiduConfig& cfg) {
    std::lock_guard<std::mutex> lk(TokenMutex());

    std::wstring& cachedToken = CachedToken();
    std::wstring& cachedApiKey = CachedTokenApiKey();
    std::wstring& cachedSecretKey = CachedTokenSecretKey();
    ULONGLONG& tokenExpiresAt = CachedTokenExpiresAt();

    // 缓存与凭据绑定：Key/Secret 一变更立即失效旧 token。
    // Settings 测试按钮直接用未保存的输入框值发起请求，不走 SaveConfig，
    // 因此不能依赖"保存时清缓存"。分别保存两个字段也避免拼接分隔符歧义。

    ULONGLONG now = GetTickCount64();
    if (!cachedToken.empty() && now < tokenExpiresAt &&
        cachedApiKey == cfg.apiKey && cachedSecretKey == cfg.secretKey) {
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
        cachedApiKey.clear();
        cachedSecretKey.clear();
        tokenExpiresAt = 0;
        return L"";
    }

    std::string respUtf8 = WideToUtf8(response);
    std::wstring token = ExtractJsonStringDecoded(respUtf8, "access_token");
    // expires_in 是数字（无引号），必须走整数解析，避免永远落回
    // 30 天默认值（防未来有效期变化）。
    const int expiresIn = ExtractJsonInt(respUtf8, "expires_in", -1);

    if (token.empty()) {
        cachedToken.clear();
        cachedApiKey.clear();
        cachedSecretKey.clear();
        tokenExpiresAt = 0;
        return L"";
    }

    // 长 token 最多提前一分钟刷新；短 token 提前 10%，避免 expires_in < 60
    // 时无符号减法下溢成一个几乎永久有效的缓存时间。
    const ULONGLONG cacheLifetimeMs = TokenCacheLifetimeMs(expiresIn);

    cachedToken = token;
    cachedApiKey = cfg.apiKey;
    cachedSecretKey = cfg.secretKey;
    tokenExpiresAt = now + cacheLifetimeMs;
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
    // 百度 result 是字符串数组，用转义感知的数组解析，
    // 不再落回裸 find('"') 的手工解析（后者遇文本内 \" 会截断）。
    std::wstring resultText = ExtractJsonArrayFirstStringDecoded(responseBody, "result");

    if (resultText.empty()) {
        // Do not persist recognition text in the runtime log; record only size.
        asr_runtime_log::Write("[Baidu] err_no=0 but result empty (body %zu bytes)", responseBody.size());
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
    req.timeoutMs = ComputeCloudAsrRecordedRequestTimeoutMs(0.0, pcm.size());

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
        asr_runtime_log::Write("[Baidu] HTTP status=%lu (body %zu bytes)",
                               response.statusCode, response.body.size());
        return result;
    }

    int errNo = ExtractJsonInt(response.body, "err_no", -1);
    result.providerCode = errNo;
    if (errNo < 0) {
        result.retryable = response.body.empty();
        result.errorText = L"Baidu ASR error: Empty response";
        asr_runtime_log::Write("[Baidu] Empty/parse error response (body %zu bytes)",
                               response.body.size());
        return result;
    }

    if (errNo != 0) {
        std::wstring errMsg = ExtractJsonStringDecoded(response.body, "err_msg");
        result.tokenError = IsTokenError(errNo, errMsg);
        result.errorText = L"Baidu ASR error " + std::to_wstring(errNo) +
            L": " + (errMsg.empty() ? L"unknown" : errMsg);
        asr_runtime_log::Write("[Baidu] err_no=%d err_msg='%s' dev_pid=%d",
                               errNo, WideToUtf8(errMsg).c_str(), cfg.devPid);
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
    // 百度短语音接口官方上限 60 s（16 kHz / 16 bit 单声道 = 32 B/ms），
    // 超限本地提前拒绝，避免白传流量再被服务端拒绝。
    constexpr size_t kBaiduMaxPcmBytes = 60u * 1000u * 32u;
    if (pcm.size() > kBaiduMaxPcmBytes) {
        return L"Baidu ASR error: audio exceeds the 60s limit";
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
            asr_runtime_log::Write("[Baidu] retrying same PCM after transient failure: %s",
                                   WideToUtf8(lastError).c_str());
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
        std::wstring errMsg = ExtractJsonStringDecoded(response.body, "err_msg");
        res.message = L"API error " + std::to_wstring(errNo) + L": " +
            (errMsg.empty() ? L"unknown" : errMsg);
    }

    return res;
}

} // namespace baidu_asr
