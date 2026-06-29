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
#include "doubao_ime_asr.h"
#include "doubao_ime_streaming_session.h"
#include "qwen_streaming_session.h"
#include "volcengine_streaming_session.h"
#include "volcengine_asr.h"
#include "streaming_vad_trimmer.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <deque>
#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <algorithm>
#include <utility>
#include <vector>
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
std::vector<HWND> g_doubaoImeControls;
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

static bool IsStreamingCloudBackend(const std::wstring& backend) {
    return backend == L"qwen" || backend == L"volcengine" || backend == L"doubao_ime";
}

static bool ShouldPreloadLocalAsr(const Config& config) {
    return config.asrBackend == L"local" || config.fallbackAsrBackend == L"local";
}

static Config LocalPreloadConfig(const Config& config) {
    Config localConfig = config;
    localConfig.asrBackend = L"local";
    return localConfig;
}

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

static void DebugPrintCloudVadTrim(const Config& config) {
    if (IsStreamingCloudBackend(config.asrBackend) &&
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

    if ((config.asrBackend == L"baidu" || config.asrBackend == L"qwen" || config.asrBackend == L"mimo") &&
        g_vadMs > 0 && g_vadTrimmedSamples > 0) {
        size_t rawBytes = g_lastPcmBytes > 0 ? g_lastPcmBytes
            : static_cast<size_t>(g_recordingMs * 32.0);
        DebugPrintVadTrimLine(rawBytes, g_vadTrimmedSamples);
    }
}

static void DebugPrintBatchVadTrim() {
    if (g_vadMs > 0 && g_vadTrimmedSamples > 0) {
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

void RefineWithLlmAsync(const AsrFinalMessage& finalMessage) {
    const std::wstring asrText = finalMessage.text;
    const Config config = finalMessage.resultConfig;
    llm::RequestConfig cfg;
    cfg.endpoint = config.llmEndpoint;
    cfg.apiKey = config.llmApiKey;
    cfg.model = config.llmModel;
    cfg.systemPrompt = config.llmPrompt;
    cfg.extraParams = config.llmExtraParams;
    bool debug = config.enableLlmDebug;
    std::thread([asrText, cfg, debug, finalMessage]() {
        HiResTimer tLlm;
        std::wstring result = llm::Refine(asrText, cfg);
        g_llmMs = tLlm.ElapsedMs();
        if (debug) {
            WriteLlmLog(asrText, result);
        }
        auto* msg = new LlmFinalMessage;
        msg->attemptId = finalMessage.attemptId;
        msg->text = result;
        msg->rawAsrText = asrText;
        msg->resultConfig = finalMessage.resultConfig;
        msg->usedFallback = finalMessage.usedFallback;
        msg->primaryBackend = finalMessage.primaryBackend;
        msg->primaryError = finalMessage.primaryError;
        if (!PostMessageW(g_mainWindow, kLlmResultMessage, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    }).detach();
}

static void ApplyBatchResultMetrics(const Config& config, const AsrSessionResult& result) {
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
}

static AsrSessionResult RunBatchAsrOnce(const Config& config,
                                        const std::vector<BYTE>& pcm,
                                        std::vector<float>&& localStreamingVadSamples) {
    AsrSessionResult result;
    result.providerName = AsrBackendDisplayName(config);
    result.pcmBytes = pcm.size();

    auto session = CreateBatchAsrSession(config, g_asrEngine, std::move(localStreamingVadSamples));
    std::wstring startError;
    if (!session || !session->Start(startError)) {
        result.text = startError.empty() ? L"ASR failed: session start failed" : startError;
        return result;
    }

    session->EnqueuePcmChunk(pcm.data(), pcm.size());
    return session->Finish();
}

static void PostFallbackHud(const Config& fallbackConfig) {
    PostMessageW(g_mainWindow, kHudUpdateMessage, 0,
                 reinterpret_cast<LPARAM>(new std::wstring(L"Fallback... " + AsrBackendDisplayName(fallbackConfig))));
}

static bool ShouldAcceptFinalMessage(uint64_t attemptId);

void RecognizeAsync(const std::vector<BYTE>& pcm, uint64_t attemptId) {
    const Config config = g_config;

    std::vector<float> localStreamingVadSamples;
    if (config.asrBackend != L"baidu" && !IsStreamingCloudBackend(config.asrBackend) &&
        config.asrBackend != L"mimo") {
        localStreamingVadSamples = std::move(g_streamingVadSamples);
        g_streamingVadSamples.clear();
    }

    std::thread([attemptId, config, pcm, localStreamingVadSamples = std::move(localStreamingVadSamples)]() mutable {
        AsrSessionResult selectedResult = RunBatchAsrOnce(config, pcm, std::move(localStreamingVadSamples));
        Config resultConfig = config;
        AsrFinalMetadata metadata;
        metadata.attemptId = attemptId;

        if (!ShouldAcceptFinalMessage(attemptId)) return;

        if (ShouldRunFallback(config, selectedResult.text, false, false)) {
            Config fallbackConfig = BuildFallbackConfig(config);
            PostFallbackHud(fallbackConfig);
            AsrSessionResult fallbackResult = RunBatchAsrOnce(fallbackConfig, pcm, {});
            metadata.usedFallback = true;
            metadata.primaryBackend = config.asrBackend;
            metadata.primaryError = NormalizeAsrText(selectedResult.text);
            metadata.fallbackBackend = fallbackConfig.asrBackend;
            resultConfig = fallbackConfig;

            if (ClassifyAsrResult(fallbackResult.text).kind == AsrResultKind::OperationalError) {
                selectedResult = fallbackResult;
                selectedResult.text = L"Fallback failed: " + AsrBackendDisplayName(fallbackConfig);
            } else {
                selectedResult = std::move(fallbackResult);
            }
        }

        if (!ShouldAcceptFinalMessage(attemptId)) return;
        ApplyBatchResultMetrics(resultConfig, selectedResult);
        DispatchAsrFinalText(g_mainWindow, selectedResult.text, resultConfig,
                             RefineWithLlmAsync, &g_lastRawAsrText, metadata);
    }).detach();
}

static std::unique_ptr<IStreamingAsrSession> TakeActiveStreamingSession();

struct AsrAttemptFinalMessage {
    uint64_t attemptId = 0;
    Config primaryConfig;
    std::wstring text;
    bool fromWatchdog = false;
};

struct RecognitionAttemptContext {
    uint64_t id = 0;
    Config primaryConfig;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    bool finalHandled = false;
    bool fallbackStarted = false;
};

static std::atomic<uint64_t> g_asrAttemptSeq{0};
static std::mutex g_asrAttemptMutex;
static RecognitionAttemptContext g_activeAttempt;

static uint64_t BeginAsrAttempt(const Config& config) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    g_activeAttempt = {};
    g_activeAttempt.id = ++g_asrAttemptSeq;
    g_activeAttempt.primaryConfig = config;
    return g_activeAttempt.id;
}

static uint64_t ActiveAsrAttemptId() {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    return g_activeAttempt.id;
}

static Config ActiveAsrAttemptConfig() {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    return g_activeAttempt.primaryConfig;
}

static bool IsActiveAsrAttempt(uint64_t attemptId) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    return attemptId != 0 && g_activeAttempt.id == attemptId;
}

static bool ShouldAcceptFinalMessage(uint64_t attemptId) {
    return attemptId == 0 || IsActiveAsrAttempt(attemptId);
}

static void StoreActiveAttemptPcm(uint64_t attemptId, const std::vector<BYTE>& pcm) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (g_activeAttempt.id == attemptId) {
        g_activeAttempt.pcm = std::make_shared<const std::vector<BYTE>>(pcm);
    }
}

static void MarkActiveAttemptFinalHandled(uint64_t attemptId) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (g_activeAttempt.id == attemptId) {
        g_activeAttempt.finalHandled = true;
    }
}

static bool TryBeginAttemptFinal(uint64_t attemptId,
                                 Config& primaryConfig,
                                 std::shared_ptr<const std::vector<BYTE>>& pcm,
                                 bool& fallbackAlreadyStarted) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (attemptId == 0 || g_activeAttempt.id != attemptId || g_activeAttempt.finalHandled) {
        return false;
    }
    g_activeAttempt.finalHandled = true;
    primaryConfig = g_activeAttempt.primaryConfig;
    pcm = g_activeAttempt.pcm;
    fallbackAlreadyStarted = g_activeAttempt.fallbackStarted;
    return true;
}

