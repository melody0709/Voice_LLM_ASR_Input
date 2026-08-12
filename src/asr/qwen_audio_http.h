#pragma once

#include <windows.h>

#include "cloud_http_common.h"

#include <string>
#include <vector>

namespace qwen_audio_http {

struct Config {
    std::wstring apiKey;
    std::wstring baseUrl;
    std::wstring model = L"qwen-audio-3.0-asr-flash";
    std::wstring languageHints;
    std::wstring vocabularyId;
    std::wstring vocabulary;
    // Optional focused input-field context. It is serialized as an
    // input_text message before the current input_audio message.
    std::wstring inputContextText;
};

// Pure request helpers used by offline protocol tests. They do not perform
// network I/O and keep the WAV/JSON contract independent from WinHTTP.
std::vector<BYTE> BuildWavForPcm(const std::vector<BYTE>& pcm16k16Mono);
std::string EncodeBase64ForTest(const std::vector<BYTE>& data);
std::string BuildRequestJsonForTest(const Config& config, const std::string& audioBase64);
bool IsNoSpeechResponseForTest(DWORD statusCode, const std::string& responseBody);
std::wstring ParseResponseTextForTest(const std::string& responseBody);

struct Result {
    bool ok = false;
    bool retryable = false;
    std::wstring text;
    std::wstring error;
    DWORD statusCode = 0;
    double elapsedMs = 0.0;
};

Result Recognize(const std::vector<BYTE>& pcm16k16Mono,
                 const Config& config,
                 DWORD timeoutMs,
                 CloudHttpCancellation* cancellation = nullptr);

struct TestResult {
    bool ok = false;
    std::wstring message;
};

TestResult TestConnection(const Config& config);

} // namespace qwen_audio_http
