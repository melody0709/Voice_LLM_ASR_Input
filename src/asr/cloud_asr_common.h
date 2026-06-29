#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <vector>

constexpr double kPcm16k16MonoBytesPerMs = 32.0;

enum class CloudReplayAppendResult {
    IgnoredEmpty,
    Stored,
    LimitExceeded,
    Disabled,
};

class CloudAsrReplayBuffer {
public:
    explicit CloudAsrReplayBuffer(size_t maxBytes);

    CloudReplayAppendResult Append(const std::vector<BYTE>& chunk);
    bool Available() const { return available_; }
    bool Empty() const { return data_.empty(); }
    size_t Size() const { return data_.size(); }
    size_t MaxBytes() const { return maxBytes_; }
    const std::vector<BYTE>& Data() const { return data_; }

private:
    size_t maxBytes_ = 0;
    bool available_ = true;
    std::vector<BYTE> data_;
};

DWORD ComputeCloudAsrLegacyFinalizeTimeoutMs(double recordingMs, size_t pcmBytes);
DWORD ComputeCloudAsrStreamingFinalWaitMs(double recordingMs, size_t pcmBytes);
DWORD ComputeCloudAsrRecordedRequestTimeoutMs(double recordingMs, size_t pcmBytes);
DWORD ComputeCloudAsrFinalizeTimeoutMs(double recordingMs, size_t pcmBytes);

bool IsShortClosedWithoutText(bool streamingMode,
                              bool finalTextEmpty,
                              bool originalServerClosed,
                              size_t replayBytes,
                              size_t shortRetrySkipBytes);

bool ShouldRetryEmptyCloudFinal(bool streamingMode,
                                bool finalTextEmpty,
                                bool replayAvailable,
                                bool replayEmpty,
                                bool shortClosedWithoutText);
