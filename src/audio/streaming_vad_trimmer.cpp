#include "streaming_vad_trimmer.h"

#include "engine.h"
#include "globals.h"

#include <algorithm>
#include <cstdint>
#include <utility>

bool StreamingVadTrimmer::Start(const Config& config, AsrEngine& engine, std::wstring* error) {
    Reset();
    if (!config.enableVad) {
        if (error) *error = L"VAD is disabled";
        return false;
    }

    const int threads = ResolveThreads(config.threads);
    engine.Lock();
    bool ok = engine.EnsureVadForConfig(config, threads);
    if (ok) {
        if (config.vadModel == L"firered") {
            if (engine.fireRedVad) engine.fireRedVad->Reset();
            else ok = false;
        } else {
            if (engine.vad) engine.vad->Reset();
            else ok = false;
        }
    }
    engine.Unlock();

    if (!ok) {
        if (error) *error = L"VAD model is not available";
        return false;
    }

    engine_ = &engine;
    active_.store(true);
    vadModel_ = config.vadModel;
    modelName_ = (config.vadModel == L"firered") ? L"FireRed" : L"Silero";
    return true;
}

void StreamingVadTrimmer::Reset() {
    engine_ = nullptr;
    active_.store(false);
    detectedSpeech_.store(false);
    vadModel_.clear();
    modelName_.clear();
    core_.Reset();
}

StreamingVadTrimStats StreamingVadTrimmer::Stats() const {
    VadTrimCoreStats coreStats = core_.Stats();
    StreamingVadTrimStats stats;
    stats.rawBytes = coreStats.rawBytes;
    stats.outputBytes = coreStats.outputBytes;
    stats.detectedSpeech = detectedSpeech_.load();
    stats.active = active_.load();
    stats.modelName = modelName_;
    return stats;
}

bool StreamingVadTrimmer::DetectVoice(const int16_t* samples, size_t sampleCount) {
    if (!active_.load() || !engine_ || !samples || sampleCount == 0) return false;

    std::vector<float> floatBuf(sampleCount);
    for (size_t i = 0; i < sampleCount; ++i) {
        floatBuf[i] = static_cast<float>(samples[i]) / 32768.0f;
    }

    bool hasVoice = false;
    engine_->Lock();
    if (vadModel_ == L"firered") {
        engine_->fireRedVad->Process(floatBuf.data(), static_cast<int>(floatBuf.size()));
        hasVoice = engine_->fireRedVad->IsInSpeech();
    } else {
        engine_->vad->AcceptWaveform(floatBuf.data(), static_cast<int32_t>(floatBuf.size()));
        hasVoice = engine_->vad->IsDetected();
    }
    engine_->Unlock();
    return hasVoice;
}

void StreamingVadTrimmer::ProcessPcm16(const BYTE* data, size_t bytes, std::vector<std::vector<BYTE>>& outputs) {
    if (!active_.load() || !data || bytes < sizeof(int16_t)) return;

    bytes -= bytes % sizeof(int16_t);
    std::vector<BYTE> frameData(data, data + bytes);
    const auto* samples = reinterpret_cast<const int16_t*>(frameData.data());
    const size_t sampleCount = frameData.size() / sizeof(int16_t);
    const bool hasVoice = DetectVoice(samples, sampleCount);
    if (hasVoice) {
        detectedSpeech_.store(true);
        g_vadDetectedVoice.store(true);
    }

    core_.ProcessChunk(std::move(frameData), hasVoice, outputs);
}

void StreamingVadTrimmer::Finish() {
    if (!active_.load()) return;
    core_.Finish();
}
