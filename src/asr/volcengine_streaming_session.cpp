#include "volcengine_streaming_session.h"

#include "asr_diagnostics.h"
#include "asr_result.h"
#include "asr_streaming_session_base.h"
#include "cloud_asr_common.h"
#include "engine.h"
#include "globals.h"
#include "input_context.h"
#include "pending_pcm_buffer.h"
#include "streaming_vad_trimmer.h"
#include "volcengine_asr.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

// Define g_volcKeepAlive before the anonymous namespace to avoid shadowing
// the volc_asr namespace from volcengine_asr.h.
namespace volc_asr { std::atomic<bool> g_volcKeepAlive{false}; }

static volc_asr::VolcSession s_volcSession;

namespace {

constexpr DWORD kVolcRecordingWatchdogMs = 18000;
constexpr DWORD kVolcOpeningFinalizeWatchdogMs = 8000;
constexpr DWORD kVolcRetryChunkBytes = 6400;
constexpr size_t kVolcMaxReplayBytes = 120u * 32000u;
constexpr size_t kVolcShortNoTextRetrySkipBytes = 3u * 32000u;
constexpr DWORD kKeepaliveMs = 3500;
constexpr DWORD kVolcOpenHardTimeouts[] = {3000, 3000, 5000, 6000};

std::deque<std::wstring> g_volcRecognitionHistory;
std::mutex g_volcRecognitionHistoryMutex;

DWORD VolcOpenHardTimeoutForAttempt(int attemptIndex) {
    if (attemptIndex < 0) attemptIndex = 0;
    const size_t count = sizeof(kVolcOpenHardTimeouts) / sizeof(kVolcOpenHardTimeouts[0]);
    const size_t index = (std::min)(static_cast<size_t>(attemptIndex), count - 1);
    return kVolcOpenHardTimeouts[index];
}

DWORD VolcFinalizeWaitMs(const Config& config, double recordingMs, size_t capturedPcmBytes) {
    return IsFallbackAsrEnabled(config)
        ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs, capturedPcmBytes)
        : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs, capturedPcmBytes);
}

std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'"') out += L"\\\"";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\b') out += L"\\b";
        else if (c == L'\f') out += L"\\f";
        else if (c < 0x20) {
            wchar_t buf[8];
            swprintf_s(buf, L"\\u%04x", (unsigned)c);
            out += buf;
        }
        else out += c;
    }
    return out;
}

void CloseVolcSessionHandles(volc_asr::VolcSession& sess) {
    if (sess.hWebSocket) {
        volc_asr::WebSocketCloseGracefully(sess.hWebSocket, &sess);
        sess.hWebSocket = nullptr;
    }
    if (sess.hConnect) {
        WinHttpCloseHandle(sess.hConnect);
        sess.hConnect = nullptr;
    }
    if (sess.hSession) {
        WinHttpCloseHandle(sess.hSession);
        sess.hSession = nullptr;
    }
    sess.connected = false;
}

// Atomically take ownership of s_volcSession.hWebSocket, setting it to nullptr.
// Only the caller that gets a non-null return value may close the handle.
// This prevents double-close when Abort() and the worker/drain threads race.
static HINTERNET AtomicTakeWebSocket() {
    return static_cast<HINTERNET>(
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&s_volcSession.hWebSocket),
            nullptr));
}

struct VolcRetryResult {
    std::wstring text;
    bool closedWithoutText = false;
    bool transportError = false;
};

class VolcengineStreamingSession final : public StreamingAsrSessionBase {
public:
    VolcengineStreamingSession(Config config,
                               HWND targetWindow,
                               AsrLlmRefineFn refineFn,
                               std::wstring* lastRawAsrText)
        : StreamingAsrSessionBase(std::move(config), targetWindow, refineFn, lastRawAsrText) {}

    ~VolcengineStreamingSession() override {
        Abort();
    }

    bool Start(std::wstring& error) override {
        if (running_.load()) {
            error = L"VolcEngine error: session already running";
            return false;
        }

        abort_.store(false);
        streaming_.store(true);
        pendingAudio_.Clear();
        recordingMs_.store(0.0);
        capturedPcmBytes_.store(0);
        openingSession_.store(false);
        openingAttempt_.store(0);
        nextRetryStageIndex_ = 1;
        running_.store(true);
        worker_ = std::thread([this]() { WorkerLoop(); });
        return true;
    }

    bool EnqueuePcmChunk(const BYTE* data, size_t bytes) override {
        if (!data || bytes == 0 || abort_.load() || !streaming_.load()) return false;
        return pendingAudio_.Append(data, bytes);
    }

