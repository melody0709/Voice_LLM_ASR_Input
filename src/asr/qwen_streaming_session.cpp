#include "qwen_streaming_session.h"

#include "asr_streaming_session_base.h"
#include "cloud_asr_common.h"
#include "globals.h"
#include "pending_pcm_buffer.h"
#include "qwen_asr.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kQwenRecordingWatchdogMs = 18000;
constexpr size_t kQwenMaxReplayBytes = 120u * 32000u;
constexpr size_t kQwenEmptyRetryMinBytes = 3u * 32000u;

qwen_asr::QwenConfig BuildQwenConfig(const Config& config) {
    qwen_asr::QwenConfig qcfg;
    qcfg.apiKey = config.qwenApiKey;
    qcfg.baseUrl = config.qwenBaseUrl;
    qcfg.model = config.qwenModel;
    qcfg.language = config.qwenLanguage;
    qcfg.turnDetection = L"manual";
    qcfg.chunkMs = config.qwenChunkMs;
    return qcfg;
}

std::wstring QwenErrorText(const std::wstring& error) {
    if (error.rfind(L"Qwen ASR error:", 0) == 0) return error;
    return L"Qwen ASR error: " + (error.empty() ? L"unknown error" : error);
}

struct QwenRetryResult {
    std::wstring text;
    std::wstring error;
    bool transportError = false;
};

class QwenStreamingSession final : public StreamingAsrSessionBase {
public:
    QwenStreamingSession(Config config,
                         HWND targetWindow,
                         AsrLlmRefineFn refineFn,
                         std::wstring* lastRawAsrText)
        : StreamingAsrSessionBase(std::move(config), targetWindow, refineFn, lastRawAsrText),
          qcfg_(BuildQwenConfig(config_)) {}

    ~QwenStreamingSession() override {
        Abort();
    }

