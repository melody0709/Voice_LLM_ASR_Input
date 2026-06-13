#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "globals.h"
#include "engine.h"
#include "hud.h"
#include "hotkey.h"
#include "settings.h"
#include "input_context.h"
#include "asr_session.h"
#include "asr_streaming_session.h"
#include "asr_result.h"
#include "cloud_asr_common.h"
#include "asr_dispatcher.h"
#include "qwen_streaming_session.h"
#include "volcengine_streaming_session.h"
#include "streaming_vad_trimmer.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <deque>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <algorithm>
#include <commctrl.h>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

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
bool g_enableDebugMode = false;
bool g_recording = false;
UINT g_activeHotkeyKey = 0;
bool g_capsLockHotkeyPending = false;
bool g_capsLockLongPressActive = false;
bool g_capsLockWasOn = false;
std::wstring g_hudText = L"Ready";
HWAVEIN g_waveIn = nullptr;
WAVEHDR g_waveHeaders[4] = {};
std::vector<std::vector<BYTE>> g_waveBuffers;
std::vector<BYTE> g_audioData;
CRITICAL_SECTION g_audioLock;
bool g_captureActive = false;
std::atomic<float> g_audioLevel{ 0.0f };
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
std::vector<HWND> g_mimoControls;
std::vector<HWND> g_vadFireredControls;
std::vector<HWND> g_vadSileroControls;
bool g_hudIsRefining = false;
bool g_llmKeyVisible = false;
bool g_baiduKeyVisible = false;
bool g_baiduApiKeyVisible = false;
bool g_volcKeyVisible = false;
bool g_qwenKeyVisible = false;
bool g_mimoKeyVisible = false;
std::unique_ptr<IStreamingAsrSession> g_activeStreamingSession;
std::unique_ptr<StreamingVadTrimmer> g_streamingVadTrimmer;
CRITICAL_SECTION g_streamingSessionCs;
volc_asr::VolcSession g_volcSession;
namespace volc_asr { std::atomic<bool> g_volcKeepAlive{false}; }
AsrEngine g_asrEngine;
int g_cloudProviderIdx = 0;
HWND g_cloudAsrHintControl = nullptr;


InputContextResult g_inputContextResult;

static ULONGLONG g_sessionStartTick = 0;
static double g_recordingMs = 0.0;
double g_vadMs = 0.0;
double g_asrDecodeMs = 0.0;
double g_punctMs = 0.0;
double g_cloudApiMs = 0.0;
double g_llmMs = 0.0;
std::wstring g_vadModelName;
size_t g_vadTrimmedSamples = 0;
std::vector<float> g_streamingVadSamples;
std::atomic<bool> g_streamingVadReady{false};
static std::wstring g_lastRawAsrText;
static size_t g_lastPcmBytes = 0;
static bool s_wasapiUsed = false;
static std::wstring s_wasapiDeviceName;
static UINT32 s_wasapiNativeRate = 0;


static bool s_debugConsoleOpen = false;

static void DebugModeOpenConsole() {
    if (s_debugConsoleOpen) return;
    if (!AllocConsole()) return;
    s_debugConsoleOpen = true;
    FILE* dummy = nullptr;
    freopen_s(&dummy, "CONOUT$", "w", stdout);
    freopen_s(&dummy, "CONOUT$", "w", stderr);
    SetConsoleTitleW(L"VoxType Debug Console");
    printf("\n--- Debug mode enabled ---\n\n");
}

static void DebugModeCloseConsole() {
    if (!s_debugConsoleOpen) return;
    s_debugConsoleOpen = false;
    FreeConsole();
}

static void DebugPrintHeader(double recMs, size_t pcmBytes) {
    SYSTEMTIME st;
    GetLocalTime(&st);
    printf("\n-- %02d:%02d:%02d  Rec %.1fs(%zuKB) --",
           st.wHour, st.wMinute, st.wSecond,
           recMs / 1000.0, (pcmBytes > 0 ? pcmBytes : static_cast<size_t>(recMs * 32)) / 1024);
    if (s_wasapiUsed) {
        printf(" WASAPI %ukHz->16kHz (%ls)", s_wasapiNativeRate / 1000, s_wasapiDeviceName.c_str());
    } else {
        printf(" waveIn 16kHz (default)");
    }
    printf("\n");
}

