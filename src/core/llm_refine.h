#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>

#include <cstdint>
#include <cwctype>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "utils.h"

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

namespace llm {

// Explicit re-export for existing short llm:: utility call sites. Do not add
// same-named functions in this namespace because they would hide these aliases.
using ::WideToUtf8;
using ::Utf8ToWide;
using ::EscapeJson;
using ::Trim;

constexpr wchar_t kSystemPrompt[] =
    L"语音识别纠错助手。修正ASR明显错误，不改写润色。\n"
    L"可修正：明确的同音错字、英文术语大小写、数字规范化、补充标点。\n"
    L"禁止：改写、增删、改变语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

constexpr wchar_t kPresetBasicFix[] =
    L"语音识别纠错助手。修正ASR明显错误，不改写润色。\n"
    L"可修正：明确的同音错字（根据语境）、英文术语大小写、数字规范化、标点。\n"
    L"禁止：改写、增删、改变语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

constexpr wchar_t kPresetDeepFix[] =
    L"语音识别纠错助手。修正ASR错误，不改写润色。\n"
    L"可修正：同音错字（根据语境）、英文术语大小写、数字规范化、标点、语法错误。\n"
    L"禁止：改写、增删、改变语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

constexpr wchar_t kPresetPolish[] =
    L"语音识别纠错助手。修正ASR错误并润色表达。\n"
    L"可修正：同音错字、英文术语大小写、数字、标点，保留中英文混合,并润色语句。\n"
    L"保持原意和语气。无错误则原样输出。\n"
    L"只输出修正后文本。";

struct PromptPreset {
    const wchar_t* name;
    const wchar_t* prompt;
    const wchar_t* description;
};

constexpr PromptPreset kPromptPresets[] = {
    {L"Basic Fix",  kPresetBasicFix,  L"Fix homophones, terms, numbers, and add missing punctuation"},
    {L"Deep Fix",   kPresetDeepFix,   L"Fix typos, terms, grammar, punctuation, and normalize numbers"},
    {L"Polish",     kPresetPolish,    L"Fix errors and polish expression while preserving original meaning"},
};
constexpr int kPromptPresetCount = sizeof(kPromptPresets) / sizeof(kPromptPresets[0]);

struct ProviderPreset {
    const wchar_t* name;
    const wchar_t* url;
    const wchar_t* defaultModel;
    const wchar_t* extraParams;
};

constexpr ProviderPreset kProviderPresets[] = {
    {L"DeepSeek",    L"https://api.deepseek.com",      L"deepseek-v4-flash",           L"\"thinking\":{\"type\":\"disabled\"}"},
    {L"OpenRouter",  L"https://openrouter.ai/api/v1",  L"qwen/qwen3.5-9b",            L"\"reasoning\":{\"effort\":\"none\"}"},
    {L"SiliconFlow", L"https://api.siliconflow.cn/v1", L"Qwen/Qwen3.6-35B-A3B",       L"\"enable_thinking\":false"},
};
constexpr int kProviderPresetCount = sizeof(kProviderPresets) / sizeof(kProviderPresets[0]);

inline std::wstring NormalizeEndpointForComparison(std::wstring endpoint) {
    endpoint = Trim(std::move(endpoint));
    if (!endpoint.empty() && endpoint.find(L"://") == std::wstring::npos) {
        endpoint = L"https://" + endpoint;
    }
    while (endpoint.size() > 1 && endpoint.back() == L'/') endpoint.pop_back();
    for (wchar_t& ch : endpoint) ch = static_cast<wchar_t>(towlower(ch));
    return endpoint;
}

inline bool IsOfficialPresetEndpoint(const std::wstring& provider,
                                     const std::wstring& endpoint) {
    const std::wstring normalized = NormalizeEndpointForComparison(endpoint);
    for (const auto& preset : kProviderPresets) {
        if (provider == preset.name) {
            const std::wstring presetEndpoint = NormalizeEndpointForComparison(preset.url);
            if (normalized == presetEndpoint ||
                normalized == presetEndpoint + L"/chat/completions") {
                return true;
            }
            // Preserve the equivalent OpenAI-compatible /v1 form accepted by
            // the service and commonly found in existing configurations.
            if (provider == L"DeepSeek" &&
                (normalized == presetEndpoint + L"/v1" ||
                 normalized == presetEndpoint + L"/v1/chat/completions")) {
                return true;
            }
            if (provider == L"SiliconFlow" &&
                (normalized == L"https://api.siliconflow.com/v1" ||
                 normalized == L"https://api.siliconflow.com/v1/chat/completions")) {
                return true;
            }
            return false;
        }
    }
    return false;
}

inline bool MigrateLegacyProviderConfig(const std::wstring& provider,
                                        const std::wstring& endpoint,
                                        std::wstring& model,
                                        std::wstring& extraParams) {
    if (!IsOfficialPresetEndpoint(provider, endpoint)) return false;
    bool changed = false;
    if (provider == L"DeepSeek") {
        constexpr wchar_t kThinkingDisabled[] = L"\"thinking\":{\"type\":\"disabled\"}";
        constexpr wchar_t kThinkingEnabled[] = L"\"thinking\":{\"type\":\"enabled\"}";
        if (model == L"deepseek-chat") {
            model = L"deepseek-v4-flash";
            if (extraParams.empty() || extraParams == kThinkingEnabled) {
                extraParams = kThinkingDisabled;
            }
            changed = true;
        } else if (model == L"deepseek-reasoner") {
            model = L"deepseek-v4-flash";
            if (extraParams.empty() || extraParams == kThinkingDisabled) {
                extraParams = kThinkingEnabled;
            }
            changed = true;
        }
    } else if (provider == L"OpenRouter" && model == L"qwen/qwen3-4b") {
        model = L"qwen/qwen3.5-9b";
        changed = true;
    } else if (provider == L"SiliconFlow" && model == L"Qwen/Qwen3.6-35B-A3B") {
        if (extraParams == L"\"chat_template_kwargs\":{\"enable_thinking\":false}") {
            extraParams = L"\"enable_thinking\":false";
            changed = true;
        }
    }
    return changed;
}

inline std::wstring EncryptString(const std::wstring& plain) {
    if (plain.empty()) return L"";
    std::string utf8 = WideToUtf8(plain);
    DATA_BLOB input = { static_cast<DWORD>(utf8.size()), reinterpret_cast<BYTE*>(utf8.data()) };
    DATA_BLOB output = {};
    if (!CryptProtectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return L"";
    }
    const char* base64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::wstring result;
    const BYTE* data = output.pbData;
    size_t len = output.cbData;
    for (size_t i = 0; i < len; i += 3) {
        DWORD n = static_cast<DWORD>(data[i]) << 16;
        if (i + 1 < len) n |= static_cast<DWORD>(data[i + 1]) << 8;
        if (i + 2 < len) n |= static_cast<DWORD>(data[i + 2]);
        result += base64[(n >> 18) & 0x3F];
        result += base64[(n >> 12) & 0x3F];
        result += (i + 1 < len) ? base64[(n >> 6) & 0x3F] : L'=';
        result += (i + 2 < len) ? base64[n & 0x3F] : L'=';
    }
    LocalFree(output.pbData);
    return result;
}

inline std::wstring DecryptString(const std::wstring& enc) {
    if (enc.empty()) return L"";
    auto base64Decode = [](wchar_t c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::vector<BYTE> data;
    for (size_t i = 0; i < enc.size(); i += 4) {
        int a = base64Decode(enc[i]);
        int b = (i + 1 < enc.size()) ? base64Decode(enc[i + 1]) : -1;
        int c2 = (i + 2 < enc.size()) ? base64Decode(enc[i + 2]) : -1;
        int d = (i + 3 < enc.size()) ? base64Decode(enc[i + 3]) : -1;
        if (a < 0) break;
        data.push_back(static_cast<BYTE>((a << 2) | (b >> 4)));
        if (b < 0 || enc[i + 2] == L'=') break;
        data.push_back(static_cast<BYTE>(((b & 0xF) << 4) | (c2 >> 2)));
        if (c2 < 0 || enc[i + 3] == L'=') break;
        data.push_back(static_cast<BYTE>(((c2 & 3) << 6) | d));
    }
    DATA_BLOB input = { static_cast<DWORD>(data.size()), data.data() };
    DATA_BLOB output = {};
    if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output)) {
        return L"";
    }
    std::wstring result = Utf8ToWide(std::string(reinterpret_cast<char*>(output.pbData), output.cbData));
    LocalFree(output.pbData);
    return result;
}

