#include "qwen_audio_streaming_session.h"

#include "asr_diagnostics.h"
#include "asr_runtime_log.h"
#include "asr_result.h"
#include "asr_streaming_session_base.h"
#include "cloud_asr_common.h"
#include "globals.h"
#include "pending_pcm_buffer.h"
#include "qwen_context.h"
#include "qwen_finalize_policy.h"
#include "qwen_audio_streaming.h"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cwctype>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kRecordingWatchdogMs = 18000;
constexpr size_t kMaxReplayBytes = 120u * 32000u;
constexpr size_t kEmptyRetryMinBytes = 3u * 32000u;
constexpr DWORD kInitialConnectAttempts = 2;

void QwenAudioSessionDebugLog(const char* format, ...) {
    if (!format) return;
    va_list args;
    va_start(args, format);
    asr_runtime_log::WriteNamedV(L"qwen_audio_debug.log", format, args);
    va_end(args);
}

qwen_audio_streaming::Config BuildConfig(const Config& c) {
    qwen_audio_streaming::Config out;
    out.attemptId = c.asrAttemptId;
    out.apiKey = c.qwenApiKey;
    out.baseUrl = c.qwenAudioStreamingBaseUrl;
    out.model = c.qwenModel;
    out.languageHints = c.qwenLanguageHints.empty() ? c.qwenLanguage : c.qwenLanguageHints;
    out.vocabularyId = c.qwenVocabularyId;
    out.vocabulary = c.qwenVocabulary;
    out.inputContextText = c.qwenInputContextSnapshotCaptured
        ? c.qwenInputContextSnapshot
        : L"";
    // A dynamic context refresh is only meaningful when the user has also
    // opted in to sending focused-field context at all.  Keep the privacy
    // boundary explicit even if an old config file enables only the refresh
    // flag.
    out.enableContinueContext =
        c.qwenEnableContinueContext && c.qwenEnableInputContext;
    out.specialWordReplaceList = c.qwenSpecialWordReplaceList;
    out.specialWordEmptyList = c.qwenSpecialWordEmptyList;
    out.systemReservedFilter = c.qwenSystemReservedFilter;
    out.semanticPunctuation = c.qwenSemanticPunctuation;
    out.maxSentenceSilenceMs = c.qwenMaxSentenceSilenceMs;
    out.multiThresholdMode = c.qwenMultiThresholdMode;
    out.heartbeat = c.qwenHeartbeat;
    out.speechNoiseThresholdEnabled = c.qwenSpeechNoiseThresholdEnabled;
    out.speechNoiseThreshold = c.qwenSpeechNoiseThreshold;
    return out;
}

std::wstring AudioErrorText(const std::wstring& error) {
    if (error.rfind(L"Qwen Audio ASR error:", 0) == 0) return error;
    return L"Qwen Audio ASR error: " + (error.empty() ? L"unknown error" : error);
}

struct RecognitionAttempt {
    std::wstring text;
    std::wstring error;
    bool ok = false;
};

class Session final : public StreamingAsrSessionBase {
public:
    Session(Config config,
            HWND target,
            AsrLlmRefineFn refine,
            std::wstring* raw)
        : StreamingAsrSessionBase(std::move(config), target, refine, raw),
          cfg_(BuildConfig(config_)) {}

    ~Session() override { Abort(); }

    bool Start(std::wstring& error) override {
        if (running_.load()) {
            error = L"Qwen Audio ASR error: session already running";
            return false;
        }
        pending_.Clear();
        abort_.store(false);
        streaming_.store(true);
        running_.store(true);
        recordingMs_.store(0.0);
        capturedBytes_.store(0);
        nextRetryStageIndex_ = 1;
        worker_ = std::thread([this] { Worker(); });
        return true;
    }

    bool EnqueuePcmChunk(const BYTE* data, size_t bytes) override {
        if (!data || bytes == 0 || abort_.load() || !streaming_.load()) return false;
        return pending_.Append(data, bytes);
    }

