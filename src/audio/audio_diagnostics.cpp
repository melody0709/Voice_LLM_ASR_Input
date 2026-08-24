#include "audio_diagnostics.h"

#include "resource.h"

#include <bcrypt.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")

namespace audio_diagnostics {
namespace {

constexpr double kNearSilentThreshold = 0.001;
constexpr double kNonSilentPacketThreshold = 0.002;
constexpr int kSha256Bytes = 32;
constexpr ULONGLONG kFileTimeTicksPerSecond = 10000000ull;
constexpr ULONGLONG kFileTimeTicksPerDay = 24ull * 60ull * 60ull * kFileTimeTicksPerSecond;

struct SignalAccumulator {
    uint64_t count = 0;
    long double sumSquares = 0.0;
    double peak = 0.0;
    uint64_t zeros = 0;
    uint64_t nearSilent = 0;
    uint64_t clipping = 0;
};

struct CaptureAccumulator {
    bool active = false;
    CaptureDeviceInfo device;
    ULONGLONG startedTick = 0;
    ULONGLONG lastCallbackTick = 0;
    double maxCallbackGapMs = 0.0;
    double firstNonSilentDelayMs = -1.0;
    uint64_t nativeFrames = 0;
    uint64_t silentPackets = 0;
    uint64_t silentFrames = 0;
    uint64_t discontinuities = 0;
    SignalAccumulator output;
    std::vector<long double> nativeChannelSquares;
    std::vector<uint64_t> nativeChannelCounts;
    long double downmixSquares = 0.0;
    uint64_t downmixCount = 0;
};

struct StageState {
    StageMetadata metadata;
    std::shared_ptr<std::vector<BYTE>> pcm;
    StageTerminal terminal;
    bool completed = false;
};

struct AttemptState {
    AttemptMetadata metadata;
    std::wstring captureId;
    SYSTEMTIME utcTime = {};
    SYSTEMTIME localTime = {};
    std::shared_ptr<const std::vector<BYTE>> capturePcm;
    CaptureSnapshot capture;
    std::vector<StageState> stages;
    FinalResult finalResult;
};

struct Artifact {
    std::string id;
    std::wstring fileName;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    std::string sha256;
};

struct ManagedGroup {
    std::wstring manifestName;
    std::wstring prefix;
    ULONGLONG writeTime = 0;
    uint64_t bytes = 0;
};

std::mutex g_captureMutex;
CaptureAccumulator g_capture;

std::mutex g_attemptMutex;
std::map<uint64_t, AttemptState> g_attempts;

std::atomic<LogCallback> g_logCallback{nullptr};
std::mutex g_directoryMutex;
std::wstring g_directoryOverride;

std::mutex g_writerMutex;
std::condition_variable g_writerCv;
size_t g_pendingWriters = 0;
std::mutex g_fileIoMutex;
std::vector<std::thread> g_writerThreads;

struct WriterThreadGuard {
    ~WriterThreadGuard() {
        WaitForPendingWrites(INFINITE);
    }
};

WriterThreadGuard g_writerThreadGuard;

double LinearToDbfs(double value) {
    if (!(value > 0.0)) return -120.0;
    return (std::max)(-120.0, 20.0 * std::log10(value));
}

void AddSignalSample(SignalAccumulator& accumulator, double value) {
    if (!std::isfinite(value)) value = 0.0;
    value = std::clamp(value, -1.0, 1.0);
    const double absolute = std::abs(value);
    ++accumulator.count;
    accumulator.sumSquares += static_cast<long double>(value) * value;
    accumulator.peak = (std::max)(accumulator.peak, absolute);
    if (absolute == 0.0) ++accumulator.zeros;
    if (absolute <= kNearSilentThreshold) ++accumulator.nearSilent;
    if (absolute >= (32760.0 / 32768.0)) ++accumulator.clipping;
}

Pcm16Metrics FinishSignal(const SignalAccumulator& accumulator) {
    Pcm16Metrics result;
    result.sampleCount = accumulator.count;
    if (accumulator.count == 0) return result;
    const double rms = std::sqrt(static_cast<double>(
        accumulator.sumSquares / static_cast<long double>(accumulator.count)));
    result.rmsDbfs = LinearToDbfs(rms);
    result.peakDbfs = LinearToDbfs(accumulator.peak);
    result.zeroRatio = static_cast<double>(accumulator.zeros) / accumulator.count;
    result.nearSilentRatio = static_cast<double>(accumulator.nearSilent) / accumulator.count;
    result.clippingRatio = static_cast<double>(accumulator.clipping) / accumulator.count;
    return result;
}

void NoteCallbackLocked(bool nonSilent) {
    const ULONGLONG now = GetTickCount64();
    if (g_capture.lastCallbackTick != 0 && now >= g_capture.lastCallbackTick) {
        g_capture.maxCallbackGapMs = (std::max)(
            g_capture.maxCallbackGapMs,
            static_cast<double>(now - g_capture.lastCallbackTick));
    }
    g_capture.lastCallbackTick = now;
    if (nonSilent && g_capture.firstNonSilentDelayMs < 0.0 &&
        now >= g_capture.startedTick) {
        g_capture.firstNonSilentDelayMs =
            static_cast<double>(now - g_capture.startedTick);
    }
}

std::string WideToUtf8Local(const std::wstring& value) {
    if (value.empty()) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), needed, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWideLocal(const std::string& value) {
    if (value.empty()) return {};
    const int needed = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                           static_cast<int>(value.size()),
                                           nullptr, 0);
    if (needed <= 0) return {};
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        result.data(), needed);
    return result;
}

std::string JsonEscape(const std::string& value) {
    std::string result;
    result.reserve(value.size() + 16);
    const char hex[] = "0123456789abcdef";
    for (unsigned char ch : value) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\b': result += "\\b"; break;
        case '\f': result += "\\f"; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default:
            if (ch < 0x20) {
                result += "\\u00";
                result.push_back(hex[(ch >> 4) & 0xf]);
                result.push_back(hex[ch & 0xf]);
            } else {
                result.push_back(static_cast<char>(ch));
            }
            break;
        }
    }
    return result;
}

std::string JsonEscape(const std::wstring& value) {
    return JsonEscape(WideToUtf8Local(value));
}

std::string SanitizeAscii(std::string value) {
    for (char& ch : value) {
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) {
            ch = '_';
        }
    }
    if (value.empty()) value = "unknown";
    if (value.size() > 48) value.resize(48);
    return value;
}

void EmitLog(std::string line) {
    if (LogCallback callback = g_logCallback.load(std::memory_order_acquire)) {
        callback(line);
    }
}

void EmitStageTerminalLog(uint64_t attemptId,
                          const StageMetadata& metadata,
                          const StageTerminal& terminal,
                          size_t inputBytes) {
    EmitLog("event=asr_stage_final attempt=" + std::to_string(attemptId) +
            " kind=" + StageKindName(metadata.kind) +
            " stage_index=" + std::to_string(metadata.index) +
            " backend=" + SanitizeAscii(WideToUtf8Local(metadata.backend)) +
            " terminal=" + SanitizeAscii(terminal.terminal) +
            " reason=" + SanitizeAscii(terminal.reason) +
            " provider_code=" + SanitizeAscii(terminal.providerCode) +
            " input_bytes=" + std::to_string(inputBytes) +
            " sent_bytes=" + std::to_string(metadata.sentBytes) +
            " network_bytes=" + std::to_string(metadata.networkBytes) +
            " text_chars=" + std::to_string(terminal.textChars) +
            " committed_text_chars=" +
                std::to_string(terminal.committedTextChars) +
            " elapsed_ms=" + std::to_string(terminal.elapsedMs));
}

