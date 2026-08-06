#pragma once

#include "qwen_free_diagnostics.h"
#include "qwen_free_json.h"
#include "qwen_free_proto_asr.h"

#include <cctype>
#include <initializer_list>
#include <string>

// Pure Qwen IME Free ASR response decoding.  Keeping this separate from the
// WinHTTP receive loop makes malformed/error envelopes testable offline and
// prevents transport code from silently deciding that every unknown object is
// a Partial transcript.
namespace qwen_free_proto_asr {
namespace asr_json_detail {

inline std::string LowerAscii(std::string value) {
    for (char& c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x80u) c = static_cast<char>(std::tolower(byte));
    }
    return value;
}

inline std::string TrimAscii(const std::string& value) {
    const std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                            value.data(), static_cast<int>(value.size()),
                            result.data(), length) <= 0) {
        return {};
    }
    return result;
}

// Gateways sometimes serialize the actual ASR control object into a string
// field. Only unwrap response-shaped containers; do not recursively inspect
// arbitrary transcript/content strings, which could otherwise turn dictated
// JSON-looking text into a protocol event.
inline std::string ExtractEmbeddedJson(const std::string& json) {
    static constexpr const char* kContainers[] = {
        "data", "result", "response", "body", "payload"
    };
    for (const char* container : kContainers) {
        std::string embedded = qwen_free_json::ExtractStringAtPath(
            json, {container});
        embedded = TrimAscii(embedded);
        if (embedded.empty() || embedded == json) continue;
        if (embedded.front() == '{' || embedded.front() == '[') {
            return embedded;
        }
    }
    return {};
}

inline bool TryExtractProtocolDirectString(const std::string& json,
                                           const char* key,
                                           std::string& value) {
    value.clear();
    if (json.empty() || !key || *key == '\0') return false;
    for (const auto path : {
             std::initializer_list<const char*>{key},
             std::initializer_list<const char*>{"data", key},
             std::initializer_list<const char*>{"result", key},
             std::initializer_list<const char*>{"response", key},
             std::initializer_list<const char*>{"body", key},
             std::initializer_list<const char*>{"payload", key}
         }) {
        if (qwen_free_json::GetValueKindAtPath(json, path) !=
            qwen_free_json::ValueKind::String) {
            continue;
        }
        value = qwen_free_json::ExtractStringAtPath(json, path);
        return true;
    }
    return false;
}

inline bool TryExtractProtocolDirectNumber(const std::string& json,
                                           const char* key,
                                           std::string& value) {
    value.clear();
    if (json.empty() || !key || *key == '\0') return false;
    for (const auto path : {
             std::initializer_list<const char*>{key},
             std::initializer_list<const char*>{"data", key},
             std::initializer_list<const char*>{"result", key},
             std::initializer_list<const char*>{"response", key},
             std::initializer_list<const char*>{"body", key},
             std::initializer_list<const char*>{"payload", key}
         }) {
        if (qwen_free_json::GetValueKindAtPath(json, path) !=
            qwen_free_json::ValueKind::Number) {
            continue;
        }
        value = qwen_free_json::ExtractNumberTextAtPath(json, path);
        return true;
    }
    return false;
}

inline bool HasMalformedEmbeddedJson(const std::string& json,
                                     int depth = 0) {
    if (depth >= 3) return false;
    static constexpr const char* kContainers[] = {
        "data", "result", "response", "body", "payload"
    };
    for (const char* container : kContainers) {
        if (qwen_free_json::GetValueKindAtPath(json, {container}) !=
            qwen_free_json::ValueKind::String) {
            continue;
        }
        const std::string embedded = TrimAscii(
            qwen_free_json::ExtractStringAtPath(json, {container}));
        if (embedded.empty() ||
            (embedded.front() != '{' && embedded.front() != '[')) {
            continue;
        }
        if (!qwen_free_json::IsValidDocument(embedded) ||
            HasMalformedEmbeddedJson(embedded, depth + 1)) {
            return true;
        }
    }
    return false;
}