    bool Start(std::wstring& error) override {
        if (running_.load()) {
            error = L"Qwen ASR error: session already running";
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
        if (streaming_.load()) return kQwenRecordingWatchdogMs;
        return ComputeCloudAsrFinalizeTimeoutMs(recordingMs_.load(), capturedPcmBytes_.load());
    }

    const wchar_t* ProviderName() const override {
        return L"Qwen ASR";
    }

private:
    void SetActiveClient(qwen_asr::RealtimeClient* client) {
        std::lock_guard<std::mutex> lock(activeClientMutex_);
        activeClient_ = client;
    }

    void ClearActiveClient(qwen_asr::RealtimeClient* expected = nullptr) {
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

    QwenRetryResult RetryRecognitionOnce(const std::vector<BYTE>& pcm, DWORD finalTimeoutMs) {
        QwenRetryResult result;
        if (pcm.empty()) return result;

        qwen_asr::QwenConfig retryCfg = qcfg_;
        retryCfg.turnDetection = L"manual";
        const int retryDelays[] = {500, 1000};
        for (int attempt = 0; attempt < 3 && !abort_.load(); ++attempt) {
            if (attempt > 0) {
                Sleep(retryDelays[(std::min)(attempt - 1, 1)]);
            }
            qwen_asr::RealtimeClient client(retryCfg);
            SetActiveClient(&client);
            std::wstring error;
            if (!client.Connect(error)) {
                client.Close();
                ClearActiveClient(&client);
                result.error = QwenErrorText(error);
                result.transportError = true;
                continue;
            }

            const size_t chunkBytes = qwen_asr::ChunkBytesForConfig(retryCfg);
            bool sendOk = true;
            for (size_t offset = 0; offset < pcm.size() && !abort_.load(); offset += chunkBytes) {
                const size_t bytes = std::min(chunkBytes, pcm.size() - offset);
                if (!client.SendAudioChunk(pcm.data() + offset, bytes, error)) {
                    sendOk = false;
                    break;
                }
            }
            if (!sendOk || abort_.load()) {
                client.Abort();
                client.Close();
                ClearActiveClient(&client);
                result.error = QwenErrorText(error);
                result.transportError = true;
                continue;
            }

            std::wstring text;
            if (abort_.load()) {
                client.Abort();
                client.Close();
                ClearActiveClient(&client);
                result.error = L"Qwen ASR error: aborted";
                result.transportError = true;
                return result;
            }
            if (!client.Finish(finalTimeoutMs, text, error)) {
                client.Close();
                ClearActiveClient(&client);
                result.error = QwenErrorText(error);
                result.transportError = true;
                continue;
            }
            client.Close();
            ClearActiveClient(&client);
            result.text = text;
            result.transportError = false;
            return result;
        }
        if (result.error.empty()) {
            result.error = L"Qwen ASR error: retry failed";
        }
        return result;
    }

    void WorkerLoop() {
        const ULONGLONG tTotal0 = GetTickCount64();
        auto markStopped = [this]() {
            running_.store(false);
            ClearActiveClient();
        };

        if (qcfg_.apiKey.empty()) {
            WaitForRecordingStop();
            if (!abort_.load()) {
                DispatchFinal(L"Qwen ASR error: missing DashScope API key");
            }
            markStopped();
            return;
        }

        std::unique_ptr<qwen_asr::RealtimeClient> client;
        std::wstring error;
        bool connected = false;
        constexpr int kMaxConnectAttempts = 2;
        for (int attempt = 0; attempt < kMaxConnectAttempts && !abort_.load(); ++attempt) {
            client = std::make_unique<qwen_asr::RealtimeClient>(qcfg_);
            SetActiveClient(client.get());
            if (client->Connect(error)) {
                connected = true;
                break;
            }
            client->Close();
            ClearActiveClient(client.get());
            if (attempt + 1 < kMaxConnectAttempts && !abort_.load()) {
                NotifyStatus(L"Reconnecting... Qwen ASR");
                Sleep(500);
            }
        }

        if (!connected) {
            WaitForRecordingStop();
            if (!abort_.load()) {
                DispatchFinal(QwenErrorText(error));
            }
            markStopped();
            return;
        }

        const size_t chunkBytes = qwen_asr::ChunkBytesForConfig(qcfg_);
        std::vector<BYTE> chunk;
        chunk.reserve(chunkBytes * 2);
        CloudAsrReplayBuffer replayBuffer(kQwenMaxReplayBytes);
        std::wstring finalText;
        std::wstring drainError;
        std::mutex drainMutex;
        std::atomic<bool> drainDone{false};
        std::atomic<bool> drainFailed{false};
        std::atomic<bool> drainCompleted{false};
        std::atomic<bool> drainSessionFinished{false};
        bool failed = false;
        bool retryWithReplay = false;

        std::thread drainThread([&]() {
            std::wstring lastPartial;
            while (!drainDone.load() && !abort_.load()) {
                qwen_asr::RealtimeEvent ev;
                std::wstring receiveError;
                if (!client->PollEvent(200, ev, receiveError)) {
                    if (!drainDone.load() && !abort_.load()) {
                        std::lock_guard<std::mutex> lock(drainMutex);
                        drainError = receiveError;
                        drainFailed.store(true);
                    }
                    break;
                }
                if (!ev.partialText.empty() && ev.partialText != lastPartial) {
                    lastPartial = ev.partialText;
                    if (config_.enablePartial) {
                        NotifyPartial(ev.partialText, false);
                    }
                }
                if (ev.transcriptionCompleted) {
                    std::lock_guard<std::mutex> lock(drainMutex);
                    finalText = ev.finalText;
                    drainCompleted.store(true);
                }
                if (ev.sessionFinished) {
                    drainSessionFinished.store(true);
                    break;
                }
            }
        });

        auto stopDrain = [&](bool forceClose) {
            drainDone.store(true);
            if (forceClose) {
                client->Abort();
            }
            if (drainThread.joinable()) {
                drainThread.join();
            }
        };

        auto appendReplay = [&](const BYTE* data, size_t bytes) {
            if (!data || bytes == 0) return;
            std::vector<BYTE> replayChunk(data, data + bytes);
            replayBuffer.Append(replayChunk);
        };

        auto bufferUntilStop = [&]() {
            NotifyStatus(L"Buffering... Qwen ASR");
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

        auto sendChunk = [&](const BYTE* data, size_t bytes) -> bool {
            if (bytes == 0) return true;
            appendReplay(data, bytes);
            if (!client->SendAudioChunk(data, bytes, error)) {
                failed = true;
                retryWithReplay = true;
                return false;
            }
            return true;
        };

        auto flushFullChunks = [&]() -> bool {
            while (chunk.size() >= chunkBytes) {
                if (!sendChunk(chunk.data(), chunkBytes)) return false;
                chunk.erase(chunk.begin(), chunk.begin() + static_cast<ptrdiff_t>(chunkBytes));
            }
            return true;
        };

        while (!abort_.load() && !drainFailed.load()) {
            std::vector<BYTE> pending;
            pendingAudio_.SwapTo(pending);
            bool streaming = streaming_.load();

            if (!pending.empty()) {
                chunk.insert(chunk.end(), pending.begin(), pending.end());
                if (!flushFullChunks()) break;
            }
            if (pendingAudio_.Overflowed()) {
                error = L"audio buffer overflow while recording";
                failed = true;
                retryWithReplay = true;
                break;
            }

            if (!streaming) break;
            Sleep(20);
        }

        if (abort_.load()) {
            stopDrain(true);
            client->Close();
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
                chunk.insert(chunk.end(), remaining.begin(), remaining.end());
                if (!flushFullChunks()) failed = true;
            }
        } else {
            pendingAudio_.Clear();
        }
        if (!failed && !chunk.empty()) {
            failed = !sendChunk(chunk.data(), chunk.size());
            chunk.clear();
        }

        if (!failed && !abort_.load()) {
            const DWORD finalTimeout = ComputeCloudAsrFinalizeTimeoutMs(recordingMs_.load(), capturedPcmBytes_.load());
            if (!client->SendFinish(error)) {
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
                    if (drainCompleted.load() || drainSessionFinished.load()) {
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
        client->Close();
        ClearActiveClient(client.get());
        if (abort_.load()) {
            markStopped();
            return;
        }

        const bool shouldRetryEmptyFinal = !failed && !abort_.load() && finalText.empty() &&
            replayBuffer.Available() && replayBuffer.Size() >= kQwenEmptyRetryMinBytes;
        const bool shouldRetryFailure = failed && !abort_.load() && retryWithReplay &&
            replayBuffer.Available() && !replayBuffer.Empty();
        if (shouldRetryEmptyFinal || shouldRetryFailure) {
            NotifyStatus(L"Retrying... Qwen ASR");
            const DWORD retryTimeout = ComputeCloudAsrFinalizeTimeoutMs(recordingMs_.load(), replayBuffer.Size());
            QwenRetryResult retryResult = RetryRecognitionOnce(replayBuffer.Data(), retryTimeout);
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
        DispatchFinal(failed ? QwenErrorText(error) : finalText);
        markStopped();
    }

    qwen_asr::QwenConfig qcfg_;
    PendingPcmBuffer pendingAudio_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> abort_{false};
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::mutex activeClientMutex_;
    qwen_asr::RealtimeClient* activeClient_ = nullptr;
    std::atomic<double> recordingMs_{0.0};
    std::atomic<size_t> capturedPcmBytes_{0};
};

} // namespace

std::unique_ptr<IStreamingAsrSession> CreateQwenStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText) {
    return std::make_unique<QwenStreamingSession>(config, targetWindow, refineFn, lastRawAsrText);
}