bool PathExists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ExecutableDir() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                               static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            std::wstring path(buffer.data(), length);
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? L"." : path.substr(0, slash);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring DefaultDiagnosticDir() {
    const std::wstring executableDir = ExecutableDir();
    if (!executableDir.empty() && PathExists(executableDir + L"\\portable.flag")) {
        return executableDir + L"\\diagnostics\\audio";
    }

    PWSTR localAppData = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE,
                                       nullptr, &localAppData)) && localAppData) {
        result = localAppData;
        CoTaskMemFree(localAppData);
        result += L"\\VoxType\\diagnostics\\audio";
    }
    return result;
}

bool EnsureDirectoryPath(const std::wstring& path, std::wstring* error) {
    if (path.empty()) {
        if (error) *error = L"Diagnostic audio directory is unavailable.";
        return false;
    }
    const int result = SHCreateDirectoryExW(nullptr, path.c_str(), nullptr);
    if (result == ERROR_SUCCESS || result == ERROR_ALREADY_EXISTS ||
        result == ERROR_FILE_EXISTS) {
        return true;
    }
    if (error) {
        *error = L"Unable to create diagnostic audio directory (error " +
                 std::to_wstring(result) + L").";
    }
    return false;
}

std::wstring CaptureId(const SYSTEMTIME& local, uint64_t attemptId) {
    wchar_t buffer[96] = {};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u.%03u-attempt%llu",
               local.wYear, local.wMonth, local.wDay,
               local.wHour, local.wMinute, local.wSecond, local.wMilliseconds,
               static_cast<unsigned long long>(attemptId));
    return buffer;
}

StageState* FindStage(AttemptState& attempt, StageKind kind, unsigned index) {
    for (StageState& stage : attempt.stages) {
        if (stage.metadata.kind == kind && stage.metadata.index == index) {
            return &stage;
        }
    }
    return nullptr;
}

StageState& FindOrCreateStage(AttemptState& attempt,
                              const StageMetadata& metadata) {
    if (StageState* existing = FindStage(attempt, metadata.kind, metadata.index)) {
        if (!metadata.backend.empty()) existing->metadata.backend = metadata.backend;
        if (!metadata.model.empty()) existing->metadata.model = metadata.model;
        if (!metadata.transport.empty()) existing->metadata.transport = metadata.transport;
        if (!metadata.reason.empty()) existing->metadata.reason = metadata.reason;
        if (!metadata.encoding.empty()) existing->metadata.encoding = metadata.encoding;
        existing->metadata.vadEnabled =
            existing->metadata.vadEnabled || metadata.vadEnabled;
        existing->metadata.vadActive =
            existing->metadata.vadActive || metadata.vadActive;
        existing->metadata.vadDetectedSpeech =
            existing->metadata.vadDetectedSpeech || metadata.vadDetectedSpeech;
        if (!metadata.vadModel.empty()) existing->metadata.vadModel = metadata.vadModel;
        if (metadata.vadInputBytes) existing->metadata.vadInputBytes = metadata.vadInputBytes;
        if (metadata.vadOutputBytes) existing->metadata.vadOutputBytes = metadata.vadOutputBytes;
        if (metadata.sentBytes) existing->metadata.sentBytes = metadata.sentBytes;
        if (metadata.networkBytes) existing->metadata.networkBytes = metadata.networkBytes;
        return *existing;
    }
    attempt.stages.push_back({});
    attempt.stages.back().metadata = metadata;
    return attempt.stages.back();
}

bool TerminalLooksSuccessful(const std::string& terminal) {
    return terminal == "success" || terminal == "task_finished" ||
           terminal == "response_done" || terminal == "provider_success";
}

bool TerminalLooksEmptyOrFailed(const std::string& terminal) {
    if (terminal.empty()) return false;
    return terminal.find("empty") != std::string::npos ||
           terminal.find("no_speech") != std::string::npos ||
           terminal.find("no_words") != std::string::npos ||
           terminal.find("timeout") != std::string::npos ||
           terminal.find("error") != std::string::npos ||
           terminal.find("failure") != std::string::npos ||
           terminal.find("peer_close") != std::string::npos ||
           terminal.find("aborted") != std::string::npos;
}

bool HasContradictoryStages(const AttemptState& attempt) {
    bool successful = false;
    bool emptyOrFailed = false;
    for (const StageState& stage : attempt.stages) {
        if (!stage.completed) continue;
        successful = successful || TerminalLooksSuccessful(stage.terminal.terminal);
        emptyOrFailed = emptyOrFailed || TerminalLooksEmptyOrFailed(stage.terminal.terminal);
    }
    return successful && emptyOrFailed;
}

void PutLe16(std::vector<BYTE>& output, size_t offset, uint16_t value) {
    output[offset] = static_cast<BYTE>(value & 0xff);
    output[offset + 1] = static_cast<BYTE>((value >> 8) & 0xff);
}

void PutLe32(std::vector<BYTE>& output, size_t offset, uint32_t value) {
    output[offset] = static_cast<BYTE>(value & 0xff);
    output[offset + 1] = static_cast<BYTE>((value >> 8) & 0xff);
    output[offset + 2] = static_cast<BYTE>((value >> 16) & 0xff);
    output[offset + 3] = static_cast<BYTE>((value >> 24) & 0xff);
}

bool WriteHandleAll(HANDLE file, const BYTE* data, size_t bytes) {
    size_t offset = 0;
    while (offset < bytes) {
        const DWORD chunk = static_cast<DWORD>((std::min<size_t>)(
            bytes - offset, static_cast<size_t>(0x7ffff000)));
        DWORD written = 0;
        if (!WriteFile(file, data + offset, chunk, &written, nullptr) ||
            written != chunk) {
            return false;
        }
        offset += written;
    }
    return true;
}

bool AtomicWriteFile(const std::wstring& finalPath,
                     const BYTE* data,
                     size_t bytes,
                     std::wstring& error) {
    const std::wstring tempPath = finalPath + L".tmp";
    HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS,
                              FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_WRITE_THROUGH,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"CreateFile failed (" + std::to_wstring(GetLastError()) + L")";
        return false;
    }
    const bool wrote = WriteHandleAll(file, data, bytes);
    const bool flushed = wrote && FlushFileBuffers(file) != FALSE;
    const DWORD closeError = CloseHandle(file) ? ERROR_SUCCESS : GetLastError();
    if (!wrote || !flushed || closeError != ERROR_SUCCESS) {
        error = L"Write/flush failed (" + std::to_wstring(GetLastError()) + L")";
        DeleteFileW(tempPath.c_str());
        return false;
    }
    if (!MoveFileExW(tempPath.c_str(), finalPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = L"Atomic rename failed (" + std::to_wstring(GetLastError()) + L")";
        DeleteFileW(tempPath.c_str());
        return false;
    }
    return true;
}

bool AtomicWriteFile(const std::wstring& finalPath,
                     const std::string& data,
                     std::wstring& error) {
    return AtomicWriteFile(finalPath,
                           reinterpret_cast<const BYTE*>(data.data()),
                           data.size(), error);
}

ULONGLONG FileTimeValue(const FILETIME& time) {
    ULARGE_INTEGER value = {};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}