struct RequestConfig {
    std::wstring endpoint;
    std::wstring apiKey;
    std::wstring model;
    std::wstring systemPrompt;
    std::wstring extraParams;
};

struct RequestTimeouts {
    int resolveMs;
    int connectMs;
    int sendMs;
    int receiveMs;
};

constexpr RequestTimeouts kRefineTimeouts{5000, 5000, 5000, 15000};
constexpr RequestTimeouts kConnectionTestTimeouts{5000, 5000, 5000, 15000};
constexpr size_t kMaxResponseBytes = 1024 * 1024;

inline std::string NormalizedExtraParams(const std::wstring& value) {
    std::wstring trimmed = Trim(value);
    if (trimmed.size() >= 2 && trimmed.front() == L'{' && trimmed.back() == L'}') {
        trimmed = Trim(trimmed.substr(1, trimmed.size() - 2));
    }
    return WideToUtf8(trimmed);
}

inline std::string BuildRequestBodyWithLimit(const std::wstring& userMsg,
                                             const RequestConfig& cfg,
                                             unsigned maxTokens) {
    const std::wstring& prompt = cfg.systemPrompt.empty() ? std::wstring(kSystemPrompt) : cfg.systemPrompt;
    std::string body = "{\"model\":\"" + EscapeJson(Trim(cfg.model))
        + "\",\"messages\":[{\"role\":\"system\",\"content\":\"" + EscapeJson(prompt)
        + "\"},{\"role\":\"user\",\"content\":\"" + EscapeJson(userMsg)
        + "\"}],\"max_tokens\":" + std::to_string(maxTokens) + ",\"temperature\":0.1";
    const std::string extraParams = NormalizedExtraParams(cfg.extraParams);
    if (!extraParams.empty()) {
        body += "," + extraParams;
    }
    body += "}";
    return body;
}

