#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "qwen_audio_streaming.h"

#include "qwen_audio_json.h"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <sstream>
#include <mutex>
#include <shared_mutex>
#include <vector>
#include <winhttp.h>
#include <objbase.h>

#pragma comment(lib, "winhttp.lib")

#ifndef WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT
#define WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT 122
#endif

namespace qwen_audio_streaming {
namespace {

struct Url { std::wstring host, path; INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT; };

Url ParseUrl(const std::wstring& value, std::wstring& error) {
    Url out;
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    std::wstring url = Trim(value);
    if (url.empty() || url.rfind(L"wss://", 0) != 0) { error = L"Audio streaming Base URL must use wss://"; return out; }
    url.replace(0, 6, L"https://");
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) { error = L"invalid Audio streaming Base URL"; return out; }
    out.host.assign(parts.lpszHostName, parts.dwHostNameLength);
    out.path.assign(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (out.path.empty() || out.path == L"/") out.path = L"/api-ws/v1/inference";
    while (out.path.size() > 1 && out.path.back() == L'/') out.path.pop_back();
    if (out.path != L"/api-ws/v1/inference") {
        error = L"Audio streaming endpoint path must be /api-ws/v1/inference";
        return out;
    }
    out.port = parts.nPort ? parts.nPort : INTERNET_DEFAULT_HTTPS_PORT;
    if (out.host.empty()) error = L"Audio streaming host is empty";
    return out;
}

std::wstring Field(const std::string& json, const char* key) {
    return qwen_audio_json::ExtractString(json, key);
}

bool BoolField(const std::string& json, const char* key, bool fallback = false) {
    return qwen_audio_json::ExtractBool(json, key, fallback);
}

std::vector<std::wstring> SplitHints(const std::wstring& raw) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= raw.size() && result.size() < 4) {
        const size_t end = raw.find_first_of(L",;", start);
        std::wstring item = Trim(raw.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!item.empty()) {
            if (item == L"fil") item = L"tl";
            bool valid = item.size() <= 16;
            for (wchar_t ch : item) valid = valid && ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-');
            if (valid) result.push_back(std::move(item));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

std::string TaskId() {
    GUID guid = {};
    if (CoCreateGuid(&guid) != S_OK) return "00000000-0000-0000-0000-000000000000";
    wchar_t text[64] = {};
    StringFromGUID2(guid, text, 64);
    std::wstring value(text);
    if (!value.empty() && value.front() == L'{') value = value.substr(1, value.size() - 2);
    return WideToUtf8(value);
}

std::string BuildRunTaskMessageImpl(const Config& cfg, const std::string& taskId) {
    std::string json = "{\"header\":{\"action\":\"run-task\",\"task_id\":\"" + taskId + "\",\"streaming\":\"duplex\"},\"payload\":{\"task_group\":\"audio\",\"task\":\"asr\",\"function\":\"recognition\",\"model\":\"" + EscapeJson(cfg.model) + "\",\"parameters\":{\"format\":\"pcm\",\"sample_rate\":16000";
    const auto hints = SplitHints(cfg.languageHints);
    if (!hints.empty()) {
        json += ",\"language_hints\":[";
        for (size_t i = 0; i < hints.size(); ++i) {
            if (i) json += ',';
            json += "\"" + EscapeJson(hints[i]) + "\"";
        }
        json += ']';
    }
    if (!cfg.vocabularyId.empty()) json += ",\"vocabulary_id\":\"" + EscapeJson(cfg.vocabularyId) + "\"";
    if (qwen_audio_json::HasValidVocabulary(cfg.vocabulary)) {
        json += ",\"vocabulary\":" + WideToUtf8(Trim(cfg.vocabulary));
    }
    json += ",\"semantic_punctuation_enabled\":" + std::string(cfg.semanticPunctuation ? "true" : "false");
    json += ",\"max_sentence_silence\":" + std::to_string(std::clamp(cfg.maxSentenceSilenceMs, 200, 6000));
    json += ",\"multi_threshold_mode_enabled\":" + std::string(cfg.multiThresholdMode && !cfg.semanticPunctuation ? "true" : "false");
    if (cfg.heartbeat) json += ",\"heartbeat\":true";
    if (cfg.speechNoiseThresholdEnabled) json += ",\"speech_noise_threshold\":" + std::to_string(std::clamp(cfg.speechNoiseThreshold, -1.0f, 1.0f));
    json += "},\"input\":{}}}";
    return json;
}

std::string BuildFinishTaskMessageImpl(const std::string& taskId) {
    return "{\"header\":{\"action\":\"finish-task\",\"task_id\":\"" +
        taskId + "\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{}}}";
}

Event ParseServerEventMessageImpl(const std::string& message) {
    Event event;
    event.action = Field(message, "action");
    if (event.action.empty()) event.action = Field(message, "event");
    event.text = Field(message, "text");
    if (event.text.empty()) event.text = Field(message, "transcript");
    if (event.text.empty()) event.text = Field(message, "sentence");
    event.sentenceEnd = BoolField(message, "sentence_end");
    event.heartbeat = BoolField(message, "heartbeat");
    event.taskStarted = event.action == L"task-started";
    event.taskFinished = event.action == L"task-finished";
    event.failed = event.action == L"task-failed" || event.action == L"error";
    event.retryable = false;
    event.message = Field(message, "error_message");
    if (event.message.empty()) event.message = Field(message, "message");
    event.noSpeech = message.find("ASR_RESPONSE_HAVE_NO_WORDS") != std::string::npos;
    return event;
}

struct Connection {
    std::atomic<HINTERNET> session{nullptr};
    std::atomic<HINTERNET> connect{nullptr};
    std::atomic<HINTERNET> request{nullptr};
    std::atomic<HINTERNET> ws{nullptr};
    ~Connection() { Close(); }
    void Close() {
        if (HINTERNET handle = ws.exchange(nullptr)) WinHttpCloseHandle(handle);
        if (HINTERNET handle = request.exchange(nullptr)) WinHttpCloseHandle(handle);
        if (HINTERNET handle = connect.exchange(nullptr)) WinHttpCloseHandle(handle);
        if (HINTERNET handle = session.exchange(nullptr)) WinHttpCloseHandle(handle);
    }
};

} // namespace

std::string BuildRunTaskMessage(const Config& config, const std::string& taskId) {
    return BuildRunTaskMessageImpl(config, taskId);
}

std::string BuildFinishTaskMessage(const std::string& taskId) {
    return BuildFinishTaskMessageImpl(taskId);
}

Event ParseServerEventMessage(const std::string& message) {
    return ParseServerEventMessageImpl(message);
}

namespace {

void AppendTranscriptSegment(std::wstring& text, const std::wstring& segment) {
    if (segment.empty()) return;
    if (!text.empty() && !iswspace(text.back()) && !iswspace(segment.front()) &&
        iswalnum(text.back()) && iswalnum(segment.front())) {
        text.push_back(L' ');
    }
    text += segment;
}

} // namespace

void TranscriptAccumulator::Apply(const Event& event) {
    if (event.heartbeat || event.text.empty()) return;
    if (event.sentenceEnd) {
        AppendTranscriptSegment(committed_, event.text);
        pending_.clear();
    } else {
        pending_ = event.text;
    }
}

std::wstring TranscriptAccumulator::Text() const {
    std::wstring result = committed_;
    AppendTranscriptSegment(result, pending_);
    return result;
}

struct Client::Impl {
    explicit Impl(Config c) : config(std::move(c)) {}
    Config config;
    Connection conn;
    // WinHTTP permits one Send and one Receive to run concurrently, but a
    // handle must not be closed while any WinHTTP operation is in flight.
    // The shared lock preserves duplex Send/Receive while making Close/Abort
    // wait for all active operations to return.
    std::shared_mutex operationMutex;
    std::mutex sendMutex;
    std::mutex receiveMutex;
    std::atomic<bool> connected{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> lastFailureRetryable{true};
    std::string taskId;
};

Client::Client(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Client::~Client() { Close(); }

bool Client::Connect(std::wstring& error) {
    impl_->lastFailureRetryable.store(true);
    if (impl_->config.apiKey.empty()) { error = L"missing API key"; return false; }
    if (!qwen_audio_json::IsValidVocabulary(impl_->config.vocabulary, &error)) {
        impl_->lastFailureRetryable.store(false);
        return false;
    }
    Url url = ParseUrl(impl_->config.baseUrl, error);
    if (!error.empty()) {
        impl_->lastFailureRetryable.store(false);
        return false;
    }
    HINTERNET session = nullptr;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
        if (impl_->cancelled.load()) { error = L"connection cancelled"; return false; }
        session = WinHttpOpen(L"VoxType/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
        impl_->conn.session.store(session);
        if (!session) { error = L"WinHttpOpen failed"; return false; }
        WinHttpSetTimeouts(session, 8000, 8000, 8000, 8000);
    }

    HINTERNET connect = nullptr;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
        if (impl_->cancelled.load()) { error = L"connection cancelled"; }
        else {
            connect = WinHttpConnect(session, url.host.c_str(), url.port, 0);
            impl_->conn.connect.store(connect);
            if (!connect) error = L"WinHttpConnect failed";
        }
    }
    if (!error.empty()) { Close(); return false; }

    HINTERNET request = nullptr;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
        if (impl_->cancelled.load()) { error = L"connection cancelled"; }
        else {
            request = WinHttpOpenRequest(connect, L"GET", url.path.c_str(), nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE);
            impl_->conn.request.store(request);
            if (!request) {
                error = L"WinHttpOpenRequest failed";
            } else {
                WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
                const std::wstring headers = L"Authorization: Bearer " + impl_->config.apiKey + L"\r\n";
                if (!WinHttpAddRequestHeaders(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD)) {
                    error = L"failed to add Authorization";
                } else if (!WinHttpSendRequest(request, nullptr, 0, nullptr, 0, 0, 0) ||
                           !WinHttpReceiveResponse(request, nullptr)) {
                    error = L"WebSocket handshake failed";
                }
            }
        }
    }
    if (!error.empty()) { Close(); return false; }

    HINTERNET ws = nullptr;
    {
        std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
        if (impl_->cancelled.load()) { error = L"connection cancelled"; }
        else {
            ws = WinHttpWebSocketCompleteUpgrade(request, 0);
            if (!ws) error = L"WebSocket upgrade failed";
            else impl_->conn.ws.store(ws);
        }
    }
    if (!error.empty()) { Close(); return false; }

    {
        std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
        if (HINTERNET oldRequest = impl_->conn.request.exchange(nullptr)) WinHttpCloseHandle(oldRequest);
    }
    if (impl_->cancelled.load()) { error = L"connection cancelled"; Close(); return false; }

    impl_->taskId = TaskId();
    const std::string task = BuildRunTaskMessage(impl_->config, impl_->taskId);
    DWORD e = NO_ERROR;
    {
        std::lock_guard<std::mutex> sendLock(impl_->sendMutex);
        std::shared_lock<std::shared_mutex> lock(impl_->operationMutex);
        ws = impl_->conn.ws.load();
        if (!ws || impl_->cancelled.load()) e = ERROR_WINHTTP_OPERATION_CANCELLED;
        else e = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                      const_cast<char*>(task.data()), static_cast<DWORD>(task.size()));
    }
    if (e != NO_ERROR) { error = L"run-task send failed"; return false; }
    impl_->connected.store(true);
    const ULONGLONG deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        Event ready;
        if (!Poll(500, ready, error)) {
            if (ready.timeout) continue;
            if (error.empty()) error = L"WebSocket receive failed";
            break;
        }
        if (ready.failed) {
            impl_->lastFailureRetryable.store(ready.retryable);
            if (error.empty()) error = ready.message.empty() ? L"task-started failed" : ready.message;
            break;
        }
        if (ready.taskStarted) return true;
    }
    if (error.empty()) error = L"timed out waiting for task-started";
    Close();
    return false;
}