uint64_t FileSizeValue(const WIN32_FIND_DATAW& data) {
    ULARGE_INTEGER value = {};
    value.LowPart = data.nFileSizeLow;
    value.HighPart = data.nFileSizeHigh;
    return value.QuadPart;
}

bool IsDigitRange(const std::wstring& value, size_t offset, size_t count) {
    if (offset + count > value.size()) return false;
    for (size_t i = offset; i < offset + count; ++i) {
        if (value[i] < L'0' || value[i] > L'9') return false;
    }
    return true;
}

bool ManagedResultName(const std::wstring& value) {
    constexpr const wchar_t* kResultNames[] = {
        L"usable_text",
        L"no_speech",
        L"too_short",
        L"operational_error",
        L"cancelled",
        L"capture_failure",
    };
    for (const wchar_t* resultName : kResultNames) {
        const std::wstring base(resultName);
        if (value == base) return true;
        const std::wstring duplicatePrefix = base + L"-dup";
        if (value.size() <= duplicatePrefix.size() ||
            value.compare(0, duplicatePrefix.size(), duplicatePrefix) != 0) {
            continue;
        }
        const size_t digits = value.size() - duplicatePrefix.size();
        const wchar_t firstDigit = value[duplicatePrefix.size()];
        if (firstDigit != L'0' &&
            !(digits == 1 && firstDigit < L'2') &&
            IsDigitRange(value, duplicatePrefix.size(), digits)) {
            return true;
        }
    }
    return false;
}

bool ManagedManifestName(const std::wstring& name, std::wstring* prefix) {
    if (name.size() < 34 || name.substr(name.size() - 5) != L".json") return false;
    if (!IsDigitRange(name, 0, 8) || name[8] != L'-' ||
        !IsDigitRange(name, 9, 6) || name[15] != L'.' ||
        !IsDigitRange(name, 16, 3) || name.substr(19, 8) != L"-attempt") {
        return false;
    }
    size_t position = 27;
    const size_t attemptStart = position;
    while (position < name.size() && name[position] >= L'0' && name[position] <= L'9') {
        ++position;
    }
    if (position == attemptStart || position >= name.size() || name[position] != L'-') {
        return false;
    }
    const std::wstring resultName = name.substr(
        position + 1, name.size() - 5 - (position + 1));
    if (!ManagedResultName(resultName)) return false;
    if (prefix) *prefix = name.substr(0, name.size() - 5);
    return true;
}

bool ManagedAudioName(const std::wstring& name, std::wstring* prefix) {
    if (name.size() < 5 || name.substr(name.size() - 4) != L".wav") {
        return false;
    }

    size_t baseLength = std::wstring::npos;
    constexpr wchar_t kCaptureSuffix[] = L"-capture.wav";
    if (name.size() > std::size(kCaptureSuffix) - 1 &&
        name.compare(name.size() - (std::size(kCaptureSuffix) - 1),
                     std::size(kCaptureSuffix) - 1,
                     kCaptureSuffix) == 0) {
        baseLength = name.size() - (std::size(kCaptureSuffix) - 1);
    } else {
        const size_t extension = name.size() - 4;
        const size_t marker = name.rfind(L"-input", extension);
        if (marker == std::wstring::npos || marker + 6 >= extension) {
            return false;
        }
        for (size_t i = marker + 6; i < extension; ++i) {
            if (name[i] < L'0' || name[i] > L'9') return false;
        }
        baseLength = marker;
    }

    const std::wstring baseName = name.substr(0, baseLength);
    std::wstring manifestPrefix;
    if (!ManagedManifestName(baseName + L".json", &manifestPrefix) ||
        manifestPrefix != baseName) {
        return false;
    }
    if (prefix) *prefix = baseName;
    return true;
}

