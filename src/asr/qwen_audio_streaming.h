#pragma once

#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>

namespace qwen_audio_streaming {

struct Config {
    uint64_t attemptId = 0;
    std::wstring apiKey;
    std::wstring baseUrl;
    std::wstring model = L"qwen-audio-3.0-asr-flash-streaming";
    std::wstring languageHints;
    std::wstring vocabularyId;
    std::wstring vocabulary;
    // Optional focused input-field context sent in run-task.payload.input.
    std::wstring inputContextText;
    bool semanticPunctuation = false;
    int maxSentenceSilenceMs = 1300;
    bool multiThresholdMode = false;
    bool heartbeat = false;
    bool speechNoiseThresholdEnabled = false;
    float speechNoiseThreshold = 0.0f;
};

struct Event {
    std::wstring action;
    std::wstring text;
    std::wstring message;
    std::wstring errorCode;
    bool taskStarted = false;
    bool taskFinished = false;
    bool failed = false;
    bool timeout = false;
    bool sentenceEnd = false;
    bool heartbeat = false;
    // Provider may encode silence as task-failed/ASR_RESPONSE_HAVE_NO_WORDS.
    bool noSpeech = false;
    // Transport-level close events may be replayed once. A server task failure
    // means the request was parsed/rejected and must not be blindly replayed.
    bool retryable = false;
};

class TranscriptAccumulator {
public:
    void Apply(const Event& event);
    std::wstring Text() const;

private:
    std::wstring committed_;
    std::wstring pending_;
};

// Pure protocol helpers are exposed so offline tests can validate the exact
// JSON/frame contract without opening a network connection.
std::string BuildRunTaskMessage(const Config& config, const std::string& taskId);
std::string BuildFinishTaskMessage(const std::string& taskId);
Event ParseServerEventMessage(const std::string& message);

class Client {
public:
    explicit Client(Config config);
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    bool Connect(std::wstring& error);
    bool SendAudio(const BYTE* data, size_t bytes, std::wstring& error);
    bool Poll(DWORD timeoutMs, Event& event, std::wstring& error);
    bool Finish(std::wstring& error);
    void Abort();
    void Close();
    // Valid after Connect() returns false. This distinguishes a transient
    // transport/handshake failure from a server-side task rejection.
    bool LastFailureRetryable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct TestResult { bool ok = false; std::wstring message; };
TestResult TestConnection(const Config& config);

} // namespace qwen_audio_streaming
