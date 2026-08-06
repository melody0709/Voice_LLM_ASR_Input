#include "qwen_free_proto_unet.h"
#include "asr_runtime_log.h"
#include "globals.h"
#include "qwen_free_proto_utdid.h"

#include <windows.h>
#include <bcrypt.h>
#include <atomic>
#include <array>
#include <mutex>
#include <vector>
#include <cstring>

#pragma comment(lib, "bcrypt.lib")

namespace qwen_free_proto_unet {

namespace {

// core_sign_function 的 RVA（版本相关，当前千问 IME 版本）。
// 来源：reverse/work/probe_unet_cryptor.cpp Step 6 + probe_unet_cryptor.log 验证。
// 如果千问 IME 更新导致此地址变化，SEH 会捕获崩溃，调用方 fallback 到纯算法。
constexpr size_t kCoreSignRva = 0x44b6b0;

// core_encrypt_function 的 RVA（与 core_sign 同版本）。
// 来源：reverse/work/ghidra/output/unet/sign_chain/FUN_18044bb30_0x44bb30.c。
// probe Step 8 验证（但 param_4 传 nullptr 导致返回空，需传 output buffer）。
constexpr size_t kCoreEncryptRva = 0x44bb30;

// unet_native_bind 的 version 参数。
// kNativeVersion = 0x1090301，断言 kNativeVersion >= version，故用 0x1090300。
constexpr int kBindVersion = 0x1090300;

// Fixed RVAs are enabled only for the exact DLL used to verify this ABI.
// Unknown builds must be reverse-engineered and added deliberately.
constexpr char kSupportedUnetSha256[] =
    "eb39951e6bcccfcec9b3576a5014ee35a7db9b3f1f35596a795e12f96f1ef5fa";

struct NativeEntry {
    void* func;
    std::string name;
};

std::vector<NativeEntry> g_table;
CRITICAL_SECTION g_lock;
bool g_lockInited = false;
std::mutex g_initMutex;

HMODULE g_hUnet = nullptr;
void* g_coreSign = nullptr;
void* g_coreEncrypt = nullptr;
std::atomic<bool> g_initialized{false};
std::wstring g_initError;
std::wstring g_loadedDllPath;
// The reverse-engineered core routines use process-global native state and
// are not re-entrant. ASR and LLM workers can otherwise call them at the same
// time (Settings tests can add a third caller).
std::mutex g_callMutex;

// unet_native_bind 的 dispatcher callback。
// 签名（探针验证）：void(void* ctx, void* func, const char* name, uint32_t name_len)。
extern "C" void __cdecl dispatcher(void* ctx, void* func,
                                     const char* name, uint32_t nameLen) {
    (void)ctx;
    if (!g_lockInited || !name) return;
    std::string n;
    if (nameLen > 0 && nameLen < 256) {
        n.assign(name, nameLen);
    } else {
        const size_t length = strnlen_s(name, 255);
        if (length == 255) return;
        n.assign(name, length);
    }
    EnterCriticalSection(&g_lock);
    g_table.push_back({func, n});
    LeaveCriticalSection(&g_lock);
}

typedef long long (__cdecl *FnBind)(int, void*, void*);
typedef void* (__cdecl *FnCoreSign)(void*, void**, int, void*);
// core_encrypt_function 签名（Ghidra 反编译 FUN_18044bb30）：
//   char core_encrypt(void* ignored, int number, ByteView* bv, void** out)
// 返回 char（成功标志），结果写入 out[0..2]（与 core_sign 的 output buffer 相同）。
// 来源：_cryptor_impl_all.c impl_cryptor_encrypt_with_number 调用链。
typedef char (__cdecl *FnCoreEncrypt)(void*, int, void*, void**);

// core_sign_function 的 byte_view 参数：{void* data, uint64_t len}。
struct ByteView {
    void* data;
    uint64_t len;
};

// SEH helper：包裹 unet_native_bind 调用。
// 必须独立函数，因为 __try 不能与带析构对象的函数共存。
// 返回 0=成功（rc 写回 *outRc），非 0=异常码。
DWORD RunBind(FnBind fn, int version, void* cb, long long* outRc) {
    __try {
        *outRc = fn(version, cb, nullptr);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<DWORD>(GetExceptionCode());
    }
}

// SEH helper：包裹 core_sign_function 调用。
// 返回 0=成功，非 0=异常码。
DWORD RunCoreSign(FnCoreSign fn, int number, ByteView* bv,
                   void* out[3]) {
    __try {
        fn(nullptr, out, number, bv);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<DWORD>(GetExceptionCode());
    }
}

// SEH helper：包裹 core_encrypt_function 调用。
// 返回 0=成功，非 0=异常码。rc 写回 *outRc。
DWORD RunCoreEncrypt(FnCoreEncrypt fn, int number, ByteView* bv,
                      void* out[3], char* outRc) {
    __try {
        *outRc = fn(nullptr, number, bv, out);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<DWORD>(GetExceptionCode());
    }
}

struct NativeOutputMeta {
    uintptr_t buffer = 0;
    uint64_t length = 0;
};

DWORD ReadNativeOutputMeta(void* out[3], NativeOutputMeta* meta) {
    if (!out || !meta) return ERROR_INVALID_PARAMETER;
    __try {
        meta->buffer = reinterpret_cast<uintptr_t>(out[0]);
        meta->length = static_cast<uint64_t>(
            reinterpret_cast<uintptr_t>(out[1]));
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<DWORD>(GetExceptionCode());
    }
}

DWORD CopyNativeBytes(const void* source, void* destination, size_t length) {
    if (length == 0) return 0;
    if (!source || !destination) return ERROR_INVALID_PARAMETER;
    __try {
        std::memcpy(destination, source, length);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<DWORD>(GetExceptionCode());
    }
}

void LogErr(const std::wstring& msg) {
    if (msg.empty()) return;
    int len = WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1,
                                   nullptr, 0, nullptr, nullptr);
    if (len <= 0) return;
    std::string utf8(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, msg.c_str(), -1,
                        utf8.data(), len, nullptr, nullptr);
    utf8.resize(static_cast<size_t>(len - 1));
    asr_runtime_log::Write("[qwen_free_unet] %s", utf8.c_str());
}

bool IsExecutableModuleRva(HMODULE module, size_t rva) {
    if (!module) return false;
    __try {
        const auto* base = static_cast<const BYTE*>(static_cast<const void*>(module));
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0) return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE ||
            nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
            rva >= static_cast<size_t>(nt->OptionalHeader.SizeOfImage)) {
            return false;
        }

