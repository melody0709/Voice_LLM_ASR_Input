#include "qwen_audio_streaming_session.h"

#include "asr_result.h"
#include "asr_streaming_session_base.h"
#include "cloud_asr_common.h"
#include "globals.h"
#include "pending_pcm_buffer.h"
#include "qwen_audio_streaming.h"

#include <algorithm>
#include <atomic>
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

qwen_audio_streaming::Config BuildConfig(const Config& c) {
    qwen_audio_streaming::Config out;
    out.apiKey = c.qwenApiKey;
    out.baseUrl = c.qwenAudioStreamingBaseUrl;
    out.model = c.qwenModel;
    out.languageHints = c.qwenLanguageHints.empty() ? c.qwenLanguage : c.qwenLanguageHints;
    out.vocabularyId = c.qwenVocabularyId;
    out.vocabulary = c.qwenVocabulary;
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

        qwen_audio_streaming::Client client(cfg_);
        SetActiveClient(&client);
        std::wstring error;
        if (!client.Connect(error)) {
            result.error = error;
            client.Close();
            ClearActiveClient(&client);
            return result;
        }

        std::mutex stateMutex;
        std::mutex textMutex;
        std::wstring taskError;
        qwen_audio_streaming::TranscriptAccumulator transcript;
        std::atomic<bool> drainDone{false};
        std::atomic<bool> failed{false};
        std::atomic<bool> finished{false};
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
                    finished.store(true);
                    break;
                }
                if (event.failed) {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    taskError = event.message.empty() ? L"task failed" : event.message;
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
        CloudAsrReplayBuffer replay(kMaxReplayBytes);
        std::unique_ptr<qwen_audio_streaming::Client> client;
        std::wstring connectError;
        bool connected = false;
        bool connectRetryable = true;

        if (cfg_.apiKey.empty()) {
            std::wstring missingKeyError = L"missing DashScope API key";
            BufferUntilStop(replay, clientBuffer_, false, missingKeyError);
            DispatchAttempt(true, missingKeyError, {});
            Finish();
            return;
        }

        for (DWORD attempt = 0; attempt < kInitialConnectAttempts && !abort_.load(); ++attempt) {
            client = std::make_unique<qwen_audio_streaming::Client>(cfg_);
            SetActiveClient(client.get());
            if (client->Connect(connectError)) {
                connected = true;
                break;
            }
            connectRetryable = client->LastFailureRetryable();
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
                DispatchAttempt(!retry.ok, retry.error, retry.text);
            } else {
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
        std::thread drain([&] {
            while (!drainDone.load() && !abort_.load()) {
                qwen_audio_streaming::Event event;
                std::wstring receiveError;
                if (!client->Poll(200, event, receiveError)) {
                    if (event.timeout) continue;
                    std::lock_guard<std::mutex> lock(stateMutex);
                    error = receiveError.empty() ? L"WebSocket receive failed" : receiveError;
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
            std::wstring finishError;
            if (!client->Finish(finishError)) {
                {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    error = finishError.empty() ? L"finish-task send failed" : finishError;
                }
                failed = true;
                retryWithReplay = replayEnabled;
            } else {
                const DWORD timeout = CurrentWatchdogMs();
                const ULONGLONG deadline = GetTickCount64() + timeout;
                while (!abort_.load() && !drainFailed.load() && !taskFinished.load() &&
                       GetTickCount64() < deadline) {
                    Sleep(25);
                }
                if (!taskFinished.load()) {
                    failed = true;
                    retryWithReplay = replayEnabled;
                    std::lock_guard<std::mutex> lock(stateMutex);
                    if (error.empty()) error = L"timed out waiting for task-finished";
                }
            }
        }

        drainDone.store(true);
        if (failed || drainFailed.load() || noSpeech.load() || abort_.load()) client->Abort();
        if (drain.joinable()) drain.join();
        client->Close();
        ClearActiveClient(client.get());

        std::wstring finalText;
        {
            std::lock_guard<std::mutex> lock(textMutex);
            finalText = transcript.Text();
        }
        std::wstring finalError;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            finalError = error;
        }

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
};

} // namespace

std::unique_ptr<IStreamingAsrSession> CreateQwenAudioStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText) {
    return std::make_unique<Session>(config, targetWindow, refineFn, lastRawAsrText);
}
