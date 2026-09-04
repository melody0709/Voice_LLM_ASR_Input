#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "asr_result.h"
#include "asr_session.h"
#include "asr_streaming_session.h"
#include "audio_diagnostics.h"
#include "doubao_ime_streaming_session.h"
#include "engine.h"
#include "qwen_audio_streaming.h"
#include "qwen_audio_streaming_session.h"
#include "qwen_free_streaming_session.h"
#include "qwen_streaming_session.h"
#include "streaming_vad_trimmer.h"
#include "volcengine_streaming_session.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// engine.cpp and the provider sessions share the application globals.  The
// replay executable owns an isolated, windowless set so it can reuse the real
// Config, AsrEngine, IAsrSession, and IStreamingAsrSession implementations
// without starting the tray application or touching its active recording.
HINSTANCE g_instance = nullptr;
HWND g_mainWindow = nullptr;
HWND g_settingsWindow = nullptr;
HWND g_hudWindow = nullptr;
HHOOK g_keyboardHook = nullptr;
HICON g_appIcon = nullptr;
HFONT g_uiFont = nullptr;
HFONT g_titleFont = nullptr;
HFONT g_sectionFont = nullptr;
HBRUSH g_settingsBgBrush = nullptr;
HBRUSH g_cardBrush = nullptr;
HBRUSH g_controlBgBrush = nullptr;
ID2D1Factory* g_d2dFactory = nullptr;
IDWriteFactory* g_dwriteFactory = nullptr;
ID2D1HwndRenderTarget* g_hudRenderTarget = nullptr;
ID2D1SolidColorBrush* g_hudBrush = nullptr;
ID2D1LinearGradientBrush* g_hudBarGradientRec = nullptr;
ID2D1LinearGradientBrush* g_hudBarGradientIdle = nullptr;
ID2D1GradientStopCollection* g_hudBarGradientStopsRec = nullptr;
ID2D1GradientStopCollection* g_hudBarGradientStopsIdle = nullptr;
IDWriteTextFormat* g_hudTextFormat = nullptr;
Config g_config;
std::atomic<bool> g_enableDebugMode{false};
bool g_recording = false;
UINT g_activeHotkeyKey = 0;
bool g_capsLockHotkeyPending = false;
bool g_capsLockLongPressActive = false;
bool g_capsLockWasOn = false;
std::wstring g_hudText;
HWAVEIN g_waveIn = nullptr;
WAVEHDR g_waveHeaders[4] = {};
std::vector<std::vector<BYTE>> g_waveBuffers;
std::vector<BYTE> g_audioData;
CRITICAL_SECTION g_audioLock;
std::atomic<bool> g_captureActive{false};
std::atomic<uint64_t> g_audioCaptureGeneration{0};
std::atomic<bool> g_audioCaptureFailurePending{false};
std::atomic<DWORD> g_audioCaptureFailureCode{0};
std::atomic<bool> g_audioCaptureFailureWasapi{false};
std::atomic<float> g_audioLevel{0.0f};
float g_hudSmoothedLevel = 0.0f;
bool g_hudHasSpoken = false;
std::atomic<bool> g_vadDetectedVoice{false};
WasapiCapture g_wasapiCapture;
std::vector<HWND> g_recognitionControls;
std::vector<HWND> g_generalControls;
std::vector<HWND> g_llmControls;
std::vector<HWND> g_promptControls;
std::vector<HWND> g_cloudAsrControls;
std::vector<HWND> g_baiduControls;
std::vector<HWND> g_volcengineControls;
std::vector<HWND> g_qwenControls;
std::vector<HWND> g_qwenAudio3Controls;
std::vector<HWND> g_qwenAudioStreamingOnlyControls;
std::vector<HWND> g_mimoControls;
std::vector<HWND> g_maiControls;
std::vector<HWND> g_maiOpenRouterControls;
std::vector<HWND> g_maiAzureControls;
std::vector<HWND> g_doubaoImeControls;
std::vector<HWND> g_qwenFreeControls;
std::vector<HWND> g_vadFireredControls;
std::vector<HWND> g_vadSileroControls;
bool g_hudIsRefining = false;
bool g_llmKeyVisible = false;
bool g_baiduKeyVisible = false;
bool g_baiduApiKeyVisible = false;
bool g_volcKeyVisible = false;
bool g_qwenKeyVisible = false;
bool g_mimoKeyVisible = false;
bool g_maiOpenRouterKeyVisible = false;
bool g_maiAzureKeyVisible = false;
std::unique_ptr<IStreamingAsrSession> g_activeStreamingSession;
std::unique_ptr<StreamingVadTrimmer> g_streamingVadTrimmer;
CRITICAL_SECTION g_streamingSessionCs;
AsrEngine g_asrEngine;
int g_cloudProviderIdx = 0;
HWND g_cloudAsrHintControl = nullptr;
std::atomic<double> g_vadMs{0.0};
std::atomic<double> g_asrDecodeMs{0.0};
std::atomic<double> g_punctMs{0.0};
std::atomic<double> g_cloudApiMs{0.0};
std::atomic<double> g_llmMs{0.0};
std::wstring g_vadModelName;
std::mutex g_vadMetricsMutex;
std::atomic<size_t> g_vadTrimmedSamples{0};
std::vector<float> g_streamingVadSamples;
std::atomic<bool> g_streamingVadReady{false};
InputContextResult g_inputContextResult;
std::mutex g_inputContextMutex;