inline std::string ExtractNestedString(const std::string& json,
                                       const char* key,
                                       int depth = 0) {
    if (json.empty() || !key || *key == '\0') return {};

    const std::string direct = qwen_free_json::ExtractString(json, key);
    if (!direct.empty()) return direct;
    // Preserve an explicitly empty direct string instead of replacing it with
    // a same-named field inside an escaped child envelope.
    if (qwen_free_json::GetValueKindAtPath(json, {key}) ==
        qwen_free_json::ValueKind::String) {
        return {};
    }
    if (depth >= 3) return {};

    static constexpr const char* kContainers[] = {
        "data", "result", "response", "body", "payload", "message",
        "error", "content"
    };
    for (const char* container : kContainers) {
        std::string embedded = qwen_free_json::ExtractStringAtPath(
            json, {container});
        if (embedded.empty()) {
            embedded = qwen_free_json::ExtractString(json, container);
        }
        embedded = TrimAscii(embedded);
        if (embedded.empty() || embedded == json) continue;
        if (embedded.front() != '{' && embedded.front() != '[') continue;
        const std::string nested =
            ExtractNestedString(embedded, key, depth + 1);
        if (!nested.empty()) return nested;
    }
    return {};
}

inline std::string ExtractAtPaths(
    const std::string& json,
    std::initializer_list<std::initializer_list<const char*>> paths) {
    for (const auto path : paths) {
        const std::string value = qwen_free_json::ExtractStringAtPath(json, path);
        if (!value.empty()) return value;
    }
    return {};
}

inline bool TryExtractStringAtPaths(
    const std::string& json,
    std::initializer_list<std::initializer_list<const char*>> paths,
    std::string& value) {
    value.clear();
    for (const auto path : paths) {
        if (qwen_free_json::GetValueKindAtPath(json, path) !=
            qwen_free_json::ValueKind::String) {
            continue;
        }
        value = qwen_free_json::ExtractStringAtPath(json, path);
        return true;
    }
    return false;
}

inline std::string ExtractAction(const std::string& json, int depth = 0) {
    const std::string explicitAction = ExtractAtPaths(json, {
        {"eventType"}, {"event_type"}, {"data", "eventType"},
        {"data", "event_type"}, {"data", "action"},
        {"data", "event"}, {"data", "eventName"},
        {"data", "type"}, {"data", "content", "action"},
        {"data", "content", "eventType"},
        {"data", "content", "type"}
    });
    if (!explicitAction.empty()) return explicitAction;

    for (const char* key : {
             "eventType", "event_type", "action", "event", "eventName", "type"
         }) {
        std::string value;
        if (TryExtractProtocolDirectString(json, key, value) &&
            !value.empty()) {
            return value;
        }
    }
    if (depth < 3) {
        const std::string embedded = ExtractEmbeddedJson(json);
        if (!embedded.empty()) return ExtractAction(embedded, depth + 1);
    }
    return {};
}

inline std::string ExtractProtocolNestedString(const std::string& json,
                                               const char* key,
                                               int depth = 0) {
    if (json.empty() || !key || *key == '\0') return {};
    std::string direct;
    if (TryExtractProtocolDirectString(json, key, direct)) return direct;
    if (depth >= 3) return {};
    // IDs must only be recovered from response envelopes. Do not unwrap a
    // `content`/`message` string, because dictated text can itself contain
    // JSON-looking sessionId/roundId keys.
    for (const char* container : {
             "data", "result", "response", "body", "payload"
         }) {
        std::string embedded = qwen_free_json::ExtractStringAtPath(
            json, {container});
        embedded = TrimAscii(embedded);
        if (embedded.empty() || embedded == json ||
            (embedded.front() != '{' && embedded.front() != '[')) {
            continue;
        }
        const std::string nested = ExtractProtocolNestedString(
            embedded, key, depth + 1);
        if (!nested.empty()) return nested;
    }
    return {};
}

inline std::string ExtractId(const std::string& json,
                             const char* camel,
                             const char* snake) {
    for (const char* key : {camel, snake}) {
        const std::string value = ExtractProtocolNestedString(json, key);
        if (!value.empty()) return value;
    }
    return {};
}

