#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "globals.h"
#include "engine.h"
#include "hud.h"
#include "hotkey.h"
#include "settings.h"
#include "input_context.h"
#include "qwen_context.h"
#include "asr_session.h"
#include "asr_diagnostics.h"
#include "asr_streaming_session.h"
#include "asr_result.h"
#include "asr_runtime_log.h"
#include "cloud_asr_common.h"
#include "asr_dispatcher.h"
#include "doubao_ime_asr.h"
#include "doubao_ime_streaming_session.h"
#include "qwen_streaming_session.h"
#include "qwen_audio_streaming_session.h"
#include "qwen_free_streaming_session.h"
#include "qwen_audio_profile.h"
#include "volcengine_streaming_session.h"
#include "volcengine_asr.h"
#include "streaming_vad_trimmer.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <condition_variable>
#include <chrono>
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

static void WriteDiagnosticAudioRuntimeLog(const std::string& line) {
    asr_runtime_log::Write("%s", line.c_str());
}

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
std::wstring g_hudText = L"Ready";
HWAVEIN g_waveIn = nullptr;
WAVEHDR g_waveHeaders[4] = {};
std::vector<std::vector<BYTE>> g_waveBuffers;
std::vector<BYTE> g_audioData;
CRITICAL_SECTION g_audioLock;
bool g_captureActive = false;
std::atomic<uint64_t> g_audioCaptureGeneration{0};
std::atomic<bool> g_audioCaptureFailurePending{false};
std::atomic<DWORD> g_audioCaptureFailureCode{0};
std::atomic<bool> g_audioCaptureFailureWasapi{false};
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
std::vector<HWND> g_qwenAudio3Controls;
std::vector<HWND> g_qwenAudioStreamingOnlyControls;
std::vector<HWND> g_mimoControls;
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

static bool IsStreamingCloudBackend(const Config& config) {
    if (config.asrBackend == L"qwen" &&
        config.qwenModel == L"qwen-audio-3.0-asr-flash") {
        return false;
    }
    return config.asrBackend == L"qwen" || config.asrBackend == L"volcengine" ||
           config.asrBackend == L"doubao_ime" || config.asrBackend == L"qwen_free";
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
    if (g_config.volcEnableInputContext || g_config.qwenEnableInputContext) {
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
    if (IsStreamingCloudBackend(config) &&
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
    const std::wstring logDir = LogDir();

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
        msg->allowCancelledAttempt = finalMessage.allowCancelledAttempt;
        msg->bundledPostProcessApplied = finalMessage.bundledPostProcessApplied;
        msg->text = result;
        msg->rawAsrText = asrText;
        msg->resultConfig = finalMessage.resultConfig;
        msg->usedFallback = finalMessage.usedFallback;
        msg->primaryBackend = finalMessage.primaryBackend;
        msg->primaryError = finalMessage.primaryError;
        msg->fallbackBackend = finalMessage.fallbackBackend;
        msg->selection = finalMessage.selection;
        if (!PostMessageW(g_mainWindow, kLlmResultMessage, 0, reinterpret_cast<LPARAM>(msg))) {
            delete msg;
        }
    }).detach();
}

static void ApplyBatchResultMetrics(const Config& config, const AsrSessionResult& result) {
    g_lastPcmBytes = result.pcmBytes;
    if (result.isStreaming ||
        result.backend == AsrSessionBackend::BaiduBatch ||
        result.backend == AsrSessionBackend::QwenRealtimeBatch ||
        result.backend == AsrSessionBackend::QwenAudioBatch ||
        result.backend == AsrSessionBackend::MimoBatch ||
        result.backend == AsrSessionBackend::DoubaoImeRecorded) {
        g_cloudApiMs = result.cloudApiMs;
    }
    if (config.enableDebugMode && result.vadTrimmedSamples > 0) {
        g_vadTrimmedSamples = result.vadTrimmedSamples;
        g_vadMs = result.vadMs;
        g_vadModelName = result.vadModelName;
    }
}

static void PostDoubaoImeCredentialsUpdate(const doubao_ime_asr::Credentials& credentials, bool clear) {
    auto* update = new doubao_ime_asr::CredentialsUpdateMessage;
    update->credentials = credentials;
    update->clear = clear;
    if (!PostMessageW(g_mainWindow, kDoubaoImeCredentialsMessage, 0,
                      reinterpret_cast<LPARAM>(update))) {
        delete update;
    }
}

static void ApplyAsrSessionSideEffects(const AsrSessionResult& result) {
    if (result.doubaoImeClearCredentials) {
        PostDoubaoImeCredentialsUpdate({}, true);
    }
    if (result.doubaoImeCredentialsChanged) {
        doubao_ime_asr::Credentials credentials;
        credentials.deviceId = result.doubaoImeDeviceId;
        credentials.cdid = result.doubaoImeCdid;
        credentials.token = result.doubaoImeToken;
        PostDoubaoImeCredentialsUpdate(credentials, false);
    }
}

struct OneShotStreamingResult {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    bool bundledPostProcessApplied = false;
    std::wstring text;
};

static void CaptureOneShotStreamingFinal(std::wstring text,
                                          const Config& /*config*/,
                                          bool bundledPostProcessApplied,
                                          void* userData) {
    auto* result = static_cast<OneShotStreamingResult*>(userData);
    if (!result) return;
    {
        std::lock_guard<std::mutex> lock(result->mutex);
        if (result->done) return;
        result->text = std::move(text);
        result->bundledPostProcessApplied = bundledPostProcessApplied;
        result->done = true;
    }
    result->cv.notify_one();
}

static std::unique_ptr<IStreamingAsrSession> CreateStreamingSessionForOneShot(
    const Config& config) {
    if (config.asrBackend == L"qwen") {
        if (config.qwenModel == L"qwen-audio-3.0-asr-flash-streaming") {
            return CreateQwenAudioStreamingSession(config, g_mainWindow, nullptr, nullptr);
        }
        return CreateQwenStreamingSession(config, g_mainWindow, nullptr, nullptr);
    }
    if (config.asrBackend == L"doubao_ime") {
        return CreateDoubaoImeStreamingSession(config, g_mainWindow, nullptr, nullptr);
    }
    if (config.asrBackend == L"qwen_free") {
        return CreateQwenFreeStreamingSession(config, g_mainWindow, nullptr, nullptr, {});
    }
    if (config.asrBackend == L"volcengine") {
        VolcengineResetForNewSession();
        return CreateVolcengineStreamingSession(config, g_mainWindow, nullptr, nullptr);
    }
    return nullptr;
}

static AsrSessionResult RunStreamingAsrOnce(const Config& config,
                                             const std::vector<BYTE>& pcm) {
    AsrSessionResult result;
    result.providerName = AsrBackendDisplayName(config);
    result.pcmBytes = pcm.size();
    result.isStreaming = true;

    OneShotStreamingResult collected;
    auto session = CreateStreamingSessionForOneShot(config);
    if (!session) {
        result.text = L"ASR failed: streaming backend is not available";
        asr_diagnostics::CompleteIfMissingFromText(config, result.text);
        return result;
    }

    session->SetFinalCallback(CaptureOneShotStreamingFinal, &collected);
    const ULONGLONG startedTick = GetTickCount64();
    std::wstring startError;
    if (!session->Start(startError)) {
        result.text = startError.empty()
            ? L"ASR failed: streaming session start failed"
            : std::move(startError);
        session->Abort();
        result.cloudApiMs = static_cast<double>(GetTickCount64() - startedTick);
        asr_diagnostics::CompleteIfMissingFromText(
            config, result.text, "session_start_failed", result.cloudApiMs);
        return result;
    }

    if (!pcm.empty() && !session->EnqueuePcmChunk(pcm.data(), pcm.size())) {
        result.text = L"ASR failed: streaming fallback audio enqueue failed";
        session->Abort();
        result.cloudApiMs = static_cast<double>(GetTickCount64() - startedTick);
        asr_diagnostics::CompleteIfMissingFromText(
            config, result.text, "audio_enqueue_failed", result.cloudApiMs);
        return result;
    }

    // The recorded PCM is 16 kHz, mono, signed 16-bit: 32 bytes per ms.
    session->StopInput(static_cast<double>(pcm.size()) / 32.0, pcm.size());
    DWORD waitMs = session->CurrentWatchdogMs();
    // A streaming fallback may include one bounded reconnect + full PCM
    // replay before its provider-specific final/post-process wait completes.
    waitMs = (std::max<DWORD>)(1000, (std::min<DWORD>)(waitMs, 60000));
    bool completed = false;
    {
        std::unique_lock<std::mutex> lock(collected.mutex);
        completed = collected.cv.wait_for(
            lock, std::chrono::milliseconds(waitMs),
            [&collected]() { return collected.done; });
        if (completed) {
            result.text = collected.text;
            result.bundledPostProcessApplied =
                collected.bundledPostProcessApplied;
        }
    }

    if (!completed) {
        result.text = L"ASR failed: streaming fallback timeout";
    }
    session->Abort();
    result.cloudApiMs = static_cast<double>(GetTickCount64() - startedTick);
    asr_diagnostics::CompleteIfMissingFromText(
        config, result.text, "provider_final", result.cloudApiMs);
    return result;
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
        asr_diagnostics::CompleteIfMissingFromText(
            config, result.text, "session_start_failed");
        return result;
    }

    session->EnqueuePcmChunk(pcm.data(), pcm.size());
    result = session->Finish();
    asr_diagnostics::CompleteIfMissingFromText(
        config, result.text, "provider_final", result.cloudApiMs);
    return result;
}

