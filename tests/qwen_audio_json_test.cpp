#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <wincrypt.h>

#include "qwen_audio_json.h"
#include "qwen_audio_http.h"
#include "qwen_audio_profile.h"
#include "qwen_audio_streaming.h"
#include "qwen_asr.h"
#include "qwen_context.h"
#include "qwen_finalize_policy.h"
#include "winhttp_websocket_transport.h"
#include "cloud_asr_common.h"
#include "pending_pcm_buffer.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
int failures = 0;

void Expect(bool condition, const char* description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
}

bool SendAll(SOCKET socket, const void* data, size_t bytes) {
    const char* cursor = static_cast<const char*>(data);
    while (bytes > 0) {
        const int chunk = send(socket, cursor,
                               static_cast<int>((std::min<size_t>)(bytes, 16384)), 0);
        if (chunk <= 0) return false;
        cursor += chunk;
        bytes -= static_cast<size_t>(chunk);
    }
    return true;
}

std::string ReceiveHttpHeaders(SOCKET socket) {
    std::string request;
    char buffer[1024];
    while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) return {};
        request.append(buffer, buffer + received);
    }
    return request;
}

std::string HeaderValue(const std::string& headers, const std::string& name) {
    const std::string needle = name + ":";
    size_t line = 0;
    while (line < headers.size()) {
        const size_t end = headers.find("\r\n", line);
        const size_t lineEnd = end == std::string::npos ? headers.size() : end;
        if (lineEnd >= line + needle.size() &&
            _strnicmp(headers.data() + line, needle.c_str(), needle.size()) == 0) {
            size_t valueStart = line + needle.size();
            while (valueStart < lineEnd &&
                   (headers[valueStart] == ' ' || headers[valueStart] == '\t')) {
                ++valueStart;
            }
            return headers.substr(valueStart, lineEnd - valueStart);
        }
        if (end == std::string::npos) break;
        line = end + 2;
    }
    return {};
}

std::string Base64Encode(const BYTE* data, DWORD bytes) {
    DWORD chars = 0;
    if (!CryptBinaryToStringA(data, bytes,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              nullptr, &chars)) {
        return {};
    }
    std::string out(chars, '\0');
    if (!CryptBinaryToStringA(data, bytes,
                              CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
                              out.data(), &chars)) {
        return {};
    }
    if (chars > 0 && out[chars - 1] == '\0') --chars;
    out.resize(chars);
    return out;
}

std::string WebSocketAccept(const std::string& key) {
    const std::string source = key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD digestBytes = 0;
    DWORD resultBytes = 0;
    std::vector<BYTE> object;
    std::vector<BYTE> digest;
    bool ok = false;

    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA1_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &resultBytes, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&digestBytes), sizeof(digestBytes),
                          &resultBytes, 0) == 0) {
        object.resize(objectBytes);
        digest.resize(digestBytes);
        ok = BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
                              nullptr, 0, 0) == 0 &&
             BCryptHashData(hash,
                            reinterpret_cast<PUCHAR>(const_cast<char*>(source.data())),
                            static_cast<ULONG>(source.size()), 0) == 0 &&
             BCryptFinishHash(hash, digest.data(), digestBytes, 0) == 0;
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return ok ? Base64Encode(digest.data(), static_cast<DWORD>(digest.size())) : std::string();
}

enum class LoopbackMode {
    BlackHoleHandshake,
    RejectHandshake,
    SilentAfterHandshake,
    PeerClose,
};

class LoopbackWebSocketServer {
public:
    explicit LoopbackWebSocketServer(LoopbackMode mode) : mode_(mode) {}

    ~LoopbackWebSocketServer() {
        Stop();
    }

    bool Start() {
        listenSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSocket_ == INVALID_SOCKET) return false;

        sockaddr_in address = {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listenSocket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            listen(listenSocket_, 1) != 0) {
            Stop();
            return false;
        }
        int addressBytes = sizeof(address);
        if (getsockname(listenSocket_, reinterpret_cast<sockaddr*>(&address),
                        &addressBytes) != 0) {
            Stop();
            return false;
        }
        port_ = ntohs(address.sin_port);
        worker_ = std::thread([this]() { Run(); });
        return true;
    }

    INTERNET_PORT Port() const {
        return port_;
    }

    bool WaitForRequest(DWORD timeoutMs = 1000) const {
        const ULONGLONG deadline = GetTickCount64() + timeoutMs;
        while (!requestReceived_.load() && GetTickCount64() < deadline) {
            Sleep(5);
        }
        return requestReceived_.load();
    }

