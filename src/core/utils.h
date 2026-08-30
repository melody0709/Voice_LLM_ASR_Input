#pragma once

#include <climits>
#include <cstdint>
#include <string>
#include <windows.h>

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(INT_MAX)) return {};
    const int length = static_cast<int>(value.size());
    int required = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), length, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), length,
                            result.data(), required, nullptr, nullptr) != required) {
        return {};
    }
    return result;
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<size_t>(INT_MAX)) return {};
    const int length = static_cast<int>(value.size());
    int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), length, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.data(), length,
                            result.data(), required) != required) {
        return {};
    }
    return result;
}

inline std::string EscapeJson(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::string out;
    out.reserve(utf8.size() + 8);
    constexpr char kHex[] = "0123456789abcdef";
    for (unsigned char c : utf8) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                out += "\\u00";
                out.push_back(kHex[(c >> 4) & 0x0f]);
                out.push_back(kHex[c & 0x0f]);
            } else {
                out.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return out;
}

inline std::wstring Trim(std::wstring value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

// JSON string decoding shared by cloud providers. Malformed input is rejected
// as a whole; callers never receive a partially decoded provider response.
namespace json_detail {

inline void SkipWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\n' || json[pos] == '\r')) {
        ++pos;
    }
}

inline bool AppendCodePointUtf8(std::string& out, uint32_t cp) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
    if (cp <= 0x7f) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(static_cast<char>(0xc0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(static_cast<char>(0xe0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(static_cast<char>(0xf0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
    }
    return true;
}

inline bool Utf8ToWideStrict(const std::string& value, std::wstring& out) {
    out.clear();
    if (value.empty()) return true;
    if (value.size() > static_cast<size_t>(INT_MAX)) return false;
    const int length = static_cast<int>(value.size());
    const int required = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length, nullptr, 0);
    if (required <= 0) return false;
    out.resize(static_cast<size_t>(required));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), length,
                               out.data(), required) == required;
}

inline bool ReadHex4(const std::string& json, size_t& pos, uint32_t& value) {
    if (pos + 4 > json.size()) return false;
    value = 0;
    for (size_t k = 0; k < 4; ++k) {
        const char h = json[pos + k];
        int digit = -1;
        if (h >= '0' && h <= '9') digit = h - '0';
        else if (h >= 'a' && h <= 'f') digit = h - 'a' + 10;
        else if (h >= 'A' && h <= 'F') digit = h - 'A' + 10;
        if (digit < 0) return false;
        value = (value << 4) | static_cast<uint32_t>(digit);
    }
    pos += 4;
    return true;
}

inline bool DecodeString(const std::string& json, size_t startQuote,
                         std::wstring& decoded, size_t* nextPos = nullptr) {
    if (startQuote >= json.size() || json[startQuote] != '"') return false;
    size_t pos = startQuote + 1;
    std::string utf8;
    while (pos < json.size()) {
        const unsigned char c = static_cast<unsigned char>(json[pos++]);
        if (c == '"') {
            if (!Utf8ToWideStrict(utf8, decoded)) return false;
            if (nextPos) *nextPos = pos;
            return true;
        }
        if (c < 0x20) return false;
        if (c != '\\') {
            utf8.push_back(static_cast<char>(c));
            continue;
        }
        if (pos >= json.size()) return false;
        const char esc = json[pos++];
        switch (esc) {
        case '"': utf8.push_back('"'); break;
        case '\\': utf8.push_back('\\'); break;
        case '/': utf8.push_back('/'); break;
        case 'b': utf8.push_back('\b'); break;
        case 'f': utf8.push_back('\f'); break;
        case 'n': utf8.push_back('\n'); break;
        case 'r': utf8.push_back('\r'); break;
        case 't': utf8.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!ReadHex4(json, pos, cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff) {
                if (pos + 2 > json.size() || json[pos] != '\\' || json[pos + 1] != 'u') {
                    return false;
                }
                pos += 2;
                uint32_t low = 0;
                if (!ReadHex4(json, pos, low) || low < 0xdc00 || low > 0xdfff) {
                    return false;
                }
                cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
            } else if (cp >= 0xdc00 && cp <= 0xdfff) {
                return false;
            }
            if (!AppendCodePointUtf8(utf8, cp)) return false;
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

inline size_t FindValueForKey(const std::string& json, const std::string& key) {
    const std::wstring wanted = Utf8ToWide(key);
    size_t pos = 0;
    while (pos < json.size()) {
        if (json[pos] != '"') {
            ++pos;
            continue;
        }
        std::wstring token;
        size_t next = 0;
        if (!DecodeString(json, pos, token, &next)) return std::string::npos;
        size_t value = next;
        SkipWhitespace(json, value);
        if (value < json.size() && json[value] == ':' && token == wanted) {
            ++value;
            SkipWhitespace(json, value);
            return value;
        }
        pos = next;
    }
    return std::string::npos;
}

inline bool SkipValue(const std::string& json, size_t& pos, unsigned depth = 0) {
    if (depth > 64) return false;
    SkipWhitespace(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == '"') {
        std::wstring ignored;
        size_t next = 0;
        if (!DecodeString(json, pos, ignored, &next)) return false;
        pos = next;
        return true;
    }
    if (json[pos] == '[') {
        ++pos;
        SkipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') { ++pos; return true; }
        while (pos < json.size()) {
            if (!SkipValue(json, pos, depth + 1)) return false;
            SkipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == ']') { ++pos; return true; }
            if (pos >= json.size() || json[pos] != ',') return false;
            ++pos;
        }
        return false;
    }
    if (json[pos] == '{') {
        ++pos;
        SkipWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') { ++pos; return true; }
        while (pos < json.size()) {
            std::wstring ignored;
            size_t next = 0;
            if (!DecodeString(json, pos, ignored, &next)) return false;
            pos = next;
            SkipWhitespace(json, pos);
            if (pos >= json.size() || json[pos] != ':') return false;
            ++pos;
            if (!SkipValue(json, pos, depth + 1)) return false;
            SkipWhitespace(json, pos);
            if (pos < json.size() && json[pos] == '}') { ++pos; return true; }
            if (pos >= json.size() || json[pos] != ',') return false;
            ++pos;
            SkipWhitespace(json, pos);
        }
        return false;
    }

    const size_t start = pos;
    if (json.compare(pos, 4, "true") == 0 || json.compare(pos, 4, "null") == 0) {
        pos += 4;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        pos += 5;
        return true;
    }
    if (json[pos] == '-') ++pos;
    if (pos >= json.size()) return false;
    if (json[pos] == '0') {
        ++pos;
    } else if (json[pos] >= '1' && json[pos] <= '9') {
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') ++pos;
    } else {
        return false;
    }
    if (pos < json.size() && json[pos] == '.') {
        ++pos;
        const size_t fractionStart = pos;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') ++pos;
        if (pos == fractionStart) return false;
    }
    if (pos < json.size() && (json[pos] == 'e' || json[pos] == 'E')) {
        ++pos;
        if (pos < json.size() && (json[pos] == '+' || json[pos] == '-')) ++pos;
        const size_t exponentStart = pos;
        while (pos < json.size() && json[pos] >= '0' && json[pos] <= '9') ++pos;
        if (pos == exponentStart) return false;
    }
    return pos > start;
}

inline bool IsValidDocument(const std::string& json) {
    size_t pos = 0;
    if (!SkipValue(json, pos)) return false;
    SkipWhitespace(json, pos);
    return pos == json.size();
}

} // namespace json_detail

inline std::wstring DecodeJsonStringAt(const std::string& json, size_t startQuote) {
    std::wstring decoded;
    return json_detail::DecodeString(json, startQuote, decoded) ? decoded : L"";
}

// Extract a JSON string value and fully unescape it. Numeric fields must use a
// numeric extractor; array fields must use ExtractJsonArrayFirstStringDecoded.
inline std::wstring ExtractJsonStringDecoded(const std::string& json, const std::string& key) {
    if (!json_detail::IsValidDocument(json)) return L"";
    const size_t pos = json_detail::FindValueForKey(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '"') return L"";
    return DecodeJsonStringAt(json, pos);
}

// Extract the first string element from a JSON array, structurally skipping
// other valid JSON values instead of scanning blindly for the next comma.
inline std::wstring ExtractJsonArrayFirstStringDecoded(const std::string& json,
                                                       const std::string& key) {
    if (!json_detail::IsValidDocument(json)) return L"";
    size_t pos = json_detail::FindValueForKey(json, key);
    if (pos == std::string::npos || pos >= json.size() || json[pos] != '[') return L"";
    ++pos;
    while (pos < json.size()) {
        json_detail::SkipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] == ']') return L"";
        if (json[pos] == '"') return DecodeJsonStringAt(json, pos);
        if (!json_detail::SkipValue(json, pos)) return L"";
        json_detail::SkipWhitespace(json, pos);
        if (pos >= json.size() || json[pos] == ']') return L"";
        if (json[pos] != ',') return L"";
        ++pos;
    }
    return L"";
}
