#include "qwen_free_proto_llm.h"

#include "asr_runtime_log.h"
#include "cloud_http_common.h"
#include "globals.h"
#include "qwen_free_diagnostics.h"
#include "qwen_free_json.h"
#include "qwen_free_llm_json.h"
#include "qwen_free_proto_unet.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <limits>
#include <random>
#include <sstream>

namespace qwen_free_proto_llm {

namespace {

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    s.resize(static_cast<size_t>(len - 1));
    return s;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    w.resize(static_cast<size_t>(len - 1));
    return w;
}

// JSON 字符串转义（仅 UTF-8 字符串内的特殊字符）。
std::string JsonEscapeStr(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"':  out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x",
                                  static_cast<unsigned int>(
                                      static_cast<unsigned char>(c)));
                    out += buf;
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

// 简单 JSON 字符串字段提取（与 qwen_free_proto_asr.cpp 一致）。
std::string ExtractJsonString(const std::string& json, const std::string& key) {
    return qwen_free_json::ExtractString(json, key);
}

// The command endpoint has returned both ordinary nested JSON and JSON
// envelopes whose `data`/`result` field is itself an escaped JSON string.
// Search those bounded envelopes without accepting arbitrary response text
// as a successful result.
std::string ExtractLlmString(const std::string& json,
                             const std::string& key,
                             int depth = 0) {
    std::string value = qwen_free_json::ExtractStringAtPath(
        json, {key.c_str()});
    if (value.empty()) value = ExtractJsonString(json, key);
    if (!value.empty() || depth >= 3) return value;

    static constexpr const char* kContainers[] = {
        "data", "result", "response", "body", "payload", "message"
    };
    for (const char* container : kContainers) {
        const std::string embedded = ExtractJsonString(json, container);
        if (embedded.empty() || embedded == json) continue;
        const size_t firstPos = embedded.find_first_not_of(" \t\r\n");
        if (firstPos == std::string::npos) continue;
        const std::string trimmed = embedded.substr(firstPos);
        const char first = trimmed.front();
        if (first != '{' && first != '[' && first != 'e' && first != 'd') {
            continue;
        }
        value = ExtractLlmString(trimmed, key, depth + 1);
        if (!value.empty()) return value;
    }
    return {};
}

// 生成 UUID 格式字符串（32 hex 无分隔，简化）。
std::string GenerateUuid() {
    std::random_device rd;
    uint64_t r0 = (static_cast<uint64_t>(rd()) << 32) | rd();
    uint64_t r1 = (static_cast<uint64_t>(rd()) << 32) | rd();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x%08x%08x%08x",
                  static_cast<uint32_t>(r0 >> 32), static_cast<uint32_t>(r0),
                  static_cast<uint32_t>(r1 >> 32), static_cast<uint32_t>(r1));
    return std::string(buf, 32);
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// unet.dll 返回原始密文的 hex 展开；HTTP body/header 使用原版的标准 Base64。
std::string HexToBase64(const std::string& hex) {
    if (hex.empty() || (hex.size() & 1u) != 0) return {};

    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        const int hi = HexValue(hex[i]);
        const int lo = HexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }

