#include "asr_runtime_log.h"

#include "globals.h"

#include <windows.h>

#include <cstdio>
#include <mutex>
#include <string>

namespace asr_runtime_log {
namespace {

constexpr ULONGLONG kMaxLogBytes = 5ull * 1024ull * 1024ull;
constexpr int kArchiveCount = 2;

std::mutex g_logMutex;

std::wstring TempLogPath(const wchar_t* fileName) {
    wchar_t tempPath[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, tempPath);
    if (len == 0 || len >= MAX_PATH) return {};
    std::wstring path(tempPath, len);
    path += fileName;
    return path;
}

std::wstring ArchivePath(const std::wstring& path, int index) {
    return path + L"." + std::to_wstring(index);
}

bool ShouldRotate(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    ULARGE_INTEGER size = {};
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;
    return size.QuadPart >= kMaxLogBytes;
}

void RotateIfNeeded(const std::wstring& path) {
    if (!ShouldRotate(path)) return;

    DeleteFileW(ArchivePath(path, kArchiveCount).c_str());
    for (int index = kArchiveCount; index >= 2; --index) {
        MoveFileExW(ArchivePath(path, index - 1).c_str(),
                    ArchivePath(path, index).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    MoveFileExW(path.c_str(), ArchivePath(path, 1).c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void WriteNamedVLocked(const wchar_t* fileName, const char* format, va_list args) {
    const std::wstring path = TempLogPath(fileName);
    if (path.empty()) return;

    RotateIfNeeded(path);

    FILE* file = nullptr;
    _wfopen_s(&file, path.c_str(), L"a");
    if (!file) return;

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    fprintf(file,
            "[%04u-%02u-%02u %02u:%02u:%02u.%03u] pid=%lu ",
            st.wYear, st.wMonth, st.wDay,
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
            static_cast<unsigned long>(GetCurrentProcessId()));
    vfprintf(file, format, args);
    fputc('\n', file);
    fclose(file);
}

} // namespace

bool Enabled() {
    return g_enableDebugMode.load(std::memory_order_relaxed);
}

void Write(const char* format, ...) {
    if (!Enabled() || !format) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    va_list args;
    va_start(args, format);
    WriteNamedVLocked(L"voxtype_asr_runtime.log", format, args);
    va_end(args);
}

void WriteNamedV(const wchar_t* fileName, const char* format, va_list args) {
    if (!Enabled() || !fileName || !format) return;
    std::lock_guard<std::mutex> lock(g_logMutex);
    WriteNamedVLocked(fileName, format, args);
}

} // namespace asr_runtime_log
