#include "qwen_free_streaming_session.h"

#include "asr_diagnostics.h"
#include "asr_result.h"
#include "asr_runtime_log.h"
#include "asr_streaming_session_base.h"
#include "cloud_asr_common.h"
#include "globals.h"
#include "pending_pcm_buffer.h"
#include "qwen_free_proto_asr.h"
#include "qwen_free_proto_llm.h"
#include "qwen_free_postprocess.h"
#include "qwen_free_recovery_policy.h"
#include "qwen_free_proto_unet.h"
#include "qwen_free_proto_utdid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

// 录音期 watchdog：与其它流式后端对齐。
constexpr DWORD kQwenFreeRecordingWatchdogMs = 18000;

// Stop 后等待 ASR final 的最大时长。
constexpr DWORD kQwenFreeFinalWaitMs = 12000;

// A stopped recording may first wait for the original final, then reconnect,
// replay and wait for a second final. Keep the main-window watchdog outside
// that whole bounded recovery window.
constexpr DWORD kQwenFreeReconnectBudgetMs = 20000;
constexpr int kQwenFreeMaxConnectAttempts = 2;
constexpr DWORD kQwenFreeReconnectBackoffMs = 500;

// A failed send can race the server's normal close after user.audio.stop.
// Keep one bounded replay so the provider can reconnect and resend the whole
// utterance instead of losing the last PCM chunk.
constexpr size_t kQwenFreeMaxReplayBytes = 120u * 32000u;
constexpr size_t kQwenFreeEmptyRetryMinBytes = 3u * 32000u;

// LLM 后处理最大等待时长。
constexpr DWORD kQwenFreeLlmWaitMs = 15000;

// 原版每累计 0xf00 字节（120ms）提交一个 user.audio.commit。
constexpr size_t kQwenFreePcmChunkBytes = 0xf00;

// pump 周期。
constexpr DWORD kQwenFreePumpSleepMs = 20;

// 单次 RecvFrame 超时（非阻塞轮询）。
constexpr DWORD kQwenFreeRecvTimeoutMs = 50;

// 预热 PCM 缓冲区：连续入队时合并到 ~100ms 再发送。
constexpr size_t kQwenFreePcmBufferTarget = 6400;  // 200ms

std::wstring QwenErrorText(const std::wstring& error) {
    if (error.rfind(L"Qwen IME ASR error:", 0) == 0) return error;
    return L"Qwen IME ASR error: " + (error.empty() ? L"unknown error" : error);
}

bool QwenFreeNeedsPostProcess(const Config& config) {
    return config.qwenFreeRewriteEnabled ||
           qwen_free_postprocess::Enabled({
               config.qwenFreePolishEnabled,
               config.qwenFreePunctEnabled,
               config.qwenFreeCorrectEnabled,
           });
}

class QwenFreeStreamingSession final : public StreamingAsrSessionBase {
public:
    QwenFreeStreamingSession(Config config,
                              HWND targetWindow,
                              AsrLlmRefineFn refineFn,
                              std::wstring* lastRawAsrText,
                              SelectionContext selection)
        : StreamingAsrSessionBase(std::move(config), targetWindow, refineFn, lastRawAsrText),
          selection_(std::move(selection)) {}

    ~QwenFreeStreamingSession() override {
        Abort();
    }

    bool Start(std::wstring& error) override {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        if (running_.load()) {
            error = QwenErrorText(L"session already running");
            return false;
        }

        // A worker that completed naturally still leaves a joinable
        // std::thread object behind.  Assigning a new thread to it would call
        // std::terminate(), so reap both generations before resetting any
        // worker-owned state.  This also prevents a late receiver from
        // observing fields that belong to the next recording.
        if (worker_.joinable()) {
            if (worker_.get_id() == std::this_thread::get_id()) {
                error = QwenErrorText(L"cannot restart session from worker thread");
                return false;
            }
            worker_.join();
        }
        if (receiver_.joinable()) {
            if (receiver_.get_id() == std::this_thread::get_id()) {
                error = QwenErrorText(L"cannot restart receiver from receiver thread");
                return false;
            }
            receiver_.join();
        }

        if (!asr_.ResetCancellationForNewSession()) {
            error = QwenErrorText(L"previous session is still closing");
            return false;
        }

        pendingAudio_.Clear();
        abort_.store(false);
        stopped_.store(false);
        streaming_.store(true);
        finalReceived_.store(false);
        finalDispatched_.store(false);
        recordingMs_.store(0.0);
        capturedPcmBytes_.store(0);
        lastPartial_.clear();
        lastFrameError_.clear();
        terminalWasFinal_ = false;
        terminalTextEmpty_ = false;
        replayPcm_.clear();
        replayPcm_.reserve(kQwenFreeMaxReplayBytes);
        replayComplete_ = true;
        nextRetryStageIndex_ = 1;
        replayInProgress_ = false;
        replayStageIndex_ = 0;
        primaryDiagnosticCompleted_.store(false);
        running_.store(true);

        // UTDID/signing setup, WinHTTP connect and user.session.start can block.
        // StartRecordingSession is reached from the keyboard-hook/UI thread, so
        // all blocking provider work must happen on this worker.
        worker_ = std::thread([this]() { WorkerLoop(); });
        return true;
    }

