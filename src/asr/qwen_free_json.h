#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <utility>
#include <vector>

// Small, dependency-free JSON field reader used by the Qwen Free protocol
// clients.  It is deliberately limited to string and boolean object fields,
// but it is aware of JSON strings and nesting so a quoted transcript cannot
// be mistaken for a control field.
namespace qwen_free_json {

namespace detail {

inline void SkipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' ||
            json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
}

inline int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline bool ParseHex4(const std::string& json, size_t pos, uint32_t& value) {
    if (pos + 4 > json.size()) return false;
    value = 0;
    for (size_t i = 0; i < 4; ++i) {
        const int digit = HexValue(json[pos + i]);
        if (digit < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    return true;
}

inline void AppendUtf8(uint32_t codePoint, std::string& out) {
    if (codePoint <= 0x7fu) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7ffu) {
        out.push_back(static_cast<char>(0xc0u | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else if (codePoint <= 0xffffu) {
        out.push_back(static_cast<char>(0xe0u | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    } else {
        out.push_back(static_cast<char>(0xf0u | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 12) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((codePoint >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (codePoint & 0x3fu)));
    }
}

inline bool ParseString(const std::string& json,
                        size_t& pos,
                        std::string& out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    out.clear();

    while (pos < json.size()) {
        const unsigned char c = static_cast<unsigned char>(json[pos++]);
        if (c == '"') return true;
        if (c != '\\') {
            if (c < 0x20u) return false;
            out.push_back(static_cast<char>(c));
            continue;
        }

        if (pos >= json.size()) return false;
        const char escaped = json[pos++];
        switch (escaped) {
        case '"': out.push_back('"'); break;
        case '\\': out.push_back('\\'); break;
        case '/': out.push_back('/'); break;
        case 'b': out.push_back('\b'); break;
        case 'f': out.push_back('\f'); break;
        case 'n': out.push_back('\n'); break;
        case 'r': out.push_back('\r'); break;
        case 't': out.push_back('\t'); break;
        case 'u': {
            uint32_t codeUnit = 0;
            if (!ParseHex4(json, pos, codeUnit)) return false;
            pos += 4;

            if (codeUnit >= 0xd800u && codeUnit <= 0xdbffu) {
                const size_t lowStart = pos;
                uint32_t low = 0;
                if (pos + 6 <= json.size() && json[pos] == '\\' &&
                    json[pos + 1] == 'u' &&
                    ParseHex4(json, pos + 2, low) &&
                    low >= 0xdc00u && low <= 0xdfffu) {
                    pos += 6;
                    const uint32_t codePoint =
                        0x10000u + ((codeUnit - 0xd800u) << 10) +
                        (low - 0xdc00u);
                    AppendUtf8(codePoint, out);
                } else {
                    // Preserve valid JSON parsing even for an unpaired
                    // surrogate; replacement is safer than emitting invalid
                    // UTF-8 into the Win32 conversion layer.
                    pos = lowStart;
                    AppendUtf8(0xfffdu, out);
                }
            } else if (codeUnit >= 0xdc00u && codeUnit <= 0xdfffu) {
                AppendUtf8(0xfffdu, out);
            } else {
                AppendUtf8(codeUnit, out);
            }
            break;
        }
        default:
            // Unknown escapes are not JSON.  Treating them as literal text
            // would make malformed provider envelopes look parseable and
            // could expose a partial field to the protocol state machine.
            return false;
        }
    }
    return false;
}

struct StringCandidate {
    std::string key;
    std::string value;
    std::vector<std::string> path;
    int objectDepth = 0;
    int arrayDepth = 0;
};

struct BoolCandidate {
    std::string key;
    bool value = false;
    std::vector<std::string> path;
    int objectDepth = 0;
    int arrayDepth = 0;
};

struct RawCandidate {
    std::string key;
    std::string value;
    std::vector<std::string> path;
    int objectDepth = 0;
    int arrayDepth = 0;
};

// Presence is tracked separately from typed candidates so callers can
// distinguish object/array/null fields from a missing field.
struct ValueCandidate {
    std::vector<std::string> path;
};

inline bool IsValueDelimiter(char c) {
    return c == ',' || c == ']' || c == '}' ||
           c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

inline bool ParseNumber(const std::string& json, size_t& pos,
                        std::string& out) {
    const size_t start = pos;
    if (pos < json.size() && json[pos] == '-') ++pos;

    if (pos >= json.size()) return false;
    if (json[pos] == '0') {
        ++pos;
        // JSON forbids a leading zero before another integer digit.
        if (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            return false;
        }
    } else {
        if (json[pos] < '1' || json[pos] > '9') return false;
        do {
            ++pos;
        } while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9');
    }

    if (pos < json.size() && json[pos] == '.') {
        ++pos;
        const size_t fractionStart = pos;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            ++pos;
        }
        if (pos == fractionStart) return false;
    }

    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        ++pos;
        if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) ++pos;
        const size_t exponentStart = pos;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') {
            ++pos;
        }
        if (pos == exponentStart) return false;
    }

    if (pos < json.size() && !IsValueDelimiter(json[pos])) return false;
    out = json.substr(start, pos - start);
    return !out.empty();
}

inline bool ParseValue(const std::string& json,
                       size_t& pos,
                       std::vector<std::string>& path,
                       int objectDepth,
                       int arrayDepth,
                       std::vector<StringCandidate>& strings,
                       std::vector<BoolCandidate>& bools,
                       std::vector<RawCandidate>& raws,
                       size_t recursionDepth = 0,
                       std::vector<ValueCandidate>* values = nullptr) {
    constexpr size_t kMaxJsonDepth = 128;
    if (recursionDepth > kMaxJsonDepth) return false;
    SkipWhitespace(json, pos);
    if (pos >= json.size()) return false;
    if (values && !path.empty()) values->push_back({path});

    if (json[pos] == '"') {
        std::string value;
        if (!ParseString(json, pos, value)) return false;
        if (!path.empty()) {
            StringCandidate candidate;
            candidate.key = path.back();
            candidate.value = std::move(value);
            candidate.path = path;
            candidate.objectDepth = objectDepth;
            candidate.arrayDepth = arrayDepth;
            strings.push_back(std::move(candidate));
        }
        return true;
    }

    if (json[pos] == '{') {
        ++pos;
        SkipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') {
            ++pos;
            return true;
        }
        for (;;) {
            SkipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] != '"') return false;
            std::string key;
            if (!ParseString(json, pos, key)) return false;
            SkipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] != ':') return false;
            ++pos;
            path.push_back(key);
            const bool ok = ParseValue(json, pos, path,
                                       objectDepth + 1, arrayDepth,
                                       strings, bools, raws, recursionDepth + 1,
                                       values);
            path.pop_back();
            if (!ok) return false;
            SkipWhitespace(json, pos);
            if (pos >= json.size()) return false;
            if (json[pos] == '}') {
                ++pos;
                return true;
            }
            if (json[pos] != ',') return false;
            ++pos;
        }
    }

