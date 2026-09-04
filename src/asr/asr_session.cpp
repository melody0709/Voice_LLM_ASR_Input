#include "asr_session.h"

#include "asr_runtime_log.h"
#include "baidu_asr.h"
#include "asr_diagnostics.h"
#include "asr_result.h"
#include "batch_vad_trimmer.h"
#include "cloud_asr_common.h"
#include "doubao_ime_asr.h"
#include "doubao_ime_config.h"
#include "engine.h"
#include "mai_transcribe.h"
#include "mimo_asr.h"
#include "qwen_asr.h"
#include "qwen_audio_http.h"
#include "qwen_context.h"
#include "utils.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <utility>

namespace {

std::vector<BYTE> FloatSamplesToPcm16(const std::vector<float>& samples) {
    std::vector<BYTE> pcm(samples.size() * sizeof(int16_t));
    auto* output = reinterpret_cast<int16_t*>(pcm.data());
    for (size_t i = 0; i < samples.size(); ++i) {
        output[i] = static_cast<int16_t>(std::clamp(
            samples[i] * 32768.0f, -32768.0f, 32767.0f));
    }
    return pcm;
}

void CompleteVadNoSpeech(const Config& config,
                         audio_diagnostics::StageMetadata metadata) {
    audio_diagnostics::StageTerminal terminal;
    terminal.terminal = "local_vad_no_speech";
    terminal.reason = "no_speech";
    audio_diagnostics::CompleteStageIfMissing(
        config.asrAttemptId, metadata, terminal);
}

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
        audio_diagnostics::StageMetadata diagnostic =
            asr_diagnostics::MakeStageMetadata(config_);
        diagnostic.vadInputBytes = pcm_.size();

