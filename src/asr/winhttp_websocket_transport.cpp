#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "winhttp_websocket_transport.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <limits>
#include <mutex>
#include <utility>

#pragma comment(lib, "winhttp.lib")

namespace winhttp_websocket {
namespace {

constexpr DWORD kHandleCloseWaitMs = 1000;
constexpr size_t kReceiveBufferBytes = 64u * 1024u;

std::wstring QueryHeader(HINTERNET request, const wchar_t* name) {
    if (!request || !name || !*name) return {};
    DWORD bytes = 0;
    WinHttpQueryHeaders(request,
                        WINHTTP_QUERY_CUSTOM,
                        name,
                        WINHTTP_NO_OUTPUT_BUFFER,
                        &bytes,
                        WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || bytes < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_CUSTOM,
                             name,
                             value.data(),
                             &bytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

} // namespace

struct Transport::Impl {
    mutable std::mutex mutex;
    std::condition_variable cv;
    std::mutex sendApiMutex;
    std::mutex receiveApiMutex;
    std::mutex closeMutex;

    HINTERNET session = nullptr;
    HINTERNET connect = nullptr;
    HINTERNET request = nullptr;
    HINTERNET websocket = nullptr;
    HINTERNET sessionIdentity = nullptr;
    HINTERNET requestIdentity = nullptr;
    HINTERNET websocketIdentity = nullptr;

    bool cancelled = false;
    bool connected = false;
    bool sessionClosed = false;
    DWORD secureFailureFlags = 0;

    bool handshakeDone = false;
    DWORD handshakeError = NO_ERROR;

    bool dataAvailableDone = false;
    DWORD dataAvailableError = NO_ERROR;
    DWORD dataAvailableBytes = 0;

    bool httpReadDone = false;
    DWORD httpReadError = NO_ERROR;
    DWORD httpReadBytes = 0;

    bool sendPending = false;
    bool sendDone = false;
    DWORD sendError = NO_ERROR;
    std::vector<BYTE> sendBuffer;

    bool receivePending = false;
    bool receiveDone = false;
    DWORD receiveError = NO_ERROR;
    DWORD receiveBytesOut = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE receiveTypeOut = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    WINHTTP_WEB_SOCKET_STATUS receiveStatus = {};
    std::vector<BYTE> receiveBuffer = std::vector<BYTE>(kReceiveBufferBytes);

    std::shared_ptr<Impl> selfKeepAlive;

    static void CALLBACK StatusCallback(HINTERNET handle,
                                        DWORD_PTR context,
                                        DWORD status,
                                        void* statusInfo,
                                        DWORD statusInfoLength) {
        auto* raw = reinterpret_cast<Impl*>(context);
        if (!raw) return;

        std::shared_ptr<Impl> keepAlive;
        {
            std::lock_guard<std::mutex> lock(raw->mutex);
            keepAlive = raw->selfKeepAlive;
        }
        if (!keepAlive) return;

        bool releaseSelf = false;
        {
            std::lock_guard<std::mutex> lock(raw->mutex);
            switch (status) {
            case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
                raw->handshakeError = NO_ERROR;
                raw->handshakeDone = true;
                break;

            case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE:
                raw->dataAvailableError = NO_ERROR;
                raw->dataAvailableBytes = statusInfo && statusInfoLength >= sizeof(DWORD)
                    ? *static_cast<DWORD*>(statusInfo)
                    : 0;
                raw->dataAvailableDone = true;
                break;

            case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
                if (handle == raw->websocketIdentity) {
                    raw->receiveError = NO_ERROR;
                    if (statusInfo && statusInfoLength >= sizeof(WINHTTP_WEB_SOCKET_STATUS)) {
                        raw->receiveStatus = *static_cast<WINHTTP_WEB_SOCKET_STATUS*>(statusInfo);
                    } else {
                        raw->receiveStatus.dwBytesTransferred = raw->receiveBytesOut;
                        raw->receiveStatus.eBufferType = raw->receiveTypeOut;
                    }
                    raw->receiveDone = true;
                } else if (handle == raw->requestIdentity) {
                    raw->httpReadError = NO_ERROR;
                    raw->httpReadBytes = statusInfoLength;
                    raw->httpReadDone = true;
                }
                break;

            case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE:
                if (handle == raw->websocketIdentity) {
                    raw->sendError = NO_ERROR;
                    raw->sendDone = true;
                }
                break;

            case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR:
                if (statusInfo && statusInfoLength >= sizeof(WINHTTP_ASYNC_RESULT)) {
                    const auto* asyncResult = static_cast<WINHTTP_ASYNC_RESULT*>(statusInfo);
                    const DWORD error = asyncResult->dwError;
                    if (handle == raw->websocketIdentity &&
                        statusInfoLength >= sizeof(WINHTTP_WEB_SOCKET_ASYNC_RESULT)) {
                        const auto* websocketResult =
                            static_cast<WINHTTP_WEB_SOCKET_ASYNC_RESULT*>(statusInfo);
                        if (websocketResult->Operation == WINHTTP_WEB_SOCKET_SEND_OPERATION) {
                            raw->sendError = error;
                            raw->sendDone = true;
                        } else if (websocketResult->Operation == WINHTTP_WEB_SOCKET_RECEIVE_OPERATION) {
                            raw->receiveError = error;
                            raw->receiveDone = true;
                        }
                    } else if (handle == raw->requestIdentity) {
                        if (!raw->handshakeDone) {
                            raw->handshakeError = error;
                            raw->handshakeDone = true;
                        }
                        if (!raw->dataAvailableDone) {
                            raw->dataAvailableError = error;
                            raw->dataAvailableDone = true;
                        }
                        if (!raw->httpReadDone) {
                            raw->httpReadError = error;
                            raw->httpReadDone = true;
                        }
                    }
                }
                break;

            case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE:
                if (statusInfo && statusInfoLength >= sizeof(DWORD)) {
                    raw->secureFailureFlags = *static_cast<DWORD*>(statusInfo);
                }
                break;

            case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING:
                if (handle == raw->sessionIdentity) {
                    raw->sessionClosed = true;
                    releaseSelf = true;
                }
                break;

            default:
                break;
            }
            raw->cv.notify_all();
            if (releaseSelf) {
                raw->selfKeepAlive.reset();
            }
        }
    }

    bool SetContext(HINTERNET handle) {
        DWORD_PTR context = reinterpret_cast<DWORD_PTR>(this);
        return handle && WinHttpSetOption(handle,
                                          WINHTTP_OPTION_CONTEXT_VALUE,
                                          &context,
                                          sizeof(context));
    }

    bool WaitFor(std::unique_lock<std::mutex>& lock,
                 DWORD timeoutMs,
                 const bool& flag) {
        if (flag) return true;
        return cv.wait_for(lock,
                           std::chrono::milliseconds((std::max<DWORD>)(timeoutMs, 1)),
                           [&]() { return flag || cancelled; });
    }

    void CloseHandles(bool markCancelled) {
        HINTERNET localWebsocket = nullptr;
        HINTERNET localRequest = nullptr;
        HINTERNET localConnect = nullptr;
        HINTERNET localSession = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (markCancelled) cancelled = true;
            connected = false;
            localWebsocket = std::exchange(websocket, nullptr);
            localRequest = std::exchange(request, nullptr);
            localConnect = std::exchange(connect, nullptr);
            localSession = std::exchange(session, nullptr);
            cv.notify_all();
        }

        // Handles were opened in WINHTTP_FLAG_ASYNC mode. Closing them is the
        // documented cancellation mechanism for outstanding asynchronous I/O.
        if (localWebsocket) WinHttpCloseHandle(localWebsocket);
        if (localRequest) WinHttpCloseHandle(localRequest);
        if (localConnect) WinHttpCloseHandle(localConnect);
        if (localSession) WinHttpCloseHandle(localSession);

        std::unique_lock<std::mutex> lock(mutex);
        if (!localSession) {
            selfKeepAlive.reset();
            return;
        }
        cv.wait_for(lock,
                    std::chrono::milliseconds(kHandleCloseWaitMs),
                    [&]() { return sessionClosed; });
        // If HANDLE_CLOSING is delayed, selfKeepAlive intentionally keeps the
        // callback context valid until WinHTTP delivers the terminal callback.
    }

    void AbortAndClose(bool markCancelled = true) {
        std::lock_guard<std::mutex> closeLock(closeMutex);
        CloseHandles(markCancelled);
    }

    bool BeginHandshakeWait() {
        std::lock_guard<std::mutex> lock(mutex);
        handshakeDone = false;
        handshakeError = NO_ERROR;
        return !cancelled;
    }

    bool CompleteHandshakeCall(BOOL callAccepted, DWORD callError, DWORD timeoutMs,
                               std::wstring& error) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            // In asynchronous WinHTTP mode, TRUE means the operation was
            // accepted, not that its completion callback has already run.
            // Advancing early can overlap SendRequest/ReceiveResponse and can
            // release buffers before WinHTTP is finished with them.
            if (!callAccepted && callError != ERROR_IO_PENDING) {
                handshakeDone = true;
                handshakeError = callError;
                cv.notify_all();
            }
        }
        std::unique_lock<std::mutex> lock(mutex);
        const bool signaled = WaitFor(lock, timeoutMs, handshakeDone);
        if (!signaled || (!handshakeDone && !cancelled)) {
            error = L"WebSocket handshake timed out";
            return false;
        }
        if (cancelled) {
            error = L"WebSocket handshake cancelled";
            return false;
        }
        if (handshakeError != NO_ERROR) {
            error = L"WebSocket handshake failed: " + FormatWinHttpError(handshakeError);
            return false;
        }
        return true;
    }