inline std::string ExtractTranscript(const std::string& json) {
    std::string explicitText;
    if (TryExtractStringAtPaths(json, {
        {"data", "content", "text"},
        {"data", "content", "transcript"},
        {"data", "text"},
        {"data", "transcript"},
        {"text"},
        {"transcript"}
    }, explicitText)) {
        // An explicitly present empty transcript is meaningful: do not fall
        // through to an older partial field elsewhere in the envelope.
        return explicitText;
    }

    for (const char* key : {
             "text", "transcript", "sentence", "result", "output",
             "content"
         }) {
        const std::string value = ExtractNestedString(json, key);
        if (!value.empty()) return value;
    }
    return {};
}

enum class TruthValue {
    Missing,
    True,
    False,
    Unknown,
};

inline TruthValue ReadTruthAtPath(
    const std::string& json,
    std::initializer_list<const char*> path) {
    const qwen_free_json::ValueKind kind =
        qwen_free_json::GetValueKindAtPath(json, path);
    switch (kind) {
    case qwen_free_json::ValueKind::Bool:
        return qwen_free_json::ExtractBoolAtPath(json, path)
            ? TruthValue::True : TruthValue::False;
    case qwen_free_json::ValueKind::Number: {
        const std::string number =
            qwen_free_json::ExtractNumberTextAtPath(json, path);
        if (number == "1" || number == "1.0") return TruthValue::True;
        if (number == "0" || number == "0.0") return TruthValue::False;
        return TruthValue::Unknown;
    }
    case qwen_free_json::ValueKind::String: {
        const std::string text = LowerAscii(
            qwen_free_json::ExtractStringAtPath(json, path));
        if (text == "true" || text == "1" || text == "yes" ||
            text == "y" || text == "on") return TruthValue::True;
        if (text == "false" || text == "0" || text == "no" ||
            text == "n" || text == "off") return TruthValue::False;
        return TruthValue::Unknown;
    }
    case qwen_free_json::ValueKind::Missing:
        return TruthValue::Missing;
    default:
        return TruthValue::Unknown;
    }
}

inline bool IsFinalTranscript(const std::string& json, int depth = 0) {
    bool sawExplicitFalse = false;
    bool sawExplicitTrue = false;
    for (const char* key : {"isFinal", "is_final", "isEnd", "is_end"}) {
        for (const auto path : {
                 std::initializer_list<const char*>{key},
                 std::initializer_list<const char*>{"data", key},
                 std::initializer_list<const char*>{"data", "content", key}
             }) {
            const TruthValue value = ReadTruthAtPath(json, path);
            if (value == TruthValue::True) {
                sawExplicitTrue = true;
                continue;
            }
            if (value == TruthValue::False) {
                sawExplicitFalse = true;
                break;
            }
        }
    }

    // A direct false flag is more authoritative than a loose nested `type`
    // or `status` string. It also prevents a wrapper's isFinal:false from
    // being overridden by an escaped child envelope.
    if (sawExplicitFalse) return false;
    if (sawExplicitTrue) return true;

    std::string type = ExtractAtPaths(json, {
        {"data", "content", "type"}, {"data", "type"}, {"type"},
        {"status"}
    });
    if (type.empty()) {
        std::string protocolStatus;
        if (TryExtractProtocolDirectString(json, "status", protocolStatus)) {
            type = protocolStatus;
        }
    }
    type = LowerAscii(type);
    if (type == "final" || type == "completed" ||
        type == "complete" || type == "done" || type == "finished" ||
        type.find(".final") != std::string::npos ||
        type.find(".completed") != std::string::npos) {
        return true;
    }

    if (depth < 3) {
        const std::string embedded = ExtractEmbeddedJson(json);
        if (!embedded.empty()) return IsFinalTranscript(embedded, depth + 1);
    }
    return false;
}

