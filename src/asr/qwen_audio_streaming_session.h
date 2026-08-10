#pragma once

#include "asr_streaming_session.h"
#include "asr_dispatcher.h"

#include <memory>

std::unique_ptr<IStreamingAsrSession> CreateQwenAudioStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText);
