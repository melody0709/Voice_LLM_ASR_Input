#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "globals.h"

#include <memory>
#include <atomic>
#include <string>
#include <vector>

enum class AsrSessionBackend {
    Local,
    BaiduBatch,
    QwenRealtimeBatch,
    QwenAudioBatch,
    MimoBatch,
    DoubaoImeRecorded,
};

class AsrEngine;

struct AsrSessionResult {
    std::wstring text;
    std::wstring providerName;
    AsrSessionBackend backend = AsrSessionBackend::Local;
    bool isStreaming = false;
    // True when a streaming provider's own bundled post-processing already
    // produced the text returned in `text`.
    bool bundledPostProcessApplied = false;
    double cloudApiMs = 0.0;
    double vadMs = 0.0;
    size_t pcmBytes = 0;
    size_t vadTrimmedSamples = 0;
    std::wstring vadModelName;
    bool doubaoImeCredentialsChanged = false;
    bool doubaoImeClearCredentials = false;
    std::wstring doubaoImeDeviceId;
    std::wstring doubaoImeCdid;
    std::wstring doubaoImeToken;
};

class IAsrSession {
public:
    virtual ~IAsrSession() = default;

    virtual bool Start(std::wstring& error) = 0;
    virtual bool EnqueuePcmChunk(const BYTE* data, size_t bytes) = 0;
    virtual AsrSessionResult Finish() = 0;
    virtual void Abort() = 0;
    virtual const wchar_t* ProviderName() const = 0;
    virtual bool IsStreaming() const = 0;
};

class BatchAsrSessionBase : public IAsrSession {
public:
    bool Start(std::wstring& error) override;
    bool EnqueuePcmChunk(const BYTE* data, size_t bytes) override;
    void Abort() override;
    bool IsStreaming() const override;

protected:
    std::atomic<bool> aborted_{false};
    std::vector<BYTE> pcm_;
};

std::unique_ptr<IAsrSession> CreateBatchAsrSession(
    const Config& config,
    AsrEngine& localEngine,
    std::vector<float>&& localPreprocessedSamples);
