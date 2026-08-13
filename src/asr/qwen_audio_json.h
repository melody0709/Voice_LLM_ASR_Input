#pragma once

#include "utils.h"

#include <cwctype>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <string_view>

namespace qwen_audio_json {
namespace detail {

inline void SkipWs(std::wstring_view text, size_t& pos) {
    while (pos < text.size() && iswspace(text[pos])) ++pos;
}

inline bool Hex(wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
}

inline bool ParseString(std::wstring_view text, size_t& pos) {
    if (pos >= text.size() || text[pos++] != L'\"') return false;
    while (pos < text.size()) {
        const wchar_t c = text[pos++];
        if (c == L'\"') return true;
        if (c < 0x20) return false;
        if (c != L'\\') continue;
        if (pos >= text.size()) return false;
        const wchar_t e = text[pos++];
        if (e == L'u') {
            if (pos + 4 > text.size()) return false;
            for (int i = 0; i < 4; ++i) if (!Hex(text[pos++])) return false;
        } else if (e != L'\"' && e != L'\\' && e != L'/' && e != L'b' && e != L'f' &&
                   e != L'n' && e != L'r' && e != L't') {
            return false;
        }
    }
    return false;
}

inline bool DecodeWideString(std::wstring_view text, size_t& pos, std::wstring& out) {
    if (pos >= text.size() || text[pos++] != L'\"') return false;
    out.clear();
    while (pos < text.size()) {
        const wchar_t c = text[pos++];
        if (c == L'\"') return true;
        if (c < 0x20) return false;
        if (c != L'\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= text.size()) return false;
        const wchar_t e = text[pos++];
        switch (e) {
        case L'\"': out.push_back(L'\"'); break;
        case L'\\': out.push_back(L'\\'); break;
        case L'/': out.push_back(L'/'); break;
        case L'b': out.push_back(L'\b'); break;
        case L'f': out.push_back(L'\f'); break;
        case L'n': out.push_back(L'\n'); break;
        case L'r': out.push_back(L'\r'); break;
        case L't': out.push_back(L'\t'); break;
        case L'u': {
            if (pos + 4 > text.size()) return false;
            uint32_t cp = 0;
            for (int i = 0; i < 4; ++i) {
                const wchar_t ch = text[pos++];
                cp <<= 4;
                if (ch >= L'0' && ch <= L'9') cp |= static_cast<uint32_t>(ch - L'0');
                else if (ch >= L'a' && ch <= L'f') cp |= static_cast<uint32_t>(ch - L'a' + 10);
                else if (ch >= L'A' && ch <= L'F') cp |= static_cast<uint32_t>(ch - L'A' + 10);
                else return false;
            }
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (pos + 6 > text.size() || text[pos] != L'\\' || text[pos + 1] != L'u') return false;
                pos += 2;
                uint32_t low = 0;
                for (int i = 0; i < 4; ++i) {
                    const wchar_t ch = text[pos++];
                    low <<= 4;
                    if (ch >= L'0' && ch <= L'9') low |= static_cast<uint32_t>(ch - L'0');
                    else if (ch >= L'a' && ch <= L'f') low |= static_cast<uint32_t>(ch - L'a' + 10);
                    else if (ch >= L'A' && ch <= L'F') low |= static_cast<uint32_t>(ch - L'A' + 10);
                    else return false;
                }
                if (low < 0xdc00 || low > 0xdfff) return false;
                out.push_back(static_cast<wchar_t>(cp));
                out.push_back(static_cast<wchar_t>(low));
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                return false;
            } else {
                out.push_back(static_cast<wchar_t>(cp));
            }
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

inline bool ParseValue(std::wstring_view text, size_t& pos);

inline bool ParseArray(std::wstring_view text, size_t& pos) {
    if (pos >= text.size() || text[pos++] != L'[') return false;
    SkipWs(text, pos);
    if (pos < text.size() && text[pos] == L']') { ++pos; return true; }
    while (pos < text.size()) {
        if (!ParseValue(text, pos)) return false;
        SkipWs(text, pos);
        if (pos < text.size() && text[pos] == L']') { ++pos; return true; }
        if (pos >= text.size() || text[pos++] != L',') return false;
        SkipWs(text, pos);
    }
    return false;
}

inline bool ParseObject(std::wstring_view text, size_t& pos) {
    if (pos >= text.size() || text[pos++] != L'{') return false;
    SkipWs(text, pos);
    if (pos < text.size() && text[pos] == L'}') { ++pos; return true; }
    while (pos < text.size()) {
        if (!ParseString(text, pos)) return false;
        SkipWs(text, pos);
        if (pos >= text.size() || text[pos++] != L':') return false;
        SkipWs(text, pos);
        if (!ParseValue(text, pos)) return false;
        SkipWs(text, pos);
        if (pos < text.size() && text[pos] == L'}') { ++pos; return true; }
        if (pos >= text.size() || text[pos++] != L',') return false;
        SkipWs(text, pos);
    }
    return false;
}

inline bool ParseNumber(std::wstring_view text, size_t& pos) {
    const size_t start = pos;
    if (pos < text.size() && text[pos] == L'-') ++pos;
    if (pos >= text.size()) return false;
    if (text[pos] == L'0') ++pos;
    else {
        if (text[pos] < L'1' || text[pos] > L'9') return false;
        while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') ++pos;
    }
    if (pos < text.size() && text[pos] == L'.') {
        ++pos;
        const size_t fraction = pos;
        while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') ++pos;
        if (fraction == pos) return false;
    }
    if (pos < text.size() && (text[pos] == L'e' || text[pos] == L'E')) {
        ++pos;
        if (pos < text.size() && (text[pos] == L'+' || text[pos] == L'-')) ++pos;
        const size_t exponent = pos;
        while (pos < text.size() && text[pos] >= L'0' && text[pos] <= L'9') ++pos;
        if (exponent == pos) return false;
    }
    return pos > start;
}

inline bool ParseValue(std::wstring_view text, size_t& pos) {
    SkipWs(text, pos);
    if (pos >= text.size()) return false;
    if (text[pos] == L'\"') return ParseString(text, pos);
    if (text[pos] == L'{') return ParseObject(text, pos);
    if (text[pos] == L'[') return ParseArray(text, pos);
    if (text.compare(pos, 4, L"true") == 0) { pos += 4; return true; }
    if (text.compare(pos, 5, L"false") == 0) { pos += 5; return true; }
    if (text.compare(pos, 4, L"null") == 0) { pos += 4; return true; }
    return ParseNumber(text, pos);
}

inline void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0x10ffff) {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
}

inline bool ParseHex4(std::string_view text, size_t& pos, uint32_t& value) {
    if (pos + 4 > text.size()) return false;
    value = 0;
    for (int i = 0; i < 4; ++i) {
        const char ch = text[pos++];
        value <<= 4;
        if (ch >= '0' && ch <= '9') value |= static_cast<uint32_t>(ch - '0');
        else if (ch >= 'a' && ch <= 'f') value |= static_cast<uint32_t>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F') value |= static_cast<uint32_t>(ch - 'A' + 10);
        else return false;
    }
    return true;
}

inline bool DecodeNarrowString(std::string_view text, size_t& pos, std::string& out) {
    if (pos >= text.size() || text[pos++] != '"') return false;
    out.clear();
    while (pos < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[pos++]);
        if (ch == '"') return true;
        if (ch < 0x20) return false;
        if (ch != '\\') {
            out.push_back(static_cast<char>(ch));
            continue;
        }
        if (pos >= text.size()) return false;
        const char escaped = text[pos++];
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
            uint32_t cp = 0;
            if (!ParseHex4(text, pos, cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (pos + 2 > text.size() || text[pos] != '\\' || text[pos + 1] != 'u') return false;
                pos += 2;
                uint32_t low = 0;
                if (!ParseHex4(text, pos, low) || low < 0xdc00 || low > 0xdfff) return false;
                cp = 0x10000u + ((cp - 0xd800u) << 10) + (low - 0xdc00u);
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                return false;
            }
            AppendUtf8(out, cp);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

inline void SkipNarrowWs(std::string_view text, size_t& pos) {
    while (pos < text.size() &&
           (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) {
        ++pos;
    }
}

inline bool SkipNarrowValue(std::string_view text, size_t& pos) {
    SkipNarrowWs(text, pos);
    if (pos >= text.size()) return false;
    if (text[pos] == '"') {
        std::string ignored;
        return DecodeNarrowString(text, pos, ignored);
    }
    if (text[pos] == '{') {
        ++pos;
        SkipNarrowWs(text, pos);
        if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
        while (pos < text.size()) {
            std::string key;
            if (!DecodeNarrowString(text, pos, key)) return false;
            SkipNarrowWs(text, pos);
            if (pos >= text.size() || text[pos++] != ':') return false;
            if (!SkipNarrowValue(text, pos)) return false;
            SkipNarrowWs(text, pos);
            if (pos < text.size() && text[pos] == '}') { ++pos; return true; }
            if (pos >= text.size() || text[pos++] != ',') return false;
            SkipNarrowWs(text, pos);
        }
        return false;
    }
    if (text[pos] == '[') {
        ++pos;
        SkipNarrowWs(text, pos);
        if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
        while (pos < text.size()) {
            if (!SkipNarrowValue(text, pos)) return false;
            SkipNarrowWs(text, pos);
            if (pos < text.size() && text[pos] == ']') { ++pos; return true; }
            if (pos >= text.size() || text[pos++] != ',') return false;
            SkipNarrowWs(text, pos);
        }
        return false;
    }
    const size_t start = pos;
    while (pos < text.size() && text[pos] != ',' && text[pos] != '}' && text[pos] != ']' &&
           text[pos] != ' ' && text[pos] != '\t' && text[pos] != '\r' && text[pos] != '\n') {
        ++pos;
    }
    return pos > start;
}

inline bool FindStringAtPath(std::string_view text,
                             size_t& pos,
                             const std::string_view* path,
                             size_t pathSize,
                             size_t pathIndex,
                             std::wstring& out) {
    SkipNarrowWs(text, pos);
    if (pos >= text.size() || text[pos++] != '{') return false;
    SkipNarrowWs(text, pos);
    while (pos < text.size() && text[pos] != '}') {
        std::string key;
        if (!DecodeNarrowString(text, pos, key)) return false;
        SkipNarrowWs(text, pos);
        if (pos >= text.size() || text[pos++] != ':') return false;
        SkipNarrowWs(text, pos);
        if (key == path[pathIndex]) {
            if (pathIndex + 1 == pathSize) {
                std::string decoded;
                if (!DecodeNarrowString(text, pos, decoded)) return false;
                out = Utf8ToWide(decoded);
                return true;
            }
            return FindStringAtPath(text, pos, path, pathSize, pathIndex + 1, out);
        }
        if (!SkipNarrowValue(text, pos)) return false;
        SkipNarrowWs(text, pos);
        if (pos < text.size() && text[pos] == '}') break;
        if (pos >= text.size() || text[pos++] != ',') return false;
        SkipNarrowWs(text, pos);
    }
    return false;
}

inline size_t UnicodeScalarCount(std::wstring_view value) {
    size_t count = 0;
    for (size_t i = 0; i < value.size(); ++i) {
        const uint32_t ch = static_cast<uint32_t>(value[i]);
        if (ch >= 0xd800 && ch <= 0xdbff && i + 1 < value.size()) {
            const uint32_t low = static_cast<uint32_t>(value[i + 1]);
            if (low >= 0xdc00 && low <= 0xdfff) ++i;
        }
        ++count;
    }
    return count;
}

inline bool IsValidVocabularyTerm(std::wstring_view term) {
    if (term.empty()) return false;
    bool hasNonAscii = false;
    for (wchar_t ch : term) {
        if (static_cast<uint32_t>(ch) > 0x7f) {
            hasNonAscii = true;
            break;
        }
    }
    if (hasNonAscii) return UnicodeScalarCount(term) <= 15;

    size_t segments = 0;
    bool inSegment = false;
    for (wchar_t ch : term) {
        if (iswspace(ch)) {
            inSegment = false;
        } else if (!inSegment) {
            inSegment = true;
            if (++segments > 7) return false;
        }
    }
    return segments > 0;
}

} // namespace detail

// Shared Unicode scalar to UTF-8 encoder used by the provider parsers. Keep
// one implementation so JSON escape handling cannot diverge between the
// Audio 3 and Qwen3 protocol stacks.
inline void AppendUtf8(std::string& out, uint32_t cp) {
    detail::AppendUtf8(out, cp);
}

inline bool IsValidValue(const std::wstring& value) {
    const std::wstring trimmed = Trim(value);
    if (trimmed.empty()) return false;
    size_t pos = 0;
    if (!detail::ParseValue(trimmed, pos)) return false;
    detail::SkipWs(trimmed, pos);
    return pos == trimmed.size();
}

inline bool IsValidVocabulary(const std::wstring& value, std::wstring* error = nullptr) {
    const std::wstring text = Trim(value);
    if (text.empty()) return true;
    size_t pos = 0;
    detail::SkipWs(text, pos);
    if (pos >= text.size() || text[pos++] != L'{') {
        if (error) *error = L"vocabulary must be a JSON object";
        return false;
    }
    detail::SkipWs(text, pos);
    size_t count = 0;
    size_t superCount = 0;
    if (pos < text.size() && text[pos] == L'}') {
        ++pos;
    } else {
        while (pos < text.size()) {
            std::wstring term;
            if (!detail::DecodeWideString(text, pos, term)) {
                if (error) *error = L"vocabulary keys must be JSON strings";
                return false;
            }
            if (!detail::IsValidVocabularyTerm(term)) {
                if (error) *error = L"vocabulary term exceeds the documented text limits";
                return false;
            }
            detail::SkipWs(text, pos);
            if (pos >= text.size() || text[pos++] != L':') {
                if (error) *error = L"vocabulary entry is missing a weight";
                return false;
            }
            detail::SkipWs(text, pos);
            const size_t weightStart = pos;
            if (!detail::ParseNumber(text, pos)) {
                if (error) *error = L"vocabulary weights must be numbers";
                return false;
            }
            const std::wstring weightText(text.substr(weightStart, pos - weightStart));
            wchar_t* end = nullptr;
            const double weight = wcstod(weightText.c_str(), &end);
            if (!end || *end != L'\0' || weightText.find_first_of(L".eE") != std::wstring::npos ||
                weight != static_cast<int>(weight) ||
                !((weight >= 1.0 && weight <= 5.0) || weight == 50.0)) {
                if (error) *error = L"vocabulary weights must be integers 1-5 or 50";
                return false;
            }
            ++count;
            if (weight == 50.0) ++superCount;
            if (count > 2000 || superCount > 50) {
                if (error) *error = count > 2000
                    ? L"vocabulary cannot exceed 2000 entries"
                    : L"vocabulary cannot exceed 50 entries with weight 50";
                return false;
            }
            detail::SkipWs(text, pos);
            if (pos < text.size() && text[pos] == L'}') { ++pos; break; }
            if (pos >= text.size() || text[pos++] != L',') {
                if (error) *error = L"vocabulary object is malformed";
                return false;
            }
            detail::SkipWs(text, pos);
        }
    }
    detail::SkipWs(text, pos);
    if (pos != text.size()) {
        if (error) *error = L"vocabulary contains trailing data";
        return false;
    }
    return true;
}

inline bool HasVocabulary(const std::wstring& value) {
    return !Trim(value).empty();
}

inline bool HasValidVocabulary(const std::wstring& value, std::wstring* error = nullptr) {
    return HasVocabulary(value) && IsValidVocabulary(value, error);
}

inline std::wstring ExtractString(const std::string& json, const std::string& key) {
    const std::string marker = "\"" + key + "\"";
    size_t search = 0;
    while ((search = json.find(marker, search)) != std::string::npos) {
        size_t pos = json.find(':', search + marker.size());
        if (pos == std::string::npos) return {};
        ++pos;
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
            ++pos;
        }
        if (pos >= json.size() || json[pos] != '"') {
            search += marker.size();
            continue;
        }
        std::string decoded;
        if (!detail::DecodeNarrowString(json, pos, decoded)) return {};
        return Utf8ToWide(decoded);
    }
    return {};
}

template <size_t N>
inline std::wstring ExtractStringAtPath(
    const std::string& json,
    const std::string_view (&path)[N]) {
    static_assert(N > 0, "JSON path cannot be empty");
    size_t pos = 0;
    std::wstring result;
    if (!detail::FindStringAtPath(json, pos, path, N, 0, result)) return {};
    return result;
}

inline bool ExtractBool(const std::string& json, const std::string& key, bool fallback = false) {
    const std::string marker = "\"" + key + "\"";
    size_t search = 0;
    while ((search = json.find(marker, search)) != std::string::npos) {
        size_t pos = json.find(':', search + marker.size());
        if (pos == std::string::npos) return fallback;
        ++pos;
        while (pos < json.size() &&
               (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
            ++pos;
        }
        if (json.compare(pos, 4, "true") == 0) return true;
        if (json.compare(pos, 5, "false") == 0) return false;
        search += marker.size();
    }
    return fallback;
}

} // namespace qwen_audio_json