std::vector<ManagedGroup> EnumerateManagedGroups(const std::wstring& directory) {
    std::vector<ManagedGroup> groups;
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW((directory + L"\\*.json").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return groups;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring prefix;
        if (!ManagedManifestName(data.cFileName, &prefix)) continue;
        ManagedGroup group;
        group.manifestName = data.cFileName;
        group.prefix = std::move(prefix);
        group.writeTime = FileTimeValue(data.ftLastWriteTime);
        group.bytes = FileSizeValue(data);

        WIN32_FIND_DATAW audio = {};
        HANDLE audioSearch = FindFirstFileW(
            (directory + L"\\" + group.prefix + L"-*.wav").c_str(), &audio);
        if (audioSearch != INVALID_HANDLE_VALUE) {
            do {
                std::wstring audioPrefix;
                if (!(audio.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                    ManagedAudioName(audio.cFileName, &audioPrefix) &&
                    audioPrefix == group.prefix) {
                    group.bytes += FileSizeValue(audio);
                }
            } while (FindNextFileW(audioSearch, &audio));
            FindClose(audioSearch);
        }
        groups.push_back(std::move(group));
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return groups;
}

bool DeleteManagedGroup(const std::wstring& directory,
                        const ManagedGroup& group,
                        std::wstring* error) {
    bool ok = true;
    WIN32_FIND_DATAW audio = {};
    HANDLE search = FindFirstFileW(
        (directory + L"\\" + group.prefix + L"-*.wav").c_str(), &audio);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (audio.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring audioPrefix;
            if (!ManagedAudioName(audio.cFileName, &audioPrefix) ||
                audioPrefix != group.prefix) {
                continue;
            }
            const std::wstring path = directory + L"\\" + audio.cFileName;
            if (!DeleteFileW(path.c_str())) ok = false;
        } while (FindNextFileW(search, &audio));
        FindClose(search);
    }
    const std::wstring manifest = directory + L"\\" + group.manifestName;
    if (!DeleteFileW(manifest.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) {
        ok = false;
    }
    if (!ok && error) {
        *error = L"Unable to delete one or more managed diagnostic files.";
    }
    return ok;
}

void CleanupOldManagedTemps(const std::wstring& directory) {
    FILETIME nowFileTime = {};
    GetSystemTimeAsFileTime(&nowFileTime);
    const ULONGLONG now = FileTimeValue(nowFileTime);
    const ULONGLONG oneDay = kFileTimeTicksPerDay;
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW((directory + L"\\*.tmp").c_str(), &data);
    if (search == INVALID_HANDLE_VALUE) return;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring fileName = data.cFileName;
        if (fileName.size() <= 4) continue;
        fileName.resize(fileName.size() - 4);
        const std::wstring finalName = fileName;
        std::wstring candidatePrefix;
        bool managed = false;
        if (finalName.size() >= 5 &&
            finalName.substr(finalName.size() - 5) == L".json") {
            managed = ManagedManifestName(finalName, &candidatePrefix);
        } else {
            managed = ManagedAudioName(finalName, &candidatePrefix);
        }
        const ULONGLONG written = FileTimeValue(data.ftLastWriteTime);
        if (managed && now > written && now - written > oneDay) {
            DeleteFileW((directory + L"\\" + data.cFileName).c_str());
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
}

void ApplyRetention(const std::wstring& directory) {
    CleanupOldManagedTemps(directory);
    std::vector<ManagedGroup> groups = EnumerateManagedGroups(directory);
    std::sort(groups.begin(), groups.end(), [](const ManagedGroup& left,
                                                const ManagedGroup& right) {
        return left.writeTime < right.writeTime;
    });

    FILETIME nowFileTime = {};
    GetSystemTimeAsFileTime(&nowFileTime);
    const ULONGLONG now = FileTimeValue(nowFileTime);
    const ULONGLONG maxAge = static_cast<ULONGLONG>(kRetentionMaxAgeDays) *
                             kFileTimeTicksPerDay;
    uint64_t totalBytes = 0;
    size_t liveGroups = groups.size();
    std::vector<bool> deleted(groups.size(), false);
    for (size_t i = 0; i < groups.size(); ++i) {
        totalBytes += groups[i].bytes;
        if (now > groups[i].writeTime && now - groups[i].writeTime > maxAge) {
            if (DeleteManagedGroup(directory, groups[i], nullptr)) {
                deleted[i] = true;
                --liveGroups;
                totalBytes -= groups[i].bytes;
            }
        }
    }
    for (size_t i = 0; i < groups.size() &&
         (liveGroups > kRetentionMaxGroups || totalBytes > kRetentionMaxBytes); ++i) {
        if (deleted[i]) continue;
        if (DeleteManagedGroup(directory, groups[i], nullptr)) {
            deleted[i] = true;
            --liveGroups;
            totalBytes -= groups[i].bytes;
        }
    }
}

std::string IsoUtc(const SYSTEMTIME& time) {
    char buffer[64] = {};
    sprintf_s(buffer, "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
              time.wYear, time.wMonth, time.wDay,
              time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return buffer;
}

std::string Number(double value, int precision = 3) {
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(precision) << value;
    return stream.str();
}

std::string BuildJson(const AttemptState& attempt,
                      const std::wstring& baseName,
                      const std::vector<Artifact>& artifacts,
                      const std::map<std::pair<StageKind, unsigned>, std::string>& stageArtifacts) {
    const std::string deviceIdUtf8 = WideToUtf8Local(attempt.capture.device.deviceId);
    const std::string deviceHash = Sha256Hex(
        reinterpret_cast<const BYTE*>(deviceIdUtf8.data()), deviceIdUtf8.size());
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": 1,\n"
         << "  \"capture_id\": \"" << JsonEscape(attempt.captureId) << "\",\n"
         << "  \"timestamp_utc\": \"" << IsoUtc(attempt.utcTime) << "\",\n"
         << "  \"app_version\": \"" << APP_VERSION_FULL_STR << "\",\n"
         << "  \"attempt_id\": " << attempt.metadata.attemptId << ",\n"
         << "  \"backend\": \"" << JsonEscape(attempt.metadata.backend) << "\",\n"
         << "  \"model\": \"" << JsonEscape(attempt.metadata.model) << "\",\n"
         << "  \"transport\": \"" << JsonEscape(attempt.metadata.transport) << "\",\n"
         << "  \"result_kind\": \"" << FinalKindName(attempt.finalResult.kind) << "\",\n"
         << "  \"result_reason\": \"" << JsonEscape(attempt.finalResult.reason) << "\",\n"
         << "  \"capture\": {\n"
         << "    \"backend\": \"" << JsonEscape(attempt.capture.device.backend) << "\",\n"
         << "    \"device_name\": \"" << JsonEscape(attempt.capture.device.deviceName) << "\",\n"
         << "    \"device_id_sha256\": \"" << deviceHash << "\",\n"
         << "    \"used_default_device\": " << (attempt.capture.device.usedDefaultDevice ? "true" : "false") << ",\n"
         << "    \"native_rate\": " << attempt.capture.device.nativeSampleRate << ",\n"
         << "    \"native_channels\": " << attempt.capture.device.nativeChannels << ",\n"
         << "    \"native_bits\": " << attempt.capture.device.nativeBitsPerSample << ",\n"
         << "    \"native_float\": " << (attempt.capture.device.nativeIsFloat ? "true" : "false") << ",\n"
         << "    \"recording_ms\": " << Number(attempt.capture.recordingMs, 0) << ",\n"
         << "    \"native_frames\": " << attempt.capture.nativeFrames << ",\n"
         << "    \"output_samples\": " << attempt.capture.outputSamples << ",\n"
         << "    \"pcm_bytes\": " << attempt.capture.pcmBytes << ",\n"
         << "    \"rms_dbfs\": " << Number(attempt.capture.output.rmsDbfs) << ",\n"
         << "    \"peak_dbfs\": " << Number(attempt.capture.output.peakDbfs) << ",\n"
         << "    \"zero_ratio\": " << Number(attempt.capture.output.zeroRatio, 6) << ",\n"
         << "    \"near_silent_ratio\": " << Number(attempt.capture.output.nearSilentRatio, 6) << ",\n"
         << "    \"clipping_ratio\": " << Number(attempt.capture.output.clippingRatio, 6) << ",\n"
         << "    \"silent_packets\": " << attempt.capture.silentPackets << ",\n"
         << "    \"silent_frames\": " << attempt.capture.silentFrames << ",\n"
         << "    \"discontinuities\": " << attempt.capture.discontinuities << ",\n"
         << "    \"max_callback_gap_ms\": " << Number(attempt.capture.maxCallbackGapMs) << ",\n"
         << "    \"first_non_silent_delay_ms\": " << Number(attempt.capture.firstNonSilentDelayMs) << ",\n"
         << "    \"downmix_rms_dbfs\": " << Number(attempt.capture.downmixRmsDbfs) << ",\n"
         << "    \"native_channel_rms_dbfs\": [";
    for (size_t i = 0; i < attempt.capture.nativeChannelRmsDbfs.size(); ++i) {
        if (i) json << ", ";
        json << Number(attempt.capture.nativeChannelRmsDbfs[i]);
    }
    json << "]\n  },\n"
         << "  \"audio_artifacts\": [\n";
    for (size_t i = 0; i < artifacts.size(); ++i) {
        const Artifact& artifact = artifacts[i];
        json << "    {\"id\": \"" << artifact.id << "\", \"file\": \""
             << JsonEscape(artifact.fileName) << "\", \"pcm_bytes\": "
             << (artifact.pcm ? artifact.pcm->size() : 0) << ", \"sha256\": \""
             << artifact.sha256 << "\"}" << (i + 1 == artifacts.size() ? "\n" : ",\n");
    }
    json << "  ],\n  \"stages\": [\n";
    for (size_t i = 0; i < attempt.stages.size(); ++i) {
        const StageState& stage = attempt.stages[i];
        const auto artifactIt = stageArtifacts.find({stage.metadata.kind, stage.metadata.index});
        json << "    {\n"
             << "      \"kind\": \"" << StageKindName(stage.metadata.kind) << "\",\n"
             << "      \"index\": " << stage.metadata.index << ",\n"
             << "      \"backend\": \"" << JsonEscape(stage.metadata.backend) << "\",\n"
             << "      \"model\": \"" << JsonEscape(stage.metadata.model) << "\",\n"
             << "      \"transport\": \"" << JsonEscape(stage.metadata.transport) << "\",\n"
             << "      \"reason\": \"" << JsonEscape(stage.metadata.reason) << "\",\n"
             << "      \"encoding\": \"" << JsonEscape(stage.metadata.encoding) << "\",\n"
             << "      \"input_artifact\": \""
             << (artifactIt == stageArtifacts.end() ? "" : artifactIt->second) << "\",\n"
             << "      \"input_bytes\": " << (stage.pcm ? stage.pcm->size() : 0) << ",\n"
             << "      \"sent_bytes\": " << stage.metadata.sentBytes << ",\n"
             << "      \"network_bytes\": " << stage.metadata.networkBytes << ",\n"
             << "      \"vad_enabled\": " << (stage.metadata.vadEnabled ? "true" : "false") << ",\n"
             << "      \"vad_active\": " << (stage.metadata.vadActive ? "true" : "false") << ",\n"
             << "      \"vad_detected_speech\": " << (stage.metadata.vadDetectedSpeech ? "true" : "false") << ",\n"
             << "      \"vad_model\": \"" << JsonEscape(stage.metadata.vadModel) << "\",\n"
             << "      \"vad_input_bytes\": " << stage.metadata.vadInputBytes << ",\n"
             << "      \"vad_output_bytes\": " << stage.metadata.vadOutputBytes << ",\n"
             << "      \"terminal\": \"" << JsonEscape(stage.terminal.terminal) << "\",\n"
             << "      \"terminal_reason\": \"" << JsonEscape(stage.terminal.reason) << "\",\n"
             << "      \"provider_code\": \"" << JsonEscape(stage.terminal.providerCode) << "\",\n"
             << "      \"error_category\": \"" << JsonEscape(stage.terminal.errorCategory) << "\",\n"
             << "      \"text_chars\": " << stage.terminal.textChars << ",\n"
             << "      \"committed_text_chars\": " << stage.terminal.committedTextChars << ",\n"
             << "      \"elapsed_ms\": " << Number(stage.terminal.elapsedMs) << "\n"
             << "    }" << (i + 1 == attempt.stages.size() ? "\n" : ",\n");
    }
    json << "  ]\n}\n";
    return json.str();
}

void SaveAttempt(AttemptState attempt, SaveDecision decision) {
    std::lock_guard<std::mutex> fileLock(g_fileIoMutex);
    std::wstring directory;
    std::wstring error;
    if (!EnsureDiagnosticAudioDir(&error)) {
        EmitLog("event=diagnostic_audio_save_failed attempt=" +
                std::to_string(attempt.metadata.attemptId) +
                " step=create_directory error_chars=" +
                std::to_string(error.size()));
        return;
    }
    directory = DiagnosticAudioDir();

    const std::string resultName = SanitizeAscii(FinalKindName(attempt.finalResult.kind));
    std::wstring baseName = attempt.captureId + L"-" + Utf8ToWideLocal(resultName);
    for (unsigned duplicate = 2;
         PathExists(directory + L"\\" + baseName + L".json"); ++duplicate) {
        baseName = attempt.captureId + L"-" + Utf8ToWideLocal(resultName) +
                   L"-dup" + std::to_wstring(duplicate);
    }

    std::vector<Artifact> artifacts;
    std::map<std::string, std::string> hashToId;
    std::map<std::pair<StageKind, unsigned>, std::string> stageArtifacts;
    if (attempt.capturePcm && !attempt.capturePcm->empty()) {
        Artifact capture;
        capture.id = "capture";
        capture.fileName = baseName + L"-capture.wav";
        capture.pcm = attempt.capturePcm;
        capture.sha256 = Sha256Hex(capture.pcm->data(), capture.pcm->size());
        hashToId[capture.sha256] = capture.id;
        artifacts.push_back(std::move(capture));
    }

    unsigned inputIndex = 1;
    for (const StageState& stage : attempt.stages) {
        if (!stage.pcm || stage.pcm->empty()) continue;
        const std::string sha = Sha256Hex(stage.pcm->data(), stage.pcm->size());
        auto known = hashToId.find(sha);
        if (known != hashToId.end()) {
            stageArtifacts[{stage.metadata.kind, stage.metadata.index}] = known->second;
            continue;
        }
        char idBuffer[24] = {};
        sprintf_s(idBuffer, "input%02u", inputIndex++);
        Artifact artifact;
        artifact.id = idBuffer;
        artifact.fileName = baseName + L"-" + Utf8ToWideLocal(artifact.id) + L".wav";
        artifact.pcm = stage.pcm;
        artifact.sha256 = sha;
        hashToId[sha] = artifact.id;
        stageArtifacts[{stage.metadata.kind, stage.metadata.index}] = artifact.id;
        artifacts.push_back(std::move(artifact));
    }

    const auto reportReplayMismatch = [&](StageKind kind,
                                          unsigned index,
                                          StageKind parentKind,
                                          unsigned parentIndex) {
        const auto replay = stageArtifacts.find({kind, index});
        const auto parent = stageArtifacts.find({parentKind, parentIndex});
        if (replay == stageArtifacts.end() || parent == stageArtifacts.end() ||
            replay->second == parent->second) {
            return;
        }
        EmitLog("event=diagnostic_audio_replay_mismatch attempt=" +
                std::to_string(attempt.metadata.attemptId) +
                " kind=" + StageKindName(kind) +
                " retry_index=" + std::to_string(index) +
                " parent_artifact=" + parent->second +
                " replay_artifact=" + replay->second);
    };
    for (const StageState& stage : attempt.stages) {
        if (stage.metadata.kind == StageKind::InternalRetry) {
            reportReplayMismatch(StageKind::InternalRetry, stage.metadata.index,
                                 StageKind::Primary, 0);
        } else if (stage.metadata.kind == StageKind::Fallback &&
                   stage.metadata.index > 0) {
            reportReplayMismatch(StageKind::Fallback, stage.metadata.index,
                                 StageKind::Fallback, 0);
        }
    }

    std::vector<std::wstring> committedFiles;
    bool success = true;
    uint64_t writtenBytes = 0;
    for (const Artifact& artifact : artifacts) {
        const std::vector<BYTE> wav = BuildPcm16MonoWav(
            artifact.pcm ? artifact.pcm->data() : nullptr,
            artifact.pcm ? artifact.pcm->size() : 0);
        const std::wstring path = directory + L"\\" + artifact.fileName;
        if (!AtomicWriteFile(path, wav.data(), wav.size(), error)) {
            success = false;
            break;
        }
        committedFiles.push_back(path);
        writtenBytes += wav.size();
    }

    const std::string json = BuildJson(attempt, baseName, artifacts, stageArtifacts);
    const std::wstring manifestName = baseName + L".json";
    const std::wstring manifestPath = directory + L"\\" + manifestName;
    if (success && AtomicWriteFile(manifestPath, json, error)) {
        committedFiles.push_back(manifestPath);
        writtenBytes += json.size();
        EmitLog("event=diagnostic_audio_saved attempt=" +
                std::to_string(attempt.metadata.attemptId) +
                " reason=" + SanitizeAscii(decision.reason) +
                " artifacts=" + std::to_string(artifacts.size()) +
                " bytes=" + std::to_string(writtenBytes) +
                " manifest=" + WideToUtf8Local(manifestName));
        ApplyRetention(directory);
        return;
    }

    for (const std::wstring& path : committedFiles) DeleteFileW(path.c_str());
    EmitLog("event=diagnostic_audio_save_failed attempt=" +
            std::to_string(attempt.metadata.attemptId) +
            " error_chars=" + std::to_string(error.size()));
}

void WriterFinished() {
    std::lock_guard<std::mutex> lock(g_writerMutex);
    if (g_pendingWriters > 0) --g_pendingWriters;
    g_writerCv.notify_all();
}

} // namespace

const char* StageKindName(StageKind kind) {
    switch (kind) {
    case StageKind::Primary: return "primary";
    case StageKind::InternalRetry: return "internal_retry";
    case StageKind::Fallback: return "fallback";
    }
    return "unknown";
}

StageKind RetryStageKind(StageKind parentKind) {
    return parentKind == StageKind::Fallback
        ? StageKind::Fallback
        : StageKind::InternalRetry;
}

unsigned RetryStageIndex(StageKind parentKind,
                         unsigned parentIndex,
                         unsigned retryIndex) {
    return parentKind == StageKind::Fallback
        ? parentIndex + retryIndex
        : retryIndex;
}

const char* FinalKindName(FinalKind kind) {
    switch (kind) {
    case FinalKind::UsableText: return "usable_text";
    case FinalKind::NoSpeech: return "no_speech";
    case FinalKind::TooShort: return "too_short";
    case FinalKind::OperationalError: return "operational_error";
    case FinalKind::Cancelled: return "cancelled";
    case FinalKind::CaptureFailure: return "capture_failure";
    }
    return "unknown";
}

void SetLogCallback(LogCallback callback) {
    g_logCallback.store(callback, std::memory_order_release);
}

std::wstring NormalizeMode(std::wstring mode) {
    std::transform(mode.begin(), mode.end(), mode.begin(), [](wchar_t ch) {
        if (ch >= L'A' && ch <= L'Z') return static_cast<wchar_t>(ch - L'A' + L'a');
        return ch;
    });
    if (mode != L"failures" && mode != L"all") return L"off";
    return mode;
}

std::wstring DiagnosticAudioDir() {
    std::lock_guard<std::mutex> lock(g_directoryMutex);
    return g_directoryOverride.empty() ? DefaultDiagnosticDir() : g_directoryOverride;
}

bool EnsureDiagnosticAudioDir(std::wstring* error) {
    return EnsureDirectoryPath(DiagnosticAudioDir(), error);
}

bool DeleteManagedRecordings(size_t* deletedGroups, std::wstring* error) {
    std::lock_guard<std::mutex> fileLock(g_fileIoMutex);
    if (deletedGroups) *deletedGroups = 0;
    const std::wstring directory = DiagnosticAudioDir();
    if (!PathExists(directory)) return true;
    std::vector<ManagedGroup> groups = EnumerateManagedGroups(directory);
    size_t deleted = 0;
    bool ok = true;
    for (const ManagedGroup& group : groups) {
        if (DeleteManagedGroup(directory, group, error)) ++deleted;
        else ok = false;
    }
    if (deletedGroups) *deletedGroups = deleted;
    return ok;
}

void ResetCapture(const CaptureDeviceInfo& device) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    g_capture = {};
    g_capture.active = true;
    g_capture.device = device;
    g_capture.startedTick = GetTickCount64();
}

void SetCaptureDeviceInfo(const CaptureDeviceInfo& device) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (g_capture.active) g_capture.device = device;
}

void RecordWasapiFloatPacket(const float* interleaved,
                             UINT32 frames,
                             UINT32 channels,
                             const float* downmixedMono,
                             bool silentPacket) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_capture.active || frames == 0) return;
    g_capture.nativeFrames += frames;
    if (silentPacket) {
        ++g_capture.silentPackets;
        g_capture.silentFrames += frames;
        NoteCallbackLocked(false);
        return;
    }
    if (channels == 0) channels = 1;
    if (g_capture.nativeChannelSquares.size() < channels) {
        g_capture.nativeChannelSquares.resize(channels);
        g_capture.nativeChannelCounts.resize(channels);
    }
    double monoPeak = 0.0;
    for (UINT32 frame = 0; frame < frames; ++frame) {
        for (UINT32 channel = 0; channel < channels; ++channel) {
            double value = interleaved ? interleaved[frame * channels + channel] : 0.0;
            if (!std::isfinite(value)) value = 0.0;
            value = std::clamp(value, -1.0, 1.0);
            g_capture.nativeChannelSquares[channel] += static_cast<long double>(value) * value;
            ++g_capture.nativeChannelCounts[channel];
        }
        const double mono = downmixedMono ? std::clamp<double>(downmixedMono[frame], -1.0, 1.0) : 0.0;
        g_capture.downmixSquares += static_cast<long double>(mono) * mono;
        ++g_capture.downmixCount;
        monoPeak = (std::max)(monoPeak, std::abs(mono));
    }
    NoteCallbackLocked(monoPeak >= kNonSilentPacketThreshold);
}