static AsrSessionResult RunConfiguredAsrOnce(
    const Config& config,
    const std::vector<BYTE>& pcm,
    std::vector<float>&& localStreamingVadSamples) {
    if (IsStreamingCloudBackend(config)) {
        return RunStreamingAsrOnce(config, pcm);
    }
    return RunBatchAsrOnce(config, pcm, std::move(localStreamingVadSamples));
}

static void PostFallbackHud(const Config& fallbackConfig) {
    auto* text = new std::wstring(L"Fallback... " + AsrBackendDisplayName(fallbackConfig));
    if (!PostMessageW(g_mainWindow, kHudUpdateMessage, 0,
                      reinterpret_cast<LPARAM>(text))) {
        delete text;
    }
}

static bool ShouldAcceptFinalMessage(uint64_t attemptId,
                                     bool allowCancelled = false);

static unsigned long long ElapsedSinceTick(ULONGLONG startTick) {
    if (startTick == 0) return 0;
    return static_cast<unsigned long long>(GetTickCount64() - startTick);
}

static void LogAsrResultEvent(const char* event,
                              uint64_t attemptId,
                              const std::wstring& backend,
                              const AsrResultClassification& classification,
                              const char* source,
                              ULONGLONG startTick,
                              bool accepted) {
    asr_runtime_log::Write(
        "event=%s attempt=%llu backend=%s kind=%s reason=%s source=%s elapsed_ms=%llu accepted=%d",
        event,
        static_cast<unsigned long long>(attemptId),
        AsrBackendLogName(backend),
        AsrResultKindDebugName(classification.kind),
        AsrFailureReasonDebugName(classification.reason),
        source,
        ElapsedSinceTick(startTick),
        accepted ? 1 : 0);
}

void RecognizeAsync(const std::vector<BYTE>& pcm,
                    uint64_t attemptId,
                    Config config) {
    std::vector<float> localStreamingVadSamples;
    if (config.asrBackend != L"baidu" && !IsStreamingCloudBackend(config) &&
        config.asrBackend != L"mimo") {
        localStreamingVadSamples = std::move(g_streamingVadSamples);
        g_streamingVadSamples.clear();
    }

    std::thread([attemptId, config, pcm, localStreamingVadSamples = std::move(localStreamingVadSamples)]() mutable {
        const ULONGLONG primaryStartTick = GetTickCount64();
        AsrSessionResult selectedResult = RunBatchAsrOnce(config, pcm, std::move(localStreamingVadSamples));
        Config resultConfig = config;
        AsrFinalMetadata metadata;
        metadata.attemptId = attemptId;

        const AsrResultClassification primaryClassification = ClassifyAsrResult(selectedResult.text);
        const bool primaryAccepted = ShouldAcceptFinalMessage(attemptId);
        LogAsrResultEvent("primary_final", attemptId, config.asrBackend,
                          primaryClassification, "batch", primaryStartTick, primaryAccepted);
        if (!primaryAccepted) {
            asr_runtime_log::Write("event=primary_discarded_stale attempt=%llu source=batch",
                                   static_cast<unsigned long long>(attemptId));
            return;
        }

        if (ShouldRunFallback(config, selectedResult.text, false, false)) {
            Config fallbackConfig = BuildFallbackConfig(config);
            asr_runtime_log::Write(
                "event=fallback_start attempt=%llu primary=%s fallback=%s reason=%s pcm_bytes=%zu",
                static_cast<unsigned long long>(attemptId),
                AsrBackendLogName(config.asrBackend),
                AsrBackendLogName(fallbackConfig.asrBackend),
                AsrFailureReasonDebugName(primaryClassification.reason),
                pcm.size());
            PostFallbackHud(fallbackConfig);
            const ULONGLONG fallbackStartTick = GetTickCount64();
            AsrSessionResult fallbackResult = RunConfiguredAsrOnce(fallbackConfig, pcm, {});
            metadata.usedFallback = true;
            metadata.primaryBackend = config.asrBackend;
            metadata.primaryError = NormalizeAsrText(selectedResult.text);
            metadata.fallbackBackend = fallbackConfig.asrBackend;
            resultConfig = fallbackConfig;

            const AsrResultClassification fallbackClassification = ClassifyAsrResult(fallbackResult.text);
            const bool fallbackAccepted = ShouldAcceptFinalMessage(attemptId);
            LogAsrResultEvent("fallback_final", attemptId, fallbackConfig.asrBackend,
                              fallbackClassification, "batch", fallbackStartTick, fallbackAccepted);
            if (!fallbackAccepted) {
                asr_runtime_log::Write("event=fallback_discarded_stale attempt=%llu stage=completed",
                                       static_cast<unsigned long long>(attemptId));
                return;
            }

            if (fallbackClassification.kind == AsrResultKind::OperationalError) {
                selectedResult = fallbackResult;
                selectedResult.text = L"Fallback failed: " + AsrBackendDisplayName(fallbackConfig);
            } else {
                selectedResult = std::move(fallbackResult);
            }
        }

        if (!ShouldAcceptFinalMessage(attemptId)) {
            asr_runtime_log::Write("event=final_dispatch_discarded_stale attempt=%llu",
                                   static_cast<unsigned long long>(attemptId));
            return;
        }
        ApplyAsrSessionSideEffects(selectedResult);
        ApplyBatchResultMetrics(resultConfig, selectedResult);
        DispatchAsrFinalText(g_mainWindow, selectedResult.text, resultConfig,
                             RefineWithLlmAsync, &g_lastRawAsrText, metadata);
    }).detach();
}

static std::unique_ptr<IStreamingAsrSession> TakeActiveStreamingSession();

enum class AsrAttemptFinalSource {
    Callback,
    Watchdog,
    SessionStart,
};

static const char* AsrAttemptFinalSourceName(AsrAttemptFinalSource source) {
    switch (source) {
    case AsrAttemptFinalSource::Callback: return "callback";
    case AsrAttemptFinalSource::Watchdog: return "watchdog";
    case AsrAttemptFinalSource::SessionStart: return "session_start";
    }
    return "unknown";
}

struct AsrAttemptFinalMessage {
    uint64_t attemptId = 0;
    Config primaryConfig;
    std::wstring text;
    AsrAttemptFinalSource source = AsrAttemptFinalSource::Callback;
    bool allowCancelledAttempt = false;
    bool bundledPostProcessApplied = false;
    SelectionContext selection;
};

struct RecognitionAttemptContext {
    uint64_t id = 0;
    Config primaryConfig;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    ULONGLONG startedTick = 0;
    ULONGLONG stoppedTick = 0;
    bool recordingStopped = false;
    bool finalHandled = false;
    // cancelled rejects late primary callbacks/results after Abort, while
    // invalidated additionally prevents watchdog recovery from continuing
    // (used for window teardown or a superseded attempt).
    bool cancelled = false;
    bool invalidated = false;
    bool fallbackStarted = false;
    bool hasDeferredFinal = false;
    std::wstring deferredFinalText;
    Config deferredFinalConfig;
    AsrAttemptFinalSource deferredFinalSource = AsrAttemptFinalSource::Callback;
    bool deferredAllowCancelledAttempt = false;
    bool deferredBundledPostProcessApplied = false;
    SelectionContext selection;
};

static std::atomic<uint64_t> g_asrAttemptSeq{0};
static std::mutex g_asrAttemptMutex;
static RecognitionAttemptContext g_activeAttempt;