static void MarkAttemptFallbackStarted(uint64_t attemptId) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (g_activeAttempt.id == attemptId) {
        g_activeAttempt.fallbackStarted = true;
    }
}

static void StreamingFinalCallback(std::wstring text, const Config& config, void* userData) {
    auto* msg = new AsrAttemptFinalMessage;
    msg->attemptId = static_cast<uint64_t>(reinterpret_cast<UINT_PTR>(userData));
    msg->primaryConfig = config;
    msg->text = std::move(text);
    if (!PostMessageW(g_mainWindow, kAsrAttemptFinalMessage, 0, reinterpret_cast<LPARAM>(msg))) {
        delete msg;
    }
}

static void DispatchStreamingFallbackAsync(uint64_t attemptId,
                                           Config primaryConfig,
                                           std::shared_ptr<const std::vector<BYTE>> pcm,
                                           std::wstring primaryError) {
    if (!IsActiveAsrAttempt(attemptId)) return;
    Config fallbackConfig = BuildFallbackConfig(primaryConfig);
    MarkAttemptFallbackStarted(attemptId);
    PostFallbackHud(fallbackConfig);
    std::thread([attemptId, primaryConfig, fallbackConfig, pcm, primaryError = std::move(primaryError)]() {
        if (!IsActiveAsrAttempt(attemptId)) return;
        AsrSessionResult selectedResult = RunBatchAsrOnce(fallbackConfig, *pcm, {});
        AsrFinalMetadata metadata;
        metadata.attemptId = attemptId;
        metadata.usedFallback = true;
        metadata.primaryBackend = primaryConfig.asrBackend;
        metadata.primaryError = NormalizeAsrText(primaryError);
        metadata.fallbackBackend = fallbackConfig.asrBackend;

        if (ClassifyAsrResult(selectedResult.text).kind == AsrResultKind::OperationalError) {
            selectedResult.text = L"Fallback failed: " + AsrBackendDisplayName(fallbackConfig);
        }

        if (!IsActiveAsrAttempt(attemptId)) return;
        ApplyBatchResultMetrics(fallbackConfig, selectedResult);
        DispatchAsrFinalText(g_mainWindow, selectedResult.text, fallbackConfig,
                             RefineWithLlmAsync, &g_lastRawAsrText, metadata);
    }).detach();
}