void RecordWasapiPcm16Packet(const int16_t* interleaved,
                             UINT32 frames,
                             UINT32 channels,
                             const float* downmixedMono,
                             bool silentPacket) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_capture.active || frames == 0) return;
    g_capture.nativeFrames += frames;
    if (silentPacket) {
        ++g_capture.silentPackets;
        g_capture.silentFrames += frames;
        NoteCallbackLocked(false);
        return;
    }
    if (channels == 0) channels = 1;
    if (g_capture.nativeChannelSquares.size() < channels) {
        g_capture.nativeChannelSquares.resize(channels);
        g_capture.nativeChannelCounts.resize(channels);
    }
    double monoPeak = 0.0;
    for (UINT32 frame = 0; frame < frames; ++frame) {
        for (UINT32 channel = 0; channel < channels; ++channel) {
            const double value = interleaved
                ? static_cast<double>(interleaved[frame * channels + channel]) / 32768.0
                : 0.0;
            g_capture.nativeChannelSquares[channel] += static_cast<long double>(value) * value;
            ++g_capture.nativeChannelCounts[channel];
        }
        const double mono = downmixedMono ? std::clamp<double>(downmixedMono[frame], -1.0, 1.0) : 0.0;
        g_capture.downmixSquares += static_cast<long double>(mono) * mono;
        ++g_capture.downmixCount;
        monoPeak = (std::max)(monoPeak, std::abs(mono));
    }
    NoteCallbackLocked(monoPeak >= kNonSilentPacketThreshold);
}