static uint64_t BeginAsrAttempt(const Config& config,
                                SelectionContext selection = {}) {
    const uint64_t attemptId = ++g_asrAttemptSeq;
    Config attemptConfig = config;
    attemptConfig.asrAttemptId = attemptId;
    if (config.asrBackend == L"qwen" && config.qwenEnableInputContext &&
        (config.qwenModel == qwen_audio_profile::kHttpModel ||
         config.qwenModel == qwen_audio_profile::kStreamingModel)) {
        attemptConfig.qwenInputContextSnapshotCaptured = true;
        attemptConfig.qwenInputContextSnapshot =
            qwen_context::CaptureInputFieldText(&g_inputContextResult);
        asr_runtime_log::Write(
            "event=input_context_snapshot captured=%d chars=%zu layer=%d timed_out=%d password=%d",
            attemptConfig.qwenInputContextSnapshot.empty() ? 0 : 1,
            attemptConfig.qwenInputContextSnapshot.size(),
            g_inputContextResult.successLayer,
            g_inputContextResult.timedOut ? 1 : 0,
            g_inputContextResult.isPassword ? 1 : 0);
    }
    const audio_diagnostics::AttemptMetadata diagnosticMetadata =
        asr_diagnostics::MakeAttemptMetadata(attemptConfig);
    uint64_t supersededAttemptId = 0;
    {
        std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
        supersededAttemptId = g_activeAttempt.id;
        g_activeAttempt = {};
        g_activeAttempt.id = attemptId;
        g_activeAttempt.primaryConfig = std::move(attemptConfig);
        g_activeAttempt.selection = std::move(selection);
        g_activeAttempt.startedTick = GetTickCount64();
    }
    if (supersededAttemptId != 0 && supersededAttemptId != attemptId) {
        audio_diagnostics::DiscardAttempt(supersededAttemptId, "superseded");
    }
    audio_diagnostics::BeginAttempt(diagnosticMetadata);
    asr_runtime_log::Write(
        "event=attempt_start attempt=%llu primary=%s fallback=%s",
        static_cast<unsigned long long>(attemptId),
        AsrBackendLogName(config.asrBackend),
        IsFallbackAsrEnabled(config) ? AsrBackendLogName(config.fallbackAsrBackend) : "none");
    return attemptId;
}

static uint64_t RecordCaptureStartFailure(
    const Config& config,
    const AudioCaptureStartFailure& failure) {
    const uint64_t attemptId = ++g_asrAttemptSeq;
    Config attemptConfig = config;
    attemptConfig.asrAttemptId = attemptId;
    audio_diagnostics::BeginAttempt(
        asr_diagnostics::MakeAttemptMetadata(attemptConfig));

    audio_diagnostics::CaptureSnapshot capture;
    capture.device.backend = failure.terminalBackend.empty()
        ? config.audioBackend : failure.terminalBackend;
    capture.device.deviceName = failure.deviceName;
    capture.device.deviceId = failure.deviceId;
    capture.device.usedDefaultDevice = failure.usedDefaultDevice;
    capture.device.nativeSampleRate = failure.nativeSampleRate;
    capture.device.nativeChannels = failure.nativeChannels;
    capture.device.nativeBitsPerSample = failure.nativeBitsPerSample;
    capture.device.nativeIsFloat = failure.nativeIsFloat;
    audio_diagnostics::AttachCapture(
        attemptId,
        std::make_shared<const std::vector<BYTE>>(),
        capture);

    audio_diagnostics::StageMetadata stage;
    stage.kind = audio_diagnostics::StageKind::Primary;
    stage.backend = L"audio_capture";
    stage.transport = failure.attemptedBackends.empty()
        ? capture.device.backend : failure.attemptedBackends;
    stage.reason = L"capture_start_" +
        (failure.phase.empty() ? std::wstring(L"unknown") : failure.phase);

    audio_diagnostics::StageTerminal terminal;
    terminal.terminal = "capture_start_failure";
    terminal.reason = WideToUtf8(stage.reason);
    terminal.providerCode = std::to_string(
        static_cast<unsigned long>(failure.code));
    terminal.errorCategory = "capture";
    audio_diagnostics::CompleteStageIfMissing(attemptId, stage, terminal);

    audio_diagnostics::FinalResult finalResult;
    finalResult.kind = audio_diagnostics::FinalKind::CaptureFailure;
    finalResult.reason = terminal.reason;
    finalResult.terminal = terminal.terminal;
    audio_diagnostics::FinalizeAttempt(attemptId, finalResult);
    return attemptId;
}

static uint64_t ActiveAsrAttemptId() {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    return g_activeAttempt.id;
}

static Config ActiveAsrAttemptConfig() {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    return g_activeAttempt.primaryConfig;
}

static bool IsActiveAsrAttempt(uint64_t attemptId,
                               bool allowCancelled = false) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (attemptId == 0 || g_activeAttempt.id != attemptId ||
        g_activeAttempt.invalidated) {
        return false;
    }
    return allowCancelled || !g_activeAttempt.cancelled;
}

static bool ShouldAcceptFinalMessage(uint64_t attemptId,
                                     bool allowCancelled) {
    return attemptId == 0 || IsActiveAsrAttempt(attemptId, allowCancelled);
}

static void CancelActiveAsrAttempt(uint64_t attemptId, bool invalidate) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
        if (attemptId != 0 && g_activeAttempt.id == attemptId) {
            g_activeAttempt.cancelled = true;
            if (invalidate) g_activeAttempt.invalidated = true;
            changed = true;
        }
    }
    if (changed) {
        asr_runtime_log::Write(
            "event=attempt_cancel attempt=%llu invalidate=%d",
            static_cast<unsigned long long>(attemptId), invalidate ? 1 : 0);
    }
}

static std::unique_ptr<AsrAttemptFinalMessage> CompleteActiveAttemptRecording(
    uint64_t attemptId,
    const std::vector<BYTE>* pcm,
    double recordingMs,
    size_t pcmBytes) {
    std::unique_ptr<AsrAttemptFinalMessage> deferred;
    std::shared_ptr<const std::vector<BYTE>> capturedPcm;
    bool completed = false;
    {
        std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
        if (g_activeAttempt.id == attemptId && !g_activeAttempt.recordingStopped) {
            g_activeAttempt.recordingStopped = true;
            g_activeAttempt.stoppedTick = GetTickCount64();
            if (pcm) {
                capturedPcm = std::make_shared<const std::vector<BYTE>>(*pcm);
                g_activeAttempt.pcm = capturedPcm;
            }
            if (g_activeAttempt.hasDeferredFinal) {
                deferred = std::make_unique<AsrAttemptFinalMessage>();
                deferred->attemptId = attemptId;
                deferred->primaryConfig = g_activeAttempt.deferredFinalConfig;
                deferred->text = std::move(g_activeAttempt.deferredFinalText);
                deferred->source = g_activeAttempt.deferredFinalSource;
                deferred->allowCancelledAttempt =
                    g_activeAttempt.deferredAllowCancelledAttempt;
                deferred->bundledPostProcessApplied =
                    g_activeAttempt.deferredBundledPostProcessApplied;
                deferred->selection = g_activeAttempt.selection;
                g_activeAttempt.hasDeferredFinal = false;
                g_activeAttempt.deferredAllowCancelledAttempt = false;
                g_activeAttempt.deferredBundledPostProcessApplied = false;
            }
            completed = true;
        }
    }
    if (completed) {
        const audio_diagnostics::CaptureSnapshot capture =
            audio_diagnostics::FreezeCapture(recordingMs, pcmBytes);
        audio_diagnostics::AttachCapture(attemptId, capturedPcm, capture);
        const std::string deviceId = WideToUtf8(capture.device.deviceId);
        const std::string deviceHash = deviceId.empty()
            ? std::string()
            : audio_diagnostics::Sha256Hex(
                reinterpret_cast<const BYTE*>(deviceId.data()), deviceId.size());
        asr_runtime_log::Write(
            "event=capture_summary attempt=%llu source=%s device_hash=%s default_device=%d native_rate=%lu native_channels=%u native_bits=%u native_float=%d recording_ms=%.0f native_frames=%llu output_samples=%llu pcm_bytes=%zu deferred_primary=%d rms_dbfs=%.2f peak_dbfs=%.2f zero_ratio=%.6f near_silent_ratio=%.6f clipping_ratio=%.6f silent_packets=%llu silent_frames=%llu discontinuities=%llu max_callback_gap_ms=%.1f first_non_silent_delay_ms=%.1f downmix_rms_dbfs=%.2f",
            static_cast<unsigned long long>(attemptId),
            WideToUtf8(capture.device.backend).c_str(),
            deviceHash.empty() ? "unavailable" : deviceHash.c_str(),
            capture.device.usedDefaultDevice ? 1 : 0,
            static_cast<unsigned long>(capture.device.nativeSampleRate),
            static_cast<unsigned>(capture.device.nativeChannels),
            static_cast<unsigned>(capture.device.nativeBitsPerSample),
            capture.device.nativeIsFloat ? 1 : 0,
            recordingMs,
            static_cast<unsigned long long>(capture.nativeFrames),
            static_cast<unsigned long long>(capture.outputSamples),
            pcmBytes,
            deferred ? 1 : 0,
            capture.output.rmsDbfs,
            capture.output.peakDbfs,
            capture.output.zeroRatio,
            capture.output.nearSilentRatio,
            capture.output.clippingRatio,
            static_cast<unsigned long long>(capture.silentPackets),
            static_cast<unsigned long long>(capture.silentFrames),
            static_cast<unsigned long long>(capture.discontinuities),
            capture.maxCallbackGapMs,
            capture.firstNonSilentDelayMs,
            capture.downmixRmsDbfs);
    }
    return deferred;
}

