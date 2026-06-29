#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace doubao_ime_asr {

struct Credentials {
    std::wstring deviceId;
    std::wstring cdid;
    std::wstring token;
};

struct DoubaoImeConfig {
    std::wstring deviceId;
    std::wstring cdid;
    std::wstring token;
    int sampleRate = 16000;
    int channels = 1;
    int frameMs = 20;
    bool enablePunctuation = true;
    DWORD connectTimeoutMs = 10000;
    DWORD recvTimeoutMs = 15000;
};

struct TestResult {
    bool ok = false;
    std::wstring message;
    Credentials credentials;
    bool credentialsChanged = false;
};

struct RecordedRecognitionResult {
    bool ok = false;
    std::wstring text;
    std::wstring error;
    double elapsedMs = 0.0;
    Credentials credentials;
    bool credentialsChanged = false;
    bool clearCredentials = false;
};

struct RealtimeEvent {
    std::wstring partialText;
    std::wstring finalText;
    bool transcriptionCompleted = false;
    bool sessionFinished = false;
};

struct CredentialsUpdateMessage {
    Credentials credentials;
    bool clear = false;
};

class RealtimeClient {
public:
    explicit RealtimeClient(DoubaoImeConfig cfg);
    ~RealtimeClient();

    RealtimeClient(const RealtimeClient&) = delete;
    RealtimeClient& operator=(const RealtimeClient&) = delete;

    bool Connect(std::wstring& error);
    bool SendPcmFrame(const BYTE* pcm, size_t bytes, bool isLast, std::wstring& error);
    bool SendFinishSession(std::wstring& error);
    bool PollEvent(DWORD timeoutMs, RealtimeEvent& event, std::wstring& error);
    bool Finish(DWORD finalTimeoutMs, std::wstring& finalText, std::wstring& error);
    void Abort();
    void Close();

    bool CredentialsChanged() const;
    Credentials CurrentCredentials() const;
    size_t FrameBytes() const;
    bool HasSentAudio() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

bool IsAuthFailure(const std::wstring& error);
bool IsTransientFailure(const std::wstring& error);
size_t FrameBytesForConfig(const DoubaoImeConfig& cfg);
std::wstring MergeRecognizedText(std::wstring base, const std::wstring& incoming);
std::wstring ErrorText(const std::wstring& error);
RecordedRecognitionResult RecognizeRecordedPcm(const DoubaoImeConfig& cfg,
                                               const std::vector<BYTE>& pcm16k16Mono,
                                               DWORD finalTimeoutMs);
TestResult TestConnection(const DoubaoImeConfig& cfg);

} // namespace doubao_ime_asr
