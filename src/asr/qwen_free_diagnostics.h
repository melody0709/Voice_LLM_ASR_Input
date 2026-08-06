#pragma once

#include "qwen_free_json.h"

#include <cctype>
#include <string>
#include <utility>

// Error bodies returned by a remote service are diagnostic input, not trusted
// log text.  Keep only short structured fields that cannot look like a signed
// URL, credential, or arbitrary transcript.  The full response may still be
// retained in memory by the caller when it needs to parse it, but it must not
// be copied into HUD text or persistent debug logs.
namespace qwen_free_diagnostics {

inline std::string LowerAscii(std::string value) {
    for (char& c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x80u) {
            c = static_cast<char>(std::tolower(byte));
        }
    }
    return value;
}

inline bool IsSafeStructuredField(const std::string& value) {
    if (value.empty() || value.size() > 256) return false;
    for (const unsigned char c : value) {
        if (c < 0x20u || c == 0x7fu || c == '&' || c == '=' ||
            c == '?' || c == '#' || c == '/' || c == '\\') {
            return false;
        }
    }

    const std::string lower = LowerAscii(value);
    static constexpr const char* kSensitiveMarkers[] = {
        "sign", "utdid", "kps", "appkey", "encrypted", "authorization",
        "token", "http:", "https:", "query"
    };
    for (const char* marker : kSensitiveMarkers) {
        if (lower.find(marker) != std::string::npos) return false;
    }
    return true;
}

inline std::string SummarizeHttpBody(const std::string& body) {
    std::string summary = "body_bytes=" + std::to_string(body.size());

    const std::string code = qwen_free_json::ExtractString(body, "code");
    const std::string numericCode = code.empty()
        ? qwen_free_json::ExtractNumberText(body, "code")
        : std::string();
    const std::string displayCode = code.empty() ? numericCode : code;
    if (IsSafeStructuredField(displayCode)) {
        summary += " code=" + displayCode;
    }

    std::string message = qwen_free_json::ExtractString(body, "message");
    if (message.empty()) message = qwen_free_json::ExtractString(body, "error_msg");
    if (message.empty()) message = qwen_free_json::ExtractString(body, "error_message");
    if (IsSafeStructuredField(message)) {
        summary += " message=" + message;
    }

    if (body.size() > 0 && body.front() != '{' && body.front() != '[') {
        const std::string lower = LowerAscii(body);
        const char* category = "plain_text";
        if (lower.find("signature") != std::string::npos ||
            lower.find("sign") != std::string::npos) {
            category = "auth_signature";
        } else if (lower.find("utdid") != std::string::npos) {
            category = "device_identity";
        } else if (lower.find("kps") != std::string::npos ||
                   lower.find("encrypt") != std::string::npos) {
            category = "auth_parameters";
        } else if (lower.find("param") != std::string::npos ||
                   lower.find("request") != std::string::npos ||
                   lower.find("body") != std::string::npos) {
            category = "request_shape";
        } else if (lower.find("limit") != std::string::npos ||
                   lower.find("quota") != std::string::npos) {
            category = "quota";
        } else if (lower.find("timeout") != std::string::npos) {
            category = "timeout";
        } else if (lower.find("error") != std::string::npos) {
            category = "server_error";
        }
        summary += " text_class=" + std::string(category);
    }
    return summary;
}

// The voice-command endpoint can return HTTP 200 for a rejected request.
// Expose the business code so callers do not mistake that envelope for a
// successful response with a missing output field.
inline std::string ExtractBusinessCode(const std::string& body) {
    std::string code = qwen_free_json::ExtractString(body, "code");
    if (code.empty()) code = qwen_free_json::ExtractNumberText(body, "code");
    if (code.empty()) code = qwen_free_json::ExtractString(body, "error_code");
    if (code.empty()) code = qwen_free_json::ExtractNumberText(body, "error_code");
    if (code.empty()) code = qwen_free_json::ExtractString(body, "errorCode");
    if (code.empty()) code = qwen_free_json::ExtractNumberText(body, "errorCode");
    return code;
}