    std::string ReadResponseSnippet(HINTERNET requestHandle, size_t maxBytes) {
        std::string body;
        while (body.size() < maxBytes && !cancelled) {
            DWORD availableOut = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                dataAvailableDone = false;
                dataAvailableError = NO_ERROR;
                dataAvailableBytes = 0;
            }
            const BOOL queryOk = WinHttpQueryDataAvailable(requestHandle, &availableOut);
            const DWORD queryError = queryOk ? NO_ERROR : GetLastError();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!queryOk && queryError != ERROR_IO_PENDING) {
                    dataAvailableError = queryError;
                    dataAvailableDone = true;
                    cv.notify_all();
                }
            }
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (!WaitFor(lock, 1000, dataAvailableDone) || cancelled ||
                    dataAvailableError != NO_ERROR) {
                    break;
                }
                availableOut = dataAvailableBytes;
            }
            if (availableOut == 0) break;

            const DWORD remaining = static_cast<DWORD>((std::min)(
                maxBytes - body.size(),
                static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
            const DWORD toRead = (std::min)(availableOut, remaining);
            if (toRead == 0) break;
            std::vector<char> buffer(toRead);
            DWORD bytesOut = 0;
            {
                std::lock_guard<std::mutex> lock(mutex);
                httpReadDone = false;
                httpReadError = NO_ERROR;
                httpReadBytes = 0;
            }
            const BOOL readOk = WinHttpReadData(requestHandle, buffer.data(), toRead, &bytesOut);
            const DWORD readError = readOk ? NO_ERROR : GetLastError();
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!readOk && readError != ERROR_IO_PENDING) {
                    httpReadError = readError;
                    httpReadDone = true;
                    cv.notify_all();
                }
            }
            {
                std::unique_lock<std::mutex> lock(mutex);
                if (!WaitFor(lock, 1000, httpReadDone) || cancelled ||
                    httpReadError != NO_ERROR) {
                    break;
                }
                bytesOut = httpReadBytes;
            }
            if (bytesOut == 0) break;
            body.append(buffer.data(), buffer.data() + (std::min)(bytesOut, toRead));
        }
        return body;
    }
};

