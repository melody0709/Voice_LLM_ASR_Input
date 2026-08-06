#include "qwen_free_proto_utdid.h"

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace qwen_free_proto_utdid {

const wchar_t* const kQianwenInstallCandidates[] = {
    L"C:\\Program Files\\QianwenIME",
    L"C:\\Program Files (x86)\\QianwenIME",
    nullptr,
};

const UtdidRegistryLocation kUtdidRegistryLocations[] = {
    { HKEY_CURRENT_USER,  L"Software\\ALIBABACOM\\UTDID\\V2",     L"UTDID" },
    { HKEY_CURRENT_USER,  L"Software\\ALIBABACOM\\UTDID",         L"UTDID" },
    { HKEY_CURRENT_USER,  L"Software\\ALIBABACOM\\UTDID\\V2",     L""      }, // default value
    { HKEY_LOCAL_MACHINE, L"Software\\ALIBABACOM\\UTDID\\V2",     L"UTDID" },
    { HKEY_LOCAL_MACHINE, L"Software\\ALIBABACOM\\UTDID",         L"UTDID" },
    { nullptr, nullptr, nullptr },
};

namespace {
std::wstring CanonicalizeExistingPath(const std::wstring& path,
                                      bool directory);
}

// 找到第一个非空的安装目录（公开接口，供 qwen_free_proto_unet 复用）。
std::wstring DetectQianwenInstallDir(const std::wstring& overrideDir) {
    if (!overrideDir.empty()) {
        DWORD attr = GetFileAttributesW(overrideDir.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return CanonicalizeExistingPath(overrideDir, true);
        }
    }
    for (size_t i = 0; kQianwenInstallCandidates[i]; ++i) {
        DWORD attr = GetFileAttributesW(kQianwenInstallCandidates[i]);
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return CanonicalizeExistingPath(kQianwenInstallCandidates[i], true);
        }
    }
    return L"";
}

