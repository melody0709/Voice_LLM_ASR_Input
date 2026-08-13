#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "qwen_asr.h"

#include "asr_runtime_log.h"
#include "qwen_audio_json.h"
#include "utils.h"
#include "winhttp_websocket_transport.h"

#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

namespace qwen_asr {
namespace {

constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSessionReadyTimeoutMs = 5000;
constexpr DWORD kSendTimeoutMs = 5000;
constexpr size_t kPcmBytesPerMs = 32; // 16kHz, mono, s16le.

void QwenRealtimeDebugLog(const char* format, ...) {
#if defined(VOXTYPE_QWEN_AUDIO_PROTOCOL_TEST)
    (void)format;
    return;
#else
    if (!format) return;
    va_list args;
    va_start(args, format);
    asr_runtime_log::WriteNamedV(L"qwen_audio_debug.log", format, args);
    va_end(args);
#endif
}

struct ParsedUrl {
    std::wstring host;
    std::wstring pathAndQuery;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

struct MessageFrame {
    std::wstring type;
    std::wstring sessionId;
    std::wstring partialText;
    std::wstring finalText;
    bool transcriptionCompleted = false;
    bool sessionFinished = false;
};

std::wstring MakeError(const std::wstring& detail) {
    return L"Qwen ASR error: " + detail;
}

std::wstring ResolveBaseUrl(const std::wstring& value) {
    return value.empty() ? std::wstring(kDefaultBaseUrl) : value;
}

std::wstring ResolveModel(const std::wstring& value) {
    return value.empty() ? std::wstring(kDefaultModel) : value;
}

std::wstring NormalizeLanguage(const std::wstring& value) {
    if (value.empty() || value == L"auto") return L"";
    size_t dash = value.find(L'-');
    return dash == std::wstring::npos ? value : value.substr(0, dash);
}

std::wstring NormalizeTurnDetection(const std::wstring& value) {
    if (value == L"server_vad") return L"server_vad";
    return L"manual";
}

std::wstring LowerCase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

std::wstring BuildEndpointUrl(const QwenConfig& cfg) {
    std::wstring url = ResolveBaseUrl(cfg.baseUrl);
    if (url.find(L"model=") != std::wstring::npos) return url;
    url += (url.find(L'?') == std::wstring::npos) ? L"?model=" : L"&model=";
    url += ResolveModel(cfg.model);
    return url;
}

bool StartsWith(const std::wstring& value, const wchar_t* prefix) {
    return value.rfind(prefix, 0) == 0;
}

bool ParseWebSocketUrl(const std::wstring& url, ParsedUrl& parsed, std::wstring& error) {
    std::wstring normalized = url;
    parsed.secure = true;

    if (!StartsWith(normalized, L"wss://")) {
        error = L"Qwen3 Realtime endpoint must use wss://";
        return false;
    }
    normalized.replace(0, 6, L"https://");

    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(normalized.c_str(), 0, 0, &parts)) {
        error = L"failed to parse WebSocket URL (err=" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    parsed.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    const std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (path != L"/api-ws/v1/realtime") {
        error = L"Qwen3 Realtime endpoint path must be /api-ws/v1/realtime";
        return false;
    }
    parsed.pathAndQuery.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.lpszExtraInfo && parts.dwExtraInfoLength > 0) {
        parsed.pathAndQuery.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    }
    if (parsed.pathAndQuery.empty()) parsed.pathAndQuery = L"/";

    parsed.port = parts.nPort;
    if (parsed.port == 0) {
        parsed.port = parsed.secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
    }

    if (parsed.host.empty()) {
        error = L"WebSocket URL missing host";
        return false;
    }

