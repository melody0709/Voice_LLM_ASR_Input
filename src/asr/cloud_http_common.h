#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <string>
#include <mutex>
#include <vector>

class CloudHttpCancellation {
public:
    void Reset();
    bool Attach(HINTERNET request);
    bool Detach(HINTERNET request);
    void Abort();
    bool IsAborted() const;

private:
    mutable std::mutex mutex_;
    HINTERNET activeRequest_ = nullptr;
    bool aborted_ = false;
};

struct CloudHttpRequest {
    std::wstring method = L"POST";
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring path;
    std::wstring headers;
    std::vector<BYTE> body;
    bool useSsl = true;
    DWORD timeoutMs = 8000;
    DWORD accessType = WINHTTP_ACCESS_TYPE_DEFAULT_PROXY;
    CloudHttpCancellation* cancellation = nullptr;
};

struct CloudHttpResponse {
    bool ok = false;
    DWORD winhttpError = 0;
    DWORD statusCode = 0;
    std::string body;
    std::wstring failedStep;
};

CloudHttpResponse SendCloudHttpRequest(const CloudHttpRequest& request);
bool IsTransientCloudHttpError(DWORD error);
bool IsRetryableCloudHttpStatus(DWORD statusCode);
void SleepCloudHttpRetryBackoff(int attemptIndex);
