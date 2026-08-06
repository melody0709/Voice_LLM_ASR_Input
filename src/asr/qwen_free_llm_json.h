#pragma once

#include "qwen_free_json.h"

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

// Conservative parsing helpers for Qwen command responses. Rewrite responses
// may contain a completed message alongside a processing/loading message.
namespace qwen_free_llm_json {

inline std::string LowerAscii(std::string value) {
    for (char& c : value) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (byte < 0x80u) c = static_cast<char>(std::tolower(byte));
    }
    return value;
}

inline std::size_t MatchingObjectEnd(const std::string& json,
                                     std::size_t start) {
    if (start >= json.size() || json[start] != '{') return std::string::npos;

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = start; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            if (--depth == 0) return i;
        }
    }
    return std::string::npos;
}

// Unlike qwen_free_json::ExtractString(), this intentionally reads only a
// direct field of one object.  A terminal parent envelope can contain nested
// processing messages; mixing the parent's `status=complete` with a child's
// `content=loading` would be unsafe for selection replacement.
inline std::string ExtractDirectString(const std::string& object,
                                       const std::string& key) {
    if (object.size() < 2 || object.front() != '{' || object.back() != '}') {
        return {};
    }
    return qwen_free_json::ExtractStringAtPath(object, {key.c_str()});
}

inline std::string ExtractDirectNumber(const std::string& object,
                                       const std::string& key) {
    if (object.size() < 2 || object.front() != '{' || object.back() != '}') {
        return {};
    }
    return qwen_free_json::ExtractNumberTextAtPath(object, {key.c_str()});
}

inline bool ExtractDirectBool(const std::string& object,
                              const std::string& key,
                              bool& present) {
    present = qwen_free_json::GetValueKindAtPath(object, {key.c_str()}) ==
              qwen_free_json::ValueKind::Bool;
    return present && qwen_free_json::ExtractBoolAtPath(object, {key.c_str()});
}

inline bool IsTerminalStatus(const std::string& status) {
    const std::string lower = LowerAscii(status);
    return lower == "complete" || lower == "completed" ||
           lower == "success" || lower == "succeed" || lower == "ok" ||
           lower == "finished" || lower == "done" || lower == "final" ||
           lower == "true" || lower == "0" || lower == "1" ||
           lower == "200";
}

// Boolean-like terminal flags use a different numeric convention from a
// provider status code: 0 means false/not-final, while 1 means true/final.
// Do not reuse IsTerminalStatus here, because status=0 is a successful
// envelope in some Qwen responses but is_final=0 is explicitly non-terminal.
inline bool IsTerminalFlagValue(const std::string& value) {
    const std::string lower = LowerAscii(value);
    return lower == "true" || lower == "1" || lower == "1.0" ||
           lower == "yes" || lower == "y" || lower == "on" ||
           lower == "final" || lower == "complete" ||
           lower == "completed" || lower == "success" ||
           lower == "succeed" || lower == "ok" || lower == "done" ||
           lower == "finished";
}

inline bool HasExplicitFailure(const std::string& object) {
    const qwen_free_json::ValueKind kind =
        qwen_free_json::GetValueKindAtPath(object, {"success"});
    switch (kind) {
    case qwen_free_json::ValueKind::Bool:
        if (!qwen_free_json::ExtractBoolAtPath(object, {"success"})) {
            return true;
        }
        break;
    case qwen_free_json::ValueKind::Number: {
        const std::string value =
            qwen_free_json::ExtractNumberTextAtPath(object, {"success"});
        if (value == "0" || value == "0.0") return true;
        break;
    }
    case qwen_free_json::ValueKind::String: {
        const std::string value = LowerAscii(
            qwen_free_json::ExtractStringAtPath(object, {"success"}));
        if (value == "false" || value == "0" || value == "no" ||
            value == "off" || value == "failed" || value == "error") {
            return true;
        }
        break;
    }
    default:
        break;
    }

    const qwen_free_json::ValueKind statusKind =
        qwen_free_json::GetValueKindAtPath(object, {"status"});
    if (statusKind == qwen_free_json::ValueKind::String) {
        const std::string status = LowerAscii(
            qwen_free_json::ExtractStringAtPath(object, {"status"}));
        return status == "error" || status == "failed" ||
               status == "failure" || status == "rejected" ||
               status == "unauthorized" || status == "forbidden" ||
               status == "cancelled" || status == "canceled";
    }
    return false;
}