namespace {

struct Options {
    std::wstring wavPath;
    std::wstring runtimeDir;
    std::vector<std::wstring> backends;
    bool showText = false;
    bool fastStreaming = false;
    bool useConfigVad = false;
    bool usePostprocess = false;
    bool listBackends = false;
    bool informationalExit = false;
};

struct WavInput {
    std::vector<BYTE> pcm;
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bitsPerSample = 0;
};

struct BackendRequest {
    std::wstring label;
    Config config;
    bool forceBatch = false;
};

struct StreamingFinalState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool bundledPostProcessApplied = false;
    std::wstring text;
};

class RuntimeScope {
public:
    RuntimeScope() {
        InitializeCriticalSection(&g_audioLock);
        InitializeCriticalSection(&g_streamingSessionCs);
        comInitialized_ = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
    }

    ~RuntimeScope() {
        qwen_audio_streaming::InvalidateReusableConnections();
        VolcengineClosePersistentConnection();
        g_asrEngine.Reload();
        DeleteCriticalSection(&g_streamingSessionCs);
        DeleteCriticalSection(&g_audioLock);
        if (comInitialized_) CoUninitialize();
    }

private:
    bool comInitialized_ = false;
};

uint16_t ReadLe16(const BYTE* value) {
    return static_cast<uint16_t>(value[0]) |
           (static_cast<uint16_t>(value[1]) << 8);
}

uint32_t ReadLe32(const BYTE* value) {
    return static_cast<uint32_t>(value[0]) |
           (static_cast<uint32_t>(value[1]) << 8) |
           (static_cast<uint32_t>(value[2]) << 16) |
           (static_cast<uint32_t>(value[3]) << 24);
}

std::string Utf8ForConsole(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string result(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(),
                        static_cast<int>(value.size()), result.data(), bytes,
                        nullptr, nullptr);
    return result;
}

std::wstring LowerBackend(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
        if (ch == L'-') return L'_';
        if (ch >= L'A' && ch <= L'Z') {
            return static_cast<wchar_t>(ch - L'A' + L'a');
        }
        return ch;
    });
    return value;
}