Transport::Transport() : impl_(std::make_shared<Impl>()) {}

Transport::~Transport() {
    Close();
}

std::shared_ptr<Transport::Impl> Transport::SnapshotImpl() const {
    std::lock_guard<std::mutex> lock(implMutex_);
    return impl_;
}

bool Transport::Connect(const ConnectOptions& options,
                        HandshakeDiagnostics& diagnostics,
                        std::wstring& error) {
    diagnostics = {};
    error.clear();

    // Each connection attempt gets a fresh callback context. This gives
    // Connect and Abort a clear linearization point: an Abort after the swap
    // always targets this attempt, while callbacks from the previous attempt
    // remain owned by their old state until HANDLE_CLOSING.
    auto state = std::make_shared<Impl>();
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->selfKeepAlive = state;
    }
    std::shared_ptr<Impl> previous;
    {
        std::lock_guard<std::mutex> lock(implMutex_);
        previous = std::exchange(impl_, state);
    }
    if (previous) previous->AbortAndClose();

    HINTERNET session = nullptr;
    {
        std::lock_guard<std::mutex> closeLock(state->closeMutex);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled) {
                error = L"WebSocket handshake cancelled";
                state->selfKeepAlive.reset();
                return false;
            }
        }
        session = WinHttpOpen(
            options.userAgent.c_str(),
            options.disableProxy
                ? WINHTTP_ACCESS_TYPE_NO_PROXY
                : WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            WINHTTP_FLAG_ASYNC);
        if (!session) {
            error = L"WinHttpOpen failed: " + FormatWinHttpError(GetLastError());
            std::lock_guard<std::mutex> lock(state->mutex);
            state->selfKeepAlive.reset();
            return false;
        }
        const auto callback = WinHttpSetStatusCallback(
            session,
            &Impl::StatusCallback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS |
                WINHTTP_CALLBACK_FLAG_HANDLES |
                WINHTTP_CALLBACK_FLAG_SECURE_FAILURE,
            0);
        if (callback == WINHTTP_INVALID_STATUS_CALLBACK || !state->SetContext(session)) {
            error = L"failed to initialize asynchronous WinHTTP callback";
            WinHttpCloseHandle(session);
            std::lock_guard<std::mutex> lock(state->mutex);
            state->selfKeepAlive.reset();
            return false;
        }
        WinHttpSetTimeouts(session,
                           options.timeoutMs,
                           options.timeoutMs,
                           options.timeoutMs,
                           options.timeoutMs);
        std::lock_guard<std::mutex> lock(state->mutex);
        state->session = session;
        state->sessionIdentity = session;
    }

    HINTERNET connect = nullptr;
    {
        std::lock_guard<std::mutex> closeLock(state->closeMutex);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled || state->session != session) {
                error = L"WebSocket handshake cancelled";
                return false;
            }
        }
        connect = WinHttpConnect(session, options.host.c_str(), options.port, 0);
        if (connect) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->connect = connect;
        }
    }
    if (!connect) {
        error = L"WinHttpConnect failed: " + FormatWinHttpError(GetLastError());
        state->AbortAndClose();
        return false;
    }

    const DWORD requestFlags = options.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET request = nullptr;
    {
        std::lock_guard<std::mutex> closeLock(state->closeMutex);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled || state->connect != connect) {
                error = L"WebSocket handshake cancelled";
                return false;
            }
        }
        request = WinHttpOpenRequest(
            connect,
            L"GET",
            options.pathAndQuery.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            requestFlags);
        if (request) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->request = request;
            state->requestIdentity = request;
        }
    }
    if (!request) {
        error = L"WinHttpOpenRequest failed: " + FormatWinHttpError(GetLastError());
        state->AbortAndClose();
        return false;
    }
    {
        std::lock_guard<std::mutex> closeLock(state->closeMutex);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled || state->request != request) {
                error = L"WebSocket handshake cancelled";
                return false;
            }
        }
        if (!state->SetContext(request) ||
            !WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
            error = L"failed to configure WebSocket upgrade: " +
                FormatWinHttpError(GetLastError());
        } else {
            if (options.closeTimeoutMs > 0) {
                DWORD closeTimeout = options.closeTimeoutMs;
                WinHttpSetOption(request,
                                 WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
                                 &closeTimeout,
                                 sizeof(closeTimeout));
            }
            if (options.keepAliveMs >= WINHTTP_WEB_SOCKET_MIN_KEEPALIVE_VALUE) {
                DWORD keepAlive = options.keepAliveMs;
                WinHttpSetOption(request,
                                 WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
                                 &keepAlive,
                                 sizeof(keepAlive));
            }
            if (!options.headers.empty() &&
                !WinHttpAddRequestHeaders(request,
                                          options.headers.c_str(),
                                          static_cast<DWORD>(-1),
                                          WINHTTP_ADDREQ_FLAG_ADD)) {
                error = L"WinHttpAddRequestHeaders failed: " +
                    FormatWinHttpError(GetLastError());
            }
        }
    }
    if (!error.empty()) {
        state->AbortAndClose();
        return false;
    }

    state->BeginHandshakeWait();
    const BOOL sendOk = WinHttpSendRequest(request,
                                           WINHTTP_NO_ADDITIONAL_HEADERS,
                                           0,
                                           WINHTTP_NO_REQUEST_DATA,
                                           0,
                                           0,
                                           0);
    const DWORD sendError = sendOk ? NO_ERROR : GetLastError();
    if (!state->CompleteHandshakeCall(sendOk, sendError, options.timeoutMs, error)) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            diagnostics.secureFailureFlags = state->secureFailureFlags;
        }
        state->AbortAndClose();
        return false;
    }

    state->BeginHandshakeWait();
    const BOOL responseOk = WinHttpReceiveResponse(request, nullptr);
    const DWORD responseError = responseOk ? NO_ERROR : GetLastError();
    if (!state->CompleteHandshakeCall(responseOk, responseError, options.timeoutMs, error)) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            diagnostics.secureFailureFlags = state->secureFailureFlags;
        }
        state->AbortAndClose();
        return false;
    }

    DWORD statusBytes = sizeof(diagnostics.statusCode);
    if (!WinHttpQueryHeaders(request,
                             WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX,
                             &diagnostics.statusCode,
                             &statusBytes,
                             WINHTTP_NO_HEADER_INDEX)) {
        error = L"WinHttpQueryHeaders failed: " + FormatWinHttpError(GetLastError());
        state->AbortAndClose();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        diagnostics.secureFailureFlags = state->secureFailureFlags;
    }
    diagnostics.requestId = QueryHeader(request, L"x-dashscope-request-id");
    if (diagnostics.requestId.empty()) diagnostics.requestId = QueryHeader(request, L"x-request-id");
    diagnostics.traceId = QueryHeader(request, L"x-trace-id");
    if (diagnostics.traceId.empty()) diagnostics.traceId = QueryHeader(request, L"trace-id");

    if (diagnostics.statusCode != 101) {
        diagnostics.responseBody = SanitizeResponseSnippet(
            state->ReadResponseSnippet(request, 1024));
        error = L"WebSocket upgrade failed (HTTP " +
            std::to_wstring(diagnostics.statusCode) + L")";
        state->AbortAndClose();
        return false;
    }

    HINTERNET websocket = nullptr;
    {
        std::lock_guard<std::mutex> closeLock(state->closeMutex);
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->cancelled || state->request != request) {
                error = L"WebSocket handshake cancelled";
                return false;
            }
        }
        websocket = WinHttpWebSocketCompleteUpgrade(
            request,
            reinterpret_cast<DWORD_PTR>(state.get()));
        if (websocket) {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->websocket = websocket;
            state->websocketIdentity = websocket;
            state->connected = true;
            state->request = nullptr;
        }
    }
    if (!websocket) {
        error = L"WinHttpWebSocketCompleteUpgrade failed: " +
            FormatWinHttpError(GetLastError());
        state->AbortAndClose();
        return false;
    }
    WinHttpCloseHandle(request);
    return true;
}

