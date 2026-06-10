#include "cloud_asr_common.h"

#include <algorithm>

CloudAsrReplayBuffer::CloudAsrReplayBuffer(size_t maxBytes)
    : maxBytes_(maxBytes) {
    data_.reserve((std::min)(maxBytes_, static_cast<size_t>(32000)));
}

CloudReplayAppendResult CloudAsrReplayBuffer::Append(const std::vector<BYTE>& chunk) {
    if (chunk.empty()) return CloudReplayAppendResult::IgnoredEmpty;
    if (!available_) return CloudReplayAppendResult::Disabled;

    if (data_.size() + chunk.size() > maxBytes_) {
        available_ = false;
        return CloudReplayAppendResult::LimitExceeded;
    }

    data_.insert(data_.end(), chunk.begin(), chunk.end());
    return CloudReplayAppendResult::Stored;
}

DWORD ComputeCloudAsrFinalizeTimeoutMs(double recordingMs, size_t pcmBytes) {
    const double audioMs = pcmBytes > 0
        ? static_cast<double>(pcmBytes) / kPcm16k16MonoBytesPerMs
        : recordingMs;
    const DWORD adaptive = static_cast<DWORD>(audioMs * 0.8 + 6000.0);
    return std::clamp<DWORD>(adaptive, 8000, 30000);
}

bool IsShortClosedWithoutText(bool streamingMode,
                              bool finalTextEmpty,
                              bool originalServerClosed,
                              size_t replayBytes,
                              size_t shortRetrySkipBytes) {
    return streamingMode
        && finalTextEmpty
        && originalServerClosed
        && replayBytes <= shortRetrySkipBytes;
}

bool ShouldRetryEmptyCloudFinal(bool streamingMode,
                                bool finalTextEmpty,
                                bool replayAvailable,
                                bool replayEmpty,
                                bool shortClosedWithoutText) {
    return streamingMode
        && finalTextEmpty
        && replayAvailable
        && !replayEmpty
        && !shortClosedWithoutText;
}