    return true;
}

std::string GenerateEventId() {
    wchar_t buf[64] = {};
    swprintf_s(buf, L"evt_%08lx_%08lx_%08llx",
               GetCurrentProcessId(),
               GetCurrentThreadId(),
               GetTickCount64());
    return WideToUtf8(buf);
}

bool Base64Encode(const BYTE* data, DWORD bytes, std::string& out) {
    out.clear();
    if (!data || bytes == 0) return true;

    DWORD required = 0;
    if (!CryptBinaryToStringA(data, bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &required)) {
        return false;
    }

    std::string encoded(required, '\0');
    if (!CryptBinaryToStringA(data, bytes, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              encoded.data(), &required)) {
        return false;
    }
    if (required > 0 && encoded[required - 1] == '\0') {
        encoded.resize(required - 1);
    } else {
        encoded.resize(required);
    }
    out = std::move(encoded);
    return true;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadHex4(const std::string& value, size_t pos, uint32_t& out) {
    if (pos + 4 > value.size()) return false;
    uint32_t cp = 0;
    for (size_t i = 0; i < 4; ++i) {
        int v = HexValue(value[pos + i]);
        if (v < 0) return false;
        cp = (cp << 4) | static_cast<uint32_t>(v);
    }
    out = cp;
    return true;
}

std::wstring ExtractJsonStringValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return L"";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return L"";
    ++pos;

    std::string unescaped;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') break;
        if (c != '\\') {
            unescaped.push_back(c);
            continue;
        }
        if (pos >= json.size()) break;

        char esc = json[pos++];
        switch (esc) {
        case '"': unescaped.push_back('"'); break;
        case '\\': unescaped.push_back('\\'); break;
        case '/': unescaped.push_back('/'); break;
        case 'b': unescaped.push_back('\b'); break;
        case 'f': unescaped.push_back('\f'); break;
        case 'n': unescaped.push_back('\n'); break;
        case 'r': unescaped.push_back('\r'); break;
        case 't': unescaped.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!ReadHex4(json, pos, cp)) break;
            pos += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                pos + 6 <= json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
                uint32_t low = 0;
                if (ReadHex4(json, pos + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                    pos += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
            }
            qwen_audio_json::AppendUtf8(unescaped, cp);
            break;
        }
        default:
            unescaped.push_back(esc);
            break;
        }
    }

    return Utf8ToWide(unescaped);
}

std::wstring ExtractJsonNumberLikeString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return L"";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' && json[pos] != '\r' && json[pos] != '\n') {
        ++pos;
    }
    return Utf8ToWide(json.substr(start, pos - start));
}

std::wstring RealtimeErrorMessage(const std::string& json, const wchar_t* fallback) {
    std::wstring code = ExtractJsonStringValue(json, "code");
    std::wstring message = ExtractJsonStringValue(json, "message");
    if (message.empty()) message = fallback;
    if (!code.empty()) message += L" (" + code + L")";
    return message;
}

std::wstring ManualTurnDetectionJson(const QwenConfig& cfg) {
    if (LowerCase(NormalizeTurnDetection(cfg.turnDetection)) != L"server_vad") {
        return L"null";
    }

    std::wostringstream oss;
    oss << L"{\"type\":\"server_vad\",\"threshold\":";
    oss << std::fixed << std::setprecision(2) << std::max(0.0f, std::min(cfg.vadThreshold, 1.0f));
    oss << L",\"silence_duration_ms\":" << std::clamp(cfg.vadSilenceMs, 200, 6000) << L"}";
    return oss.str();
}

MessageFrame ParseMessageFrame(const std::string& message) {
    MessageFrame frame;
    frame.type = ExtractJsonStringValue(message, "type");
    if (frame.type == L"session.created" || frame.type == L"session.updated") {
        frame.sessionId = ExtractJsonStringValue(message, "id");
    } else if (frame.type == L"conversation.item.input_audio_transcription.text") {
        std::wstring confirmed = ExtractJsonStringValue(message, "text");
        std::wstring stash = ExtractJsonStringValue(message, "stash");
        frame.partialText = confirmed + stash;
    } else if (frame.type == L"conversation.item.input_audio_transcription.completed") {
        frame.finalText = ExtractJsonStringValue(message, "transcript");
        frame.transcriptionCompleted = true;
    } else if (frame.type == L"session.finished") {
        frame.sessionFinished = true;
    }
    return frame;
}

bool SendTextMessage(winhttp_websocket::Transport& transport,
                     const std::string& message,
                     std::wstring& error) {
    DWORD winhttpError = NO_ERROR;
    return transport.Send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                          message.data(),
                          message.size(),
                          kSendTimeoutMs,
                          winhttpError,
                          error);
}