std::wstring ParentDirectory(std::wstring path) {
    while (path.size() > 3 && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

bool DirectoryExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring InferDevelopmentRuntimeDir() {
    // The CMake target is emitted to build\artifacts\tools.  Resolve the
    // sibling canonical payload so direct developer invocations use the same
    // DLLs, bundled VAD models, and Portable flag/config as the wrapper.
    const std::wstring artifactsDir = ParentDirectory(AppRootDir());
    const std::wstring buildDir = ParentDirectory(artifactsDir);
    if (buildDir.empty()) return {};
    const std::wstring candidate = buildDir + L"\\run\\x64-release";
    return DirectoryExists(candidate) ? candidate : std::wstring();
}

bool ReadWholeFile(const std::wstring& path,
                   std::vector<BYTE>& bytes,
                   std::wstring& error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Unable to open WAV file (Win32 " +
                std::to_wstring(GetLastError()) + L").";
        return false;
    }
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > static_cast<LONGLONG>(UINT32_MAX)) {
        error = L"WAV file size is invalid or exceeds 4 GiB.";
        CloseHandle(file);
        return false;
    }
    bytes.resize(static_cast<size_t>(size.QuadPart));
    size_t offset = 0;
    while (offset < bytes.size()) {
        const DWORD request = static_cast<DWORD>((std::min<size_t>)(
            bytes.size() - offset, 16u * 1024u * 1024u));
        DWORD read = 0;
        if (!ReadFile(file, bytes.data() + offset, request, &read, nullptr) ||
            read == 0) {
            error = L"Unable to read the complete WAV file.";
            CloseHandle(file);
            return false;
        }
        offset += read;
    }
    CloseHandle(file);
    return true;
}

bool LoadCanonicalWav(const std::wstring& path,
                      WavInput& input,
                      std::wstring& error) {
    std::vector<BYTE> file;
    if (!ReadWholeFile(path, file, error)) return false;
    if (file.size() < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 ||
        std::memcmp(file.data() + 8, "WAVE", 4) != 0) {
        error = L"The input is not a RIFF/WAVE file.";
        return false;
    }

    bool haveFormat = false;
    bool haveData = false;
    size_t offset = 12;
    while (offset + 8 <= file.size()) {
        const BYTE* header = file.data() + offset;
        const uint32_t chunkBytes = ReadLe32(header + 4);
        offset += 8;
        if (chunkBytes > file.size() - offset) {
            error = L"A WAV chunk extends past the end of the file.";
            return false;
        }

        if (std::memcmp(header, "fmt ", 4) == 0) {
            if (chunkBytes < 16) {
                error = L"The WAV fmt chunk is too short.";
                return false;
            }
            const BYTE* format = file.data() + offset;
            const uint16_t encoding = ReadLe16(format);
            input.channels = ReadLe16(format + 2);
            input.sampleRate = ReadLe32(format + 4);
            input.bitsPerSample = ReadLe16(format + 14);
            if (encoding != 1 || input.channels != 1 ||
                input.sampleRate != audio_diagnostics::kTargetSampleRate ||
                input.bitsPerSample != 16) {
                error = L"Replay accepts only PCM 16 kHz, mono, 16-bit WAV files.";
                return false;
            }
            haveFormat = true;
        } else if (std::memcmp(header, "data", 4) == 0) {
            if (!haveFormat) {
                error = L"The WAV data chunk appears before a valid fmt chunk.";
                return false;
            }
            if (chunkBytes == 0 || (chunkBytes % sizeof(int16_t)) != 0) {
                error = L"The WAV data chunk is empty or not aligned to PCM16 samples.";
                return false;
            }
            input.pcm.assign(file.begin() + offset,
                             file.begin() + offset + chunkBytes);
            haveData = true;
            break;
        }

        offset += chunkBytes;
        if ((chunkBytes & 1u) != 0 && offset < file.size()) ++offset;
    }
    if (!haveFormat || !haveData) {
        error = L"The WAV file is missing a usable fmt or data chunk.";
        return false;
    }
    return true;
}

void PrintUsage() {
    std::cout
        << "VoxType ASR audio replay\n\n"
        << "Usage:\n"
        << "  asr_audio_replay --wav <diagnostic.wav> [options]\n\n"
        << "Options:\n"
        << "  --backend <id>       Repeat for multiple backends (default: local).\n"
        << "  --all-configured     Run local, configured primary, and configured fallback.\n"
        << "  --runtime-dir <dir>  Runtime payload used for assets/config (wrapper sets it).\n"
        << "  --use-config-vad     Reapply the current VAD settings (off by default).\n"
        << "  --use-postprocess    Keep configured punctuation/provider post-processing.\n"
        << "  --fast               Feed streaming providers without real-time cadence.\n"
        << "  --show-text          Print transcript to the console; never writes it to disk.\n"
        << "  --list-backends      Show backend IDs.\n"
        << "  --help               Show this help.\n\n"
        << "Cloud backends upload the selected WAV when explicitly requested.\n";
}

void PrintBackends() {
    std::cout
        << "Backend IDs:\n"
        << "  local, configured, fallback, baidu, mimo, mai, qwen, qwen_http,\n"
        << "  qwen_streaming, volcengine, doubao_ime, doubao_ime_batch, qwen_free\n"
        << "The qwen ID uses the model/transport currently selected in VoxType.\n";
}

bool ParseArguments(int argc, wchar_t** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        if (arg == L"--wav" && i + 1 < argc) {
            options.wavPath = argv[++i];
        } else if (arg == L"--runtime-dir" && i + 1 < argc) {
            options.runtimeDir = argv[++i];
        } else if (arg == L"--backend" && i + 1 < argc) {
            options.backends.push_back(argv[++i]);
        } else if (arg == L"--all-configured") {
            options.backends.push_back(L"all_configured");
        } else if (arg == L"--show-text") {
            options.showText = true;
        } else if (arg == L"--fast") {
            options.fastStreaming = true;
        } else if (arg == L"--use-config-vad") {
            options.useConfigVad = true;
        } else if (arg == L"--use-postprocess") {
            options.usePostprocess = true;
        } else if (arg == L"--list-backends") {
            options.listBackends = true;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            PrintUsage();
            options.informationalExit = true;
            return false;
        } else {
            std::cerr << "Unknown or incomplete argument: " << Utf8ForConsole(arg)
                      << '\n';
            PrintUsage();
            return false;
        }
    }
    if (options.listBackends) {
        PrintBackends();
        options.informationalExit = true;
        return false;
    }
    if (options.wavPath.empty()) {
        std::cerr << "--wav is required.\n";
        PrintUsage();
        return false;
    }
    if (options.backends.empty()) options.backends.push_back(L"local");
    return true;
}