    bool EnqueuePcmChunk(const BYTE* data, size_t bytes) override {
        if (!data || bytes == 0 || abort_.load() || stopped_.load() || !streaming_.load()) return false;
        return pendingAudio_.Append(data, bytes);
    }

    void StopInput(double recordingMs, size_t capturedPcmBytes) override {
        recordingMs_.store(recordingMs);
        capturedPcmBytes_.store(capturedPcmBytes);
        // 最后的 WASAPI 回调可能已经入队。不能在这里把 streaming_ 置为 false，
        // 否则 worker 会直接跳到接收阶段，尾部 PCM 从未送达服务端。
        stopped_.store(true);
    }

    void Abort() override {
        std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex_);
        abort_.store(true);
        streaming_.store(false);
        receiverStop_.store(true);
        // Request cancellation without closing a handle from this thread.
        // The protocol layer serializes WinHTTP operation lifetime and the
        // worker performs the final Close after its current operation returns.
        asr_.RequestCancel();
        // The worker owns receiver_ and joins it from StopReceiver(). Joining
        // the worker first prevents Abort() and WorkerLoop() from concurrently
        // joining the same std::thread object.
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        if (receiver_.joinable() && receiver_.get_id() != std::this_thread::get_id()) {
            receiver_.join();
        }
        asr_.Close();
        running_.store(false);
    }

    bool IsRunning() const override { return running_.load(); }

    DWORD CurrentWatchdogMs() const override {
        if (stopped_.load()) {
            // The ASR final and the bundled VoiceInputWrite/Rewrite HTTP
            // request happen on the worker after StopInput. Reserve time for
            // both phases so a slow but healthy LLM response is not mistaken
            // for a streaming timeout by the main-window watchdog.
            return (2 * kQwenFreeFinalWaitMs) + kQwenFreeReconnectBudgetMs +
                   (QwenFreeNeedsPostProcess(config_) ? kQwenFreeLlmWaitMs : 0);
        }
        if (streaming_.load()) return kQwenFreeRecordingWatchdogMs;
        return kQwenFreeRecordingWatchdogMs;
    }

    const wchar_t* ProviderName() const override { return L"Qwen IME (Free)"; }

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

    audio_diagnostics::StageMetadata CurrentStage(
        std::wstring reason = {}) const {
        if (replayInProgress_) {
            return RetryStage(replayStageIndex_, std::move(reason));
        }
        return PrimaryStage(std::move(reason));
    }

    void CompleteCurrentStage(audio_diagnostics::StageTerminal terminal) {
        if (!replayInProgress_) {
            if (primaryDiagnosticCompleted_.exchange(true)) return;
            audio_diagnostics::CompleteStage(
                config_.asrAttemptId, config_.asrDiagnosticStageKind,
                config_.asrDiagnosticStageIndex, terminal);
            return;
        }
        const audio_diagnostics::StageMetadata stage =
            RetryStage(replayStageIndex_, L"replay");
        audio_diagnostics::CompleteStage(
            config_.asrAttemptId, stage.kind, stage.index, terminal);
    }

    void CompletePrimaryErrorIfNeeded(const std::wstring& error) {
        if (primaryDiagnosticCompleted_.load()) return;
        audio_diagnostics::StageTerminal terminal =
            asr_diagnostics::TerminalFromText(QwenErrorText(error));
        if (error.find(L"timed out") != std::wstring::npos ||
            error.find(L"timeout") != std::wstring::npos) {
            terminal.terminal = "timeout";
        } else if (error.find(L"closed") != std::wstring::npos) {
            terminal.terminal = "peer_close";
        } else if (error.find(L"send failed") != std::wstring::npos ||
                   error.find(L"audio send") != std::wstring::npos) {
            terminal.terminal = "send_error";
        } else if (error.find(L"connect") != std::wstring::npos ||
                   error.find(L"start failed") != std::wstring::npos) {
            terminal.terminal = "connect_error";
        }
        CompleteCurrentStage(terminal);
    }

    void StartReceiver() {
        receiverStop_.store(false);
        receiver_ = std::thread([this]() { ReceiverLoop(); });
    }

    void StopReceiver() {
        receiverStop_.store(true);
        asr_runtime_log::WriteIf(config_.qwenFreeDebugLog, "[qwen_free] stopping receiver");
        // Close() supplies an internal receive wake-up.  Do not call the
        // public RequestCancel() here: that flag is intentionally sticky for
        // Abort(), and setting it while preparing a replay would make the
        // next Connect() fail ResetCancellation() before it can reconnect.
        asr_.Close();
        if (receiver_.joinable() && receiver_.get_id() != std::this_thread::get_id()) {
            receiver_.join();
        }
        std::lock_guard<std::mutex> lock(receivedFramesMutex_);
        receivedFrames_.clear();
        asr_runtime_log::WriteIf(config_.qwenFreeDebugLog, "[qwen_free] receiver stopped");
    }

    bool SleepUntilAbort(DWORD delayMs) const {
        const ULONGLONG deadline = GetTickCount64() + delayMs;
        while (!abort_.load()) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) break;
            Sleep(static_cast<DWORD>((std::min<ULONGLONG>)(20, deadline - now)));
        }
        return !abort_.load();
    }

    bool PrepareProtocol(std::wstring& error) {
        auto utdidResult = qwen_free_proto_utdid::GetUtdid(
            config_.qwenFreeUtdidOverride, config_.qwenFreeShellPath);
        if (!utdidResult.ok) {
            error = L"UTDID acquisition failed: " + utdidResult.error;
            return false;
        }
        utdid_ = utdidResult.utdid;

        std::wstring unetError;
        if (!qwen_free_proto_unet::Initialize(config_.qwenFreeShellPath, unetError)) {
            error = L"Native Qwen signer initialization failed: " + unetError;
            return false;
        }

        asrCfg_ = {};
        asrCfg_.utdid = utdid_;
        asrCfg_.appkey = qwen_free_proto_asr::kDefaultAppkey;
        asrCfg_.host = qwen_free_proto_asr::kDefaultAsrHost;
        asrCfg_.path = qwen_free_proto_asr::kDefaultAsrPath;
        asrCfg_.sampleRate = 16000;
        asrCfg_.channels = 1;

        llmCfg_ = {};
        llmCfg_.utdid = utdid_;
        llmCfg_.appkey = qwen_free_proto_llm::kDefaultAppkey;
        llmCfg_.appname = qwen_free_proto_llm::kDefaultAppname;
        llmCfg_.host = qwen_free_proto_llm::kDefaultLlmHost;
        llmCfg_.path = qwen_free_proto_llm::kDefaultLlmPath;
        llmCfg_.shellPath = config_.qwenFreeShellPath;
        llmCfg_.triggerType = "long_press";
        return true;
    }

    bool ConnectProtocolWithRetry(std::wstring& error) {
        for (int attempt = 0;
             attempt < kQwenFreeMaxConnectAttempts && !abort_.load();
             ++attempt) {
            if (attempt > 0) {
                NotifyStatus(L"Reconnecting... Qwen IME (Free)");
            }

            std::wstring attemptError;
            if (!asr_.Connect(asrCfg_, attemptError)) {
                error = L"ASR connect failed: " +
                        (attemptError.empty() ? L"unknown error" : attemptError);
            } else if (abort_.load()) {
                asr_.Close();
                return false;
            } else if (!asr_.SendStart(attemptError)) {
                error = L"ASR start failed: " +
                        (attemptError.empty() ? L"unknown error" : attemptError);
            } else {
                return true;
            }

            asr_.Close();
            if (abort_.load() || attempt + 1 >= kQwenFreeMaxConnectAttempts ||
                !qwen_free_recovery_policy::ShouldRetryConnect(error)) {
                return false;
            }
            if (!SleepUntilAbort(kQwenFreeReconnectBackoffMs)) return false;
        }
        if (error.empty()) error = L"ASR connection failed";
        return false;
    }

    void WaitForRecordingStop() const {
        while (!abort_.load() && !stopped_.load()) {
            Sleep(kQwenFreePumpSleepMs);
        }
    }

    void RecoverAndFinalize(std::wstring error) {
        CompletePrimaryErrorIfNeeded(error);
        if (!stopped_.load()) {
            NotifyStatus(L"Buffering... Qwen IME (Free)");
        }
        BufferUntilStopped();
        streaming_.store(false);
        if (abort_.load()) return;

        // Authentication/configuration failures cannot be repaired by
        // replaying the same PCM.  Preserve the recording for the main
        // fallback path, but avoid an unnecessary reconnect and its extra
        // latency once the user releases the hotkey.
        if (!qwen_free_recovery_policy::ShouldRetryConnect(error)) {
            asr_runtime_log::WriteIf(
                config_.qwenFreeDebugLog,
                "[qwen_free] skip replay for non-retryable error: %ls",
                error.c_str());
            finalReceived_.store(true);
            EmitFinal(QwenErrorText(error));
            return;
        }

        NotifyStatus(L"Retrying... Qwen IME (Free)");
        std::wstring recoveredText;
        std::wstring retryError = error;
        const ReplayRecognitionOutcome replay =
            TryReplayRecognition(recoveredText, retryError);
        if (replay == ReplayRecognitionOutcome::Transcript) {
            finalReceived_.store(true);
            FinalizeWithLlm(recoveredText);
            return;
        }
        if (abort_.load()) return;

        if (replay == ReplayRecognitionOutcome::EmptyFinal) {
            finalReceived_.store(true);
            EmitFinal(L"");
            return;
        }

        finalReceived_.store(true);
        EmitFinal(QwenErrorText(retryError.empty() ? error : retryError));
    }

    void RetryEmptyFinalOrEmitNoSpeech() {
        if (!stopped_.load()) {
            NotifyStatus(L"Buffering... Qwen IME (Free)");
        }
        BufferUntilStopped();
        streaming_.store(false);
        if (abort_.load()) return;

        bool replayAttempted = false;
        std::wstring retryError;
        if (replayComplete_ && replayPcm_.size() >= kQwenFreeEmptyRetryMinBytes) {
            replayAttempted = true;
            NotifyStatus(L"Retrying... Qwen IME (Free)");
            std::wstring recoveredText;
            const ReplayRecognitionOutcome replay =
                TryReplayRecognition(recoveredText, retryError);
            if (replay == ReplayRecognitionOutcome::Transcript) {
                finalReceived_.store(true);
                FinalizeWithLlm(recoveredText);
                return;
            }
            if (abort_.load()) return;
            if (replay == ReplayRecognitionOutcome::EmptyFinal) {
                finalReceived_.store(true);
                EmitFinal(L"");
                return;
            }
            asr_runtime_log::WriteIf(
                config_.qwenFreeDebugLog,
                "[qwen_free] empty-final replay failed: %ls",
                retryError.empty() ? L"unknown error" : retryError.c_str());
        }

        finalReceived_.store(true);
        if (replayAttempted && !retryError.empty()) {
            // A replay that was actually attempted but failed is a transport
            // error, not proof that the user was silent. Preserve the
            // operational prefix so the main attempt policy can run fallback.
            EmitFinal(QwenErrorText(retryError));
        } else {
            EmitFinal(L"");
        }
    }

    bool HandleTerminalOutcome() {
        if (!finalReceived_.load()) return false;
        if (terminalWasFinal_ && lastFrameError_.empty()) {
            // A provider final with an empty transcript is not a usable
            // result, even if an older partial is still cached.  Otherwise a
            // clean empty final could silently terminate the worker without
            // dispatching anything.  Give recordings large enough to contain
            // speech one bounded replay, matching the other streaming Qwen
            // clients; short/silent recordings are emitted as an empty final.
            if (finalDispatched_.load()) {
                streaming_.store(false);
            } else {
                RetryEmptyFinalOrEmitNoSpeech();
            }
            return true;
        }

        RecoverAndFinalize(lastFrameError_.empty()
                               ? L"WebSocket closed before final transcript"
                               : lastFrameError_);
        return true;
    }

    void ReceiverLoop() {
        while (!abort_.load() && !receiverStop_.load()) {
            auto frame = asr_.RecvFrame(kQwenFreeRecvTimeoutMs);
            if (abort_.load() || receiverStop_.load()) return;
            if (frame.type == qwen_free_proto_asr::FrameType::Timeout) continue;
            const auto type = frame.type;

            {
                std::lock_guard<std::mutex> lock(receivedFramesMutex_);
                if (receivedFrames_.size() >= 128) receivedFrames_.pop_front();
                receivedFrames_.push_back(std::move(frame));
            }

            // A `Final` frame can be a server-side VAD segment final while
            // the user is still holding the hotkey.  Keep receiving until
            // the worker explicitly stops the input; only error/close frames
            // terminate the receiver unconditionally.
            if (type == qwen_free_proto_asr::FrameType::Error ||
                type == qwen_free_proto_asr::FrameType::Closed) {
                return;
            }
        }
    }

    void WorkerLoop() {
        const ULONGLONG totalStartTick = GetTickCount64();
        totalStartTick_ = totalStartTick;
        auto finish = [this]() {
            StopReceiver();
            running_.store(false);
        };

        if (abort_.load()) {
            finish();
            return;
        }

        std::wstring setupError;
        if (!PrepareProtocol(setupError)) {
            WaitForRecordingStop();
            streaming_.store(false);
            if (!abort_.load()) {
                CompletePrimaryErrorIfNeeded(setupError);
                finalReceived_.store(true);
                EmitFinal(QwenErrorText(setupError));
            }
            finish();
            return;
        }
        if (abort_.load()) {
            finish();
            return;
        }

        std::wstring connectError;
        if (!ConnectProtocolWithRetry(connectError)) {
            WaitForRecordingStop();
            streaming_.store(false);
            if (!abort_.load()) {
                CompletePrimaryErrorIfNeeded(connectError);
                finalReceived_.store(true);
                EmitFinal(QwenErrorText(connectError));
            }
            finish();
            return;
        }
        if (abort_.load()) {
            finish();
            return;
        }
        StartReceiver();

        DWORD finalWaitStart = 0;

        // Phase 1: streaming - drain PCM + recv partial frames
        while (!abort_.load() && streaming_.load()) {
            ProcessReceivedFrames(true, !stopped_.load());
            if (HandleTerminalOutcome()) break;
            std::wstring sendError;
            if (!DrainAndSendPcm(&sendError)) {
                if (abort_.load()) break;
                RecoverAndFinalize(L"audio send failed: " + sendError);
                break;
            }
            ProcessReceivedFrames(true, !stopped_.load());
            if (HandleTerminalOutcome()) break;
            if (stopped_.load()) {
                // StopInput runs after the capture callback has stopped. Drain
                // the last queued PCM before sending the protocol stop frame.
                sendError.clear();
                if (!DrainAndSendPcm(&sendError)) {
                    if (abort_.load()) break;
                    RecoverAndFinalize(L"tail audio send failed: " + sendError);
                    break;
                }
                ProcessReceivedFrames(true, false);
                if (HandleTerminalOutcome()) break;
                std::wstring stopError;
                if (!asr_.SendStop(stopError)) {
                    if (abort_.load()) break;
                    RecoverAndFinalize(L"ASR stop failed: " + stopError);
                    break;
                }
                streaming_.store(false);
                break;
            }
            Sleep(kQwenFreePumpSleepMs);
        }

        // Phase 2: stopped - wait for final transcript
        while (!abort_.load() && !finalReceived_.load()) {
            ProcessReceivedFrames(true, true);
            if (HandleTerminalOutcome()) break;
            if (finalWaitStart == 0) {
                finalWaitStart = GetTickCount();
            } else if (GetTickCount() - finalWaitStart >= kQwenFreeFinalWaitMs) {
                RecoverAndFinalize(L"timed out waiting for final transcript");
                break;
            }
            Sleep(kQwenFreePumpSleepMs);
        }

        finish();

        // Abort is cancellation, not an ASR result. Closing a WinHTTP
        // WebSocket wakes Receive with error 12017; never dispatch that
        // shutdown as a final/empty transcript.
    }

    bool DrainAndSendPcm(std::wstring* errorOut = nullptr) {
        // PendingPcmBuffer has a hard cap so a stalled WebSocket cannot grow
        // memory without bound.  An append after the cap is rejected and the
        // overflow flag is sticky; surface that condition through the same
        // transport-failure path as a failed SendPcm instead of silently
        // dropping microphone audio.
        if (pendingAudio_.Overflowed()) {
            if (errorOut) *errorOut = L"audio buffer overflow while recording";
            return false;
        }

        std::vector<BYTE> chunk;
        chunk.reserve(kQwenFreePcmBufferTarget);
        // SendPcm performs the protocol-level 0xf00 accumulation. This layer
        // only drains the thread-safe capture queue.
        while (pendingAudio_.DrainTo(chunk, kQwenFreePcmChunkBytes)) {
            if (chunk.empty()) continue;
            ProcessReceivedFrames(true, !stopped_.load());
            if (finalReceived_.load()) return true;
            std::wstring err;
            AppendReplayPcm(chunk.data(), chunk.size());
            if (!asr_.SendPcm(chunk.data(), chunk.size(), err)) {
                asr_runtime_log::WriteIf(config_.qwenFreeDebugLog,
                                         "[qwen_free] SendPcm failed: %ls", err.c_str());
                if (errorOut) *errorOut = std::move(err);
                return false;
            }
            audio_diagnostics::AppendStageInput(
                config_.asrAttemptId, CurrentStage(),
                chunk.data(), chunk.size(), chunk.size());
            chunk.clear();

            if (pendingAudio_.Overflowed()) {
                if (errorOut) *errorOut = L"audio buffer overflow while recording";
                return false;
            }
        }
        if (pendingAudio_.Overflowed()) {
            if (errorOut) *errorOut = L"audio buffer overflow while recording";
            return false;
        }
        return true;
    }

    void BufferPendingAudioForReplay() {
        std::vector<BYTE> pending;
        pendingAudio_.SwapTo(pending);
        if (!pending.empty()) AppendReplayPcm(pending.data(), pending.size());
        if (pendingAudio_.Overflowed()) {
            replayComplete_ = false;
        }
    }

    void BufferUntilStopped() {
        while (!abort_.load() && !stopped_.load()) {
            BufferPendingAudioForReplay();
            Sleep(kQwenFreePumpSleepMs);
        }
        BufferPendingAudioForReplay();
    }

    void AppendReplayPcm(const BYTE* data, size_t bytes) {
        if (!data || bytes == 0 || !replayComplete_) return;
        if (bytes > kQwenFreeMaxReplayBytes - replayPcm_.size()) {
            replayComplete_ = false;
            asr_runtime_log::WriteIf(config_.qwenFreeDebugLog,
                "[qwen_free] replay buffer overflow: current=%zu append=%zu max=%zu",
                replayPcm_.size(), bytes, kQwenFreeMaxReplayBytes);
            return;
        }
        replayPcm_.insert(replayPcm_.end(), data, data + bytes);
    }

    enum class ReplayRecognitionOutcome {
        Transcript,
        EmptyFinal,
        Failed,
    };

    ReplayRecognitionOutcome TryReplayRecognition(std::wstring& text,
                                                   std::wstring& error) {
        text.clear();
        if (abort_.load()) return ReplayRecognitionOutcome::Failed;
        if (!replayComplete_ || replayPcm_.empty()) {
            error = error.empty() ? L"replay audio unavailable" : error;
            return ReplayRecognitionOutcome::Failed;
        }

        replayStageIndex_ = nextRetryStageIndex_++;
        replayInProgress_ = true;
        const ULONGLONG replayStarted = GetTickCount64();
        auto finishReplay = [this]() {
            replayInProgress_ = false;
            replayStageIndex_ = 0;
        };

        const std::wstring originalPartial = lastPartial_;
        lastPartial_.clear();
        lastFrameError_.clear();
        terminalWasFinal_ = false;
        terminalTextEmpty_ = false;
        finalReceived_.store(false);
        StopReceiver();

        std::wstring retryError;
        if (!ConnectProtocolWithRetry(retryError)) {
            lastPartial_ = originalPartial;
            error = retryError.empty() ? error : retryError;
            audio_diagnostics::StageTerminal terminal =
                asr_diagnostics::TerminalFromText(QwenErrorText(error));
            terminal.terminal = "connect_error";
            terminal.elapsedMs = static_cast<double>(GetTickCount64() - replayStarted);
            CompleteCurrentStage(terminal);
            finishReplay();
            return ReplayRecognitionOutcome::Failed;
        }
        StartReceiver();

        bool sendOk = true;
        size_t offset = 0;
        for (; offset < replayPcm_.size() && !abort_.load();) {
            const size_t bytes = (std::min)(
                kQwenFreePcmChunkBytes, replayPcm_.size() - offset);
            if (!asr_.SendPcm(replayPcm_.data() + offset, bytes, retryError)) {
                sendOk = false;
                break;
            }
            audio_diagnostics::AppendStageInput(
                config_.asrAttemptId,
                CurrentStage(L"replay"),
                replayPcm_.data() + offset, bytes, bytes);
            offset += bytes;
            // Let the server update partial/final state while replaying.
            // During replay a provider may emit a segment final before all
            // PCM has been resent.  It must not become the replay terminal.
            ProcessReceivedFrames(false, false);
            if (finalReceived_.load()) break;
        }
        const bool allAudioSent = offset >= replayPcm_.size();

        if (sendOk && allAudioSent && !abort_.load() && !finalReceived_.load()) {
            if (!asr_.SendStop(retryError)) {
                sendOk = false;
            }
        }

        const ULONGLONG deadline = GetTickCount64() + kQwenFreeFinalWaitMs;
        while (sendOk && allAudioSent && !abort_.load() && !finalReceived_.load() &&
               GetTickCount64() < deadline) {
            ProcessReceivedFrames(false, true);
            Sleep(kQwenFreePumpSleepMs);
        }

        if (qwen_free_recovery_policy::ReplayOutcomeIsUsable(
                sendOk, allAudioSent, finalReceived_.load(), terminalWasFinal_,
                !lastFrameError_.empty(), !terminalTextEmpty_)) {
            text = lastPartial_;
            asr_runtime_log::WriteIf(config_.qwenFreeDebugLog,
                "[qwen_free] replay succeeded: pcm_bytes=%zu text_wlen=%zu",
                replayPcm_.size(), text.size());
            StopReceiver();
            audio_diagnostics::StageTerminal terminal;
            terminal.terminal = "provider_final";
            terminal.textChars = text.size();
            terminal.elapsedMs = static_cast<double>(GetTickCount64() - replayStarted);
            CompleteCurrentStage(terminal);
            finishReplay();
            return ReplayRecognitionOutcome::Transcript;
        }

        const bool cleanEmptyFinal =
            sendOk && allAudioSent && finalReceived_.load() &&
            terminalWasFinal_ && lastFrameError_.empty() &&
            terminalTextEmpty_;
        if (cleanEmptyFinal) {
            StopReceiver();
            lastPartial_ = originalPartial;
            audio_diagnostics::StageTerminal terminal;
            terminal.terminal = "provider_final_empty";
            terminal.reason = "no_speech";
            terminal.elapsedMs = static_cast<double>(GetTickCount64() - replayStarted);
            CompleteCurrentStage(terminal);
            finishReplay();
            return ReplayRecognitionOutcome::EmptyFinal;
        }

        if (!lastFrameError_.empty()) retryError = lastFrameError_;
        if (retryError.empty() && !abort_.load()) {
            retryError = !allAudioSent
                ? L"replay ended before all audio was sent"
                : (sendOk ? L"timed out waiting for replay final transcript"
                          : L"replay audio send failed");
        }
        StopReceiver();
        lastPartial_ = originalPartial;
        if (!retryError.empty()) error = retryError;
        audio_diagnostics::StageTerminal terminal =
            asr_diagnostics::TerminalFromText(QwenErrorText(error));
        if (!allAudioSent || !sendOk) {
            terminal.terminal = "send_error";
        } else if (error.find(L"timed out") != std::wstring::npos ||
                   error.find(L"timeout") != std::wstring::npos) {
            terminal.terminal = "timeout";
        } else if (error.find(L"closed") != std::wstring::npos) {
            terminal.terminal = "peer_close";
        }
        terminal.elapsedMs = static_cast<double>(GetTickCount64() - replayStarted);
        CompleteCurrentStage(terminal);
        finishReplay();
        return ReplayRecognitionOutcome::Failed;
    }

    void ProcessReceivedFrames(bool dispatchFinal = true,
                               bool allowFinal = true) {
        for (int i = 0; i < 4 && !abort_.load(); ++i) {
            qwen_free_proto_asr::AsrFrame frame;
            {
                std::lock_guard<std::mutex> lock(receivedFramesMutex_);
                if (receivedFrames_.empty()) return;
                frame = std::move(receivedFrames_.front());
                receivedFrames_.pop_front();
            }
            asr_runtime_log::WriteIf(config_.qwenFreeDebugLog,
                "[qwen_free] process frame type=%d text_wlen=%zu error_wlen=%zu dispatch=%d",
                static_cast<int>(frame.type), frame.text.size(),
                frame.errorMsg.size(), dispatchFinal ? 1 : 0);
            switch (frame.type) {
                case qwen_free_proto_asr::FrameType::Started:
                    // session 已开始，无需处理。
                    break;
                case qwen_free_proto_asr::FrameType::Partial:
                    if (!frame.text.empty()) {
                        lastPartial_ = frame.text;
                        if (config_.enablePartial) {
                            NotifyPartial(frame.text, false);
                        }
                    }
                    break;
                case qwen_free_proto_asr::FrameType::Final:
                    // Do not turn an early server-side VAD segment final into
                    // the recording final.  We keep its text as the latest
                    // partial and continue draining microphone PCM until
                    // StopInput() has marked the recording stopped.
                    if (!allowFinal || !stopped_.load()) {
                        if (!frame.text.empty()) {
                            lastPartial_ = frame.text;
                            if (config_.enablePartial && !stopped_.load()) {
                                NotifyPartial(frame.text, false);
                            }
                        }
                        continue;
                    }
                    terminalWasFinal_ = true;
                    terminalTextEmpty_ = frame.text.empty();
                    lastFrameError_.clear();
                    finalReceived_.store(true);
                    if (!frame.text.empty()) lastPartial_ = frame.text;
                    {
                        audio_diagnostics::StageTerminal terminal;
                        terminal.terminal = frame.text.empty()
                            ? "provider_final_empty" : "provider_final";
                        terminal.reason = frame.text.empty() ? "no_speech" : "";
                        terminal.textChars = frame.text.size();
                        terminal.elapsedMs = totalStartTick_ == 0 ? 0.0 :
                            static_cast<double>(GetTickCount64() - totalStartTick_);
                        CompleteCurrentStage(terminal);
                    }
                    if (dispatchFinal && !lastPartial_.empty()) {
                        FinalizeWithLlm(lastPartial_);
                    }
                    return;
                case qwen_free_proto_asr::FrameType::Error:
                    terminalWasFinal_ = false;
                    finalReceived_.store(true);
                    {
                        std::wstring detail = frame.errorMsg;
                        if (!frame.errorCode.empty()) {
                            if (detail.empty()) {
                                detail = L"server error";
                            }
                            detail += L" (code=" + frame.errorCode + L")";
                        }
                        lastFrameError_ = QwenErrorText(
                            detail.empty() ? L"server error" : detail);
                    }
                    {
                        audio_diagnostics::StageTerminal terminal =
                            asr_diagnostics::TerminalFromText(lastFrameError_);
                        terminal.terminal = "provider_error";
                        terminal.providerCode = WideToUtf8(frame.errorCode);
                        CompleteCurrentStage(terminal);
                    }
                    return;
                case qwen_free_proto_asr::FrameType::Closed:
                    terminalWasFinal_ = false;
                    finalReceived_.store(true);
                    lastFrameError_ = QwenErrorText(
                        L"WebSocket closed before final transcript");
                    {
                        audio_diagnostics::StageTerminal terminal =
                            asr_diagnostics::TerminalFromText(lastFrameError_);
                        terminal.terminal = "peer_close";
                        CompleteCurrentStage(terminal);
                    }
                    return;
                case qwen_free_proto_asr::FrameType::Timeout:
                    return;  // 无数据，退出 recv 循环
            }
        }
    }

    void FinalizeWithLlm(const std::wstring& asrText) {
        if (abort_.load()) return;
        // EmitFinal owns the one-shot dispatch guard.  Do not set
        // finalDispatched_ here: every path below calls EmitFinal(), and
        // setting it first would make EmitFinal discard the actual result.
        if (finalDispatched_.load()) return;
        asr_runtime_log::WriteIf(config_.qwenFreeDebugLog,
                                 "[qwen_free] finalize text_wlen=%zu", asrText.size());

        // ASR final 文本为空时跳过 LLM。
        if (asrText.empty()) {
            EmitFinal(L"");
            return;
        }

        // 选区改写使用语音识别出的文本作为改写指令，选区内容作为
        // VoiceInputRewrite 的上下文。失败时不能把“改写指令”误粘贴到
        // 原选区，因此这里只在拿到真正的改写输出后才 EmitFinal。
        if (config_.qwenFreeRewriteEnabled && selection_.HasCapturedSelection()) {
            if (lastRawAsrText_) *lastRawAsrText_ = asrText;
            if (!selection_.Usable()) {
                EmitFinal(L"Qwen IME rewrite failed: selection target unavailable");
                return;
            }
            qwen_free_proto_llm::LlmConfig rewriteCfg = llmCfg_;
            rewriteCfg.recordingMs = recordingMs_.load();
            rewriteCfg.capturedPcmBytes = capturedPcmBytes_.load();
            auto rewrite = qwen_free_proto_llm::RewriteSelection(
                rewriteCfg, selection_.selectedText, asrText);
            g_llmMs = static_cast<double>(rewrite.elapsedMs);
            if (abort_.load()) return;
            if (rewrite.ok && !rewrite.polishedText.empty()) {
                EmitFinal(rewrite.polishedText, true);
                return;
            }
            asr_runtime_log::WriteIf(
                config_.qwenFreeDebugLog,
                "[qwen_free] selection rewrite failed; suppressing instruction paste");
            EmitFinal(L"Qwen IME rewrite failed: " +
                      (rewrite.error.empty() ? L"empty output" : rewrite.error));
            return;
        }

        // 调用 LLM 做润色/标点/纠错。
        if (lastRawAsrText_) *lastRawAsrText_ = asrText;

        // 用户禁用润色时直接用 ASR 原文。
        if (!qwen_free_postprocess::Enabled({
                config_.qwenFreePolishEnabled,
                config_.qwenFreePunctEnabled,
                config_.qwenFreeCorrectEnabled,
            })) {
            EmitFinal(asrText);
            return;
        }

        auto llmR = qwen_free_proto_llm::PolishText(
            llmCfg_, asrText,
            recordingMs_.load(),
            capturedPcmBytes_.load());
        g_llmMs = static_cast<double>(llmR.elapsedMs);

        if (abort_.load()) return;

        if (llmR.ok && !llmR.polishedText.empty()) {
            EmitFinal(llmR.polishedText, true);
        } else {
            // LLM 失败降级：用 ASR 原文。
            EmitFinal(asrText);
        }
    }

    void EmitFinal(const std::wstring& text,
                   bool bundledPostProcessApplied = false) {
        // Cancellation is deliberately checked both before and immediately
        // after claiming the dispatch slot.  The second check closes the
        // common Abort-vs-final race; the main attempt generation check is
        // the final stale-message barrier for a callback already posted.
        if (abort_.load(std::memory_order_acquire)) return;
        if (totalStartTick_ != 0) {
            g_cloudApiMs = (std::max)(
                0.0,
                static_cast<double>(GetTickCount64() - totalStartTick_) -
                    recordingMs_.load());
        }
        if (finalDispatched_.exchange(true)) return;
        if (abort_.load(std::memory_order_acquire)) return;
        asr_runtime_log::WriteIf(config_.qwenFreeDebugLog,
                                 "[qwen_free] emit final text_wlen=%zu", text.size());
        DispatchFinal(text, bundledPostProcessApplied);
    }

    qwen_free_proto_asr::QwenFreeProtoAsrSession asr_;
    qwen_free_proto_asr::AsrConfig asrCfg_;
    qwen_free_proto_llm::LlmConfig llmCfg_;
    std::string utdid_;
    PendingPcmBuffer pendingAudio_{120u * 32000u};

    std::thread worker_;
    std::thread receiver_;
    std::mutex lifecycleMutex_;
    std::mutex receivedFramesMutex_;
    std::deque<qwen_free_proto_asr::AsrFrame> receivedFrames_;
    std::atomic<bool> receiverStop_{false};
    std::atomic<bool> streaming_{false};
    std::atomic<bool> stopped_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> finalReceived_{false};
    std::atomic<bool> finalDispatched_{false};
    std::atomic<double> recordingMs_{0.0};
    std::atomic<size_t> capturedPcmBytes_{0};
    std::wstring lastPartial_;
    std::wstring lastFrameError_;
    bool terminalWasFinal_ = false;
    bool terminalTextEmpty_ = false;
    std::vector<BYTE> replayPcm_;
    bool replayComplete_ = true;
    unsigned nextRetryStageIndex_ = 1;
    bool replayInProgress_ = false;
    unsigned replayStageIndex_ = 0;
    std::atomic<bool> primaryDiagnosticCompleted_{false};
    SelectionContext selection_;
    ULONGLONG totalStartTick_ = 0;
};

} // namespace

std::unique_ptr<IStreamingAsrSession> CreateQwenFreeStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText,
    const SelectionContext& selection) {
    return std::make_unique<QwenFreeStreamingSession>(
        config, targetWindow, refineFn, lastRawAsrText, selection);
}