std::string BuildSimpleEventMessage(const char* type) {
    return "{\"type\":\"" + std::string(type) +
        "\",\"event_id\":\"" + GenerateEventId() + "\"}";
}

std::string BuildAudioAppendMessage(const BYTE* data, DWORD bytes, std::wstring& error) {
    std::string base64;
    if (!Base64Encode(data, bytes, base64)) {
        error = L"failed to base64-encode audio";
        return {};
    }

    return "{\"type\":\"input_audio_buffer.append\",\"event_id\":\"" + GenerateEventId() +
        "\",\"audio\":\"" + base64 + "\"}";
}

bool SendBinaryChunk(winhttp_websocket::Transport& transport,
                     const BYTE* data,
                     DWORD bytes,
                     std::wstring& error) {
    const std::string message = BuildAudioAppendMessage(data, bytes, error);
    if (!error.empty()) return false;
    return SendTextMessage(transport, message, error);
}

std::string BuildSessionUpdateMessage(const QwenConfig& cfg) {
    std::wstring lang = NormalizeLanguage(cfg.language);
    std::wstring turnDetection = ManualTurnDetectionJson(cfg);

    std::ostringstream oss;
    oss << "{\"type\":\"session.update\",\"event_id\":\"" << GenerateEventId() << "\",\"session\":{";
    oss << "\"modalities\":[\"text\"],";
    oss << "\"input_audio_format\":\"pcm\",";
    oss << "\"sample_rate\":16000,";
    if (!lang.empty()) {
        oss << "\"input_audio_transcription\":{\"language\":\""
            << EscapeJson(lang) << "\"},";
    }
    oss << "\"turn_detection\":" << WideToUtf8(turnDetection);
    oss << "}}";
    return oss.str();
}

size_t ClampChunkBytes(size_t bytes) {
    const size_t minBytes = 320;   // 10ms, keep it sane
    const size_t maxBytes = 32000; // 1s
    return std::clamp(bytes, minBytes, maxBytes);
}

enum class ReceiveStatus {
    Message,
    Timeout,
    PeerClosed,
    Cancelled,
    Error,
};

ReceiveStatus ParseReceiveMessage(winhttp_websocket::Transport& transport,
                                  DWORD timeoutMs,
                                  std::string& message,
                                  std::wstring& error,
                                  USHORT* closeStatus = nullptr,
                                  std::wstring* closeReason = nullptr) {
    message.clear();
    std::string assembled;

    while (true) {
        const auto received = transport.Receive(timeoutMs);
        if (received.kind == winhttp_websocket::ReceiveKind::Timeout) {
            return ReceiveStatus::Timeout;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::Cancelled) {
            error = L"WebSocket operation cancelled";
            return ReceiveStatus::Cancelled;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::Error) {
            error = L"WebSocket receive failed: " +
                winhttp_websocket::FormatWinHttpError(received.winhttpError);
            return ReceiveStatus::Error;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::PeerClosed) {
            message.clear();
            error = L"WebSocket peer closed before session.finished";
            if (closeStatus) *closeStatus = received.closeStatus;
            if (closeReason) *closeReason = received.closeReason;
            return ReceiveStatus::PeerClosed;
        }
        if (!received.data.empty()) {
            assembled.append(reinterpret_cast<const char*>(received.data.data()),
                             received.data.size());
        }
        if (received.bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            received.bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            message = std::move(assembled);
            return ReceiveStatus::Message;
        }
    }
}

