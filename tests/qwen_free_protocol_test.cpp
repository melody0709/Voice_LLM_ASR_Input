#include "asr_result_policy.h"
#include "qwen_free_json.h"
#include "qwen_free_diagnostics.h"
#include "qwen_free_proto_asr.h"
#include "qwen_free_asr_json.h"
#include "pending_pcm_buffer.h"
#include "qwen_free_llm_json.h"
#include "qwen_free_postprocess.h"
#include "qwen_free_recovery_policy.h"
#include "qwen_free_proto_sign.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++g_failures;
}

bool IsLowerHex(const std::string& value) {
    for (const unsigned char c : value) {
        if (!std::isxdigit(c) || (std::isalpha(c) && std::isupper(c))) {
            return false;
        }
    }
    return true;
}

uint32_t ReadBe32(const std::vector<BYTE>& value, size_t offset) {
    if (offset + 4 > value.size()) return 0;
    return (static_cast<uint32_t>(value[offset]) << 24) |
           (static_cast<uint32_t>(value[offset + 1]) << 16) |
           (static_cast<uint32_t>(value[offset + 2]) << 8) |
           static_cast<uint32_t>(value[offset + 3]);
}

} // namespace

int main() {
    Expect(asr_result_policy::LooksLikeOperationalPrefix(
               L"Qwen IME (Free) error: timeout"),
           "Qwen Free watchdog failures are operational errors, not text to paste");
    Expect(asr_result_policy::LooksLikeOperationalPrefix(
               L"Qwen IME rewrite failed: empty output"),
           "Qwen rewrite failures are operational errors, not rewrite output");

    Expect(!qwen_free_recovery_policy::ShouldRetryConnect(
               L"WebSocket upgrade failed (HTTP 401): code=D30112 message=验签未通过"),
           "Qwen authentication/signature failures are not retried");
    Expect(!qwen_free_recovery_policy::ShouldRetryConnect(
               L"WebSocket upgrade failed (HTTP 404): path not found"),
           "Qwen endpoint/path failures are not retried");
    Expect(!qwen_free_recovery_policy::ShouldRetryConnect(
               L"ASR connect failed: native Qwen signer unavailable: unet.dll version is not supported"),
           "local signer incompatibility is not retried as a transport failure");
    Expect(qwen_free_recovery_policy::ShouldRetryConnect(
               L"WebSocket upgrade failed (HTTP 500) | x_u_vcode=1785860401404"),
           "timestamps containing 401/404 digits do not suppress transient retries");
    Expect(qwen_free_recovery_policy::ShouldRetryConnect(
               L"WinHttpWebSocketReceive failed: 12017"),
           "Qwen transport cancellation/close failures remain retryable");

    Expect(qwen_free_recovery_policy::ReplayOutcomeIsUsable(
               true, true, true, true, false),
           "replay accepts a genuine clean final after all PCM is sent");
    Expect(!qwen_free_recovery_policy::ReplayOutcomeIsUsable(
               true, false, true, true, false),
           "replay rejects a final produced before all PCM is sent");
    Expect(!qwen_free_recovery_policy::ReplayOutcomeIsUsable(
               true, true, true, false, false),
           "replay rejects a close/error terminal masquerading as final");
    Expect(!qwen_free_recovery_policy::ReplayOutcomeIsUsable(
               true, true, true, true, true),
           "replay rejects a terminal frame accompanied by a frame error");
    Expect(!qwen_free_recovery_policy::ReplayOutcomeIsUsable(
               true, true, true, true, false, false),
           "replay rejects a clean but empty terminal transcript");

    const auto disabled = qwen_free_postprocess::Normalize({false, false, false});
    Expect(!disabled.polish && !disabled.punctuate && !disabled.correct,
           "bundled Qwen post-processing stays disabled when all legacy flags are off");
    const auto legacyPunctOnly = qwen_free_postprocess::Normalize({false, true, false});
    Expect(legacyPunctOnly.polish && legacyPunctOnly.punctuate && legacyPunctOnly.correct,
           "legacy punctuation-only config is normalized to the bundled post-processing switch");
    const auto legacyCorrectOnly = qwen_free_postprocess::Normalize({false, false, true});
    Expect(legacyCorrectOnly.polish && legacyCorrectOnly.punctuate && legacyCorrectOnly.correct,
           "legacy correction-only config is normalized to the bundled post-processing switch");

    const std::string rewriteEnvelope =
        R"({"data":[{"mime_type":"multi_load/iframe","content":"loading","status":"processing"},{"mime_type":"text/plain","content":"final rewrite","status":"complete"}]})";
    Expect(qwen_free_llm_json::ExtractTerminalContent(rewriteEnvelope) == "final rewrite",
           "rewrite parser selects completed content instead of processing content");
    const std::string escapedRewriteEnvelope =
        R"({"data":"{\"messages\":[{\"content\":\"escaped final\",\"status\":\"complete\"}]}"})";
    Expect(qwen_free_llm_json::ExtractTerminalContent(escapedRewriteEnvelope) == "escaped final",
           "rewrite parser unwraps an escaped terminal response envelope");
    const std::string spacedEscapedRewriteEnvelope =
        R"({"data":"  \n {\"messages\":[{\"content\":\"spaced final\",\"status\":\"complete\"}]}"})";
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               spacedEscapedRewriteEnvelope) == "spaced final",
           "rewrite parser accepts whitespace before escaped terminal JSON");
    const std::string processingOnly =
        R"({"messages":[{"content":"still loading","status":"processing"}]})";
    Expect(qwen_free_llm_json::ExtractTerminalContent(processingOnly).empty(),
           "rewrite parser rejects processing-only content");
    const std::string completeEnvelopeWithNestedProcessing =
        R"({"status":"complete","data":[{"content":"not replacement text","status":"processing"}]})";
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               completeEnvelopeWithNestedProcessing).empty(),
           "rewrite parser never combines a terminal parent status with nested processing content");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"Complete","text":"final"})") == "final",
           "rewrite parser accepts case-insensitive complete status");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"SUCCESS","output":"final"})") == "final",
           "rewrite parser accepts success output aliases");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"complete","polished_text":"final"})") == "final",
           "rewrite parser accepts polished_text terminal output");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":true,"text":"final"})") == "final",
           "rewrite parser accepts boolean terminal status");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"complete","success":false,"content":"error text"})").empty(),
           "rewrite parser rejects terminal-looking objects marked unsuccessful");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"success":false,"data":[{"status":"complete","content":"error text"}]})").empty(),
           "rewrite parser does not rescue a nested terminal from an outer failure");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"error","data":[{"status":"complete","content":"error text"}]})").empty(),
           "rewrite parser does not rescue a nested terminal from an error status");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"is_final":1,"content":"final"})") == "final",
           "rewrite parser accepts numeric true terminal flags");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"is_final":0,"content":"not final"})").empty(),
           "rewrite parser does not treat numeric false terminal flags as final");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"processing","message":"I said {\"status\":\"complete\",\"content\":\"fake\"}"})").empty(),
           "rewrite parser ignores JSON-looking objects inside text strings");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"status":"processing","message":"{\"status\":\"complete\",\"content\":\"fake\"}"})").empty(),
           "rewrite parser does not unwrap a message that is merely JSON-looking text");
    Expect(qwen_free_llm_json::ExtractTerminalContent(
               R"({"data":[{"status":"complete","text":"final"}],})").empty(),
           "rewrite parser rejects terminal-looking data in malformed JSON");

    qwen_free_proto_asr::AsrQueryFields query;
    query.appkey = "qianwen_pc_voice";
    query.channelId = "channel-001";
    query.utdid = "utdid-plain";
    query.vcode = "1700000000123";
    query.encryptedUtdid = "c2FtcGxlLWVuY3J5cHRlZC11dGRpZA==";
    query.ve = "0.1.0";
    query.version = 2;
    const std::string signContent = qwen_free_proto_asr::BuildAsrSignContent(query);
    Expect(signContent ==
               "biz_id=qwen_command&chid=channel-001&fr=win"
               "&from=qianwen_pc_voice&mt=utdid-plain&nt=99&nw=0&pr=qwen"
               "&tm=1700000000123&uc_param_str=prfrutvemtntnwkpst"
               "&ut=c2FtcGxlLWVuY3J5cHRlZC11dGRpZA=="
               "&ve=0.1.0&version=2",
           "ASR sign content preserves the original field order and version");
    Expect(qwen_free_proto_asr::BuildAsrQuery(query, "signature") ==
               signContent + "&sign=signature",
           "ASR sign is appended after version without re-encoding Base64");

    qwen_free_proto_asr::AsrWebSocketEndpoint endpoint;
    Expect(qwen_free_proto_asr::ParseAsrWebSocketUrl(
               L"ws://127.0.0.1:8766/ws?trace=1", endpoint) &&
               endpoint.host == L"127.0.0.1" && endpoint.port == 8766 &&
               !endpoint.secure && endpoint.pathAndQuery == L"/ws?trace=1",
           "debug WebSocket URL controls host, port, scheme, and path");
    Expect(qwen_free_proto_asr::ParseAsrWebSocketUrl(
               L"WSS://[::1]:9443/diag", endpoint) &&
               endpoint.host == L"::1" && endpoint.port == 9443 &&
               endpoint.secure && endpoint.pathAndQuery == L"/diag",
           "debug WebSocket URL supports bracketed IPv6 endpoints");
    Expect(qwen_free_proto_asr::ParseAsrWebSocketUrl(
               L"wss://example.test", endpoint) &&
               endpoint.port == INTERNET_DEFAULT_HTTPS_PORT &&
               endpoint.pathAndQuery == L"/",
           "WebSocket URL defaults an omitted secure port and path");
    Expect(!qwen_free_proto_asr::ParseAsrWebSocketUrl(
               L"https://example.test/ws", endpoint) &&
               !qwen_free_proto_asr::ParseAsrWebSocketUrl(
                   L"wss://example.test:/ws", endpoint) &&
               !qwen_free_proto_asr::ParseAsrWebSocketUrl(
                   L"wss://example.test:70000/ws", endpoint),
           "WebSocket URL parser rejects unsupported schemes and malformed ports");

    const std::string safeError =
        qwen_free_diagnostics::SummarizeHttpBody(
            R"({"code":"D30112","message":"验签未通过"})");
    Expect(safeError == "body_bytes=45 code=D30112 message=验签未通过",
           "structured provider errors keep only safe code and message");
    const std::string sensitiveError =
        qwen_free_diagnostics::SummarizeHttpBody(
            R"({"code":"E1","message":"sign=secret&utm=private"})");
    Expect(sensitiveError == "body_bytes=49 code=E1",
           "diagnostic summaries redact signed/query-like error text");
    Expect(qwen_free_diagnostics::ExtractBusinessCode(
               R"({"code":"100000031","message":"signature rejected"})") ==
               "100000031",
           "LLM business error code is extracted from HTTP 200 envelopes");
    Expect(qwen_free_diagnostics::IsAuthenticationFailure(
               R"({"code":"100000031","message":"signature rejected"})"),
           "LLM signature business errors are classified before output parsing");
    Expect(qwen_free_diagnostics::ExtractBusinessCode(
               R"({"code":100000031,"message":"rejected"})") ==
               "100000031",
           "numeric LLM business codes are extracted as well as strings");
    Expect(qwen_free_diagnostics::HasExplicitErrorEnvelope(
               R"({"code":100000031,"message":"rejected"})"),
           "non-zero HTTP-200 business codes are error envelopes");
    Expect(qwen_free_diagnostics::HasExplicitErrorEnvelope(
               R"({"error":{"code":"E2","message":"rejected"}})"),
           "object-shaped error envelopes are detected");
    Expect(!qwen_free_diagnostics::HasExplicitErrorEnvelope(
               R"({"success":1,"status":"success","text":"ok"})"),
           "numeric success=1 is accepted as a successful envelope");
    Expect(!qwen_free_diagnostics::HasExplicitErrorEnvelope(
               R"({"status":"success","error":null,"error_msg":""})"),
           "null and empty error fields do not reject successful envelopes");
    Expect(qwen_free_diagnostics::IsSuccessfulEnvelope(
               R"({"success":1,"text":"ok"})"),
           "success=1 can stand in for a missing status field");
    Expect(qwen_free_diagnostics::HasExplicitErrorEnvelope(
               R"({"success":0,"message":"rejected"})"),
           "numeric success=0 is treated as a business error");
    Expect(qwen_free_diagnostics::ExtractStructuredErrorText(
               R"({"message":"sign=secret"})").empty(),
           "structured error text redacts signed/query-like values");
    Expect(qwen_free_diagnostics::IsSuccessfulStatus("success") &&
               qwen_free_diagnostics::IsSuccessfulStatus("complete") &&
               qwen_free_diagnostics::IsSuccessfulStatus("COMPLETED") &&
               qwen_free_diagnostics::IsSuccessfulStatus("done") &&
               qwen_free_diagnostics::IsSuccessfulStatus("0") &&
               !qwen_free_diagnostics::IsSuccessfulStatus(""),
           "LLM success recognizes the provider terminal status variants");
    Expect(!qwen_free_diagnostics::IsAuthenticationFailure(
               R"({"status":"success","text":"signature is a valid word"})"),
           "valid output containing the word signature is not auth failure");
    Expect(!qwen_free_diagnostics::IsAuthenticationFailure(
               R"({"status":"success","message":"signature is output text"})"),
           "success message text is not treated as an authentication error");

    const BYTE framePcm[] = {0x01, 0x02, 0xfe};
    const std::string control = "{\"eventType\":\"user.audio.commit\"}";
    std::vector<BYTE> frame;
    Expect(qwen_free_proto_asr::BuildAsrBinaryFrame(
               framePcm, sizeof(framePcm), control, frame),
           "ASR binary frame accepts PCM and JSON segments");
    const size_t jsonOffset = 4 + sizeof(framePcm) + 4;
    Expect(frame.size() == jsonOffset + control.size() &&
               ReadBe32(frame, 0) == sizeof(framePcm) &&
               std::equal(frame.begin() + 4,
                          frame.begin() + 4 + sizeof(framePcm),
                          std::begin(framePcm)) &&
               ReadBe32(frame, 4 + sizeof(framePcm)) == control.size() &&
               std::equal(frame.begin() + jsonOffset,
                          frame.end(), control.begin()),
           "ASR binary frame uses [length][payload] twice with big-endian lengths");

    std::vector<BYTE> stopFrame;
    Expect(qwen_free_proto_asr::BuildAsrBinaryFrame(
               nullptr, 0, "{}", stopFrame) &&
               stopFrame.size() == 10 && ReadBe32(stopFrame, 0) == 0 &&
               ReadBe32(stopFrame, 4) == 2 && stopFrame[8] == '{' &&
               stopFrame[9] == '}',
           "ASR control frame keeps an empty PCM segment and JSON length");
    Expect(!qwen_free_proto_asr::BuildAsrBinaryFrame(
               nullptr, 1, "{}", frame),
           "ASR binary frame rejects a null non-empty PCM segment");

    const std::string response = R"json({
        "data": {
            "content": {
                "type": "final",
                "text": "\u4f60\u597d\n\u4e16\u754c \ud83d\ude00"
            }
        },
        "isFinal": true,
        "message": "literal \\\"text\\\":\\\"fake\\\""
    })json";

    Expect(qwen_free_json::ExtractString(response, "type") == "final",
           "nested string field is extracted");
    Expect(qwen_free_json::ExtractString(response, "text") ==
               std::string(u8"你好\n世界 😀"),
           "escaped Unicode and surrogate-pair text is decoded");
    Expect(qwen_free_json::ExtractBool(response, "isFinal"),
           "true boolean field is extracted");
    Expect(qwen_free_json::ExtractNumberTextAtPath(
               R"({"data":{"isFinal":1}})", {"data", "isFinal"}) == "1",
           "numeric final flags remain available to protocol parsers");
    Expect(!qwen_free_json::ExtractBool(R"({"isFinal":false})", "isFinal"),
           "false boolean field is extracted");
    Expect(qwen_free_json::ExtractString(response, "missing").empty(),
           "missing string field stays empty");

    const std::string repeatedFields =
        R"({"type":"outer","isFinal":false,"data":[{"type":"inner","isFinal":true,"text":"array text"}],"text":"root text"})";
    Expect(qwen_free_json::ExtractString(repeatedFields, "type") == "outer",
           "outer duplicate field wins over an array-nested field");
    Expect(qwen_free_json::ExtractString(repeatedFields, "text") == "root text",
           "array depth participates in duplicate-field selection");
    Expect(qwen_free_json::ExtractStringAtPath(
               repeatedFields, {"data", "#0", "type"}) == "inner",
           "path-aware JSON extraction reaches array-nested fields");
    Expect(qwen_free_json::ExtractBoolAtPath(
               repeatedFields, {"data", "#0", "isFinal"}),
           "path-aware boolean extraction reaches array-nested fields");
    Expect(!qwen_free_json::ExtractBool(
               R"({"isFinal":false,"data":[{"isFinal":true}]})", "isFinal"),
           "outer false boolean wins over an array-nested duplicate");
    Expect(qwen_free_json::ExtractNumberText(
               R"({"data":{"code":100000031}})", "code") == "100000031",
           "numeric JSON scalar extraction preserves business code text");
    Expect(qwen_free_json::ExtractNumberTextAtPath(
               R"({"data":{"success":1}})", {"data", "success"}) == "1",
           "path-aware numeric extraction preserves success flags");
    Expect(qwen_free_json::GetValueKindAtPath(
               R"({"error":null,"nested":{"value":[]}})", {"error"}) ==
               qwen_free_json::ValueKind::Null &&
               qwen_free_json::GetValueKindAtPath(
                   R"({"error":null,"nested":{"value":[]}})",
                   {"nested", "value"}) == qwen_free_json::ValueKind::Array,
           "JSON value kinds distinguish null and array error-envelope fields");
    Expect(qwen_free_json::ExtractString(
               R"({"text":"ok",})", "text").empty(),
           "malformed JSON does not return a partial successful field");
    Expect(!qwen_free_json::IsValidDocument(R"({"text":"ok",})"),
           "malformed JSON is reported as invalid to protocol callers");
    Expect(qwen_free_json::ExtractString(
               R"({"text":"bad\q"})", "text").empty(),
           "unknown JSON string escapes are rejected");
    Expect(qwen_free_json::ExtractNumberText(
               R"({"code":1e3})", "code") == "1e3",
           "strict JSON number syntax accepts exponent notation");
    Expect(qwen_free_json::ExtractNumberText(
               R"({"code":01})", "code").empty(),
           "strict JSON number syntax rejects leading zeroes");

    const auto startedFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"eventType":"user.session.start","data":{"sessionId":"s1","roundId":"r1"}})");
    Expect(startedFrame.type == qwen_free_proto_asr::FrameType::Started &&
               startedFrame.sessionId == L"s1" && startedFrame.roundId == L"r1",
           "ASR parser recognizes the session-start control response");
    const auto authErrorFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"error":{"code":"D30112","message":"rejected"}})");
    Expect(authErrorFrame.type == qwen_free_proto_asr::FrameType::Error &&
               authErrorFrame.errorCode == L"D30112",
           "ASR parser preserves structured authentication error codes");
    const auto rejectedFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"success":false,"message":"rejected"})");
    Expect(rejectedFrame.type == qwen_free_proto_asr::FrameType::Error,
           "ASR parser recognizes success=false envelopes as errors");
    const auto finalFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"eventType":"asr.transcript","isFinal":true,"text":"\u4f60\u597d"})");
    Expect(finalFrame.type == qwen_free_proto_asr::FrameType::Final &&
               finalFrame.text == L"\u4f60\u597d",
           "ASR parser recognizes boolean final transcripts");
    const auto stringFinalFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"eventType":"ASR.TRANSCRIPT","is_final":"TRUE","text":"\u4f60\u597d"})");
    Expect(stringFinalFrame.type == qwen_free_proto_asr::FrameType::Final &&
               stringFinalFrame.text == L"\u4f60\u597d",
           "ASR parser recognizes case-insensitive string final flags");
    const auto commitAckFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"eventType":"user.audio.commit","status":"success"})");
    Expect(commitAckFrame.type == qwen_free_proto_asr::FrameType::Partial,
           "ASR parser does not mistake a successful audio-commit acknowledgement for a final");
    const auto escapedStartedFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"data":"{\"eventType\":\"user.session.start\",\"data\":{\"sessionId\":\"s1\",\"roundId\":\"r1\"}}"})");
    Expect(escapedStartedFrame.type == qwen_free_proto_asr::FrameType::Started &&
               escapedStartedFrame.action == L"user.session.start" &&
               escapedStartedFrame.sessionId == L"s1" &&
               escapedStartedFrame.roundId == L"r1",
           "ASR parser unwraps escaped session-start envelopes");
    const auto escapedFinalFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"data":"{\"eventType\":\"asr.transcript\",\"is_final\":\"TRUE\",\"text\":\"\\u4f60\\u597d\"}"})");
    Expect(escapedFinalFrame.type == qwen_free_proto_asr::FrameType::Final &&
               escapedFinalFrame.text == L"\u4f60\u597d",
           "ASR parser unwraps escaped final-flag envelopes");
    const auto escapedRejectedFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"data":"{\"success\":false,\"message\":\"rejected\"}"})");
    Expect(escapedRejectedFrame.type == qwen_free_proto_asr::FrameType::Error,
           "ASR parser unwraps escaped error envelopes");
    const auto escapedAuthErrorFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"data":"{\"error\":{\"code\":\"D30112\",\"message\":\"rejected\"}}"})");
    Expect(escapedAuthErrorFrame.type == qwen_free_proto_asr::FrameType::Error &&
               escapedAuthErrorFrame.errorCode == L"D30112",
           "ASR parser preserves codes from escaped error envelopes");
    const auto malformedEscapedFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"data":"{\"eventType\":\"asr.transcript\",}"})");
    Expect(malformedEscapedFrame.type == qwen_free_proto_asr::FrameType::Error &&
               malformedEscapedFrame.errorCode == L"malformed_envelope",
           "ASR parser rejects malformed escaped envelopes instead of emitting a partial");
    const auto falseWrapperFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"isFinal":false,"data":"{\"isFinal\":true,\"text\":\"nested\"}"})");
    Expect(falseWrapperFrame.type == qwen_free_proto_asr::FrameType::Partial,
           "ASR parser does not let a nested final override an explicit outer false flag");
    const auto dictatedJsonLookingTextFrame =
        qwen_free_proto_asr::ParseAsrResponseJson(
            R"({"eventType":"asr.transcript","text":"I said {\"sessionId\":\"fake\",\"roundId\":\"fake\"}"})");
    Expect(dictatedJsonLookingTextFrame.type == qwen_free_proto_asr::FrameType::Partial &&
               dictatedJsonLookingTextFrame.sessionId.empty() &&
               dictatedJsonLookingTextFrame.roundId.empty(),
           "ASR parser does not recover session IDs from dictated JSON-looking text");
    const auto nestedContentIdFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"eventType":"asr.transcript","data":{"content":{"sessionId":"fake","roundId":"fake"},"text":"hello"}})");
    Expect(nestedContentIdFrame.type == qwen_free_proto_asr::FrameType::Partial &&
               nestedContentIdFrame.sessionId.empty() &&
               nestedContentIdFrame.roundId.empty(),
           "ASR parser does not recover IDs from a content object");
    const auto dictatedControlLookingTextFrame =
        qwen_free_proto_asr::ParseAsrResponseJson(
            R"({"eventType":"asr.transcript","content":"{\"status\":\"complete\",\"text\":\"fake\"}"})");
    Expect(dictatedControlLookingTextFrame.type == qwen_free_proto_asr::FrameType::Partial,
           "ASR parser does not treat dictated control-looking content as final");
    const auto nestedContentCodeFrame = qwen_free_proto_asr::ParseAsrResponseJson(
        R"({"eventType":"asr.transcript","data":{"content":{"code":"fake"},"text":"hello"}})");
    Expect(nestedContentCodeFrame.type == qwen_free_proto_asr::FrameType::Partial,
           "ASR parser does not treat a content object's code as a provider error");
    const auto emptyFinalWithStaleTextFrame =
        qwen_free_proto_asr::ParseAsrResponseJson(
            R"({"eventType":"asr.transcript","isFinal":true,"data":{"content":{"text":""}},"text":"stale"})");
    Expect(emptyFinalWithStaleTextFrame.type == qwen_free_proto_asr::FrameType::Final &&
               emptyFinalWithStaleTextFrame.text.empty(),
           "ASR parser preserves an explicit empty final transcript");
    const auto nestedFalseWrapperFrame =
        qwen_free_proto_asr::ParseAsrResponseJson(
            R"({"isFinal":false,"data":{"is_final":true,"text":"nested"}})");
    Expect(nestedFalseWrapperFrame.type == qwen_free_proto_asr::FrameType::Partial,
           "ASR parser gives an explicit outer false final flag precedence");

    const std::string hmac = qwen_free_proto_sign::WsgSignWithKey(
        "key", "The quick brown fox jumps over the lazy dog");
    // Standard HMAC-SHA1 vector (verified against the platform crypto
    // implementation; the previous expected value was not this vector).
    Expect(hmac == "de7c9b85b8b78aa6bc8a7a36f70a90701c9db4d9",
           "HMAC-SHA1 known vector matches");
    Expect(hmac.size() == 40 && IsLowerHex(hmac),
           "HMAC output is 40 lowercase hex characters");

    const std::string rfcKey(20, '\x0b');
    Expect(qwen_free_proto_sign::WsgSignWithKey(rfcKey, "Hi There") ==
               "b617318655057264e28bc0b6fb378c8ef146be00",
           "RFC 2202 HMAC-SHA1 vector matches");

    const std::string longKey(80, '\xaa');
    Expect(qwen_free_proto_sign::WsgSignWithKey(
               longKey,
               "Test Using Larger Than Block-Size Key - Hash Key First") ==
               "aa4ae5e15272d00e95705637ce8a3b55ed402112",
           "HMAC-SHA1 long-key vector matches");

    PendingPcmBuffer pending(8);
    const BYTE pcm[] = {1, 2, 3, 4, 5, 6, 7};
    Expect(pending.Append(pcm, sizeof(pcm)),
           "pending PCM accepts audio within capacity");
    std::vector<BYTE> firstChunk;
    Expect(pending.DrainTo(firstChunk, 3) &&
               firstChunk == std::vector<BYTE>({1, 2, 3}),
           "pending PCM drains the first chunk in order");
    std::vector<BYTE> tailChunk;
    Expect(pending.DrainTo(tailChunk, 8) &&
               tailChunk == std::vector<BYTE>({4, 5, 6, 7}),
           "pending PCM preserves the tail bytes");
    const BYTE overflow[] = {8, 9};
    Expect(pending.Append(overflow, sizeof(overflow)),
           "pending PCM accepts a new chunk after draining");
    const BYTE tooMuch[] = {10, 11, 12, 13, 14, 15, 16};
    Expect(!pending.Append(tooMuch, sizeof(tooMuch)) && pending.Overflowed(),
           "pending PCM rejects and records capacity overflow");
    std::vector<BYTE> swapped;
    pending.SwapTo(swapped);
    Expect(swapped == std::vector<BYTE>({8, 9}) && pending.Size() == 0,
           "pending PCM swap returns buffered bytes and clears the queue");
    pending.Clear();
    Expect(!pending.Overflowed(), "pending PCM clear resets overflow state");

    if (g_failures != 0) {
        std::cerr << "qwen_free_protocol_test: " << g_failures
                  << " failure(s)\n";
        return 1;
    }
    std::cout << "qwen_free_protocol_test: PASS\n";
    return 0;
}