inline bool IsSuccessBusinessCode(const std::string& code) {
    if (code.empty()) return false;
    const std::string lower = LowerAscii(code);
    return lower == "0" || lower == "200" || lower == "ok" ||
           lower == "success" || lower == "succeed";
}

inline bool IsSuccessfulStatus(std::string status) {
    status = LowerAscii(std::move(status));
    return status == "success" || status == "complete" ||
           status == "completed" || status == "ok" ||
           status == "finished" || status == "succeed" ||
           status == "done" || status == "final" || status == "true" ||
           status == "0" || status == "1" || status == "200";
}

inline std::string ExtractStructuredErrorText(const std::string& body) {
    for (const char* key : {"message", "error", "error_msg", "error_message"}) {
        const std::string value = qwen_free_json::ExtractString(body, key);
        if (IsSafeStructuredField(value)) return value;
    }
    return {};
}

inline bool IsMeaningfulErrorFieldAtPath(
    const std::string& body,
    std::initializer_list<const char*> path) {
    const qwen_free_json::ValueKind kind =
        qwen_free_json::GetValueKindAtPath(body, path);
    switch (kind) {
    case qwen_free_json::ValueKind::String:
        return !qwen_free_json::ExtractStringAtPath(body, path).empty();
    case qwen_free_json::ValueKind::Number: {
        const std::string value =
            qwen_free_json::ExtractNumberTextAtPath(body, path);
        return !value.empty() && value != "0" && value != "0.0";
    }
    case qwen_free_json::ValueKind::Bool:
        return qwen_free_json::ExtractBoolAtPath(body, path);
    case qwen_free_json::ValueKind::Object:
    case qwen_free_json::ValueKind::Array:
        return true;
    case qwen_free_json::ValueKind::Null:
    case qwen_free_json::ValueKind::Missing:
    case qwen_free_json::ValueKind::Invalid:
        return false;
    }
    return false;
}

inline bool ReadSuccessField(const std::string& body,
                             bool& present,
                             bool& success) {
    present = false;
    success = false;
    const std::initializer_list<const char*> rootPath = {"success"};
    const std::initializer_list<const char*> dataPath = {"data", "success"};
    for (const auto path : {rootPath, dataPath}) {
        const qwen_free_json::ValueKind kind =
            qwen_free_json::GetValueKindAtPath(body, path);
        if (kind == qwen_free_json::ValueKind::Missing) continue;
        present = true;
        if (kind == qwen_free_json::ValueKind::Bool) {
            success = qwen_free_json::ExtractBoolAtPath(body, path);
            return true;
        }
        if (kind == qwen_free_json::ValueKind::Number) {
            const std::string value =
                qwen_free_json::ExtractNumberTextAtPath(body, path);
            success = value == "1" || value == "1.0";
            return true;
        }
        if (kind == qwen_free_json::ValueKind::String) {
            const std::string value = LowerAscii(
                qwen_free_json::ExtractStringAtPath(body, path));
            success = value == "true" || value == "1" ||
                      value == "yes" || value == "ok";
            return true;
        }
        // null/object/array is present but not a success scalar. Keep the
        // conservative failure result and let the caller reject the envelope.
        return true;
    }
    return false;
}

inline bool HasExplicitErrorEnvelope(const std::string& body) {
    const std::string code = ExtractBusinessCode(body);
    if (!code.empty() && !IsSuccessBusinessCode(code)) return true;

    for (const char* key : {"error", "error_msg", "error_message"}) {
        // Error fields are not always strings; the service has returned
        // envelopes such as {"error":{"code":...,"message":...}}.
        // Empty strings and null fields are common on successful responses
        // (for example error_msg=""), so only meaningful values count.
        if (IsMeaningfulErrorFieldAtPath(body, {key}) ||
            IsMeaningfulErrorFieldAtPath(body, {"data", key})) {
            return true;
        }
    }

    // `success` appears as bool, numeric 1/0, or a string in different
    // command envelopes.  Treat unknown present values conservatively as an
    // error instead of accepting a response with no output text.
    bool successPresent = false;
    bool success = false;
    if (ReadSuccessField(body, successPresent, success) && successPresent) {
        return !success;
    }
    return false;
}