inline std::string BuildRequestBody(const std::wstring& userMsg, const RequestConfig& cfg) {
    return BuildRequestBodyWithLimit(userMsg, cfg, 1024);
}

inline std::string BuildTestBody(const RequestConfig& cfg) {
    return BuildRequestBodyWithLimit(L"ping", cfg, 8);
}

inline void SkipJsonWhitespace(const std::string& json, size_t& pos) {
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
}

inline int JsonHexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline void AppendUtf8CodePoint(uint32_t codePoint, std::string& out) {
    if (codePoint <= 0x7F) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

inline bool ParseJsonString(const std::string& json, size_t& pos, std::string* decoded) {
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    while (pos < json.size()) {
        const unsigned char c = static_cast<unsigned char>(json[pos++]);
        if (c == '"') return true;
        if (c < 0x20) return false;
        if (c != '\\') {
            if (decoded) decoded->push_back(static_cast<char>(c));
            continue;
        }
        if (pos >= json.size()) return false;
        const char escape = json[pos++];
        switch (escape) {
        case '"': if (decoded) decoded->push_back('"'); break;
        case '\\': if (decoded) decoded->push_back('\\'); break;
        case '/': if (decoded) decoded->push_back('/'); break;
        case 'b': if (decoded) decoded->push_back('\b'); break;
        case 'f': if (decoded) decoded->push_back('\f'); break;
        case 'n': if (decoded) decoded->push_back('\n'); break;
        case 'r': if (decoded) decoded->push_back('\r'); break;
        case 't': if (decoded) decoded->push_back('\t'); break;
        case 'u': {
            if (pos + 4 > json.size()) return false;
            uint32_t codePoint = 0;
            for (int i = 0; i < 4; ++i) {
                const int value = JsonHexValue(json[pos++]);
                if (value < 0) return false;
                codePoint = (codePoint << 4) | static_cast<uint32_t>(value);
            }
            if (codePoint >= 0xD800 && codePoint <= 0xDBFF) {
                if (pos + 6 > json.size() || json[pos] != '\\' || json[pos + 1] != 'u') return false;
                pos += 2;
                uint32_t low = 0;
                for (int i = 0; i < 4; ++i) {
                    const int value = JsonHexValue(json[pos++]);
                    if (value < 0) return false;
                    low = (low << 4) | static_cast<uint32_t>(value);
                }
                if (low < 0xDC00 || low > 0xDFFF) return false;
                codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
            } else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF) {
                return false;
            }
            if (decoded) AppendUtf8CodePoint(codePoint, *decoded);
            break;
        }
        default:
            return false;
        }
    }
    return false;
}