inline std::string ExtractCode(const std::string& json, int depth = 0) {
    for (const auto path : {
             std::initializer_list<const char*>{"error", "code"},
             std::initializer_list<const char*>{"data", "error", "code"},
             std::initializer_list<const char*>{"code"},
             std::initializer_list<const char*>{"error_code"},
             std::initializer_list<const char*>{"errorCode"}
         }) {
        const qwen_free_json::ValueKind kind =
            qwen_free_json::GetValueKindAtPath(json, path);
        if (kind == qwen_free_json::ValueKind::String) {
            const std::string value =
                qwen_free_json::ExtractStringAtPath(json, path);
            if (!value.empty()) return value;
        } else if (kind == qwen_free_json::ValueKind::Number) {
            const std::string value =
                qwen_free_json::ExtractNumberTextAtPath(json, path);
            if (!value.empty()) return value;
        }
    }
    for (const char* key : {"code", "error_code", "errorCode"}) {
        std::string value;
        if (TryExtractProtocolDirectString(json, key, value) &&
            !value.empty()) {
            return value;
        }
        std::string number;
        if (TryExtractProtocolDirectNumber(json, key, number) &&
            !number.empty()) {
            return number;
        }
    }
    if (depth < 3) {
        const std::string embedded = ExtractEmbeddedJson(json);
        if (!embedded.empty()) return ExtractCode(embedded, depth + 1);
    }
    return {};
}

inline std::string ExtractErrorMessage(const std::string& json, int depth = 0) {
    const std::string explicitMessage = ExtractAtPaths(json, {
        {"error", "message"}, {"data", "error", "message"},
        {"message"}, {"error_msg"}, {"error_message"}, {"detail"}
    });
    if (!explicitMessage.empty()) return explicitMessage;
    for (const char* key : {"message", "error_msg", "error_message", "detail"}) {
        const std::string value = ExtractNestedString(json, key);
        if (!value.empty()) return value;
    }
    if (depth < 3) {
        const std::string embedded = ExtractEmbeddedJson(json);
        if (!embedded.empty()) return ExtractErrorMessage(embedded, depth + 1);
    }
    return {};
}

inline bool IsMeaningfulErrorField(const std::string& json,
                                   const char* key) {
    for (const auto path : {
             std::initializer_list<const char*>{key},
             std::initializer_list<const char*>{"data", key},
             std::initializer_list<const char*>{"data", "content", key}
         }) {
        const qwen_free_json::ValueKind kind =
            qwen_free_json::GetValueKindAtPath(json, path);
        switch (kind) {
        case qwen_free_json::ValueKind::String:
            if (!qwen_free_json::ExtractStringAtPath(json, path).empty()) return true;
            break;
        case qwen_free_json::ValueKind::Number: {
            const std::string value =
                qwen_free_json::ExtractNumberTextAtPath(json, path);
            if (!value.empty() && value != "0" && value != "0.0") return true;
            break;
        }
        case qwen_free_json::ValueKind::Bool:
            if (qwen_free_json::ExtractBoolAtPath(json, path)) return true;
            break;
        case qwen_free_json::ValueKind::Object:
        case qwen_free_json::ValueKind::Array:
            return true;
        default:
            break;
        }
    }
    return false;
}