static void MarkActiveAttemptFinalHandled(uint64_t attemptId) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (g_activeAttempt.id == attemptId) {
        g_activeAttempt.finalHandled = true;
        g_activeAttempt.cancelled = true;
        g_activeAttempt.pcm.reset();
        g_activeAttempt.hasDeferredFinal = false;
        g_activeAttempt.deferredFinalText.clear();
        g_activeAttempt.deferredFinalConfig = {};
        g_activeAttempt.deferredAllowCancelledAttempt = false;
        g_activeAttempt.deferredBundledPostProcessApplied = false;
    }
}

enum class AttemptFinalDisposition {
    Ready,
    DeferredUntilStop,
    Rejected,
};

static AttemptFinalDisposition PrepareAttemptFinal(
    const AsrAttemptFinalMessage& message,
    Config& primaryConfig,
    Config& resultConfig,
    std::shared_ptr<const std::vector<BYTE>>& pcm,
    bool& fallbackAlreadyStarted,
    ULONGLONG& elapsedStartTick,
    SelectionContext& selection) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (message.attemptId == 0 || g_activeAttempt.id != message.attemptId ||
        g_activeAttempt.finalHandled || g_activeAttempt.invalidated ||
        (g_activeAttempt.cancelled && !message.allowCancelledAttempt)) {
        return AttemptFinalDisposition::Rejected;
    }
    if (!g_activeAttempt.recordingStopped) {
        if (g_activeAttempt.hasDeferredFinal) {
            return AttemptFinalDisposition::Rejected;
        }
        g_activeAttempt.hasDeferredFinal = true;
        g_activeAttempt.deferredFinalText = message.text;
        g_activeAttempt.deferredFinalConfig = message.primaryConfig;
        g_activeAttempt.deferredFinalSource = message.source;
        g_activeAttempt.deferredAllowCancelledAttempt =
            message.allowCancelledAttempt;
        g_activeAttempt.deferredBundledPostProcessApplied =
            message.bundledPostProcessApplied;
        return AttemptFinalDisposition::DeferredUntilStop;
    }
    g_activeAttempt.finalHandled = true;
    primaryConfig = g_activeAttempt.primaryConfig;
    resultConfig = message.primaryConfig;
    pcm = g_activeAttempt.pcm;
    g_activeAttempt.pcm.reset();
    fallbackAlreadyStarted = g_activeAttempt.fallbackStarted;
    selection = g_activeAttempt.selection;
    elapsedStartTick = g_activeAttempt.stoppedTick != 0
        ? g_activeAttempt.stoppedTick
        : g_activeAttempt.startedTick;
    return AttemptFinalDisposition::Ready;
}

static bool TryMarkAttemptFallbackStarted(uint64_t attemptId,
                                          bool allowCancelled = false) {
    std::lock_guard<std::mutex> lock(g_asrAttemptMutex);
    if (g_activeAttempt.id == attemptId && g_activeAttempt.finalHandled &&
        !g_activeAttempt.invalidated &&
        (allowCancelled || !g_activeAttempt.cancelled) &&
        !g_activeAttempt.fallbackStarted) {
        g_activeAttempt.fallbackStarted = true;
        return true;
    }
    return false;
}

static void StreamingFinalCallback(std::wstring text,
                                   const Config& config,
                                   bool bundledPostProcessApplied,
                                   void* userData) {
    auto* msg = new AsrAttemptFinalMessage;
    msg->attemptId = static_cast<uint64_t>(reinterpret_cast<UINT_PTR>(userData));
    msg->primaryConfig = config;
    msg->text = std::move(text);
    msg->bundledPostProcessApplied = bundledPostProcessApplied;
    if (!PostMessageW(g_mainWindow, kAsrAttemptFinalMessage, 0, reinterpret_cast<LPARAM>(msg))) {
        delete msg;
    }
}

static void DispatchStreamingFallbackAsync(uint64_t attemptId,
                                           Config primaryConfig,
                                           std::shared_ptr<const std::vector<BYTE>> pcm,
                                           std::wstring primaryError,
                                           bool allowCancelledAttempt = false) {
    if (!IsActiveAsrAttempt(attemptId, allowCancelledAttempt)) {
        asr_runtime_log::Write("event=fallback_discarded_stale attempt=%llu stage=before_start",
                               static_cast<unsigned long long>(attemptId));
        return;
    }
    Config fallbackConfig = BuildFallbackConfig(primaryConfig);
    if (!TryMarkAttemptFallbackStarted(attemptId, allowCancelledAttempt)) {
        asr_runtime_log::Write("event=fallback_discarded_stale attempt=%llu stage=claim",
                               static_cast<unsigned long long>(attemptId));
        return;
    }
    const AsrResultClassification primaryClassification = ClassifyAsrResult(primaryError);
    asr_runtime_log::Write(
        "event=fallback_start attempt=%llu primary=%s fallback=%s reason=%s pcm_bytes=%zu",
        static_cast<unsigned long long>(attemptId),
        AsrBackendLogName(primaryConfig.asrBackend),
        AsrBackendLogName(fallbackConfig.asrBackend),
        AsrFailureReasonDebugName(primaryClassification.reason),
        pcm ? pcm->size() : 0);
    PostFallbackHud(fallbackConfig);
    std::thread([attemptId, primaryConfig, fallbackConfig, pcm,
                 primaryError = std::move(primaryError),
                 allowCancelledAttempt]() {
        if (!IsActiveAsrAttempt(attemptId, allowCancelledAttempt)) {
            asr_runtime_log::Write("event=fallback_discarded_stale attempt=%llu stage=worker_start",
                                   static_cast<unsigned long long>(attemptId));
            return;
        }
        const ULONGLONG fallbackStartTick = GetTickCount64();
        AsrSessionResult selectedResult = RunConfiguredAsrOnce(fallbackConfig, *pcm, {});
        AsrFinalMetadata metadata;
        metadata.attemptId = attemptId;
        metadata.allowCancelledAttempt = allowCancelledAttempt;
        metadata.bundledPostProcessApplied =
            selectedResult.bundledPostProcessApplied;
        metadata.usedFallback = true;
        metadata.primaryBackend = primaryConfig.asrBackend;
        metadata.primaryError = NormalizeAsrText(primaryError);
        metadata.fallbackBackend = fallbackConfig.asrBackend;

        const AsrResultClassification fallbackClassification = ClassifyAsrResult(selectedResult.text);
        const bool fallbackAccepted = IsActiveAsrAttempt(
            attemptId, allowCancelledAttempt);
        LogAsrResultEvent("fallback_final", attemptId, fallbackConfig.asrBackend,
                          fallbackClassification, "streaming_fallback",
                          fallbackStartTick, fallbackAccepted);
        if (!fallbackAccepted) {
            asr_runtime_log::Write("event=fallback_discarded_stale attempt=%llu stage=completed",
                                   static_cast<unsigned long long>(attemptId));
            return;
        }

        if (fallbackClassification.kind == AsrResultKind::OperationalError) {
            selectedResult.text = L"Fallback failed: " + AsrBackendDisplayName(fallbackConfig);
        }

        if (!IsActiveAsrAttempt(attemptId, allowCancelledAttempt)) {
            asr_runtime_log::Write("event=fallback_discarded_stale attempt=%llu stage=dispatch",
                                   static_cast<unsigned long long>(attemptId));
            return;
        }
        ApplyAsrSessionSideEffects(selectedResult);
        ApplyBatchResultMetrics(fallbackConfig, selectedResult);
        DispatchAsrFinalText(g_mainWindow, selectedResult.text, fallbackConfig,
                             RefineWithLlmAsync, &g_lastRawAsrText, metadata);
    }).detach();
}