void AddUniqueBackend(std::vector<std::wstring>& backends,
                      std::set<std::wstring>& seen,
                      const std::wstring& backend) {
    const std::wstring normalized = LowerBackend(backend);
    if (!normalized.empty() && seen.insert(normalized).second) {
        backends.push_back(normalized);
    }
}

std::vector<std::wstring> ExpandBackends(const Options& options,
                                         const Config& loaded) {
    std::vector<std::wstring> expanded;
    std::set<std::wstring> seen;
    for (const std::wstring& raw : options.backends) {
        const std::wstring backend = LowerBackend(raw);
        if (backend == L"all_configured") {
            AddUniqueBackend(expanded, seen, L"local");
            AddUniqueBackend(expanded, seen, L"configured");
            if (IsFallbackAsrEnabled(loaded)) {
                AddUniqueBackend(expanded, seen, L"fallback");
            }
        } else {
            AddUniqueBackend(expanded, seen, backend);
        }
    }
    return expanded;
}

bool ResolveBackend(const Config& loaded,
                    const Options& options,
                    const std::wstring& backend,
                    BackendRequest& request,
                    std::wstring& error) {
    request = {};
    request.label = backend;
    request.config = loaded;

    if (backend == L"configured") {
        request.label = L"configured:" + loaded.asrBackend;
    } else if (backend == L"fallback") {
        if (!IsFallbackAsrEnabled(loaded)) {
            error = L"No supported fallback backend is configured.";
            return false;
        }
        request.config = BuildFallbackConfig(loaded);
        request.label = L"fallback:" + request.config.asrBackend;
    } else if (backend == L"qwen_http") {
        request.config.asrBackend = L"qwen";
        request.config.qwenModel = L"qwen-audio-3.0-asr-flash";
        request.config.qwenTransport = L"audio_http";
    } else if (backend == L"qwen_streaming") {
        request.config.asrBackend = L"qwen";
        request.config.qwenModel = L"qwen-audio-3.0-asr-flash-streaming";
        request.config.qwenTransport = L"audio_streaming";
    } else if (backend == L"doubao_ime_batch") {
        request.config.asrBackend = L"doubao_ime";
        request.forceBatch = true;
    } else if (backend == L"local" || backend == L"baidu" ||
               backend == L"mimo" || backend == L"mai" ||
               backend == L"qwen" ||
               backend == L"volcengine" || backend == L"doubao_ime" ||
               backend == L"qwen_free") {
        request.config.asrBackend = backend;
    } else {
        error = L"Unknown backend ID: " + backend;
        return false;
    }

    request.config.fallbackAsrBackend = L"none";
    request.config.diagnosticAudioMode = L"off";
    request.config.asrAttemptId = 0;
    request.config.asrDiagnosticStageKind = audio_diagnostics::StageKind::Primary;
    request.config.asrDiagnosticStageIndex = 0;
    request.config.enableDebugMode = false;
    request.config.qwenFreeDebugLog = false;
    if (!options.useConfigVad) request.config.enableVad = false;
    if (!options.usePostprocess) {
        request.config.postprocess = L"none";
        request.config.qwenFreePolishEnabled = false;
        request.config.qwenFreePunctEnabled = false;
        request.config.qwenFreeCorrectEnabled = false;
        request.config.qwenFreeRewriteEnabled = false;
    }
    return true;
}

