#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include "globals.h"

#include <string>

using AsrPartialCallback = void(*)(const std::wstring& text, bool isFinal, void* userData);
// bundledPostProcessApplied is true when the provider has already produced
// the final post-processed text (for example Qwen-free's VoiceInputWrite or
// selection rewrite response).  The main pipeline must not run its generic
// LLM on that text a second time.
using AsrFinalCallback = void(*)(std::wstring text,
                                 const Config& config,
                                 bool bundledPostProcessApplied,
                                 void* userData);

class IStreamingAsrSession {
public:
    virtual ~IStreamingAsrSession() = default;

    virtual bool Start(std::wstring& error) = 0;
    virtual bool EnqueuePcmChunk(const BYTE* data, size_t bytes) = 0;
    virtual void StopInput(double recordingMs, size_t capturedPcmBytes) = 0;
    virtual void Abort() = 0;
    virtual bool IsRunning() const = 0;
    virtual DWORD CurrentWatchdogMs() const = 0;
    virtual const wchar_t* ProviderName() const = 0;
    virtual void SetPartialCallback(AsrPartialCallback cb, void* userData) = 0;
    virtual void SetFinalCallback(AsrFinalCallback cb, void* userData) = 0;
};