        if (!preprocessedSamples_.empty()) {
            result.vadTrimmedSamples = preprocessedSamples_.size();
            samples = std::move(preprocessedSamples_);
            workConfig.enableVad = false;
            diagnostic.vadActive = true;
            diagnostic.vadDetectedSpeech = true;
            diagnostic.vadModel = config_.vadModel;
            diagnostic.vadOutputBytes = samples.size() * sizeof(int16_t);
        } else if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                diagnostic.vadActive = true;
                diagnostic.vadDetectedSpeech = vad.detectedSpeech;
                diagnostic.vadModel = vad.modelName;
                diagnostic.vadOutputBytes = vad.pcm.size();
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    CompleteVadNoSpeech(config_, diagnostic);
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                samples = PcmToFloat(vad.pcm);
                workConfig.enableVad = false;
            } else {
                samples = PcmToFloat(pcm_);
                if (config_.enableDebugMode && !vad.error.empty()) {
                    asr_runtime_log::Write("[Local diag] VAD trim disabled: %s", WideToUtf8(vad.error).c_str());
                }
            }
        } else {
            samples = PcmToFloat(pcm_);
        }

        if (diagnostic.vadOutputBytes == 0) {
            diagnostic.vadOutputBytes = samples.size() * sizeof(int16_t);
        }
        const std::vector<BYTE> recognizePcm = FloatSamplesToPcm16(samples);
        asr_diagnostics::RegisterInput(config_, recognizePcm, diagnostic);
        const HiResTimer diagnosticTimer;
        result.text = NormalizeAsrText(engine_.Recognize(samples, 16000, workConfig));
        asr_diagnostics::CompleteFromText(
            config_, result.text, "local_decode_complete", diagnosticTimer.ElapsedMs());
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
        audio_diagnostics::StageMetadata diagnostic =
            asr_diagnostics::MakeStageMetadata(config_);
        diagnostic.vadInputBytes = pcm_.size();
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                diagnostic.vadActive = true;
                diagnostic.vadDetectedSpeech = vad.detectedSpeech;
                diagnostic.vadModel = vad.modelName;
                diagnostic.vadOutputBytes = vad.pcm.size();
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    CompleteVadNoSpeech(config_, diagnostic);
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                asr_runtime_log::Write("[Baidu diag] VAD trim disabled: %s", WideToUtf8(vad.error).c_str());
            }
        }

        if (diagnostic.vadOutputBytes == 0) diagnostic.vadOutputBytes = uploadPcm.size();
        diagnostic.sentBytes = uploadPcm.size();
        asr_diagnostics::RegisterInput(config_, uploadPcm, diagnostic);

        baidu_asr::BaiduConfig bcfg;
        bcfg.apiKey = config_.baiduApiKey;
        bcfg.secretKey = config_.baiduSecretKey;
        bcfg.devPid = config_.baiduDevPid;
        bcfg.diagnosticAttemptId = config_.asrAttemptId;
        bcfg.diagnosticStageKind = config_.asrDiagnosticStageKind;
        bcfg.diagnosticStageIndex = config_.asrDiagnosticStageIndex;

        HiResTimer timer;
        result.text = NormalizeAsrText(baidu_asr::Recognize(uploadPcm, bcfg));
        result.cloudApiMs = timer.ElapsedMs();
        asr_diagnostics::CompleteIfMissingFromText(
            config_, result.text, "http_response", result.cloudApiMs);
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
        audio_diagnostics::StageMetadata diagnostic =
            asr_diagnostics::MakeStageMetadata(config_);
        diagnostic.vadInputBytes = pcm_.size();
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                diagnostic.vadActive = true;
                diagnostic.vadDetectedSpeech = vad.detectedSpeech;
                diagnostic.vadModel = vad.modelName;
                diagnostic.vadOutputBytes = vad.pcm.size();
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    CompleteVadNoSpeech(config_, diagnostic);
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                asr_runtime_log::Write("[Qwen diag] VAD trim disabled: %s", WideToUtf8(vad.error).c_str());
            }
        }

        if (diagnostic.vadOutputBytes == 0) diagnostic.vadOutputBytes = uploadPcm.size();
        diagnostic.sentBytes = uploadPcm.size();
        asr_diagnostics::RegisterInput(config_, uploadPcm, diagnostic);

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
        asr_diagnostics::CompleteFromText(
            config_, result.text, "response_done", result.cloudApiMs);
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
        audio_diagnostics::StageMetadata diagnostic =
            asr_diagnostics::MakeStageMetadata(config_);
        diagnostic.vadInputBytes = pcm_.size();
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                diagnostic.vadActive = true;
                diagnostic.vadDetectedSpeech = vad.detectedSpeech;
                diagnostic.vadModel = vad.modelName;
                diagnostic.vadOutputBytes = vad.pcm.size();
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    CompleteVadNoSpeech(config_, diagnostic);
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                asr_runtime_log::Write("[MiMo diag] VAD trim disabled: %s", WideToUtf8(vad.error).c_str());
            }
        }

        if (diagnostic.vadOutputBytes == 0) diagnostic.vadOutputBytes = uploadPcm.size();
        diagnostic.sentBytes = uploadPcm.size();
        asr_diagnostics::RegisterInput(config_, uploadPcm, diagnostic);

        mimo_asr::MimoConfig mcfg;
        mcfg.apiKey = config_.mimoApiKey;
        mcfg.baseUrl = config_.mimoBaseUrl;
        mcfg.model = config_.mimoModel;
        mcfg.language = config_.mimoLanguage;
        mcfg.diagnosticAttemptId = config_.asrAttemptId;
        mcfg.diagnosticStageKind = config_.asrDiagnosticStageKind;
        mcfg.diagnosticStageIndex = config_.asrDiagnosticStageIndex;

        HiResTimer timer;
        result.text = NormalizeAsrText(mimo_asr::Recognize(uploadPcm, mcfg));
        result.cloudApiMs = timer.ElapsedMs();
        asr_diagnostics::CompleteIfMissingFromText(
            config_, result.text, "http_response", result.cloudApiMs);
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"MiMo ASR";
    }

private:
    Config config_;
    AsrEngine& engine_;
};