bool ConnectInternal(winhttp_websocket::Transport& transport,
                     const QwenConfig& cfg,
                     std::wstring& error,
                     bool& retryable) {
    error.clear();
    retryable = true;
    ParsedUrl parsed;
    std::wstring endpoint = BuildEndpointUrl(cfg);
    if (!ParseWebSocketUrl(endpoint, parsed, error)) {
        retryable = false;
        QwenRealtimeDebugLog("event=connect_rejected attempt=%llu phase=validate reason=invalid_endpoint model=%s error_len=%zu",
                             static_cast<unsigned long long>(cfg.attemptId),
                             WideToUtf8(ResolveModel(cfg.model)).c_str(), error.size());
        return false;
    }
    QwenRealtimeDebugLog("event=connect_start attempt=%llu phase=handshake model=%s host=%s path=%s port=%u",
                         static_cast<unsigned long long>(cfg.attemptId),
                         WideToUtf8(ResolveModel(cfg.model)).c_str(),
                         WideToUtf8(parsed.host).c_str(),
                         WideToUtf8(parsed.pathAndQuery).c_str(),
                         static_cast<unsigned>(parsed.port));

    winhttp_websocket::ConnectOptions options;
    options.host = parsed.host;
    options.port = parsed.port;
    options.pathAndQuery = parsed.pathAndQuery;
    options.secure = parsed.secure;
    options.timeoutMs = kConnectTimeoutMs;
    options.closeTimeoutMs = 1000;
    options.keepAliveMs = 30000;
    options.headers = L"Authorization: Bearer " + cfg.apiKey +
        L"\r\nOpenAI-Beta: realtime=v1\r\n";

    winhttp_websocket::HandshakeDiagnostics diagnostics;
    if (!transport.Connect(options, diagnostics, error)) {
        if (diagnostics.statusCode != 0) {
            retryable = diagnostics.statusCode == 408 || diagnostics.statusCode == 409 ||
                diagnostics.statusCode == 425 || diagnostics.statusCode == 429 ||
                (diagnostics.statusCode >= 500 && diagnostics.statusCode <= 599);
        }
        QwenRealtimeDebugLog(
            "event=connect_failed attempt=%llu phase=handshake status=%lu retryable=%d request_id=%s trace_id=%s secure_flags=%lu body=%s error=%s",
            static_cast<unsigned long long>(cfg.attemptId),
            static_cast<unsigned long>(diagnostics.statusCode),
            retryable ? 1 : 0,
            WideToUtf8(diagnostics.requestId).c_str(),
            WideToUtf8(diagnostics.traceId).c_str(),
            static_cast<unsigned long>(diagnostics.secureFailureFlags),
            diagnostics.responseBody.c_str(),
            WideToUtf8(error).c_str());
        return false;
    }

    QwenRealtimeDebugLog(
        "event=handshake_response attempt=%llu phase=handshake status=%lu request_id=%s trace_id=%s secure_flags=%lu",
        static_cast<unsigned long long>(cfg.attemptId),
        static_cast<unsigned long>(diagnostics.statusCode),
        WideToUtf8(diagnostics.requestId).c_str(),
        WideToUtf8(diagnostics.traceId).c_str(),
        static_cast<unsigned long>(diagnostics.secureFailureFlags));
    return true;
}

} // namespace

#if defined(VOXTYPE_QWEN_AUDIO_PROTOCOL_TEST)
std::wstring BuildEndpointUrlForTest(const QwenConfig& cfg) {
    return BuildEndpointUrl(cfg);
}

std::string BuildSessionUpdateMessageForTest(const QwenConfig& cfg) {
    return BuildSessionUpdateMessage(cfg);
}

std::string BuildAudioAppendMessageForTest(const BYTE* data, size_t bytes) {
    std::wstring error;
    return BuildAudioAppendMessage(data, static_cast<DWORD>(bytes), error);
}

std::string BuildCommitMessageForTest() {
    return BuildSimpleEventMessage("input_audio_buffer.commit");
}

std::string BuildSessionFinishMessageForTest() {
    return BuildSimpleEventMessage("session.finish");
}

ProtocolEventForTest ParseServerEventForTest(const std::string& message) {
    ProtocolEventForTest result;
    const MessageFrame frame = ParseMessageFrame(message);
    result.type = frame.type;
    result.partialText = frame.partialText;
    result.finalText = frame.finalText;
    result.transcriptionCompleted = frame.transcriptionCompleted;
    result.sessionFinished = frame.sessionFinished;
    if (frame.type == L"conversation.item.input_audio_transcription.failed" ||
        frame.type == L"error") {
        result.failed = true;
        result.error = RealtimeErrorMessage(message, L"realtime server error");
    }
    return result;
}
#endif

