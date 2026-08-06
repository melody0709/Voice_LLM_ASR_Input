#pragma once

#include "qwen_free_json.h"

#include <cstddef>
#include <string>

// Conservative parsing helpers for Qwen command responses. Rewrite responses
// may contain a completed message alongside a processing/loading message.
namespace qwen_free_llm_json {

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

    int depth = 0;
    std::size_t pos = 0;
    while (pos < object.size()) {
        const char c = object[pos];
        if (c == '{') {
            ++depth;
            ++pos;
            continue;
        }
        if (c == '}') {
            if (depth > 0) --depth;
            ++pos;
            continue;
        }
        if (c != '"') {
            ++pos;
            continue;
        }

        std::string field;
        if (!qwen_free_json::detail::ParseString(object, pos, field)) {
            return {};
        }
        qwen_free_json::detail::SkipWhitespace(object, pos);
        if (pos >= object.size() || object[pos] != ':') continue;
        ++pos;
        qwen_free_json::detail::SkipWhitespace(object, pos);
        if (depth != 1 || field != key || pos >= object.size() ||
            object[pos] != '"') {
            continue;
        }

        std::string value;
        return qwen_free_json::detail::ParseString(object, pos, value)
            ? value
            : std::string();
    }
    return {};
}

inline std::string ExtractTerminalContentDirect(const std::string& json) {
    // Scan innermost objects first, so an outer envelope cannot mask the
    // individual completed message objects inside it.
    for (std::size_t pos = json.size(); pos > 0; --pos) {
        const std::size_t start = pos - 1;
        if (json[start] != '{') continue;
        const std::size_t end = MatchingObjectEnd(json, start);
        if (end == std::string::npos) continue;

        const std::string object = json.substr(start, end - start + 1);
        const std::string status = ExtractDirectString(object, "status");
        if (status != "complete" && status != "success") continue;

        const std::string content = ExtractDirectString(object, "content");
        if (!content.empty()) return content;
    }
    return {};
}

inline std::string ExtractTerminalContent(const std::string& json,
                                          int depth = 0) {
    if (json.empty() || depth > 3) return {};

    const std::size_t first = json.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    if (first != 0) {
        return ExtractTerminalContent(json.substr(first), depth);
    }

    std::string result = ExtractTerminalContentDirect(json);
    if (!result.empty()) return result;

    // Some responses wrap the actual JSON in an escaped data/result string.
    static constexpr const char* kContainers[] = {
        "data", "result", "response", "body", "payload", "message"
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
