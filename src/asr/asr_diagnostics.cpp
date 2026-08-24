#include "asr_diagnostics.h"

#include "asr_result.h"
#include "qwen_audio_profile.h"

namespace asr_diagnostics {

std::wstring ModelName(const Config& config) {
    if (config.asrBackend == L"local") return config.modelId;
    if (config.asrBackend == L"baidu") {
        return L"dev_pid_" + std::to_wstring(config.baiduDevPid);
    }
    if (config.asrBackend == L"qwen") return config.qwenModel;
    if (config.asrBackend == L"volcengine") return config.volcResourceId;
    if (config.asrBackend == L"mimo") return config.mimoModel;
    if (config.asrBackend == L"doubao_ime") return L"doubao_ime_asr";
    if (config.asrBackend == L"qwen_free") return L"qwen_ime_free";
    return {};
}

std::wstring TransportName(const Config& config) {
    if (config.asrBackend == L"local") return L"local_offline";
    if (config.asrBackend == L"baidu") return L"batch_http_pcm";
    if (config.asrBackend == L"qwen") {
        if (config.qwenModel == qwen_audio_profile::kHttpModel) return L"audio_http";
        if (config.qwenModel == qwen_audio_profile::kStreamingModel) {
            return L"audio_streaming_websocket";
        }
        return L"realtime_websocket";
    }
    if (config.asrBackend == L"volcengine") {
        return L"websocket_" + config.volcMode;
    }
    if (config.asrBackend == L"mimo") return L"batch_http_wav_json";
    if (config.asrBackend == L"doubao_ime") return L"websocket_opus";
    if (config.asrBackend == L"qwen_free") return L"websocket_protobuf";
    return L"unknown";
}

audio_diagnostics::AttemptMetadata MakeAttemptMetadata(const Config& config) {
    audio_diagnostics::AttemptMetadata metadata;
    metadata.attemptId = config.asrAttemptId;
    metadata.mode = config.diagnosticAudioMode;
    metadata.backend = config.asrBackend;
    metadata.model = ModelName(config);
    metadata.transport = TransportName(config);
    return metadata;
}

audio_diagnostics::StageMetadata MakeStageMetadata(
    const Config& config,
    std::wstring reason) {
    audio_diagnostics::StageMetadata metadata;
    metadata.kind = config.asrDiagnosticStageKind;
    metadata.index = config.asrDiagnosticStageIndex;
    metadata.backend = config.asrBackend;
    metadata.model = ModelName(config);
    metadata.transport = TransportName(config);
    metadata.reason = std::move(reason);
    metadata.vadEnabled = config.enableVad;
    return metadata;
}

audio_diagnostics::StageMetadata MakeRetryStageMetadata(
    const Config& config,
    unsigned retryIndex,
    std::wstring reason) {
    audio_diagnostics::StageMetadata metadata =
        MakeStageMetadata(config, std::move(reason));
    // Keep retries performed by a fallback provider in the fallback
    // namespace.  Otherwise a primary retry at index 1 and a fallback retry
    // at index 1 would collapse into the same diagnostic stage.
    metadata.kind = audio_diagnostics::RetryStageKind(
        config.asrDiagnosticStageKind);
    metadata.index = audio_diagnostics::RetryStageIndex(
        config.asrDiagnosticStageKind,
        config.asrDiagnosticStageIndex,
        retryIndex);
    return metadata;
}

audio_diagnostics::StageTerminal TerminalFromText(
    const std::wstring& text,
    const char* successfulTerminal,
    double elapsedMs) {
    const AsrResultClassification classification = ClassifyAsrResult(text);
    audio_diagnostics::StageTerminal terminal;
    terminal.elapsedMs = elapsedMs;
    terminal.textChars = classification.kind == AsrResultKind::UsableText
        ? text.size() : 0;
    switch (classification.kind) {
    case AsrResultKind::UsableText:
        terminal.terminal = successfulTerminal ? successfulTerminal : "success";
        break;
    case AsrResultKind::NoSpeech:
        terminal.terminal = "no_speech";
        break;
    case AsrResultKind::TooShort:
        terminal.terminal = "too_short";
        break;
    case AsrResultKind::Cancelled:
        terminal.terminal = "aborted";
        terminal.reason = "cancelled";
        break;
    case AsrResultKind::OperationalError:
        terminal.reason = AsrFailureReasonDebugName(classification.reason);
        terminal.errorCategory = terminal.reason;
        switch (classification.reason) {
        case AsrFailureReason::Timeout: terminal.terminal = "timeout"; break;
        case AsrFailureReason::Network: terminal.terminal = "transport_error"; break;
        case AsrFailureReason::AuthOrConfig: terminal.terminal = "auth_or_config_error"; break;
        case AsrFailureReason::ModelLoad: terminal.terminal = "model_load_error"; break;
        case AsrFailureReason::ProviderError: terminal.terminal = "provider_error"; break;
        case AsrFailureReason::BufferOverflow: terminal.terminal = "buffer_overflow"; break;
        case AsrFailureReason::Unknown:
        case AsrFailureReason::None:
            terminal.terminal = "operational_error";
            break;
        }
        break;
    }
    return terminal;
}

audio_diagnostics::FinalResult FinalFromText(const std::wstring& text) {
    const AsrResultClassification classification = ClassifyAsrResult(text);
    audio_diagnostics::FinalResult result;
    result.reason = AsrFailureReasonDebugName(classification.reason);
    switch (classification.kind) {
    case AsrResultKind::UsableText:
        result.kind = audio_diagnostics::FinalKind::UsableText;
        result.reason = "none";
        break;
    case AsrResultKind::NoSpeech:
        result.kind = audio_diagnostics::FinalKind::NoSpeech;
        result.reason = "no_speech";
        break;
    case AsrResultKind::TooShort:
        result.kind = audio_diagnostics::FinalKind::TooShort;
        result.reason = "too_short";
        break;
    case AsrResultKind::Cancelled:
        result.kind = audio_diagnostics::FinalKind::Cancelled;
        result.reason = "cancelled";
        result.userCancelled = true;
        break;
    case AsrResultKind::OperationalError:
        result.kind = audio_diagnostics::FinalKind::OperationalError;
        break;
    }
    return result;
}

void RegisterInput(const Config& config,
                   const std::vector<BYTE>& pcm,
                   audio_diagnostics::StageMetadata metadata) {
    if (metadata.backend.empty()) metadata = MakeStageMetadata(config);
    audio_diagnostics::RegisterStageInput(config.asrAttemptId, metadata, pcm);
}

void CompleteFromText(const Config& config,
                      const std::wstring& text,
                      const char* successfulTerminal,
                      double elapsedMs) {
    audio_diagnostics::CompleteStage(
        config.asrAttemptId,
        config.asrDiagnosticStageKind,
        config.asrDiagnosticStageIndex,
        TerminalFromText(text, successfulTerminal, elapsedMs));
}

void CompleteIfMissingFromText(const Config& config,
                               const std::wstring& text,
                               const char* successfulTerminal,
                               double elapsedMs) {
    audio_diagnostics::CompleteStageIfMissing(
        config.asrAttemptId,
        MakeStageMetadata(config),
        TerminalFromText(text, successfulTerminal, elapsedMs));
}

} // namespace asr_diagnostics