static void DebugPrintTextLine(const wchar_t* prefix, const std::wstring& text) {
    if (text.empty()) return;
    std::wstring oneLine = text;
    for (auto& c : oneLine) if (c == L'\n' || c == L'\r') c = L' ';
    std::wstring line = std::wstring(L"  ") + prefix + L": \"" + oneLine + L"\"\n";
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}

static void DebugPrintInputContext() {
    if (g_config.volcEnableInputContext) {
        auto& ic = g_inputContextResult;
        printf("  Context: %s %.0fms", input_context::LayerName(ic.successLayer), ic.elapsedMs);
        if (!ic.focusWindowClass.empty())
            printf(" class=%s", ic.focusWindowClass.c_str());
        if (!ic.controlType.empty())
            printf(" uia=%s", ic.controlType.c_str());
        if (ic.inputFieldText.empty()) {
            printf(" [%ls]\n", input_context::TruncateForDisplay(ic.windowTitle).c_str());
        } else {
            printf(" len=%d\n", ic.textLength);
            printf("  ContextText: \"");
            DWORD written = 0;
            WriteConsoleW(GetStdHandle(STD_OUTPUT_HANDLE), ic.inputFieldText.c_str(), (DWORD)ic.inputFieldText.size(), &written, nullptr);
            printf("\"\n");
        }
        if (ic.successLayer < 0 && !ic.failReason.empty())
            printf("  ContextFail: %s\n", ic.failReason.c_str());
    } else if (g_config.volcEnableContext) {
        printf("  Context: HISTORY %zu rounds\n", VolcengineRecognitionHistorySize());
    }
}

static void DebugPrintVadTrimLine(size_t rawBytes, size_t trimmedSamples) {
    if (trimmedSamples == 0) return;
    if (rawBytes == 0) rawBytes = static_cast<size_t>(g_recordingMs * 32.0);
    const size_t trimBytes = trimmedSamples * sizeof(int16_t);
    const double trimMs = trimmedSamples / 16.0;
    printf("  VAD trim: %.1fs/%zuKB -> %.1fs/%zuKB (%.0f%%)\n",
           g_recordingMs / 1000.0, rawBytes / 1024,
           trimMs / 1000.0, trimBytes / 1024,
           rawBytes > 0 ? 100.0 * trimBytes / rawBytes : 0.0);
}

static void DebugPrintCloudVadTrim() {
    if ((g_config.asrBackend == L"volcengine" || g_config.asrBackend == L"qwen") &&
        g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive()) {
        StreamingVadTrimStats stats = g_streamingVadTrimmer->Stats();
        size_t rawBytes = stats.rawBytes > 0 ? stats.rawBytes : static_cast<size_t>(g_recordingMs * 32.0);
        if (g_lastPcmBytes > 0) rawBytes = g_lastPcmBytes;
        if (stats.outputBytes > 0) {
            double sentMs = stats.outputBytes / 32.0;
            printf("  VAD trim: %.1fs/%zuKB -> %.1fs/%zuKB (%.0f%%)\n",
                   g_recordingMs / 1000.0, rawBytes / 1024,
                   sentMs / 1000.0, stats.outputBytes / 1024,
                   rawBytes > 0 ? 100.0 * stats.outputBytes / rawBytes : 0.0);
        } else {
            printf("  VAD trim: %.1fs/%zuKB raw, sent 0KB\n",
                   g_recordingMs / 1000.0, rawBytes / 1024);
        }
        return;
    }

    if ((g_config.asrBackend == L"baidu" || g_config.asrBackend == L"mimo") &&
        g_vadMs > 0 && g_vadTrimmedSamples > 0) {
        size_t rawBytes = g_lastPcmBytes > 0 ? g_lastPcmBytes
            : static_cast<size_t>(g_recordingMs * 32.0);
        DebugPrintVadTrimLine(rawBytes, g_vadTrimmedSamples);
    }
}