inline bool SkipJsonValue(const std::string& json, size_t& pos, int depth = 0) {
    if (depth > 64) return false;
    SkipJsonWhitespace(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == '"') return ParseJsonString(json, pos, nullptr);
    if (json[pos] == '{') {
        ++pos;
        SkipJsonWhitespace(json, pos);
        if (pos < json.size() && json[pos] == '}') { ++pos; return true; }
        for (;;) {
            if (!ParseJsonString(json, pos, nullptr)) return false;
            SkipJsonWhitespace(json, pos);
            if (pos >= json.size() || json[pos++] != ':') return false;
            if (!SkipJsonValue(json, pos, depth + 1)) return false;
            SkipJsonWhitespace(json, pos);
            if (pos >= json.size()) return false;
            if (json[pos] == '}') { ++pos; return true; }
            if (json[pos++] != ',') return false;
            SkipJsonWhitespace(json, pos);
        }
    }
    if (json[pos] == '[') {
        ++pos;
        SkipJsonWhitespace(json, pos);
        if (pos < json.size() && json[pos] == ']') { ++pos; return true; }
        for (;;) {
            if (!SkipJsonValue(json, pos, depth + 1)) return false;
            SkipJsonWhitespace(json, pos);
            if (pos >= json.size()) return false;
            if (json[pos] == ']') { ++pos; return true; }
            if (json[pos++] != ',') return false;
        }
    }
    if (json.compare(pos, 4, "true") == 0 || json.compare(pos, 4, "null") == 0) {
        pos += 4;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        pos += 5;
        return true;
    }

    size_t numberPos = pos;
    if (json[numberPos] == '-') ++numberPos;
    if (numberPos >= json.size()) return false;
    if (json[numberPos] == '0') {
        ++numberPos;
    } else if (json[numberPos] >= '1' && json[numberPos] <= '9') {
        while (numberPos < json.size() && json[numberPos] >= '0' && json[numberPos] <= '9') ++numberPos;
    } else {
        return false;
    }
    if (numberPos < json.size() && json[numberPos] == '.') {
        ++numberPos;
        const size_t fractionStart = numberPos;
        while (numberPos < json.size() && json[numberPos] >= '0' && json[numberPos] <= '9') ++numberPos;
        if (numberPos == fractionStart) return false;
    }
    if (numberPos < json.size() && (json[numberPos] == 'e' || json[numberPos] == 'E')) {
        ++numberPos;
        if (numberPos < json.size() && (json[numberPos] == '+' || json[numberPos] == '-')) ++numberPos;
        const size_t exponentStart = numberPos;
        while (numberPos < json.size() && json[numberPos] >= '0' && json[numberPos] <= '9') ++numberPos;
        if (numberPos == exponentStart) return false;
    }
    pos = numberPos;
    return true;
}

inline bool IsValidJson(const std::string& json) {
    size_t pos = 0;
    if (!SkipJsonValue(json, pos)) return false;
    SkipJsonWhitespace(json, pos);
    return pos == json.size();
}

struct JsonObjectMemberSpan {
    std::string name;
    size_t keyStart = 0;
    size_t valueStart = 0;
    size_t valueEnd = 0;
};

inline bool ParseJsonObjectMembers(const std::string& json,
                                   size_t objectPos,
                                   std::vector<JsonObjectMemberSpan>& members,
                                   size_t* objectEnd = nullptr) {
    members.clear();
    size_t pos = objectPos;
    SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '{') return false;
    SkipJsonWhitespace(json, pos);
    if (pos < json.size() && json[pos] == '}') {
        if (objectEnd) *objectEnd = pos + 1;
        return true;
    }

    for (;;) {
        JsonObjectMemberSpan span;
        span.keyStart = pos;
        if (!ParseJsonString(json, pos, &span.name)) return false;
        SkipJsonWhitespace(json, pos);
        if (pos >= json.size() || json[pos++] != ':') return false;
        SkipJsonWhitespace(json, pos);
        span.valueStart = pos;
        if (!SkipJsonValue(json, pos)) return false;
        span.valueEnd = pos;
        members.push_back(std::move(span));

        SkipJsonWhitespace(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == '}') {
            if (objectEnd) *objectEnd = pos + 1;
            return true;
        }
        if (json[pos++] != ',') return false;
        SkipJsonWhitespace(json, pos);
    }
}