    if (json[pos] == '[') {
        ++pos;
        SkipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') {
            ++pos;
            return true;
        }
        size_t index = 0;
        for (;;) {
            path.push_back("#" + std::to_string(index++));
            const bool ok = ParseValue(json, pos, path,
                                       objectDepth, arrayDepth + 1,
                                       strings, bools, raws, recursionDepth + 1,
                                       values);
            path.pop_back();
            if (!ok) return false;
            SkipWhitespace(json, pos);
            if (pos >= json.size()) return false;
            if (json[pos] == ']') {
                ++pos;
                return true;
            }
            if (json[pos] != ',') return false;
            ++pos;
        }
    }

    if (json.compare(pos, 4, "true") == 0) {
        pos += 4;
        if (pos < json.size() && !IsValueDelimiter(json[pos])) return false;
        if (!path.empty()) {
            BoolCandidate candidate;
            candidate.key = path.back();
            candidate.value = true;
            candidate.path = path;
            candidate.objectDepth = objectDepth;
            candidate.arrayDepth = arrayDepth;
            bools.push_back(std::move(candidate));
        }
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        pos += 5;
        if (pos < json.size() && !IsValueDelimiter(json[pos])) return false;
        if (!path.empty()) {
            BoolCandidate candidate;
            candidate.key = path.back();
            candidate.value = false;
            candidate.path = path;
            candidate.objectDepth = objectDepth;
            candidate.arrayDepth = arrayDepth;
            bools.push_back(std::move(candidate));
        }
        return true;
    }

    if (json.compare(pos, 4, "null") == 0) {
        pos += 4;
        return pos == json.size() || IsValueDelimiter(json[pos]);
    }

    std::string raw;
    if (!ParseNumber(json, pos, raw)) return false;
    if (!path.empty()) {
        RawCandidate candidate;
        candidate.key = path.back();
        candidate.value = raw;
        candidate.path = path;
        candidate.objectDepth = objectDepth;
        candidate.arrayDepth = arrayDepth;
        raws.push_back(std::move(candidate));
    }
    return true;
}

inline bool ParseDocument(const std::string& json,
                          std::vector<StringCandidate>& strings,
                          std::vector<BoolCandidate>& bools,
                          std::vector<RawCandidate>& raws,
                          std::vector<ValueCandidate>* values = nullptr) {
    if (json.empty()) return false;
    size_t pos = 0;
    std::vector<std::string> path;
    SkipWhitespace(json, pos);
    if (!ParseValue(json, pos, path, 0, 0, strings, bools, raws, 0,
                    values)) return false;
    SkipWhitespace(json, pos);
    return pos == json.size();
}

inline bool CandidateIsBetter(int arrayDepth, int objectDepth,
                              size_t pathLength,
                              int bestArrayDepth, int bestObjectDepth,
                              size_t bestPathLength) {
    if (arrayDepth != bestArrayDepth) return arrayDepth < bestArrayDepth;
    if (objectDepth != bestObjectDepth) return objectDepth < bestObjectDepth;
    return pathLength < bestPathLength;
}