void RecordWaveInPcm16(const BYTE* data, size_t bytes) {
    if (!data || bytes < sizeof(int16_t)) return;
    bytes -= bytes % sizeof(int16_t);
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const size_t count = bytes / sizeof(int16_t);
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_capture.active) return;
    if (g_capture.nativeChannelSquares.empty()) {
        g_capture.nativeChannelSquares.resize(1);
        g_capture.nativeChannelCounts.resize(1);
    }
    double peak = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double value = static_cast<double>(samples[i]) / 32768.0;
        g_capture.nativeChannelSquares[0] += static_cast<long double>(value) * value;
        ++g_capture.nativeChannelCounts[0];
        g_capture.downmixSquares += static_cast<long double>(value) * value;
        ++g_capture.downmixCount;
        AddSignalSample(g_capture.output, value);
        peak = (std::max)(peak, std::abs(value));
    }
    g_capture.nativeFrames += count;
    NoteCallbackLocked(peak >= kNonSilentPacketThreshold);
}

void RecordOutputPcm16(const BYTE* data, size_t bytes) {
    if (!data || bytes < sizeof(int16_t)) return;
    bytes -= bytes % sizeof(int16_t);
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const size_t count = bytes / sizeof(int16_t);
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (!g_capture.active) return;
    for (size_t i = 0; i < count; ++i) {
        AddSignalSample(g_capture.output,
                        static_cast<double>(samples[i]) / 32768.0);
    }
}

void RecordCaptureDiscontinuity() {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    if (g_capture.active) ++g_capture.discontinuities;
}

