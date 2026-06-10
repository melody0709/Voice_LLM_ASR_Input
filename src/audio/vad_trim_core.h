#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <deque>
#include <vector>

struct VadTrimCoreStats {
    size_t rawBytes = 0;
    size_t outputBytes = 0;
    bool detectedSpeech = false;
};

class VadTrimCore {
public:
    void Reset();
    // Appends newly emitted PCM chunks to outputs; callers should clear outputs first
    // when they need per-call results instead of accumulated results.
    void ProcessChunk(std::vector<BYTE> chunk,
                      bool hasVoice,
                      std::vector<std::vector<BYTE>>& outputs);
    void Finish();
    VadTrimCoreStats Stats() const;

private:
    enum class State {
        PreSpeech,
        InSpeech,
        PossibleTail,
    };

    void AppendOutput(const std::vector<BYTE>& chunk,
                      std::vector<std::vector<BYTE>>& outputs);

    State state_ = State::PreSpeech;
    int silentCount_ = 0;
    size_t rawBytes_ = 0;
    size_t outputBytes_ = 0;
    bool detectedSpeech_ = false;
    std::deque<std::vector<BYTE>> preBuffer_;
    std::vector<std::vector<BYTE>> tailBuffer_;
};
