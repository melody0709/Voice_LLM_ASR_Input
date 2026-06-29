#include "asr_session.h"

#include "baidu_asr.h"
#include "asr_result.h"
#include "batch_vad_trimmer.h"
#include "cloud_asr_common.h"
#include "doubao_ime_asr.h"
#include "doubao_ime_config.h"
#include "engine.h"
#include "mimo_asr.h"
#include "qwen_asr.h"

#include <cstdint>
#include <cstdio>
#include <utility>

namespace {

class LocalAsrSession final : public BatchAsrSessionBase {
public:
    LocalAsrSession(Config config,
                    AsrEngine& engine,
                    std::vector<float>&& preprocessedSamples)
        : config_(std::move(config)),
          engine_(engine),
          preprocessedSamples_(std::move(preprocessedSamples)) {}

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::Local;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();

        if (aborted_) {
            result.text = L"ASR failed: aborted";
            return result;
        }

        std::vector<float> samples;
        Config workConfig = config_;

        if (!preprocessedSamples_.empty()) {
            result.vadTrimmedSamples = preprocessedSamples_.size();
            samples = std::move(preprocessedSamples_);
            workConfig.enableVad = false;
        } else {
            samples = PcmToFloat(pcm_);
        }

        result.text = NormalizeAsrText(engine_.Recognize(samples, 16000, workConfig));
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"Local ASR";
    }

private:
    Config config_;
    AsrEngine& engine_;
    std::vector<float> preprocessedSamples_;
};

class BaiduAsrSession final : public BatchAsrSessionBase {
public:
    BaiduAsrSession(Config config, AsrEngine& engine)
        : config_(std::move(config)),
          engine_(engine) {}

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::BaiduBatch;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();

        if (aborted_) {
            result.text = L"Baidu ASR failed: aborted";
            return result;
        }

        if (config_.baiduApiKey.empty() || config_.baiduSecretKey.empty()) {
            result.text = L"Baidu ASR error: missing API key";
            return result;
        }

        std::vector<BYTE> uploadPcm = pcm_;
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                printf("[Baidu diag] VAD trim disabled: %ls\n", vad.error.c_str());
            }
        }

        baidu_asr::BaiduConfig bcfg;
        bcfg.apiKey = config_.baiduApiKey;
        bcfg.secretKey = config_.baiduSecretKey;
        bcfg.devPid = config_.baiduDevPid;

        HiResTimer timer;
        result.text = NormalizeAsrText(baidu_asr::Recognize(uploadPcm, bcfg));
        result.cloudApiMs = timer.ElapsedMs();
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"Baidu Cloud";
    }

private:
    Config config_;
    AsrEngine& engine_;
};

class QwenAsrSession final : public BatchAsrSessionBase {
public:
    QwenAsrSession(Config config, AsrEngine& engine)
        : config_(std::move(config)),
          engine_(engine) {}

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::QwenRealtimeBatch;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();

        if (aborted_) {
            result.text = L"Qwen ASR error: aborted";
            return result;
        }

        if (config_.qwenApiKey.empty()) {
            result.text = L"Qwen ASR error: missing API key";
            return result;
        }

        std::vector<BYTE> uploadPcm = pcm_;
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                printf("[Qwen diag] VAD trim disabled: %ls\n", vad.error.c_str());
            }
        }

        qwen_asr::QwenConfig qcfg;
        qcfg.apiKey = config_.qwenApiKey;
        qcfg.baseUrl = config_.qwenBaseUrl;
        qcfg.model = config_.qwenModel;
        qcfg.language = config_.qwenLanguage;
        qcfg.turnDetection = L"manual";
        qcfg.chunkMs = config_.qwenChunkMs;

        const DWORD finalTimeout = ComputeCloudAsrRecordedRequestTimeoutMs(0.0, uploadPcm.size());

        HiResTimer timer;
        result.text = NormalizeAsrText(qwen_asr::Recognize(uploadPcm, qcfg, finalTimeout));
        result.cloudApiMs = timer.ElapsedMs();
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"Qwen ASR";
    }