CaptureSnapshot FreezeCapture(double recordingMs, size_t pcmBytes) {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    CaptureSnapshot result;
    result.device = g_capture.device;
    result.nativeFrames = g_capture.nativeFrames;
    result.outputSamples = g_capture.output.count;
    result.pcmBytes = pcmBytes;
    result.recordingMs = recordingMs;
    result.output = FinishSignal(g_capture.output);
    result.silentPackets = g_capture.silentPackets;
    result.silentFrames = g_capture.silentFrames;
    result.discontinuities = g_capture.discontinuities;
    result.maxCallbackGapMs = g_capture.maxCallbackGapMs;
    result.firstNonSilentDelayMs = g_capture.firstNonSilentDelayMs;
    for (size_t i = 0; i < g_capture.nativeChannelSquares.size(); ++i) {
        const uint64_t count = i < g_capture.nativeChannelCounts.size()
            ? g_capture.nativeChannelCounts[i] : 0;
        const double rms = count == 0 ? 0.0 : std::sqrt(static_cast<double>(
            g_capture.nativeChannelSquares[i] / static_cast<long double>(count)));
        result.nativeChannelRmsDbfs.push_back(LinearToDbfs(rms));
    }
    const double downmixRms = g_capture.downmixCount == 0 ? 0.0 :
        std::sqrt(static_cast<double>(g_capture.downmixSquares /
                  static_cast<long double>(g_capture.downmixCount)));
    result.downmixRmsDbfs = LinearToDbfs(downmixRms);
    g_capture.active = false;
    return result;
}

void CancelCapture() {
    std::lock_guard<std::mutex> lock(g_captureMutex);
    g_capture = {};
}

void BeginAttempt(const AttemptMetadata& metadata) {
    if (metadata.attemptId == 0) return;
    AttemptState attempt;
    attempt.metadata = metadata;
    attempt.metadata.mode = NormalizeMode(attempt.metadata.mode);
    if (attempt.metadata.mode == L"off") return;
    GetSystemTime(&attempt.utcTime);
    GetLocalTime(&attempt.localTime);
    attempt.captureId = CaptureId(attempt.localTime, metadata.attemptId);
    std::lock_guard<std::mutex> lock(g_attemptMutex);
    g_attempts[metadata.attemptId] = std::move(attempt);
}

void AttachCapture(uint64_t attemptId,
                   std::shared_ptr<const std::vector<BYTE>> pcm,
                   const CaptureSnapshot& capture) {
    std::lock_guard<std::mutex> lock(g_attemptMutex);
    auto found = g_attempts.find(attemptId);
    if (found == g_attempts.end()) return;
    found->second.capturePcm = std::move(pcm);
    found->second.capture = capture;
    if (found->second.capturePcm) {
        found->second.capture.pcmBytes = found->second.capturePcm->size();
        if (found->second.capture.output.sampleCount == 0) {
            found->second.capture.output = CalculatePcm16Metrics(
                found->second.capturePcm->data(), found->second.capturePcm->size());
            found->second.capture.outputSamples = found->second.capture.output.sampleCount;
        }
    }
}

void RegisterStageInput(uint64_t attemptId,
                        const StageMetadata& metadata,
                        const std::vector<BYTE>& pcm) {
    std::lock_guard<std::mutex> lock(g_attemptMutex);
    auto found = g_attempts.find(attemptId);
    if (found == g_attempts.end()) return;
    StageState& stage = FindOrCreateStage(found->second, metadata);
    stage.pcm = std::make_shared<std::vector<BYTE>>(pcm);
    if (stage.metadata.sentBytes == 0) stage.metadata.sentBytes = pcm.size();
}

void AppendStageInput(uint64_t attemptId,
                      const StageMetadata& metadata,
                      const BYTE* data,
                      size_t bytes,
                      size_t networkBytes) {
    if (!data || bytes == 0) return;
    std::lock_guard<std::mutex> lock(g_attemptMutex);
    auto found = g_attempts.find(attemptId);
    if (found == g_attempts.end()) return;
    StageState& stage = FindOrCreateStage(found->second, metadata);
    if (!stage.pcm) stage.pcm = std::make_shared<std::vector<BYTE>>();
    stage.pcm->insert(stage.pcm->end(), data, data + bytes);
    stage.metadata.sentBytes += bytes;
    stage.metadata.networkBytes += networkBytes;
}

void UpdateStageMetadata(uint64_t attemptId,
                         const StageMetadata& metadata) {
    std::lock_guard<std::mutex> lock(g_attemptMutex);
    auto found = g_attempts.find(attemptId);
    if (found == g_attempts.end()) return;
    FindOrCreateStage(found->second, metadata);
}

void CompleteStage(uint64_t attemptId,
                   StageKind kind,
                   unsigned index,
                   const StageTerminal& terminal) {
    StageMetadata loggedMetadata;
    size_t inputBytes = 0;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_attemptMutex);
        auto found = g_attempts.find(attemptId);
        if (found == g_attempts.end()) return;
        StageMetadata metadata;
        metadata.kind = kind;
        metadata.index = index;
        StageState& stage = FindOrCreateStage(found->second, metadata);
        changed = !stage.completed ||
            stage.terminal.terminal != terminal.terminal ||
            stage.terminal.reason != terminal.reason ||
            stage.terminal.providerCode != terminal.providerCode ||
            stage.terminal.textChars != terminal.textChars ||
            stage.terminal.committedTextChars != terminal.committedTextChars;
        stage.terminal = terminal;
        stage.completed = true;
        loggedMetadata = stage.metadata;
        inputBytes = stage.pcm ? stage.pcm->size() : 0;
    }
    if (changed) {
        EmitStageTerminalLog(attemptId, loggedMetadata, terminal, inputBytes);
    }
}

void CompleteStageIfMissing(uint64_t attemptId,
                            const StageMetadata& metadata,
                            const StageTerminal& terminal) {
    StageMetadata loggedMetadata;
    size_t inputBytes = 0;
    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(g_attemptMutex);
        auto found = g_attempts.find(attemptId);
        if (found == g_attempts.end()) return;
        StageState& stage = FindOrCreateStage(found->second, metadata);
        if (!stage.completed) {
            stage.terminal = terminal;
            stage.completed = true;
            loggedMetadata = stage.metadata;
            inputBytes = stage.pcm ? stage.pcm->size() : 0;
            completed = true;
        }
    }
    if (completed) {
        EmitStageTerminalLog(attemptId, loggedMetadata, terminal, inputBytes);
    }
}

