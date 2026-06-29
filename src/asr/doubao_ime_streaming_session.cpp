#include "doubao_ime_streaming_session.h"

#include "asr_result.h"
#include "asr_streaming_session_base.h"
#include "cloud_asr_common.h"
#include "doubao_ime_asr.h"
#include "doubao_ime_config.h"
#include "globals.h"
#include "pending_pcm_buffer.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cwctype>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kDoubaoRecordingWatchdogMs = 18000;
constexpr size_t kDoubaoMaxReplayBytes = 120u * 32000u;
constexpr size_t kDoubaoEmptyRetryMinBytes = 3u * 32000u;

std::wstring DoubaoErrorText(const std::wstring& error) {
    return doubao_ime_asr::ErrorText(error);
}

struct DoubaoRetryResult {
    std::wstring text;
    std::wstring error;
    bool transportError = false;
};

bool IsIgnoredPartialCompareChar(wchar_t c) {
    return iswspace(c) ||
           c == L',' || c == L'.' || c == L';' || c == L':' ||
           c == L'!' || c == L'?' ||
           c == static_cast<wchar_t>(0x3001) ||
           c == static_cast<wchar_t>(0x3002) ||
           c == static_cast<wchar_t>(0xff0c) ||
           c == static_cast<wchar_t>(0xff0e) ||
           c == static_cast<wchar_t>(0xff1b) ||
           c == static_cast<wchar_t>(0xff1a) ||
           c == static_cast<wchar_t>(0xff01) ||
           c == static_cast<wchar_t>(0xff1f);
}

std::wstring NormalizePartialCompareText(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size());
    for (wchar_t c : text) {
        if (IsIgnoredPartialCompareChar(c)) continue;
        out.push_back(static_cast<wchar_t>(towlower(c)));
    }
    return out;
}

bool StartsWithText(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

size_t CommonPrefixChars(const std::wstring& a, const std::wstring& b) {
    const size_t n = (std::min)(a.size(), b.size());
    size_t i = 0;
    while (i < n && a[i] == b[i]) ++i;
    return i;
}

bool LooksLikeSamePartialWindow(const std::wstring& previous, const std::wstring& incoming) {
    const std::wstring a = NormalizePartialCompareText(previous);
    const std::wstring b = NormalizePartialCompareText(incoming);
    if (a.empty() || b.empty()) return false;
    if (StartsWithText(a, b) || StartsWithText(b, a)) return true;
    const size_t common = CommonPrefixChars(a, b);
    const size_t shorter = (std::min)(a.size(), b.size());
    return common >= 4 && common * 2 >= shorter;
}

std::wstring PreferRicherPartialText(const std::wstring& previous, const std::wstring& incoming) {
    const size_t prevLen = NormalizePartialCompareText(previous).size();
    const size_t incomingLen = NormalizePartialCompareText(incoming).size();
    return incomingLen + 2 >= prevLen ? incoming : previous;
}

bool StartsNewPartialWindow(const std::wstring& previous, const std::wstring& incoming) {
    const std::wstring a = NormalizePartialCompareText(previous);
    const std::wstring b = NormalizePartialCompareText(incoming);
    if (a.empty() || b.empty()) return false;
    if (LooksLikeSamePartialWindow(previous, incoming)) return false;

    // Doubao IME may reset its displayed partial window after the text gets
    // very long. Treat only a clear length drop as a new window; otherwise the
    // event is usually a service-side correction of the same window.
    if (a.size() >= 48 && b.size() + 16 < a.size() && b.size() * 4 < a.size() * 3) {
        return true;
    }
    if (a.size() >= 80 && b.size() <= 32) {
        return true;
    }
    return false;
}

class DoubaoImeStreamingSession final : public StreamingAsrSessionBase {
public:
    DoubaoImeStreamingSession(Config config,
                              HWND targetWindow,
                              AsrLlmRefineFn refineFn,
                              std::wstring* lastRawAsrText)
        : StreamingAsrSessionBase(std::move(config), targetWindow, refineFn, lastRawAsrText),
          dcfg_(BuildDoubaoImeConfigFromConfig(config_)) {}

    ~DoubaoImeStreamingSession() override {
        Abort();
    }

    bool Start(std::wstring& error) override {
        if (running_.load()) {
            error = L"Doubao IME ASR error: session already running";
            return false;
        }

        pendingAudio_.Clear();
        abort_.store(false);
        streaming_.store(true);
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
        AbortActiveClient();
        if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
            worker_.join();
        }
        running_.store(false);
    }

    bool IsRunning() const override {
        return running_.load();
    }

    DWORD CurrentWatchdogMs() const override {
        if (streaming_.load()) return kDoubaoRecordingWatchdogMs;
        return IsFallbackAsrEnabled(config_)
            ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs_.load(), capturedPcmBytes_.load())
            : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs_.load(), capturedPcmBytes_.load());
    }

    const wchar_t* ProviderName() const override {
        return L"Doubao IME";
    }