inline bool GetJsonObjectMemberRaw(const std::string& json,
                                   const std::string& member,
                                   std::string& rawValue) {
    if (!IsValidJson(json)) return false;
    std::vector<JsonObjectMemberSpan> members;
    if (!ParseJsonObjectMembers(json, 0, members)) return false;
    for (const auto& span : members) {
        if (span.name == member) {
            rawValue = json.substr(span.valueStart, span.valueEnd - span.valueStart);
            return true;
        }
    }
    return false;
}

inline bool GetJsonObjectMemberString(const std::string& json,
                                      const std::string& member,
                                      std::string& value) {
    std::string rawValue;
    if (!GetJsonObjectMemberRaw(json, member, rawValue)) return false;
    size_t pos = 0;
    SkipJsonWhitespace(rawValue, pos);
    std::string decoded;
    if (!ParseJsonString(rawValue, pos, &decoded)) return false;
    SkipJsonWhitespace(rawValue, pos);
    if (pos != rawValue.size()) return false;
    value = std::move(decoded);
    return true;
}

inline bool GetJsonObjectMemberNames(const std::string& json,
                                     std::vector<std::string>& names) {
    names.clear();
    if (!IsValidJson(json)) return false;
    std::vector<JsonObjectMemberSpan> members;
    if (!ParseJsonObjectMembers(json, 0, members)) return false;
    names.reserve(members.size());
    for (const auto& span : members) names.push_back(span.name);
    return true;
}

inline bool SetJsonObjectMemberRaw(std::string& json,
                                   const std::string& member,
                                   const std::string& rawValue) {
    if (!IsValidJson(rawValue)) return false;
    if (json.empty()) json = "{}";
    if (!IsValidJson(json)) return false;

    std::vector<JsonObjectMemberSpan> members;
    size_t objectEnd = 0;
    if (!ParseJsonObjectMembers(json, 0, members, &objectEnd)) return false;
    for (const auto& span : members) {
        if (span.name == member) {
            json.replace(span.valueStart, span.valueEnd - span.valueStart, rawValue);
            return true;
        }
    }

    const std::string encodedName = EscapeJson(Utf8ToWide(member));
    const std::string entry = (members.empty() ? "" : ",") +
        std::string("\"") + encodedName + "\":" + rawValue;
    json.insert(objectEnd - 1, entry);
    return true;
}

inline bool RemoveJsonObjectMember(std::string& json, const std::string& member) {
    if (!IsValidJson(json)) return false;
    std::vector<JsonObjectMemberSpan> members;
    if (!ParseJsonObjectMembers(json, 0, members)) return false;
    for (size_t i = 0; i < members.size(); ++i) {
        if (members[i].name != member) continue;
        size_t eraseStart = members[i].keyStart;
        size_t eraseEnd = members[i].valueEnd;
        if (i + 1 < members.size()) {
            eraseEnd = members[i + 1].keyStart;
        } else if (i > 0) {
            eraseStart = members[i - 1].valueEnd;
        }
        json.erase(eraseStart, eraseEnd - eraseStart);
        return true;
    }
    return false;
}

inline bool FindJsonObjectMember(const std::string& json,
                                 size_t objectPos,
                                 const std::string& member,
                                 size_t& valuePos) {
    size_t pos = objectPos;
    SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '{') return false;
    SkipJsonWhitespace(json, pos);
    if (pos < json.size() && json[pos] == '}') return false;
    for (;;) {
        std::string key;
        if (!ParseJsonString(json, pos, &key)) return false;
        SkipJsonWhitespace(json, pos);
        if (pos >= json.size() || json[pos++] != ':') return false;
        SkipJsonWhitespace(json, pos);
        if (key == member) {
            valuePos = pos;
            return true;
        }
        if (!SkipJsonValue(json, pos)) return false;
        SkipJsonWhitespace(json, pos);
        if (pos >= json.size() || json[pos] == '}') return false;
        if (json[pos++] != ',') return false;
        SkipJsonWhitespace(json, pos);
    }
}

inline bool FirstJsonArrayElement(const std::string& json, size_t arrayPos, size_t& valuePos) {
    size_t pos = arrayPos;
    SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos++] != '[') return false;
    SkipJsonWhitespace(json, pos);
    if (pos >= json.size() || json[pos] == ']') return false;
    valuePos = pos;
    return true;
}