    void StopInput(double recordingMs, size_t capturedPcmBytes) override {
        recordingMs_.store(recordingMs);
        capturedPcmBytes_.store(capturedPcmBytes);
        streaming_.store(false);
        const DWORD baseFinalizeMs = VolcFinalizeWaitMs(config_, recordingMs, capturedPcmBytes);
        if (openingSession_.load()) {
            VolcDebugLog("Volc watchdog: OpenSession pending at StopInput; base=%ums guarded=%ums attempt=%d pending=%zu pcm=%zu",
                         baseFinalizeMs,
                         (std::max)(baseFinalizeMs, kVolcOpeningFinalizeWatchdogMs),
                         openingAttempt_.load(),
                         pendingAudio_.Size(),
                         capturedPcmBytes);
        }
    }

    void Abort() override {
        abort_.store(true);
        streaming_.store(false);
        s_volcSession.forceAbort = true;
        // Immediately close activeReq to unblock WinHttpSendRequest in OpenSessionImpl.
        // This eliminates the 3-second UI freeze that would otherwise occur while
        // the watchdog timer waits for kHardTimeoutMs before closing hReq.
        HINTERNET req = s_volcSession.activeReq.exchange(nullptr);
        if (req) WinHttpCloseHandle(req);
        // Atomically take and close the WebSocket handle to unblock any
        // pending WinHTTP operations in the worker / drain threads.
        HINTERNET ws = AtomicTakeWebSocket();
        if (ws) WinHttpCloseHandle(ws);
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        running_.store(false);
    }

    bool IsRunning() const override {
        return running_.load();
    }

    DWORD CurrentWatchdogMs() const override {
        if (streaming_.load()) return kVolcRecordingWatchdogMs;
        const DWORD finalizeMs = VolcFinalizeWaitMs(config_, recordingMs_.load(), capturedPcmBytes_.load());
        if (openingSession_.load()) {
            return (std::max)(finalizeMs, kVolcOpeningFinalizeWatchdogMs);
        }
        return finalizeMs;
    }

    const wchar_t* ProviderName() const override {
        return L"Volcano Engine";
    }

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

    std::wstring BuildContextJson(const std::wstring& inputFieldText, bool includeHistory) const {
        std::wstring json = L"{\"context_type\":\"dialog_ctx\",\"context_data\":[";
        int idx = 0;

        if (!inputFieldText.empty()) {
            json += L"{\"text\":\"" + JsonEscape(inputFieldText) + L"\"}";
            idx++;
        }

        if (includeHistory) {
            std::lock_guard<std::mutex> lock(g_volcRecognitionHistoryMutex);
            for (size_t i = 0; i < g_volcRecognitionHistory.size(); ++i) {
                if (idx > 0) json += L",";
                json += L"{\"text\":\"" + JsonEscape(g_volcRecognitionHistory[i]) + L"\"}";
                idx++;
            }
        }
        json += L"]}";
        return json;
    }

    void AddRecognitionHistory(const std::wstring& text) {
        if (!IsUsableAsrTextForContext(text)) return;
        std::lock_guard<std::mutex> lock(g_volcRecognitionHistoryMutex);
        g_volcRecognitionHistory.push_back(text);
        int maxHistory = config_.volcContextHistory;
        if (maxHistory < 1) maxHistory = 5;
        if (maxHistory > 20) maxHistory = 20;
        while (static_cast<int>(g_volcRecognitionHistory.size()) > maxHistory) {
            g_volcRecognitionHistory.pop_front();
        }
    }