bool Transport::Send(WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType,
                     const void* data,
                     size_t bytes,
                     DWORD timeoutMs,
                     DWORD& winhttpError,
                     std::wstring& error) {
    winhttpError = NO_ERROR;
    error.clear();
    if (bytes > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        winhttpError = ERROR_INSUFFICIENT_BUFFER;
        error = L"WebSocket send payload is too large";
        return false;
    }
    auto state = SnapshotImpl();
    std::lock_guard<std::mutex> apiLock(state->sendApiMutex);

    HINTERNET websocket = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelled || !state->connected || !state->websocket) {
            winhttpError = ERROR_WINHTTP_OPERATION_CANCELLED;
            error = L"WebSocket operation cancelled";
            return false;
        }
        state->sendBuffer.assign(static_cast<const BYTE*>(data),
                                 static_cast<const BYTE*>(data) + bytes);
        state->sendPending = true;
        state->sendDone = false;
        state->sendError = NO_ERROR;
        websocket = state->websocket;
    }

    DWORD callError = WinHttpWebSocketSend(
        websocket,
        bufferType,
        state->sendBuffer.empty() ? nullptr : state->sendBuffer.data(),
        static_cast<DWORD>(state->sendBuffer.size()));
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        // With an asynchronous session, NO_ERROR means WinHTTP accepted the
        // operation. The send buffer must remain valid until WRITE_COMPLETE.
        if (callError != NO_ERROR && callError != ERROR_IO_PENDING) {
            state->sendError = callError;
            state->sendDone = true;
        }
        state->cv.notify_all();
    }

    bool timedOut = false;
    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->WaitFor(lock, timeoutMs, state->sendDone) ||
            (!state->sendDone && !state->cancelled)) {
            timedOut = true;
        }
        completed = state->sendDone;
        winhttpError = timedOut ? ERROR_WINHTTP_TIMEOUT : state->sendError;
        if (completed) {
            state->sendPending = false;
            state->sendBuffer.clear();
        }
        if (!timedOut && state->cancelled && !completed) {
            winhttpError = ERROR_WINHTTP_OPERATION_CANCELLED;
        }
    }
    if (timedOut) {
        error = L"WebSocket send timed out";
        state->AbortAndClose();
        return false;
    }
    if (winhttpError != NO_ERROR) {
        error = L"WebSocket send failed: " + FormatWinHttpError(winhttpError);
        return false;
    }
    return true;
}