void WriteLlmLog(const std::wstring& asrText, const std::wstring& llmText) {
    std::wstring logDir = AppRootDir() + L"\\log";
    CreateDirectoryW(logDir.c_str(), nullptr);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t dateStr[16];
    swprintf_s(dateStr, L"%04d%02d%02d", st.wYear, st.wMonth, st.wDay);
    std::wstring logPath = logDir + L"\\llm_refine_" + dateStr + L".log";

    wchar_t timeStr[32];
    swprintf_s(timeStr, L"[%04d-%02d-%02d %02d:%02d:%02d]", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::ofstream file(logPath, std::ios::app | std::ios::binary);
    if (!file) return;
    std::string ts = llm::WideToUtf8(timeStr);
    std::string asr = llm::WideToUtf8(asrText);
    std::string llmS = llm::WideToUtf8(llmText);
    file << ts << "\n"
         << "[ASR]  " << asr << "\n"
         << "[LLM]  " << llmS << "\n"
         << "---\n";
    file.flush();
}

void RefineWithLlmAsync(const std::wstring& asrText, const Config& config) {
    llm::RequestConfig cfg;
    cfg.endpoint = config.llmEndpoint;
    cfg.apiKey = config.llmApiKey;
    cfg.model = config.llmModel;
    cfg.systemPrompt = config.llmPrompt;
    cfg.extraParams = config.llmExtraParams;
    bool debug = config.enableLlmDebug;
    std::thread([asrText, cfg, debug]() {
        HiResTimer tLlm;
        std::wstring result = llm::Refine(asrText, cfg);
        g_llmMs = tLlm.ElapsedMs();
        if (debug) {
            WriteLlmLog(asrText, result);
        }
        PostMessageW(g_mainWindow, kLlmResultMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(result)));
    }).detach();
}

void RecognizeAsync(const std::vector<BYTE>& pcm) {
    const Config config = g_config;

    std::vector<float> localStreamingVadSamples;
    if (config.asrBackend != L"baidu" && config.asrBackend != L"volcengine" &&
        config.asrBackend != L"qwen" && config.asrBackend != L"mimo") {
        localStreamingVadSamples = std::move(g_streamingVadSamples);
        g_streamingVadSamples.clear();
    }

    std::thread([config, pcm, localStreamingVadSamples = std::move(localStreamingVadSamples)]() mutable {
        auto session = CreateBatchAsrSession(config, g_asrEngine, std::move(localStreamingVadSamples));
        std::wstring startError;
        if (!session || !session->Start(startError)) {
            std::wstring text = startError.empty() ? L"ASR failed: session start failed" : startError;
            DispatchAsrFinalText(g_mainWindow, text, config, RefineWithLlmAsync, &g_lastRawAsrText);
            return;
        }

        g_lastPcmBytes = pcm.size();
        session->EnqueuePcmChunk(pcm.data(), pcm.size());
        AsrSessionResult result = session->Finish();

        g_lastPcmBytes = result.pcmBytes;
        if (result.backend == AsrSessionBackend::BaiduBatch ||
            result.backend == AsrSessionBackend::QwenRealtimeBatch ||
            result.backend == AsrSessionBackend::MimoBatch) {
            g_cloudApiMs = result.cloudApiMs;
        }
        if (config.enableDebugMode && result.vadTrimmedSamples > 0) {
            g_vadTrimmedSamples = result.vadTrimmedSamples;
            g_vadMs = result.vadMs;
            g_vadModelName = result.vadModelName;
        }

        DispatchAsrFinalText(g_mainWindow, result.text, config, RefineWithLlmAsync, &g_lastRawAsrText);
    }).detach();
}

static void QwenPartialHudCallback(const std::wstring& text, bool, void*) {
    if (text.empty()) return;
    PostMessageW(g_mainWindow, kHudUpdateMessage, 0,
                 reinterpret_cast<LPARAM>(new std::wstring(L"Listening... Qwen ASR\n" + text)));
}

static std::unique_ptr<IStreamingAsrSession> TakeActiveStreamingSession() {
    EnterCriticalSection(&g_streamingSessionCs);
    auto session = std::move(g_activeStreamingSession);
    LeaveCriticalSection(&g_streamingSessionCs);
    return session;
}

static bool HasActiveStreamingSession() {
    EnterCriticalSection(&g_streamingSessionCs);
    const bool hasSession = (g_activeStreamingSession != nullptr);
    LeaveCriticalSection(&g_streamingSessionCs);
    return hasSession;
}

static void AbortAndResetActiveStreamingSession() {
    auto session = TakeActiveStreamingSession();
    if (session) {
        session->Abort();
    }
}

static void ResetStreamingVadTrimmerState() {
    g_streamingVadTrimmer.reset();
    g_streamingVadReady = false;
    g_vadDetectedVoice.store(false);
}