    VolcRetryResult RetryRecognitionOnce(const volc_asr::VolcConfig& vcfg,
                                          const std::vector<BYTE>& pcm,
                                          DWORD finalTimeoutMs) {
        VolcRetryResult result;
        if (pcm.empty()) {
            result.closedWithoutText = true;
            return result;
        }

        const unsigned diagnosticIndex = nextRetryStageIndex_++;
        const audio_diagnostics::StageMetadata diagnostic = RetryStage(
            diagnosticIndex,
            config_.asrDiagnosticStageKind == audio_diagnostics::StageKind::Fallback
                ? L"fallback_empty_final_replay" : L"empty_final_replay");
        const ULONGLONG attemptStarted = GetTickCount64();
        bool asyncMode = (vcfg.mode == L"bigmodel_async");
        bool nostreamMode = (vcfg.mode == L"bigmodel_nostream");

        volc_asr::VolcSession retrySess;
        DWORD openHardTimeoutMs = VolcOpenHardTimeoutForAttempt(0);
        VolcDebugLog("Volc retry: OpenSession attempt 1 starting (hardTimeout=%ums, replay=%zu)",
                     openHardTimeoutMs, pcm.size());
        bool sessionOpened = volc_asr::OpenSession(retrySess, vcfg, openHardTimeoutMs);
        const int retryDelays[] = {500, 1000};
        for (int i = 0; !sessionOpened && i < 2; i++) {
            openHardTimeoutMs = VolcOpenHardTimeoutForAttempt(i + 1);
            VolcDebugLog("Volc retry: OpenSession attempt %d failed, retrying in %dms (next hardTimeout=%ums)...",
                         i + 1, retryDelays[i], openHardTimeoutMs);
            Sleep(retryDelays[i]);
            volc_asr::RebuildConnection(retrySess);
            VolcDebugLog("Volc retry: OpenSession attempt %d starting (hardTimeout=%ums)",
                         i + 2, openHardTimeoutMs);
            sessionOpened = volc_asr::OpenSession(retrySess, vcfg, openHardTimeoutMs);
        }
        if (!sessionOpened) {
            VolcDebugLog("Volc retry: OpenSession failed after 3 attempts");
            CloseVolcSessionHandles(retrySess);
            result.transportError = true;
            audio_diagnostics::StageTerminal terminal;
            terminal.terminal = "connect_error";
            terminal.reason = "network";
            terminal.elapsedMs = static_cast<double>(GetTickCount64() - attemptStarted);
            audio_diagnostics::CompleteStage(
                config_.asrAttemptId, diagnostic.kind, diagnostic.index, terminal);
            return result;
        }
        retrySess.connected = true;

        for (size_t offset = 0; offset < pcm.size();) {
            const size_t take = (std::min)(static_cast<size_t>(kVolcRetryChunkBytes), pcm.size() - offset);
            std::vector<BYTE> chunk(pcm.begin() + static_cast<ptrdiff_t>(offset),
                                    pcm.begin() + static_cast<ptrdiff_t>(offset + take));
            volc_asr::SendAudio(retrySess, chunk, false, asyncMode, nostreamMode);
            if (!retrySess.hWebSocket || !retrySess.connected.load() || retrySess.forceAbort.load()) {
                VolcDebugLog("Volc retry: send failed at offset=%zu", offset);
                CloseVolcSessionHandles(retrySess);
                result.transportError = true;
                audio_diagnostics::StageTerminal terminal;
                terminal.terminal = "send_error";
                terminal.reason = "network";
                terminal.elapsedMs = static_cast<double>(GetTickCount64() - attemptStarted);
                audio_diagnostics::CompleteStage(
                    config_.asrAttemptId, diagnostic.kind, diagnostic.index, terminal);
                return result;
            }
            audio_diagnostics::AppendStageInput(
                config_.asrAttemptId, diagnostic,
                pcm.data() + offset, take);
            offset += take;
        }

        std::vector<BYTE> empty;
        volc_asr::SendAudio(retrySess, empty, true, asyncMode, nostreamMode);
        if (!retrySess.hWebSocket || retrySess.forceAbort.load()) {
            VolcDebugLog("Volc retry: final packet failed");
            CloseVolcSessionHandles(retrySess);
            result.transportError = true;
            audio_diagnostics::StageTerminal terminal;
            terminal.terminal = "final_send_error";
            terminal.reason = "network";
            terminal.elapsedMs = static_cast<double>(GetTickCount64() - attemptStarted);
            audio_diagnostics::CompleteStage(
                config_.asrAttemptId, diagnostic.kind, diagnostic.index, terminal);
            return result;
        }

        const ULONGLONG drainStart = GetTickCount64();
        while (retrySess.hWebSocket && !retrySess.forceAbort.load()
               && (GetTickCount64() - drainStart < finalTimeoutMs)) {
            volc_asr::VolcResult vr = volc_asr::ReceiveResult(retrySess.hWebSocket, 1000, &retrySess);
            if (!vr.text.empty()) {
                result.text = vr.text;
                VolcDebugLog("Volc retry: got text (%u chars, %llums)",
                             (unsigned)result.text.size(), GetTickCount64() - drainStart);
                break;
            }
            if (!retrySess.connected.load()) break;
        }

        if (result.text.empty()) {
            result.closedWithoutText = !retrySess.connected.load();
            result.transportError = !result.closedWithoutText;
        }

        CloseVolcSessionHandles(retrySess);
        if (!result.text.empty()) {
            VolcDebugLog("Volc retry: succeeded");
        } else if (result.closedWithoutText) {
            VolcDebugLog("Volc retry: closed without text");
        } else {
            VolcDebugLog("Volc retry: failed");
        }
        audio_diagnostics::StageTerminal terminal;
        if (!result.text.empty()) {
            terminal.terminal = "provider_final";
            terminal.textChars = result.text.size();
        } else if (result.closedWithoutText) {
            terminal.terminal = "peer_close_empty";
            terminal.reason = "no_speech";
        } else {
            terminal.terminal = "timeout";
            terminal.reason = "timeout";
        }
        terminal.elapsedMs = static_cast<double>(GetTickCount64() - attemptStarted);
        audio_diagnostics::CompleteStage(
            config_.asrAttemptId, diagnostic.kind, diagnostic.index, terminal);
        return result;
    }