namespace {

std::mutex g_utdidDllMutex;

std::wstring CanonicalizeExistingPath(const std::wstring& path,
                                      bool directory) {
    if (path.empty()) return {};
    DWORD required = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (required == 0) return path;
    std::vector<wchar_t> full(static_cast<size_t>(required), L'\0');
    DWORD written = GetFullPathNameW(path.c_str(), required, full.data(), nullptr);
    if (written == 0 || written >= required) return path;
    std::wstring result(full.data(), written);

    HANDLE handle = CreateFileW(
        result.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        directory ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (handle == INVALID_HANDLE_VALUE) return result;
    DWORD needed = GetFinalPathNameByHandleW(handle, nullptr, 0,
                                             FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (needed == 0) {
        CloseHandle(handle);
        return result;
    }
    std::vector<wchar_t> finalPath(static_cast<size_t>(needed) + 1, L'\0');
    const DWORD finalWritten = GetFinalPathNameByHandleW(
        handle, finalPath.data(), static_cast<DWORD>(finalPath.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    if (finalWritten == 0 || finalWritten >= finalPath.size()) return result;
    return std::wstring(finalPath.data(), finalWritten);
}

// SEH 安全的 DLL 调用包装。UTDID.dll 在外部进程加载时可能因
// 全局状态未初始化而访问违规。
struct SafeCallState {
    DWORD exceptionCode = 0;
    bool crashed = false;
};

// UTDID.dll 的 getUTDID 函数原型推断：
//   char* getUTDID(const char* appkey, char* buf, int* len, int unused);
// 缓存命中时不会访问 buf/len，直接返回内部缓存指针。
// 但缓存未命中时会触发 libcurl 网络请求（可能崩溃或挂起）。
//
// 安全策略：调用时传 buf=nullptr, len=0，让 getUTDID 走缓存命中分支。
// 若缓存为空，返回 "ffffffffffffffffffffffff"（24 个 f）。
typedef const char* (__cdecl *FnGetUTDID)(const char*, char*, int*, int);

// SEH 包装的 getUTDID 调用。函数本体不能含需要析构的 C++ 对象。
struct GetUtdidCallResult {
    char value[129] = {};
    size_t length = 0;
    bool nullResult = false;
    bool unterminated = false;
    SafeCallState state;
};

GetUtdidCallResult SafeCallGetUTDID(FnGetUTDID fn, const char* appkey) {
    GetUtdidCallResult r;
    __try {
        // buf=nullptr, len_ptr 指向 0：让 DLL 走缓存命中分支。
        // 若 DLL 强制写 buf 会触发 SEH，被外层捕获。
        int len = 0;
        const char* result = fn(appkey, nullptr, &len, 0);
        if (!result) {
            r.nullResult = true;
            return r;
        }
        size_t length = 0;
        for (; length < 128; ++length) {
            const char c = result[length];
            r.value[length] = c;
            if (c == '\0') break;
        }
        if (length == 128) {
            r.unterminated = true;
            r.length = 128;
        } else {
            r.length = length;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        r.state.crashed = true;
        r.state.exceptionCode = GetExceptionCode();
    }
    return r;
}

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

// 尝试从 UTDID.dll 加载并读取缓存 UTDID。
// 加载失败或 DLL 崩溃时返回空串，调用方降级到注册表扫描。
std::string LoadUtdidFromDll(const std::wstring& qianwenDir, std::wstring& error) {
    std::lock_guard<std::mutex> dllLock(g_utdidDllMutex);
    if (qianwenDir.empty()) {
        error = L"Qianwen IME install dir not found";
        return {};
    }
    const std::wstring canonicalDir = CanonicalizeExistingPath(qianwenDir, true);
    std::wstring dllPath = canonicalDir.empty() ? qianwenDir : canonicalDir;
    if (!dllPath.empty() && dllPath.back() != L'\\' && dllPath.back() != L'/') {
        dllPath += L'\\';
    }
    dllPath += L"UTDID.dll";
    // Load from the installed Qianwen directory with a scoped dependency
    // search path; never alter the global DLL search directory.
    // Use the DLL's own directory for dependencies instead of mutating the
    // process-wide DLL directory while Settings probes and ASR may overlap.
    HMODULE hDll = LoadLibraryExW(dllPath.c_str(), nullptr,
                                  LOAD_WITH_ALTERED_SEARCH_PATH);
    if (hDll) {
        auto fn = reinterpret_cast<FnGetUTDID>(GetProcAddress(hDll, "getUTDID"));
        if (fn) {
            // ASR 用 qianwen_pc_voice；UTDID 是设备级，appkey 不影响缓存读。
            auto r = SafeCallGetUTDID(fn, "qianwen_pc_voice");
            if (!r.state.crashed && !r.nullResult) {
                if (r.unterminated) {
                    error = L"UTDID.dll returned an unterminated value";
                    FreeLibrary(hDll);
                    return {};
                }
                std::string utdid(r.value, r.length);
                if (IsValidUtdid(utdid)) {
                    FreeLibrary(hDll);
                    return utdid;
                }
                // 缓存未初始化，返回 24 个 f。
                if (utdid.find_first_not_of('f') == std::string::npos) {
                    error = L"UTDID.dll cache not initialized (returns 24 f's); "
                            L"need App Secret to generate, fallback to registry";
                } else {
                    // A malformed return can still be a device identifier.
                    // Keep it out of Settings/HUD diagnostics and report only
                    // the shape needed to investigate the failure.
                    error = L"UTDID.dll returned invalid value (length=" +
                            std::to_wstring(utdid.size()) + L")";
                }
            } else if (r.state.crashed) {
                error = L"UTDID.dll getUTDID crashed (0x" +
                        std::to_wstring(r.state.exceptionCode) + L")";
            } else {
                error = L"UTDID.dll getUTDID returned null";
            }
        } else {
            error = L"UTDID.dll missing getUTDID export";
        }
        FreeLibrary(hDll);
    } else {
        error = FormatLoadLibraryError(GetLastError(), dllPath);
    }
    return {};
}

// 从注册表读 UTDID。Aliyun UTForPC 持久化位置。
std::string LoadUtdidFromRegistry(std::wstring& usedLocation) {
    constexpr REGSAM kViews[] = {0, KEY_WOW64_64KEY, KEY_WOW64_32KEY};
    for (size_t i = 0; kUtdidRegistryLocations[i].root; ++i) {
        const auto& loc = kUtdidRegistryLocations[i];
        for (const REGSAM view : kViews) {
            HKEY hKey = nullptr;
            if (RegOpenKeyExW(loc.root, loc.subkey, 0,
                              KEY_READ | view, &hKey) != ERROR_SUCCESS) {
                continue;
            }
            wchar_t buf[256] = {0};
            DWORD bufLen = sizeof(buf) - sizeof(wchar_t);
            DWORD type = 0;
            LSTATUS rc = RegQueryValueExW(
                hKey, loc.valueName, nullptr, &type,
                reinterpret_cast<LPBYTE>(buf), &bufLen);
            RegCloseKey(hKey);
            if (rc != ERROR_SUCCESS) continue;
            if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
            const size_t charCount =
                (std::min)(static_cast<size_t>(bufLen / sizeof(wchar_t)),
                           (sizeof(buf) / sizeof(buf[0])) - 1);
            buf[charCount] = L'\0';
            std::wstring wide(buf, charCount);
            while (!wide.empty() && std::iswspace(wide.back())) {
                wide.pop_back();
            }
            std::string utdid = WideToUtf8(wide);
            if (IsValidUtdid(utdid)) {
                const wchar_t* viewName = view == KEY_WOW64_64KEY ? L"64" :
                                          view == KEY_WOW64_32KEY ? L"32" : L"native";
                usedLocation = L"HK" + std::to_wstring(i) + L"/" + viewName;
                return utdid;
            }
        }
    }
    return {};
}

} // namespace

bool IsValidUtdid(const std::string& utdid) {
    if (utdid.size() != 24) return false;
    // 24 个 'f' 是未初始化占位符。
    if (utdid.find_first_not_of('f') == std::string::npos) return false;
    // base62 字符集 [0-9A-Za-z]
    for (char c : utdid) {
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z'))) {
            return false;
        }
    }
    return true;
}

std::wstring FormatLoadLibraryError(DWORD err, const std::wstring& path) {
    return L"LoadLibraryExW(" + path + L") failed: " + std::to_wstring(err) +
           L" (" + (err == ERROR_MOD_NOT_FOUND ? L"module not found"
                  : err == ERROR_DLL_NOT_FOUND ? L"dll not found"
                  : L"other") + L")";
}

UtdidResult GetUtdid(const std::wstring& overrideUtdid,
                     const std::wstring& qianwenInstallDir) {
    UtdidResult r;
    DWORD t0 = GetTickCount();

    // 1. override 优先（调试用）
    if (!overrideUtdid.empty()) {
        std::string utdid = WideToUtf8(overrideUtdid);
        if (IsValidUtdid(utdid)) {
            r.ok = true;
            r.utdid = utdid;
            r.source = L"override";
            r.elapsedMs = GetTickCount() - t0;
            return r;
        }
        r.error = L"override UTDID invalid (expect 24 base62 chars)";
    }

    // 2. LoadLibrary UTDID.dll + getUTDID() 读缓存
    std::wstring dir = DetectQianwenInstallDir(qianwenInstallDir);
    std::wstring dllErr;
    std::string utdid = LoadUtdidFromDll(dir, dllErr);
    if (!utdid.empty()) {
        r.ok = true;
        r.utdid = utdid;
        r.source = L"dll:" + dir;
        r.elapsedMs = GetTickCount() - t0;
        return r;
    }

    // 3. 注册表扫描降级
    std::wstring usedLoc;
    utdid = LoadUtdidFromRegistry(usedLoc);
    if (!utdid.empty()) {
        r.ok = true;
        r.utdid = utdid;
        r.source = L"registry:" + usedLoc;
        r.elapsedMs = GetTickCount() - t0;
        return r;
    }

    r.ok = false;
    r.error = L"All UTDID sources failed. DLL: " + dllErr +
             L". Registry: no valid UTDID found in known locations. "
             L"Install Qianwen IME and use it once to generate UTDID, "
             L"or manually fill UTDID override in Settings.";
    r.elapsedMs = GetTickCount() - t0;
    return r;
}

} // namespace qwen_free_proto_utdid