static bool StartStreamingVadTrimmerForCloud(const wchar_t* debugPrefix) {
    ResetStreamingVadTrimmerState();
    if (!g_config.enableVad) return false;

    auto trimmer = std::make_unique<StreamingVadTrimmer>();
    std::wstring error;
    const bool ok = trimmer->Start(g_config, g_asrEngine, &error);
    if (!ok) {
        VolcDebugLog("%ls: VAD trim disabled (%ls)", debugPrefix,
                     error.empty() ? L"init failed" : error.c_str());
        return false;
    }

    g_streamingVadReady = true;
    g_streamingVadTrimmer = std::move(trimmer);
    VolcDebugLog("%ls: VAD trim active (%ls)", debugPrefix, g_config.vadModel.c_str());
    return true;
}

static void FinishStreamingVadTrimmer() {
    if (!g_streamingVadTrimmer || !g_streamingVadTrimmer->IsActive()) return;
    g_streamingVadTrimmer->Finish();
}

static bool StreamingVadTrimSawNoSpeech() {
    return g_streamingVadTrimmer &&
           g_streamingVadTrimmer->IsActive() &&
           !g_streamingVadTrimmer->DetectedSpeech();
}

void StartRecordingSession() {
    if (g_recording) return;
    if (g_hudWindow) KillTimer(g_hudWindow, kHudHideTimer);

    AbortAndResetActiveStreamingSession();
    ResetStreamingVadTrimmerState();

    if (g_config.asrBackend == L"qwen") {
        auto session = CreateQwenStreamingSession(g_config, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(QwenPartialHudCallback, nullptr);

        std::wstring startError;
        if (!session->Start(startError)) {
            ShowHud(startError.empty() ? L"Qwen ASR error: session start failed" : startError);
            if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
            return;
        }

        EnterCriticalSection(&g_streamingSessionCs);
        g_activeStreamingSession = std::move(session);
        LeaveCriticalSection(&g_streamingSessionCs);

        StartStreamingVadTrimmerForCloud(L"Qwen thread");
    }

    if (g_config.asrBackend == L"volcengine") {
        ShowHud(L"Listening... Volcano Engine");

        g_volcSession.forceAbort = false;
        g_volcSession.lastError.clear();

        StartStreamingVadTrimmerForCloud(L"Volc thread");

        auto session = CreateVolcengineStreamingSession(g_config, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback([](const std::wstring& text, bool isFinal, void* userData) {
            (void)userData;
            if (!isFinal) {
                PostMessageW(g_mainWindow, kHudUpdateMessage, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(L"Listening... Volcano Engine\n" + text)));
            }
        }, nullptr);

        std::wstring startError;
        if (!session->Start(startError)) {
            DispatchAsrFinalText(g_mainWindow, startError, g_config, RefineWithLlmAsync, &g_lastRawAsrText);
            return;
        }

        EnterCriticalSection(&g_streamingSessionCs);
        g_activeStreamingSession = std::move(session);
        LeaveCriticalSection(&g_streamingSessionCs);
    }

    std::wstring error;
    if (!StartAudioCapture(error)) {
        AbortAndResetActiveStreamingSession();
        ShowHud(error);
        if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
        return;
    }
    g_sessionStartTick = GetTickCount64();
    g_recording = true;
    s_wasapiUsed = g_wasapiCapture.IsInitialized();
    if (s_wasapiUsed) {
        s_wasapiDeviceName = g_wasapiCapture.GetDeviceName();
        s_wasapiNativeRate = g_wasapiCapture.GetNativeSampleRate();
    }

    if (g_config.asrBackend == L"qwen") {
        ShowHud(L"Listening... Qwen ASR");
        DWORD watchdogMs = 18000;
        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession) {
            watchdogMs = g_activeStreamingSession->CurrentWatchdogMs();
        }
        LeaveCriticalSection(&g_streamingSessionCs);
        SetTimer(g_mainWindow, kStreamingWatchdogTimer, watchdogMs, nullptr);
        return;
    }

    if (g_config.asrBackend == L"volcengine") {
        ShowHud(L"Listening... Volcano Engine");
        DWORD watchdogMs = 18000;
        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession) {
            watchdogMs = g_activeStreamingSession->CurrentWatchdogMs();
        }
        LeaveCriticalSection(&g_streamingSessionCs);
        SetTimer(g_mainWindow, kStreamingWatchdogTimer, watchdogMs, nullptr);
        return;
    }

    std::wstring name = AsrBackendDisplayName(g_config);
    ShowHud(L"Listening... " + name);

    g_streamingVadReady = false;
    g_streamingVadSamples.clear();
    if (g_config.asrBackend != L"baidu" && g_config.asrBackend != L"volcengine" &&
        g_config.asrBackend != L"qwen" && g_config.asrBackend != L"mimo" &&
        g_config.enableVad) {
        const int threads = ResolveThreads(g_config.threads);
        g_asrEngine.Lock();
        bool ok = g_asrEngine.EnsureVadForConfig(g_config, threads);
        if (ok) {
            if (g_config.vadModel == L"firered") {
                g_asrEngine.fireRedVad->Reset();
            } else {
                g_asrEngine.vad->Reset();
            }
        }
        g_asrEngine.Unlock();
        g_streamingVadReady = ok;
    }
}