        MEMORY_BASIC_INFORMATION info = {};
        if (VirtualQuery(base + rva, &info, sizeof(info)) != sizeof(info)) {
            return false;
        }
        switch (info.Protect & 0xffu) {
        case PAGE_EXECUTE:
        case PAGE_EXECUTE_READ:
        case PAGE_EXECUTE_READWRITE:
        case PAGE_EXECUTE_WRITECOPY:
            return true;
        default:
            return false;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::wstring FullPath(const std::wstring& path) {
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::vector<wchar_t> buffer(required, L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), required, buffer.data(), nullptr);
    if (written == 0 || written >= required) return path;
    std::wstring result(buffer.data(), written);

    HANDLE file = CreateFileW(
        result.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return result;
    DWORD finalRequired = GetFinalPathNameByHandleW(
        file, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (finalRequired == 0) {
        CloseHandle(file);
        return result;
    }
    std::vector<wchar_t> finalPath(static_cast<size_t>(finalRequired) + 1, L'\0');
    const DWORD finalWritten = GetFinalPathNameByHandleW(
        file, finalPath.data(), static_cast<DWORD>(finalPath.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(file);
    if (finalWritten == 0 || finalWritten >= finalPath.size()) return result;
    return std::wstring(finalPath.data(), finalWritten);
}

bool ComputeSha256(const std::wstring& path, std::string& outHash,
                   std::wstring& outError) {
    outHash.clear();
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        outError = L"cannot open unet.dll for fingerprinting: " +
                   std::to_wstring(GetLastError());
        return false;
    }

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD cb = 0;
    bool ok = false;
    std::vector<BYTE> hashObject;
    std::vector<BYTE> digest;
    do {
        if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                        nullptr, 0) != 0) break;
        if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                              reinterpret_cast<PUCHAR>(&objectLength),
                              sizeof(objectLength), &cb, 0) != 0) break;
        if (BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                              reinterpret_cast<PUCHAR>(&hashLength),
                              sizeof(hashLength), &cb, 0) != 0) break;
        hashObject.resize(objectLength);
        digest.resize(hashLength);
        if (BCryptCreateHash(algorithm, &hash, hashObject.data(), objectLength,
                             nullptr, 0, 0) != 0) break;
        std::array<BYTE, 64 * 1024> buffer{};
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                          &read, nullptr)) break;
            if (read == 0) {
                if (BCryptFinishHash(hash, digest.data(), hashLength, 0) != 0) break;
                static constexpr char kHex[] = "0123456789abcdef";
                outHash.reserve(digest.size() * 2);
                for (BYTE value : digest) {
                    outHash.push_back(kHex[value >> 4]);
                    outHash.push_back(kHex[value & 0x0f]);
                }
                ok = true;
                break;
            }
            if (BCryptHashData(hash, buffer.data(), read, 0) != 0) break;
        }
    } while (false);

    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok && outError.empty()) outError = L"failed to calculate unet.dll SHA-256";
    return ok;
}

