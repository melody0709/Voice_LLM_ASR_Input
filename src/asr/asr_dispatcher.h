#pragma once

#include "globals.h"

#include <cstdint>
#include <string>

struct AsrFinalMetadata {
    uint64_t attemptId = 0;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
    std::wstring fallbackBackend;
};

struct AsrFinalMessage {
    uint64_t attemptId = 0;
    std::wstring text;
    Config resultConfig;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
    std::wstring fallbackBackend;
};

struct LlmFinalMessage {
    uint64_t attemptId = 0;
    std::wstring text;
    std::wstring rawAsrText;
    Config resultConfig;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
};

using AsrLlmRefineFn = void (*)(const AsrFinalMessage& finalMessage);

void DispatchAsrFinalText(HWND targetWindow,
                          std::wstring text,
                          const Config& config,
                          AsrLlmRefineFn refineFn,
                          std::wstring* lastRawAsrText,
                          const AsrFinalMetadata& metadata = {});