private:
    void SetActiveClient(doubao_ime_asr::RealtimeClient* client) {
        std::lock_guard<std::mutex> lock(activeClientMutex_);
        activeClient_ = client;
    }

    void ClearActiveClient(doubao_ime_asr::RealtimeClient* expected = nullptr) {
        std::lock_guard<std::mutex> lock(activeClientMutex_);
        if (!expected || activeClient_ == expected) {
            activeClient_ = nullptr;
        }
    }

    void AbortActiveClient() {
        std::lock_guard<std::mutex> lock(activeClientMutex_);
        if (activeClient_) {
            activeClient_->Abort();
        }
    }

    void WaitForRecordingStop() {
        while (streaming_.load() && !abort_.load()) {
            Sleep(20);
        }
    }

    bool SleepUntilAbort(DWORD totalMs) const {
        DWORD waited = 0;
        while (waited < totalMs && !abort_.load()) {
            const DWORD slice = (std::min<DWORD>)(20, totalMs - waited);
            Sleep(slice);
            waited += slice;
        }
        return !abort_.load();
    }

    void PostCredentials(const doubao_ime_asr::Credentials& credentials, bool clear) const {
        if (!targetWindow_) return;
        auto* update = new doubao_ime_asr::CredentialsUpdateMessage;
        update->credentials = credentials;
        update->clear = clear;
        if (!PostMessageW(targetWindow_, kDoubaoImeCredentialsMessage, 0,
                          reinterpret_cast<LPARAM>(update))) {
            delete update;
        }
    }

    void SyncCredentialsFromClient(const doubao_ime_asr::RealtimeClient& client) {
        doubao_ime_asr::Credentials credentials = client.CurrentCredentials();
        dcfg_.deviceId = credentials.deviceId;
        dcfg_.cdid = credentials.cdid;
        dcfg_.token = credentials.token;
        if (client.CredentialsChanged()) {
            PostCredentials(credentials, false);
        }
    }

    bool ConnectWithCredentialRetry(std::unique_ptr<doubao_ime_asr::RealtimeClient>& client,
                                    std::wstring& error) {
        constexpr int kMaxAttempts = 3;
        bool refreshedCredentials = false;
        for (int attempt = 0; attempt < kMaxAttempts && !abort_.load(); ++attempt) {
            if (attempt > 0) {
                NotifyStatus(L"Reconnecting... Doubao IME");
            }
            error.clear();
            client = std::make_unique<doubao_ime_asr::RealtimeClient>(dcfg_);
            SetActiveClient(client.get());
            if (client->Connect(error)) {
                if (abort_.load()) {
                    client->Abort();
                    client->Close();
                    ClearActiveClient(client.get());
                    return false;
                }
                SyncCredentialsFromClient(*client);
                return true;
            }
            const bool authFailure = doubao_ime_asr::IsAuthFailure(error);
            if (client->CredentialsChanged() && !authFailure) {
                SyncCredentialsFromClient(*client);
            }
            client->Close();
            ClearActiveClient(client.get());

            if (abort_.load()) {
                return false;
            }

            const bool canRefresh = !refreshedCredentials && authFailure;
            if (canRefresh) {
                NotifyStatus(L"Refreshing... Doubao IME");
                dcfg_.deviceId.clear();
                dcfg_.cdid.clear();
                dcfg_.token.clear();
                PostCredentials({}, true);
                refreshedCredentials = true;
                if (!SleepUntilAbort(300)) return false;
                continue;
            }

            const bool canRetryTransient = attempt + 1 < kMaxAttempts &&
                doubao_ime_asr::IsTransientFailure(error);
            if (!canRetryTransient) {
                return false;
            }
            if (!SleepUntilAbort(attempt == 0 ? 500 : 1000)) return false;
        }
        if (error.empty()) error = L"connection failed";
        return false;
    }

    bool SendFrameWithReplay(doubao_ime_asr::RealtimeClient& client,
                             CloudAsrReplayBuffer& replayBuffer,
                             const BYTE* data,
                             size_t bytes,
                             size_t replayBytes,
                             bool isLast,
                             std::wstring& error) {
        if (!data || bytes == 0) return true;
        replayBytes = (std::min)(replayBytes, bytes);
        if (replayBytes > 0) {
            std::vector<BYTE> replayChunk(data, data + replayBytes);
            replayBuffer.Append(replayChunk);
        }
        return client.SendPcmFrame(data, bytes, isLast, error);
    }

    DoubaoRetryResult RetryRecognitionOnce(const std::vector<BYTE>& pcm, DWORD finalTimeoutMs) {
        DoubaoRetryResult result;
        if (pcm.empty()) return result;

        doubao_ime_asr::DoubaoImeConfig retryCfg = dcfg_;
        const size_t frameBytes = doubao_ime_asr::FrameBytesForConfig(retryCfg);
        if (frameBytes == 0) {
            result.error = DoubaoErrorText(L"invalid frame size");
            result.transportError = true;
            return result;
        }

        for (int attempt = 0; attempt < 2 && !abort_.load(); ++attempt) {
            doubao_ime_asr::RealtimeClient client(retryCfg);
            SetActiveClient(&client);
            std::wstring error;
            if (!client.Connect(error)) {
                client.Close();
                ClearActiveClient(&client);
                if (attempt == 0 && doubao_ime_asr::IsAuthFailure(error)) {
                    retryCfg.deviceId.clear();
                    retryCfg.cdid.clear();
                    retryCfg.token.clear();
                    PostCredentials({}, true);
                    continue;
                }
                result.error = DoubaoErrorText(error);
                result.transportError = true;
                return result;
            }
            SyncCredentialsFromClient(client);

            bool sendOk = true;
            std::vector<BYTE> frame;
            frame.reserve(frameBytes);
            for (size_t offset = 0; offset < pcm.size() && !abort_.load(); offset += frameBytes) {
                const size_t bytes = std::min(frameBytes, pcm.size() - offset);
                frame.assign(pcm.begin() + static_cast<ptrdiff_t>(offset),
                             pcm.begin() + static_cast<ptrdiff_t>(offset + bytes));
                if (frame.size() < frameBytes) frame.resize(frameBytes, 0);
                const bool isLast = offset + bytes >= pcm.size();
                if (!client.SendPcmFrame(frame.data(), frame.size(), isLast, error)) {
                    sendOk = false;
                    break;
                }
            }

            if (!sendOk || abort_.load()) {
                client.Abort();
                client.Close();
                ClearActiveClient(&client);
                result.error = DoubaoErrorText(error.empty() ? L"send failed" : error);
                result.transportError = true;
                continue;
            }

            std::wstring text;
            if (!client.Finish(finalTimeoutMs, text, error)) {
                client.Close();
                ClearActiveClient(&client);
                result.error = DoubaoErrorText(error);
                result.transportError = true;
                continue;
            }
            client.Close();
            ClearActiveClient(&client);
            result.text = text;
            result.transportError = false;
            return result;
        }

        if (result.error.empty()) result.error = DoubaoErrorText(L"retry failed");
        return result;
    }

    void WorkerLoop() {
        const ULONGLONG tTotal0 = GetTickCount64();
        auto markStopped = [this]() {
            running_.store(false);
            ClearActiveClient();
        };

        std::unique_ptr<doubao_ime_asr::RealtimeClient> client;
        std::wstring error;
        if (!ConnectWithCredentialRetry(client, error)) {
            WaitForRecordingStop();
            if (!abort_.load()) {
                DispatchFinal(DoubaoErrorText(error));
            }
            markStopped();
            return;
        }

        const size_t frameBytes = client->FrameBytes();
        std::vector<BYTE> frame;
        frame.reserve(frameBytes * 2);
        CloudAsrReplayBuffer replayBuffer(kDoubaoMaxReplayBytes);
        std::wstring finalText;
        std::wstring drainError;
        std::mutex drainMutex;
        std::atomic<bool> drainDone{false};
        std::atomic<bool> drainFailed{false};
        std::atomic<bool> finishSent{false};
        std::atomic<bool> postFinishFinalReceived{false};
        std::atomic<bool> drainSessionFinished{false};
        bool failed = false;
        bool retryWithReplay = false;

        std::thread drainThread([&]() {
            std::wstring committedPrefix;
            std::wstring activePartialWindow;
            std::wstring lastFinalSegment;
            std::wstring lastHudText;
            auto currentPreview = [&]() {
                return doubao_ime_asr::MergeRecognizedText(committedPrefix, activePartialWindow);
            };
            while (!drainDone.load() && !abort_.load()) {
                doubao_ime_asr::RealtimeEvent ev;
                std::wstring receiveError;
                if (!client->PollEvent(200, ev, receiveError)) {
                    if (!drainDone.load() && !abort_.load()) {
                        std::lock_guard<std::mutex> lock(drainMutex);
                        drainError = receiveError;
                        drainFailed.store(true);
                    }
                    break;
                }
                if (!ev.partialText.empty()) {
                    std::wstring preview;
                    {
                        std::lock_guard<std::mutex> lock(drainMutex);
                        if (activePartialWindow.empty()) {
                            activePartialWindow = ev.partialText;
                        } else if (LooksLikeSamePartialWindow(activePartialWindow, ev.partialText)) {
                            activePartialWindow = PreferRicherPartialText(activePartialWindow, ev.partialText);
                        } else if (StartsNewPartialWindow(activePartialWindow, ev.partialText)) {
                            committedPrefix = doubao_ime_asr::MergeRecognizedText(committedPrefix, activePartialWindow);
                            activePartialWindow = ev.partialText;
                        } else {
                            activePartialWindow = ev.partialText;
                        }
                        preview = currentPreview();
                        finalText = preview;
                    }
                    if (config_.enablePartial && !preview.empty() && preview != lastHudText) {
                        lastHudText = preview;
                        NotifyPartial(preview, false);
                    }
                }
                if (ev.transcriptionCompleted && !ev.finalText.empty()) {
                    std::wstring committed;
                    {
                        std::lock_guard<std::mutex> lock(drainMutex);
                        if (ev.finalText != lastFinalSegment) {
                            if (!activePartialWindow.empty() &&
                                LooksLikeSamePartialWindow(activePartialWindow, ev.finalText)) {
                                const std::wstring segment =
                                    PreferRicherPartialText(activePartialWindow, ev.finalText);
                                committedPrefix = doubao_ime_asr::MergeRecognizedText(committedPrefix, segment);
                                activePartialWindow.clear();
                            } else if (!activePartialWindow.empty() &&
                                       StartsNewPartialWindow(activePartialWindow, ev.finalText)) {
                                committedPrefix = doubao_ime_asr::MergeRecognizedText(committedPrefix, activePartialWindow);
                                committedPrefix = doubao_ime_asr::MergeRecognizedText(committedPrefix, ev.finalText);
                                activePartialWindow.clear();
                            } else {
                                committedPrefix = doubao_ime_asr::MergeRecognizedText(committedPrefix, ev.finalText);
                                activePartialWindow.clear();
                            }
                            lastFinalSegment = ev.finalText;
                        }
                        finalText = currentPreview();
                        committed = finalText;
                    }
                    if (config_.enablePartial && !committed.empty() && committed != lastHudText) {
                        lastHudText = committed;
                        NotifyPartial(committed, false);
                    }
                    if (finishSent.load()) {
                        postFinishFinalReceived.store(true);
                    }
                }
                if (ev.sessionFinished) {
                    if (!committedPrefix.empty() || !activePartialWindow.empty()) {
                        std::lock_guard<std::mutex> lock(drainMutex);
                        if (finalText.empty()) finalText = currentPreview();
                    }
                    drainSessionFinished.store(true);
                    break;
                }
            }
        });

        auto stopDrain = [&](bool forceClose) {
            drainDone.store(true);
            if (forceClose && client) {
                client->Abort();
            }
            if (drainThread.joinable()) {
                drainThread.join();
            }
        };

        auto bufferUntilStop = [&]() {
            NotifyStatus(L"Buffering... Doubao IME");
            while (streaming_.load() && !abort_.load()) {
                std::vector<BYTE> buffered;
                pendingAudio_.SwapTo(buffered);
                if (!buffered.empty()) {
                    replayBuffer.Append(buffered);
                }
                if (pendingAudio_.Overflowed()) {
                    error = L"audio buffer overflow while reconnecting";
                    failed = true;
                    retryWithReplay = true;
                    break;
                }
                Sleep(20);
            }
        };

        auto flushFullFrames = [&]() -> bool {
            while (frame.size() >= frameBytes) {
                if (!SendFrameWithReplay(*client, replayBuffer, frame.data(), frameBytes, frameBytes, false, error)) {
                    failed = true;
                    retryWithReplay = true;
                    return false;
                }
                frame.erase(frame.begin(), frame.begin() + static_cast<ptrdiff_t>(frameBytes));
            }
            return true;
        };

        while (!abort_.load() && !drainFailed.load()) {
            std::vector<BYTE> pending;
            pendingAudio_.SwapTo(pending);
            const bool stillStreaming = streaming_.load();

            if (!pending.empty()) {
                frame.insert(frame.end(), pending.begin(), pending.end());
                if (!flushFullFrames()) break;
            }
            if (pendingAudio_.Overflowed()) {
                error = L"audio buffer overflow while recording";
                failed = true;
                retryWithReplay = true;
                break;
            }

            if (!stillStreaming) break;
            Sleep(20);
        }

        if (abort_.load()) {
            stopDrain(true);
            if (client) client->Close();
            markStopped();
            return;
        }

        if (drainFailed.load()) {
            std::lock_guard<std::mutex> lock(drainMutex);
            error = drainError.empty() ? L"receive failed" : drainError;
            failed = true;
            retryWithReplay = true;
        }

        if (failed && streaming_.load()) {
            bufferUntilStop();
        }

        if (!failed) {
            std::vector<BYTE> remaining;
            pendingAudio_.SwapTo(remaining);
            if (!remaining.empty()) {
                frame.insert(frame.end(), remaining.begin(), remaining.end());
                if (!flushFullFrames()) failed = true;
            }
        } else {
            if (!frame.empty()) {
                std::vector<BYTE> unsent = frame;
                replayBuffer.Append(unsent);
            }
            pendingAudio_.Clear();
        }

        if (!failed && !abort_.load()) {
            std::vector<BYTE> lastFrame;
            size_t lastReplayBytes = 0;
            if (!frame.empty()) {
                lastReplayBytes = frame.size();
                lastFrame = std::move(frame);
                lastFrame.resize(frameBytes, 0);
            } else if (client->HasSentAudio()) {
                lastFrame.assign(frameBytes, 0);
            }

            if (!lastFrame.empty()) {
                if (!SendFrameWithReplay(*client, replayBuffer, lastFrame.data(), lastFrame.size(), lastReplayBytes, true, error)) {
                    failed = true;
                    retryWithReplay = true;
                }
            }
        }

        if (!failed && !abort_.load()) {
            const DWORD finalTimeout = IsFallbackAsrEnabled(config_)
                ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs_.load(), capturedPcmBytes_.load())
                : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs_.load(), capturedPcmBytes_.load());
            finishSent.store(true);
            if (!client->SendFinishSession(error)) {
                failed = true;
                retryWithReplay = true;
            } else {
                const ULONGLONG deadline = GetTickCount64() + std::max<DWORD>(finalTimeout, 1000);
                while (!abort_.load()) {
                    if (drainFailed.load()) {
                        std::lock_guard<std::mutex> lock(drainMutex);
                        error = drainError.empty() ? L"receive failed" : drainError;
                        failed = true;
                        retryWithReplay = true;
                        break;
                    }
                    if (postFinishFinalReceived.load() || drainSessionFinished.load()) {
                        break;
                    }
                    if (GetTickCount64() >= deadline) {
                        error = L"timed out waiting for final transcript";
                        failed = true;
                        retryWithReplay = true;
                        break;
                    }
                    Sleep(50);
                }
            }
        }

        stopDrain(failed || abort_.load() || !drainSessionFinished.load());
        if (client) {
            client->Close();
            ClearActiveClient(client.get());
        }
        if (abort_.load()) {
            markStopped();
            return;
        }

        const bool shouldRetryEmptyFinal = !failed && finalText.empty() &&
            replayBuffer.Available() && replayBuffer.Size() >= kDoubaoEmptyRetryMinBytes;
        const bool shouldRetryFailure = failed && retryWithReplay &&
            replayBuffer.Available() && !replayBuffer.Empty();
        if (shouldRetryEmptyFinal || shouldRetryFailure) {
            NotifyStatus(L"Retrying... Doubao IME");
            const DWORD retryTimeout = IsFallbackAsrEnabled(config_)
                ? ComputeCloudAsrStreamingFinalWaitMs(recordingMs_.load(), replayBuffer.Size())
                : ComputeCloudAsrLegacyFinalizeTimeoutMs(recordingMs_.load(), replayBuffer.Size());
            DoubaoRetryResult retryResult = RetryRecognitionOnce(replayBuffer.Data(), retryTimeout);
            if (!retryResult.text.empty()) {
                finalText = retryResult.text;
                failed = false;
            } else if (!retryResult.transportError) {
                finalText.clear();
                failed = false;
            } else if (shouldRetryFailure) {
                error = retryResult.error.empty() ? error : retryResult.error;
                failed = true;
            }
        }
        if (abort_.load()) {
            markStopped();
            return;
        }

        g_cloudApiMs = std::max(0.0, static_cast<double>(GetTickCount64() - tTotal0) - recordingMs_.load());
        DispatchFinal(failed ? DoubaoErrorText(error) : finalText);
        markStopped();
    }

    doubao_ime_asr::DoubaoImeConfig dcfg_;
    PendingPcmBuffer pendingAudio_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex activeClientMutex_;
    doubao_ime_asr::RealtimeClient* activeClient_ = nullptr;
    std::atomic<double> recordingMs_{0.0};
    std::atomic<size_t> capturedPcmBytes_{0};
};

} // namespace

std::unique_ptr<IStreamingAsrSession> CreateDoubaoImeStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText) {
    return std::make_unique<DoubaoImeStreamingSession>(config, targetWindow, refineFn, lastRawAsrText);
}
