#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "audio_diagnostics.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mimo_asr {

constexpr wchar_t kDefaultBaseUrl[] = L"https://token-plan-ams.xiaomimimo.com/v1";
constexpr wchar_t kDefaultModel[] = L"mimo-v2.5-asr";
constexpr wchar_t kDefaultLanguage[] = L"auto";

struct MimoConfig {
    std::wstring apiKey;
    std::wstring baseUrl = kDefaultBaseUrl;
    std::wstring model = kDefaultModel;
    std::wstring language = kDefaultLanguage;
    uint64_t diagnosticAttemptId = 0;
    audio_diagnostics::StageKind diagnosticStageKind =
        audio_diagnostics::StageKind::Primary;
    unsigned diagnosticStageIndex = 0;
};

struct TestResult {
    bool ok = false;
    std::wstring message;
};

std::wstring Recognize(const std::vector<BYTE>& pcm16k16Mono, const MimoConfig& cfg);
TestResult TestConnection(const MimoConfig& cfg);

} // namespace mimo_asr