private:
    Config config_;
    AsrEngine& engine_;
};

class MimoAsrSession final : public BatchAsrSessionBase {
public:
    MimoAsrSession(Config config, AsrEngine& engine)
        : config_(std::move(config)),
          engine_(engine) {}

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::MimoBatch;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();

        if (aborted_) {
            result.text = L"MiMo ASR error: aborted";
            return result;
        }

        if (config_.mimoApiKey.empty()) {
            result.text = L"MiMo ASR error: missing API key";
            return result;
        }

        std::vector<BYTE> uploadPcm = pcm_;
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                printf("[MiMo diag] VAD trim disabled: %ls\n", vad.error.c_str());
            }
        }

        mimo_asr::MimoConfig mcfg;
        mcfg.apiKey = config_.mimoApiKey;
        mcfg.baseUrl = config_.mimoBaseUrl;
        mcfg.model = config_.mimoModel;
        mcfg.language = config_.mimoLanguage;

        HiResTimer timer;
        result.text = NormalizeAsrText(mimo_asr::Recognize(uploadPcm, mcfg));
        result.cloudApiMs = timer.ElapsedMs();
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"MiMo ASR";
    }

private:
    Config config_;
    AsrEngine& engine_;
};

class DoubaoImeRecordedSession final : public BatchAsrSessionBase {
public:
    explicit DoubaoImeRecordedSession(Config config)
        : config_(std::move(config)) {}

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::DoubaoImeRecorded;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();

        if (aborted_) {
            result.text = L"Doubao IME ASR error: aborted";
            return result;
        }

        doubao_ime_asr::DoubaoImeConfig dcfg = BuildDoubaoImeConfigFromConfig(config_);
        const DWORD finalTimeout = ComputeCloudAsrRecordedRequestTimeoutMs(0.0, pcm_.size());

        doubao_ime_asr::RecordedRecognitionResult recorded =
            doubao_ime_asr::RecognizeRecordedPcm(dcfg, pcm_, finalTimeout);

        result.cloudApiMs = recorded.elapsedMs;
        result.doubaoImeClearCredentials = recorded.clearCredentials;
        if (recorded.credentialsChanged) {
            result.doubaoImeCredentialsChanged = true;
            result.doubaoImeDeviceId = recorded.credentials.deviceId;
            result.doubaoImeCdid = recorded.credentials.cdid;
            result.doubaoImeToken = recorded.credentials.token;
        }

        if (!recorded.ok) {
            result.text = doubao_ime_asr::ErrorText(recorded.error);
            return result;
        }

        result.text = NormalizeAsrText(recorded.text);
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"Doubao IME";
    }

private:
    Config config_;
};

} // namespace

bool BatchAsrSessionBase::Start(std::wstring& error) {
    error.clear();
    aborted_ = false;
    return true;
}

bool BatchAsrSessionBase::EnqueuePcmChunk(const BYTE* data, size_t bytes) {
    if (aborted_) return false;
    if (!data || bytes == 0) return true;
    pcm_.insert(pcm_.end(), data, data + bytes);
    return true;
}

void BatchAsrSessionBase::Abort() {
    aborted_ = true;
    pcm_.clear();
}

bool BatchAsrSessionBase::IsStreaming() const {
    return false;
}

std::unique_ptr<IAsrSession> CreateBatchAsrSession(
    const Config& config,
    AsrEngine& localEngine,
    std::vector<float>&& localPreprocessedSamples) {
    if (config.asrBackend == L"baidu") {
        return std::make_unique<BaiduAsrSession>(config, localEngine);
    }
    if (config.asrBackend == L"qwen") {
        return std::make_unique<QwenAsrSession>(config, localEngine);
    }
    if (config.asrBackend == L"mimo") {
        return std::make_unique<MimoAsrSession>(config, localEngine);
    }
    if (config.asrBackend == L"doubao_ime") {
        return std::make_unique<DoubaoImeRecordedSession>(config);
    }

    return std::make_unique<LocalAsrSession>(
        config,
        localEngine,
        std::move(localPreprocessedSamples));
}