static void HandleAsrAttemptFinal(AsrAttemptFinalMessage& msg) {
    Config primaryConfig = msg.primaryConfig;
    Config resultConfig = msg.primaryConfig;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    bool fallbackAlreadyStarted = false;
    ULONGLONG elapsedStartTick = 0;
    SelectionContext selection;
    const bool allowCancelledAttempt = msg.allowCancelledAttempt;
    const AttemptFinalDisposition disposition = PrepareAttemptFinal(
        msg, primaryConfig, resultConfig, pcm,
        fallbackAlreadyStarted, elapsedStartTick, selection);
    if (disposition == AttemptFinalDisposition::DeferredUntilStop) {
        asr_runtime_log::Write(
            "event=primary_final_deferred attempt=%llu source=%s reason=recording_active",
            static_cast<unsigned long long>(msg.attemptId),
            AsrAttemptFinalSourceName(msg.source));
        return;
    }
    if (disposition == AttemptFinalDisposition::Rejected) {
        asr_runtime_log::Write(
            "event=primary_final_discarded attempt=%llu source=%s reason=stale_or_duplicate",
            static_cast<unsigned long long>(msg.attemptId),
            AsrAttemptFinalSourceName(msg.source));
        return;
    }

    auto finishedSession = TakeActiveStreamingSession();
    if (finishedSession) {
        // A streaming fallback may use the same provider/global connection
        // state as the primary session.  Join the old worker before the
        // fallback thread is allowed to open another streaming session.
        finishedSession->Abort();
        finishedSession.reset();
    }

    const std::wstring primaryText = NormalizeAsrText(msg.text);
    const double primaryElapsedMs =
        static_cast<double>(ElapsedSinceTick(elapsedStartTick));
    if (msg.source == AsrAttemptFinalSource::Watchdog) {
        // Abort wakes the provider worker and may make it report "aborted".
        // The externally observed terminal is still the watchdog timeout, so
        // replace that cancellation artifact with the exact final cause.
        asr_diagnostics::CompleteFromText(
            primaryConfig, primaryText, "watchdog_final", primaryElapsedMs);
    } else {
        asr_diagnostics::CompleteIfMissingFromText(
            primaryConfig, primaryText, "provider_final", primaryElapsedMs);
    }
    const AsrResultClassification primaryClassification = ClassifyAsrResult(primaryText);
    LogAsrResultEvent("primary_final", msg.attemptId, primaryConfig.asrBackend,
                      primaryClassification, AsrAttemptFinalSourceName(msg.source),
                      elapsedStartTick, true);
    const bool selectionRewriteRequested =
        primaryConfig.asrBackend == L"qwen_free" &&
        primaryConfig.qwenFreeRewriteEnabled &&
        selection.HasCapturedSelection();
    const bool automaticQwenUtdidFallback =
        !fallbackAlreadyStarted &&
        !selectionRewriteRequested &&
        primaryConfig.asrBackend == L"qwen_free" &&
        primaryClassification.kind == AsrResultKind::OperationalError &&
        primaryText.find(L"UTDID acquisition failed") != std::wstring::npos &&
        !primaryConfig.qwenApiKey.empty();

    // Qwen Free now performs UTDID loading on its worker so the keyboard hook
    // never blocks on provider setup. Preserve the previous automatic
    // DashScope fallback only for ordinary dictation. With a live selection,
    // the ASR text is a rewrite instruction; a plain fallback paste could
    // overwrite the selected content with that instruction.
    if (automaticQwenUtdidFallback && pcm && pcm->size() >= 8000) {
        Config automaticFallbackConfig = primaryConfig;
        automaticFallbackConfig.fallbackAsrBackend = L"qwen";
        DispatchStreamingFallbackAsync(
            msg.attemptId, automaticFallbackConfig, pcm, primaryText,
            allowCancelledAttempt);
        return;
    }
    if (automaticQwenUtdidFallback) {
        asr_runtime_log::Write(
            "event=fallback_not_started attempt=%llu fallback=qwen reason=%s pcm_bytes=%zu",
            static_cast<unsigned long long>(msg.attemptId),
            pcm ? "pcm_too_short" : "pcm_unavailable",
            pcm ? pcm->size() : 0);
    }

    const bool canFallback = !selectionRewriteRequested &&
        ShouldRunFallback(primaryConfig, primaryText, fallbackAlreadyStarted, false);
    if (selectionRewriteRequested && !automaticQwenUtdidFallback &&
        primaryClassification.kind == AsrResultKind::OperationalError) {
        asr_runtime_log::Write(
            "event=fallback_suppressed attempt=%llu reason=selection_rewrite_safety",
            static_cast<unsigned long long>(msg.attemptId));
    }
    if (canFallback && pcm && pcm->size() >= 8000) {
        DispatchStreamingFallbackAsync(msg.attemptId, primaryConfig, pcm,
                                        primaryText, allowCancelledAttempt);
        return;
    }
    if (canFallback) {
        asr_runtime_log::Write(
            "event=fallback_not_started attempt=%llu fallback=%s reason=%s pcm_bytes=%zu",
            static_cast<unsigned long long>(msg.attemptId),
            AsrBackendLogName(primaryConfig.fallbackAsrBackend),
            pcm ? "pcm_too_short" : "pcm_unavailable",
            pcm ? pcm->size() : 0);
    }

    AsrFinalMetadata metadata;
    metadata.attemptId = msg.attemptId;
    metadata.allowCancelledAttempt = allowCancelledAttempt;
    metadata.bundledPostProcessApplied = msg.bundledPostProcessApplied;
    metadata.selection = std::move(selection);
    DispatchAsrFinalText(g_mainWindow, primaryText, resultConfig,
                         RefineWithLlmAsync, &g_lastRawAsrText, metadata);
}

