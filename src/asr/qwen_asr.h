#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace qwen_asr {

constexpr wchar_t kDefaultBaseUrl[] = L"wss://dashscope.aliyuncs.com/api-ws/v1/realtime";
constexpr wchar_t kDefaultModel[] = L"qwen3-asr-flash-realtime";

struct QwenConfig {
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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

size_t ChunkBytesForConfig(const QwenConfig& cfg);
std::wstring Recognize(const std::vector<BYTE>& pcm, const QwenConfig& cfg, DWORD finalTimeoutMs);
TestResult TestConnection(const QwenConfig& cfg);

} // namespace qwen_asr
