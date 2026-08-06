#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <string>

// qwen_free_proto_utdid
//
// 千问 IME 免费后端协议层 - UTDID 设备指纹获取（A1 纯协议还原）。
//
// 逆向来源：reverse/work/ghidra/output/UTDID/ + reverse/work/PROTOCOL.md §2.4。
//
// UTDID 生成链：setAppkeyAndSecret(appkey, secret) → setAppName(name) →
// initAUDID() → getUTDID(appkey, buf, &len, 0)。
//
// 关键约束（GHIDRA_FINDINGS.md §1.2）：
//   - UTDID.dll 的 setAppkeyAndSecret 是纯 setter，secret 由调用方传入；
//   - initAUDID 内部创建 CUTDIDLibcurlImpl 实例，**生成涉及 libcurl 网络请求**
//     （服务端下发/校验），不是纯本地算法；
//   - 因此 VoxType **无法自行生成新 UTDID**，只能复用已生成的缓存值。
//
// 本模块提供 3 条获取路径，按优先级降级：
//   1. 用户在 Settings 中手动填入 qwenFreeUtdidOverride（调试用，最高优先级）；
//   2. LoadLibrary UTDID.dll + getUTDID() 读已缓存的 UTDID（千问 IME 已生成）；
//   3. 扫注册表已知持久化位置（HKCU\Software\ALIBABACOM\UTDID\V2 等）。
//
// Appkey 选择（PROTOCOL.md §2.3）：
//   - ASR 调用：qianwen_pc_voice
//   - LLM 后处理：qianwen_voice_app_pc
// UTDID 是设备级标识，与 appkey 无关（一个设备一个 UTDID）。

namespace qwen_free_proto_utdid {

// 千问 IME 安装目录下的 UTDID.dll 相对路径。
constexpr const wchar_t* kQianwenUtdidRelativePath =
    L"\\UTDID.dll";

// 千问 IME 可能的安装根目录（按优先级）。
extern const wchar_t* const kQianwenInstallCandidates[];

// 已知的 UTDID 持久化注册表位置（Aliyun UTForPC 标准）。
struct UtdidRegistryLocation {
    HKEY root;
    const wchar_t* subkey;
    const wchar_t* valueName;
};
extern const UtdidRegistryLocation kUtdidRegistryLocations[];

// 探测千问 IME 安装目录。
// overrideDir 非空且存在时直接用；否则扫描 kQianwenInstallCandidates。
// 返回空串表示未找到。
std::wstring DetectQianwenInstallDir(const std::wstring& overrideDir);

// UTDID 获取结果。
struct UtdidResult {
    bool ok = false;
    std::string utdid;        // 24 字符 base62（示例值不写入源码）
    std::wstring source;      // 来源描述：override / dll / registry
    std::wstring error;       // 失败原因
    DWORD elapsedMs = 0;
};

// 获取 UTDID。
// overrideUtdid 非空时直接使用（最高优先级，调试用）。
// qianwenInstallDir 非空时优先在该目录加载 UTDID.dll；
// 否则扫描 kQianwenInstallCandidates。
UtdidResult GetUtdid(const std::wstring& overrideUtdid,
                     const std::wstring& qianwenInstallDir);

// 快速判断 UTDID 是否看起来合法（24 字符 base62，不含 fff... 占位符）。
bool IsValidUtdid(const std::string& utdid);

// 获取 LoadLibrary 失败的详细错误（用于日志）。
std::wstring FormatLoadLibraryError(DWORD err, const std::wstring& path);

} // namespace qwen_free_proto_utdid
