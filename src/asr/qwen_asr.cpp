#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "qwen_asr.h"

#include "utils.h"

#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwctype>
#include <iomanip>
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

#ifndef WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT
#define WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT 122
#endif

namespace qwen_asr {
namespace {

constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSessionReadyTimeoutMs = 5000;
constexpr DWORD kReceiveSliceMs = 1000;
constexpr size_t kPcmBytesPerMs = 32; // 16kHz, mono, s16le.

struct ParsedUrl {
    std::wstring host;
    std::wstring pathAndQuery;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

struct QwenConnection {
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    std::atomic<HINTERNET> hWebSocket{nullptr};

    ~QwenConnection() {
        Close();
    }

    void Close() {
        HINTERNET ws = TakeWebSocket();
        if (ws) {
            WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(ws);
        }
        if (hConnect) {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
        if (hSession) {
            WinHttpCloseHandle(hSession);
            hSession = nullptr;
        }
    }

    HINTERNET WebSocket() const {
        return hWebSocket.load();
    }

    void SetWebSocket(HINTERNET ws) {
        hWebSocket.store(ws);
    }

    HINTERNET TakeWebSocket() {
        return hWebSocket.exchange(nullptr);
    }
};

struct MessageFrame {
    std::wstring type;
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

    if (StartsWith(normalized, L"wss://")) {
        normalized.replace(0, 6, L"https://");
        parsed.secure = true;
    } else if (StartsWith(normalized, L"ws://")) {
        normalized.replace(0, 5, L"http://");
        parsed.secure = false;
    } else if (StartsWith(normalized, L"https://")) {
        parsed.secure = true;
    } else if (StartsWith(normalized, L"http://")) {
        parsed.secure = false;
    } else {
        error = L"invalid WebSocket URL";
        return false;
    }

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

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
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
            AppendUtf8(unescaped, cp);
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
    if (frame.type == L"conversation.item.input_audio_transcription.text") {
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

bool SendTextMessage(HINTERNET hWebSocket, const std::string& message, std::wstring& error) {
    DWORD err = WinHttpWebSocketSend(
        hWebSocket,
        WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
        const_cast<char*>(message.data()),
        static_cast<DWORD>(message.size()));
    if (err != NO_ERROR) {
        error = L"WebSocket send failed (err=" + std::to_wstring(err) + L")";
        return false;
    }
    return true;
}

std::string BuildSimpleEventMessage(const char* type) {
    return "{\"type\":\"" + std::string(type) +
        "\",\"event_id\":\"" + GenerateEventId() + "\"}";
}

bool SendBinaryChunk(HINTERNET hWebSocket, const BYTE* data, DWORD bytes, std::wstring& error) {
    std::string base64;
    if (!Base64Encode(data, bytes, base64)) {
        error = L"failed to base64-encode audio";
        return false;
    }

    std::string message = "{\"type\":\"input_audio_buffer.append\",\"event_id\":\"" + GenerateEventId() +
        "\",\"audio\":\"" + base64 + "\"}";
    return SendTextMessage(hWebSocket, message, error);
}

std::string BuildSessionUpdateMessage(const QwenConfig& cfg) {
    std::wstring lang = NormalizeLanguage(cfg.language);
    std::wstring turnDetection = ManualTurnDetectionJson(cfg);

    std::ostringstream oss;
    oss << "{\"type\":\"session.update\",\"event_id\":\"" << GenerateEventId() << "\",\"session\":{";
    oss << "\"input_audio_format\":\"pcm\",";
    oss << "\"sample_rate\":16000,";
    oss << "\"input_audio_transcription\":{";
    if (!lang.empty()) {
        oss << "\"language\":\"" << EscapeJson(lang) << "\"";
    }
    oss << "},";
    oss << "\"turn_detection\":" << WideToUtf8(turnDetection);
    oss << "}}";
    return oss.str();
}

size_t ClampChunkBytes(size_t bytes) {
    const size_t minBytes = 320;   // 10ms, keep it sane
    const size_t maxBytes = 32000; // 1s
    return std::clamp(bytes, minBytes, maxBytes);
}

bool ParseReceiveMessage(HINTERNET hWebSocket,
                         DWORD timeoutMs,
                         std::string& message,
                         std::wstring& error) {
    message.clear();
    DWORD effectiveTimeout = std::max<DWORD>(timeoutMs, 1);
    WinHttpSetOption(hWebSocket, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &effectiveTimeout, sizeof(effectiveTimeout));

    std::vector<char> buffer(64 * 1024);
    std::string assembled;

    while (true) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        DWORD err = WinHttpWebSocketReceive(hWebSocket,
                                            buffer.data(),
                                            static_cast<DWORD>(buffer.size()),
                                            &bytesRead,
                                            &bufferType);
        if (err == ERROR_WINHTTP_TIMEOUT) return false;
        if (err != NO_ERROR) {
            error = L"WebSocket receive failed (err=" + std::to_wstring(err) + L")";
            return false;
        }

        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            message.clear();
            return true;
        }

        if (bytesRead > 0) {
            assembled.append(buffer.data(), buffer.data() + bytesRead);
        }

        if (bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
            message = std::move(assembled);
            return true;
        }
    }
}

bool ConnectInternal(QwenConnection& conn, const QwenConfig& cfg, std::wstring& error) {
    ParsedUrl parsed;
    std::wstring endpoint = BuildEndpointUrl(cfg);
    if (!ParseWebSocketUrl(endpoint, parsed, error)) {
        return false;
    }

    conn.hSession = WinHttpOpen(L"VoxType/1.0",
                                WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME,
                                WINHTTP_NO_PROXY_BYPASS,
                                0);
    if (!conn.hSession) {
        error = L"WinHttpOpen failed";
        return false;
    }
    WinHttpSetTimeouts(conn.hSession, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs);

    conn.hConnect = WinHttpConnect(conn.hSession, parsed.host.c_str(), parsed.port, 0);
    if (!conn.hConnect) {
        error = L"WinHttpConnect failed (err=" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    DWORD flags = parsed.secure ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(conn.hConnect,
                                        L"GET",
                                        parsed.pathAndQuery.c_str(),
                                        nullptr,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        flags);
    if (!hReq) {
        error = L"WinHttpOpenRequest failed (err=" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    WinHttpSetTimeouts(hReq, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs, kConnectTimeoutMs);
    WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

    DWORD closeTimeout = 3000;
    DWORD keepAlive = 30000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &closeTimeout, sizeof(closeTimeout));
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof(keepAlive));

    std::wstring headers = L"Authorization: Bearer " + cfg.apiKey + L"\r\n";
    if (!WinHttpAddRequestHeaders(hReq, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
        error = L"WinHttpAddRequestHeaders failed (err=" + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hReq);
        return false;
    }

    std::mutex requestMutex;
    auto closeRequest = [&]() {
        std::lock_guard<std::mutex> lock(requestMutex);
        if (hReq) {
            WinHttpCloseHandle(hReq);
            hReq = nullptr;
        }
    };

    const ULONGLONG requestStart = GetTickCount64();
    std::atomic<bool> requestDone{false};
    std::thread watchdogThread([&]() {
        while (!requestDone.load()) {
            Sleep(100);
            if (requestDone.load()) return;
            if (GetTickCount64() - requestStart >= kConnectTimeoutMs) {
                closeRequest();
                return;
            }
        }
    });

    bool sendOk = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                     WINHTTP_NO_REQUEST_DATA, 0, 0, 0) != FALSE;
    bool recvOk = sendOk && WinHttpReceiveResponse(hReq, nullptr) != FALSE;
    requestDone.store(true);
    watchdogThread.join();