static void HandleAsrAttemptFinal(AsrAttemptFinalMessage& msg) {
    Config primaryConfig = msg.primaryConfig;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    bool fallbackAlreadyStarted = false;
    if (!TryBeginAttemptFinal(msg.attemptId, primaryConfig, pcm, fallbackAlreadyStarted)) {
        return;
    }

    auto finishedSession = TakeActiveStreamingSession();
    (void)finishedSession;

    const std::wstring primaryText = NormalizeAsrText(msg.text);
    const bool canFallback = ShouldRunFallback(primaryConfig,
                                               primaryText,
                                               fallbackAlreadyStarted,
                                               false);
    if (canFallback && pcm && !pcm->empty()) {
        DispatchStreamingFallbackAsync(msg.attemptId, primaryConfig, pcm, primaryText);
        return;
    }

    AsrFinalMetadata metadata;
    metadata.attemptId = msg.attemptId;
    DispatchAsrFinalText(g_mainWindow, primaryText, primaryConfig,
                         RefineWithLlmAsync, &g_lastRawAsrText, metadata);
}

struct HudUpdateWithOptionsMessage {
    std::wstring statusLine;
    std::wstring text;
    float maxWidthDip = 0.0f;
    float maxScreenWidthFraction = 0.0f;
    int maxLines = 0;
    int fixedLines = 0;
    bool streamingPartial = false;
};

struct StreamingPartialHudCallbackContext {
    const wchar_t* statusLine = nullptr;
};

struct StreamingPartialHudState {
    bool clearPageMode = false;
    bool fixedHeightMode = false;
    size_t pageStart = 0;
    std::wstring body;
};

static StreamingPartialHudState g_streamingPartialHudState;

static void ResetStreamingPartialHudState() {
    g_streamingPartialHudState = {};
}

