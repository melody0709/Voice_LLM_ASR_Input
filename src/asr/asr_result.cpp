#include "asr_result.h"

#include "engine.h"

#include <algorithm>
#include <cwctype>

namespace {

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
    return text.rfind(prefix, 0) == 0;
}

bool Contains(const std::wstring& text, const wchar_t* needle) {
    return text.find(needle) != std::wstring::npos;
}

std::wstring Lower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return text;
}

bool LooksLikeOperationalPrefix(const std::wstring& text) {
    return StartsWith(text, L"ASR failed:")
        || StartsWith(text, L"Fallback failed:")
        || StartsWith(text, L"Baidu ASR error:")
        || StartsWith(text, L"Baidu ASR failed:")
        || StartsWith(text, L"Qwen ASR error:")
        || StartsWith(text, L"Qwen ASR failed:")
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

AsrFailureReason ClassifyFailureReason(const std::wstring& text) {
    const std::wstring lower = Lower(text);
    if (Contains(lower, L"timeout") || Contains(lower, L"timed out")) {
        return AsrFailureReason::Timeout;
    }
    if (Contains(lower, L"missing") || Contains(lower, L"api key") ||
        Contains(lower, L"auth") || Contains(lower, L"token") ||
        Contains(lower, L"401") || Contains(lower, L"403")) {
        return AsrFailureReason::AuthOrConfig;
    }
    if (Contains(lower, L"model load") || Contains(lower, L"model is not available")) {
        return AsrFailureReason::ModelLoad;
    }
    if (Contains(lower, L"buffer") || Contains(lower, L"exceeds")) {
        return AsrFailureReason::BufferOverflow;
    }
    if (Contains(lower, L"network") || Contains(lower, L"connect") ||
        Contains(lower, L"winhttp") || Contains(lower, L"websocket") ||
        Contains(lower, L"transport") || Contains(lower, L"receive") ||
        Contains(lower, L"send failed") || Contains(lower, L"http 5") ||
        Contains(lower, L"429") || Contains(lower, L"408")) {
        return AsrFailureReason::Network;
    }
    if (Contains(lower, L"error") || Contains(lower, L"failed")) {
        return AsrFailureReason::ProviderError;
    }
    return AsrFailureReason::Unknown;
}

} // namespace

std::wstring NormalizeAsrText(std::wstring text) {
    if (text.empty()) {
        return L"No speech detected";
    }
    return text;
}

AsrResultClassification ClassifyAsrResult(const std::wstring& text) {
    if (text.empty() || text == L"No speech detected" || text == L"(empty result)") {
        return {AsrResultKind::NoSpeech, AsrFailureReason::None};
    }
    if (text == L"Too short") {
        return {AsrResultKind::TooShort, AsrFailureReason::None};
    }
    if (LooksLikeOperationalPrefix(text)) {
        return {AsrResultKind::OperationalError, ClassifyFailureReason(text)};
    }
    if (Lower(text) == L"aborted" || Lower(text) == L"cancelled" || Lower(text) == L"canceled") {
        return {AsrResultKind::Cancelled, AsrFailureReason::None};
    }
    return {AsrResultKind::UsableText, AsrFailureReason::None};
}

bool IsOperationalAsrError(const std::wstring& text) {
    return ClassifyAsrResult(text).kind == AsrResultKind::OperationalError;
}

bool IsUsableAsrTextForContext(const std::wstring& text) {
    return ClassifyAsrResult(text).kind == AsrResultKind::UsableText;
}

bool ShouldRunLlmRefine(const Config& config, const std::wstring& text) {
    return config.postprocess == L"llm"
        && !config.llmEndpoint.empty()
        && !config.llmApiKey.empty()
        && IsUsableAsrTextForContext(text);
}

std::wstring AsrBackendDisplayName(const Config& config) {
    if (config.asrBackend == L"baidu") return L"Baidu Cloud";
    if (config.asrBackend == L"volcengine") return L"Volcano Engine";
    if (config.asrBackend == L"qwen") return L"Qwen ASR";
    if (config.asrBackend == L"mimo") return L"MiMo ASR";
    if (config.asrBackend == L"doubao_ime") return L"Doubao IME";
    return ModelDisplayName(config.modelId);
}

const char* AsrBackendDebugName(const std::wstring& asrBackend) {
    if (asrBackend == L"baidu") return "Baidu";
    if (asrBackend == L"volcengine") return "Volcengine";
    if (asrBackend == L"qwen") return "Qwen";
    if (asrBackend == L"mimo") return "MiMo";
    if (asrBackend == L"doubao_ime") return "DoubaoIME";
    return "Local";
}

bool IsSupportedFallbackBackend(const std::wstring& backend) {
    return backend == L"local" || backend == L"baidu" ||
           backend == L"qwen" || backend == L"mimo";
}

bool IsFallbackAsrEnabled(const Config& config) {
    return IsSupportedFallbackBackend(config.fallbackAsrBackend) &&
           config.fallbackAsrBackend != config.asrBackend;
}

Config BuildFallbackConfig(const Config& primary) {
    Config fallback = primary;
    fallback.asrBackend = primary.fallbackAsrBackend;
    fallback.fallbackAsrBackend = L"none";
    return fallback;
}

bool ShouldRunFallback(const Config& primary,
                       const std::wstring& text,
                       bool fallbackAlreadyAttempted,
                       bool selfAbortOrStaleAttempt) {
    if (fallbackAlreadyAttempted || selfAbortOrStaleAttempt) return false;
    if (!IsFallbackAsrEnabled(primary)) return false;
    return ClassifyAsrResult(text).kind == AsrResultKind::OperationalError;
}