void StopRecordingSession() {
    if (!g_recording) return;
    g_recording = false;
    g_recordingMs = static_cast<double>(GetTickCount64() - g_sessionStartTick);
    g_vadMs = 0.0;
    g_asrDecodeMs = 0.0;
    g_punctMs = 0.0;
    g_cloudApiMs = 0.0;
    g_llmMs = 0.0;
    g_vadModelName.clear();
    g_vadTrimmedSamples = 0;
    g_lastRawAsrText.clear();

    if (g_config.asrBackend == L"qwen" && HasActiveStreamingSession()) {
        const std::vector<BYTE> pcm = StopAudioCapture();
        g_lastPcmBytes = pcm.size();
        if (pcm.size() < 8000) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            AbortAndResetActiveStreamingSession();
            ShowHud(L"Too short");
            SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
            return;
        }
        FinishStreamingVadTrimmer();
        if (StreamingVadTrimSawNoSpeech()) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            AbortAndResetActiveStreamingSession();
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
        DWORD finalizeTimeout = ComputeCloudAsrFinalizeTimeoutMs(g_recordingMs, pcm.size());
        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession) {
            g_activeStreamingSession->StopInput(g_recordingMs, pcm.size());
            finalizeTimeout = g_activeStreamingSession->CurrentWatchdogMs();
        }
        LeaveCriticalSection(&g_streamingSessionCs);
        KillTimer(g_mainWindow, kStreamingWatchdogTimer);
        SetTimer(g_mainWindow, kStreamingWatchdogTimer, finalizeTimeout, nullptr);
        ShowHud(L"Recognizing... Qwen ASR");
        return;
    }

    if (g_config.asrBackend == L"volcengine" && HasActiveStreamingSession()) {
        const std::vector<BYTE> pcm = StopAudioCapture();
        g_lastPcmBytes = pcm.size();
        if (pcm.size() < 8000) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            AbortAndResetActiveStreamingSession();
            ShowHud(L"Too short");
            SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
            return;
        }
        FinishStreamingVadTrimmer();
        if (StreamingVadTrimSawNoSpeech()) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            AbortAndResetActiveStreamingSession();
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
        DWORD finalizeTimeout = ComputeCloudAsrFinalizeTimeoutMs(g_recordingMs, pcm.size());
        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession) {
            g_activeStreamingSession->StopInput(g_recordingMs, pcm.size());
            finalizeTimeout = g_activeStreamingSession->CurrentWatchdogMs();
        }
        LeaveCriticalSection(&g_streamingSessionCs);
        KillTimer(g_mainWindow, kStreamingWatchdogTimer);
        SetTimer(g_mainWindow, kStreamingWatchdogTimer, finalizeTimeout, nullptr);
        VolcDebugLog("Volc watchdog: finalize timeout reset to %ums (recording=%.0fms, pcm=%zu)",
                     finalizeTimeout, g_recordingMs, pcm.size());
        ShowHud(L"Recognizing... Volcano Engine");
        return;
    }

    const std::vector<BYTE> pcm = StopAudioCapture();
    if (pcm.size() < 8000) {
        ShowHud(L"Too short");
        SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
        return;
    }

    if (g_streamingVadReady) {
        HiResTimer tVad;
        g_asrEngine.Lock();
        if (g_config.vadModel == L"firered") {
            g_asrEngine.fireRedVad->Flush();
            auto samples = PcmToFloat(pcm);
            auto concat = g_asrEngine.fireRedVad->GetConcatenatedSamples(samples.data(), static_cast<int>(samples.size()));
            if (!concat.empty()) {
                g_streamingVadSamples = std::move(concat);
            }
        } else {
            g_asrEngine.vad->Flush();
            while (!g_asrEngine.vad->IsEmpty()) {
                auto seg = g_asrEngine.vad->Front();
                g_streamingVadSamples.insert(g_streamingVadSamples.end(),
                    seg.samples.begin(), seg.samples.end());
                g_asrEngine.vad->Pop();
            }
        }
        g_asrEngine.Unlock();
        double ms = tVad.ElapsedMs();
        if (g_config.enableDebugMode) {
            g_vadMs = ms;
            g_vadModelName = (g_config.vadModel == L"firered") ? L"FireRed" : L"Silero";
        }
        g_streamingVadReady = false;

        if (g_streamingVadSamples.empty()) {
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
    }

    std::wstring name = AsrBackendDisplayName(g_config);
    ShowHud(L"Recognizing... " + name);
    RecognizeAsync(pcm);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED, ID_TRAY_VERSION, APP_VERSION_WSTR);
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD, L"Reload ASR Engine");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (g_config.enableDebugMode ? MF_CHECKED : 0),
                ID_TRAY_DEBUG_MODE, L"Debug Mode");
    AppendMenuW(menu, MF_STRING | (g_config.forceUnicodeInput ? MF_CHECKED : 0),
                ID_TRAY_FORCE_UNICODE, L"Force Unicode Input");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_QUIT, L"Quit");
    SetForegroundWindow(hwnd);
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    if (pt.y > work.bottom) pt.y = work.bottom;
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