static bool IsStreamingPartialStrongBoundary(wchar_t c) {
    return c == L'\n' || c == L'\r' ||
           c == L'。' || c == L'！' || c == L'？' ||
           c == L'!' || c == L'?' ||
           c == L'；' || c == L';';
}

static bool IsStreamingPartialSoftBoundary(wchar_t c) {
    return c == L'，' || c == L',' || c == L'、' || c == L'：' || c == L':';
}

static size_t SkipStreamingPartialLeadingSeparators(const std::wstring& text, size_t pos) {
    while (pos < text.size() &&
           (iswspace(text[pos]) ||
            IsStreamingPartialStrongBoundary(text[pos]) ||
            IsStreamingPartialSoftBoundary(text[pos]))) {
        ++pos;
    }
    return pos;
}

static size_t SafeStreamingPartialSubstringStart(const std::wstring& text, size_t start) {
    if (start > 0 && start < text.size() && text[start] >= 0xDC00 && text[start] <= 0xDFFF) {
        --start;
    }
    return start;
}

static size_t AdvanceStreamingPartialSubstringStart(const std::wstring& text, size_t start) {
    if (start >= text.size()) return text.size();
    ++start;
    if (start < text.size() && text[start] >= 0xDC00 && text[start] <= 0xDFFF) {
        ++start;
    }
    return (std::min)(start, text.size());
}

static std::wstring BuildStreamingPartialHudText(const std::wstring& statusLine, const std::wstring& body) {
    return body.empty()
        ? statusLine
        : statusLine + L"\n" + body;
}

static UINT32 StreamingPartialHudLineCount(const std::wstring& statusLine, const std::wstring& body) {
    UINT32 lines = HudWrappedLineCount(BuildStreamingPartialHudText(statusLine, body),
                                       kStreamingPartialHudMaxWidthDip,
                                       kStreamingPartialHudMaxScreenFraction);
    if (lines == 0) {
        lines = static_cast<UINT32>(1 + (body.size() + 43) / 44);
    }
    return lines;
}

static std::wstring BuildStreamingHudPageBody(const std::wstring& text, size_t pageStart) {
    pageStart = (std::min)(pageStart, text.size());
    pageStart = SafeStreamingPartialSubstringStart(text, pageStart);
    return text.substr(pageStart);
}

static bool StreamingHudPageFits(const std::wstring& statusLine, const std::wstring& text, size_t pageStart) {
    return StreamingPartialHudLineCount(statusLine, BuildStreamingHudPageBody(text, pageStart)) <=
           static_cast<UINT32>(kStreamingPartialHudMaxLines);
}

static size_t FindStreamingCurrentSentenceStart(const std::wstring& statusLine,
                                                const std::wstring& text,
                                                size_t pageStart) {
    if (text.empty()) return 0;
    pageStart = (std::min)(pageStart, text.size());

    size_t scanEnd = text.size();
    while (scanEnd > pageStart && iswspace(text[scanEnd - 1])) {
        --scanEnd;
    }
    size_t contentEnd = scanEnd;
    while (scanEnd > pageStart &&
           (IsStreamingPartialStrongBoundary(text[scanEnd - 1]) ||
            IsStreamingPartialSoftBoundary(text[scanEnd - 1]))) {
        --scanEnd;
    }

    for (size_t i = scanEnd; i > pageStart; --i) {
        if (IsStreamingPartialStrongBoundary(text[i - 1])) {
            return SkipStreamingPartialLeadingSeparators(text, i);
        }
    }

    for (size_t i = scanEnd; i > pageStart; --i) {
        if (IsStreamingPartialSoftBoundary(text[i - 1])) {
            return SkipStreamingPartialLeadingSeparators(text, i);
        }
    }

    size_t candidate = contentEnd > kStreamingPartialHudTailChars
        ? contentEnd - kStreamingPartialHudTailChars
        : pageStart + 1;
    candidate = (std::min)(candidate, contentEnd);
    if (candidate <= pageStart && pageStart < text.size()) {
        candidate = pageStart + 1;
    }
    candidate = SafeStreamingPartialSubstringStart(text, candidate);
    candidate = SkipStreamingPartialLeadingSeparators(text, candidate);

    while (candidate < contentEnd && !StreamingHudPageFits(statusLine, text, candidate)) {
        candidate = AdvanceStreamingPartialSubstringStart(text, candidate);
        candidate = SkipStreamingPartialLeadingSeparators(text, candidate);
    }
    return (std::min)(candidate, text.size());
}

