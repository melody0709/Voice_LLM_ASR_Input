#pragma once

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <string>

namespace qwen_free_recovery_policy {

inline std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text;
}

inline bool ContainsAny(const std::wstring& text,
                        std::initializer_list<const wchar_t*> needles) {
    for (const wchar_t* needle : needles) {
        if (text.find(needle) != std::wstring::npos) return true;
    }
    return false;
}

inline bool ShouldRetryConnect(const std::wstring& error) {
    const std::wstring lower = Lower(error);
    // Match complete HTTP-status tokens. Diagnostics also contain numeric
    // timestamps, so searching for bare "401"/"403"/"404" can randomly turn
    // a transient network error into a non-retryable authentication error.
    return !ContainsAny(lower, {
        L"http 400",
        L"http 401",
        L"http 403",
        L"http 404",
        L"d30112",
        L"invalid utdid",
        L"invalid signature",
        L"signature verification",
        L"signature validation",
        L"sign failed",
        L"unauthorized",
        L"forbidden",
        L"authentication failed",
        L"验签",
        L"鉴权失败",
    });
}

inline bool ReplayOutcomeIsUsable(bool sendOk,
                                  bool allAudioSent,
                                  bool finalReceived,
                                  bool terminalWasFinal,
                                  bool hasFrameError,
                                  bool finalHasText = true) {
    return sendOk && allAudioSent && finalReceived && terminalWasFinal &&
           !hasFrameError && finalHasText;
}

} // namespace qwen_free_recovery_policy