    void StopInput(double recordingMs, size_t capturedBytes) override {
        recordingMs_.store(recordingMs);
        capturedBytes_.store(capturedBytes);
        streaming_.store(false);
    }

    void Abort() override {
        abort_.store(true);
        streaming_.store(false);
        AbortActiveClient();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        running_.store(false);
    }

    bool IsRunning() const override { return running_.load(); }

    DWORD CurrentWatchdogMs() const override {
        if (streaming_.load()) return kRecordingWatchdogMs;
        const DWORD base = IsFallbackAsrEnabled(config_)
            ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs_.load(), capturedBytes_.load())
            : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs_.load(), capturedBytes_.load());
        // Reserve a bounded connection/replay attempt after the primary task.
        return (std::min<DWORD>)(base + 9000, 45000);
    }

    const wchar_t* ProviderName() const override { return L"Qwen Audio 3 ASR"; }

private:
    audio_diagnostics::StageMetadata PrimaryStage(
        std::wstring reason = {}) const {
        return asr_diagnostics::MakeStageMetadata(config_, std::move(reason));
    }

    audio_diagnostics::StageMetadata RetryStage(unsigned index,
                                                 std::wstring reason) const {
        return asr_diagnostics::MakeRetryStageMetadata(
            config_, index, std::move(reason));
    }

    void CompletePrimary(audio_diagnostics::StageTerminal terminal) {
        audio_diagnostics::CompleteStage(
            config_.asrAttemptId, config_.asrDiagnosticStageKind,
            config_.asrDiagnosticStageIndex, terminal);
    }

    void SetActiveClient(qwen_audio_streaming::Client* client) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        activeClient_ = client;
    }

    void ClearActiveClient(qwen_audio_streaming::Client* expected = nullptr) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (!expected || activeClient_ == expected) activeClient_ = nullptr;
    }

    void AbortActiveClient() {
        std::lock_guard<std::mutex> lock(clientMutex_);
        if (activeClient_) activeClient_->Abort();
    }

    // Ownership transfer and active-client clearing must be one serialized
    // operation.  Otherwise Session::Abort() could retain a raw pointer while
    // the idle manager acquires (or destroys) the same Client.
    void FinishClient(std::unique_ptr<qwen_audio_streaming::Client>& client,
                      bool allowReuse) {
        std::lock_guard<std::mutex> lock(clientMutex_);
        qwen_audio_streaming::Client* expected = client.get();
        if (allowReuse && !abort_.load()) {
            activeClient_ = nullptr;
            qwen_audio_streaming::ReleaseReusableClient(std::move(client));
        } else {
            if (client) client->Close();
            if (activeClient_ == expected) activeClient_ = nullptr;
        }
    }

    static bool AppendReplay(CloudAsrReplayBuffer& replay,
                             const BYTE* data,
                             size_t bytes) {
        if (!data || bytes == 0) return true;
        std::vector<BYTE> copy(data, data + bytes);
        const CloudReplayAppendResult result = replay.Append(copy);
        return result == CloudReplayAppendResult::Stored;
    }

    static size_t ChunkBytes(const Config& config) {
        return (std::clamp<size_t>(
            static_cast<size_t>(std::clamp(config.qwenChunkMs, 20, 1000)) * 32,
            640,
            32000));
    }

    RecognitionAttempt RetryRecognitionOnce(const std::vector<BYTE>& pcm,
                                             DWORD timeoutMs) {
        RecognitionAttempt result;
        if (pcm.empty()) {
            result.error = L"no PCM available for replay";
            return result;
        }

        const unsigned diagnosticIndex = nextRetryStageIndex_++;
        const audio_diagnostics::StageMetadata diagnostic = RetryStage(
            diagnosticIndex,
            config_.asrDiagnosticStageKind == audio_diagnostics::StageKind::Fallback
                ? L"fallback_replay" : L"empty_or_transport_replay");
        const ULONGLONG attemptStarted = GetTickCount64();
        qwen_audio_streaming::Client client(cfg_);
        SetActiveClient(&client);
        std::wstring error;
        if (!client.Connect(error)) {
            result.error = error;
            audio_diagnostics::StageTerminal terminal =
                asr_diagnostics::TerminalFromText(AudioErrorText(error));
            terminal.terminal = "connect_error";
            terminal.elapsedMs = static_cast<double>(GetTickCount64() - attemptStarted);
            audio_diagnostics::CompleteStage(
                config_.asrAttemptId, diagnostic.kind, diagnostic.index, terminal);
            client.Close();
            ClearActiveClient(&client);
            return result;
        }

        std::mutex stateMutex;
        std::mutex textMutex;
        std::wstring taskError;
        std::string providerCode;
        qwen_audio_streaming::TranscriptAccumulator transcript;
        std::atomic<bool> drainDone{false};
        std::atomic<bool> failed{false};
        std::atomic<bool> finished{false};
        std::atomic<bool> providerNoSpeech{false};
        std::atomic<bool> providerFailed{false};
        std::thread drain([&] {
            while (!drainDone.load() && !abort_.load()) {
                qwen_audio_streaming::Event event;
                std::wstring receiveError;
                if (!client.Poll(200, event, receiveError)) {
                    if (event.timeout) continue;
                    std::lock_guard<std::mutex> lock(stateMutex);
                    taskError = receiveError.empty() ? L"WebSocket receive failed" : receiveError;
                    failed.store(true);
                    break;
                }
                if (!event.heartbeat && !event.text.empty()) {
                    std::wstring displayText;
                    {
                        std::lock_guard<std::mutex> lock(textMutex);
                        transcript.Apply(event);
                        displayText = transcript.Text();
                    }
                    NotifyPartial(displayText, false);
                }
                if (event.taskFinished) {
                    finished.store(true);
                    break;
                }
                if (event.noSpeech) {
                    // The provider may report silence as task-failed rather
                    // than returning an empty final. Treat it as a clean
                    // empty recognition result, not an operational failure.
                    providerNoSpeech.store(true);
                    finished.store(true);
                    break;
                }
                if (event.failed) {
                    QwenAudioSessionDebugLog(
                        "event=task_failed retryable=%d error_code=%s message=%s",
                        event.retryable ? 1 : 0,
                        WideToUtf8(event.errorCode).c_str(),
                        WideToUtf8(event.message).c_str());
                    std::lock_guard<std::mutex> lock(stateMutex);
                    taskError = event.message.empty() ? L"task failed" : event.message;
                    providerCode = WideToUtf8(event.errorCode);
                    providerFailed.store(true);
                    failed.store(true);
                    break;
                }
            }
        });

        const size_t chunkBytes = ChunkBytes(config_);
        bool sendOk = true;
        for (size_t offset = 0; offset < pcm.size() && !abort_.load(); offset += chunkBytes) {
            const size_t bytes = (std::min)(chunkBytes, pcm.size() - offset);
            if (!client.SendAudio(pcm.data() + offset, bytes, error)) {
                sendOk = false;
                break;
            }
            audio_diagnostics::AppendStageInput(
                config_.asrAttemptId, diagnostic,
                pcm.data() + offset, bytes, bytes);
            // Replay the same real-time cadence as the primary stream. A
            // burst upload can trigger provider-side backpressure and create
            // a misleading task failure even when the socket is healthy.
            if (offset + bytes < pcm.size()) {
                Sleep(static_cast<DWORD>(std::clamp(config_.qwenChunkMs, 20, 1000)));
            }
        }
        if (sendOk && !abort_.load()) {
            if (!client.Finish(error)) sendOk = false;
        }

        const ULONGLONG deadline = GetTickCount64() + (std::max<DWORD>)(timeoutMs, 1000);
        while (sendOk && !abort_.load() && !failed.load() && !finished.load() &&
               GetTickCount64() < deadline) {
            Sleep(25);
        }
        if (sendOk && !abort_.load() && !failed.load() && !finished.load()) {
            std::lock_guard<std::mutex> lock(stateMutex);
            taskError = L"timed out waiting for task-finished";
            failed.store(true);
        }

        drainDone.store(true);
        if (failed.load() || abort_.load()) client.Abort();
        if (drain.joinable()) drain.join();

        {
            std::lock_guard<std::mutex> lock(textMutex);
            result.text = transcript.Text();
        }
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            result.error = taskError.empty() ? error : taskError;
        }
        result.ok = finished.load() && !failed.load() && !abort_.load();
        if (abort_.load()) result.error = L"aborted";
        audio_diagnostics::StageTerminal terminal;
        if (abort_.load()) {
            terminal.terminal = "aborted";
            terminal.reason = "cancelled";
        } else if (providerNoSpeech.load()) {
            terminal.terminal = "provider_no_words";
            terminal.reason = "no_speech";
        } else if (result.ok && result.text.empty()) {
            terminal.terminal = "task_finished_empty";
            terminal.reason = "no_speech";
        } else if (result.ok) {
            terminal.terminal = "task_finished";
            terminal.textChars = result.text.size();
        } else {
            terminal = asr_diagnostics::TerminalFromText(AudioErrorText(result.error));
            if (providerFailed.load()) terminal.terminal = "provider_error";
        }
        terminal.providerCode = providerCode;
        terminal.elapsedMs = static_cast<double>(GetTickCount64() - attemptStarted);
        audio_diagnostics::CompleteStage(
            config_.asrAttemptId, diagnostic.kind, diagnostic.index, terminal);
        client.Close();
        ClearActiveClient(&client);
        return result;
    }

    bool BufferUntilStop(CloudAsrReplayBuffer& replay,
                         std::vector<BYTE>& unsent,
                         bool keepReplay,
                         std::wstring& error) {
        bool replayOk = keepReplay && replay.Available();
        auto append = [&](const BYTE* data, size_t bytes) {
            if (!replayOk || !data || bytes == 0) return;
            if (!AppendReplay(replay, data, bytes)) {
                replayOk = false;
            }
        };

        append(unsent.data(), unsent.size());
        unsent.clear();
        NotifyStatus(L"Buffering... Qwen Audio ASR");
        while (streaming_.load() && !abort_.load()) {
            std::vector<BYTE> buffered;
            pending_.SwapTo(buffered);
            append(buffered.data(), buffered.size());
            if (pending_.Overflowed() && error.empty()) {
                error = L"audio buffer overflow while buffering";
                replayOk = false;
            }
            Sleep(20);
        }
        std::vector<BYTE> buffered;
        pending_.SwapTo(buffered);
        append(buffered.data(), buffered.size());
        if (pending_.Overflowed() && error.empty()) {
            error = L"audio buffer overflow while buffering";
            replayOk = false;
        }
        return replayOk && !pending_.Overflowed();
    }

    void DispatchAttempt(bool failed,
                         const std::wstring& error,
                         const std::wstring& text) {
        if (abort_.load()) return;
        DispatchFinal(failed ? AudioErrorText(error) : text);
    }

    void Worker() {
        const ULONGLONG workerStarted = GetTickCount64();
        CloudAsrReplayBuffer replay(kMaxReplayBytes);
        std::unique_ptr<qwen_audio_streaming::Client> client;
        std::wstring connectError;
        bool connected = false;
        bool connectRetryable = true;

        if (config_.qwenEnableInputContext) {
            QwenAudioSessionDebugLog(
                "event=input_context using_snapshot=%d captured=%d chars=%zu",
                config_.qwenInputContextSnapshotCaptured ? 1 : 0,
                cfg_.inputContextText.empty() ? 0 : 1,
                cfg_.inputContextText.size());
        }

        if (cfg_.apiKey.empty()) {
            std::wstring missingKeyError = L"missing DashScope API key";
            BufferUntilStop(replay, clientBuffer_, false, missingKeyError);
            CompletePrimary(asr_diagnostics::TerminalFromText(
                AudioErrorText(missingKeyError)));
            DispatchAttempt(true, missingKeyError, {});
            Finish();
            return;
        }

        for (DWORD attempt = 0; attempt < kInitialConnectAttempts && !abort_.load(); ++attempt) {
            QwenAudioSessionDebugLog("event=session_connect_attempt attempt=%lu max_attempts=%lu model=%s",
                                     static_cast<unsigned long>(attempt + 1),
                                     static_cast<unsigned long>(kInitialConnectAttempts),
                                     WideToUtf8(cfg_.model).c_str());
            client = qwen_audio_streaming::AcquireReusableClient(cfg_);
            if (!client) {
                client = std::make_unique<qwen_audio_streaming::Client>(cfg_);
            } else {
                QwenAudioSessionDebugLog("event=session_connection_reuse_candidate attempt=%lu",
                                         static_cast<unsigned long>(attempt + 1));
            }
            SetActiveClient(client.get());
            if (client->Connect(connectError)) {
                connected = true;
                QwenAudioSessionDebugLog("event=session_connect_ok attempt=%lu",
                                         static_cast<unsigned long>(attempt + 1));
                break;
            }
            connectRetryable = client->LastFailureRetryable();
            QwenAudioSessionDebugLog("event=session_connect_failed attempt=%lu retryable=%d error=%s",
                                     static_cast<unsigned long>(attempt + 1),
                                     connectRetryable ? 1 : 0,
                                     WideToUtf8(connectError).c_str());
            client->Close();
            ClearActiveClient(client.get());
            if (!connectRetryable) break;
            if (attempt + 1 < kInitialConnectAttempts && !abort_.load()) {
                NotifyStatus(L"Reconnecting... Qwen Audio ASR");
                Sleep(500);
            }
        }

        if (!connected) {
            const bool replayReady = BufferUntilStop(
                replay, clientBuffer_, connectRetryable, connectError);
            if (!abort_.load() && connectRetryable && replayReady &&
                replay.Available() && !replay.Empty()) {
                NotifyStatus(L"Retrying... Qwen Audio ASR");
                const DWORD timeout = IsFallbackAsrEnabled(config_)
                    ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs_.load(), replay.Size())
                    : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs_.load(), replay.Size());
                const RecognitionAttempt retry = RetryRecognitionOnce(replay.Data(), timeout);
                audio_diagnostics::StageTerminal primaryTerminal =
                    asr_diagnostics::TerminalFromText(AudioErrorText(connectError));
                primaryTerminal.terminal = "connect_error";
                CompletePrimary(primaryTerminal);
                DispatchAttempt(!retry.ok, retry.error, retry.text);
            } else {
                audio_diagnostics::StageTerminal primaryTerminal =
                    asr_diagnostics::TerminalFromText(AudioErrorText(connectError));
                primaryTerminal.terminal = "connect_error";
                CompletePrimary(primaryTerminal);
                DispatchAttempt(true, connectError, {});
            }
            Finish();
            return;
        }

        std::mutex textMutex;
        std::mutex stateMutex;
        std::wstring error;
        qwen_audio_streaming::TranscriptAccumulator transcript;
        std::atomic<bool> drainDone{false};
        std::atomic<bool> drainFailed{false};
        std::atomic<bool> drainRetryable{true};
        std::atomic<bool> taskFinished{false};
        std::atomic<bool> noSpeech{false};
        std::atomic<qwen_finalize_policy::TerminalReason> terminalReason{
            qwen_finalize_policy::TerminalReason::None};
        std::thread drain([&] {
            while (!drainDone.load() && !abort_.load()) {
                qwen_audio_streaming::Event event;
                std::wstring receiveError;
                if (!client->Poll(200, event, receiveError)) {
                    if (event.timeout) continue;
                    // Abort() is used to wake the receive thread after the
                    // worker has already chosen a terminal outcome. Do not let
                    // that expected cancellation overwrite Timeout/PeerClosed
                    // with a synthetic transport failure.
                    if (drainDone.load() || abort_.load()) break;
                    std::lock_guard<std::mutex> lock(stateMutex);
                    error = receiveError.empty() ? L"WebSocket receive failed" : receiveError;
                    terminalReason.store(qwen_finalize_policy::TerminalReason::TransportFailure);
                    drainFailed.store(true);
                    break;
                }
                if (!event.heartbeat && !event.text.empty()) {
                    std::wstring displayText;
                    {
                        std::lock_guard<std::mutex> lock(textMutex);
                        transcript.Apply(event);
                        displayText = transcript.Text();
                    }
                    NotifyPartial(displayText, false);
                }
                if (event.taskFinished) {
                    taskFinished.store(true);
                    break;
                }
                if (event.noSpeech) {
                    noSpeech.store(true);
                    drainFailed.store(true);
                    break;
                }
                if (event.failed) {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    error = event.message.empty() ? L"task failed" : event.message;
                    drainRetryable.store(event.retryable);
                    terminalReason.store(event.peerClosed
                        ? qwen_finalize_policy::TerminalReason::PeerClosed
                        : qwen_finalize_policy::TerminalReason::ProviderFailure);
                    drainFailed.store(true);
                    break;
                }
            }
        });

        const size_t chunkBytes = ChunkBytes(config_);
        std::vector<BYTE> chunk;
        chunk.reserve(chunkBytes * 2);
        bool failed = false;
        bool retryWithReplay = false;
        bool replayEnabled = true;
        bool finishTaskSent = false;

        auto sendChunk = [&](const BYTE* data, size_t bytes) -> bool {
            if (bytes == 0) return true;
            std::wstring sendError;
            const bool sent = client->SendAudio(data, bytes, sendError);
            if (replayEnabled && !AppendReplay(replay, data, bytes)) {
                // Replay is a recovery aid, not part of the primary live
                // path. Once its bounded budget is exhausted, keep sending
                // live PCM and simply disable replay for this attempt.
                replayEnabled = false;
            }
            if (!sent) {
                std::lock_guard<std::mutex> lock(stateMutex);
                error = sendError.empty() ? L"binary PCM send failed" : sendError;
                failed = true;
                retryWithReplay = replayEnabled;
                return false;
            }
            audio_diagnostics::AppendStageInput(
                config_.asrAttemptId, PrimaryStage(), data, bytes, bytes);
            return true;
        };

        while (!abort_.load() && !drainFailed.load()) {
            std::vector<BYTE> buffered;
            pending_.SwapTo(buffered);
            if (!buffered.empty()) chunk.insert(chunk.end(), buffered.begin(), buffered.end());
            if (pending_.Overflowed()) {
                std::lock_guard<std::mutex> lock(stateMutex);
                error = L"audio buffer overflow while recording";
                failed = true;
                retryWithReplay = false;
                break;
            }
            while (chunk.size() >= chunkBytes && !abort_.load()) {
                if (!sendChunk(chunk.data(), chunkBytes)) break;
                chunk.erase(chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(chunkBytes));
            }
            if (failed || drainFailed.load() || !streaming_.load()) break;
            Sleep(20);
        }

        if (drainFailed.load()) {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (error.empty()) error = L"WebSocket receive failed";
            failed = true;
            retryWithReplay = drainRetryable.load();
        }
        if (noSpeech.load()) {
            failed = false;
            retryWithReplay = false;
            std::lock_guard<std::mutex> lock(stateMutex);
            error.clear();
        }
        if (abort_.load()) {
            drainDone.store(true);
            client->Abort();
            if (drain.joinable()) drain.join();
            client->Close();
            ClearActiveClient(client.get());
            Finish();
            return;
        }

        if (failed || drainFailed.load()) {
            const bool replayReady = BufferUntilStop(
                replay, chunk, retryWithReplay && replayEnabled, error);
            retryWithReplay = retryWithReplay && replayReady && replay.Available();
        } else {
            std::vector<BYTE> buffered;
            pending_.SwapTo(buffered);
            if (!buffered.empty()) chunk.insert(chunk.end(), buffered.begin(), buffered.end());
            if (pending_.Overflowed()) {
                std::lock_guard<std::mutex> lock(stateMutex);
                error = L"audio buffer overflow while recording";
                failed = true;
                retryWithReplay = false;
            }
            if (!failed && !chunk.empty() && !sendChunk(chunk.data(), chunk.size())) {
                retryWithReplay = retryWithReplay && replayEnabled;
            }
            chunk.clear();
        }

        if (!noSpeech.load() && !failed && !drainFailed.load() && !abort_.load()) {
            // The focused control may have changed while the user was
            // holding the hotkey.  Refresh it once, from the session worker,
            // immediately before finish-task.  This keeps UI Automation and
            // network writes out of the audio callback and avoids polling.
            if (cfg_.enableContinueContext && !taskFinished.load() && !abort_.load()) {
                InputContextResult diagnostics;
                const std::wstring refreshed =
                    qwen_context::CaptureInputFieldText(&diagnostics);
                const bool readable = diagnostics.successLayer >= 0 &&
                    !diagnostics.timedOut && !diagnostics.isPassword;
                if (readable && refreshed != cfg_.inputContextText) {
                    std::wstring contextError;
                    if (!client->ContinueContext(refreshed, contextError)) {
                        QwenAudioSessionDebugLog(
                            "event=continue_task_ignored attempt=%llu reason=send_failed error_chars=%zu",
                            static_cast<unsigned long long>(cfg_.attemptId),
                            contextError.size());
                    } else {
                        QwenAudioSessionDebugLog(
                            "event=continue_task_context_refreshed attempt=%llu chars=%zu layer=%d",
                            static_cast<unsigned long long>(cfg_.attemptId), refreshed.size(),
                            diagnostics.successLayer);
                    }
                }
            }
            // A provider may have completed the task while the final PCM was
            // being drained.  In that case the terminal event is already the
            // finalization handshake; do not send finish-task to a completed
            // task a second time.
            if (taskFinished.load()) {
                finishTaskSent = true;
            } else {
                std::wstring finishError;
                if (!client->Finish(finishError)) {
                    {
                        std::lock_guard<std::mutex> lock(stateMutex);
                        error = finishError.empty() ? L"finish-task send failed" : finishError;
                    }
                    failed = true;
                    retryWithReplay = replayEnabled;
                } else {
                    finishTaskSent = true;
                    const DWORD timeout = CurrentWatchdogMs();
                    const ULONGLONG deadline = GetTickCount64() + timeout;
                    while (!abort_.load() && !drainFailed.load() && !taskFinished.load() &&
                           GetTickCount64() < deadline) {
                        Sleep(25);
                    }
                    if (!taskFinished.load()) {
                        if (terminalReason.load() == qwen_finalize_policy::TerminalReason::None) {
                            terminalReason.store(qwen_finalize_policy::TerminalReason::Timeout);
                        }
                        failed = true;
                        retryWithReplay = replayEnabled;
                        std::lock_guard<std::mutex> lock(stateMutex);
                        if (error.empty()) error = L"timed out waiting for task-finished";
                    }
                }
            }
        }

        const bool successfulTask = !failed && !drainFailed.load() &&
            !noSpeech.load() && !abort_.load() && finishTaskSent &&
            taskFinished.load();
        drainDone.store(true);
        if (!successfulTask || abort_.load()) client->Abort();
        if (drain.joinable()) drain.join();
        FinishClient(client, successfulTask);

        std::wstring finalText;
        std::wstring committedText;
        bool hasCommittedText = false;
        {
            std::lock_guard<std::mutex> lock(textMutex);
            finalText = transcript.Text();
            committedText = transcript.CommittedText();
            hasCommittedText = transcript.HasCommittedText();
        }
        const auto finalReason = terminalReason.load();
        // Only an incomplete transport finalization may recover already
        // committed sentences. An explicit task-failed remains a failure, and
        // an uncommitted partial is never promoted to a final transcript.
        if (!abort_.load() && !noSpeech.load() &&
            qwen_finalize_policy::CanRecoverAudioStreaming(
                finishTaskSent, taskFinished.load(), hasCommittedText, finalReason)) {
            finalText = committedText;
            QwenAudioSessionDebugLog(
                "event=transcript_recovered_despite_incomplete_finalize attempt=%llu reason=%s text_chars=%zu",
                static_cast<unsigned long long>(cfg_.attemptId),
                qwen_finalize_policy::TerminalReasonName(finalReason), finalText.size());
            failed = false;
            retryWithReplay = false;
            std::lock_guard<std::mutex> lock(stateMutex);
            error.clear();
        }
        std::wstring finalError;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            finalError = error;
        }

        audio_diagnostics::StageTerminal primaryTerminal;
        if (noSpeech.load()) {
            primaryTerminal.terminal = "provider_no_words";
            primaryTerminal.reason = "no_speech";
        } else if (failed || drainFailed.load()) {
            primaryTerminal = asr_diagnostics::TerminalFromText(
                AudioErrorText(finalError));
            switch (finalReason) {
            case qwen_finalize_policy::TerminalReason::Timeout:
                primaryTerminal.terminal = "timeout";
                break;
            case qwen_finalize_policy::TerminalReason::PeerClosed:
                primaryTerminal.terminal = "peer_close";
                break;
            case qwen_finalize_policy::TerminalReason::ProviderFailure:
                primaryTerminal.terminal = "provider_error";
                break;
            case qwen_finalize_policy::TerminalReason::TransportFailure:
                primaryTerminal.terminal = "transport_error";
                break;
            case qwen_finalize_policy::TerminalReason::None:
                break;
            }
        } else if (finalText.empty()) {
            primaryTerminal.terminal = "task_finished_empty";
            primaryTerminal.reason = "no_speech";
        } else {
            primaryTerminal.terminal = "task_finished";
            primaryTerminal.textChars = finalText.size();
            primaryTerminal.committedTextChars = committedText.size();
        }
        primaryTerminal.elapsedMs =
            static_cast<double>(GetTickCount64() - workerStarted);
        CompletePrimary(primaryTerminal);

        const bool retryEmpty = !noSpeech.load() && !failed && !abort_.load() && finalText.empty() &&
            replay.Available() && replay.Size() >= kEmptyRetryMinBytes;
        if (!abort_.load() && (retryWithReplay || retryEmpty) &&
            replay.Available() && !replay.Empty()) {
            NotifyStatus(L"Retrying... Qwen Audio ASR");
            const DWORD retryTimeout = IsFallbackAsrEnabled(config_)
                ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs_.load(), replay.Size())
                : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs_.load(), replay.Size());
            const RecognitionAttempt retry = RetryRecognitionOnce(replay.Data(), retryTimeout);
            if (retry.ok) {
                finalText = retry.text;
                failed = false;
            } else if (retryWithReplay) {
                failed = true;
                finalError = retry.error.empty() ? finalError : retry.error;
            }
        }

        DispatchAttempt(failed, finalError, finalText);
        Finish();
    }

    void Finish() {
        ClearActiveClient();
        running_.store(false);
    }

    qwen_audio_streaming::Config cfg_;
    PendingPcmBuffer pending_;
    std::vector<BYTE> clientBuffer_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::atomic<double> recordingMs_{0.0};
    std::atomic<size_t> capturedBytes_{0};
    std::thread worker_;
    std::mutex clientMutex_;
    qwen_audio_streaming::Client* activeClient_ = nullptr;
    unsigned nextRetryStageIndex_ = 1;
};

} // namespace

std::unique_ptr<IStreamingAsrSession> CreateQwenAudioStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText) {
    return std::make_unique<Session>(config, targetWindow, refineFn, lastRawAsrText);
}