DWORD TryReleaseNativeBuffer(void* buffer) {
    if (!buffer) return 0;
    __try {
        HANDLE heap = GetProcessHeap();
        if (heap && HeapValidate(heap, 0, buffer)) {
            HeapFree(heap, 0, buffer);
            return 0;
        }
        return ERROR_INVALID_DATA;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return static_cast<DWORD>(GetExceptionCode());
    }
}

void ReleaseNativeBuffer(void* buffer) {
    if (!buffer) return;
    const DWORD result = TryReleaseNativeBuffer(buffer);
    if (result == 0) return;
    if (result != ERROR_INVALID_DATA) {
        LogErr(L"native output buffer validation crashed");
        return;
    }
    // Unknown allocator: retain safety over attempting an invalid free.
    LogErr(L"native output buffer is not owned by the process heap; not freed");
}

bool IsLowerHexString(const std::string& value, size_t expectedLength) {
    if (value.size() != expectedLength) return false;
    for (const unsigned char c : value) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return false;
        }
    }
    return true;
}

} // namespace

bool Initialize(const std::wstring& installDir, std::wstring& out_error) {
    std::lock_guard<std::mutex> initLock(g_initMutex);
    std::wstring dir = installDir;
    if (dir.empty()) {
        dir = qwen_free_proto_utdid::DetectQianwenInstallDir(L"");
        if (dir.empty()) {
            out_error = L"Qianwen IME install dir not found (set qwenFreeShellPath in Settings)";
            g_initError = out_error;
            return false;
        }
    }

    // The full path below keeps dependency resolution scoped to unet.dll.
    std::wstring dllPath = dir;
    if (!dllPath.empty() && dllPath.back() != L'\\' && dllPath.back() != L'/') {
        dllPath += L'\\';
    }
    dllPath += L"unet.dll";
    dllPath = FullPath(dllPath);

    if (g_initialized.load(std::memory_order_acquire)) {
        if (_wcsicmp(g_loadedDllPath.c_str(), dllPath.c_str()) == 0) {
            out_error.clear();
            g_initError.clear();
            return true;
        }
        out_error = L"a different unet.dll is already active; restart VoxType to apply the new Shell Path";
        g_initError = out_error;
        return false;
    }

    std::string dllHash;
    if (!ComputeSha256(dllPath, dllHash, out_error)) {
        g_initError = out_error;
        return false;
    }
    if (dllHash != kSupportedUnetSha256) {
        out_error = L"unet.dll version is not supported (SHA-256 mismatch)";
        g_initError = out_error;
        return false;
    }

    if (!g_lockInited) {
        InitializeCriticalSection(&g_lock);
        g_lockInited = true;
    }
    {
        EnterCriticalSection(&g_lock);
        g_table.clear();
        LeaveCriticalSection(&g_lock);
    }
    // A fully qualified path plus ALTERED_SEARCH_PATH resolves dependencies
    // beside unet.dll without changing the process-wide DLL search directory.
    g_hUnet = LoadLibraryExW(dllPath.c_str(), nullptr,
                              LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_hUnet) {
        DWORD err = GetLastError();
        out_error = L"LoadLibrary unet.dll failed: " + std::to_wstring(err) +
                    L" (path=" + dllPath + L")";
        g_initError = out_error;
        return false;
    }

    // unet_native_bind 注册函数表 + 初始化全局状态。
    FnBind fnBind = (FnBind)GetProcAddress(g_hUnet, "unet_native_bind");
    if (!fnBind) {
        out_error = L"unet_native_bind export not found";
        g_initError = out_error;
        FreeLibrary(g_hUnet);
        g_hUnet = nullptr;
        return false;
    }

    long long rc = -1;
    DWORD exc = RunBind(fnBind, kBindVersion, (void*)dispatcher, &rc);
    if (exc != 0) {
        out_error = L"unet_native_bind crashed: 0x" +
                    std::to_wstring(static_cast<unsigned long>(exc));
        g_initError = out_error;
        FreeLibrary(g_hUnet);
        g_hUnet = nullptr;
        return false;
    }

    bool hasNativeEntries = false;
    size_t nativeEntryCount = 0;
    {
        EnterCriticalSection(&g_lock);
        hasNativeEntries = !g_table.empty();
        nativeEntryCount = g_table.size();
        LeaveCriticalSection(&g_lock);
    }
    if (!hasNativeEntries) {
        out_error = L"unet_native_bind registered 0 functions (rc=" +
                    std::to_wstring(rc) + L")";
        g_initError = out_error;
        FreeLibrary(g_hUnet);
        g_hUnet = nullptr;
        return false;
    }

    // core_sign_function @ RVA 0x44b6b0。
    if (!IsExecutableModuleRva(g_hUnet, kCoreSignRva) ||
        !IsExecutableModuleRva(g_hUnet, kCoreEncryptRva)) {
        out_error = L"unet.dll is incompatible: required signer RVAs are outside executable image pages";
        g_initError = out_error;
        FreeLibrary(g_hUnet);
        g_hUnet = nullptr;
        return false;
    }

    g_coreSign = static_cast<char*>(static_cast<void*>(g_hUnet)) + kCoreSignRva;
    // core_encrypt_function @ RVA 0x44bb30。
    g_coreEncrypt = static_cast<char*>(static_cast<void*>(g_hUnet)) + kCoreEncryptRva;
    g_loadedDllPath = dllPath;
    g_initialized.store(true, std::memory_order_release);
    g_initError.clear();

    LogErr(L"initialized: " + std::to_wstring(nativeEntryCount) +
           L" functions registered, core_sign RVA=0x" +
           std::to_wstring(static_cast<unsigned long>(kCoreSignRva)) +
           L", core_encrypt RVA=0x" +
           std::to_wstring(static_cast<unsigned long>(kCoreEncryptRva)));
    return true;
}

