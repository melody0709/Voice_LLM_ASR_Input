#include "volcengine_streaming_session.h"

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

namespace {

constexpr DWORD kVolcRecordingWatchdogMs = 18000;
constexpr DWORD kVolcRetryChunkBytes = 6400;
constexpr size_t kVolcMaxReplayBytes = 120u * 32000u;
constexpr size_t kVolcShortNoTextRetrySkipBytes = 3u * 32000u;
constexpr DWORD kKeepaliveMs = 3500;

std::deque<std::wstring> g_volcRecognitionHistory;
std::mutex g_volcRecognitionHistoryMutex;

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

// Atomically take ownership of g_volcSession.hWebSocket, setting it to nullptr.
// Only the caller that gets a non-null return value may close the handle.
// This prevents double-close when Abort() and the worker/drain threads race.
static HINTERNET AtomicTakeWebSocket() {
    return static_cast<HINTERNET>(
        InterlockedExchangePointer(
            reinterpret_cast<void* volatile*>(&g_volcSession.hWebSocket),
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
    }

    void Abort() override {
        abort_.store(true);
        streaming_.store(false);
        g_volcSession.forceAbort = true;
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
        return ComputeCloudAsrFinalizeTimeoutMs(recordingMs_.load(), capturedPcmBytes_.load());
    }

    const wchar_t* ProviderName() const override {
        return L"Volcano Engine";
    }

private:
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

        bool asyncMode = (vcfg.mode == L"bigmodel_async");
        bool nostreamMode = (vcfg.mode == L"bigmodel_nostream");

        volc_asr::VolcSession retrySess;
        bool sessionOpened = volc_asr::OpenSession(retrySess, vcfg);
        const int retryDelays[] = {500, 1000};
        for (int i = 0; !sessionOpened && i < 2; i++) {
            VolcDebugLog("Volc retry: OpenSession attempt %d failed, retrying in %dms...", i + 1, retryDelays[i]);
            Sleep(retryDelays[i]);
            volc_asr::RebuildConnection(retrySess);
            sessionOpened = volc_asr::OpenSession(retrySess, vcfg);
        }
        if (!sessionOpened) {
            VolcDebugLog("Volc retry: OpenSession failed after 3 attempts");
            CloseVolcSessionHandles(retrySess);
            result.transportError = true;
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
                return result;
            }
            offset += take;
        }

        std::vector<BYTE> empty;
        volc_asr::SendAudio(retrySess, empty, true, asyncMode, nostreamMode);
        if (!retrySess.hWebSocket || retrySess.forceAbort.load()) {
            VolcDebugLog("Volc retry: final packet failed");
            CloseVolcSessionHandles(retrySess);
            result.transportError = true;
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
        return result;
    }

    void WorkerLoop() {
        const ULONGLONG tTotal0 = GetTickCount64();
        auto markStopped = [this]() {
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
        bool sessionOpened = volc_asr::OpenSession(g_volcSession, vcfg);
        int openAttempts = 0;
        const int retryDelays[] = {500, 1000, 2000, 3000};
        while (!sessionOpened && !g_volcSession.forceAbort.load()) {
            if (!g_volcSession.lastError.empty()) break;
            if (!streaming_.load() && openAttempts >= 3) break;
            const int delayMs = retryDelays[(std::min)(openAttempts, 3)];
            NotifyStatus(L"Reconnecting... Volcano Engine");
            VolcDebugLog("Volc thread: attempt %d failed, retrying in %dms...",
                         openAttempts + 1, delayMs);
            Sleep(delayMs);
            volc_asr::RebuildConnection(g_volcSession);
            sessionOpened = volc_asr::OpenSession(g_volcSession, vcfg);
            openAttempts++;
        }
        if (!sessionOpened) {
            g_volcSession.connected = false;
            std::wstring errMsg = L"VolcEngine connect failed";
            if (!g_volcSession.lastError.empty()) {
                errMsg = g_volcSession.lastError;
            }
            if (!abort_.load()) {
                DispatchFinal(errMsg);
            }
            markStopped();
            return;
        }
        if (openAttempts > 0) {
            VolcDebugLog("Volc thread: OpenSession recovered after %d retries", openAttempts);
        }
        g_volcSession.connected = true;
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
            return volc_asr::SendAudio(g_volcSession, c, false, asyncMode, nostreamMode);
        };

        auto bufferUntilStop = [&]() {
            VolcDebugLog("Volc thread: connection lost while recording, buffering until stop");
            NotifyStatus(L"Reconnecting... Volcano Engine");
            size_t bufferedBytes = 0;
            while (streaming_.load() && !g_volcSession.forceAbort.load()) {
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
                while (!asyncDrainDone && g_volcSession.hWebSocket && !g_volcSession.forceAbort.load() && g_volcSession.connected) {
                    volc_asr::VolcResult vr = volc_asr::ReceiveResult(g_volcSession.hWebSocket, 200, &g_volcSession);
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
                if (g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()) {
                    ULONGLONG drainStart = GetTickCount64();
                    while (g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()
                           && (GetTickCount64() - drainStart < 5000)) {
                        volc_asr::VolcResult vr = volc_asr::ReceiveResult(g_volcSession.hWebSocket, 1000, &g_volcSession);
                        if (!vr.text.empty()) {
                            {
                                std::lock_guard<std::mutex> lock(asyncMutex);
                                asyncPartial = vr.text;
                            }
                            VolcDebugLog("drainThread: final drain got text (%u chars, %llums)",
                                         (unsigned)vr.text.size(), GetTickCount64() - drainStart);
                            break;
                        }
                        if (!g_volcSession.connected) break;
                    }
                }
                while (g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()
                       && g_volcSession.connected.load()) {
                    std::wstring partial = volc_asr::DrainReceiveBuffer(g_volcSession.hWebSocket, &g_volcSession);
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
            if (g_volcSession.forceAbort.load()) break;
            bool hasData = pendingAudio_.DrainTo(chunk, kVolcRetryChunkBytes);
            bool isStreaming = streaming_.load();

            if (hasData && chunk.size() >= kVolcRetryChunkBytes) {
                std::wstring partial = sendChunk(chunk, true);
                chunk.clear();
                lastSendTick = GetTickCount64();
                if (!g_volcSession.hWebSocket || !g_volcSession.connected.load()) {
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
                if (idleMs >= kKeepaliveMs && g_volcSession.connected && g_volcSession.hWebSocket) {
                    std::vector<BYTE> keepalive(kVolcRetryChunkBytes, 0);
                    sendChunk(keepalive, false);
                    lastSendTick = GetTickCount64();
                    VolcDebugLog("Volc thread: keepalive sent (%ums idle)", idleMs);
                }
                Sleep(20);
            }
        }

        if (g_volcSession.forceAbort.load()) {
            VolcDebugLog("Volc thread: forceAbort detected, skipping drain");
        }

        if (!g_volcSession.forceAbort.load() && !chunk.empty()) {
            std::wstring partial = sendChunk(chunk, true);
            if (!asyncMode && !partial.empty()) lastPartial = partial;
        }

        if (!g_volcSession.forceAbort.load()) {
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
        int chunksSent = g_volcSession.sequence - 2;
        VolcDebugLog("Volc thread: send loop ended, chunks_sent=%d, vad_voice=%d",
                     chunksSent, vadDetectedSpeech ? 1 : 0);

        // VAD no-speech check
        if (vadTrimActive && !vadDetectedSpeech) {
            VolcDebugLog("Volc thread: no speech detected, forcing drainThread exit...");
            asyncDrainDone = true;
            g_volcSession.forceAbort = true;
            {
                HINTERNET ws = AtomicTakeWebSocket();
                if (ws) WinHttpCloseHandle(ws);
            }
            if (drainThread.joinable()) drainThread.join();
            if (volc_asr::g_volcKeepAlive) {
                g_volcSession.lastUsedTick = GetTickCount64();
            } else {
                if (g_volcSession.hConnect) {
                    WinHttpCloseHandle(g_volcSession.hConnect);
                    g_volcSession.hConnect = nullptr;
                }
                if (g_volcSession.hSession) {
                    WinHttpCloseHandle(g_volcSession.hSession);
                    g_volcSession.hSession = nullptr;
                }
            }
            g_volcSession.connected = false;
            if (!abort_.load()) {
                DispatchFinal(L"No speech detected");
            }
            VolcDebugLog("=== TOTAL session: %llums (no speech) ===", GetTickCount64() - tTotal0);
            markStopped();
            return;
        }

        // Send last packet
        if (!g_volcSession.forceAbort.load()) {
            std::vector<BYTE> empty;
            std::wstring lastResult = volc_asr::SendAudio(g_volcSession, empty, true, asyncMode, nostreamMode);
            if (!lastResult.empty()) lastPartial = lastResult;
        }

        // Drain final results
        bool originalServerClosed = false;
        if (asyncMode || nostreamMode) {
            asyncDrainDone = true;
            if (!drainFinalDone && !g_volcSession.forceAbort.load()) {
                VolcDebugLog("Volc thread: waiting for drainThread final drain...");
                ULONGLONG waitStart = GetTickCount64();
                while (!drainFinalDone
                       && !g_volcSession.forceAbort.load()
                       && (GetTickCount64() - waitStart < 5000)) {
                    Sleep(100);
                }
                VolcDebugLog("Volc thread: wait done, drainFinalDone=%d, asyncPartial%s empty (%llums)",
                             drainFinalDone.load() ? 1 : 0,
                             [&]() { std::lock_guard<std::mutex> lock(asyncMutex); return asyncPartial.empty(); }() ? "" : " NOT",
                             GetTickCount64() - waitStart);
            }
            originalServerClosed = !g_volcSession.connected;
            g_volcSession.forceAbort = !originalServerClosed;
            {
                HINTERNET ws = AtomicTakeWebSocket();
                if (ws) WinHttpCloseHandle(ws);
            }
            if (drainThread.joinable()) drainThread.join();
            if (volc_asr::g_volcKeepAlive) {
                g_volcSession.lastUsedTick = GetTickCount64();
            } else {
                if (g_volcSession.hConnect) {
                    WinHttpCloseHandle(g_volcSession.hConnect);
                    g_volcSession.hConnect = nullptr;
                }
                if (g_volcSession.hSession) {
                    WinHttpCloseHandle(g_volcSession.hSession);
                    g_volcSession.hSession = nullptr;
                }
            }
            g_volcSession.connected = false;
        }

        // Get final text
        std::wstring finalText;
        if (asyncMode || nostreamMode) {
            finalText = asyncPartial;
            if (finalText.empty()) finalText = lastPartial;
        } else {
            finalText = volc_asr::CloseSession(g_volcSession);
            if (finalText.empty()) finalText = lastPartial;
        }

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
                         replayBuffer.Size(), g_volcSession.forceAbort.load() ? 1 : 0);
            NotifyStatus(L"Retrying... Volcano Engine");
            DWORD retryTimeout = ComputeCloudAsrFinalizeTimeoutMs(recordingMs_.load(), replayBuffer.Size());
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
        if (g_volcSession.forceAbort.load() && finalText == L"No speech detected" && !retryClosedWithoutText) {
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
    std::thread worker_;
    std::atomic<double> recordingMs_{0.0};
    std::atomic<size_t> capturedPcmBytes_{0};

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