    if (!sendOk) {
        error = L"WinHttpSendRequest failed (err=" + std::to_wstring(GetLastError()) + L")";
        closeRequest();
        return false;
    }

    if (!recvOk) {
        error = L"WinHttpReceiveResponse failed (err=" + std::to_wstring(GetLastError()) + L")";
        closeRequest();
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 101) {
        error = L"WebSocket upgrade failed (HTTP " + std::to_wstring(statusCode) + L")";
        closeRequest();
        return false;
    }

    conn.SetWebSocket(WinHttpWebSocketCompleteUpgrade(hReq, 0));
    closeRequest();
    if (!conn.WebSocket()) {
        error = L"WinHttpWebSocketCompleteUpgrade failed (err=" + std::to_wstring(GetLastError()) + L")";
        return false;
    }

    return true;
}

} // namespace

struct RealtimeClient::Impl {
    QwenConfig cfg;
    QwenConnection conn;
    std::atomic<bool> connected{false};

    explicit Impl(QwenConfig c) : cfg(std::move(c)) {}
};

RealtimeClient::RealtimeClient(QwenConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

RealtimeClient::~RealtimeClient() = default;

bool RealtimeClient::Connect(std::wstring& error) {
    if (!impl_) {
        error = L"Qwen client not initialized";
        return false;
    }
    if (!ConnectInternal(impl_->conn, impl_->cfg, error)) {
        return false;
    }
    impl_->connected.store(true);

    HINTERNET ws = impl_->conn.WebSocket();
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }
    if (!SendTextMessage(ws, BuildSessionUpdateMessage(impl_->cfg), error)) {
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
        ws = impl_->conn.WebSocket();
        if (!ws) {
            error = L"WebSocket not connected";
            Close();
            return false;
        }
        if (!ParseReceiveMessage(ws,
                                 static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, kReceiveSliceMs)),
                                 message,
                                 error)) {
            if (error.empty()) continue;
            Close();
            return false;
        }