SignResult SignWithNumber(int number, const std::string& content) {
    SignResult r;
    std::lock_guard<std::mutex> callLock(g_callMutex);
    if (!g_initialized.load(std::memory_order_acquire) || !g_coreSign) {
        r.error = L"unet.dll not initialized";
        return r;
    }

    FnCoreSign fn = (FnCoreSign)g_coreSign;
    ByteView bv;
    bv.data = const_cast<char*>(content.data());
    bv.len = content.size();

    // 输出 buffer：3 qwords。
    //   out[0] = result buffer ptr（operator_new 分配，48 字节）
    //   out[1] = result length（字节数，通常 44）
    //   out[2] = capacity flag（0x8000000000000030）
    void* out[3] = {nullptr, nullptr, nullptr};

    DWORD exc = RunCoreSign(fn, number, &bv, out);
    if (exc != 0) {
        r.error = L"core_sign_function crashed: 0x" +
                  std::to_wstring(static_cast<unsigned long>(exc));
        return r;
    }

    NativeOutputMeta meta;
    const DWORD metaError = ReadNativeOutputMeta(out, &meta);
    if (metaError != 0) {
        r.error = L"core_sign output metadata unreadable: 0x" +
                  std::to_wstring(static_cast<unsigned long>(metaError));
        return r;
    }
    if (!meta.buffer) {
        r.error = L"core_sign returned null (number=" +
                  std::to_wstring(number) + L" not in dispatch table?)";
        return r;
    }

    auto* buf = reinterpret_cast<uint8_t*>(meta.buffer);
    const uint64_t len = meta.length;

    if (len == 0 || len > 256) {
        r.error = L"core_sign bad length: " + std::to_wstring(len);
        ReleaseNativeBuffer(buf);
        return r;
    }

    r.sign.resize(static_cast<size_t>(len));
    const DWORD copyError = CopyNativeBytes(
        buf, r.sign.data(), static_cast<size_t>(len));
    ReleaseNativeBuffer(buf);
    if (copyError != 0) {
        r.sign.clear();
        r.error = L"core_sign output unreadable: 0x" +
                  std::to_wstring(static_cast<unsigned long>(copyError));
        return r;
    }
    // The approved ABI returns a 4-hex WSG number plus a 40-hex HMAC-SHA1
    // digest.  Never let an unexpected native buffer (including embedded NUL
    // or arbitrary bytes) reach a URL/header builder.
    if (!IsLowerHexString(r.sign, 44)) {
        r.sign.clear();
        r.error = L"core_sign returned malformed WSG signature";
        return r;
    }
    r.ok = true;
    return r;
}