private:
    void Stop() {
        SOCKET client = clientSocket_.exchange(INVALID_SOCKET);
        if (client != INVALID_SOCKET) {
            shutdown(client, SD_BOTH);
            closesocket(client);
        }
        SOCKET listener = std::exchange(listenSocket_, INVALID_SOCKET);
        if (listener != INVALID_SOCKET) {
            shutdown(listener, SD_BOTH);
            closesocket(listener);
        }
        if (worker_.joinable()) worker_.join();
    }

    void CloseAcceptedSocket(SOCKET socketValue) {
        SOCKET expected = socketValue;
        if (clientSocket_.compare_exchange_strong(expected, INVALID_SOCKET)) {
            shutdown(socketValue, SD_BOTH);
            closesocket(socketValue);
        }
    }

    void Run() {
        SOCKET accepted = accept(listenSocket_, nullptr, nullptr);
        if (accepted == INVALID_SOCKET) return;
        clientSocket_.store(accepted);
        DWORD receiveTimeoutMs = 5000;
        setsockopt(accepted, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&receiveTimeoutMs),
                   sizeof(receiveTimeoutMs));

        const std::string request = ReceiveHttpHeaders(accepted);
        if (request.empty()) {
            CloseAcceptedSocket(accepted);
            return;
        }
        requestReceived_.store(true);

        if (mode_ == LoopbackMode::BlackHoleHandshake) {
            char buffer[1024];
            while (recv(accepted, buffer, sizeof(buffer), 0) > 0) {
            }
            CloseAcceptedSocket(accepted);
            return;
        }

        if (mode_ == LoopbackMode::RejectHandshake) {
            const std::string body = "forbidden";
            const std::string response =
                "HTTP/1.1 403 Forbidden\r\n"
                "Content-Length: 9\r\n"
                "Content-Type: text/plain\r\n"
                "x-dashscope-request-id: request-test\r\n"
                "x-trace-id: trace-test\r\n"
                "Connection: close\r\n\r\n" + body;
            SendAll(accepted, response.data(), response.size());
            CloseAcceptedSocket(accepted);
            return;
        }

        const std::string acceptValue = WebSocketAccept(
            HeaderValue(request, "Sec-WebSocket-Key"));
        if (acceptValue.empty()) {
            CloseAcceptedSocket(accepted);
            return;
        }
        const std::string response =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + acceptValue + "\r\n\r\n";
        if (!SendAll(accepted, response.data(), response.size())) {
            CloseAcceptedSocket(accepted);
            return;
        }

        if (mode_ == LoopbackMode::PeerClose) {
            Sleep(50);
            const BYTE closeFrame[] = {0x88, 0x05, 0x03, 0xE8, 'b', 'y', 'e'};
            SendAll(accepted, closeFrame, sizeof(closeFrame));
        }

        char buffer[1024];
        while (recv(accepted, buffer, sizeof(buffer), 0) > 0) {
        }
        CloseAcceptedSocket(accepted);
    }

    LoopbackMode mode_;
    SOCKET listenSocket_ = INVALID_SOCKET;
    std::atomic<SOCKET> clientSocket_{INVALID_SOCKET};
    std::atomic<bool> requestReceived_{false};
    INTERNET_PORT port_ = 0;
    std::thread worker_;
};

winhttp_websocket::ConnectOptions LoopbackOptions(INTERNET_PORT port) {
    winhttp_websocket::ConnectOptions options;
    options.host = L"127.0.0.1";
    options.port = port;
    options.pathAndQuery = L"/websocket-test";
    options.secure = false;
    options.disableProxy = true;
    options.timeoutMs = 2000;
    options.closeTimeoutMs = 500;
    options.keepAliveMs = 0;
    return options;
}
} // namespace