LRESULT CALLBACK MainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE:
        AddTrayIcon(hwnd);
        InstallKeyboardHook();
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ShowSettingsWindow(hwnd);
        }
        return 0;
    case kReloadMessage:
        g_enableDebugMode = g_config.enableDebugMode;
        g_asrEngine.Reload();
        if (g_config.asrBackend == L"local") {
            const Config cfg = g_config;
            std::thread([cfg]() {
                PreloadAsrEngine(cfg);
                PostMessageW(g_mainWindow, kPreloadDoneMessage, 0, 0);
            }).detach();
        }
        return 0;
    case kPreloadDoneMessage:
        return 0;
    case kHudUpdateMessage: {
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lParam));
        if (text) ShowHud(*text);
        return 0;
    }
    case kAsrResultMessage: {
        KillTimer(g_mainWindow, kStreamingWatchdogTimer);
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
        const std::wstring text = NormalizeAsrText(result ? *result : L"ASR failed");
        const bool isError = IsOperationalAsrError(text);
        if (wParam == 1 && !text.empty() && !isError) {
            g_hudIsRefining = true;
            ShowHud(L"Refining...");
        } else {
            g_hudIsRefining = false;
            ShowHud(text);
            if (g_hudWindow) {
                const bool isNoSpeech = (text == L"No speech detected");
                UINT hideMs = isError ? 2200 : (isNoSpeech ? 1500 : 200);
                SetTimer(g_hudWindow, kHudHideTimer, hideMs, nullptr);
            }
            if (!text.empty() && text != L"No speech detected" && !isError) {
                VolcDebugLog("PasteTextImeAware: starting (text=%u chars)", (unsigned)text.size());
                HiResTimer tPaste;
                PasteTextImeAware(text);
                double pasteMs = tPaste.ElapsedMs();
                VolcDebugLog("PasteTextImeAware: done (%.0fms)", pasteMs);

                if (g_config.enableDebugMode) {
                    DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

                    DebugPrintInputContext();

                    if (g_config.asrBackend == L"local") {
                        printf("  Pipeline: ");
                        if (g_vadMs > 0) printf("VAD(%ls) %.0f | ", g_vadModelName.c_str(), g_vadMs);
                        printf("ASR %.0f", g_asrDecodeMs);
                        if (g_punctMs > 0) printf(" | Punct %.0f", g_punctMs);
                        printf(" | Paste %.0f = Total %.0fms\n", pasteMs,
                               g_vadMs + g_asrDecodeMs + g_punctMs + pasteMs);
                        if (g_vadMs > 0 && g_vadTrimmedSamples > 0) {
                            size_t rawBytes = g_lastPcmBytes > 0 ? g_lastPcmBytes
                                : static_cast<size_t>(g_recordingMs * 32.0);
                            DebugPrintVadTrimLine(rawBytes, g_vadTrimmedSamples);
                        }
                    } else {
                        const char* backend = AsrBackendDebugName(g_config.asrBackend);
                        printf("  Pipeline: %s %.0f | Paste %.0f = Total %.0fms\n",
                               backend, g_cloudApiMs, pasteMs, g_cloudApiMs + pasteMs);
                        DebugPrintCloudVadTrim();
                    }

                    DebugPrintTextLine(L"OK", text);
                }
            } else if (isError) {
                VolcDebugLog("PasteTextImeAware: skipped operational error '%ls'", text.c_str());
            }
        }
        return 0;
    }
    case kLlmResultMessage: {
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
        const std::wstring text = result ? *result : L"LLM failed";
        g_hudIsRefining = false;
        ShowHud(text);
        if (!text.empty() && text.rfind(L"LLM failed:", 0) != 0) {
            HiResTimer tPaste;
            PasteTextImeAware(text);
            double pasteMs = tPaste.ElapsedMs();

            if (g_config.enableDebugMode) {
                DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

                DebugPrintInputContext();

                if (g_config.asrBackend == L"local") {
                    printf("  Pipeline: ");
                    if (g_vadMs > 0) printf("VAD(%ls) %.0f | ", g_vadModelName.c_str(), g_vadMs);
                    printf("ASR %.0f", g_asrDecodeMs);
                    if (g_punctMs > 0) printf(" | Punct %.0f", g_punctMs);
                    printf(" | LLM %.0f | Paste %.0f = Total %.0fms\n",
                           g_llmMs, pasteMs,
                           g_vadMs + g_asrDecodeMs + g_punctMs + g_llmMs + pasteMs);
                } else {
                    const char* backend = AsrBackendDebugName(g_config.asrBackend);
                    printf("  Pipeline: %s %.0f | LLM %.0f | Paste %.0f = Total %.0fms\n",
                           backend, g_cloudApiMs, g_llmMs, pasteMs,
                           g_cloudApiMs + g_llmMs + pasteMs);
                    DebugPrintCloudVadTrim();
                }

                DebugPrintTextLine(L"ASR", g_lastRawAsrText);
                DebugPrintTextLine(L"LLM", text);
            }
        }
        if (g_hudWindow) {
            SetTimer(g_hudWindow, kHudHideTimer, 200, nullptr);
        }
        return 0;
    }
    case WM_TIMER:
        if (wParam == kCapsLockLongPressTimer) {
            KillTimer(hwnd, kCapsLockLongPressTimer);
            ActivateCapsLockLongPress();
            return 0;
        }
        if (wParam == kStreamingWatchdogTimer) {
            KillTimer(hwnd, kStreamingWatchdogTimer);
            if (g_recording) {
                DWORD watchdogMs = 18000;
                EnterCriticalSection(&g_streamingSessionCs);
                if (g_activeStreamingSession) {
                    watchdogMs = g_activeStreamingSession->CurrentWatchdogMs();
                }
                LeaveCriticalSection(&g_streamingSessionCs);
                SetTimer(hwnd, kStreamingWatchdogTimer, watchdogMs, nullptr);
                return 0;
            }
            auto session = TakeActiveStreamingSession();
            if (session && session->IsRunning()) {
                session->Abort();
                std::wstring providerName = session->ProviderName();
                std::wstring timeoutText = std::wstring(providerName) + L" error: timeout";
                if (providerName == L"Volcano Engine") {
                    timeoutText = L"ASR failed: VolcEngine timeout";
                }
                PostMessageW(hwnd, kAsrResultMessage, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(timeoutText)));
            }
            return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case ID_TRAY_SETTINGS:
            ShowSettingsWindow(hwnd);
            return 0;
        case ID_TRAY_RELOAD:
            PostMessageW(hwnd, kReloadMessage, 0, 0);
            return 0;
        case ID_TRAY_QUIT:
            DestroyWindow(hwnd);
            return 0;
        case ID_TRAY_DEBUG_MODE:
            g_config.enableDebugMode = !g_config.enableDebugMode;
            g_enableDebugMode = g_config.enableDebugMode;
            if (g_config.enableDebugMode) DebugModeOpenConsole();
            else DebugModeCloseConsole();
            SaveConfig();
            return 0;
        case ID_TRAY_FORCE_UNICODE:
            g_config.forceUnicodeInput = !g_config.forceUnicodeInput;
            SaveConfig();
            return 0;
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kStreamingWatchdogTimer);
        AbortAndResetActiveStreamingSession();
        g_volcSession.forceAbort = true;
        g_captureActive = false;
        StopAudioCapture();
        {
            HINTERNET hWs = g_volcSession.hWebSocket;
            if (hWs) { WinHttpCloseHandle(hWs); g_volcSession.hWebSocket = nullptr; }
            HINTERNET hConn = g_volcSession.hConnect;
            if (hConn) { WinHttpCloseHandle(hConn); g_volcSession.hConnect = nullptr; }
            HINTERNET hSess = g_volcSession.hSession;
            if (hSess) { WinHttpCloseHandle(hSess); g_volcSession.hSession = nullptr; }
        }
        UninstallKeyboardHook();
        RemoveTrayIcon(hwnd);
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

