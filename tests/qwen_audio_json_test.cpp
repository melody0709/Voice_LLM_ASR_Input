#include "qwen_audio_json.h"
#include "qwen_audio_http.h"
#include "qwen_audio_profile.h"
#include "qwen_audio_streaming.h"
#include "cloud_asr_common.h"
#include "pending_pcm_buffer.h"

#include <cstring>
#include <iostream>
#include <vector>

namespace {
int failures = 0;

void Expect(bool condition, const char* description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
}
} // namespace

int main() {
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
        const std::string json = qwen_audio_http::BuildRequestJsonForTest(http, "AQID");
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(json)),
               "HTTP request JSON is syntactically valid");
        Expect(json.find("\"language_hints\"") != std::string::npos &&
                   json.find("\"zh\"") != std::string::npos &&
                   json.find("\"en\"") != std::string::npos,
               "HTTP request carries language hints as an array");
        Expect(json.find("\"vocabulary\":{\"VoxType\":5}") != std::string::npos,
               "HTTP request carries immediate vocabulary");
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
    }

    {
        qwen_audio_streaming::Config streaming;
        streaming.model = L"qwen-audio-3.0-asr-flash-streaming";
        streaming.languageHints = L"zh";
        const std::string runTask = qwen_audio_streaming::BuildRunTaskMessage(streaming, "task-1");
        Expect(qwen_audio_json::IsValidValue(Utf8ToWide(runTask)),
               "streaming run-task JSON is syntactically valid");
        Expect(runTask.find("\"parameters\":") < runTask.find("\"input\":{}"),
               "streaming run-task follows the documented parameter/input layout");
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

        qwen_audio_streaming::TranscriptAccumulator transcript;
        transcript.Apply(partial);
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
    }

    Expect(qwen_audio_json::ExtractString(R"({"text":"\u4F60\u597D \uD83D\uDE00"})", "text") == L"你好 😀",
           "JSON string extraction decodes Unicode and surrogate pairs");
    if (failures == 0) {
        std::cout << "qwen_audio_json_test: PASS\n";
        return 0;
    }
    return 1;
}
