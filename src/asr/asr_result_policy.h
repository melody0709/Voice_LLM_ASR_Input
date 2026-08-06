#pragma once

#include <string>

namespace asr_result_policy {

inline bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
    return text.rfind(prefix, 0) == 0;
}

inline bool LooksLikeOperationalPrefix(const std::wstring& text) {
    return StartsWith(text, L"ASR failed:")
        || StartsWith(text, L"Fallback failed:")
        || StartsWith(text, L"Baidu ASR error:")
        || StartsWith(text, L"Baidu ASR failed:")
        || StartsWith(text, L"Qwen ASR error:")
        || StartsWith(text, L"Qwen ASR failed:")
        || StartsWith(text, L"Qwen IME ASR error:")
        || StartsWith(text, L"Qwen IME ASR failed:")
        || StartsWith(text, L"Qwen IME (Free) error:")
        || StartsWith(text, L"Qwen IME rewrite failed:")
        || StartsWith(text, L"MiMo ASR error:")
        || StartsWith(text, L"MiMo ASR failed:")
        || StartsWith(text, L"Doubao IME ASR error:")
        || StartsWith(text, L"Doubao IME ASR failed:")
        || StartsWith(text, L"Doubao IME error:")
        || StartsWith(text, L"VolcEngine timeout")
        || StartsWith(text, L"VolcEngine connect failed")
        || StartsWith(text, L"VolcEngine error")
        || StartsWith(text, L"[VolcEngine error:");
}

} // namespace asr_result_policy