        if (message.empty()) {
            Close();
            error = L"server closed connection before session was ready";
            return false;
        }

        MessageFrame frame = ParseMessageFrame(message);
        if (frame.type == L"session.updated") {
            return true;
        }
        if (frame.type == L"error") {
            error = RealtimeErrorMessage(message, L"realtime server error");
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
    HINTERNET ws = impl_->conn.WebSocket();
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }
    return SendBinaryChunk(ws, data, static_cast<DWORD>(bytes), error);
}

bool RealtimeClient::PollEvent(DWORD timeoutMs, RealtimeEvent& event, std::wstring& error) {
    event = {};
    if (!impl_) {
        error = L"WebSocket not connected";
        return false;
    }

    std::string message;
    HINTERNET ws = impl_->conn.WebSocket();
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }
    if (!ParseReceiveMessage(ws, timeoutMs, message, error)) {
        if (error.empty()) return true; // timeout
        return false;
    }
    if (message.empty()) {
        event.sessionFinished = true;
        return true;
    }

    MessageFrame frame = ParseMessageFrame(message);
    if (frame.type == L"conversation.item.input_audio_transcription.text") {
        event.partialText = frame.partialText;
    } else if (frame.type == L"conversation.item.input_audio_transcription.completed") {
        event.finalText = frame.finalText;
        event.transcriptionCompleted = true;
    } else if (frame.type == L"session.finished") {
        event.sessionFinished = true;
    } else if (frame.type == L"conversation.item.input_audio_transcription.failed") {
        error = RealtimeErrorMessage(message, L"transcription failed");
        return false;
    } else if (frame.type == L"error") {
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
    HINTERNET ws = impl_->conn.WebSocket();
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }

    if (LowerCase(NormalizeTurnDetection(impl_->cfg.turnDetection)) != L"server_vad") {
        if (!SendTextMessage(ws, BuildSimpleEventMessage("input_audio_buffer.commit"), error)) {
            return false;
        }
    }

    ws = impl_->conn.WebSocket();
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }
    if (!SendTextMessage(ws,
                         BuildSimpleEventMessage("session.finish"),
                         error)) {
        return false;
    }
    return true;
}

bool RealtimeClient::Finish(DWORD finalTimeoutMs, std::wstring& finalText, std::wstring& error) {
    finalText.clear();
    if (!SendFinish(error)) {
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + std::max<DWORD>(finalTimeoutMs, 1000);
    while (true) {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            if (!finalText.empty()) return true;
            error = L"timed out waiting for final transcript";
            Close();
            return false;
        }

        RealtimeEvent ev;
        if (!PollEvent(static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, kReceiveSliceMs)), ev, error)) {
            if (error.empty()) continue;
            Close();
            return false;
        }
        if (!ev.partialText.empty()) {
            // Streamed partial is handled by caller if needed.
        }
        if (ev.transcriptionCompleted) {
            finalText = ev.finalText;
        }
        if (ev.sessionFinished) {
            return true;
        }
    }
}

void RealtimeClient::Abort() {
    if (!impl_) return;
    HINTERNET ws = impl_->conn.TakeWebSocket();
    if (ws) {
        WinHttpCloseHandle(ws);
    }
    impl_->connected.store(false);
}

void RealtimeClient::Close() {
    if (!impl_) return;
    impl_->conn.Close();
    impl_->connected.store(false);
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
        result.message = MakeError(error);
        return result;
    }

    result.ok = true;
    result.message = L"Connection OK. Qwen ASR realtime session is ready.";
    return result;
}

} // namespace qwen_asr
