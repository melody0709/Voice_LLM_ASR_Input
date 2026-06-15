#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "asr_dispatcher.h"
#include "asr_streaming_session.h"

#include <cstddef>
#include <memory>
#include <string>

std::unique_ptr<IStreamingAsrSession> CreateVolcengineStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText);

size_t VolcengineRecognitionHistorySize();

// Lifecycle management for the persistent VolcSession (hSession + hConnect).
// These replace direct access to the former global g_volcSession.
void VolcengineResetForNewSession();       // clear forceAbort + lastError before starting a new recording
void VolcenginePrewarmConnection();        // pre-create hSession + hConnect in background
void VolcengineClosePersistentConnection();// close hSession + hConnect (app exit or keep-alive off)
void VolcengineForceAbortAndCloseAll();    // force-abort + close all handles (WM_DESTROY)
