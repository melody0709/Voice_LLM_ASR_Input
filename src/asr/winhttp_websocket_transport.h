#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace winhttp_websocket {

struct ConnectOptions {
    std::wstring userAgent = L"VoxType/1.0";
    std::wstring host;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    std::wstring pathAndQuery;
    std::wstring headers;
    bool secure = true;
    bool disableProxy = false;
    DWORD timeoutMs = 8000;
    DWORD closeTimeoutMs = 1000;
    DWORD keepAliveMs = 30000;
};

struct HandshakeDiagnostics {
    DWORD statusCode = 0;
    DWORD secureFailureFlags = 0;
    std::wstring requestId;
    std::wstring traceId;
    std::string responseBody;
};

enum class ReceiveKind {
    Message,
    Timeout,
    PeerClosed,
    Cancelled,
    Error,
};

struct ReceiveResult {
    ReceiveKind kind = ReceiveKind::Error;
    std::vector<BYTE> data;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    DWORD winhttpError = NO_ERROR;
    USHORT closeStatus = WINHTTP_WEB_SOCKET_EMPTY_CLOSE_STATUS;
    std::wstring closeReason;
};

// Thin asynchronous WinHTTP WebSocket owner. Protocol-specific JSON and event
// state remain in each provider client; this class only owns handle lifetime,
// bounded waits, duplex send/receive, cancellation, and handshake diagnostics.
class Transport {
public:
    Transport();
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    bool Connect(const ConnectOptions& options,
                 HandshakeDiagnostics& diagnostics,
                 std::wstring& error);
    bool Send(WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType,
              const void* data,
              size_t bytes,
              DWORD timeoutMs,
              DWORD& winhttpError,
              std::wstring& error);
    ReceiveResult Receive(DWORD waitMs);

    // Abort is safe with an outstanding asynchronous send/receive. Closing an
    // async WinHTTP handle cancels the operation and completes it through the
    // registered callback instead of racing a synchronous API call.
    void Abort();
    void Close();
    bool IsConnected() const;

private:
    struct Impl;
    std::shared_ptr<Impl> SnapshotImpl() const;

    mutable std::mutex implMutex_;
    std::shared_ptr<Impl> impl_;
};

std::wstring FormatWinHttpError(DWORD error);
std::string SanitizeResponseSnippet(const std::string& body, size_t maxBytes = 1024);

} // namespace winhttp_websocket
