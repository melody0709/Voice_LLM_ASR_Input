#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "qwen_audio_streaming.h"

#include "asr_runtime_log.h"
#include "qwen_audio_json.h"
#include "qwen_context.h"
#include "qwen_special_word_filter.h"
#include "utils.h"
#include "winhttp_websocket_transport.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <condition_variable>
#include <cwctype>
#include <initializer_list>
#include <sstream>
#include <mutex>
#include <thread>
#include <chrono>
#include <vector>
#include <winhttp.h>
#include <objbase.h>

#pragma comment(lib, "winhttp.lib")

namespace qwen_audio_streaming {
namespace {

constexpr DWORD kConnectTimeoutMs = 8000;
constexpr DWORD kSendTimeoutMs = 5000;

// Settings changes invalidate the authenticated transport identity.  The
// epoch is captured when a Client is created so an in-flight task that began
// under the previous identity cannot be admitted to the idle pool after the
// change, even if it finishes after InvalidateReusableConnections().
std::atomic<uint64_t> g_reuseEpoch{1};

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
    qwen_special_word_filter::Config specialFilter;
    std::wstring filterError;
    if (qwen_special_word_filter::Normalize(
            cfg.specialWordReplaceList,
            cfg.specialWordEmptyList,
            cfg.systemReservedFilter,
            specialFilter, &filterError) &&
        qwen_special_word_filter::HasAny(specialFilter)) {
        json += ",\"special_word_filter\":" +
            qwen_special_word_filter::BuildJson(specialFilter);
    }
    std::wstring context = Trim(cfg.inputContextText);
    if (context.size() > qwen_context::kMaxContextCharacters) {
        context = input_context::TakeFirstN(context, qwen_context::kMaxContextCharacters);
    }
    if (context.empty()) {
        json += "},\"input\":{}}}";
    } else {
        json += "},\"input\":{\"context\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"";
        json += EscapeJson(context);
        json += "\"}]}]}}}";
    }
    return json;
}

std::string BuildContinueTaskMessageImpl(const std::string& taskId,
                                         const std::wstring& contextText) {
    std::wstring context = Trim(contextText);
    if (context.size() > qwen_context::kMaxContextCharacters) {
        context = input_context::TakeFirstN(context, qwen_context::kMaxContextCharacters);
    }
    std::string json = "{\"header\":{\"action\":\"continue-task\",\"task_id\":\"" +
        taskId + "\",\"streaming\":\"duplex\"},\"payload\":{\"input\":{";
    if (context.empty()) {
        // An explicit empty context lets the worker clear a stale initial
        // snapshot when the focused field was cleared before key release.
        json += "\"context\":[]}}}";
    } else {
        json += "\"context\":[{\"role\":\"user\",\"content\":[{\"type\":\"input_text\",\"text\":\"";
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

std::string BuildContinueTaskMessage(const std::string& taskId,
                                     const std::wstring& contextText) {
    return BuildContinueTaskMessageImpl(taskId, contextText);
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
    explicit Impl(Config c)
        : config(std::move(c)), reuseEpoch(g_reuseEpoch.load(std::memory_order_acquire)) {}
    Config config;
    const uint64_t reuseEpoch;
    winhttp_websocket::Transport transport;
    std::atomic<bool> connected{false};
    std::atomic<bool> taskFinished{false};
    std::atomic<bool> reusable{false};
    std::atomic<bool> reuseAllowed{true};
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
    impl_->taskFinished.store(false);
    impl_->reuseAllowed.store(true);
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
    const bool reuseTransport = impl_->reusable.load() && impl_->transport.IsConnected();
    // Once Connect claims an idle client, it is no longer visible to the
    // manager.  Any failure below will close the transport instead of putting
    // a half-started task back into the idle slot.
    impl_->reusable.store(false);
    const std::string hostLog = WideToUtf8(url.host);
    const std::string pathLog = WideToUtf8(url.path);
    QwenAudioDebugLog("event=connect_start attempt=%llu phase=%s model=%s host=%s path=%s port=%u reuse=%d",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      reuseTransport ? "reuse" : "handshake",
                      modelLog.c_str(), hostLog.c_str(), pathLog.c_str(),
                      static_cast<unsigned>(url.port), reuseTransport ? 1 : 0);

    winhttp_websocket::HandshakeDiagnostics diagnostics;
    if (!reuseTransport) {
        winhttp_websocket::ConnectOptions connectOptions;
        connectOptions.host = url.host;
        connectOptions.port = url.port;
        connectOptions.pathAndQuery = url.path;
        connectOptions.headers = L"Authorization: Bearer " + impl_->config.apiKey + L"\r\n";
        connectOptions.secure = true;
        connectOptions.timeoutMs = kConnectTimeoutMs;
        connectOptions.closeTimeoutMs = 1000;
        connectOptions.keepAliveMs = 30000;
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
    } else {
        QwenAudioDebugLog("event=connection_reused attempt=%llu phase=transport_idle model=%s",
                          static_cast<unsigned long long>(impl_->config.attemptId),
                          modelLog.c_str());
    }

    const std::string taskId = TaskId();
    impl_->SetTaskId(taskId);
    const std::string task = BuildRunTaskMessage(impl_->config, taskId);
    DWORD sendError = NO_ERROR;
    if (!impl_->transport.Send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                               task.data(), task.size(), kSendTimeoutMs,
                               sendError, error)) {
        impl_->transport.Close();
        impl_->connected.store(false);
        impl_->reusable.store(false);
        impl_->reuseAllowed.store(false);
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
    if (impl_->cancelled.load() || !impl_->connected.load()) {
        error = L"WebSocket operation cancelled";
        return false;
    }
    const std::string taskId = impl_->TaskIdSnapshot();
    DWORD sendError = NO_ERROR;
    if (!impl_->transport.Send(WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                               data, bytes, kSendTimeoutMs, sendError, error)) {
        impl_->reusable.store(false);
        impl_->reuseAllowed.store(false);
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

bool Client::ContinueContext(const std::wstring& contextText, std::wstring& error) {
    error.clear();
    if (impl_->cancelled.load() || !impl_->connected.load()) {
        error = L"WebSocket operation cancelled";
        return false;
    }
    if (impl_->taskFinished.load()) {
        error = L"task already finished";
        return false;
    }
    const std::string taskId = impl_->TaskIdSnapshot();
    const std::string msg = BuildContinueTaskMessage(taskId, contextText);
    DWORD sendError = NO_ERROR;
    if (!impl_->transport.Send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                               msg.data(), msg.size(), kSendTimeoutMs,
                               sendError, error)) {
        impl_->reusable.store(false);
        impl_->reuseAllowed.store(false);
        QwenAudioDebugLog(
            "event=continue_task_failed attempt=%llu task_id=%s phase=context_update error=%lu error_text=%s",
            static_cast<unsigned long long>(impl_->config.attemptId), taskId.c_str(),
            static_cast<unsigned long>(sendError),
            WideToUtf8(winhttp_websocket::FormatWinHttpError(sendError)).c_str());
        return false;
    }
    QwenAudioDebugLog(
        "event=continue_task_sent attempt=%llu task_id=%s phase=context_update context_chars=%zu",
        static_cast<unsigned long long>(impl_->config.attemptId), taskId.c_str(),
        contextText.size());
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
            impl_->reusable.store(false);
            impl_->reuseAllowed.store(false);
            impl_->connected.store(false);
            error = L"WebSocket operation cancelled";
            return false;
        }
        if (received.kind == winhttp_websocket::ReceiveKind::Error) {
            impl_->reusable.store(false);
            impl_->reuseAllowed.store(false);
            impl_->connected.store(false);
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
            impl_->reusable.store(false);
            impl_->reuseAllowed.store(false);
            impl_->connected.store(false);
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
    if (event.taskFinished) {
        impl_->taskFinished.store(true);
    }
    if (event.failed) {
        impl_->taskFinished.store(false);
        impl_->reusable.store(false);
        impl_->reuseAllowed.store(false);
    }
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
        impl_->reusable.store(false);
        impl_->reuseAllowed.store(false);
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
    impl_->reusable.store(false);
    impl_->reuseAllowed.store(false);
    impl_->taskFinished.store(false);
    const std::string taskId = impl_->TaskIdSnapshot();
    QwenAudioDebugLog("event=client_abort attempt=%llu task_id=%s phase=abort audio_bytes=%zu",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      taskId.c_str(), impl_->audioBytes.load());
    impl_->transport.Abort();
    impl_->connected.store(false);
}

void Client::Close() {
    impl_->reusable.store(false);
    impl_->reuseAllowed.store(false);
    impl_->taskFinished.store(false);
    impl_->transport.Close();
    impl_->connected.store(false);
}

bool Client::PrepareForReuse() {
    if (impl_->cancelled.load() || !impl_->connected.load() ||
        !impl_->taskFinished.load() || !impl_->reuseAllowed.load() ||
        !impl_->transport.IsConnected() ||
        impl_->reuseEpoch != g_reuseEpoch.load(std::memory_order_acquire)) {
        return false;
    }
    impl_->connected.store(false);
    impl_->reusable.store(true);
    QwenAudioDebugLog("event=connection_idle attempt=%llu task_id=%s idle_timeout_s=60",
                      static_cast<unsigned long long>(impl_->config.attemptId),
                      impl_->TaskIdSnapshot().c_str());
    return true;
}

bool Client::IsReusable() const {
    return impl_->reusable.load() && impl_->transport.IsConnected();
}

bool Client::MatchesReuseIdentity(const Config& config) const {
    return impl_->config.apiKey == config.apiKey &&
        Trim(impl_->config.baseUrl) == Trim(config.baseUrl) &&
        impl_->config.model == config.model;
}

bool Client::ReconfigureForReuse(Config config) {
    if (!IsReusable() || !MatchesReuseIdentity(config) ||
        impl_->reuseEpoch != g_reuseEpoch.load(std::memory_order_acquire)) {
        return false;
    }
    impl_->config = std::move(config);
    impl_->cancelled.store(false);
    impl_->connected.store(false);
    impl_->taskFinished.store(true);
    impl_->reuseAllowed.store(true);
    impl_->audioBytes.store(0);
    impl_->SetTaskId({});
    return true;
}

bool Client::LastFailureRetryable() const {
    return impl_->lastFailureRetryable.load();
}

namespace {

constexpr ULONGLONG kReusableIdleTimeoutMs = 60 * 1000;
#if defined(VOXTYPE_DISABLE_QWEN_AUDIO_CONNECTION_REUSE)
constexpr bool kConnectionReuseEnabled = false;
#else
constexpr bool kConnectionReuseEnabled = true;
#endif

class ReusableConnectionManager {
public:
    ReusableConnectionManager()
        : janitor_([this] { JanitorLoop(); }) {}

    ~ReusableConnectionManager() {
        std::unique_ptr<Client> idle;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            idle = std::move(idle_);
        }
        cv_.notify_all();
        if (janitor_.joinable()) janitor_.join();
        if (idle) idle->Close();
    }

    std::unique_ptr<Client> Acquire(const Config& config) {
        if (!kConnectionReuseEnabled) return nullptr;

        std::unique_ptr<Client> candidate;
        std::unique_ptr<Client> discarded;
        bool identityMismatch = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!idle_) return nullptr;
            const ULONGLONG age = GetTickCount64() - idleSince_;
            if (age >= kReusableIdleTimeoutMs) {
                discarded = std::move(idle_);
            } else if (!idle_->MatchesReuseIdentity(config)) {
                identityMismatch = true;
                discarded = std::move(idle_);
            } else {
                candidate = std::move(idle_);
            }
        }

        if (discarded) {
            QwenAudioDebugLog("event=connection_idle_discard reason=%s",
                              identityMismatch ? "identity_changed" : "idle_timeout");
            discarded->Close();
        }
        if (!candidate) return nullptr;
        if (!candidate->ReconfigureForReuse(config)) {
            QwenAudioDebugLog("event=connection_idle_discard reason=transport_not_reusable");
            candidate->Close();
            return nullptr;
        }
        QwenAudioDebugLog("event=connection_reuse_hit model=%s",
                          WideToUtf8(config.model).c_str());
        return candidate;
    }

    void Release(std::unique_ptr<Client> client) {
        if (!client) return;
        std::unique_ptr<Client> replaced;
        bool accepted = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Keep the admission check under the same mutex as Invalidate():
            // either the task wins the race and is immediately invalidated,
            // or it observes the new epoch and is closed instead of becoming
            // a stale idle candidate.
            if (kConnectionReuseEnabled && client->PrepareForReuse()) {
                replaced = std::move(idle_);
                idleSince_ = GetTickCount64();
                idle_ = std::move(client);
                accepted = true;
            }
        }
        if (!accepted) {
            client->Close();
            return;
        }
        if (replaced) {
            QwenAudioDebugLog("event=connection_idle_replaced reason=new_successful_task");
            replaced->Close();
        }
        cv_.notify_all();
    }

    void Invalidate() {
        std::unique_ptr<Client> discarded;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            g_reuseEpoch.fetch_add(1, std::memory_order_acq_rel);
            discarded = std::move(idle_);
        }
        if (discarded) {
            QwenAudioDebugLog("event=connection_idle_discard reason=config_changed");
            discarded->Close();
        }
    }

private:
    void JanitorLoop() {
        while (true) {
            std::unique_ptr<Client> expired;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::seconds(1), [this] { return stopping_; });
                if (stopping_) return;
                if (idle_ && GetTickCount64() - idleSince_ >= kReusableIdleTimeoutMs) {
                    expired = std::move(idle_);
                }
            }
            if (expired) {
                QwenAudioDebugLog("event=connection_idle_discard reason=idle_timeout");
                expired->Close();
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<Client> idle_;
    ULONGLONG idleSince_ = 0;
    bool stopping_ = false;
    std::thread janitor_;
};

ReusableConnectionManager& ConnectionManager() {
    static ReusableConnectionManager manager;
    return manager;
}

} // namespace

std::unique_ptr<Client> AcquireReusableClient(const Config& config) {
    return ConnectionManager().Acquire(config);
}

void ReleaseReusableClient(std::unique_ptr<Client> client) {
    ConnectionManager().Release(std::move(client));
}

void InvalidateReusableConnections() {
    ConnectionManager().Invalidate();
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