struct RealtimeClient::Impl {
    QwenConfig cfg;
    winhttp_websocket::Transport transport;
    std::atomic<bool> connected{false};
    std::atomic<bool> lastFailureRetryable{true};
    std::atomic<size_t> audioBytes{0};
    mutable std::mutex metadataMutex;
    std::wstring sessionId;

    explicit Impl(QwenConfig c) : cfg(std::move(c)) {}

    void SetSessionId(std::wstring value) {
        std::lock_guard<std::mutex> lock(metadataMutex);
        sessionId = std::move(value);
    }

    std::wstring SessionId() const {
        std::lock_guard<std::mutex> lock(metadataMutex);
        return sessionId;
    }
};

RealtimeClient::RealtimeClient(QwenConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

RealtimeClient::~RealtimeClient() = default;

bool RealtimeClient::Connect(std::wstring& error) {
    error.clear();
    if (!impl_) {
        error = L"Qwen client not initialized";
        return false;
    }
    impl_->lastFailureRetryable.store(true);
    impl_->connected.store(false);
    impl_->audioBytes.store(0);
    impl_->SetSessionId({});
    impl_->transport.Close();
    if (impl_->cfg.apiKey.empty()) {
        error = L"missing DashScope API key";
        impl_->lastFailureRetryable.store(false);
        return false;
    }
    bool connectRetryable = true;
    const bool connectOk = ConnectInternal(impl_->transport, impl_->cfg, error, connectRetryable);
    if (!connectOk) {
        impl_->lastFailureRetryable.store(connectRetryable);
        Close();
        return false;
    }
    impl_->connected.store(true);

    if (!SendTextMessage(impl_->transport,
                         BuildSessionUpdateMessage(impl_->cfg),
                         error)) {
        Close();
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + kSessionReadyTimeoutMs;
    while (true) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            error = L"timed out waiting for session.updated";
            Close();
            return false;
        }

        std::string message;
        const ReceiveStatus receiveStatus = ParseReceiveMessage(
            impl_->transport,
            static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 500)),
            message,
            error);
        if (receiveStatus == ReceiveStatus::Timeout) continue;
        if (receiveStatus == ReceiveStatus::Cancelled) {
            Close();
            return false;
        }
        if (receiveStatus == ReceiveStatus::PeerClosed) {
            Close();
            error = L"server closed connection before session was ready";
            return false;
        }
        if (receiveStatus == ReceiveStatus::Error) {
            Close();
            return false;
        }

        MessageFrame frame = ParseMessageFrame(message);
        if (frame.type == L"session.updated") {
            impl_->SetSessionId(frame.sessionId);
            const std::wstring sessionId = impl_->SessionId();
            QwenRealtimeDebugLog(
                "event=session_ready attempt=%llu session_id=%s phase=streaming audio_bytes=0",
                static_cast<unsigned long long>(impl_->cfg.attemptId),
                WideToUtf8(sessionId).c_str());
            return true;
        }
        if (frame.type == L"error") {
            error = RealtimeErrorMessage(message, L"realtime server error");
            impl_->lastFailureRetryable.store(false);
            Close();
            return false;
        }
    }
}

bool RealtimeClient::SendAudioChunk(const BYTE* data, size_t bytes, std::wstring& error) {
    if (!impl_ || !impl_->connected.load()) {
        error = L"WebSocket not connected";
        return false;
    }
    if (bytes == 0) return true;
    if (bytes > static_cast<size_t>((std::numeric_limits<DWORD>::max)())) {
        error = L"audio chunk is too large";
        return false;
    }
    if (!SendBinaryChunk(impl_->transport, data, static_cast<DWORD>(bytes), error)) {
        const std::wstring sessionId = impl_->SessionId();
        QwenRealtimeDebugLog(
            "event=send_audio_failed attempt=%llu session_id=%s phase=streaming chunk_bytes=%zu audio_bytes=%zu error=%s",
            static_cast<unsigned long long>(impl_->cfg.attemptId),
            WideToUtf8(sessionId).c_str(), bytes, impl_->audioBytes.load(),
            WideToUtf8(error).c_str());
        return false;
    }
    impl_->audioBytes.fetch_add(bytes);
    return true;
}