SignResult EncryptWithNumber(int number, const std::string& content) {
    SignResult r;
    std::lock_guard<std::mutex> callLock(g_callMutex);
    if (!g_initialized.load(std::memory_order_acquire) || !g_coreEncrypt) {
        r.error = L"unet.dll not initialized";
        return r;
    }

    FnCoreEncrypt fn = (FnCoreEncrypt)g_coreEncrypt;
    ByteView bv;
    bv.data = const_cast<char*>(content.data());
    bv.len = content.size();

    // 输出 buffer：3 qwords（与 core_sign 相同格式）。
    // 来源：_cryptor_impl_all.c impl_cryptor_encrypt_with_number：
    //   local_58/uStack_50/local_48 三连续 qword 作为 output buffer。
    void* out[3] = {nullptr, nullptr, nullptr};

    char rc = 0;
    DWORD exc = RunCoreEncrypt(fn, number, &bv, out, &rc);
    if (exc != 0) {
        r.error = L"core_encrypt_function crashed: 0x" +
                  std::to_wstring(static_cast<unsigned long>(exc));
        return r;
    }

    if (rc == 0) {
        r.error = L"core_encrypt returned false (number=" +
                  std::to_wstring(number) + L" not in dispatch table?)";
        return r;
    }

    NativeOutputMeta meta;
    const DWORD metaError = ReadNativeOutputMeta(out, &meta);
    if (metaError != 0) {
        r.error = L"core_encrypt output metadata unreadable: 0x" +
                  std::to_wstring(static_cast<unsigned long>(metaError));
        return r;
    }
    if (!meta.buffer) {
        r.error = L"core_encrypt returned null output";
        return r;
    }

    auto* buf = reinterpret_cast<uint8_t*>(meta.buffer);
    const uint64_t len = meta.length;

    if (len == 0 || len > 4096) {
        r.error = L"core_encrypt bad length: " + std::to_wstring(len);
        ReleaseNativeBuffer(buf);
        return r;
    }

    // core_encrypt 返回原始二进制（与 core_sign 不同，sign 内部已做 hex 转换）。
    // URL query 和签名内容需要 ASCII hex，与 sign_wg 格式一致。
    // 来源：memscan 真实样本 URL 参数均为可打印 hex（如 sign=4ea3...）。
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(static_cast<size_t>(len) * 2);
    std::vector<uint8_t> nativeBytes(static_cast<size_t>(len));
    const DWORD copyError = CopyNativeBytes(
        buf, nativeBytes.data(), static_cast<size_t>(len));
    if (copyError != 0) {
        ReleaseNativeBuffer(buf);
        r.error = L"core_encrypt output unreadable: 0x" +
                  std::to_wstring(static_cast<unsigned long>(copyError));
        return r;
    }
    for (uint64_t i = 0; i < len; ++i) {
        hex.push_back(kHex[(nativeBytes[static_cast<size_t>(i)] >> 4) & 0x0f]);
        hex.push_back(kHex[nativeBytes[static_cast<size_t>(i)] & 0x0f]);
    }
    ReleaseNativeBuffer(buf);
    r.sign = hex;
    r.ok = true;
    return r;
}

bool IsReady() {
    return g_initialized.load(std::memory_order_acquire);
}

std::wstring GetInitError() {
    std::lock_guard<std::mutex> initLock(g_initMutex);
    return g_initError;
}

} // namespace qwen_free_proto_unet
