#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <cstddef>
#include <string>

// qwen_free_proto_llm
//
// 千问 IME 免费后端协议层 - LLM 后处理 HTTP（A1 纯协议还原）。
//
// 逆向来源：reverse/work/PROTOCOL.md §4 + reverse/work/ghidra/output/shell_ffi/
//   - 端点：POST https://voice-input.qianwen.com/api/voice/command/execute
//   - 鉴权：HTTP 头 x-audid-appkey / x-audid-appname / x-audid-sdk /
//           x-audid-utdid / x-u-kps-wg / x-u-sign-wsg
//   - 签名：x-u-sign-wsg 由指纹验证后的 unet.dll 原生 signer 生成；
//           provider 专用签名材料不进入应用源码。
//   - Body：JSON，含 asr_original_text / intent / scene / req_id 等
//
// 调用方：VoxType 在拿到 ASR final text 后调用此模块做润色/标点/纠错。

namespace qwen_free_proto_llm {

constexpr const wchar_t* kDefaultLlmHost = L"voice-input.qianwen.com";
constexpr const wchar_t* kDefaultLlmPath = L"/api/voice/command/execute";
constexpr INTERNET_PORT kLlmPort = INTERNET_DEFAULT_HTTPS_PORT;
constexpr const char*    kDefaultAppkey    = "qianwen_voice_app_pc";
constexpr const char*    kDefaultAppname   = "qianwen-ime-windows";
constexpr const char*    kDefaultSdk       = "1.0.0";
constexpr const char*    kDefaultUserAgent =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/605.1.15 (KHTML, like Gecko) "
    "Version/17.0 Safari/605.1.15 TONGYI_DESKTOP/0.1.0 QuarkPC/standalone";

// Intent 类型（PROTOCOL.md §4.3）。
enum class Intent {
    VoiceInputWrite,   // 长按录音松开 → polishedText
    VoiceInputAsk,     // 选中文本 + 双击热键 → 改写/提问响应
    VoiceInputRewrite, // 选中文本 + 改写动作 → rewrite_query
};

struct LlmConfig {
    std::string utdid;                              // 设备指纹（必填）
    std::string appkey = kDefaultAppkey;            // LLM appkey
    std::string appname = kDefaultAppname;
    std::string sdk = kDefaultSdk;
    std::string userAgent = kDefaultUserAgent;
    std::wstring host = kDefaultLlmHost;
    std::wstring path = kDefaultLlmPath;
    std::wstring shellPath;                         // 千问 IME 安装目录（用于 WSG FFI）
    std::string triggerType = "long_press";        // long_press / short_press
    // VoiceInputRewrite carries the spoken instruction separately from the
    // text that was selected in the target application.
    std::wstring rewriteSelectionText;
    std::wstring rewriteInstruction;
    double recordingMs = 0.0;                       // 原版 recording_duration_ms
    size_t capturedPcmBytes = 0;                    // 诊断元数据
    DWORD timeoutMs = 12000;
};

// LLM 后处理结果。
struct LlmResult {
    bool ok = false;
    std::wstring polishedText;       // 润色后的最终文本
    std::wstring asrOriginalText;     // 原始 ASR 文本（回声）
    std::wstring rewriteQuery;        // 改写指令（VoiceInputRewrite）
    std::wstring sessionId;
    std::wstring intent;
    std::string  rawResponse;         // 完整响应 JSON（调试用）
    std::wstring error;
    DWORD httpStatus = 0;
    DWORD elapsedMs = 0;
};

// 执行 LLM 后处理（润色/标点/纠错）。
// asrText 为 ASR 最终文本；intent 决定调用哪种后处理。
LlmResult Execute(const LlmConfig& cfg,
                   const std::wstring& asrText,
                   Intent intent);

// 显式重新润色（VoiceInputWrite intent）。
// recordingMs / capturedPcmBytes 用于元数据字段。
LlmResult PolishText(const LlmConfig& cfg,
                       const std::wstring& asrText,
                       double recordingMs,
                       size_t capturedPcmBytes);

// 选中文本改写（VoiceInputRewrite intent）。
LlmResult RewriteSelection(const LlmConfig& cfg,
                             const std::wstring& selectedText,
                             const std::wstring& instruction);

} // namespace qwen_free_proto_llm
