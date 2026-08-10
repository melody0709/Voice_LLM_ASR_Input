#include "cloud_http_common.h"

#include <algorithm>
#include <limits>

namespace {

constexpr size_t kMaxCloudHttpBodyBytes = 16u * 1024u * 1024u;

class ScopedWinHttpHandle {
public:
    explicit ScopedWinHttpHandle(HINTERNET handle = nullptr) : handle_(handle) {}
    ~ScopedWinHttpHandle() {
        if (!handle_) return;
        if (cancellation_ && registered_ && !cancellation_->Detach(handle_)) {
            // Abort() already closed this handle to interrupt a blocking
            // WinHTTP call; do not close it a second time.
            return;
        }
        WinHttpCloseHandle(handle_);
    }

    ScopedWinHttpHandle(const ScopedWinHttpHandle&) = delete;
    ScopedWinHttpHandle& operator=(const ScopedWinHttpHandle&) = delete;

    bool RegisterCancellation(CloudHttpCancellation* cancellation) {
        cancellation_ = cancellation;
        if (!cancellation_) {
            registered_ = false;
            return true;
        }
        registered_ = cancellation_->Attach(handle_);
        return registered_;
    }

    HINTERNET get() const { return handle_; }
    explicit operator bool() const { return handle_ != nullptr; }

private:
    HINTERNET handle_ = nullptr;
    CloudHttpCancellation* cancellation_ = nullptr;
    bool registered_ = false;
};

} // namespace

void CloudHttpCancellation::Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    activeRequest_ = nullptr;
    aborted_ = false;
}

bool CloudHttpCancellation::Attach(HINTERNET request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (aborted_ || activeRequest_ != nullptr) return false;
    activeRequest_ = request;
    return true;
}

bool CloudHttpCancellation::Detach(HINTERNET request) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (activeRequest_ != request) return false;
    activeRequest_ = nullptr;
    return true;
}

void CloudHttpCancellation::Abort() {
    HINTERNET request = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        aborted_ = true;
        request = activeRequest_;
        activeRequest_ = nullptr;
    }
    if (request) WinHttpCloseHandle(request);
}

bool CloudHttpCancellation::IsAborted() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return aborted_;
}

CloudHttpResponse SendCloudHttpRequest(const CloudHttpRequest& request) {
    CloudHttpResponse response;

    ScopedWinHttpHandle hSession(WinHttpOpen(
        L"VoxType/1.0",
        request.accessType,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0));
    if (!hSession) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpOpen";
        return response;
    }
    WinHttpSetTimeouts(hSession.get(), request.timeoutMs, request.timeoutMs,
                       request.timeoutMs, request.timeoutMs);

    ScopedWinHttpHandle hConnect(WinHttpConnect(hSession.get(), request.host.c_str(), request.port, 0));
    if (!hConnect) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpConnect";
        return response;
    }

    DWORD flags = request.useSsl ? WINHTTP_FLAG_SECURE : 0;
    ScopedWinHttpHandle hRequest(WinHttpOpenRequest(
        hConnect.get(),
        request.method.c_str(),
        request.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!hRequest) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpOpenRequest";
        return response;
    }
    if (!hRequest.RegisterCancellation(request.cancellation)) {
        response.winhttpError = ERROR_WINHTTP_OPERATION_CANCELLED;
        response.failedStep = L"request cancelled";
        return response;
    }
    WinHttpSetTimeouts(hRequest.get(), request.timeoutMs, request.timeoutMs,
                       request.timeoutMs, request.timeoutMs);

    void* bodyData = request.body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : const_cast<BYTE*>(request.body.data());
    if (request.body.size() >
        static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        response.winhttpError = ERROR_INSUFFICIENT_BUFFER;
        response.failedStep = L"request body too large";
        return response;
    }
    DWORD bodySize = static_cast<DWORD>((std::min)(
        request.body.size(),
        static_cast<size_t>((std::numeric_limits<DWORD>::max)())));

    if (!WinHttpSendRequest(hRequest.get(),
                            request.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : request.headers.c_str(),
                            request.headers.empty() ? 0 : static_cast<DWORD>(-1),
                            bodyData,
                            bodySize,
                            bodySize,
                            0)) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpSendRequest";
        return response;
    }

    if (!WinHttpReceiveResponse(hRequest.get(), nullptr)) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpReceiveResponse";
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest.get(),
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest.get(), &bytesAvailable)) {
            response.winhttpError = GetLastError();
            response.failedStep = L"WinHttpQueryDataAvailable";
            return response;
        }
        if (bytesAvailable == 0) break;

        if (response.body.size() > kMaxCloudHttpBodyBytes ||
            static_cast<size_t>(bytesAvailable) >
                kMaxCloudHttpBodyBytes - response.body.size()) {
            response.winhttpError = ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW;
            response.failedStep = L"response body too large";
            response.body.clear();
            return response;
        }

        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest.get(), chunk.data(), bytesAvailable, &bytesRead)) {
            response.winhttpError = GetLastError();
            response.failedStep = L"WinHttpReadData";
            return response;
        }
        response.body.append(chunk.data(), bytesRead);
    }

    response.ok = true;
    return response;
}

bool IsTransientCloudHttpError(DWORD error) {
    switch (error) {
    case ERROR_WINHTTP_TIMEOUT:
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR:
    case ERROR_WINHTTP_RESEND_REQUEST:
    case ERROR_WINHTTP_RESPONSE_DRAIN_OVERFLOW:
    case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
    case ERROR_WINHTTP_OPERATION_CANCELLED:
        return true;
    default:
        return false;
    }
}

bool IsRetryableCloudHttpStatus(DWORD statusCode) {
    return statusCode == 408 ||
           statusCode == 409 ||
           statusCode == 425 ||
           statusCode == 429 ||
           (statusCode >= 500 && statusCode <= 599);
}

void SleepCloudHttpRetryBackoff(int attemptIndex) {
    const DWORD delayMs = attemptIndex <= 0 ? 250 : 700;
    Sleep(delayMs);
}