bool RealtimeClient::PollEvent(DWORD timeoutMs, RealtimeEvent& event, std::wstring& error) {
    event = {};
    if (!impl_) {
        error = L"WebSocket not connected";
        return false;
    }

    std::string message;
    const ReceiveStatus receiveStatus = ParseReceiveMessage(
        impl_->transport, timeoutMs, message, error);
    if (receiveStatus == ReceiveStatus::Timeout) {
        error.clear();
        return true;
    }
    if (receiveStatus == ReceiveStatus::PeerClosed) {
        event.peerClosed = true;
        const std::wstring sessionId = impl_->SessionId();
        QwenRealtimeDebugLog(
            "event=peer_close_before_session_finished attempt=%llu session_id=%s phase=finalize audio_bytes=%zu",
            static_cast<unsigned long long>(impl_->cfg.attemptId),
            WideToUtf8(sessionId).c_str(), impl_->audioBytes.load());
        return true;
    }
    if (receiveStatus == ReceiveStatus::Cancelled) {
        return false;
    }
    if (receiveStatus == ReceiveStatus::Error) {
        return false;
    }
    if (message.empty()) {
        return true;
    }

    MessageFrame frame = ParseMessageFrame(message);
    if (frame.type == L"conversation.item.input_audio_transcription.text") {
        event.partialText = frame.partialText;
    } else if (frame.type == L"conversation.item.input_audio_transcription.completed") {
        event.finalText = frame.finalText;
        event.transcriptionCompleted = true;
        const std::wstring sessionId = impl_->SessionId();
        QwenRealtimeDebugLog(
            "event=transcription_completed attempt=%llu session_id=%s phase=finalize audio_bytes=%zu text_chars=%zu",
            static_cast<unsigned long long>(impl_->cfg.attemptId),
            WideToUtf8(sessionId).c_str(), impl_->audioBytes.load(),
            frame.finalText.size());
    } else if (frame.type == L"session.finished") {
        event.sessionFinished = true;
        const std::wstring sessionId = impl_->SessionId();
        QwenRealtimeDebugLog(
            "event=session_finished attempt=%llu session_id=%s phase=complete audio_bytes=%zu",
            static_cast<unsigned long long>(impl_->cfg.attemptId),
            WideToUtf8(sessionId).c_str(), impl_->audioBytes.load());
    } else if (frame.type == L"conversation.item.input_audio_transcription.failed") {
        event.providerFailed = true;
        error = RealtimeErrorMessage(message, L"transcription failed");
        return false;
    } else if (frame.type == L"error") {
        event.providerFailed = true;
        error = RealtimeErrorMessage(message, L"realtime server error");
        return false;
    }
    return true;
}

bool RealtimeClient::SendFinish(std::wstring& error) {
    if (!impl_) {
        error = L"WebSocket not connected";
        return false;
    }
    if (!impl_->connected.load()) {
        error = L"WebSocket not connected";
        return false;
    }

    if (LowerCase(NormalizeTurnDetection(impl_->cfg.turnDetection)) != L"server_vad") {
        if (!SendTextMessage(impl_->transport,
                             BuildSimpleEventMessage("input_audio_buffer.commit"),
                             error)) {
            const std::wstring sessionId = impl_->SessionId();
            QwenRealtimeDebugLog(
                "event=commit_failed attempt=%llu session_id=%s phase=finalize audio_bytes=%zu error=%s",
                static_cast<unsigned long long>(impl_->cfg.attemptId),
                WideToUtf8(sessionId).c_str(), impl_->audioBytes.load(),
                WideToUtf8(error).c_str());
            return false;
        }
    }

    if (!SendTextMessage(impl_->transport,
                         BuildSimpleEventMessage("session.finish"),
                         error)) {
        const std::wstring sessionId = impl_->SessionId();
        QwenRealtimeDebugLog(
            "event=session_finish_failed attempt=%llu session_id=%s phase=finalize audio_bytes=%zu error=%s",
            static_cast<unsigned long long>(impl_->cfg.attemptId),
            WideToUtf8(sessionId).c_str(), impl_->audioBytes.load(),
            WideToUtf8(error).c_str());
        return false;
    }
    const std::wstring sessionId = impl_->SessionId();
    QwenRealtimeDebugLog(
        "event=session_finish_sent attempt=%llu session_id=%s phase=finalize audio_bytes=%zu",
        static_cast<unsigned long long>(impl_->cfg.attemptId),
        WideToUtf8(sessionId).c_str(), impl_->audioBytes.load());
    return true;
}