bool RegisterWindowClasses() {
    WNDCLASSEXW mainClass = { sizeof(mainClass) };
    mainClass.lpfnWndProc = MainWndProc;
    mainClass.hInstance = g_instance;
    mainClass.lpszClassName = kMainClass;
    mainClass.hIcon = g_appIcon;
    mainClass.hIconSm = g_appIcon;
    mainClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassExW(&mainClass)) return false;

    WNDCLASSEXW settingsClass = { sizeof(settingsClass) };
    settingsClass.lpfnWndProc = SettingsWndProc;
    settingsClass.hInstance = g_instance;
    settingsClass.lpszClassName = kSettingsClass;
    settingsClass.hIcon = g_appIcon;
    settingsClass.hIconSm = g_appIcon;
    settingsClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    settingsClass.hbrBackground = g_settingsBgBrush;
    if (!RegisterClassExW(&settingsClass)) return false;

    WNDCLASSEXW hudClass = { sizeof(hudClass) };
    hudClass.lpfnWndProc = HudWndProc;
    hudClass.hInstance = g_instance;
    hudClass.lpszClassName = kHudClass;
    hudClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hudClass.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    if (!RegisterClassExW(&hudClass)) return false;

    WNDCLASSEXW hotkeyClass = { sizeof(hotkeyClass) };
    hotkeyClass.lpfnWndProc = HotkeyEditWndProc;
    hotkeyClass.hInstance = g_instance;
    hotkeyClass.lpszClassName = kHotkeyEditClass;
    hotkeyClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    hotkeyClass.hbrBackground = g_controlBgBrush;
    return RegisterClassExW(&hotkeyClass) != 0;
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    InitializeCriticalSection(&g_audioLock);
    InitializeCriticalSection(&g_streamingSessionCs);
    InitCommonControls();
    g_appIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_appIcon) {
        g_appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    CreateUiResources();

    LoadConfig();
    g_enableDebugMode = g_config.enableDebugMode;

    if (g_config.enableDebugMode) {
        DebugModeOpenConsole();
    }

    if (g_config.asrBackend == L"local") {
        const std::wstring modelDir = g_config.modelDir.empty() ? DefaultModelDir(g_config.modelId) : g_config.modelDir;
        if (ModelDirExists(modelDir)) {
            const Config cfg = g_config;
            std::thread([cfg]() {
                PreloadAsrEngine(cfg);
                PostMessageW(g_mainWindow, kPreloadDoneMessage, 0, 0);
            }).detach();
        }
    }

    if (g_config.asrBackend == L"volcengine" && !g_config.volcApiKey.empty()) {
        std::thread([]() {
            volc_asr::PrewarmConnection(g_volcSession);
        }).detach();
    }

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VoxType.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"VoxType is already running.", kAppName, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
        DeleteCriticalSection(&g_streamingSessionCs);
        return 0;
    }

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"Failed to register window classes.", kAppName, MB_OK | MB_ICONERROR);
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
        DeleteCriticalSection(&g_streamingSessionCs);
        return 1;
    }

    g_mainWindow = CreateWindowExW(
        0,
        kMainClass,
        kAppName,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        320,
        240,
        nullptr,
        nullptr,
        instance,
        nullptr);

    if (!g_mainWindow) {
        MessageBoxW(nullptr, L"Failed to create main window.", kAppName, MB_OK | MB_ICONERROR);
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
        DeleteCriticalSection(&g_streamingSessionCs);
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    volc_asr::ClosePersistentConnection(g_volcSession);

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    DeleteUiResources();
    DeleteCriticalSection(&g_audioLock);
    DeleteCriticalSection(&g_streamingSessionCs);
    return static_cast<int>(msg.wParam);
}
