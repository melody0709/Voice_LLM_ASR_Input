#pragma once

#include <string>
#include <windows.h>

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    return result;
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    return result;
}

inline std::string EscapeJson(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::string out;
    out.reserve(utf8.size() + 8);
    for (char c : utf8) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"':  out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:   out += c; break;
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

inline std::wstring ExtractJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos += search.size();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return L"";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size() || json[pos] != '"') return L"";
    pos++;
    size_t start = pos;
    while (pos < json.size()) {
        if (json[pos] == '"') {
            size_t bs = 0;
            size_t k = pos;
            while (k > start && json[k - 1] == '\\') { bs++; k--; }
            if (bs % 2 == 0) break;
        }
        if (json[pos] == '\\' && pos + 1 < json.size()) pos++;
        pos++;
    }
    return Utf8ToWide(json.substr(start, pos - start));
}