static std::wstring FormatStreamingPartialHudText(const std::wstring& statusLine, const std::wstring& text) {
    auto& state = g_streamingPartialHudState;
    if (state.pageStart > text.size()) {
        const bool keepFixedHeight = state.fixedHeightMode;
        g_streamingPartialHudState = {};
        g_streamingPartialHudState.fixedHeightMode = keepFixedHeight;
    }

    if (!state.clearPageMode &&
        StreamingPartialHudLineCount(statusLine, text) <= static_cast<UINT32>(kStreamingPartialHudMaxLines)) {
        return BuildStreamingPartialHudText(statusLine, text);
    }

    if (!StreamingHudPageFits(statusLine, text, state.pageStart)) {
        state.fixedHeightMode = true;
        state.pageStart = FindStreamingCurrentSentenceStart(statusLine, text, state.pageStart);
    }

    state.clearPageMode = state.pageStart > 0;
    if (state.clearPageMode) {
        state.fixedHeightMode = true;
    }
    state.body = BuildStreamingHudPageBody(text, state.pageStart);
    while (state.pageStart < text.size() &&
           StreamingPartialHudLineCount(statusLine, state.body) > static_cast<UINT32>(kStreamingPartialHudMaxLines)) {
        state.fixedHeightMode = true;
        state.pageStart = AdvanceStreamingPartialSubstringStart(text, state.pageStart);
        state.pageStart = SkipStreamingPartialLeadingSeparators(text, state.pageStart);
        state.body = BuildStreamingHudPageBody(text, state.pageStart);
    }
    return BuildStreamingPartialHudText(statusLine, state.body);
}

static void StreamingPartialHudCallback(const std::wstring& text, bool, void* userData) {
    if (text.empty()) return;
    const auto* ctx = static_cast<const StreamingPartialHudCallbackContext*>(userData);
    auto* msg = new HudUpdateWithOptionsMessage;
    msg->statusLine = (ctx && ctx->statusLine) ? ctx->statusLine : L"Listening...";
    msg->text = text;
    msg->maxWidthDip = kStreamingPartialHudMaxWidthDip;
    msg->maxScreenWidthFraction = kStreamingPartialHudMaxScreenFraction;
    msg->maxLines = kStreamingPartialHudMaxLines;
    msg->fixedLines = 0;
    msg->streamingPartial = true;
    if (!PostMessageW(g_mainWindow, kHudUpdateWithOptionsMessage, 0,
                      reinterpret_cast<LPARAM>(msg))) {
        delete msg;
    }
}

static StreamingPartialHudCallbackContext g_qwenPartialHudContext{L"Listening... Qwen ASR"};
static StreamingPartialHudCallbackContext g_doubaoImePartialHudContext{L"Listening... Doubao IME"};
static StreamingPartialHudCallbackContext g_volcenginePartialHudContext{L"Listening... Volcano Engine"};

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