// Some gateways omit `status` and use only success:true/1 (or code:0) for a
// successful HTTP-200 envelope.  Let the response parser promote that shape
// to the same explicit-success state used by status=success.
inline bool IsSuccessfulEnvelope(const std::string& body) {
    bool present = false;
    bool success = false;
    if (ReadSuccessField(body, present, success) && present) {
        return success;
    }
    return IsSuccessBusinessCode(ExtractBusinessCode(body));
}

inline bool IsAuthenticationFailure(const std::string& body) {
    const std::string code = ExtractBusinessCode(body);
    const std::string lowerCode = LowerAscii(code);
    if (lowerCode == "100000031" || lowerCode == "d30112") return true;

    // Inspect only structured error fields. Scanning the complete JSON body
    // misclassifies valid output text such as {"text":"signature ..."}.
    std::string structured;
    for (const char* key : {"error", "error_msg", "error_message"}) {
        structured = qwen_free_json::ExtractString(body, key);
        if (!structured.empty()) break;
    }
    if (structured.empty() && !code.empty() && !IsSuccessBusinessCode(code)) {
        structured = qwen_free_json::ExtractString(body, "message");
    }
    const std::string lower = LowerAscii(structured);
    if (lower.find("signature") != std::string::npos ||
        lower.find("sign failed") != std::string::npos ||
        structured.find("验签") != std::string::npos) {
        return true;
    }

    // Plain-text error bodies have no structured output field and may still
    // be scanned wholesale. JSON envelopes are intentionally excluded.
    const size_t first = body.find_first_not_of(" \t\r\n");
    if (first == std::string::npos || body[first] == '{' || body[first] == '[') {
        return false;
    }
    const std::string plain = LowerAscii(body);
    return plain.find("signature") != std::string::npos ||
           plain.find("sign failed") != std::string::npos ||
           body.find("验签") != std::string::npos;
}

inline size_t CountToken(const std::string& body, const std::string& token) {
    if (body.empty() || token.empty()) return 0;
    size_t count = 0;
    size_t pos = 0;
    while ((pos = body.find(token, pos)) != std::string::npos) {
        ++count;
        pos += token.size();
    }
    return count;
}

// Keep response-shape diagnostics useful without copying any returned text,
// device identity, signature, or arbitrary server payload into the log.
inline std::string SummarizeOutputShape(const std::string& body) {
    static constexpr const char* kFields[] = {
        "polished_text", "polishedText", "text", "content", "output",
        "answer", "result", "data", "response", "message"
    };

    std::string summary = "body_bytes=" + std::to_string(body.size());
    if (!body.empty()) {
        summary += " first=";
        const unsigned char first = static_cast<unsigned char>(body.front());
        summary += (first < 0x20u || first >= 0x7fu) ? "nonprintable" :
                   std::string(1, static_cast<char>(first));
    }

    for (const char* field : kFields) {
        const std::string key = std::string("\"") + field + "\"";
        const std::string escapedKey = std::string("\\\"") + field + "\\\"";
        summary += " " + std::string(field) + "_keys=" +
                   std::to_string(CountToken(body, key)) + "/" +
                   std::to_string(CountToken(body, escapedKey));
        const size_t valueLen = qwen_free_json::ExtractString(body, field).size();
        if (valueLen != 0) {
            summary += "_len=" + std::to_string(valueLen);
        }
    }
    return summary;
}

} // namespace qwen_free_diagnostics