class QwenAudioAsrSession final : public BatchAsrSessionBase {
public:
    QwenAudioAsrSession(Config config, AsrEngine& engine)
        : config_(std::move(config)), engine_(engine) {}

    bool Start(std::wstring& error) override {
        cancellation_.Reset();
        inputContextText_.clear();
        if (config_.qwenEnableInputContext) {
            if (config_.qwenInputContextSnapshotCaptured) {
                inputContextText_ = config_.qwenInputContextSnapshot;
            } else {
                // The worker updates the shared UI-context snapshot under its mutex.
                std::lock_guard<std::mutex> lk(g_inputContextMutex);
                inputContextText_ = qwen_context::CaptureInputFieldText(&g_inputContextResult);
            }
        }
        return BatchAsrSessionBase::Start(error);
    }

    void Abort() override {
        aborted_.store(true);
        cancellation_.Abort();
    }

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::QwenAudioBatch;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();
        if (aborted_) { result.text = L"Qwen Audio ASR error: aborted"; return result; }
        if (config_.qwenApiKey.empty()) { result.text = L"Qwen Audio ASR error: missing API key"; return result; }
        std::vector<BYTE> uploadPcm = pcm_;
        audio_diagnostics::StageMetadata diagnostic =
            asr_diagnostics::MakeStageMetadata(config_);
        diagnostic.vadInputBytes = pcm_.size();
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                diagnostic.vadActive = true;
                diagnostic.vadDetectedSpeech = vad.detectedSpeech;
                diagnostic.vadModel = vad.modelName;
                diagnostic.vadOutputBytes = vad.pcm.size();
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text = L"";
                    CompleteVadNoSpeech(config_, diagnostic);
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            }
        }
        if (diagnostic.vadOutputBytes == 0) diagnostic.vadOutputBytes = uploadPcm.size();
        diagnostic.sentBytes = uploadPcm.size();
        qwen_audio_http::Config cfg;
        cfg.apiKey = config_.qwenApiKey;
        cfg.baseUrl = config_.qwenHttpBaseUrl;
        cfg.model = config_.qwenModel;
        cfg.languageHints = config_.qwenLanguageHints.empty() ? config_.qwenLanguage : config_.qwenLanguageHints;
        cfg.vocabularyId = config_.qwenVocabularyId;
        cfg.vocabulary = config_.qwenVocabulary;
        cfg.inputContextText = inputContextText_;
        HiResTimer timer;
        const DWORD timeoutMs = ComputeCloudAsrRecordedRequestTimeoutMs(0.0, uploadPcm.size());
        qwen_audio_http::Result r;
        for (int attempt = 0; attempt < 2; ++attempt) {
            audio_diagnostics::StageMetadata stage = diagnostic;
            if (attempt > 0) {
                stage = asr_diagnostics::MakeRetryStageMetadata(
                    config_, static_cast<unsigned>(attempt),
                    config_.asrDiagnosticStageKind ==
                            audio_diagnostics::StageKind::Fallback
                        ? L"fallback_http_retry" : L"transient_http_retry");
                stage.vadEnabled = diagnostic.vadEnabled;
                stage.vadActive = diagnostic.vadActive;
                stage.vadDetectedSpeech = diagnostic.vadDetectedSpeech;
                stage.vadModel = diagnostic.vadModel;
                stage.vadInputBytes = diagnostic.vadInputBytes;
                stage.vadOutputBytes = diagnostic.vadOutputBytes;
                stage.sentBytes = diagnostic.sentBytes;
            }
            asr_diagnostics::RegisterInput(config_, uploadPcm, stage);
            r = qwen_audio_http::Recognize(uploadPcm, cfg, timeoutMs, &cancellation_);
            audio_diagnostics::StageTerminal terminal;
            if (r.ok && r.text.empty()) {
                terminal.terminal = r.statusCode == 400
                    ? "provider_no_words" : "http_success_empty";
                terminal.reason = "no_speech";
            } else {
                const std::wstring stageText = r.ok ? r.text : r.error;
                terminal = asr_diagnostics::TerminalFromText(
                    stageText, "http_success", r.elapsedMs);
            }
            terminal.elapsedMs = r.elapsedMs;
            terminal.providerCode = r.providerCode.empty()
                ? std::to_string(static_cast<unsigned long>(r.statusCode))
                : r.providerCode;
            audio_diagnostics::CompleteStage(
                config_.asrAttemptId, stage.kind, stage.index, terminal);
            if (aborted_.load() || r.ok || !r.retryable || attempt == 1) break;
            Sleep(150);
        }
        result.cloudApiMs = timer.ElapsedMs();
        result.text = r.ok ? NormalizeAsrText(r.text)
                           : (r.error.empty() ? L"Qwen Audio ASR error: request failed" : r.error);
        return result;
    }

    const wchar_t* ProviderName() const override { return L"Qwen Audio 3 ASR"; }