inline bool IsErrorEnvelope(const std::string& json,
                            const std::string& action,
                            const std::string& code,
                            int depth = 0) {
    const std::string lowerAction = LowerAscii(action);
    if (lowerAction == "error" || lowerAction == "asr_error" ||
        lowerAction.find(".error") != std::string::npos ||
        lowerAction.find("failed") != std::string::npos ||
        lowerAction.find("failure") != std::string::npos ||
        lowerAction.find("rejected") != std::string::npos ||
        lowerAction.find("unauthorized") != std::string::npos ||
        lowerAction.find("forbidden") != std::string::npos) {
        return true;
    }

    if (IsMeaningfulErrorField(json, "error") ||
        IsMeaningfulErrorField(json, "error_msg") ||
        IsMeaningfulErrorField(json, "error_message")) {
        return true;
    }

    for (const auto path : {
             std::initializer_list<const char*>{"success"},
             std::initializer_list<const char*>{"data", "success"}
         }) {
        if (ReadTruthAtPath(json, path) == TruthValue::False) return true;
    }

    std::string status;
    if (!TryExtractProtocolDirectString(json, "status", status)) status.clear();
    status = LowerAscii(status);
    if (status == "error" || status == "failed" || status == "failure" ||
        status == "rejected" || status == "unauthorized" ||
        status == "forbidden" || status == "cancelled" ||
        status == "canceled") {
        return true;
    }

    if (!code.empty()) {
        const std::string lowerCode = LowerAscii(code);
        if (lowerCode != "0" && lowerCode != "0.0" &&
            lowerCode != "200" && lowerCode != "ok" &&
            lowerCode != "success" && lowerCode != "succeed") {
            return true;
        }
    }

    if (depth < 3) {
        const std::string embedded = ExtractEmbeddedJson(json);
        if (!embedded.empty()) {
            return IsErrorEnvelope(
                embedded, ExtractAction(embedded), ExtractCode(embedded), depth + 1);
        }
    }
    return false;
}

} // namespace asr_json_detail

inline AsrFrame ParseAsrResponseJson(const std::string& json) {
    AsrFrame frame;
    if (!qwen_free_json::IsValidDocument(json)) {
        frame.type = FrameType::Error;
        frame.errorCode = L"malformed_json";
        frame.errorMsg = L"Qwen ASR returned malformed JSON";
        return frame;
    }
    if (asr_json_detail::HasMalformedEmbeddedJson(json)) {
        frame.type = FrameType::Error;
        frame.errorCode = L"malformed_envelope";
        frame.errorMsg = L"Qwen ASR returned malformed nested JSON";
        return frame;
    }

    const std::string action = asr_json_detail::ExtractAction(json);
    const std::string sessionId = asr_json_detail::ExtractId(
        json, "sessionId", "session_id");
    const std::string roundId = asr_json_detail::ExtractId(
        json, "roundId", "round_id");
    const std::string transcript = asr_json_detail::ExtractTranscript(json);
    const std::string errorCode = asr_json_detail::ExtractCode(json);
    const std::string errorMessage = asr_json_detail::ExtractErrorMessage(json);

    frame.action = asr_json_detail::Utf8ToWide(action);
    frame.sessionId = asr_json_detail::Utf8ToWide(sessionId);
    frame.roundId = asr_json_detail::Utf8ToWide(roundId);

    if (asr_json_detail::IsErrorEnvelope(json, action, errorCode)) {
        frame.type = FrameType::Error;
        if (qwen_free_diagnostics::IsSafeStructuredField(errorCode)) {
            frame.errorCode = asr_json_detail::Utf8ToWide(errorCode);
        }
        if (qwen_free_diagnostics::IsSafeStructuredField(errorMessage)) {
            frame.errorMsg = asr_json_detail::Utf8ToWide(errorMessage);
        }
        if (frame.errorMsg.empty()) {
            frame.errorMsg = L"server returned an ASR error";
        }
        return frame;
    }

    const bool isFinal = asr_json_detail::IsFinalTranscript(json);
    frame.text = asr_json_detail::Utf8ToWide(transcript);
    const std::string lowerAction = asr_json_detail::LowerAscii(action);
    const bool hasIdentifiers = !sessionId.empty() && !roundId.empty();
    if (lowerAction == "user.session.start" || lowerAction == "started" ||
        lowerAction == "asr_started" || lowerAction == "session.started" ||
        (hasIdentifiers && transcript.empty() && !isFinal)) {
        frame.type = FrameType::Started;
    } else if (lowerAction == "completed" ||
               lowerAction == "asr_completed" ||
               lowerAction == "asr.final" || isFinal ||
               lowerAction.find(".final") != std::string::npos ||
               lowerAction.find(".completed") != std::string::npos) {
        frame.type = FrameType::Final;
    } else {
        frame.type = FrameType::Partial;
    }
    return frame;
}

} // namespace qwen_free_proto_asr