inline bool ParseContentValue(const std::string& json, size_t valuePos, std::string& content) {
    size_t pos = valuePos;
    SkipJsonWhitespace(json, pos);
    if (pos >= json.size()) return false;
    if (json[pos] == '"') return ParseJsonString(json, pos, &content);
    if (json[pos] != '[') return false;

    ++pos;
    SkipJsonWhitespace(json, pos);
    while (pos < json.size() && json[pos] != ']') {
        if (json[pos] == '"') {
            if (!ParseJsonString(json, pos, &content)) return false;
        } else if (json[pos] == '{') {
            size_t textPos = 0;
            if (FindJsonObjectMember(json, pos, "text", textPos)) {
                size_t parsedTextPos = textPos;
                std::string text;
                if (ParseJsonString(json, parsedTextPos, &text)) content += text;
            }
            if (!SkipJsonValue(json, pos)) return false;
        } else if (!SkipJsonValue(json, pos)) {
            return false;
        }
        SkipJsonWhitespace(json, pos);
        if (pos >= json.size()) return false;
        if (json[pos] == ']') break;
        if (json[pos++] != ',') return false;
        SkipJsonWhitespace(json, pos);
    }
    return pos < json.size() && json[pos] == ']';
}

inline std::wstring ParseResponse(const std::string& response) {
    if (!IsValidJson(response)) return L"";
    size_t rootPos = 0;
    SkipJsonWhitespace(response, rootPos);
    size_t choicesPos = 0;
    size_t choicePos = 0;
    size_t messagePos = 0;
    size_t contentPos = 0;
    if (!FindJsonObjectMember(response, rootPos, "choices", choicesPos) ||
        !FirstJsonArrayElement(response, choicesPos, choicePos) ||
        !FindJsonObjectMember(response, choicePos, "message", messagePos) ||
        !FindJsonObjectMember(response, messagePos, "content", contentPos)) {
        return L"";
    }
    std::string content;
    if (!ParseContentValue(response, contentPos, content)) return L"";
    return Trim(Utf8ToWide(content));
}

inline bool ParseEndpoint(const std::wstring& endpoint, std::wstring& host, std::wstring& path, bool& useSsl, INTERNET_PORT& port) {
    std::wstring url = Trim(endpoint);
    if (url.empty()) return false;
    if (url.find(L"://") == std::wstring::npos) url = L"https://" + url;

    URL_COMPONENTS components{};
    components.dwStructSize = sizeof(components);
    components.dwSchemeLength = static_cast<DWORD>(-1);
    components.dwHostNameLength = static_cast<DWORD>(-1);
    components.dwUserNameLength = static_cast<DWORD>(-1);
    components.dwPasswordLength = static_cast<DWORD>(-1);
    components.dwUrlPathLength = static_cast<DWORD>(-1);
    components.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &components)) return false;
    if (components.nScheme != INTERNET_SCHEME_HTTP &&
        components.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }
    if (components.dwUserNameLength != 0 || components.dwPasswordLength != 0 ||
        components.dwExtraInfoLength != 0 || components.dwHostNameLength == 0) {
        return false;
    }

    host.assign(components.lpszHostName, components.dwHostNameLength);
    path.clear();
    if (components.dwUrlPathLength != 0) {
        path.assign(components.lpszUrlPath, components.dwUrlPathLength);
    }
    useSsl = components.nScheme == INTERNET_SCHEME_HTTPS;
    port = components.nPort != 0
        ? components.nPort
        : (useSsl ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT);

    while (path.size() > 1 && path.back() == L'/') path.pop_back();
    constexpr wchar_t kChatCompletionsPath[] = L"/chat/completions";
    if (path.empty() || path == L"/") {
        path = kChatCompletionsPath;
    } else if (path.size() < std::size(kChatCompletionsPath) - 1 ||
               path.compare(path.size() - (std::size(kChatCompletionsPath) - 1),
                            std::size(kChatCompletionsPath) - 1,
                            kChatCompletionsPath) != 0) {
        path += kChatCompletionsPath;
    }
    return true;
}

struct RequestResult {
    bool success = false;
    DWORD statusCode = 0;
    std::wstring error;
    std::wstring responseText;
};

