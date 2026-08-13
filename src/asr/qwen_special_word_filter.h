#pragma once

#include "utils.h"

#include <algorithm>
#include <string>
#include <vector>

namespace qwen_special_word_filter {

// Settings stores the two lists as newline-delimited text so users do not
// have to edit provider JSON.  A line is one word; surrounding whitespace is
// ignored, while whitespace inside a word remains significant.
struct Config {
    std::vector<std::wstring> replaceWords;
    std::vector<std::wstring> emptyWords;
    bool systemReservedFilter = false;
};

inline std::vector<std::wstring> ParseLines(const std::wstring& raw) {
    std::vector<std::wstring> result;
    size_t start = 0;
    while (start <= raw.size()) {
        const size_t end = raw.find_first_of(L"\r\n", start);
        const std::wstring item = Trim(raw.substr(
            start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!item.empty() &&
            std::find(result.begin(), result.end(), item) == result.end()) {
            result.push_back(item);
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
        while (start < raw.size() &&
               (raw[start] == L'\r' || raw[start] == L'\n')) {
            ++start;
        }
    }
    return result;
}

inline std::wstring JoinLines(const std::vector<std::wstring>& words) {
    std::wstring result;
    for (const auto& word : words) {
        if (word.empty()) continue;
        if (!result.empty()) result.push_back(L'\n');
        result += word;
    }
    return result;
}

inline bool Normalize(const std::wstring& replaceRaw,
                      const std::wstring& emptyRaw,
                      bool systemReservedFilter,
                      Config& out,
                      std::wstring* error = nullptr) {
    out.replaceWords = ParseLines(replaceRaw);
    out.emptyWords = ParseLines(emptyRaw);
    out.systemReservedFilter = systemReservedFilter;

    if (out.replaceWords.size() + out.emptyWords.size() > 32) {
        if (error) *error = L"special word filter supports at most 32 words in total.";
        return false;
    }
    for (const auto& word : out.replaceWords) {
        if (std::find(out.emptyWords.begin(), out.emptyWords.end(), word) !=
            out.emptyWords.end()) {
            if (error) *error = L"A special word cannot be in both replace and delete lists.";
            return false;
        }
    }
    return true;
}

inline bool HasAny(const Config& config) {
    return config.systemReservedFilter || !config.replaceWords.empty() ||
           !config.emptyWords.empty();
}

inline std::string BuildJson(const Config& config) {
    std::string json = "{\"filter_with_signed\":{\"word_list\":[";
    for (size_t i = 0; i < config.replaceWords.size(); ++i) {
        if (i) json.push_back(',');
        json += "\"" + EscapeJson(config.replaceWords[i]) + "\"";
    }
    json += "]},\"filter_with_empty\":{\"word_list\":[";
    for (size_t i = 0; i < config.emptyWords.size(); ++i) {
        if (i) json.push_back(',');
        json += "\"" + EscapeJson(config.emptyWords[i]) + "\"";
    }
    json += "]},\"system_reserved_filter\":";
    json += config.systemReservedFilter ? "true}" : "false}";
    return json;
}

} // namespace qwen_special_word_filter
