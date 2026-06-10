#include "vad_trim_core.h"

#include <utility>

namespace {
constexpr int kTailSilentThreshold = 20;
constexpr size_t kPreSpeechKeep = 20;
}

void VadTrimCore::Reset() {
    state_ = State::PreSpeech;
    silentCount_ = 0;
    rawBytes_ = 0;
    outputBytes_ = 0;
    detectedSpeech_ = false;
    preBuffer_.clear();
    tailBuffer_.clear();
}

void VadTrimCore::AppendOutput(const std::vector<BYTE>& chunk,
                               std::vector<std::vector<BYTE>>& outputs) {
    if (chunk.empty()) return;
    outputBytes_ += chunk.size();
    outputs.push_back(chunk);
}

void VadTrimCore::ProcessChunk(std::vector<BYTE> chunk,
                               bool hasVoice,
                               std::vector<std::vector<BYTE>>& outputs) {
    if (chunk.empty()) return;
    rawBytes_ += chunk.size();
    if (hasVoice) detectedSpeech_ = true;

    if (state_ == State::PreSpeech) {
        if (hasVoice) {
            while (preBuffer_.size() > kPreSpeechKeep) {
                preBuffer_.pop_front();
            }
            for (const auto& buffered : preBuffer_) {
                AppendOutput(buffered, outputs);
            }
            preBuffer_.clear();
            AppendOutput(chunk, outputs);
            state_ = State::InSpeech;
        } else {
            preBuffer_.push_back(std::move(chunk));
        }
    } else if (state_ == State::InSpeech) {
        AppendOutput(chunk, outputs);
        if (!hasVoice) {
            ++silentCount_;
            if (silentCount_ >= kTailSilentThreshold) {
                state_ = State::PossibleTail;
                tailBuffer_.clear();
            }
        } else {
            silentCount_ = 0;
        }
    } else {
        tailBuffer_.push_back(std::move(chunk));
        if (hasVoice) {
            for (const auto& buffered : tailBuffer_) {
                AppendOutput(buffered, outputs);
            }
            tailBuffer_.clear();
            silentCount_ = 0;
            state_ = State::InSpeech;
        }
    }
}

void VadTrimCore::Finish() {
    preBuffer_.clear();
    tailBuffer_.clear();
    silentCount_ = 0;
}

VadTrimCoreStats VadTrimCore::Stats() const {
    VadTrimCoreStats stats;
    stats.rawBytes = rawBytes_;
    stats.outputBytes = outputBytes_;
    stats.detectedSpeech = detectedSpeech_;
    return stats;
}