SaveDecision FinalizeAttempt(uint64_t attemptId, const FinalResult& result) {
    // Close handles for writers that finished between recordings. The final
    // shutdown drain still joins any overlapping writers that remain active.
    WaitForPendingWrites(0);

    AttemptState attempt;
    {
        std::lock_guard<std::mutex> lock(g_attemptMutex);
        auto found = g_attempts.find(attemptId);
        if (found == g_attempts.end()) return {false, "attempt_not_found"};
        found->second.finalResult = result;
        attempt = std::move(found->second);
        g_attempts.erase(found);
    }

    const size_t pcmBytes = attempt.capturePcm ? attempt.capturePcm->size() : 0;
    const bool contradictory = HasContradictoryStages(attempt);
    const SaveDecision decision = ShouldSaveDiagnosticAudio(
        attempt.metadata.mode, result, pcmBytes,
        attempt.capture.recordingMs, contradictory);
    EmitLog("event=diagnostic_audio_decision attempt=" +
            std::to_string(attemptId) +
            " mode=" + WideToUtf8Local(NormalizeMode(attempt.metadata.mode)) +
            " save=" + (decision.save ? "1" : "0") +
            " reason=" + SanitizeAscii(decision.reason) +
            " pcm_bytes=" + std::to_string(pcmBytes));
    if (!decision.save) return decision;

    {
        std::lock_guard<std::mutex> lock(g_writerMutex);
        ++g_pendingWriters;
    }
    try {
        std::lock_guard<std::mutex> lock(g_writerMutex);
        g_writerThreads.emplace_back([attempt = std::move(attempt), decision]() mutable {
            const uint64_t writerAttemptId = attempt.metadata.attemptId;
            try {
                SaveAttempt(std::move(attempt), decision);
            } catch (...) {
                EmitLog("event=diagnostic_audio_save_failed attempt=" +
                        std::to_string(writerAttemptId) +
                        " step=writer_exception");
            }
            WriterFinished();
        });
    } catch (...) {
        WriterFinished();
        EmitLog("event=diagnostic_audio_save_failed attempt=" +
                std::to_string(attemptId) + " step=start_writer");
        return {false, "writer_start_failed"};
    }
    return decision;
}

void DiscardAttempt(uint64_t attemptId, const char* reason) {
    bool erased = false;
    {
        std::lock_guard<std::mutex> lock(g_attemptMutex);
        erased = g_attempts.erase(attemptId) != 0;
    }
    if (erased) {
        EmitLog("event=diagnostic_audio_decision attempt=" +
                std::to_string(attemptId) + " save=0 reason=" +
                SanitizeAscii(reason ? reason : "discarded"));
    }
}

bool WaitForPendingWrites(DWORD timeoutMs) {
    std::vector<std::thread> writers;
    {
        std::unique_lock<std::mutex> lock(g_writerMutex);
        bool completed = false;
        if (timeoutMs == INFINITE) {
            g_writerCv.wait(lock, [] { return g_pendingWriters == 0; });
            completed = true;
        } else {
            completed = g_writerCv.wait_for(
                lock, std::chrono::milliseconds(timeoutMs), [] {
                    return g_pendingWriters == 0;
                });
        }
        if (!completed) return false;
        writers.swap(g_writerThreads);
    }
    for (std::thread& writer : writers) {
        if (writer.joinable()) writer.join();
    }
    return true;
}

Pcm16Metrics CalculatePcm16Metrics(const BYTE* data, size_t bytes) {
    SignalAccumulator accumulator;
    if (!data || bytes < sizeof(int16_t)) return FinishSignal(accumulator);
    bytes -= bytes % sizeof(int16_t);
    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const size_t count = bytes / sizeof(int16_t);
    for (size_t i = 0; i < count; ++i) {
        AddSignalSample(accumulator,
                        static_cast<double>(samples[i]) / 32768.0);
    }
    return FinishSignal(accumulator);
}

std::vector<BYTE> BuildPcm16MonoWav(const BYTE* data, size_t bytes) {
    bytes -= bytes % sizeof(int16_t);
    if (bytes > UINT32_MAX - 44u) return {};
    std::vector<BYTE> output(44 + bytes, 0);
    std::memcpy(output.data(), "RIFF", 4);
    PutLe32(output, 4, static_cast<uint32_t>(36 + bytes));
    std::memcpy(output.data() + 8, "WAVEfmt ", 8);
    PutLe32(output, 16, 16);
    PutLe16(output, 20, 1);
    PutLe16(output, 22, kTargetChannels);
    PutLe32(output, 24, kTargetSampleRate);
    PutLe32(output, 28, kTargetSampleRate * kTargetChannels *
                         kTargetBitsPerSample / 8);
    PutLe16(output, 32, kTargetChannels * kTargetBitsPerSample / 8);
    PutLe16(output, 34, kTargetBitsPerSample);
    std::memcpy(output.data() + 36, "data", 4);
    PutLe32(output, 40, static_cast<uint32_t>(bytes));
    if (data && bytes) std::memcpy(output.data() + 44, data, bytes);
    return output;
}

std::string Sha256Hex(const BYTE* data, size_t bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectBytes = 0;
    DWORD resultBytes = 0;
    DWORD hashBytes = 0;
    std::vector<BYTE> object;
    std::vector<BYTE> digest;
    std::string result;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) < 0) {
        return result;
    }
    if (BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH,
                          reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
                          &resultBytes, 0) < 0 ||
        BCryptGetProperty(algorithm, BCRYPT_HASH_LENGTH,
                          reinterpret_cast<PUCHAR>(&hashBytes), sizeof(hashBytes),
                          &resultBytes, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return result;
    }
    object.resize(objectBytes);
    digest.resize(hashBytes);
    if (BCryptCreateHash(algorithm, &hash, object.data(), objectBytes,
                         nullptr, 0, 0) >= 0) {
        bool ok = true;
        size_t offset = 0;
        while (offset < bytes && ok) {
            const ULONG chunk = static_cast<ULONG>((std::min<size_t>)(
                bytes - offset, static_cast<size_t>((std::numeric_limits<ULONG>::max)())));
            ok = BCryptHashData(hash,
                                const_cast<PUCHAR>(data ? data + offset : nullptr),
                                chunk, 0) >= 0;
            offset += chunk;
        }
        if (ok && BCryptFinishHash(hash, digest.data(), hashBytes, 0) >= 0) {
            static const char hex[] = "0123456789abcdef";
            result.reserve(digest.size() * 2);
            for (BYTE value : digest) {
                result.push_back(hex[(value >> 4) & 0xf]);
                result.push_back(hex[value & 0xf]);
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}

SaveDecision ShouldSaveDiagnosticAudio(const std::wstring& mode,
                                       const FinalResult& result,
                                       size_t pcmBytes,
                                       double recordingMs,
                                       bool contradictoryStages) {
    const std::wstring normalized = NormalizeMode(mode);
    if (normalized == L"off") return {false, "mode_off"};
    if (result.userCancelled || result.kind == FinalKind::Cancelled) {
        return {false, "cancelled"};
    }
    // A device can fail before producing its first sample. Persist the JSON
    // capture/terminal metadata even though there is no legitimate WAV
    // artifact to write.
    if (result.kind == FinalKind::CaptureFailure) {
        return {true, "capture_failure"};
    }
    if (pcmBytes == 0) return {false, "no_pcm"};
    if (normalized == L"all") return {true, "mode_all"};
    if (result.kind == FinalKind::TooShort) return {false, "too_short"};
    if (contradictoryStages) return {true, "stage_outcome_mismatch"};
    if (result.kind == FinalKind::NoSpeech) {
        if (pcmBytes >= kFailureNoSpeechMinPcmBytes || recordingMs >= 3000.0) {
            return {true, "no_speech"};
        }
        return {false, "no_speech_too_short"};
    }
    if (result.kind == FinalKind::OperationalError) {
        if (result.reason == "auth_or_config" || result.reason == "model_load") {
            return {false, result.reason};
        }
        if (pcmBytes >= kFailureOperationalMinPcmBytes) {
            return {true, result.reason.empty() ? "operational_error" : result.reason};
        }
        return {false, "operational_pcm_too_short"};
    }
    return {false, "successful"};
}

void SetDirectoryOverrideForTesting(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_directoryMutex);
    g_directoryOverride = path;
}

} // namespace audio_diagnostics