    static constexpr char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = (i + 1 < bytes.size())
            ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned b2 = (i + 2 < bytes.size())
            ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out.push_back(kBase64[(b0 >> 2) & 0x3f]);
        out.push_back(kBase64[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(i + 1 < bytes.size()
            ? kBase64[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
        out.push_back(i + 2 < bytes.size()
            ? kBase64[b2 & 0x3f] : '=');
    }
    return out;
}

std::string IntentStr(Intent intent) {
    switch (intent) {
        case Intent::VoiceInputWrite:   return "VoiceInputWrite";
        case Intent::VoiceInputAsk:     return "VoiceInputAsk";
        case Intent::VoiceInputRewrite: return "VoiceInputRewrite";
    }
    return "VoiceInputWrite";
}

void LogErr(const std::wstring& msg) {
    std::wstring line = L"[qwen_free_llm] " + msg;
    std::string utf8 = WideToUtf8(line);
    asr_runtime_log::Write("%s", utf8.c_str());
}

} // namespace

LlmResult Execute(const LlmConfig& cfg,
                   const std::wstring& asrText,
                   Intent intent) {
    LlmResult r;
    auto t0 = std::chrono::steady_clock::now();

    if (cfg.utdid.empty() || cfg.utdid.size() != 24) {
        r.error = L"invalid utdid (expect 24 chars)";
        return r;
    }

    // 1. 生成原版 LLM 使用的 voice encrypted kps。
    //    ASR 已经在同一进程初始化过 unet.dll；这里复用同一个 WSG FFI，
    //    避免继续发送空的 x-u-kps-wg / common_params.vekp。
    std::string encryptedKps;
    bool nativeWsgReady = false;
    std::wstring unetError;
    if (!qwen_free_proto_unet::Initialize(cfg.shellPath, unetError)) {
        r.error = L"native Qwen signer unavailable: " + unetError;
        return r;
    }
    if (qwen_free_proto_unet::IsReady()) {
        const auto encrypted = qwen_free_proto_unet::EncryptWithNumber(
            qwen_free_proto_unet::kWsgNumberLlm, cfg.utdid);
        if (encrypted.ok) {
            encryptedKps = HexToBase64(encrypted.sign);
        } else {
            LogErr(L"LLM kps encryption failed: " + encrypted.error);
        }
    }

    // 2. 构造请求体（PROTOCOL.md §4.1）。
    const bool isRewrite = intent == Intent::VoiceInputRewrite;
    const std::wstring& rewriteInstruction = cfg.rewriteInstruction;
    const std::wstring& rewriteSelection = cfg.rewriteSelectionText;
    std::string asrUtf8 = WideToUtf8(asrText);
    if (isRewrite && !rewriteInstruction.empty()) {
        // The ASR result is the spoken command.  The selected text is sent as
        // explicit rewrite context below; putting the selection in
        // asr_original_text makes the server treat it as dictated text.
        asrUtf8 = WideToUtf8(rewriteInstruction);
    }
    std::string reqId = GenerateUuid();
    std::string llmSessionId = GenerateUuid();
    long long nowMs = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    long long recordingDurationMs = 0;
    if (std::isfinite(cfg.recordingMs) && cfg.recordingMs > 0.0) {
        // Converting LLONG_MAX to double rounds to 2^63 on MSVC/IEEE-754.
        // A direct min() followed by static_cast<long long> would therefore
        // still attempt an out-of-range floating-to-integral conversion for
        // very large input.  Handle the saturated case before converting;
        // every finite double below 2^63 is representable inside long long's
        // range (the largest such value is 2^63 - 1024).
        const double maxAsDouble =
            static_cast<double>((std::numeric_limits<long long>::max)());
        if (cfg.recordingMs >= maxAsDouble) {
            recordingDurationMs = (std::numeric_limits<long long>::max)();
        } else {
            recordingDurationMs = static_cast<long long>(cfg.recordingMs);
        }
    }

    std::ostringstream body;
    body << "{"
         << "\"asr_original_text\":\"" << JsonEscapeStr(asrUtf8) << "\","
         << "\"intent\":\"" << IntentStr(intent) << "\","
         << "\"scene\":\"voice_input_assistant\","
         << "\"source\":\"shell_embedded_voice_usage\","
         << "\"trigger_type\":\"" << JsonEscapeStr(cfg.triggerType) << "\"," 
         << "\"req_id\":\"" << reqId << "\"," 
         << "\"llm_session_id\":\"" << llmSessionId << "\"," 
         << "\"recording_duration_ms\":"
         << recordingDurationMs << ","
         << "\"received_at_ms\":" << nowMs << ","
         << "\"timestamp_ms\":" << nowMs << ","
         << "\"retry_available\":false,"
         << "\"abnormal_reason\":\"none\",";
    if (isRewrite) {
        const std::string selectedUtf8 = WideToUtf8(rewriteSelection);
        const std::string instructionUtf8 = WideToUtf8(rewriteInstruction);
        body << "\"rewrite_query\":\"" << JsonEscapeStr(instructionUtf8) << "\","
             << "\"meta_data\":{"
             << "\"intent_content\":\"" << JsonEscapeStr(selectedUtf8) << "\","
             << "\"rewrite_query\":\"" << JsonEscapeStr(instructionUtf8) << "\"},";
    }
    body << "\"common_params\":{" 
         << "\"utdid\":\"" << JsonEscapeStr(cfg.utdid) << "\"," 
         << "\"appkey\":\"" << JsonEscapeStr(cfg.appkey) << "\"," 
         << "\"vekp\":\"" << JsonEscapeStr(encryptedKps) << "\""
         << "}"
         << "}";
    std::string bodyStr = body.str();

    // 3. 构造签名。当前安装版 shell_ffi 使用 0x4ea3 voice WSG。
    //    没有经过指纹验证的 native signer 时直接失败，避免发送已知
    //    无效的通用 HMAC/空鉴权请求。
    std::string signWg;
    if (qwen_free_proto_unet::IsReady() && !encryptedKps.empty()) {
        const auto nativeSign = qwen_free_proto_unet::SignWithNumber(
            qwen_free_proto_unet::kWsgNumberLlm, bodyStr);
        if (nativeSign.ok) {
            signWg = nativeSign.sign;
            nativeWsgReady = true;
        } else {
            LogErr(L"LLM WSG signing failed: " + nativeSign.error);
        }
    }
    if (encryptedKps.empty() || signWg.empty()) {
        r.error = L"native Qwen signer failed to produce authentication fields";
        return r;
    }

    // 4. 构造 HTTP 头。
    std::wostringstream hdr;
    hdr << L"Content-Type: application/json\r\n"
        << L"Origin: https://www.qianwen.com\r\n"
        << L"User-Agent: " << Utf8ToWide(cfg.userAgent) << L"\r\n"
        << L"x-audid-appkey: " << Utf8ToWide(cfg.appkey) << L"\r\n"
        << L"x-audid-appname: " << Utf8ToWide(cfg.appname) << L"\r\n"
        << L"x-audid-sdk: " << Utf8ToWide(cfg.sdk) << L"\r\n"
        << L"x-audid-utdid: " << Utf8ToWide(cfg.utdid) << L"\r\n"
        << L"x-u-kps-wg: " << Utf8ToWide(encryptedKps) << L"\r\n"
        << L"x-u-sign-wsg: " << Utf8ToWide(signWg) << L"\r\n"
        // The native voice-command client uses the unprefixed WSG names in
        // its request header map. Keep the x-u-* compatibility aliases above
        // for older gateways, but send the original names as well.
        << L"kps_wsg: " << Utf8ToWide(encryptedKps) << L"\r\n"
        << L"kps_wg: " << Utf8ToWide(encryptedKps) << L"\r\n"
        << L"sign_wsg: " << Utf8ToWide(signWg) << L"\r\n"
        << L"sign_wg: " << Utf8ToWide(signWg) << L"\r\n"
        << L"vcode: " << nowMs << L"\r\n";

    // 5. 发送 HTTP 请求。
    CloudHttpRequest req;
    req.method = L"POST";
    req.host = cfg.host;
    req.port = kLlmPort;
    req.path = cfg.path;
    req.headers = hdr.str();
    req.body.assign(bodyStr.begin(), bodyStr.end());
    req.useSsl = true;
    req.timeoutMs = cfg.timeoutMs;

    LogErr(L"sending POST body_len=" + std::to_wstring(bodyStr.size()) +
           L" kps_len=" + std::to_wstring(encryptedKps.size()) +
           L" sign_len=" + std::to_wstring(signWg.size()) +
           L" native_wsg=" + std::to_wstring(nativeWsgReady ? 1 : 0));

    CloudHttpResponse resp = SendCloudHttpRequest(req);
    r.httpStatus = resp.statusCode;
    r.rawResponse = resp.body;
    r.elapsedMs = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count());

    if (!resp.ok) {
        r.error = L"HTTP failed: " + resp.failedStep +
                  L" (err=" + std::to_wstring(resp.winhttpError) +
                  L", status=" + std::to_wstring(resp.statusCode) + L")";
        LogErr(r.error);
        return r;
    }
    if (resp.statusCode != 200) {
        r.error = L"HTTP " + std::to_wstring(resp.statusCode) +
                  L": " + Utf8ToWide(
                      qwen_free_diagnostics::SummarizeHttpBody(resp.body));
        LogErr(r.error);
        return r;
    }

    LogErr(Utf8ToWide("LLM response shape: " +
                      qwen_free_diagnostics::SummarizeOutputShape(resp.body)));
    LogErr(Utf8ToWide("LLM response summary: " +
                      qwen_free_diagnostics::SummarizeHttpBody(resp.body)));

    // 6. 解析响应。
    const std::string businessCode =
        qwen_free_diagnostics::ExtractBusinessCode(resp.body);
    if (qwen_free_diagnostics::IsAuthenticationFailure(resp.body)) {
        r.error = L"LLM authentication failed";
        if (qwen_free_diagnostics::IsSafeStructuredField(businessCode)) {
            r.error += L" (code=" + Utf8ToWide(businessCode) + L")";
        }
        LogErr(r.error);
        return r;
    }
    if (qwen_free_diagnostics::HasExplicitErrorEnvelope(resp.body)) {
        const std::string errorText =
            qwen_free_diagnostics::ExtractStructuredErrorText(resp.body);
        r.error = L"LLM business error";
        if (qwen_free_diagnostics::IsSafeStructuredField(businessCode)) {
            r.error += L" (code=" + Utf8ToWide(businessCode) + L")";
        }
        if (!errorText.empty()) {
            r.error += L": " + Utf8ToWide(errorText);
        }
        LogErr(r.error);
        return r;
    }
    std::string status = ExtractLlmString(resp.body, "status");
    if (status.empty()) {
        status = qwen_free_json::ExtractNumberText(resp.body, "status");
    }
    if (status.empty() && qwen_free_diagnostics::IsSuccessfulEnvelope(resp.body)) {
        status = "success";
    }
    status = qwen_free_diagnostics::LowerAscii(status);
    const std::string terminalRewriteContent = isRewrite
        ? qwen_free_llm_json::ExtractTerminalContent(resp.body)
        : std::string();
    // Rewrite responses observed in the original client can carry a
    // completed message inside an envelope whose first visible status is
    // "complete", while ordinary VoiceInputWrite responses use "success".
    const bool successfulStatus =
        qwen_free_diagnostics::IsSuccessfulStatus(status);
    const bool terminalRewriteReady =
        isRewrite && !terminalRewriteContent.empty();
    if (!successfulStatus && !terminalRewriteReady) {
        r.error = status.empty() ||
                  !qwen_free_diagnostics::IsSafeStructuredField(status)
            ? L"LLM response missing successful status"
            : L"LLM status=" + Utf8ToWide(status);
        LogErr(r.error);
        return r;
    }
    {
        std::string polished;
        if (isRewrite) {
            // A rewrite response may contain a terminal message beside a
            // processing/loading message. Only terminal content is safe to
            // replace the user's selected text with.
            polished = terminalRewriteContent;
        }
        if (polished.empty()) polished = ExtractLlmString(resp.body, "polished_text");
        if (polished.empty()) polished = ExtractLlmString(resp.body, "polishedText");
        if (polished.empty() && !isRewrite) polished = ExtractLlmString(resp.body, "content");
        if (polished.empty()) polished = ExtractLlmString(resp.body, "output_text");
        if (polished.empty()) polished = ExtractLlmString(resp.body, "outputText");
        if (polished.empty()) polished = ExtractLlmString(resp.body, "answer");
        if (polished.empty()) polished = ExtractLlmString(resp.body, "text");
        std::string rewrite = ExtractLlmString(resp.body, "rewrite_query");
        std::string orig = ExtractLlmString(resp.body, "asr_original_text");
        std::string sess = ExtractLlmString(resp.body, "llm_session_id");
        if (sess.empty()) sess = ExtractLlmString(resp.body, "llmSessionId");
        std::string intentEcho = ExtractLlmString(resp.body, "intent");

        r.polishedText = Utf8ToWide(polished);
        r.rewriteQuery = Utf8ToWide(rewrite);
        r.asrOriginalText = Utf8ToWide(orig);
        r.sessionId = Utf8ToWide(sess);
        r.intent = Utf8ToWide(intentEcho);
        // For VoiceInputRewrite, rewrite_query is request/metadata echo in
        // the observed envelope, not proof that the server returned a new
        // text.  Never treat that echo as replacement content.
        const bool hasOutput = !polished.empty();
        if (!hasOutput) {
            r.error = L"LLM response missing output text";
            LogErr(r.error);
        } else {
            r.ok = true;
        }
    }
    return r;
}

LlmResult PolishText(const LlmConfig& cfg,
                       const std::wstring& asrText,
                       double recordingMs,
                       size_t capturedPcmBytes) {
    LlmConfig requestCfg = cfg;
    requestCfg.recordingMs = recordingMs;
    requestCfg.capturedPcmBytes = capturedPcmBytes;
    return Execute(requestCfg, asrText, Intent::VoiceInputWrite);
}

LlmResult RewriteSelection(const LlmConfig& cfg,
                             const std::wstring& selectedText,
                             const std::wstring& instruction) {
    LlmConfig requestCfg = cfg;
    requestCfg.triggerType = "selection_detected";
    requestCfg.rewriteSelectionText = selectedText;
    requestCfg.rewriteInstruction = instruction;
    return Execute(requestCfg, instruction, Intent::VoiceInputRewrite);
}

} // namespace qwen_free_proto_llm
