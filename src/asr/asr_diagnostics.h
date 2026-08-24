#pragma once

#include "audio_diagnostics.h"
#include "globals.h"

#include <string>
#include <vector>

namespace asr_diagnostics {

std::wstring ModelName(const Config& config);
std::wstring TransportName(const Config& config);

audio_diagnostics::AttemptMetadata MakeAttemptMetadata(const Config& config);
audio_diagnostics::StageMetadata MakeStageMetadata(
    const Config& config,
    std::wstring reason = {});
audio_diagnostics::StageMetadata MakeRetryStageMetadata(
    const Config& config,
    unsigned retryIndex,
    std::wstring reason = {});
audio_diagnostics::StageTerminal TerminalFromText(
    const std::wstring& text,
    const char* successfulTerminal = "success",
    double elapsedMs = 0.0);
audio_diagnostics::FinalResult FinalFromText(const std::wstring& text);

void RegisterInput(const Config& config,
                   const std::vector<BYTE>& pcm,
                   audio_diagnostics::StageMetadata metadata = {});
void CompleteFromText(const Config& config,
                      const std::wstring& text,
                      const char* successfulTerminal = "success",
                      double elapsedMs = 0.0);
void CompleteIfMissingFromText(const Config& config,
                               const std::wstring& text,
                               const char* successfulTerminal = "success",
                               double elapsedMs = 0.0);

} // namespace asr_diagnostics
