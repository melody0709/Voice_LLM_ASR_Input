#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace qwen_asr {

constexpr wchar_t kDefaultBaseUrl[] =
    L"wss://llm-c6rtn7zy4nw0u39k.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime";
constexpr wchar_t kDefaultModel[] = L"qwen3-asr-flash-realtime";

struct QwenConfig {
    uint64_t attemptId = 0;
    std::wstring apiKey;
    std::wstring baseUrl = kDefaultBaseUrl;
    std::wstring model = kDefaultModel;
    std::wstring language;
    std::wstring turnDetection = L"manual";
    float vadThreshold = 0.0f;
    int vadSilenceMs = 400;
    int chunkMs = 100;
};

struct TestResult {
    bool ok = false;
    std::wstring message;
};

struct RealtimeEvent {
    std::wstring partialText;
    std::wstring finalText;
    bool transcriptionCompleted = false;
    bool sessionFinished = false;
    // WebSocket peer close is a transport outcome, not the provider's
    // JSON session.finished event.
    bool peerClosed = false;
};

class RealtimeClient {
public:
    explicit RealtimeClient(QwenConfig cfg);
    ~RealtimeClient();

    RealtimeClient(const RealtimeClient&) = delete;
    RealtimeClient& operator=(const RealtimeClient&) = delete;

    bool Connect(std::wstring& error);
    bool SendAudioChunk(const BYTE* data, size_t bytes, std::wstring& error);
    bool PollEvent(DWORD timeoutMs, RealtimeEvent& event, std::wstring& error);
    bool SendFinish(std::wstring& error);
    bool Finish(DWORD finalTimeoutMs, std::wstring& finalText, std::wstring& error);
    void Abort();
    void Close();
    bool LastFailureRetryable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

size_t ChunkBytesForConfig(const QwenConfig& cfg);
std::wstring Recognize(const std::vector<BYTE>& pcm, const QwenConfig& cfg, DWORD finalTimeoutMs);
TestResult TestConnection(const QwenConfig& cfg);

#if defined(VOXTYPE_QWEN_AUDIO_PROTOCOL_TEST)
struct ProtocolEventForTest {
    std::wstring type;
    std::wstring partialText;
    std::wstring finalText;
    std::wstring error;
    bool transcriptionCompleted = false;
    bool sessionFinished = false;
    bool failed = false;
};

std::wstring BuildEndpointUrlForTest(const QwenConfig& cfg);
std::string BuildSessionUpdateMessageForTest(const QwenConfig& cfg);
std::string BuildAudioAppendMessageForTest(const BYTE* data, size_t bytes);
std::string BuildCommitMessageForTest();
std::string BuildSessionFinishMessageForTest();
ProtocolEventForTest ParseServerEventForTest(const std::string& message);
#endif

} // namespace qwen_asr