inline bool PathEquals(const std::vector<std::string>& actual,
                       std::initializer_list<const char*> expected) {
    if (actual.size() != expected.size()) return false;
    size_t i = 0;
    for (const char* part : expected) {
        if (!part || actual[i++] != part) return false;
    }
    return true;
}

} // namespace detail

inline bool IsValidDocument(const std::string& json) {
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    return detail::ParseDocument(json, strings, bools, raws);
}

inline std::string ExtractString(const std::string& json,
                                 const std::string& key) {
    if (json.empty() || key.empty()) return {};
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return {};

    const detail::StringCandidate* best = nullptr;
    for (const auto& candidate : strings) {
        if (candidate.key != key) continue;
        if (!best || detail::CandidateIsBetter(
                candidate.arrayDepth, candidate.objectDepth,
                candidate.path.size(), best->arrayDepth, best->objectDepth,
                best->path.size()) ||
            (candidate.arrayDepth == best->arrayDepth &&
             candidate.objectDepth == best->objectDepth &&
             candidate.path.size() == best->path.size() &&
             best->value.empty() && !candidate.value.empty())) {
            best = &candidate;
        }
    }
    return best ? best->value : std::string();
}

inline std::string ExtractStringAtPath(
    const std::string& json,
    std::initializer_list<const char*> path) {
    if (json.empty() || path.size() == 0) return {};
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return {};
    for (const auto& candidate : strings) {
        if (detail::PathEquals(candidate.path, path)) return candidate.value;
    }
    return {};
}

inline bool ExtractBool(const std::string& json,
                        const std::string& key) {
    if (json.empty() || key.empty()) return false;

    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return false;

    const detail::BoolCandidate* best = nullptr;
    for (const auto& candidate : bools) {
        if (candidate.key != key) continue;
        if (!best || detail::CandidateIsBetter(
                candidate.arrayDepth, candidate.objectDepth,
                candidate.path.size(), best->arrayDepth, best->objectDepth,
                best->path.size())) {
            best = &candidate;
        }
    }
    return best ? best->value : false;
}

inline std::string ExtractNumberText(const std::string& json,
                                     const std::string& key) {
    if (json.empty() || key.empty()) return {};
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return {};

    const detail::RawCandidate* best = nullptr;
    for (const auto& candidate : raws) {
        if (candidate.key != key) continue;
        if (!best || detail::CandidateIsBetter(
                candidate.arrayDepth, candidate.objectDepth,
                candidate.path.size(), best->arrayDepth, best->objectDepth,
                best->path.size())) {
            best = &candidate;
        }
    }
    return best ? best->value : std::string();
}

inline std::string ExtractNumberTextAtPath(
    const std::string& json,
    std::initializer_list<const char*> path) {
    if (json.empty() || path.size() == 0) return {};
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return {};
    for (const auto& candidate : raws) {
        if (detail::PathEquals(candidate.path, path)) return candidate.value;
    }
    return {};
}

inline bool ExtractBoolAtPath(const std::string& json,
                              std::initializer_list<const char*> path) {
    if (json.empty() || path.size() == 0) return false;
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return false;
    for (const auto& candidate : bools) {
        if (detail::PathEquals(candidate.path, path)) return candidate.value;
    }
    return false;
}

inline bool HasFieldAtPath(const std::string& json,
                           std::initializer_list<const char*> path) {
    if (json.empty() || path.size() == 0) return false;
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    if (!detail::ParseDocument(json, strings, bools, raws)) return false;
    for (const auto& candidate : strings) {
        if (detail::PathEquals(candidate.path, path)) return true;
    }
    for (const auto& candidate : bools) {
        if (detail::PathEquals(candidate.path, path)) return true;
    }
    for (const auto& candidate : raws) {
        if (detail::PathEquals(candidate.path, path)) return true;
    }
    return false;
}

// Presence-aware variant that also sees object/array/null values.  The
// legacy HasFieldAtPath intentionally remains typed-only for protocol flags.
inline bool HasValueAtPath(const std::string& json,
                           std::initializer_list<const char*> path) {
    if (json.empty() || path.size() == 0) return false;
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    std::vector<detail::ValueCandidate> values;
    if (!detail::ParseDocument(json, strings, bools, raws, &values)) return false;
    for (const auto& candidate : values) {
        if (detail::PathEquals(candidate.path, path)) return true;
    }
    return false;
}

// True when any value in a valid document uses the requested key, including
// nested object/array/null values.
inline bool HasKey(const std::string& json, const std::string& key) {
    if (json.empty() || key.empty()) return false;
    std::vector<detail::StringCandidate> strings;
    std::vector<detail::BoolCandidate> bools;
    std::vector<detail::RawCandidate> raws;
    std::vector<detail::ValueCandidate> values;
    if (!detail::ParseDocument(json, strings, bools, raws, &values)) return false;
    for (const auto& candidate : values) {
        if (!candidate.path.empty() && candidate.path.back() == key) return true;
    }
    return false;
}

} // namespace qwen_free_json