bool IsStreamingBackend(const BackendRequest& request) {
    if (request.forceBatch) return false;
    const Config& config = request.config;
    if (config.asrBackend == L"qwen" &&
        config.qwenModel == L"qwen-audio-3.0-asr-flash") {
        return false;
    }
    return config.asrBackend == L"qwen" ||
           config.asrBackend == L"volcengine" ||
           config.asrBackend == L"doubao_ime" ||
           config.asrBackend == L"qwen_free";
}

std::unique_ptr<IStreamingAsrSession> CreateStreamingSession(
    const Config& config) {
    if (config.asrBackend == L"qwen") {
        if (config.qwenModel == L"qwen-audio-3.0-asr-flash-streaming") {
            return CreateQwenAudioStreamingSession(
                config, nullptr, nullptr, nullptr);
        }
        return CreateQwenStreamingSession(config, nullptr, nullptr, nullptr);
    }
    if (config.asrBackend == L"volcengine") {
        VolcengineResetForNewSession();
        return CreateVolcengineStreamingSession(
            config, nullptr, nullptr, nullptr);
    }
    if (config.asrBackend == L"doubao_ime") {
        return CreateDoubaoImeStreamingSession(
            config, nullptr, nullptr, nullptr);
    }
    if (config.asrBackend == L"qwen_free") {
        return CreateQwenFreeStreamingSession(
            config, nullptr, nullptr, nullptr, {});
    }
    return nullptr;
}

void CaptureStreamingFinal(std::wstring text,
                           const Config&,
                           bool bundledPostProcessApplied,
                           void* userData) {
    auto* state = static_cast<StreamingFinalState*>(userData);
    if (!state) return;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->done) return;
        state->text = std::move(text);
        state->bundledPostProcessApplied = bundledPostProcessApplied;
        state->done = true;
    }
    state->cv.notify_one();
}

AsrSessionResult RunBatch(const Config& config,
                          const std::vector<BYTE>& pcm) {
    AsrSessionResult result;
    result.providerName = AsrBackendDisplayName(config);
    result.pcmBytes = pcm.size();
    auto session = CreateBatchAsrSession(config, g_asrEngine, {});
    std::wstring error;
    if (!session || !session->Start(error)) {
        result.text = error.empty() ? L"ASR failed: session start failed" : error;
        return result;
    }
    if (!session->EnqueuePcmChunk(pcm.data(), pcm.size())) {
        session->Abort();
        result.text = L"ASR failed: audio enqueue failed";
        return result;
    }
    return session->Finish();
}

AsrSessionResult RunStreaming(const Config& config,
                              const std::vector<BYTE>& pcm,
                              bool fast) {
    AsrSessionResult result;
    result.providerName = AsrBackendDisplayName(config);
    result.pcmBytes = pcm.size();
    result.isStreaming = true;

    StreamingFinalState finalState;
    auto session = CreateStreamingSession(config);
    if (!session) {
        result.text = L"ASR failed: streaming backend is not available";
        return result;
    }
    session->SetFinalCallback(CaptureStreamingFinal, &finalState);

    std::wstring startError;
    const auto started = std::chrono::steady_clock::now();
    if (!session->Start(startError)) {
        result.text = startError.empty()
            ? L"ASR failed: streaming session start failed"
            : startError;
        session->Abort();
        result.cloudApiMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    }

    constexpr size_t kCadenceBytes = 20u * 32u;
    size_t offset = 0;
    bool enqueueOk = true;
    while (offset < pcm.size()) {
        const size_t bytes = (std::min)(kCadenceBytes, pcm.size() - offset);
        if (!session->EnqueuePcmChunk(pcm.data() + offset, bytes)) {
            enqueueOk = false;
            break;
        }
        offset += bytes;
        if (!fast) {
            const auto deadline = started + std::chrono::microseconds(
                static_cast<long long>(offset) * 1000000ll / 32000ll);
            std::this_thread::sleep_until(deadline);
        }
    }
    if (!enqueueOk) {
        session->Abort();
        result.text = L"ASR failed: streaming audio enqueue failed";
        result.cloudApiMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - started).count();
        return result;
    }

    const double recordingMs = static_cast<double>(pcm.size()) / 32.0;
    session->StopInput(recordingMs, pcm.size());
    const DWORD waitMs = (std::max<DWORD>)(
        1000, (std::min<DWORD>)(session->CurrentWatchdogMs(), 120000));
    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(finalState.mutex);
        completed = finalState.cv.wait_for(
            lock, std::chrono::milliseconds(waitMs),
            [&finalState]() { return finalState.done; });
        if (completed) {
            result.text = finalState.text;
            result.bundledPostProcessApplied =
                finalState.bundledPostProcessApplied;
        }
    }
    if (!completed) result.text = L"ASR failed: streaming replay timeout";
    session->Abort();
    result.cloudApiMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    return result;
}

