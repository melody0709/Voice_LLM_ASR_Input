#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "cloud_http_common.h"

#include <windows.h>

#include <string>
#include <vector>

namespace mai_transcribe {

enum class ApiProvider {
    OpenRouter,
    AzureSpeech,
};

struct Config {
    ApiProvider apiProvider = ApiProvider::OpenRouter;
    std::wstring apiKey;
    std::wstring azureEndpoint;
    std::wstring language = L"auto";
};

struct Result {
    bool ok = false;
    bool retryable = false;
    std::wstring text;
    std::wstring error;
    std::string providerCode;
    DWORD statusCode = 0;
    DWORD winhttpError = 0;
    double elapsedMs = 0.0;
    size_t networkBytes = 0;
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

// Pure protocol helpers used by the offline regression target.
std::string EncodeBase64ForTest(const std::vector<BYTE>& data);
std::string BuildOpenRouterJsonForTest(const std::string& audioBase64,
                                       const std::wstring& language);
std::vector<BYTE> BuildAzureMultipartForTest(const std::vector<BYTE>& wav,
                                             const std::wstring& language,
                                             const std::string& boundary);
std::wstring ParseOpenRouterTextForTest(const std::string& responseBody);
std::wstring ParseAzureTextForTest(const std::string& responseBody);
bool ValidateAzureEndpointForTest(const std::wstring& endpoint,
                                  std::wstring& error);

} // namespace mai_transcribe