    void WorkerLoop() {
        const ULONGLONG tTotal0 = GetTickCount64();
        auto markStopped = [this]() {
            openingSession_.store(false);
            openingAttempt_.store(0);
            running_.store(false);
        };

        // Build volcengine config
        volc_asr::VolcConfig vcfg;
        vcfg.apiKey = config_.volcApiKey;
        vcfg.resourceId = config_.volcResourceId;
        vcfg.mode = config_.volcMode;
        vcfg.language = config_.volcLanguage;
        vcfg.enableNonstream = config_.volcEnableNonstream;
        vcfg.endWindowSize = config_.volcEndWindowSize;
        vcfg.enableDdc = config_.volcEnableDdc;
        vcfg.enableMusicFc = config_.volcEnableMusicFc;
        vcfg.enablePoiFc = config_.volcEnablePoiFc;
        vcfg.forceToSpeechTime = config_.volcForceToSpeechTime;
        vcfg.extraParams = config_.volcExtraParams;
        vcfg.hotwordsId = config_.volcHotwordsId;
        vcfg.hotwordsName = config_.volcHotwordsName;
        vcfg.correctTableId = config_.volcCorrectTableId;
        vcfg.correctTableName = config_.volcCorrectTableName;
        {
            std::wstring ctxInputText;
            bool hasInputText = false;

            if (config_.volcEnableInputContext) {
                HiResTimer tCtx;
                g_inputContextResult = input_context::GetInputFieldContext();
                g_inputContextResult.elapsedMs = tCtx.ElapsedMs();
                ctxInputText = g_inputContextResult.inputFieldText;
                hasInputText = !ctxInputText.empty();
            }

            if (hasInputText) {
                vcfg.contextJson = BuildContextJson(ctxInputText, false);
            } else if (config_.volcEnableContext) {
                vcfg.contextJson = BuildContextJson(L"", true);
            }
        }

        // Open session with retries
        openingSession_.store(true);
        openingAttempt_.store(1);
        DWORD openHardTimeoutMs = VolcOpenHardTimeoutForAttempt(0);
        VolcDebugLog("Volc thread: OpenSession attempt 1 starting (hardTimeout=%ums, streaming=%d, pending=%zu)",
                     openHardTimeoutMs, streaming_.load() ? 1 : 0, pendingAudio_.Size());
        bool sessionOpened = volc_asr::OpenSession(s_volcSession, vcfg, openHardTimeoutMs);
        int openAttempts = 0;
        const int retryDelays[] = {500, 1000, 2000, 3000};
        while (!sessionOpened && !s_volcSession.forceAbort.load()) {
            if (!s_volcSession.lastError.empty()) break;
            if (!streaming_.load() && openAttempts >= 3) break;
            const int delayMs = retryDelays[(std::min)(openAttempts, 3)];
            const int nextAttempt = openAttempts + 2;
            openHardTimeoutMs = VolcOpenHardTimeoutForAttempt(nextAttempt - 1);
            NotifyStatus(L"Reconnecting... Volcano Engine");
            VolcDebugLog("Volc thread: attempt %d failed, retrying in %dms (nextAttempt=%d hardTimeout=%ums, streaming=%d, pending=%zu)",
                         openAttempts + 1, delayMs, nextAttempt, openHardTimeoutMs,
                         streaming_.load() ? 1 : 0, pendingAudio_.Size());
            Sleep(delayMs);
            volc_asr::RebuildConnection(s_volcSession);
            openingAttempt_.store(nextAttempt);
            VolcDebugLog("Volc thread: OpenSession attempt %d starting (hardTimeout=%ums, streaming=%d, pending=%zu)",
                         nextAttempt, openHardTimeoutMs, streaming_.load() ? 1 : 0, pendingAudio_.Size());
            sessionOpened = volc_asr::OpenSession(s_volcSession, vcfg, openHardTimeoutMs);
            openAttempts++;
        }
        openingSession_.store(false);
        openingAttempt_.store(0);
        if (!sessionOpened) {
            s_volcSession.connected = false;
            std::wstring errMsg = L"VolcEngine connect failed";
            if (!s_volcSession.lastError.empty()) {
                errMsg = s_volcSession.lastError;
            }
            VolcDebugLog("Volc thread: OpenSession failed after %d attempts (forceAbort=%d, streaming=%d, pending=%zu, error_wlen=%zu)",
                         openAttempts + 1,
                         s_volcSession.forceAbort.load() ? 1 : 0,
                         streaming_.load() ? 1 : 0,
                         pendingAudio_.Size(),
                         errMsg.size());
            if (!abort_.load()) {
                audio_diagnostics::StageTerminal terminal =
                    asr_diagnostics::TerminalFromText(errMsg);
                terminal.terminal = "connect_error";
                CompletePrimary(terminal);
                DispatchFinal(errMsg);
            }
            markStopped();
            return;
        }
        if (openAttempts > 0) {
            VolcDebugLog("Volc thread: OpenSession recovered after %d retries", openAttempts);
        }
        VolcDebugLog("Volc thread: OpenSession ready (attempts=%d, stopped=%d, pending=%zu)",
                     openAttempts + 1, streaming_.load() ? 0 : 1, pendingAudio_.Size());
        s_volcSession.connected = true;
        volc_asr::g_volcKeepAlive = true;

        bool asyncMode = (vcfg.mode == L"bigmodel_async");
        bool nostreamMode = (vcfg.mode == L"bigmodel_nostream");
        std::vector<BYTE> chunk;
        chunk.reserve(kVolcRetryChunkBytes);
        CloudAsrReplayBuffer replayBuffer(kVolcMaxReplayBytes);
        bool replayLimitLogged = false;
        ULONGLONG lastSendTick = GetTickCount64();

        auto appendReplay = [&](const std::vector<BYTE>& c) {
            if (c.empty()) return;
            CloudReplayAppendResult appendResult = replayBuffer.Append(c);
            if (appendResult == CloudReplayAppendResult::LimitExceeded) {
                if (!replayLimitLogged) {
                    VolcDebugLog("Volc replay: disabled, buffer limit exceeded (current=%zu, add=%zu, limit=%zu)",
                                 replayBuffer.Size(), c.size(), replayBuffer.MaxBytes());
                    replayLimitLogged = true;
                }
            }
        };

        auto sendChunk = [&](std::vector<BYTE>& c, bool recordReplay) -> std::wstring {
            if (recordReplay && !c.empty()) {
                appendReplay(c);
            }
            std::wstring partial =
                volc_asr::SendAudio(s_volcSession, c, false, asyncMode, nostreamMode);
            if (recordReplay && !c.empty() && s_volcSession.hWebSocket &&
                s_volcSession.connected.load() && !s_volcSession.forceAbort.load()) {
                audio_diagnostics::AppendStageInput(
                    config_.asrAttemptId, PrimaryStage(),
                    c.data(), c.size());
            }
            return partial;
        };

        auto bufferUntilStop = [&]() {
            VolcDebugLog("Volc thread: connection lost while recording, buffering until stop");
            NotifyStatus(L"Buffering... Volcano Engine");
            size_t bufferedBytes = 0;
            while (streaming_.load() && !s_volcSession.forceAbort.load()) {
                std::vector<BYTE> buffered;
                pendingAudio_.SwapTo(buffered);
                bufferedBytes += buffered.size();
                appendReplay(buffered);
                Sleep(20);
            }
            VolcDebugLog("Volc thread: buffered %zu bytes after connection loss (replay=%zu, available=%d)",
                         bufferedBytes, replayBuffer.Size(), replayBuffer.Available() ? 1 : 0);
        };

        std::wstring lastPartial;
        std::wstring asyncPartial;
        std::mutex asyncMutex;  // protects asyncPartial across worker/drainThread
        std::atomic<bool> asyncDrainDone{false};
        std::atomic<bool> drainFinalDone{false};
        std::thread drainThread;

        if (asyncMode || nostreamMode) {
            drainThread = std::thread([&]() {
                VolcDebugLog("drainThread: started (async=%d nostream=%d)", asyncMode ? 1 : 0, nostreamMode ? 1 : 0);
                while (!asyncDrainDone && s_volcSession.hWebSocket && !s_volcSession.forceAbort.load() && s_volcSession.connected) {
                    volc_asr::VolcResult vr = volc_asr::ReceiveResult(s_volcSession.hWebSocket, 200, &s_volcSession);
                    if (!vr.text.empty()) {
                        bool changed = false;
                        {
                            std::lock_guard<std::mutex> lock(asyncMutex);
                            if (vr.text != asyncPartial) {
                                asyncPartial = vr.text;
                                changed = true;
                            }
                        }
                        if (changed && config_.enablePartial) {
                            NotifyPartial(vr.text, false);
                        }
                    }
                }
                VolcDebugLog("drainThread: main loop exited, doing final drain...");
                if (s_volcSession.hWebSocket && !s_volcSession.forceAbort.load()) {
                    ULONGLONG drainStart = GetTickCount64();
                    while (s_volcSession.hWebSocket && !s_volcSession.forceAbort.load()
                           && (GetTickCount64() - drainStart < 5000)) {
                        volc_asr::VolcResult vr = volc_asr::ReceiveResult(s_volcSession.hWebSocket, 1000, &s_volcSession);
                        if (!vr.text.empty()) {
                            {
                                std::lock_guard<std::mutex> lock(asyncMutex);
                                asyncPartial = vr.text;
                            }
                            VolcDebugLog("drainThread: final drain got text (%u chars, %llums)",
                                         (unsigned)vr.text.size(), GetTickCount64() - drainStart);
                            break;
                        }
                        if (!s_volcSession.connected) break;
                    }
                }
                while (s_volcSession.hWebSocket && !s_volcSession.forceAbort.load()
                       && s_volcSession.connected.load()) {
                    std::wstring partial = volc_asr::DrainReceiveBuffer(s_volcSession.hWebSocket, &s_volcSession);
                    if (!partial.empty()) {
                        std::lock_guard<std::mutex> lock(asyncMutex);
                        asyncPartial = partial;
                    } else {
                        break;
                    }
                }
                drainFinalDone = true;
                VolcDebugLog("drainThread: done");
            });
        }

        // Send loop
        while (true) {
            if (s_volcSession.forceAbort.load()) break;
            bool hasData = pendingAudio_.DrainTo(chunk, kVolcRetryChunkBytes);
            bool isStreaming = streaming_.load();

            if (hasData && chunk.size() >= kVolcRetryChunkBytes) {
                std::wstring partial = sendChunk(chunk, true);
                chunk.clear();
                lastSendTick = GetTickCount64();
                if (!s_volcSession.hWebSocket || !s_volcSession.connected.load()) {
                    if (streaming_.load()) bufferUntilStop();
                    break;
                }
                if (!asyncMode && !partial.empty() && partial != lastPartial) {
                    lastPartial = partial;
                    if (config_.enablePartial) {
                        NotifyPartial(partial, false);
                    }
                }
            } else if (!isStreaming) {
                break;
            } else {
                DWORD idleMs = static_cast<DWORD>(GetTickCount64() - lastSendTick);
                if (idleMs >= kKeepaliveMs && s_volcSession.connected && s_volcSession.hWebSocket) {
                    std::vector<BYTE> keepalive(kVolcRetryChunkBytes, 0);
                    sendChunk(keepalive, false);
                    lastSendTick = GetTickCount64();
                    VolcDebugLog("Volc thread: keepalive sent (%ums idle)", idleMs);
                }
                Sleep(20);
            }
        }

        if (s_volcSession.forceAbort.load()) {
            VolcDebugLog("Volc thread: forceAbort detected, skipping drain");
        }

        if (!s_volcSession.forceAbort.load() && !chunk.empty()) {
            std::wstring partial = sendChunk(chunk, true);
            if (!asyncMode && !partial.empty()) lastPartial = partial;
        }

        if (!s_volcSession.forceAbort.load()) {
            std::vector<BYTE> remaining;
            pendingAudio_.SwapTo(remaining);
            if (!remaining.empty()) {
                VolcDebugLog("Volc thread: flushing %zu remaining bytes from pending buffer", remaining.size());
                std::wstring partial = sendChunk(remaining, true);
                if (!asyncMode && !partial.empty()) lastPartial = partial;
            }
        }

        const bool vadTrimActive = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
        const bool vadDetectedSpeech = vadTrimActive && g_streamingVadTrimmer->DetectedSpeech();
        int chunksSent = s_volcSession.sequence - 2;
        VolcDebugLog("Volc thread: send loop ended, chunks_sent=%d, vad_voice=%d",
                     chunksSent, vadDetectedSpeech ? 1 : 0);

        // VAD no-speech check
        if (vadTrimActive && !vadDetectedSpeech) {
            VolcDebugLog("Volc thread: no speech detected, forcing drainThread exit...");
            asyncDrainDone = true;
            s_volcSession.forceAbort = true;
            {
                HINTERNET ws = AtomicTakeWebSocket();
                if (ws) WinHttpCloseHandle(ws);
            }
            if (drainThread.joinable()) drainThread.join();
            if (volc_asr::g_volcKeepAlive) {
                s_volcSession.lastUsedTick = GetTickCount64();
            } else {
                if (s_volcSession.hConnect) {
                    WinHttpCloseHandle(s_volcSession.hConnect);
                    s_volcSession.hConnect = nullptr;
                }
                if (s_volcSession.hSession) {
                    WinHttpCloseHandle(s_volcSession.hSession);
                    s_volcSession.hSession = nullptr;
                }
            }
            s_volcSession.connected = false;
            if (!abort_.load()) {
                audio_diagnostics::StageTerminal terminal;
                terminal.terminal = "local_vad_no_speech";
                terminal.reason = "no_speech";
                CompletePrimary(terminal);
                DispatchFinal(L"No speech detected");
            }
            VolcDebugLog("=== TOTAL session: %llums (no speech) ===", GetTickCount64() - tTotal0);
            markStopped();
            return;
        }

        // Send last packet
        if (!s_volcSession.forceAbort.load()) {
            std::vector<BYTE> empty;
            std::wstring lastResult = volc_asr::SendAudio(s_volcSession, empty, true, asyncMode, nostreamMode);
            if (!lastResult.empty()) lastPartial = lastResult;
        }

        // Drain final results
        bool originalServerClosed = false;
        if (asyncMode || nostreamMode) {
            asyncDrainDone = true;
            if (!drainFinalDone && !s_volcSession.forceAbort.load()) {
                VolcDebugLog("Volc thread: waiting for drainThread final drain...");
                ULONGLONG waitStart = GetTickCount64();
                while (!drainFinalDone
                       && !s_volcSession.forceAbort.load()
                       && (GetTickCount64() - waitStart < 5000)) {
                    Sleep(100);
                }
                VolcDebugLog("Volc thread: wait done, drainFinalDone=%d, asyncPartial%s empty (%llums)",
                             drainFinalDone.load() ? 1 : 0,
                             [&]() { std::lock_guard<std::mutex> lock(asyncMutex); return asyncPartial.empty(); }() ? "" : " NOT",
                             GetTickCount64() - waitStart);
            }
            originalServerClosed = !s_volcSession.connected;
            s_volcSession.forceAbort = !originalServerClosed;
            {
                HINTERNET ws = AtomicTakeWebSocket();
                if (ws) WinHttpCloseHandle(ws);
            }
            if (drainThread.joinable()) drainThread.join();
            if (volc_asr::g_volcKeepAlive) {
                s_volcSession.lastUsedTick = GetTickCount64();
            } else {
                if (s_volcSession.hConnect) {
                    WinHttpCloseHandle(s_volcSession.hConnect);
                    s_volcSession.hConnect = nullptr;
                }
                if (s_volcSession.hSession) {
                    WinHttpCloseHandle(s_volcSession.hSession);
                    s_volcSession.hSession = nullptr;
                }
            }
            s_volcSession.connected = false;
        }

        // Get final text
        std::wstring finalText;
        if (asyncMode || nostreamMode) {
            finalText = asyncPartial;
            if (finalText.empty()) finalText = lastPartial;
        } else {
            finalText = volc_asr::CloseSession(s_volcSession);
            if (finalText.empty()) finalText = lastPartial;
        }

        audio_diagnostics::StageTerminal primaryTerminal;
        if (!finalText.empty()) {
            primaryTerminal.terminal = "provider_final";
            primaryTerminal.textChars = finalText.size();
        } else if (originalServerClosed) {
            primaryTerminal.terminal = "peer_close_empty";
            primaryTerminal.reason = "no_speech";
        } else if (s_volcSession.forceAbort.load()) {
            primaryTerminal.terminal = "transport_error";
            primaryTerminal.reason = "network";
        } else {
            primaryTerminal.terminal = "provider_final_empty";
            primaryTerminal.reason = "no_speech";
        }
        primaryTerminal.elapsedMs = static_cast<double>(GetTickCount64() - tTotal0);
        CompletePrimary(primaryTerminal);

        // Empty final retry
        bool retryAttempted = false;
        bool retryClosedWithoutText = false;
        const bool streamingMode = asyncMode || nostreamMode;
        const bool shortClosedWithoutText = IsShortClosedWithoutText(
            streamingMode,
            finalText.empty(),
            originalServerClosed,
            replayBuffer.Size(),
            kVolcShortNoTextRetrySkipBytes);
        if (!abort_.load() && ShouldRetryEmptyCloudFinal(streamingMode,
                                        finalText.empty(),
                                        replayBuffer.Available(),
                                        replayBuffer.Empty(),
                                        shortClosedWithoutText)) {
            retryAttempted = true;
            VolcDebugLog("Volc retry: final text empty, replay available (bytes=%zu, forceAbort=%d)",
                         replayBuffer.Size(), s_volcSession.forceAbort.load() ? 1 : 0);
            NotifyStatus(L"Retrying... Volcano Engine");
            DWORD retryTimeout = VolcFinalizeWaitMs(config_, recordingMs_.load(), replayBuffer.Size());
            VolcRetryResult retryResult = RetryRecognitionOnce(vcfg, replayBuffer.Data(), retryTimeout);
            if (!retryResult.text.empty()) {
                finalText = retryResult.text;
            } else if (retryResult.closedWithoutText) {
                retryClosedWithoutText = true;
                finalText = L"No speech detected";
            }
        } else if ((asyncMode || nostreamMode) && finalText.empty()) {
            if (shortClosedWithoutText) {
                VolcDebugLog("Volc retry: skipped (server closed without text, short audio %.0fms)",
                             replayBuffer.Size() / kPcm16k16MonoBytesPerMs);
            } else {
                VolcDebugLog("Volc retry: skipped (replayAvailable=%d, replayBytes=%zu)",
                             replayBuffer.Available() ? 1 : 0, replayBuffer.Size());
            }
        }

        if (finalText.empty()) finalText = retryAttempted ? L"ASR failed: VolcEngine timeout" : L"No speech detected";
        if (s_volcSession.forceAbort.load() && finalText == L"No speech detected" && !retryClosedWithoutText) {
            finalText = L"ASR failed: VolcEngine timeout";
        }

        AddRecognitionHistory(finalText);
        VolcDebugLog("=== TOTAL session: %llums ===", GetTickCount64() - tTotal0);
        g_cloudApiMs = static_cast<double>(GetTickCount64() - tTotal0) - recordingMs_.load();
        if (!abort_.load()) {
            DispatchFinal(finalText);
        }
        markStopped();
    }

