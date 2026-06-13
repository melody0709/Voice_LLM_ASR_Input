#include "asr_result.h"

#include "engine.h"

namespace {

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
    return text.rfind(prefix, 0) == 0;
}

} // namespace

std::wstring NormalizeAsrText(std::wstring text) {
    if (text.empty()) {
        return L"No speech detected";
    }
    return text;
}

bool IsOperationalAsrError(const std::wstring& text) {
    return StartsWith(text, L"ASR failed:")
        || StartsWith(text, L"Baidu ASR error:")
        || StartsWith(text, L"Baidu ASR failed:")
        || StartsWith(text, L"Qwen ASR error:")
        || StartsWith(text, L"Qwen ASR failed:")
        || StartsWith(text, L"MiMo ASR error:")
        || StartsWith(text, L"MiMo ASR failed:")
        || StartsWith(text, L"VolcEngine timeout")
        || StartsWith(text, L"VolcEngine connect failed")
        || StartsWith(text, L"VolcEngine error")
        || StartsWith(text, L"[VolcEngine error:");
}

bool IsUsableAsrTextForContext(const std::wstring& text) {
    if (text.empty() || text == L"(empty result)" || text == L"Too short" || text == L"No speech detected") {
        return false;
    }
    return !IsOperationalAsrError(text);
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
    return ModelDisplayName(config.modelId);
}

const char* AsrBackendDebugName(const std::wstring& asrBackend) {
    if (asrBackend == L"baidu") return "Baidu";
    if (asrBackend == L"volcengine") return "Volcengine";
    if (asrBackend == L"qwen") return "Qwen";
    if (asrBackend == L"mimo") return "MiMo";
    return "Local";
}