inline std::string ExtractTerminalOutput(const std::string& object) {
    // Keep this list deliberately explicit. `rewrite_query` and metadata
    // fields are request/diagnostic echoes, not replacement text.
    static constexpr const char* kOutputKeys[] = {
        "content", "text", "output", "output_text", "outputText",
        "polished_text", "polishedText", "answer", "result"
    };
    for (const char* key : kOutputKeys) {
        const std::string value = ExtractDirectString(object, key);
        if (!value.empty()) return value;
    }
    return {};
}

inline std::string ExtractTerminalContentDirect(const std::string& json) {
    if (HasExplicitFailure(json)) return {};
    // Scan innermost objects first, so an outer envelope cannot mask the
    // individual completed message objects inside it.
    std::vector<std::size_t> objectStarts;
    objectStarts.reserve(8);
    bool inString = false;
    bool escaped = false;
    for (std::size_t i = 0; i < json.size(); ++i) {
        const char c = json[i];
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            objectStarts.push_back(i);
        }
    }

    for (auto it = objectStarts.rbegin(); it != objectStarts.rend(); ++it) {
        const std::size_t start = *it;
        const std::size_t end = MatchingObjectEnd(json, start);
        if (end == std::string::npos) continue;

        const std::string object = json.substr(start, end - start + 1);
        if (HasExplicitFailure(object)) continue;
        bool terminal = false;
        if (qwen_free_json::GetValueKindAtPath(object, {"status"}) ==
            qwen_free_json::ValueKind::Bool) {
            terminal = qwen_free_json::ExtractBoolAtPath(object, {"status"});
        }
        if (!terminal) {
            terminal = IsTerminalStatus(ExtractDirectString(object, "status"));
        }
        if (!terminal) {
            const std::string numericStatus =
                ExtractDirectNumber(object, "status");
            terminal = IsTerminalStatus(numericStatus);
        }
        if (!terminal) {
            for (const char* key : {"is_final", "isFinal", "done", "finished"}) {
                bool present = false;
                if (ExtractDirectBool(object, key, present) && present) {
                    terminal = true;
                    break;
                }
                const std::string text = ExtractDirectString(object, key);
                if (IsTerminalFlagValue(text)) {
                    terminal = true;
                    break;
                }
                const std::string number = ExtractDirectNumber(object, key);
                if (IsTerminalFlagValue(number)) {
                    terminal = true;
                    break;
                }
            }
        }
        if (!terminal) continue;

        const std::string content = ExtractTerminalOutput(object);
        if (!content.empty()) return content;
    }
    return {};
}

inline std::string ExtractTerminalContent(const std::string& json,
                                          int depth = 0) {
    if (json.empty() || depth > 3) return {};
    // Do not accept a valid-looking nested object from a malformed outer
    // response. The caller must only treat a terminal result from a complete
    // JSON document as safe replacement text.
    if (!qwen_free_json::IsValidDocument(json)) return {};
    if (HasExplicitFailure(json)) return {};

    const std::size_t first = json.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    if (first != 0) {
        return ExtractTerminalContent(json.substr(first), depth);
    }

    std::string result = ExtractTerminalContentDirect(json);
    if (!result.empty()) return result;

    // Some responses wrap the actual JSON in an escaped data/result string.
    static constexpr const char* kContainers[] = {
        "data", "result", "response", "body", "payload"
    };
    for (const char* container : kContainers) {
        const std::string embedded = qwen_free_json::ExtractString(json, container);
        if (embedded.empty() || embedded == json) continue;
        const std::size_t firstPos = embedded.find_first_not_of(" \t\r\n");
        if (firstPos == std::string::npos) continue;
        const std::string trimmed = embedded.substr(firstPos);
        const char first = trimmed.front();
        if (first != '{' && first != '[') continue;
        result = ExtractTerminalContent(trimmed, depth + 1);
        if (!result.empty()) return result;
    }
    return {};
}

} // namespace qwen_free_llm_json