int RunBackend(const BackendRequest& request,
               const Options& options,
               const WavInput& input) {
    const bool streaming = IsStreamingBackend(request);
    const auto started = std::chrono::steady_clock::now();
    AsrSessionResult result = streaming
        ? RunStreaming(request.config, input.pcm, options.fastStreaming)
        : RunBatch(request.config, input.pcm);
    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - started).count();
    result.text = NormalizeAsrText(std::move(result.text));
    const AsrResultClassification classification = ClassifyAsrResult(result.text);

    std::cout << "backend=" << Utf8ForConsole(request.label)
              << " resolved=" << AsrBackendLogName(request.config.asrBackend)
              << " transport=" << (streaming ? "streaming" : "batch")
              << " terminal=" << AsrResultKindDebugName(classification.kind)
              << " reason=" << AsrFailureReasonDebugName(classification.reason)
              << " text_chars=" << result.text.size()
              << " elapsed_ms=" << static_cast<unsigned long long>(elapsedMs)
              << " provider_ms=" << static_cast<unsigned long long>(result.cloudApiMs)
              << '\n';
    if (options.showText) {
        std::cout << "text=" << Utf8ForConsole(result.text) << '\n';
    }
    return classification.kind == AsrResultKind::OperationalError ? 1 : 0;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    Options options;
    if (!ParseArguments(argc, argv, options)) {
        return options.informationalExit ? 0 : 2;
    }

    if (options.runtimeDir.empty()) {
        options.runtimeDir = InferDevelopmentRuntimeDir();
    }
    if (!options.runtimeDir.empty() &&
        !SetRuntimeAssetDirOverrideForProcess(options.runtimeDir)) {
        std::cerr << "Runtime directory is unavailable: "
                  << Utf8ForConsole(options.runtimeDir) << '\n';
        return 2;
    }
    if (!SetDllDirectoryW(RuntimeAssetDir().c_str())) {
        std::cerr << "Unable to activate the runtime DLL directory: "
                  << Utf8ForConsole(RuntimeAssetDir()) << '\n';
        return 2;
    }

    WavInput input;
    std::wstring wavError;
    if (!LoadCanonicalWav(options.wavPath, input, wavError)) {
        std::cerr << "WAV validation failed: " << Utf8ForConsole(wavError) << '\n';
        return 2;
    }

    RuntimeScope runtime;
    LoadConfig();
    const Config loaded = g_config;
    std::cout << "runtime_dir=" << Utf8ForConsole(RuntimeAssetDir())
              << " config=" << Utf8ForConsole(ConfigPath())
              << " portable=" << (IsPortableMode() ? 1 : 0) << '\n';

    const std::string hash = audio_diagnostics::Sha256Hex(
        input.pcm.data(), input.pcm.size());
    const double durationMs = static_cast<double>(input.pcm.size()) / 32.0;
    const audio_diagnostics::Pcm16Metrics metrics =
        audio_diagnostics::CalculatePcm16Metrics(
            input.pcm.data(), input.pcm.size());
    std::cout << "wav=" << Utf8ForConsole(options.wavPath)
              << " pcm_bytes=" << input.pcm.size()
              << " duration_ms=" << static_cast<unsigned long long>(durationMs)
              << " sha256=" << hash
              << " rms_dbfs=" << metrics.rmsDbfs
              << " peak_dbfs=" << metrics.peakDbfs
              << " zero_ratio=" << metrics.zeroRatio
              << '\n';

    int failures = 0;
    const std::vector<std::wstring> backends = ExpandBackends(options, loaded);
    for (const std::wstring& backend : backends) {
        BackendRequest request;
        std::wstring error;
        if (!ResolveBackend(loaded, options, backend, request, error)) {
            std::cerr << "backend=" << Utf8ForConsole(backend)
                      << " error=" << Utf8ForConsole(error) << '\n';
            ++failures;
            continue;
        }
        if (request.config.asrBackend != L"local") {
            std::cout << "notice=cloud_upload backend="
                      << Utf8ForConsole(request.label) << '\n';
        }
        failures += RunBackend(request, options, input);
    }
    return failures == 0 ? 0 : 1;
}