bool RealtimeClient::Finish(DWORD finalTimeoutMs, std::wstring& finalText, std::wstring& error) {
    finalText.clear();
    if (!SendFinish(error)) {
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + (std::max<DWORD>(finalTimeoutMs, 1000));
    while (GetTickCount64() < deadline) {
        const DWORD waitMs = static_cast<DWORD>((std::min<ULONGLONG>)(
            deadline - GetTickCount64(), 500));
        RealtimeEvent ev;
        std::wstring receiveError;
        if (!PollEvent(waitMs, ev, receiveError)) {
            error = receiveError.empty() ? L"WebSocket receive failed" : receiveError;
            return false;
        }
        if (ev.peerClosed) {
            error = L"WebSocket peer closed before session.finished";
            return false;
        }
        if (ev.transcriptionCompleted) finalText = ev.finalText;
        if (ev.sessionFinished) return true;
    }
    Abort();
    error = L"timed out waiting for final transcript";
    return false;
}

void RealtimeClient::Abort() {
    if (!impl_) return;
    const std::wstring sessionId = impl_->SessionId();
    QwenRealtimeDebugLog(
        "event=client_abort attempt=%llu session_id=%s phase=abort audio_bytes=%zu",
        static_cast<unsigned long long>(impl_->cfg.attemptId),
        WideToUtf8(sessionId).c_str(), impl_->audioBytes.load());
    impl_->transport.Abort();
    impl_->connected.store(false);
}

void RealtimeClient::Close() {
    if (!impl_) return;
    impl_->transport.Close();
    impl_->connected.store(false);
}

bool RealtimeClient::LastFailureRetryable() const {
    return impl_ && impl_->lastFailureRetryable.load();
}

size_t ChunkBytesForConfig(const QwenConfig& cfg) {
    const int chunkMs = std::clamp(cfg.chunkMs, 20, 1000);
    return std::clamp<size_t>(static_cast<size_t>(chunkMs) * kPcmBytesPerMs, 320, 32000);
}

std::wstring Recognize(const std::vector<BYTE>& pcm, const QwenConfig& cfg, DWORD finalTimeoutMs) {
    if (cfg.apiKey.empty()) {
        return MakeError(L"missing DashScope API key");
    }
    if (pcm.empty()) {
        return L"";
    }

    RealtimeClient client(cfg);
    std::wstring error;
    if (!client.Connect(error)) {
        return MakeError(error);
    }

    const size_t chunkBytes = ChunkBytesForConfig(cfg);
    for (size_t offset = 0; offset < pcm.size(); offset += chunkBytes) {
        const size_t bytes = std::min(chunkBytes, pcm.size() - offset);
        if (!client.SendAudioChunk(pcm.data() + offset, bytes, error)) {
            return MakeError(error);
        }
        // The realtime endpoint expects a realtime-like stream.  Pace replay
        // and recorded fallback traffic instead of flooding the WebSocket.
        if (offset + bytes < pcm.size()) {
            Sleep(static_cast<DWORD>(std::clamp(cfg.chunkMs, 20, 1000)));
        }
    }

    std::wstring finalText;
    if (!client.Finish(finalTimeoutMs, finalText, error)) {
        return MakeError(error);
    }
    return finalText;
}

TestResult TestConnection(const QwenConfig& cfg) {
    TestResult result;
    if (cfg.apiKey.empty()) {
        result.message = L"DashScope API key is empty.";
        return result;
    }

    RealtimeClient client(cfg);
    std::wstring error;
    if (!client.Connect(error)) {
        result.message = MakeError(error.empty() ? L"WebSocket handshake failed" : error);
        client.Close();
        return result;
    }

    result.ok = true;
    result.message = L"Connection OK. Qwen ASR realtime session is ready.";
    return result;
}

} // namespace qwen_asr