ReceiveResult Transport::Receive(DWORD waitMs) {
    auto state = SnapshotImpl();
    std::lock_guard<std::mutex> apiLock(state->receiveApiMutex);
    ReceiveResult result;

    HINTERNET websocket = nullptr;
    bool startReceive = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->cancelled || !state->connected || !state->websocket) {
            result.kind = state->cancelled ? ReceiveKind::Cancelled : ReceiveKind::Error;
            result.winhttpError = state->cancelled
                ? ERROR_WINHTTP_OPERATION_CANCELLED
                : ERROR_WINHTTP_INCORRECT_HANDLE_STATE;
            return result;
        }
        websocket = state->websocket;
        if (!state->receivePending) {
            state->receivePending = true;
            state->receiveDone = false;
            state->receiveError = NO_ERROR;
            state->receiveBytesOut = 0;
            state->receiveTypeOut = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
            state->receiveStatus = {};
            startReceive = true;
        }
    }

    if (startReceive) {
        const DWORD callError = WinHttpWebSocketReceive(
            websocket,
            state->receiveBuffer.data(),
            static_cast<DWORD>(state->receiveBuffer.size()),
            &state->receiveBytesOut,
            &state->receiveTypeOut);
        std::lock_guard<std::mutex> lock(state->mutex);
        // Async receive completion data is delivered in READ_COMPLETE as a
        // WINHTTP_WEB_SOCKET_STATUS structure. The output pointers above are
        // only meaningful for a truly synchronous handle.
        if (callError != NO_ERROR && callError != ERROR_IO_PENDING) {
            state->receiveError = callError;
            state->receiveDone = true;
        }
        state->cv.notify_all();
    }

    {
        std::unique_lock<std::mutex> lock(state->mutex);
        const bool signaled = state->WaitFor(lock, waitMs, state->receiveDone);
        if ((!signaled || (!state->receiveDone && !state->cancelled))) {
            result.kind = ReceiveKind::Timeout;
            result.winhttpError = ERROR_WINHTTP_TIMEOUT;
            return result;
        }
        if (state->cancelled && !state->receiveDone) {
            result.kind = ReceiveKind::Cancelled;
            result.winhttpError = ERROR_WINHTTP_OPERATION_CANCELLED;
            return result;
        }

        result.winhttpError = state->receiveError;
        result.bufferType = state->receiveStatus.eBufferType;
        const DWORD bytes = (std::min<DWORD>)(
            state->receiveStatus.dwBytesTransferred,
            static_cast<DWORD>(state->receiveBuffer.size()));
        if (bytes > 0) {
            result.data.assign(state->receiveBuffer.begin(),
                               state->receiveBuffer.begin() + bytes);
        }
        state->receivePending = false;
        state->receiveDone = false;
    }

    if (result.winhttpError != NO_ERROR) {
        result.kind = result.winhttpError == ERROR_WINHTTP_OPERATION_CANCELLED
            ? ReceiveKind::Cancelled
            : ReceiveKind::Error;
        return result;
    }
    if (result.bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        result.kind = ReceiveKind::PeerClosed;
        BYTE reason[WINHTTP_WEB_SOCKET_MAX_CLOSE_REASON_LENGTH] = {};
        DWORD reasonBytes = 0;
        USHORT status = WINHTTP_WEB_SOCKET_EMPTY_CLOSE_STATUS;
        const DWORD queryError = WinHttpWebSocketQueryCloseStatus(
            websocket,
            &status,
            reason,
            sizeof(reason),
            &reasonBytes);
        if (queryError == NO_ERROR) {
            result.closeStatus = status;
            if (reasonBytes > 0) {
                const std::string utf8(reinterpret_cast<char*>(reason), reasonBytes);
                const int chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                      utf8.data(),
                                                      static_cast<int>(utf8.size()),
                                                      nullptr, 0);
                if (chars > 0) {
                    result.closeReason.resize(chars);
                    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                        utf8.data(),
                                        static_cast<int>(utf8.size()),
                                        result.closeReason.data(), chars);
                }
            }
        }
        return result;
    }
    result.kind = ReceiveKind::Message;
    return result;
}

void Transport::Abort() {
    SnapshotImpl()->AbortAndClose();
}

void Transport::Close() {
    Abort();
}

bool Transport::IsConnected() const {
    auto state = SnapshotImpl();
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->connected && !state->cancelled && state->websocket != nullptr;
}

std::wstring FormatWinHttpError(DWORD error) {
    if (error == NO_ERROR) return L"success";
    wchar_t* message = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER |
                        FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD chars = FormatMessageW(
        flags,
        nullptr,
        error,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::wstring text = chars && message ? std::wstring(message, chars) : L"unknown error";
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' ||
                             text.back() == L' ' || text.back() == L'\t')) {
        text.pop_back();
    }
    return L"error=" + std::to_wstring(error) + L" (" + text + L")";
}

std::string SanitizeResponseSnippet(const std::string& body, size_t maxBytes) {
    std::string result;
    result.reserve((std::min)(body.size(), maxBytes));
    for (unsigned char ch : body) {
        if (result.size() >= maxBytes) break;
        if (ch == '\r' || ch == '\n' || ch == '\t') {
            result.push_back(' ');
        } else if (std::isprint(ch) || ch >= 0x80) {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('?');
        }
    }
    return result;
}

} // namespace winhttp_websocket
