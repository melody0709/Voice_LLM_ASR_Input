#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "qwen_audio_streaming.h"

#include "asr_runtime_log.h"
#include "qwen_audio_json.h"
#include "utils.h"
#include "winhttp_websocket_transport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cwctype>
#include <initializer_list>
#include <sstream>
#include <mutex>
#include <thread>
#include <vector>
#include <winhttp.h>
#include <objbase.h>

#pragma comment(lib, "winhttp.lib")

namespace qwen_audio_streaming {
namespace {

constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSendTimeoutMs = 5000;

struct Url { std::wstring host, path; INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT; };

void QwenAudioDebugLog(const char* format, ...) {
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

std::string TruncateLogText(const std::wstring& value, size_t maxBytes = 256) {
    std::string text = WideToUtf8(value);
    if (text.size() > maxBytes) text.resize(maxBytes);
    return text;
}

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

bool ContainsAny(const std::wstring& value, std::initializer_list<const wchar_t*> terms) {
    std::wstring lower = value;
    for (wchar_t& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
    for (const wchar_t* term : terms) {
        if (term && lower.find(term) != std::wstring::npos) return true;
    }
    return false;
}

bool IsRetryableTaskFailure(const std::wstring& errorCode,
                            const std::wstring& errorMessage) {
    if (ContainsAny(errorCode, {L"no_words", L"have_no_words", L"invalid_parameter",
                                L"invalid_request", L"invalid_api_key", L"unauthorized",
                                L"forbidden", L"not_found", L"vocabulary"}) ||
        ContainsAny(errorMessage, {L"ASR_RESPONSE_HAVE_NO_WORDS", L"invalid parameter",
                                   L"invalid request", L"api key", L"unauthorized",
                                   L"forbidden", L"vocabulary"})) {
        return false;
    }
    return ContainsAny(errorCode, {L"timeout", L"tempor", L"internal", L"service_unavailable",
                                   L"overload", L"busy", L"rate_limit", L"throttl"}) ||
           ContainsAny(errorMessage, {L"timeout", L"tempor", L"internal server",
                                      L"service unavailable", L"overload", L"busy",
                                      L"rate limit", L"throttl"});
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
    std::wstring context = Trim(cfg.inputContextText);
    if (context.size() > 400) context.resize(400);
    if (context.empty()) {
        json += "},\"input\":{}}}";
    } else {
        json += "},\"input\":{\"context\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"";
        json += EscapeJson(context);
        json += "\"}]}]}}}";
    }
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
    event.errorCode = Field(message, "error_code");
    event.message = Field(message, "error_message");
    if (event.message.empty()) event.message = Field(message, "message");
    event.noSpeech = message.find("ASR_RESPONSE_HAVE_NO_WORDS") != std::string::npos;
    event.retryable = event.failed && !event.noSpeech &&
        IsRetryableTaskFailure(event.errorCode, event.message);
    return event;
}

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

std::wstring TranscriptAccumulator::CommittedText() const {
    return committed_;
}

bool TranscriptAccumulator::HasCommittedText() const {
    return !committed_.empty();
}

struct Client::Impl {
    explicit Impl(Config c) : config(std::move(c)) {}
    Config config;
    winhttp_websocket::Transport transport;
    std::atomic<bool> connected{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> lastFailureRetryable{true};
    std::atomic<size_t> audioBytes{0};
    mutable std::mutex metadataMutex;
    std::string taskId;

    void SetTaskId(std::string value) {
        std::lock_guard<std::mutex> lock(metadataMutex);
        taskId = std::move(value);
    }

    std::string TaskIdSnapshot() const {
        std::lock_guard<std::mutex> lock(metadataMutex);
        return taskId;
    }
};

Client::Client(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
Client::~Client() { Close(); }

bool Client::Connect(std::wstring& error) {
    error.clear();
    const ULONGLONG connectStartTick = GetTickCount64();
    impl_->cancelled.store(false);
    impl_->connected.store(false);
    impl_->lastFailureRetryable.store(true);
    impl_->audioBytes.store(0);
    const std::string modelLog = WideToUtf8(impl_->config.model);
    if (impl_->config.apiKey.empty()) {
        impl_->lastFailureRetryable.store(false);
        error = L"missing API key";
        QwenAudioDebugLog("event=connect_rejected attempt=%llu phase=validate reason=missing_api_key model=%s",
                          static_cast<unsigned long long>(impl_->config.attemptId), modelLog.c_str());
        return false;
    }
    if (!qwen_audio_json::IsValidVocabulary(impl_->config.vocabulary, &error)) {
        impl_->lastFailureRetryable.store(false);
        QwenAudioDebugLog("event=connect_rejected attempt=%llu phase=validate reason=invalid_vocabulary model=%s",
                          static_cast<unsigned long long>(impl_->config.attemptId), modelLog.c_str());
        return false;
    }
    Url url = ParseUrl(impl_->config.baseUrl, error);
    if (!error.empty()) {
        impl_->lastFailureRetryable.store(false);
        QwenAudioDebugLog("event=connect_rejected attempt=%llu phase=validate reason=invalid_endpoint model=%s error=%s",
                          static_cast<unsigned long long>(impl_->config.attemptId),
                          modelLog.c_str(), TruncateLogText(error).c_str());
        return false;
    }
    const std::string hostLog = WideToUtf8(url.host);
    const std::string pathLog = WideToUtf8(url.path);
    QwenAudioDebugLog("event=connect_start attempt=%llu phase=handshake model=%s host=%s path=%s port=%u",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      modelLog.c_str(), hostLog.c_str(), pathLog.c_str(),
                      static_cast<unsigned>(url.port));

    winhttp_websocket::ConnectOptions connectOptions;
    connectOptions.host = url.host;
    connectOptions.port = url.port;
    connectOptions.pathAndQuery = url.path;
    connectOptions.headers = L"Authorization: Bearer " + impl_->config.apiKey + L"\r\n";
    connectOptions.secure = true;
    connectOptions.timeoutMs = kConnectTimeoutMs;
    connectOptions.closeTimeoutMs = 1000;
    connectOptions.keepAliveMs = 30000;
    winhttp_websocket::HandshakeDiagnostics diagnostics;
    if (!impl_->transport.Connect(connectOptions, diagnostics, error)) {
        const bool statusRetryable = diagnostics.statusCode == 408 || diagnostics.statusCode == 409 ||
            diagnostics.statusCode == 425 || diagnostics.statusCode == 429 ||
            (diagnostics.statusCode >= 500 && diagnostics.statusCode <= 599);
        if (diagnostics.statusCode != 0) {
            impl_->lastFailureRetryable.store(statusRetryable);
        }
        QwenAudioDebugLog(
            "event=connect_end attempt=%llu ok=0 phase=handshake status=%lu retryable=%d request_id=%s trace_id=%s secure_flags=%lu body=%s error=%s elapsed_ms=%llu",
            static_cast<unsigned long long>(impl_->config.attemptId),
            static_cast<unsigned long>(diagnostics.statusCode),
            impl_->lastFailureRetryable.load() ? 1 : 0,
            WideToUtf8(diagnostics.requestId).c_str(),
            WideToUtf8(diagnostics.traceId).c_str(),
            static_cast<unsigned long>(diagnostics.secureFailureFlags),
            diagnostics.responseBody.c_str(),
            TruncateLogText(error).c_str(),
            static_cast<unsigned long long>(GetTickCount64() - connectStartTick));
        return false;
    }
    QwenAudioDebugLog(
        "event=handshake_response attempt=%llu phase=handshake status=%lu request_id=%s trace_id=%s secure_flags=%lu",
        static_cast<unsigned long long>(impl_->config.attemptId),
        static_cast<unsigned long>(diagnostics.statusCode),
        WideToUtf8(diagnostics.requestId).c_str(),
        WideToUtf8(diagnostics.traceId).c_str(),
        static_cast<unsigned long>(diagnostics.secureFailureFlags));

    const std::string taskId = TaskId();
    impl_->SetTaskId(taskId);
    const std::string task = BuildRunTaskMessage(impl_->config, taskId);
    DWORD sendError = NO_ERROR;
    if (!impl_->transport.Send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                               task.data(), task.size(), kSendTimeoutMs,
                               sendError, error)) {
        QwenAudioDebugLog("event=connect_failed attempt=%llu task_id=%s phase=run_task_send error=%lu error_text=%s elapsed_ms=%llu",
                          static_cast<unsigned long long>(impl_->config.attemptId),
                          taskId.c_str(),
                          static_cast<unsigned long>(sendError),
                          WideToUtf8(winhttp_websocket::FormatWinHttpError(sendError)).c_str(),
                          static_cast<unsigned long long>(GetTickCount64() - connectStartTick));
        return false;
    }
    QwenAudioDebugLog("event=run_task_sent attempt=%llu task_id=%s phase=task_start elapsed_ms=%llu",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      taskId.c_str(),
                      static_cast<unsigned long long>(GetTickCount64() - connectStartTick));
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
            const std::string messageLog = TruncateLogText(ready.message);
            const std::string codeLog = TruncateLogText(ready.errorCode);
            QwenAudioDebugLog("event=task_start_failed attempt=%llu task_id=%s phase=task_start retryable=%d error_code=%s message=%s",
                              static_cast<unsigned long long>(impl_->config.attemptId),
                              taskId.c_str(), ready.retryable ? 1 : 0,
                              codeLog.c_str(), messageLog.c_str());
            if (error.empty()) error = ready.message.empty() ? L"task-started failed" : ready.message;
            break;
        }
        if (ready.taskStarted) {
            QwenAudioDebugLog("event=task_started attempt=%llu task_id=%s phase=streaming elapsed_ms=%llu",
                              static_cast<unsigned long long>(impl_->config.attemptId),
                              taskId.c_str(),
                              static_cast<unsigned long long>(GetTickCount64() - connectStartTick));
            return true;
        }
    }
    if (error.empty()) error = L"timed out waiting for task-started";
    QwenAudioDebugLog("event=connect_end attempt=%llu task_id=%s ok=0 phase=task_started_wait error=%s elapsed_ms=%llu",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      taskId.c_str(), TruncateLogText(error).c_str(),
                      static_cast<unsigned long long>(GetTickCount64() - connectStartTick));
    Close();
    return false;
}

bool Client::SendAudio(const BYTE* data, size_t bytes, std::wstring& error) {
    if (!data || bytes == 0) return true;
    const std::string taskId = impl_->TaskIdSnapshot();
    DWORD sendError = NO_ERROR;
    if (!impl_->transport.Send(WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                               data, bytes, kSendTimeoutMs, sendError, error)) {
        QwenAudioDebugLog("event=send_audio_failed attempt=%llu task_id=%s phase=streaming chunk_bytes=%zu audio_bytes=%zu error=%lu error_text=%s",
                          static_cast<unsigned long long>(impl_->config.attemptId),
                          taskId.c_str(), bytes, impl_->audioBytes.load(),
                          static_cast<unsigned long>(sendError),
                          WideToUtf8(winhttp_websocket::FormatWinHttpError(sendError)).c_str());
        return false;
    }
    impl_->audioBytes.fetch_add(bytes);
    return true;
}

bool Client::Poll(DWORD timeoutMs, Event& event, std::wstring& error) {
    event = {};
    const std::string taskId = impl_->TaskIdSnapshot();
    std::string message;
    while (true) {
        auto received = impl_->transport.Receive(timeoutMs);
        if (received.kind == winhttp_websocket::ReceiveKind::Timeout) {
            event.timeout = true;
            error.clear();
            return false;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::Cancelled) {
            error = L"WebSocket operation cancelled";
            return false;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::Error) {
            error = L"WebSocket receive failed: " +
                winhttp_websocket::FormatWinHttpError(received.winhttpError);
            QwenAudioDebugLog("event=receive_failed attempt=%llu task_id=%s phase=streaming audio_bytes=%zu error=%lu error_text=%s timeout_ms=%lu",
                              static_cast<unsigned long long>(impl_->config.attemptId),
                              taskId.c_str(), impl_->audioBytes.load(),
                              static_cast<unsigned long>(received.winhttpError),
                              WideToUtf8(winhttp_websocket::FormatWinHttpError(received.winhttpError)).c_str(),
                              static_cast<unsigned long>(timeoutMs));
            return false;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::PeerClosed) {
            event.failed = true;
            event.retryable = true;
            event.peerClosed = true;
            event.message = L"WebSocket closed before task-finished";
            error = event.message;
            QwenAudioDebugLog("event=peer_close_before_task_finished attempt=%llu task_id=%s phase=finalize audio_bytes=%zu retryable=1 close_status=%u reason_chars=%zu",
                              static_cast<unsigned long long>(impl_->config.attemptId),
                              taskId.c_str(), impl_->audioBytes.load(),
                              static_cast<unsigned>(received.closeStatus),
                              received.closeReason.size());
            return true;
        }
        if (received.bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            received.bufferType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE) {
            return true;
        }
        message.append(reinterpret_cast<const char*>(received.data.data()), received.data.size());
        if (received.bufferType != WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE) break;
    }
    const Event parsed = ParseServerEventMessage(message);
    event = parsed;
    if (event.failed) error = event.message;
    return true;
}

bool Client::Finish(std::wstring& error) {
    if (impl_->cancelled.load()) {
        error = L"WebSocket operation cancelled";
        return false;
    }
    const std::string taskId = impl_->TaskIdSnapshot();
    const std::string msg = BuildFinishTaskMessage(taskId);
    DWORD sendError = NO_ERROR;
    if (!impl_->transport.Send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                               msg.data(), msg.size(), kSendTimeoutMs,
                               sendError, error)) {
        QwenAudioDebugLog("event=finish_task_failed attempt=%llu task_id=%s phase=finalize audio_bytes=%zu error=%lu error_text=%s",
                          static_cast<unsigned long long>(impl_->config.attemptId),
                          taskId.c_str(), impl_->audioBytes.load(),
                          static_cast<unsigned long>(sendError),
                          WideToUtf8(winhttp_websocket::FormatWinHttpError(sendError)).c_str());
        return false;
    }
    QwenAudioDebugLog("event=finish_task_sent attempt=%llu task_id=%s phase=finalize audio_bytes=%zu",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      taskId.c_str(), impl_->audioBytes.load());
    return true;
}

void Client::Abort() {
    impl_->cancelled.store(true);
    const std::string taskId = impl_->TaskIdSnapshot();
    QwenAudioDebugLog("event=client_abort attempt=%llu task_id=%s phase=abort audio_bytes=%zu",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      taskId.c_str(), impl_->audioBytes.load());
    impl_->transport.Abort();
    impl_->connected.store(false);
}

void Client::Close() {
    impl_->transport.Close();
    impl_->connected.store(false);
}

bool Client::LastFailureRetryable() const {
    return impl_->lastFailureRetryable.load();
}

TestResult TestConnection(const Config& cfg) {
    TestResult r;
    Client c(cfg);
    std::wstring error;

    // Keep an outer Settings-level deadline in addition to the transport's
    // per-phase deadlines so a future client regression cannot strand the
    // detached connection-test worker indefinitely.
    std::atomic<bool> connectDone{false};
    bool connectOk = false;
    std::wstring connectError;
    std::thread connectThread([&]() {
        connectOk = c.Connect(connectError);
        connectDone.store(true);
    });
    const ULONGLONG connectDeadline = GetTickCount64() + 12000;
    while (!connectDone.load() && GetTickCount64() < connectDeadline) Sleep(20);
    const bool connectTimedOut = !connectDone.load();
    if (connectTimedOut) c.Abort();
    if (connectThread.joinable()) connectThread.join();
    error = connectTimedOut ? L"timed out waiting for WebSocket handshake" : connectError;
    r.ok = !connectTimedOut && connectOk;

    if (r.ok) {
        std::vector<BYTE> silence(3200, 0); // 100 ms, 16 kHz mono s16le.
        if (!c.SendAudio(silence.data(), silence.size(), error)) r.ok = false;
    }
    if (r.ok && !c.Finish(error)) r.ok = false;
    if (r.ok) {
        std::atomic<bool> receiveDone{false};
        bool finished = false;
        std::wstring receiveError;
        std::thread receiveThread([&]() {
            while (!receiveDone.load()) {
                Event ev;
                std::wstring currentError;
                if (!c.Poll(0, ev, currentError)) {
                    if (!currentError.empty()) receiveError = currentError;
                    break;
                }
                if (ev.noSpeech || ev.taskFinished) { finished = true; break; }
                if (ev.failed) {
                    receiveError = ev.message.empty() ? L"task failed" : ev.message;
                    break;
                }
            }
            receiveDone.store(true);
        });
        const ULONGLONG deadline = GetTickCount64() + 7000;
        while (!receiveDone.load() && GetTickCount64() < deadline) Sleep(20);
        const bool receiveTimedOut = !receiveDone.load();
        if (receiveTimedOut) c.Abort();
        if (receiveThread.joinable()) receiveThread.join();
        error = receiveTimedOut
            ? L"timed out waiting for task-finished"
            : receiveError;
        r.ok = finished && !receiveTimedOut && error.empty();
    }
    r.message = r.ok ? L"Connection OK. Qwen Audio streaming endpoint is reachable."
                    : (error.empty() ? L"Connection failed." : error);
    c.Close();
    return r;
}

} // namespace qwen_audio_streaming