bool Client::SendAudio(const BYTE* data, size_t bytes, std::wstring& error) {
    std::lock_guard<std::mutex> sendLock(impl_->sendMutex);
    std::shared_lock<std::shared_mutex> lock(impl_->operationMutex);
    HINTERNET ws = impl_->conn.ws.load();
    if (!ws || !data || !bytes) return true;
    DWORD e = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<BYTE*>(data), static_cast<DWORD>(bytes));
    if (e != NO_ERROR) { error = L"binary PCM send failed"; return false; }
    return true;
}

bool Client::Poll(DWORD timeoutMs, Event& event, std::wstring& error) {
    event = {};
    std::lock_guard<std::mutex> receiveLock(impl_->receiveMutex);
    std::shared_lock<std::shared_mutex> lock(impl_->operationMutex);
    HINTERNET ws = impl_->conn.ws.load();
    if (!ws) { error = L"WebSocket is closed"; return false; }
    DWORD slice = std::max<DWORD>(1, timeoutMs);
    WinHttpSetOption(ws, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT, &slice, sizeof(slice));
    std::string message;
    char buffer[64 * 1024] = {};
    DWORD bytes = 0; WINHTTP_WEB_SOCKET_BUFFER_TYPE type = WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
    DWORD e = WinHttpWebSocketReceive(ws, buffer, sizeof(buffer), &bytes, &type);
    if (e == ERROR_WINHTTP_TIMEOUT) { event.timeout = true; return false; }
    if (e != NO_ERROR) { error = L"WebSocket receive failed"; return false; }
    if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
        // A peer close is not the protocol's task-finished acknowledgement.
        // Treating it as final would let a truncated session paste partial
        // text as a successful result.
        event.failed = true;
        event.retryable = true;
        event.message = L"WebSocket closed before task-finished";
        error = event.message;
        return true;
    }
    if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) return true;
    message.append(buffer, bytes);
    while (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) {
        bytes = 0;
        e = WinHttpWebSocketReceive(ws, buffer, sizeof(buffer), &bytes, &type);
        if (e == ERROR_WINHTTP_TIMEOUT) { event.timeout = true; return false; }
        if (e != NO_ERROR) { error = L"WebSocket receive failed"; return false; }
        message.append(buffer, bytes);
    }
    const Event parsed = ParseServerEventMessage(message);
    event = parsed;
    if (event.failed) error = event.message;
    return true;
}