private:
    Config config_;
    AsrEngine& engine_;
    CloudHttpCancellation cancellation_;
    std::wstring inputContextText_;
};

class MaiAsrSession final : public BatchAsrSessionBase {
public:
    MaiAsrSession(Config config, AsrEngine& engine)
        : config_(std::move(config)), engine_(engine) {}

    bool Start(std::wstring& error) override {
        cancellation_.Reset();
        return BatchAsrSessionBase::Start(error);
    }

    void Abort() override {
        aborted_.store(true);
        cancellation_.Abort();
    }

    AsrSessionResult Finish() override {
        AsrSessionResult result;
        result.backend = AsrSessionBackend::MaiBatch;
        result.providerName = AsrBackendDisplayName(config_);
        result.pcmBytes = pcm_.size();
        if (aborted_) {
            result.text = L"MAI ASR error: aborted";
            return result;
        }

        std::vector<BYTE> uploadPcm = pcm_;
        audio_diagnostics::StageMetadata diagnostic =
            asr_diagnostics::MakeStageMetadata(config_);
        diagnostic.vadInputBytes = pcm_.size();
        if (config_.enableVad) {
            BatchVadTrimResult vad = TrimBatchPcm16WithVad(config_, engine_, pcm_);
            if (vad.active) {
                diagnostic.vadActive = true;
                diagnostic.vadDetectedSpeech = vad.detectedSpeech;
                diagnostic.vadModel = vad.modelName;
                diagnostic.vadOutputBytes = vad.pcm.size();
                result.vadMs = vad.elapsedMs;
                result.vadModelName = vad.modelName;
                if (!vad.detectedSpeech || vad.pcm.empty()) {
                    result.text.clear();
                    CompleteVadNoSpeech(config_, diagnostic);
                    return result;
                }
                result.vadTrimmedSamples = vad.pcm.size() / sizeof(int16_t);
                uploadPcm = std::move(vad.pcm);
            } else if (config_.enableDebugMode && !vad.error.empty()) {
                asr_runtime_log::Write(
                    "[MAI diag] VAD trim disabled: %s",
                    WideToUtf8(vad.error).c_str());
            }
        }
        if (diagnostic.vadOutputBytes == 0) {
            diagnostic.vadOutputBytes = uploadPcm.size();
        }
        diagnostic.sentBytes = uploadPcm.size();

        mai_transcribe::Config providerConfig;
        providerConfig.apiProvider = config_.maiApiProvider == L"azure"
            ? mai_transcribe::ApiProvider::AzureSpeech
            : mai_transcribe::ApiProvider::OpenRouter;
        providerConfig.apiKey = providerConfig.apiProvider ==
                mai_transcribe::ApiProvider::AzureSpeech
            ? config_.maiAzureApiKey
            : config_.maiOpenRouterApiKey;
        providerConfig.azureEndpoint = config_.maiAzureEndpoint;
        providerConfig.language = config_.maiLanguage;

        const DWORD timeoutMs =
            ComputeCloudAsrRecordedRequestTimeoutMs(0.0, uploadPcm.size());
        const HiResTimer totalTimer;
        mai_transcribe::Result response;
        for (int attempt = 0; attempt < 2; ++attempt) {
            audio_diagnostics::StageMetadata stage = diagnostic;
            if (attempt > 0) {
                stage = asr_diagnostics::MakeRetryStageMetadata(
                    config_, static_cast<unsigned>(attempt),
                    config_.asrDiagnosticStageKind ==
                            audio_diagnostics::StageKind::Fallback
                        ? L"fallback_http_retry"
                        : L"transient_http_retry");
                stage.vadEnabled = diagnostic.vadEnabled;
                stage.vadActive = diagnostic.vadActive;
                stage.vadDetectedSpeech = diagnostic.vadDetectedSpeech;
                stage.vadModel = diagnostic.vadModel;
                stage.vadInputBytes = diagnostic.vadInputBytes;
                stage.vadOutputBytes = diagnostic.vadOutputBytes;
                stage.sentBytes = diagnostic.sentBytes;
            }
            asr_diagnostics::RegisterInput(config_, uploadPcm, stage);
            response = mai_transcribe::Recognize(
                uploadPcm, providerConfig, timeoutMs, &cancellation_);
            stage.networkBytes = response.networkBytes;
            audio_diagnostics::UpdateStageMetadata(config_.asrAttemptId, stage);

            const std::wstring stageText = response.ok
                ? response.text
                : response.error;
            audio_diagnostics::StageTerminal terminal =
                asr_diagnostics::TerminalFromText(
                    stageText,
                    response.text.empty() ? "http_success_empty" : "http_success",
                    response.elapsedMs);
            terminal.providerCode = response.providerCode.empty()
                ? std::to_string(static_cast<unsigned long>(response.statusCode))
                : response.providerCode;
            audio_diagnostics::CompleteStage(
                config_.asrAttemptId, stage.kind, stage.index, terminal);

            if (aborted_.load() || response.ok || !response.retryable ||
                attempt == 1) {
                break;
            }
            SleepCloudHttpRetryBackoff(attempt);
        }

        result.cloudApiMs = totalTimer.ElapsedMs();
        result.text = response.ok
            ? NormalizeAsrText(response.text)
            : (response.error.empty()
                ? L"MAI ASR error: request failed"
                : response.error);
        return result;
    }