static bool StartStreamingVadTrimmerForCloud(const wchar_t* debugPrefix, bool markReady = true) {
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

    g_streamingVadTrimmer = std::move(trimmer);
    g_streamingVadReady = markReady;
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

static void ReplayPreCapturedAudio(IStreamingAsrSession* session) {
    if (!session) return;
    std::vector<BYTE> preCaptured;
    EnterCriticalSection(&g_audioLock);
    preCaptured = g_audioData;
    LeaveCriticalSection(&g_audioLock);
    if (preCaptured.empty()) return;
    if (g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive()) {
        std::vector<std::vector<BYTE>> streamingOutputs;
        g_streamingVadTrimmer->ProcessPcm16(preCaptured.data(), preCaptured.size(), streamingOutputs);
        for (const auto& chunk : streamingOutputs) {
            if (!chunk.empty()) {
                session->EnqueuePcmChunk(chunk.data(), chunk.size());
            }
        }
        return;
    }
    session->EnqueuePcmChunk(preCaptured.data(), preCaptured.size());
}

void StartRecordingSession() {
    if (g_recording) return;
    if (g_hudWindow) KillTimer(g_hudWindow, kHudHideTimer);

    g_hudIsRefining = false;
    std::wstring name = AsrBackendDisplayName(g_config);
    ShowHud(L"Listening... " + name);

    std::wstring error;
    if (!StartAudioCapture(error)) {
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

    const uint64_t attemptId = BeginAsrAttempt(g_config);
    AbortAndResetActiveStreamingSession();
    ResetStreamingVadTrimmerState();
    ResetStreamingPartialHudState();

    if (g_config.asrBackend == L"qwen") {
        auto session = CreateQwenStreamingSession(g_config, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_qwenPartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            StopAudioCapture();
            g_recording = false;
            MarkActiveAttemptFinalHandled(attemptId);
            ShowHud(startError.empty() ? L"Qwen ASR error: session start failed" : startError);
            if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
            return;
        }

        StartStreamingVadTrimmerForCloud(L"Qwen thread", false);
        ReplayPreCapturedAudio(session.get());

        EnterCriticalSection(&g_streamingSessionCs);
        g_activeStreamingSession = std::move(session);
        g_streamingVadReady = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
        LeaveCriticalSection(&g_streamingSessionCs);

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

    if (g_config.asrBackend == L"doubao_ime") {
        auto session = CreateDoubaoImeStreamingSession(g_config, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_doubaoImePartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            StopAudioCapture();
            g_recording = false;
            MarkActiveAttemptFinalHandled(attemptId);
            ShowHud(startError.empty() ? L"Doubao IME ASR error: session start failed" : startError);
            if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
            return;
        }

        ReplayPreCapturedAudio(session.get());

        EnterCriticalSection(&g_streamingSessionCs);
        g_activeStreamingSession = std::move(session);
        LeaveCriticalSection(&g_streamingSessionCs);

        ShowHud(L"Listening... Doubao IME");
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
        VolcengineResetForNewSession();

        auto session = CreateVolcengineStreamingSession(g_config, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_volcenginePartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            StopAudioCapture();
            g_recording = false;
            MarkActiveAttemptFinalHandled(attemptId);
            AsrFinalMetadata metadata;
            metadata.attemptId = attemptId;
            DispatchAsrFinalText(g_mainWindow, startError, g_config,
                                 RefineWithLlmAsync, &g_lastRawAsrText, metadata);
            return;
        }

        StartStreamingVadTrimmerForCloud(L"Volc thread", false);
        ReplayPreCapturedAudio(session.get());

        EnterCriticalSection(&g_streamingSessionCs);
        g_activeStreamingSession = std::move(session);
        g_streamingVadReady = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
        LeaveCriticalSection(&g_streamingSessionCs);

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

    g_streamingVadReady = false;
    g_streamingVadSamples.clear();
    if (g_config.asrBackend != L"baidu" && !IsStreamingCloudBackend(g_config.asrBackend) &&
        g_config.asrBackend != L"mimo" &&
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
    const uint64_t attemptId = ActiveAsrAttemptId();

    if (IsStreamingCloudBackend(g_config.asrBackend) && HasActiveStreamingSession()) {
        const std::vector<BYTE> pcm = StopAudioCapture();
        StoreActiveAttemptPcm(attemptId, pcm);
        g_lastPcmBytes = pcm.size();
        if (pcm.size() < 8000) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            MarkActiveAttemptFinalHandled(attemptId);
            AbortAndResetActiveStreamingSession();
            ShowHud(L"Too short");
            SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
            return;
        }
        FinishStreamingVadTrimmer();
        if (StreamingVadTrimSawNoSpeech()) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            MarkActiveAttemptFinalHandled(attemptId);
            AbortAndResetActiveStreamingSession();
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
        DWORD finalizeTimeout = IsFallbackAsrEnabled(g_config)
            ? ComputeCloudAsrStreamingFinalWaitMs(g_recordingMs, pcm.size())
            : ComputeCloudAsrLegacyFinalizeTimeoutMs(g_recordingMs, pcm.size());
        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession) {
            g_activeStreamingSession->StopInput(g_recordingMs, pcm.size());
            finalizeTimeout = g_activeStreamingSession->CurrentWatchdogMs();
        }
        LeaveCriticalSection(&g_streamingSessionCs);
        KillTimer(g_mainWindow, kStreamingWatchdogTimer);
        SetTimer(g_mainWindow, kStreamingWatchdogTimer, finalizeTimeout, nullptr);
        if (g_config.asrBackend == L"volcengine") {
            VolcDebugLog("Volc watchdog: finalize timeout reset to %ums (recording=%.0fms, pcm=%zu)",
                         finalizeTimeout, g_recordingMs, pcm.size());
        }
        ShowHud(L"Recognizing... " + AsrBackendDisplayName(g_config));
        return;
    }

    const std::vector<BYTE> pcm = StopAudioCapture();
    if (pcm.size() < 8000) {
        MarkActiveAttemptFinalHandled(attemptId);
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
            MarkActiveAttemptFinalHandled(attemptId);
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
    }

    std::wstring name = AsrBackendDisplayName(g_config);
    ShowHud(L"Recognizing... " + name);
    RecognizeAsync(pcm, attemptId);
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
        if (ShouldPreloadLocalAsr(g_config)) {
            const Config cfg = LocalPreloadConfig(g_config);
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
    case kHudUpdateWithOptionsMessage: {
        std::unique_ptr<HudUpdateWithOptionsMessage> msg(
            reinterpret_cast<HudUpdateWithOptionsMessage*>(lParam));
        if (msg) {
            const std::wstring text = msg->streamingPartial
                ? FormatStreamingPartialHudText(msg->statusLine, msg->text)
                : msg->text;
            const int fixedLines = msg->streamingPartial && g_streamingPartialHudState.fixedHeightMode
                ? kStreamingPartialHudMaxLines
                : msg->fixedLines;
            ShowHudConstrained(text,
                               msg->maxWidthDip,
                               msg->maxScreenWidthFraction,
                               msg->maxLines,
                               fixedLines);
        }
        return 0;
    }
    case kDoubaoImeCredentialsMessage: {
        std::unique_ptr<doubao_ime_asr::CredentialsUpdateMessage> update(
            reinterpret_cast<doubao_ime_asr::CredentialsUpdateMessage*>(lParam));
        if (update) {
            if (update->clear) {
                g_config.doubaoImeDeviceId.clear();
                g_config.doubaoImeCdid.clear();
                g_config.doubaoImeToken.clear();
            } else {
                g_config.doubaoImeDeviceId = update->credentials.deviceId;
                g_config.doubaoImeCdid = update->credentials.cdid;
                g_config.doubaoImeToken = update->credentials.token;
            }
            SaveConfig();
            if (g_settingsWindow && IsWindow(g_settingsWindow)) {
                PostMessageW(g_settingsWindow, kDoubaoImeSettingsRefreshMessage, 0, 0);
            }
        }
        return 0;
    }
    case kAsrAttemptFinalMessage: {
        std::unique_ptr<AsrAttemptFinalMessage> result(
            reinterpret_cast<AsrAttemptFinalMessage*>(lParam));
        if (result) {
            HandleAsrAttemptFinal(*result);
        }
        return 0;
    }
    case kAsrResultMessage: {
        std::unique_ptr<AsrFinalMessage> result(reinterpret_cast<AsrFinalMessage*>(lParam));
        if (result && !ShouldAcceptFinalMessage(result->attemptId)) {
            return 0;
        }
        KillTimer(g_mainWindow, kStreamingWatchdogTimer);
        const Config resultConfig = result ? result->resultConfig : g_config;
        const std::wstring text = NormalizeAsrText(result ? result->text : L"ASR failed");
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

                if (resultConfig.enableDebugMode) {
                    DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

                    DebugPrintInputContext();

                    if (result && result->usedFallback && !result->primaryError.empty()) {
                        DebugPrintTextLine(L"Primary failed", result->primaryError);
                    }

                    if (resultConfig.asrBackend == L"local") {
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
                        const char* backend = AsrBackendDebugName(resultConfig.asrBackend);
                        printf("  Pipeline: %s %.0f | Paste %.0f = Total %.0fms\n",
                               backend, g_cloudApiMs, pasteMs, g_cloudApiMs + pasteMs);
                        if (result && result->usedFallback) {
                            DebugPrintBatchVadTrim();
                        } else {
                            DebugPrintCloudVadTrim(resultConfig);
                        }
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
        std::unique_ptr<LlmFinalMessage> result(reinterpret_cast<LlmFinalMessage*>(lParam));
        if (result && !ShouldAcceptFinalMessage(result->attemptId)) {
            return 0;
        }
        const Config resultConfig = result ? result->resultConfig : g_config;
        const std::wstring text = result ? result->text : L"LLM failed";
        g_hudIsRefining = false;
        ShowHud(text);
        if (!text.empty() && text.rfind(L"LLM failed:", 0) != 0) {
            HiResTimer tPaste;
            PasteTextImeAware(text);
            double pasteMs = tPaste.ElapsedMs();

            if (resultConfig.enableDebugMode) {
                DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

                DebugPrintInputContext();

                if (result && result->usedFallback && !result->primaryError.empty()) {
                    DebugPrintTextLine(L"Primary failed", result->primaryError);
                }

                if (resultConfig.asrBackend == L"local") {
                    printf("  Pipeline: ");
                    if (g_vadMs > 0) printf("VAD(%ls) %.0f | ", g_vadModelName.c_str(), g_vadMs);
                    printf("ASR %.0f", g_asrDecodeMs);
                    if (g_punctMs > 0) printf(" | Punct %.0f", g_punctMs);
                    printf(" | LLM %.0f | Paste %.0f = Total %.0fms\n",
                           g_llmMs, pasteMs,
                           g_vadMs + g_asrDecodeMs + g_punctMs + g_llmMs + pasteMs);
                } else {
                    const char* backend = AsrBackendDebugName(resultConfig.asrBackend);
                    printf("  Pipeline: %s %.0f | LLM %.0f | Paste %.0f = Total %.0fms\n",
                           backend, g_cloudApiMs, g_llmMs, pasteMs,
                           g_cloudApiMs + g_llmMs + pasteMs);
                    if (result && result->usedFallback) {
                        DebugPrintBatchVadTrim();
                    } else {
                        DebugPrintCloudVadTrim(resultConfig);
                    }
                }

                DebugPrintTextLine(L"ASR", result ? result->rawAsrText : g_lastRawAsrText);
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
                const uint64_t attemptId = ActiveAsrAttemptId();
                const Config primaryConfig = ActiveAsrAttemptConfig();
                std::wstring providerName = session->ProviderName();
                session->Abort();
                std::wstring timeoutText = std::wstring(providerName) + L" error: timeout";
                if (providerName == L"Volcano Engine") {
                    timeoutText = L"ASR failed: VolcEngine timeout";
                }
                auto* msg = new AsrAttemptFinalMessage;
                msg->attemptId = attemptId;
                msg->primaryConfig = primaryConfig;
                msg->text = std::move(timeoutText);
                msg->fromWatchdog = true;
                if (!PostMessageW(hwnd, kAsrAttemptFinalMessage, 0,
                                  reinterpret_cast<LPARAM>(msg))) {
                    delete msg;
                }
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
        MarkActiveAttemptFinalHandled(ActiveAsrAttemptId());
        AbortAndResetActiveStreamingSession();
        VolcengineForceAbortAndCloseAll();
        g_captureActive = false;
        StopAudioCapture();
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

    if (ShouldPreloadLocalAsr(g_config)) {
        const Config localConfig = LocalPreloadConfig(g_config);
        const std::wstring modelDir = localConfig.modelDir.empty() ? DefaultModelDir(localConfig.modelId) : localConfig.modelDir;
        if (ModelDirExists(modelDir)) {
            const Config cfg = localConfig;
            std::thread([cfg]() {
                PreloadAsrEngine(cfg);
                PostMessageW(g_mainWindow, kPreloadDoneMessage, 0, 0);
            }).detach();
        }
    }

    if (g_config.asrBackend == L"volcengine" && !g_config.volcApiKey.empty()) {
        std::thread([]() {
            VolcenginePrewarmConnection();
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

    VolcengineClosePersistentConnection();

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    DeleteUiResources();
    DeleteCriticalSection(&g_audioLock);
    DeleteCriticalSection(&g_streamingSessionCs);
    return static_cast<int>(msg.wParam);
}