bool Client::Finish(std::wstring& error) {
    std::lock_guard<std::mutex> sendLock(impl_->sendMutex);
    std::shared_lock<std::shared_mutex> lock(impl_->operationMutex);
    HINTERNET ws = impl_->conn.ws.load();
    if (!ws) return false;
    const std::string msg = BuildFinishTaskMessage(impl_->taskId);
    DWORD e = WinHttpWebSocketSend(ws, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, const_cast<char*>(msg.data()), static_cast<DWORD>(msg.size()));
    if (e != NO_ERROR) { error = L"finish-task send failed"; return false; }
    return true;
}

void Client::Abort() {
    impl_->cancelled.store(true);
    std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
    impl_->conn.Close();
    impl_->connected.store(false);
}

void Client::Close() {
    std::unique_lock<std::shared_mutex> lock(impl_->operationMutex);
    impl_->conn.Close();
    impl_->connected.store(false);
}

bool Client::LastFailureRetryable() const {
    return impl_->lastFailureRetryable.load();
}

TestResult TestConnection(const Config& cfg) {
    TestResult r; Client c(cfg); std::wstring error;
    r.ok = c.Connect(error);
    if (r.ok) {
        std::vector<BYTE> silence(3200, 0); // 100 ms, 16 kHz mono s16le.
        if (!c.SendAudio(silence.data(), silence.size(), error)) r.ok = false;
    }
    if (r.ok && !c.Finish(error)) r.ok = false;
    if (r.ok) {
        const ULONGLONG deadline = GetTickCount64() + 5000;
        bool finished = false;
        while (GetTickCount64() < deadline) {
            Event ev;
            if (!c.Poll(500, ev, error)) {
                if (!error.empty()) { r.ok = false; break; }
                continue;
            }
            if (ev.noSpeech) { finished = true; break; }
            if (ev.failed) { r.ok = false; if (error.empty()) error = ev.message; break; }
            if (ev.taskFinished) { finished = true; break; }
        }
        if (!finished && r.ok) { r.ok = false; error = L"timed out waiting for task-finished"; }
    }
    r.message = r.ok ? L"Connection OK. Qwen Audio streaming endpoint is reachable."
                    : (error.empty() ? L"Connection failed." : error);
    c.Close(); return r;
}

} // namespace qwen_audio_streaming