int main() {
    WSADATA winsock = {};
    const bool winsockReady = WSAStartup(MAKEWORD(2, 2), &winsock) == 0;
    Expect(winsockReady, "WinSock initializes for loopback WebSocket lifecycle tests");

    Expect(qwen_audio_json::IsValidValue(L"{\"word\":5}"),
           "valid JSON object is accepted");
    Expect(!qwen_audio_json::IsValidValue(L"{\"word\":}"),
           "malformed JSON object is rejected");
    Expect(qwen_audio_json::IsValidVocabulary(L"{\"VoxType\":5}"),
           "valid vocabulary weights are accepted");
    Expect(qwen_audio_json::IsValidVocabulary(L"{\"super\":50}"),
           "weight 50 is accepted");
    Expect(!qwen_audio_json::IsValidVocabulary(L"{\"word\":6}"),
           "unsupported vocabulary weight is rejected");
    Expect(!qwen_audio_json::IsValidVocabulary(L"[\"word\"]"),
           "vocabulary arrays are rejected");
    Expect(!qwen_audio_json::IsValidVocabulary(L"{\"word\":1.0}"),
           "fractional vocabulary weights are rejected");
    Expect(qwen_audio_json::IsValidVocabulary(L"{\"Human immunodeficiency virus type 1\":4}"),
           "ASCII vocabulary terms with at most seven segments are accepted");
    Expect(!qwen_audio_json::IsValidVocabulary(
               L"{\"one two three four five six seven eight\":4}"),
           "ASCII vocabulary terms over seven segments are rejected");
    Expect(qwen_audio_json::IsValidVocabulary(L"{\"厄洛替尼盐酸盐\":4}"),
           "non-ASCII vocabulary terms within fifteen characters are accepted");
    Expect(!qwen_audio_json::IsValidVocabulary(L"{\"一二三四五六七八九十一二三四五六\":4}"),
           "non-ASCII vocabulary terms over fifteen characters are rejected");
    Expect(!qwen_audio_json::HasValidVocabulary(L""),
           "empty vocabulary is omitted instead of emitting a missing JSON value");
    Expect(qwen_audio_json::HasValidVocabulary(L"{\"VoxType\":5}"),
           "non-empty valid vocabulary is emitted");

    {
        std::wstring model = L"qwen-audio-3.0-asr-flash-streaming";
        std::wstring transport = L"audio_streaming";
        qwen_audio_profile::NormalizePersistedProfile(model, transport, false, false);
        Expect(model == qwen_audio_profile::kLegacyModel &&
                   transport == qwen_audio_profile::kLegacyTransport,
               "pre-selector config migrates to legacy realtime");
    }
    {
        std::wstring model = L"qwen-audio-3.0-asr-flash-streaming";
        std::wstring transport = L"legacy_realtime";
        qwen_audio_profile::NormalizePersistedProfile(model, transport, true, true);
        Expect(transport == qwen_audio_profile::kStreamingTransport,
               "known model repairs mismatched transport");
    }
    {
        std::wstring model = L"future-qwen-model";
        std::wstring transport = L"audio_http";
        qwen_audio_profile::NormalizePersistedProfile(model, transport, true, true);
        Expect(model == qwen_audio_profile::kLegacyModel &&
                   transport == qwen_audio_profile::kLegacyTransport,
               "unknown model falls back to legacy profile");
    }

    {
        const std::vector<BYTE> pcm = {0, 1, 2, 3};
        const std::vector<BYTE> wav = qwen_audio_http::BuildWavForPcm(pcm);
        Expect(wav.size() == 48, "WAV header wraps PCM with the expected size");
        Expect(wav.size() >= 44 && std::memcmp(wav.data(), "RIFF", 4) == 0 &&
                   std::memcmp(wav.data() + 8, "WAVE", 4) == 0,
               "WAV header contains RIFF/WAVE markers");
        Expect(qwen_audio_http::EncodeBase64ForTest(wav).size() > 0,
               "WAV base64 encoding produces data");
    }

    {
        CloudAsrReplayBuffer replay(4);
        Expect(replay.Append(std::vector<BYTE>{1, 2, 3, 4}) == CloudReplayAppendResult::Stored &&
                   replay.Size() == 4 && replay.Available(),
               "bounded replay stores audio up to its exact limit");
        Expect(replay.Append(std::vector<BYTE>{5}) == CloudReplayAppendResult::LimitExceeded &&
                   !replay.Available() && replay.Size() == 4,
               "bounded replay disables itself instead of growing past its limit");
    }

    {
        PendingPcmBuffer pending(4);
        const BYTE first[] = {1, 2, 3, 4};
        const BYTE extra[] = {5};
        Expect(pending.Append(first, sizeof(first)) && !pending.Overflowed(),
               "pending PCM accepts data within its bound");
        Expect(!pending.Append(extra, sizeof(extra)) && pending.Overflowed(),
               "pending PCM reports overflow instead of silently dropping data");
        std::vector<BYTE> drained;
        pending.SwapTo(drained);
        pending.Clear();
        Expect(drained.size() == 4 && !pending.Overflowed(),
               "pending PCM can be drained and reset after overflow");
    }

    {
        qwen_audio_http::Config http;
        http.model = L"qwen-audio-3.0-asr-flash";
        http.languageHints = L"zh,en";
        http.vocabulary = L"{\"VoxType\":5}";
        http.inputContextText = L"前文 \"context\"";
        const std::string json = qwen_audio_http::BuildRequestJsonForTest(http, "AQID");
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(json)),
               "HTTP request JSON is syntactically valid");
        Expect(json.find("\"language_hints\"") != std::string::npos &&
                   json.find("\"zh\"") != std::string::npos &&
                   json.find("\"en\"") != std::string::npos,
               "HTTP request carries language hints as an array");
        Expect(json.find("\"vocabulary\":{\"VoxType\":5}") != std::string::npos,
               "HTTP request carries immediate vocabulary");
        Expect(json.find("\"type\":\"input_text\"") != std::string::npos &&
                   json.find("前文") != std::string::npos &&
                   json.find("input_text") < json.find("input_audio"),
               "HTTP request places input-field context before audio");
        Expect(json.find("\"sample_rate\":\"16000\"") != std::string::npos,
               "HTTP request serializes sample_rate with the documented string type");
        Expect(json.find("semantic_punctuation") == std::string::npos &&
                   json.find("max_sentence_silence") == std::string::npos,
               "HTTP request omits streaming-only parameters");
        Expect(qwen_audio_http::IsNoSpeechResponseForTest(
                   400, R"({"message":"ASR_RESPONSE_HAVE_NO_WORDS"})"),
               "Audio 3 no-words HTTP response is recognized as no speech");
        Expect(!qwen_audio_http::IsNoSpeechResponseForTest(
                   400, R"({"message":"invalid parameter"})"),
               "generic HTTP 400 remains an operational error");
        http.vocabulary.clear();
        const std::string noVocab = qwen_audio_http::BuildRequestJsonForTest(http, "AQID");
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(noVocab)) &&
                   noVocab.find("\"vocabulary\":") == std::string::npos,
               "HTTP request omits empty vocabulary without corrupting JSON");

        const std::string officialResponse =
            R"({"output":{"output":{"sentence":{"text":"识别文本"}},"text":"识别文本"}})";
        Expect(qwen_audio_http::ParseResponseTextForTest(officialResponse) == L"识别文本",
               "HTTP parser accepts the official nested output.output.sentence.text response");
        const std::string unrelatedTextFirst =
            R"({"diagnostic":{"text":"不要返回我"},"output":{"output":{"sentence":{"text":"正确文本"}},"text":"备用文本"}})";
        Expect(qwen_audio_http::ParseResponseTextForTest(unrelatedTextFirst) == L"正确文本",
               "HTTP parser reads the documented nested path instead of the first text key");
        const std::string outputTextOnly =
            R"({"diagnostic":{"text":"不要返回我"},"output":{"text":"备用文本"}})";
        Expect(qwen_audio_http::ParseResponseTextForTest(outputTextOnly) == L"备用文本",
               "HTTP parser falls back to output.text by exact path");
    }

    {
        InputContextResult input;
        input.inputFieldText = std::wstring(401, L'甲');
        const std::wstring sanitized = qwen_context::SanitizeText(input);
        Expect(sanitized.size() == 400 && sanitized.front() == L'甲' && sanitized.back() == L'甲',
               "context keeps the first 400 characters and drops character 401");
    }

    {
        qwen_audio_streaming::Config streaming;
        streaming.model = L"qwen-audio-3.0-asr-flash-streaming";
        streaming.languageHints = L"zh";
        streaming.inputContextText = L"Bulge Bracket";
        const std::string runTask = qwen_audio_streaming::BuildRunTaskMessage(streaming, "task-1");
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(runTask)),
               "streaming run-task JSON is syntactically valid");
        const size_t parametersPos = runTask.find("\"parameters\":");
        const size_t inputPos = runTask.find("\"input\":");
        Expect(parametersPos != std::string::npos && inputPos != std::string::npos &&
                   parametersPos < inputPos,
               "streaming run-task follows the documented parameter/input layout");
        Expect(runTask.find("\"context\":") != std::string::npos &&
                   runTask.find("input_text") != std::string::npos,
               "streaming run-task carries input.context text");
        const std::string finishTask = qwen_audio_streaming::BuildFinishTaskMessage("task-1");
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(finishTask)),
               "streaming finish-task JSON is syntactically valid");

        const auto started = qwen_audio_streaming::ParseServerEventMessage(
            R"({"header":{"event":"task-started"},"payload":{}})");
        Expect(started.taskStarted, "task-started event is recognized");
        const auto partial = qwen_audio_streaming::ParseServerEventMessage(
            R"({"header":{"event":"result-generated"},"payload":{"output":{"sentence":{"text":"你好","sentence_end":false,"heartbeat":false}}}})");
        Expect(partial.text == L"你好" && !partial.sentenceEnd && !partial.heartbeat,
               "result-generated partial event is parsed");
        const auto failed = qwen_audio_streaming::ParseServerEventMessage(
            R"({"header":{"event":"task-failed","error_message":"bad request"},"payload":{}})");
        Expect(failed.failed && failed.message == L"bad request" && !failed.retryable,
               "task-failed is parsed as a non-retryable server rejection");
        const auto noSpeech = qwen_audio_streaming::ParseServerEventMessage(
            R"({"header":{"event":"task-failed","error_message":"ASR_RESPONSE_HAVE_NO_WORDS"},"payload":{}})");
        Expect(noSpeech.noSpeech,
               "streaming no-words response is recognized as no speech");
        const auto transient = qwen_audio_streaming::ParseServerEventMessage(
            R"({"header":{"event":"task-failed","error_code":"InternalError","error_message":"internal server error"},"payload":{}})");
        Expect(transient.failed && transient.retryable && transient.errorCode == L"InternalError",
               "streaming transient error_code is eligible for bounded retry");
        const auto stable = qwen_audio_streaming::ParseServerEventMessage(
            R"({"header":{"event":"task-failed","error_code":"InvalidParameter","error_message":"invalid parameter"},"payload":{}})");
        Expect(stable.failed && !stable.retryable && stable.errorCode == L"InvalidParameter",
               "streaming stable configuration error is not retried");

        qwen_audio_streaming::TranscriptAccumulator transcript;
        transcript.Apply(partial);
        Expect(transcript.Text() == L"你好" && transcript.CommittedText().empty() &&
                   !transcript.HasCommittedText(),
               "streaming pending partial is not classified as committed final text");
        auto second = partial;
        second.text = L"你好";
        second.sentenceEnd = true;
        transcript.Apply(second);
        auto third = second;
        third.text = L"世界";
        third.sentenceEnd = true;
        transcript.Apply(third);
        Expect(transcript.Text() == L"你好 世界",
               "multiple sentence_end events accumulate without stale partial text");
        Expect(transcript.CommittedText() == L"你好 世界" && transcript.HasCommittedText(),
               "sentence_end events produce recoverable committed text");

        using qwen_finalize_policy::TerminalReason;
        Expect(qwen_finalize_policy::CanRecoverAudioStreaming(
                   true, false, true, TerminalReason::PeerClosed),
               "Audio streaming may recover committed text after finalize peer close");
        Expect(qwen_finalize_policy::CanRecoverAudioStreaming(
                   true, false, true, TerminalReason::Timeout),
               "Audio streaming may recover committed text after finalize timeout");
        Expect(!qwen_finalize_policy::CanRecoverAudioStreaming(
                   true, false, true, TerminalReason::ProviderFailure),
               "Audio streaming never hides an explicit task-failed event");
        Expect(!qwen_finalize_policy::CanRecoverAudioStreaming(
                   true, false, false, TerminalReason::PeerClosed),
               "Audio streaming never promotes a pending partial to final text");
        Expect(!qwen_finalize_policy::CanRecoverAudioStreaming(
                   false, false, true, TerminalReason::PeerClosed),
               "Audio streaming does not recover a mid-stream peer close");
    }

    {
        qwen_asr::QwenConfig realtime;
        realtime.model = L"qwen3-asr-flash-realtime";
        realtime.language = L"zh-CN";
        realtime.turnDetection = L"manual";
        const std::wstring endpoint = qwen_asr::BuildEndpointUrlForTest(realtime);
        Expect(endpoint == std::wstring(qwen_asr::kDefaultBaseUrl) +
                               L"?model=qwen3-asr-flash-realtime",
               "Qwen3 Realtime endpoint uses the dedicated realtime path and model query");

        const std::string sessionUpdate = qwen_asr::BuildSessionUpdateMessageForTest(realtime);
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(sessionUpdate)) &&
                   sessionUpdate.find("\"type\":\"session.update\"") != std::string::npos &&
                   sessionUpdate.find("\"input_audio_format\":\"pcm\"") != std::string::npos &&
                   sessionUpdate.find("\"sample_rate\":16000") != std::string::npos &&
                   sessionUpdate.find("\"turn_detection\":null") != std::string::npos,
               "Qwen3 session.update follows the documented Manual PCM session shape");
        realtime.language.clear();
        const std::string automaticLanguageUpdate =
            qwen_asr::BuildSessionUpdateMessageForTest(realtime);
        Expect(automaticLanguageUpdate.find("\"input_audio_transcription\"") == std::string::npos,
               "Qwen3 session.update omits input_audio_transcription when language is automatic");

        const BYTE pcm[] = {1, 2, 3};
        const std::string append = qwen_asr::BuildAudioAppendMessageForTest(pcm, sizeof(pcm));
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(append)) &&
                   append.find("\"type\":\"input_audio_buffer.append\"") != std::string::npos &&
                   append.find("\"audio\":\"AQID\"") != std::string::npos,
               "Qwen3 audio append carries base64 PCM");
        const std::string commit = qwen_asr::BuildCommitMessageForTest();
        const std::string finish = qwen_asr::BuildSessionFinishMessageForTest();
        Expect(commit.find("\"type\":\"input_audio_buffer.commit\"") != std::string::npos &&
                   finish.find("\"type\":\"session.finish\"") != std::string::npos,
               "Qwen3 Manual completion emits commit followed by session.finish events");

        const auto partial = qwen_asr::ParseServerEventForTest(
            R"({"type":"conversation.item.input_audio_transcription.text","text":"你","stash":"好"})");
        Expect(partial.partialText == L"你好" && !partial.transcriptionCompleted,
               "Qwen3 partial combines confirmed text and stash");
        const auto completed = qwen_asr::ParseServerEventForTest(
            R"({"type":"conversation.item.input_audio_transcription.completed","transcript":"你好"})");
        Expect(completed.transcriptionCompleted && completed.finalText == L"你好",
               "Qwen3 completed event is classified as final transcript");
        const auto sessionFinished = qwen_asr::ParseServerEventForTest(
            R"({"type":"session.finished"})");
        Expect(sessionFinished.sessionFinished && !sessionFinished.failed,
               "Qwen3 JSON session.finished is the normal session terminator");
        const auto error = qwen_asr::ParseServerEventForTest(
            R"({"type":"error","code":"InvalidParameter","message":"bad request"})");
        Expect(error.failed && error.error.find(L"InvalidParameter") != std::wstring::npos,
               "Qwen3 error event remains distinct from session.finished");

        using qwen_finalize_policy::TerminalReason;
        Expect(qwen_finalize_policy::CanRecoverRealtime(
                   true, false, true, TerminalReason::PeerClosed),
               "Qwen3 may recover completed text after peer close before session.finished");
        Expect(qwen_finalize_policy::CanRecoverRealtime(
                   true, false, true, TerminalReason::Timeout),
               "Qwen3 may recover completed text after session.finished timeout");
        Expect(!qwen_finalize_policy::CanRecoverRealtime(
                   true, false, true, TerminalReason::ProviderFailure),
               "Qwen3 completed text does not hide a later provider error");
        Expect(!qwen_finalize_policy::CanRecoverRealtime(
                   false, false, true, TerminalReason::PeerClosed),
               "Qwen3 peer close without a completed event is not successful");
    }

    if (winsockReady) {
        {
            LoopbackWebSocketServer server(LoopbackMode::BlackHoleHandshake);
            const bool serverStarted = server.Start();
            Expect(serverStarted, "loopback black-hole handshake server starts");
            if (serverStarted) {
                winhttp_websocket::Transport transport;
                winhttp_websocket::HandshakeDiagnostics diagnostics;
                std::wstring error;
                bool connected = false;
                std::thread connectThread([&]() {
                    connected = transport.Connect(
                        LoopbackOptions(server.Port()), diagnostics, error);
                });
                Expect(server.WaitForRequest(),
                       "black-hole server receives the WebSocket upgrade request");
                const auto abortStart = std::chrono::steady_clock::now();
                transport.Abort();
                connectThread.join();
                const auto abortMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - abortStart).count();
                Expect(!connected && abortMs <= 1500,
                       "Abort cancels a pending asynchronous handshake within the close deadline");
            }
        }

        {
            LoopbackWebSocketServer server(LoopbackMode::RejectHandshake);
            const bool serverStarted = server.Start();
            Expect(serverStarted, "loopback rejection server starts");
            if (serverStarted) {
                winhttp_websocket::Transport transport;
                winhttp_websocket::HandshakeDiagnostics diagnostics;
                std::wstring error;
                const bool connected = transport.Connect(
                    LoopbackOptions(server.Port()), diagnostics, error);
                Expect(!connected && diagnostics.statusCode == 403 &&
                           diagnostics.requestId == L"request-test" &&
                           diagnostics.traceId == L"trace-test" &&
                           diagnostics.responseBody == "forbidden",
                       "non-101 handshake captures status, request/trace ids, and bounded body");
            }
        }

        {
            LoopbackWebSocketServer server(LoopbackMode::PeerClose);
            const bool serverStarted = server.Start();
            Expect(serverStarted, "loopback peer-close server starts");
            if (serverStarted) {
                winhttp_websocket::Transport transport;
                winhttp_websocket::HandshakeDiagnostics diagnostics;
                std::wstring error;
                const bool connected = transport.Connect(
                    LoopbackOptions(server.Port()), diagnostics, error);
                Expect(connected, "loopback WebSocket 101 handshake succeeds");
                if (connected) {
                    const auto received = transport.Receive(2000);
                    Expect(received.kind == winhttp_websocket::ReceiveKind::PeerClosed &&
                               received.closeStatus == 1000 && received.closeReason == L"bye",
                           "peer close remains a transport event with close status and reason");
                }
                transport.Close();
            }
        }

        {
            LoopbackWebSocketServer server(LoopbackMode::SilentAfterHandshake);
            const bool serverStarted = server.Start();
            Expect(serverStarted, "loopback silent server starts");
            if (serverStarted) {
                winhttp_websocket::Transport transport;
                winhttp_websocket::HandshakeDiagnostics diagnostics;
                std::wstring error;
                const bool connected = transport.Connect(
                    LoopbackOptions(server.Port()), diagnostics, error);
                Expect(connected, "silent loopback WebSocket handshake succeeds");
                if (connected) {
                    const std::string payload = "ping";
                    DWORD sendError = NO_ERROR;
                    Expect(transport.Send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                          payload.data(), payload.size(), 1000,
                                          sendError, error),
                           "asynchronous WebSocket send waits for WRITE_COMPLETE");
                    const auto pending = transport.Receive(100);
                    Expect(pending.kind == winhttp_websocket::ReceiveKind::Timeout,
                           "silent server leaves one asynchronous Receive pending");
                    const auto abortStart = std::chrono::steady_clock::now();
                    transport.Abort();
                    const auto abortMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - abortStart).count();
                    const auto cancelled = transport.Receive(100);
                    Expect(abortMs <= 1500 &&
                               cancelled.kind == winhttp_websocket::ReceiveKind::Cancelled,
                           "Abort cancels a pending Receive within the bounded close deadline");
                }
            }
        }
        WSACleanup();
    }

    Expect(qwen_audio_json::ExtractString(R"({"text":"\u4F60\u597D \uD83D\uDE00"})", "text") == L"你好 😀",
           "JSON string extraction decodes Unicode and surrogate pairs");
    if (failures == 0) {
        std::cout << "qwen_audio_json_test: PASS\n";
        return 0;
    }
    return 1;
}