inline RequestResult SendRequestRaw(const RequestConfig& cfg,
                                    const std::string& body,
                                    const RequestTimeouts& timeouts = kRefineTimeouts) {
    RequestResult res;
    if (!IsValidJson(body)) {
        res.error = L"Invalid request JSON. Check Extra Params.";
        return res;
    }
    const std::wstring endpoint = Trim(cfg.endpoint);
    const std::wstring apiKey = Trim(cfg.apiKey);
    const std::wstring model = Trim(cfg.model);
    if (endpoint.empty() || apiKey.empty() || model.empty()) {
        res.error = L"Endpoint, API key, and model are required.";
        return res;
    }
    std::wstring host, path;
    bool useSsl = true;
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    if (!ParseEndpoint(endpoint, host, path, useSsl, port)) {
        res.error = L"Invalid endpoint URL";
        return res;
    }

    HINTERNET hSession = WinHttpOpen(L"VoxType/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { res.error = L"WinHttpOpen failed"; return res; }
    WinHttpSetTimeouts(hSession, timeouts.resolveMs, timeouts.connectMs,
                      timeouts.sendMs, timeouts.receiveMs);

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), port, 0);
    if (!hConnect) {
        res.error = L"WinHttpConnect failed to " + host + L":" + std::to_wstring(port);
        WinHttpCloseHandle(hSession); return res;
    }

    DWORD flags = useSsl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!hRequest) {
        res.error = L"WinHttpOpenRequest failed";
        WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return res;
    }

    WinHttpSetTimeouts(hRequest, timeouts.resolveMs, timeouts.connectMs,
                      timeouts.sendMs, timeouts.receiveMs);

    std::wstring headers = L"Content-Type: application/json\r\nAccept: application/json\r\nAuthorization: Bearer " + apiKey + L"\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(-1),
        const_cast<char*>(body.c_str()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (!sent) {
        res.error = L"WinHttpSendRequest failed (error " + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return res;
    }

    BOOL received = WinHttpReceiveResponse(hRequest, nullptr);
    if (!received) {
        res.error = L"WinHttpReceiveResponse failed (error " + std::to_wstring(GetLastError()) + L")";
        WinHttpCloseHandle(hRequest); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return res;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize, WINHTTP_NO_HEADER_INDEX);
    res.statusCode = statusCode;

    std::string responseBody;
    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            res.error = L"WinHttpQueryDataAvailable failed (error " + std::to_wstring(GetLastError()) + L")";
            break;
        }
        if (bytesAvailable == 0) break;
        if (responseBody.size() + bytesAvailable > kMaxResponseBytes) {
            res.error = L"LLM response exceeded 1 MB";
            break;
        }
        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
            res.error = L"WinHttpReadData failed (error " + std::to_wstring(GetLastError()) + L")";
            break;
        }
        responseBody += chunk;
        responseBody.resize(responseBody.size() - (bytesAvailable - bytesRead));
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    res.responseText = Utf8ToWide(responseBody);
    if (!res.error.empty()) {
        return res;
    }
    if (statusCode == 200) {
        res.responseText = ParseResponse(responseBody);
        if (!res.responseText.empty()) {
            res.success = true;
        } else {
            res.error = L"HTTP 200 response did not contain assistant text";
        }
    } else {
        res.error = L"HTTP " + std::to_wstring(statusCode) + L": " + res.responseText.substr(0, 200);
    }
    return res;
}

inline std::wstring Refine(const std::wstring& asrText, const RequestConfig& cfg) {
    std::string body = BuildRequestBody(asrText, cfg);
    RequestResult res = SendRequestRaw(cfg, body, kRefineTimeouts);
    return (res.success && !res.responseText.empty()) ? res.responseText : asrText;
}

struct TestResult {
    bool ok = false;
    DWORD elapsed = 0;
    std::wstring message;
};

inline TestResult TestConnection(const RequestConfig& cfg) {
    TestResult result;
    ULONGLONG startTime = GetTickCount64();

    std::string body = BuildTestBody(cfg);
    RequestResult res = SendRequestRaw(cfg, body, kConnectionTestTimeouts);
    result.elapsed = static_cast<DWORD>(GetTickCount64() - startTime);

    if (res.success) {
        result.ok = true;
        result.message = L"Connection OK (" + std::to_wstring(result.elapsed) + L" ms)";
    } else {
        result.message = res.error;
    }
    return result;
}

} // namespace llm