static void HandleStreamingSessionStartFailure(uint64_t attemptId,
                                               const Config& primaryConfig,
                                               std::wstring errorText) {
    const std::vector<BYTE> pcm = StopAudioCapture();
    g_recording = false;
    g_recordingMs = static_cast<double>(GetTickCount64() - g_sessionStartTick);
    g_lastPcmBytes = pcm.size();
    CompleteActiveAttemptRecording(attemptId, &pcm, g_recordingMs, pcm.size());

    AsrAttemptFinalMessage message;
    message.attemptId = attemptId;
    message.primaryConfig = primaryConfig;
    message.text = std::move(errorText);
    message.source = AsrAttemptFinalSource::SessionStart;
    HandleAsrAttemptFinal(message);
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
static StreamingPartialHudCallbackContext g_qwenFreePartialHudContext{L"Listening... Qwen IME (Free)"};

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

static bool StartStreamingVadTrimmerForCloud(const Config& config,
                                             const wchar_t* debugPrefix,
                                             bool markReady = true) {
    ResetStreamingVadTrimmerState();
    if (!config.enableVad) return false;

    auto trimmer = std::make_unique<StreamingVadTrimmer>();
    std::wstring error;
    const bool ok = trimmer->Start(config, g_asrEngine, &error);
    if (!ok) {
        VolcDebugLog("%ls: VAD trim disabled (error_wlen=%zu)", debugPrefix, error.size());
        return false;
    }

    g_streamingVadTrimmer = std::move(trimmer);
    g_streamingVadReady = markReady;
    VolcDebugLog("%ls: VAD trim active (%ls)", debugPrefix, config.vadModel.c_str());
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

// Qwen-free is started after capture so the keyboard hook never waits on
// UTDID/signing/WinHTTP setup.  Activate the session and replay the already
// captured prefix while holding both locks used by the capture callback.  A
// callback that arrives during this window therefore waits until the session
// is visible and cannot leave a gap between the copied prefix and live PCM.
static void ActivateQwenFreeStreamingSession(
    std::unique_ptr<IStreamingAsrSession> session) {
    if (!session) return;

    EnterCriticalSection(&g_audioLock);
    EnterCriticalSection(&g_streamingSessionCs);

    if (!g_audioData.empty() &&
        !session->EnqueuePcmChunk(g_audioData.data(), g_audioData.size())) {
        asr_runtime_log::Write(
            "event=qwen_free_initial_audio_enqueue_failed pcm_bytes=%zu",
            g_audioData.size());
    }
    g_activeStreamingSession = std::move(session);
    // Qwen-free intentionally sends the complete PCM stream and does not use
    // the shared local streaming VAD trimmer.
    g_streamingVadReady = false;

    LeaveCriticalSection(&g_streamingSessionCs);
    LeaveCriticalSection(&g_audioLock);
}

static std::wstring AudioCaptureFailureHudText(bool wasapi, DWORD code) {
    if (wasapi && (code == static_cast<DWORD>(AUDCLNT_E_DEVICE_INVALIDATED) ||
                   code == static_cast<DWORD>(AUDCLNT_E_SERVICE_NOT_RUNNING))) {
        return L"Microphone disconnected";
    }
    return L"Microphone capture failed";
}

static void FinishAudioCaptureFailure(uint64_t generation,
                                      DWORD code,
                                      bool wasapi,
                                      uint64_t attemptId,
                                      double recordingMs,
                                      const std::vector<BYTE>& pcm) {
    g_audioCaptureFailurePending.store(false, std::memory_order_release);
    g_lastPcmBytes = pcm.size();
    CompleteActiveAttemptRecording(attemptId, &pcm, recordingMs, pcm.size());
    const Config attemptConfig = ActiveAsrAttemptConfig();
    audio_diagnostics::StageMetadata stage =
        asr_diagnostics::MakeStageMetadata(attemptConfig, L"capture_failure");
    audio_diagnostics::StageTerminal terminal;
    terminal.terminal = "capture_failure";
    terminal.reason = wasapi ? "wasapi_runtime_failure" : "wavein_runtime_failure";
    terminal.providerCode = std::to_string(static_cast<unsigned long>(code));
    audio_diagnostics::CompleteStageIfMissing(attemptId, stage, terminal);
    audio_diagnostics::FinalResult finalResult;
    finalResult.kind = audio_diagnostics::FinalKind::CaptureFailure;
    finalResult.reason = terminal.reason;
    finalResult.terminal = terminal.terminal;
    audio_diagnostics::FinalizeAttempt(attemptId, finalResult);
    CancelActiveAsrAttempt(attemptId, true);
    MarkActiveAttemptFinalHandled(attemptId);
    KillTimer(g_mainWindow, kStreamingWatchdogTimer);
    AbortAndResetActiveStreamingSession();
    ResetStreamingVadTrimmerState();
    ResetStreamingPartialHudState();

    const std::wstring hudText = AudioCaptureFailureHudText(wasapi, code);
    asr_runtime_log::Write(
        "event=capture_runtime_failed generation=%llu attempt=%llu source=%s code=0x%08lx pcm_bytes=%zu",
        static_cast<unsigned long long>(generation),
        static_cast<unsigned long long>(attemptId),
        wasapi ? "wasapi" : "wavein",
        static_cast<unsigned long>(code),
        pcm.size());
    ShowHud(hudText);
    if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 2200, nullptr);
}

static void HandleAudioCaptureFailure(uint64_t generation,
                                      DWORD code,
                                      bool wasapi) {
    const uint64_t currentGeneration =
        g_audioCaptureGeneration.load(std::memory_order_acquire);
    if (generation == 0 || generation != currentGeneration || !g_recording) {
        asr_runtime_log::Write(
            "event=capture_runtime_failed_stale generation=%llu current=%llu source=%s code=0x%08lx recording=%d",
            static_cast<unsigned long long>(generation),
            static_cast<unsigned long long>(currentGeneration),
            wasapi ? "wasapi" : "wavein",
            static_cast<unsigned long>(code),
            g_recording ? 1 : 0);
        return;
    }

    const uint64_t attemptId = ActiveAsrAttemptId();
    const double recordingMs = static_cast<double>(GetTickCount64() - g_sessionStartTick);
    g_recording = false;

    // Capture failure is a local input fault, not a partial ASR result.  Stop
    // and discard the active provider session instead of sending incomplete
    // PCM into fallback or pasting a truncated transcript.
    const std::vector<BYTE> pcm = StopAudioCapture();
    FinishAudioCaptureFailure(generation, code, wasapi, attemptId, recordingMs, pcm);
}

void StartRecordingSession() {
    if (g_recording) return;
    if (g_hudWindow) KillTimer(g_hudWindow, kHudHideTimer);

    g_hudIsRefining = false;
    g_hudHasSpoken = false;
    g_vadDetectedVoice.store(false);
    std::wstring name = AsrBackendDisplayName(g_config);
    ShowHud(L"Listening... " + name);

    std::wstring error;
    AudioCaptureStartFailure captureFailure;
    if (!StartAudioCapture(error, &captureFailure)) {
        const uint64_t attemptId = RecordCaptureStartFailure(
            g_config, captureFailure);
        asr_runtime_log::Write(
            "event=capture_start_failed attempt=%llu primary=%s capture_backends=%s terminal_backend=%s phase=%s code=%lu error_chars=%zu",
            static_cast<unsigned long long>(attemptId),
            AsrBackendLogName(g_config.asrBackend),
            WideToUtf8(captureFailure.attemptedBackends).c_str(),
            WideToUtf8(captureFailure.terminalBackend).c_str(),
            WideToUtf8(captureFailure.phase).c_str(),
            static_cast<unsigned long>(captureFailure.code),
            error.size());
        ShowHud(error);
        if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
        return;
    }
    g_sessionStartTick = GetTickCount64();
    g_recording = true;
    // Preserve the capture-first startup order introduced for streaming head
    // audio reliability. The initial HUD call happened before g_recording was
    // set, so arm only its shared animation timer once recording is active.
    StartHudRecordingAnimation();
    s_wasapiUsed = g_wasapiCapture.IsInitialized();
    if (s_wasapiUsed) {
        s_wasapiDeviceName = g_wasapiCapture.GetDeviceName();
        s_wasapiNativeRate = g_wasapiCapture.GetNativeSampleRate();
    }

    // Start capture before querying UI Automation/WM_COPY. Some target
    // controls can take hundreds of milliseconds to answer WM_COPY; keeping
    // WASAPI active prevents the beginning of the utterance from being lost.
    SelectionContext selection;
    if (g_config.asrBackend == L"qwen_free" && g_config.qwenFreeRewriteEnabled) {
        selection = selection_context::Capture();
        if (selection.Usable()) {
            asr_runtime_log::Write(
                "event=selection_capture ok=1 chars=%zu uia=%d clipboard=%d",
                selection.selectedText.size(),
                selection.capturedWithUiAutomation ? 1 : 0,
                selection.capturedWithClipboard ? 1 : 0);
        } else {
            asr_runtime_log::Write(
                "event=selection_capture ok=0 reason=%s",
                selection.error.empty() ? "unknown" : selection.error.c_str());
        }
    }

    const uint64_t attemptId = BeginAsrAttempt(g_config, selection);
    AbortAndResetActiveStreamingSession();
    ResetStreamingVadTrimmerState();
    ResetStreamingPartialHudState();
    const Config attemptConfig = ActiveAsrAttemptConfig();

    if (attemptConfig.asrBackend == L"qwen" && IsStreamingCloudBackend(attemptConfig)) {
        auto session = (attemptConfig.qwenModel == L"qwen-audio-3.0-asr-flash-streaming")
            ? CreateQwenAudioStreamingSession(attemptConfig, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText)
            : CreateQwenStreamingSession(attemptConfig, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_qwenPartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            HandleStreamingSessionStartFailure(
                attemptId,
                attemptConfig,
                startError.empty() ? L"Qwen ASR error: session start failed" : std::move(startError));
            return;
        }

        StartStreamingVadTrimmerForCloud(attemptConfig, L"Qwen thread", false);
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

    if (attemptConfig.asrBackend == L"doubao_ime") {
        auto session = CreateDoubaoImeStreamingSession(attemptConfig, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_doubaoImePartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            HandleStreamingSessionStartFailure(
                attemptId,
                attemptConfig,
                startError.empty() ? L"Doubao IME ASR error: session start failed" : std::move(startError));
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

    if (attemptConfig.asrBackend == L"qwen_free") {
        // 千问 IME 免费后端（A1 纯协议还原）：
        //   - VoxType 负责 WASAPI 采集 PCM 送 EnqueuePcmChunk；
        //   - 协议层 qwen_free_proto_* 负责 UTDID/签名/ASR WebSocket/LLM 后处理；
        //   - Start 仅建立异步 session；UTDID/签名/连接都在 worker 完成，
        //     失败后保留完整录音并进入统一 fallback。
        auto session = CreateQwenFreeStreamingSession(
            attemptConfig, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText, selection);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_qwenFreePartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            HandleStreamingSessionStartFailure(
                attemptId,
                attemptConfig,
                startError.empty()
                    ? L"Qwen IME ASR error: session start failed"
                    : std::move(startError));
            return;
        }

        // Qwen Free has its own server-side VAD.  A local streaming trimmer
        // can suppress the only PCM copy before the service sees it, which
        // turns a valid recording into the generic "No speech detected" HUD.
        // Keep the full 16 kHz PCM stream for this protocol backend.
        ResetStreamingVadTrimmerState();
        ActivateQwenFreeStreamingSession(std::move(session));

        ShowHud(L"Listening... Qwen IME (Free)");
        DWORD watchdogMs = 18000;
        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession) {
            watchdogMs = g_activeStreamingSession->CurrentWatchdogMs();
        }
        LeaveCriticalSection(&g_streamingSessionCs);
        SetTimer(g_mainWindow, kStreamingWatchdogTimer, watchdogMs, nullptr);
        return;
    }

    if (attemptConfig.asrBackend == L"volcengine") {
        VolcengineResetForNewSession();

        auto session = CreateVolcengineStreamingSession(attemptConfig, g_mainWindow, RefineWithLlmAsync, &g_lastRawAsrText);
        session->SetPartialCallback(StreamingPartialHudCallback, &g_volcenginePartialHudContext);
        session->SetFinalCallback(StreamingFinalCallback, reinterpret_cast<void*>(static_cast<UINT_PTR>(attemptId)));

        std::wstring startError;
        if (!session->Start(startError)) {
            HandleStreamingSessionStartFailure(
                attemptId,
                attemptConfig,
                startError.empty() ? L"VolcEngine error: session start failed" : std::move(startError));
            return;
        }

        StartStreamingVadTrimmerForCloud(attemptConfig, L"Volc thread", false);
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
    if (attemptConfig.asrBackend != L"baidu" && !IsStreamingCloudBackend(attemptConfig) &&
        attemptConfig.asrBackend != L"mimo" &&
        attemptConfig.enableVad) {
        const int threads = ResolveThreads(attemptConfig.threads);
        g_asrEngine.Lock();
        bool ok = g_asrEngine.EnsureVadForConfig(attemptConfig, threads);
        if (ok) {
            if (attemptConfig.vadModel == L"firered") {
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
    // A device/driver failure can post its message concurrently with the
    // physical key-up.  Consume the pending capture fault first so a queued
    // stop cannot turn incomplete PCM into a normal ASR request.
    if (g_audioCaptureFailurePending.load(std::memory_order_acquire)) {
        HandleAudioCaptureFailure(
            g_audioCaptureGeneration.load(std::memory_order_acquire),
            g_audioCaptureFailureCode.load(std::memory_order_acquire),
            g_audioCaptureFailureWasapi.load(std::memory_order_acquire));
        return;
    }
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
    // Settings may be edited while the microphone is active.  The attempt's
    // configuration is immutable; using the live global here can route a
    // streaming session through the batch stop path (or vice versa).
    const Config recordingConfig = ActiveAsrAttemptConfig();
    const bool hasStreamingSession = HasActiveStreamingSession();
    const uint64_t captureGeneration =
        g_audioCaptureGeneration.load(std::memory_order_acquire);
    const std::vector<BYTE> pcm = StopAudioCapture();
    if (g_audioCaptureFailurePending.load(std::memory_order_acquire)) {
        FinishAudioCaptureFailure(
            captureGeneration,
            g_audioCaptureFailureCode.load(std::memory_order_acquire),
            g_audioCaptureFailureWasapi.load(std::memory_order_acquire),
            attemptId,
            g_recordingMs,
            pcm);
        return;
    }

    if (IsStreamingCloudBackend(recordingConfig) && hasStreamingSession) {
        std::unique_ptr<AsrAttemptFinalMessage> deferredFinal =
            CompleteActiveAttemptRecording(attemptId, &pcm,
                                            g_recordingMs, pcm.size());
        g_lastPcmBytes = pcm.size();
        if (pcm.size() < 8000) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            asr_diagnostics::CompleteIfMissingFromText(
                recordingConfig, L"Too short", "too_short");
            audio_diagnostics::FinalizeAttempt(
                attemptId, asr_diagnostics::FinalFromText(L"Too short"));
            MarkActiveAttemptFinalHandled(attemptId);
            AbortAndResetActiveStreamingSession();
            asr_runtime_log::Write("event=attempt_skipped attempt=%llu kind=too_short pcm_bytes=%zu",
                                   static_cast<unsigned long long>(attemptId), pcm.size());
            ShowHud(L"Too short");
            SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
            return;
        }
        FinishStreamingVadTrimmer();
        if (g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive()) {
            const StreamingVadTrimStats stats = g_streamingVadTrimmer->Stats();
            audio_diagnostics::StageMetadata stage =
                asr_diagnostics::MakeStageMetadata(recordingConfig);
            stage.vadActive = stats.active;
            stage.vadDetectedSpeech = stats.detectedSpeech;
            stage.vadModel = stats.modelName;
            stage.vadInputBytes = stats.rawBytes;
            stage.vadOutputBytes = stats.outputBytes;
            audio_diagnostics::UpdateStageMetadata(attemptId, stage);
        }
        if (StreamingVadTrimSawNoSpeech()) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            audio_diagnostics::StageMetadata stage =
                asr_diagnostics::MakeStageMetadata(recordingConfig, L"local_vad_no_speech");
            stage.vadActive = true;
            stage.vadDetectedSpeech = false;
            audio_diagnostics::StageTerminal terminal;
            terminal.terminal = "local_vad_no_speech";
            terminal.reason = "no_speech";
            audio_diagnostics::CompleteStageIfMissing(attemptId, stage, terminal);
            audio_diagnostics::FinalizeAttempt(
                attemptId, asr_diagnostics::FinalFromText(L"No speech detected"));
            MarkActiveAttemptFinalHandled(attemptId);
            AbortAndResetActiveStreamingSession();
            asr_runtime_log::Write("event=attempt_skipped attempt=%llu kind=no_speech pcm_bytes=%zu",
                                   static_cast<unsigned long long>(attemptId), pcm.size());
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
        if (deferredFinal) {
            KillTimer(g_mainWindow, kStreamingWatchdogTimer);
            HandleAsrAttemptFinal(*deferredFinal);
            return;
        }
        DWORD finalizeTimeout = IsFallbackAsrEnabled(recordingConfig)
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
        if (recordingConfig.asrBackend == L"volcengine") {
            VolcDebugLog("Volc watchdog: finalize timeout reset to %ums (recording=%.0fms, pcm=%zu)",
                         finalizeTimeout, g_recordingMs, pcm.size());
        }
        ShowHud(L"Recognizing... " + AsrBackendDisplayName(recordingConfig));
        return;
    }

    CompleteActiveAttemptRecording(attemptId, &pcm, g_recordingMs, pcm.size());
    if (pcm.size() < 8000) {
        asr_diagnostics::CompleteIfMissingFromText(
            recordingConfig, L"Too short", "too_short");
        audio_diagnostics::FinalizeAttempt(
            attemptId, asr_diagnostics::FinalFromText(L"Too short"));
        MarkActiveAttemptFinalHandled(attemptId);
        asr_runtime_log::Write("event=attempt_skipped attempt=%llu kind=too_short pcm_bytes=%zu",
                               static_cast<unsigned long long>(attemptId), pcm.size());
        ShowHud(L"Too short");
        SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
        return;
    }

    if (g_streamingVadReady) {
        HiResTimer tVad;
        g_asrEngine.Lock();
        if (recordingConfig.vadModel == L"firered") {
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
        if (recordingConfig.enableDebugMode) {
            g_vadMs = ms;
            g_vadModelName = (recordingConfig.vadModel == L"firered")
                ? L"FireRed" : L"Silero";
        }
        g_streamingVadReady = false;

        if (g_streamingVadSamples.empty()) {
            audio_diagnostics::StageMetadata stage =
                asr_diagnostics::MakeStageMetadata(recordingConfig, L"local_vad_no_speech");
            stage.vadActive = true;
            stage.vadDetectedSpeech = false;
            audio_diagnostics::StageTerminal terminal;
            terminal.terminal = "local_vad_no_speech";
            terminal.reason = "no_speech";
            audio_diagnostics::CompleteStageIfMissing(attemptId, stage, terminal);
            audio_diagnostics::FinalizeAttempt(
                attemptId, asr_diagnostics::FinalFromText(L"No speech detected"));
            MarkActiveAttemptFinalHandled(attemptId);
            asr_runtime_log::Write("event=attempt_skipped attempt=%llu kind=no_speech pcm_bytes=%zu",
                                   static_cast<unsigned long long>(attemptId), pcm.size());
            ShowHud(L"No speech detected");
            SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
            return;
        }
    }

    std::wstring name = AsrBackendDisplayName(recordingConfig);
    ShowHud(L"Recognizing... " + name);
    RecognizeAsync(pcm, attemptId, recordingConfig);
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
    case kHotkeyRecordingMessage:
        if (wParam == kHotkeyRecordingStart) {
            StartRecordingSession();
        } else if (wParam == kHotkeyCapsLockRecordingStop) {
            StopRecordingSession();
            RestoreCapsLockState();
        } else if (wParam == kHotkeyRecordingStop) {
            StopRecordingSession();
        }
        return 0;
    case kAudioCaptureErrorMessage:
        HandleAudioCaptureFailure(static_cast<uint64_t>(wParam),
                                  static_cast<DWORD>(lParam),
                                  true);
        return 0;
    case kWaveInCaptureErrorMessage:
        HandleAudioCaptureFailure(static_cast<uint64_t>(wParam),
                                  static_cast<DWORD>(lParam),
                                  false);
        return 0;
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
        if (result && !ShouldAcceptFinalMessage(
                result->attemptId, result->allowCancelledAttempt)) {
            return 0;
        }
        KillTimer(g_mainWindow, kStreamingWatchdogTimer);
        const Config resultConfig = result ? result->resultConfig : g_config;
        const std::wstring text = NormalizeAsrText(result ? result->text : L"ASR failed");
        const bool isRewriteFailure = text.rfind(L"Qwen IME rewrite failed:", 0) == 0;
        const bool isError = IsOperationalAsrError(text) || isRewriteFailure;
        const bool hasSelectionRewrite = result &&
            resultConfig.asrBackend == L"qwen_free" &&
            resultConfig.qwenFreeRewriteEnabled &&
            result->selection.HasCapturedSelection() && !isRewriteFailure;
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
                VolcDebugLog("DeliverFinalText: starting (text=%u chars, selection_rewrite=%d)",
                             (unsigned)text.size(), hasSelectionRewrite ? 1 : 0);
                HiResTimer tPaste;
                bool delivered = true;
                if (hasSelectionRewrite) {
                    std::wstring replaceError;
                    delivered = ReplaceSelectionTextImeAware(
                        result->selection, text, &replaceError);
                    if (!delivered) {
                        VolcDebugLog("ReplaceSelectionTextImeAware: skipped (%ls)",
                                     replaceError.c_str());
                        ShowHud(L"Rewrite skipped: " + replaceError);
                        if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 2200, nullptr);
                    }
                } else {
                    PasteTextImeAware(text);
                }
                double pasteMs = tPaste.ElapsedMs();
                VolcDebugLog("DeliverFinalText: done (%.0fms, delivered=%d)",
                             pasteMs, delivered ? 1 : 0);

                if (resultConfig.enableDebugMode && delivered) {
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
                const AsrResultClassification classification = ClassifyAsrResult(text);
                VolcDebugLog("DeliverFinalText: skipped operational error (reason=%s)",
                             AsrFailureReasonDebugName(classification.reason));
            }
        }
        return 0;
    }
    case kLlmResultMessage: {
        std::unique_ptr<LlmFinalMessage> result(reinterpret_cast<LlmFinalMessage*>(lParam));
        if (result && !ShouldAcceptFinalMessage(
                result->attemptId, result->allowCancelledAttempt)) {
            return 0;
        }
        const Config resultConfig = result ? result->resultConfig : g_config;
        const std::wstring text = result ? result->text : L"LLM failed";
        g_hudIsRefining = false;
        ShowHud(text);
        if (!text.empty() && text.rfind(L"LLM failed:", 0) != 0) {
            HiResTimer tPaste;
            const bool hasSelectionRewrite = result &&
                resultConfig.asrBackend == L"qwen_free" &&
                resultConfig.qwenFreeRewriteEnabled &&
                result->selection.HasCapturedSelection();
            bool delivered = true;
            if (hasSelectionRewrite) {
                std::wstring replaceError;
                delivered = ReplaceSelectionTextImeAware(
                    result->selection, text, &replaceError);
                if (!delivered) {
                    VolcDebugLog("ReplaceSelectionTextImeAware (LLM): skipped (%ls)",
                                 replaceError.c_str());
                    ShowHud(L"Rewrite skipped: " + replaceError);
                    if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 2200, nullptr);
                }
            } else {
                PasteTextImeAware(text);
            }
            double pasteMs = tPaste.ElapsedMs();

            if (resultConfig.enableDebugMode && delivered) {
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
                DWORD recordingLimitMs = 0;
                EnterCriticalSection(&g_streamingSessionCs);
                if (g_activeStreamingSession) {
                    watchdogMs = g_activeStreamingSession->CurrentWatchdogMs();
                    recordingLimitMs = g_activeStreamingSession->MaxRecordingMs();
                }
                LeaveCriticalSection(&g_streamingSessionCs);
                if (recordingLimitMs > 0) {
                    const ULONGLONG elapsedMs = GetTickCount64() - g_sessionStartTick;
                    if (elapsedMs >= recordingLimitMs) {
                        const uint64_t attemptId = ActiveAsrAttemptId();
                        const Config attemptConfig = ActiveAsrAttemptConfig();
                        asr_runtime_log::Write(
                            "event=recording_limit_reached attempt=%llu backend=%s model=%s elapsed_ms=%llu limit_ms=%lu",
                            static_cast<unsigned long long>(attemptId),
                            AsrBackendLogName(attemptConfig.asrBackend),
                            WideToUtf8(attemptConfig.qwenModel).c_str(),
                            static_cast<unsigned long long>(elapsedMs),
                            static_cast<unsigned long>(recordingLimitMs));
                        StopRecordingSession();
                        return 0;
                    }
                    const DWORD remainingMs = static_cast<DWORD>(recordingLimitMs - elapsedMs);
                    watchdogMs = (std::min)(watchdogMs, (std::max<DWORD>)(remainingMs, 1));
                }
                SetTimer(hwnd, kStreamingWatchdogTimer, watchdogMs, nullptr);
                return 0;
            }
            auto session = TakeActiveStreamingSession();
            if (session && session->IsRunning()) {
                const uint64_t attemptId = ActiveAsrAttemptId();
                const Config primaryConfig = ActiveAsrAttemptConfig();
                std::wstring providerName = session->ProviderName();
                // Invalidate late provider callbacks before Abort wakes the
                // worker.  The watchdog message itself is explicitly marked
                // as allowed so timeout/fallback handling still runs.
                CancelActiveAsrAttempt(attemptId, false);
                session->Abort();
                std::wstring timeoutText = std::wstring(providerName) + L" error: timeout";
                if (providerName == L"Volcano Engine") {
                    timeoutText = L"ASR failed: VolcEngine timeout";
                }
                auto* msg = new AsrAttemptFinalMessage;
                msg->attemptId = attemptId;
                msg->primaryConfig = primaryConfig;
                msg->text = std::move(timeoutText);
                msg->source = AsrAttemptFinalSource::Watchdog;
                msg->allowCancelledAttempt = true;
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
        {
            const uint64_t attemptId = ActiveAsrAttemptId();
            CancelActiveAsrAttempt(attemptId, true);
            MarkActiveAttemptFinalHandled(attemptId);
            audio_diagnostics::DiscardAttempt(attemptId, "application_exit");
        }
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
    audio_diagnostics::SetLogCallback(WriteDiagnosticAudioRuntimeLog);

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
    audio_diagnostics::WaitForPendingWrites(INFINITE);

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    DeleteUiResources();
    DeleteCriticalSection(&g_audioLock);
    DeleteCriticalSection(&g_streamingSessionCs);
    return static_cast<int>(msg.wParam);
}
