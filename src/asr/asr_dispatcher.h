#pragma once

#include "globals.h"
#include "selection_context.h"

#include <cstdint>
#include <string>

struct AsrFinalMetadata {
    uint64_t attemptId = 0;
    // Watchdog-driven recovery may continue after the primary streaming
    // attempt has been cancelled.  Keep this explicit so stale primary/LLM
    // messages remain rejected while the watchdog's own fallback result can
    // still be delivered.
    bool allowCancelledAttempt = false;
    // The provider already ran its own post-processing request.  Keep this
    // separate from Config so it is per-result metadata, not persisted user
    // configuration.
    bool bundledPostProcessApplied = false;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
    std::wstring fallbackBackend;
    SelectionContext selection;
};

struct AsrFinalMessage {
    uint64_t attemptId = 0;
    bool allowCancelledAttempt = false;
    bool bundledPostProcessApplied = false;
    std::wstring text;
    Config resultConfig;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
    std::wstring fallbackBackend;
    SelectionContext selection;
};

struct LlmFinalMessage {
    uint64_t attemptId = 0;
    bool allowCancelledAttempt = false;
    bool bundledPostProcessApplied = false;
    std::wstring text;
    std::wstring rawAsrText;
    Config resultConfig;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
    std::wstring fallbackBackend;
    SelectionContext selection;
};

using AsrLlmRefineFn = void (*)(const AsrFinalMessage& finalMessage);

void DispatchAsrFinalText(HWND targetWindow,
                          std::wstring text,
                          const Config& config,
                          AsrLlmRefineFn refineFn,
                          std::wstring* lastRawAsrText,
                          const AsrFinalMetadata& metadata = {});