    PendingPcmBuffer pendingAudio_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> openingSession_{false};
    std::atomic<int> openingAttempt_{0};
    std::thread worker_;
    std::atomic<double> recordingMs_{0.0};
    std::atomic<size_t> capturedPcmBytes_{0};
    unsigned nextRetryStageIndex_ = 1;

};

} // namespace

std::unique_ptr<IStreamingAsrSession> CreateVolcengineStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText) {
    return std::make_unique<VolcengineStreamingSession>(config, targetWindow, refineFn, lastRawAsrText);
}

size_t VolcengineRecognitionHistorySize() {
    std::lock_guard<std::mutex> lock(g_volcRecognitionHistoryMutex);
    return g_volcRecognitionHistory.size();
}

void VolcengineResetForNewSession() {
    s_volcSession.forceAbort = false;
    s_volcSession.lastError.clear();
}

void VolcenginePrewarmConnection() {
    volc_asr::PrewarmConnection(s_volcSession);
}

void VolcengineClosePersistentConnection() {
    volc_asr::ClosePersistentConnection(s_volcSession);
}

void VolcengineForceAbortAndCloseAll() {
    s_volcSession.forceAbort = true;
    // Close activeReq to unblock any pending OpenSessionImpl
    HINTERNET req = s_volcSession.activeReq.exchange(nullptr);
    if (req) WinHttpCloseHandle(req);
    // Close WebSocket
    HINTERNET ws = AtomicTakeWebSocket();
    if (ws) WinHttpCloseHandle(ws);
    // Close hConnect + hSession
    if (s_volcSession.hConnect) {
        WinHttpCloseHandle(s_volcSession.hConnect);
        s_volcSession.hConnect = nullptr;
    }
    if (s_volcSession.hSession) {
        WinHttpCloseHandle(s_volcSession.hSession);
        s_volcSession.hSession = nullptr;
    }
}
