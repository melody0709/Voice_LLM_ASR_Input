#pragma once

#include "globals.h"

#include <string>

enum class AsrResultKind {
    UsableText,
    NoSpeech,
    TooShort,
    OperationalError,
    Cancelled,
};

enum class AsrFailureReason {
    None,
    Timeout,
    Network,
    AuthOrConfig,
    ModelLoad,
    ProviderError,
    BufferOverflow,
    Unknown,
};

struct AsrResultClassification {
    AsrResultKind kind = AsrResultKind::UsableText;
    AsrFailureReason reason = AsrFailureReason::None;
};

std::wstring NormalizeAsrText(std::wstring text);
AsrResultClassification ClassifyAsrResult(const std::wstring& text);
const char* AsrResultKindDebugName(AsrResultKind kind);
const char* AsrFailureReasonDebugName(AsrFailureReason reason);
bool IsOperationalAsrError(const std::wstring& text);
bool IsUsableAsrTextForContext(const std::wstring& text);
bool ShouldRunLlmRefine(const Config& config, const std::wstring& text);
std::wstring AsrBackendDisplayName(const Config& config);
const char* AsrBackendDebugName(const std::wstring& asrBackend);
const char* AsrBackendLogName(const std::wstring& asrBackend);
bool IsSupportedFallbackBackend(const std::wstring& backend);
bool IsFallbackAsrEnabled(const Config& config);
Config BuildFallbackConfig(const Config& primary);
bool ShouldRunFallback(const Config& primary,
                       const std::wstring& text,
                       bool fallbackAlreadyAttempted,
                       bool selfAbortOrStaleAttempt);