    const wchar_t* ProviderName() const override {
        return L"Microsoft MAI Transcribe 2";
    }

private:
    Config config_;
    AsrEngine& engine_;
    CloudHttpCancellation cancellation_;
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
            asr_diagnostics::CompleteIfMissingFromText(
                config_, result.text, "session_finished", result.cloudApiMs);
            return result;
        }

        result.text = NormalizeAsrText(recorded.text);
        asr_diagnostics::CompleteIfMissingFromText(
            config_, result.text,
            result.text.empty() ? "session_finished_empty" : "session_finished",
            result.cloudApiMs);
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
    aborted_.store(false);
    return true;
}

bool BatchAsrSessionBase::EnqueuePcmChunk(const BYTE* data, size_t bytes) {
    if (aborted_.load()) return false;
    if (!data || bytes == 0) return true;
    pcm_.insert(pcm_.end(), data, data + bytes);
    return true;
}

void BatchAsrSessionBase::Abort() {
    aborted_.store(true);
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
        if (config.qwenModel == L"qwen-audio-3.0-asr-flash") {
            return std::make_unique<QwenAudioAsrSession>(config, localEngine);
        }
        return std::make_unique<QwenAsrSession>(config, localEngine);
    }
    if (config.asrBackend == L"mimo") {
        return std::make_unique<MimoAsrSession>(config, localEngine);
    }
    if (config.asrBackend == L"mai") {
        return std::make_unique<MaiAsrSession>(config, localEngine);
    }
    if (config.asrBackend == L"doubao_ime") {
        return std::make_unique<DoubaoImeRecordedSession>(config);
    }

    return std::make_unique<LocalAsrSession>(
        config,
        localEngine,
        std::move(localPreprocessedSamples));
}
