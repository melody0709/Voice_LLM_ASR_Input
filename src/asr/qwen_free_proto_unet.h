#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

// qwen_free_proto_unet
//
// 千问 IME 免费后端协议层 - unet.dll FFI（A2 混合策略）。
//
// Loads a fingerprint-approved unet.dll, registers its native function table,
// and calls the verified version-dependent signer/encryptor implementation.
//
// 逆向来源：
//   - reverse/work/probe_unet_cryptor.cpp：探针验证 unet.dll 独立加载可行。
//   - reverse/work/ghidra/output/unet/core_sign_function_0x44bb30.c：反编译。
//   - reverse/work/probe_unet_cryptor.log：9 个 number 全部成功返回 44 字节 ASCII。
//
// 签名格式：44 字符 = 4 字符 hex(number) + 40 字符 hex(HMAC-SHA1 digest)。
//   真实样本（memscan_final_out.txt @ 0x600000272a9）：
//   sign=<redacted signature sample>
//        ^^^^                                  ^^^^^^
//        number=0x4ea3                          40 hex digest
//
// The internal RVAs are version-dependent. Unknown SHA-256 fingerprints are
// rejected before LoadLibrary/RVA dispatch; SEH is only a final isolation
// layer for the approved build.

#include <cstdint>
#include <string>

namespace qwen_free_proto_unet {

// WSG number 值（决定 dispatch table 中的 cryptor 实例和 HMAC key）。
// 真实 ASR URL 样本 sign=4ea3... → number=0x4ea3 (20131)。
// LLM HTTP 头 x-u-sign-wsg 使用与语音请求一致的 0x4ea3（已按服务端
// 100000031 鉴权失败样本修正；0x4ea2 仅是旧的静态表推断）。
constexpr int kWsgNumberAsr = 0x4ea3;  // 20131
// The installed Qianwen signer emits the accepted voice WSG family with the
// 0x4ea3 prefix.  0x4ea2 was selected from an unverified static table entry;
// the LLM endpoint rejected those requests with business code 100000031.
constexpr int kWsgNumberLlm = 0x4ea3;  // 20131

struct SignResult {
    bool ok = false;
    std::string sign;       // 44 字符 ASCII（4hex number + 40hex digest）
    std::wstring error;
};

// 初始化：加载 unet.dll + unet_native_bind 注册函数表。
// installDir: 千问 IME 安装目录（含 unet.dll，通常 C:\Program Files\QianwenIME）。
// 幂等：重复调用安全。
// 成功后 SignWithNumber 可用。
bool Initialize(const std::wstring& installDir, std::wstring& out_error);

// 用 unet.dll 生成 WSG 签名。
// number: wsg_number（当前 ASR/LLM 均使用 0x4ea3）。
// content: 签名输入串（URL query 参数按出现顺序拼接）。
// 返回 44 字符 ASCII 签名。
// 必须先调用 Initialize 成功。
SignResult SignWithNumber(int number, const std::string& content);

// 用 unet.dll 生成 WSG 加密参数（kps_wg）。
// number: wsg_number（ASR=0x4ea3）。
// content: 加密明文（通常是 utdid）。
// 返回加密结果（ASCII 字符串）。
// 必须先调用 Initialize 成功。
SignResult EncryptWithNumber(int number, const std::string& content);

// 是否已初始化（unet.dll 已加载 + bind 成功）。
bool IsReady();

// 初始化失败原因（IsReady()=false 时有意义，用于诊断 HUD）。
std::wstring GetInitError();

} // namespace qwen_free_proto_unet
