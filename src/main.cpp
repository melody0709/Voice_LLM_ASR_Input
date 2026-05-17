#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "globals.h"
#include "engine.h"
#include "hud.h"
#include "hotkey.h"
#include "settings.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <deque>
#include <cstdio>
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
std::vector<HWND> g_vadFireredControls;
std::vector<HWND> g_vadSileroControls;
bool g_hudIsRefining = false;
bool g_llmKeyVisible = false;
bool g_baiduKeyVisible = false;
bool g_baiduApiKeyVisible = false;
bool g_volcKeyVisible = false;
volc_asr::VolcSession g_volcSession;
std::atomic<bool> g_volcStreaming{false};
std::thread g_volcThread;
CRITICAL_SECTION g_volcAudioCs;
std::vector<BYTE> g_volcPendingAudio;
std::atomic<int> g_volcVadState{0};
int g_volcVadSilentCount = 0;
std::deque<std::vector<BYTE>> g_volcVadPreBuffer;
std::vector<std::vector<BYTE>> g_volcVadTailBuffer;
std::atomic<bool> g_volcVadDoTrim{false};
size_t g_volcSentBytes = 0;
namespace volc_asr { std::atomic<bool> g_volcKeepAlive{false}; }
AsrEngine g_asrEngine;
int g_cloudProviderIdx = 0;
HWND g_cloudAsrHintControl = nullptr;

static std::deque<std::wstring> g_volcRecognitionHistory;

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

static std::wstring BuildVolcContextJson() {
    if (g_volcRecognitionHistory.empty()) return L"";
    std::wstring json = L"{\"context_type\":\"dialog_ctx\",\"context_data\":[";
    for (size_t i = 0; i < g_volcRecognitionHistory.size(); ++i) {
        if (i > 0) json += L",";
        std::wstring text = g_volcRecognitionHistory[i];
        std::wstring escaped;
        for (wchar_t c : text) {
            if (c == L'\\') escaped += L"\\\\";
            else if (c == L'"') escaped += L"\\\"";
            else if (c == L'\n') escaped += L"\\n";
            else if (c == L'\r') escaped += L"\\r";
            else if (c == L'\t') escaped += L"\\t";
            else escaped += c;
        }
        json += L"{\"text\":\"" + escaped + L"\"}";
    }
    json += L"]}";
    return json;
}

static std::vector<float> PcmToFloat(const std::vector<BYTE>& pcm) {
    const auto* samples = reinterpret_cast<const int16_t*>(pcm.data());
    const size_t count = pcm.size() / sizeof(int16_t);
    std::vector<float> floats(count);
    for (size_t i = 0; i < count; ++i)
        floats[i] = static_cast<float>(samples[i]) / 32768.0f;
    return floats;
}

static void AddVolcRecognitionHistory(const std::wstring& text) {
    if (text.empty() || text == L"(empty result)" || text == L"Too short" || text == L"No speech detected") return;
    if (text.rfind(L"VolcEngine error", 0) == 0) return;
    if (text.rfind(L"ASR failed:", 0) == 0) return;
    g_volcRecognitionHistory.push_back(text);
    int maxHistory = g_config.volcContextHistory;
    if (maxHistory < 1) maxHistory = 5;
    if (maxHistory > 20) maxHistory = 20;
    while (static_cast<int>(g_volcRecognitionHistory.size()) > maxHistory) {
        g_volcRecognitionHistory.pop_front();
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

    if (config.asrBackend == L"baidu") {
        std::thread([config, pcm]() {
            g_lastPcmBytes = pcm.size();

            HiResTimer tBaidu;
            baidu_asr::BaiduConfig bcfg;
            bcfg.apiKey = config.baiduApiKey;
            bcfg.secretKey = config.baiduSecretKey;
            bcfg.devPid = config.baiduDevPid;

            std::wstring text = baidu_asr::Recognize(pcm, bcfg);
            g_cloudApiMs = tBaidu.ElapsedMs();

            if (text.empty()) {
                text = L"(empty result)";
            }

            bool needLlm = (config.postprocess == L"llm")
                         && !config.llmEndpoint.empty()
                         && !config.llmApiKey.empty()
                         && !text.empty()
                         && text.rfind(L"Baidu ASR error:", 0) != 0;

            if (needLlm) {
                g_lastRawAsrText = text;
                PostMessageW(g_mainWindow, kAsrResultMessage, 1, reinterpret_cast<LPARAM>(new std::wstring(text)));
                RefineWithLlmAsync(text, config);
            } else {
                PostMessageW(g_mainWindow, kAsrResultMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
            }
        }).detach();
        return;
    }

    std::vector<float> localStreamingVadSamples = std::move(g_streamingVadSamples);
    g_streamingVadSamples.clear();
    std::thread([config, pcm, localStreamingVadSamples = std::move(localStreamingVadSamples)]() {
        g_lastPcmBytes = pcm.size();

        HiResTimer tPcm;
        std::vector<float> samples;
        Config workConfig = config;

        if (!localStreamingVadSamples.empty()) {
            if (config.enableDebugMode) g_vadTrimmedSamples = localStreamingVadSamples.size();
            samples = std::move(localStreamingVadSamples);
            workConfig.enableVad = false;
        } else {
            samples = PcmToFloat(pcm);
        }

        std::wstring text = g_asrEngine.Recognize(samples, 16000, workConfig);
        // Recognize 内部已设 g_vadMs / g_asrDecodeMs / g_punctMs / g_vadModelName
        if (text.empty()) {
            text = L"(empty result)";
        }

        bool needLlm = (config.postprocess == L"llm")
                     && !config.llmEndpoint.empty()
                     && !config.llmApiKey.empty()
                     && !text.empty()
                     && text.rfind(L"ASR failed:", 0) != 0;

        if (needLlm) {
            g_lastRawAsrText = text;
            PostMessageW(g_mainWindow, kAsrResultMessage, 1, reinterpret_cast<LPARAM>(new std::wstring(text)));
            RefineWithLlmAsync(text, config);
        } else {
            PostMessageW(g_mainWindow, kAsrResultMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
        }
    }).detach();
}

void StartRecordingSession() {
    if (g_recording) return;
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

    if (g_config.asrBackend == L"volcengine") {
        EnterCriticalSection(&g_volcAudioCs);
        g_volcPendingAudio.clear();
        LeaveCriticalSection(&g_volcAudioCs);

        const Config config = g_config;
        volc_asr::VolcConfig vcfg;
        vcfg.apiKey = config.volcApiKey;
        vcfg.resourceId = config.volcResourceId;
        vcfg.mode = config.volcMode;
        if (!config.volcLanguage.empty()) vcfg.language = config.volcLanguage;
        vcfg.enableNonstream = config.volcEnableNonstream;
        vcfg.endWindowSize = config.volcEndWindowSize;
        vcfg.enableDdc = config.volcEnableDdc;
        vcfg.enableMusicFc = config.volcEnableMusicFc;
        vcfg.enablePoiFc = config.volcEnablePoiFc;
        vcfg.forceToSpeechTime = config.volcForceToSpeechTime;
        vcfg.enableAccelerateText = config.volcEnableAccelerate;
        vcfg.accelerateScore = config.volcAccelerateScore;
        vcfg.extraParams = config.volcExtraParams;
        vcfg.hotwordsId = config.volcHotwordsId;
        vcfg.hotwordsName = config.volcHotwordsName;
        vcfg.correctTableId = config.volcCorrectTableId;
        vcfg.correctTableName = config.volcCorrectTableName;
        if (config.volcEnableContext) {
            vcfg.contextJson = BuildVolcContextJson();
        }

        g_volcStreaming.store(true);
        ShowHud(L"Listening... Volcano Engine");

        if (g_volcThread.joinable()) g_volcThread.join();
        g_volcSession.forceAbort = false;

        g_streamingVadReady = false;
        g_volcVadState = 0;
        g_volcVadSilentCount = 0;
        g_volcVadPreBuffer.clear();
        g_volcVadTailBuffer.clear();
        g_volcVadDoTrim = false;
        g_volcSentBytes = 0;

        bool doVad = g_config.enableVad;
        if (doVad) {
            const int threads = ResolveThreads(g_config.threads);
            g_asrEngine.Lock();
            bool ok = g_asrEngine.EnsureVadForConfig(g_config, threads);
            if (ok) {
                if (g_config.vadModel == L"firered") {
                    g_asrEngine.fireRedVad->Reset();
                } else {
                    g_asrEngine.vad->Reset();
                }
            } else {
                doVad = false;
            }
            g_asrEngine.Unlock();
            if (doVad) {
                g_streamingVadReady = true;
                g_volcVadDoTrim = true;
            }
            VolcDebugLog("Volc thread: VAD active (%ls), trim=%d", g_config.vadModel.c_str(), g_volcVadDoTrim.load() ? 1 : 0);
        }
        g_volcThread = std::thread([vcfg, config]() {
            ULONGLONG tTotal0 = GetTickCount64();

            if (!volc_asr::OpenSession(g_volcSession, vcfg)) {
                if (g_volcStreaming.load() && !g_volcSession.forceAbort.load()) {
                    ShowHud(L"Reconnecting... (1/3)");
                    VolcDebugLog("Volc thread: Round 1 failed, retrying...");
                    Sleep(1500);
                    if (volc_asr::OpenSession(g_volcSession, vcfg)) goto openSessionOk;
                }
                EnterCriticalSection(&g_volcAudioCs);
                bool hasPending = !g_volcPendingAudio.empty();
                LeaveCriticalSection(&g_volcAudioCs);
                if (hasPending && !g_volcSession.forceAbort.load()) {
                    ShowHud(L"Reconnecting... (2/3)");
                    VolcDebugLog("Volc thread: Round 2 failed, final attempt...");
                    Sleep(1500);
                    if (volc_asr::OpenSession(g_volcSession, vcfg)) goto openSessionOk;
                }
                g_volcSession.connected = false;
                std::wstring errMsg = L"VolcEngine connect failed";
                if (!g_volcSession.lastError.empty()) {
                    errMsg = g_volcSession.lastError;
                }
                ShowHud(errMsg);
                if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 4000, nullptr);
                return;
            }
            openSessionOk:
            g_volcSession.connected = true;
            volc_asr::g_volcKeepAlive = true;

            constexpr size_t kChunkBytes = 6400;
            bool asyncMode = (vcfg.mode == L"bigmodel_async");
            bool nostreamMode = (vcfg.mode == L"bigmodel_nostream");
            std::vector<BYTE> chunk;
            chunk.reserve(kChunkBytes);

            auto sendChunk = [&](std::vector<BYTE>& c) -> std::wstring {
                g_volcSentBytes += c.size();
                return volc_asr::SendAudio(g_volcSession, c, false, asyncMode, nostreamMode);
            };

            std::wstring lastPartial;
            std::wstring asyncPartial;
            std::atomic<bool> asyncDrainDone{false};
            std::thread drainThread;

            if (asyncMode || nostreamMode) {
                drainThread = std::thread([&]() {
                    VolcDebugLog("drainThread: started (async=%d nostream=%d)", asyncMode ? 1 : 0, nostreamMode ? 1 : 0);
                    while (!asyncDrainDone && g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()) {
                        std::wstring partial = volc_asr::DrainReceiveBuffer(g_volcSession.hWebSocket, &g_volcSession);
                        if (!partial.empty() && partial != asyncPartial) {
                            asyncPartial = partial;
                            ShowHud(L"Listening... Volcano Engine\n" + partial);
                        }
                    }
                    VolcDebugLog("drainThread: main loop exited, doing final drain...");
                    while (g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()) {
                        std::wstring partial = volc_asr::DrainReceiveBuffer(g_volcSession.hWebSocket, &g_volcSession);
                        if (!partial.empty()) {
                            asyncPartial = partial;
                        } else {
                            break;
                        }
                    }
                    VolcDebugLog("drainThread: done");
                });
            }

            while (true) {
                if (g_volcSession.forceAbort.load()) break;
                bool hasData = false;
                EnterCriticalSection(&g_volcAudioCs);
                while (!g_volcPendingAudio.empty() && chunk.size() < kChunkBytes) {
                    size_t take = (std::min)(kChunkBytes - chunk.size(), g_volcPendingAudio.size());
                    chunk.insert(chunk.end(), g_volcPendingAudio.begin(), g_volcPendingAudio.begin() + take);
                    g_volcPendingAudio.erase(g_volcPendingAudio.begin(), g_volcPendingAudio.begin() + take);
                    hasData = true;
                }
                bool streaming = g_volcStreaming.load();
                LeaveCriticalSection(&g_volcAudioCs);

                if (hasData && chunk.size() >= kChunkBytes) {
                    std::wstring partial = sendChunk(chunk);
                    chunk.clear();
                    if (!g_volcSession.hWebSocket) break;
                    if (!asyncMode && !partial.empty() && partial != lastPartial) {
                        lastPartial = partial;
                        ShowHud(L"Listening... Volcano Engine\n" + partial);
                    }
                } else if (!streaming) {
                    break;
                } else {
                    Sleep(20);
                }
            }

            if (g_volcSession.forceAbort.load()) {
                VolcDebugLog("Volc thread: forceAbort detected, skipping drain");
            }

            if (!g_volcSession.forceAbort.load() && !chunk.empty()) {
                std::wstring partial = sendChunk(chunk);
                if (!asyncMode && !partial.empty()) lastPartial = partial;
            }

            int chunksSent = g_volcSession.sequence - 2;
            VolcDebugLog("Volc thread: send loop ended, chunks_sent=%d, vad_state=%d", chunksSent, g_volcVadState.load());

            if (g_volcVadState == 0 && g_volcVadDoTrim) {
                VolcDebugLog("Volc thread: no speech detected, forcing drainThread exit...");
                asyncDrainDone = true;
                g_volcSession.forceAbort = true;
                if (g_volcSession.hWebSocket) {
                    WinHttpCloseHandle(g_volcSession.hWebSocket);
                }
                if (drainThread.joinable()) drainThread.join();
                g_volcSession.hWebSocket = nullptr;
                if (g_volcSession.hConnect) {
                    WinHttpCloseHandle(g_volcSession.hConnect);
                    g_volcSession.hConnect = nullptr;
                }
                if (volc_asr::g_volcKeepAlive) {
                    g_volcSession.lastUsedTick = GetTickCount64();
                } else if (g_volcSession.hSession) {
                    WinHttpCloseHandle(g_volcSession.hSession);
                    g_volcSession.hSession = nullptr;
                }
                g_volcSession.connected = false;
                PostMessageW(g_mainWindow, kAsrResultMessage, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(L"No speech detected")));
                VolcDebugLog("=== TOTAL session: %llums (no speech) ===", GetTickCount64() - tTotal0);
                return;
            }

            if (!g_volcSession.forceAbort.load()) {
                std::vector<BYTE> empty;
                std::wstring lastResult = volc_asr::SendAudio(g_volcSession, empty, true, asyncMode, nostreamMode);
                if (!lastResult.empty()) lastPartial = lastResult;
            }

            if (asyncMode || nostreamMode) {
                asyncDrainDone = true;
                if (drainThread.joinable()) drainThread.join();
            }

            std::wstring finalText = volc_asr::CloseSession(g_volcSession);
            if (finalText.empty()) finalText = lastPartial;
            if (finalText.empty()) finalText = asyncPartial;

            if (finalText.empty()) finalText = L"(empty result)";
            if (g_volcSession.forceAbort.load() && finalText == L"(empty result)") {
                finalText = L"VolcEngine timeout";
            }

            bool needLlm = (config.postprocess == L"llm")
                         && !config.llmEndpoint.empty()
                         && !config.llmApiKey.empty()
                         && !finalText.empty()
                         && finalText.rfind(L"VolcEngine error", 0) != 0
                         && finalText.rfind(L"VolcEngine timeout", 0) != 0;

            if (needLlm) {
                g_lastRawAsrText = finalText;
                PostMessageW(g_mainWindow, kAsrResultMessage, 1,
                             reinterpret_cast<LPARAM>(new std::wstring(finalText)));
                RefineWithLlmAsync(finalText, config);
            } else {
                PostMessageW(g_mainWindow, kAsrResultMessage, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(finalText)));
            }
            AddVolcRecognitionHistory(finalText);
            VolcDebugLog("=== TOTAL session: %llums ===", GetTickCount64() - tTotal0);
            g_cloudApiMs = static_cast<double>(GetTickCount64() - tTotal0) - g_recordingMs;
        });
        SetTimer(g_mainWindow, kVolcWatchdogTimer, 18000, nullptr);
        return;
    }

    std::wstring name = (g_config.asrBackend == L"baidu") ? L"Baidu Cloud" : ModelDisplayName(g_config.modelId);
    ShowHud(L"Listening... " + name);

    g_streamingVadReady = false;
    g_streamingVadSamples.clear();
    if (g_config.asrBackend != L"baidu" && g_config.asrBackend != L"volcengine" && g_config.enableVad) {
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

    if (g_config.asrBackend == L"volcengine" && g_volcStreaming.load()) {
        g_volcStreaming.store(false);
        g_streamingVadReady = false;
        const std::vector<BYTE> pcm = StopAudioCapture();
        if (pcm.size() < 8000) {
            ShowHud(L"Too short");
            SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
            return;
        }
        if (g_volcSession.connected.load()) {
            ShowHud(L"Recognizing... Volcano Engine");
        }
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

    std::wstring name = (g_config.asrBackend == L"baidu") ? L"Baidu Cloud" : ModelDisplayName(g_config.modelId);
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
    case kAsrResultMessage: {
        KillTimer(g_mainWindow, kVolcWatchdogTimer);
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
        const std::wstring text = result ? *result : L"ASR failed";
        if (wParam == 1 && !text.empty() && text.rfind(L"ASR failed:", 0) != 0) {
            g_hudIsRefining = true;
            ShowHud(L"Refining...");
        } else {
            g_hudIsRefining = false;
            ShowHud(text.empty() ? L"(empty result)" : text);
            if (!text.empty() && text != L"No speech detected" && text.rfind(L"ASR failed:", 0) != 0) {
                HiResTimer tPaste;
                PasteTextImeAware(text);
                double pasteMs = tPaste.ElapsedMs();

                if (g_config.enableDebugMode) {
                    DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

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
                            size_t trimBytes = g_vadTrimmedSamples * 2;
                            double trimMs = g_vadTrimmedSamples / 16.0;
                            printf("  VAD trim: %.1fs/%zuKB -> %.1fs/%zuKB (%.0f%%)\n",
                                   g_recordingMs / 1000.0, rawBytes / 1024,
                                   trimMs / 1000.0, trimBytes / 1024,
                                   rawBytes > 0 ? 100.0 * trimBytes / rawBytes : 0.0);
                        }
                    } else {
                        const char* backend = (g_config.asrBackend == L"baidu") ? "Baidu" : "Volcengine";
                        printf("  Pipeline: %s %.0f | Paste %.0f = Total %.0fms\n",
                               backend, g_cloudApiMs, pasteMs, g_cloudApiMs + pasteMs);
                        if (g_config.asrBackend == L"volcengine" && g_volcVadDoTrim) {
                            size_t rawBytes = static_cast<size_t>(g_recordingMs * 32.0);
                            if (g_lastPcmBytes > 0) rawBytes = g_lastPcmBytes;
                            double trimMs = g_volcSentBytes / 32.0;
                            printf("  VAD trim: %.1fs/%zuKB -> %.1fs/%zuKB (%.0f%%)\n",
                                   g_recordingMs / 1000.0, rawBytes / 1024,
                                   trimMs / 1000.0, g_volcSentBytes / 1024,
                                   rawBytes > 0 ? 100.0 * g_volcSentBytes / rawBytes : 0.0);
                        }
                    }

                    DebugPrintTextLine(L"OK", text);
                }
            }
            if (g_hudWindow) {
                const bool isError = text.rfind(L"ASR failed:", 0) == 0;
                const bool isNoSpeech = (text == L"No speech detected");
                UINT hideMs = isError ? 2200 : (isNoSpeech ? 1500 : 200);
                SetTimer(g_hudWindow, kHudHideTimer, hideMs, nullptr);
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

                if (g_config.asrBackend == L"local") {
                    printf("  Pipeline: ");
                    if (g_vadMs > 0) printf("VAD(%ls) %.0f | ", g_vadModelName.c_str(), g_vadMs);
                    printf("ASR %.0f", g_asrDecodeMs);
                    if (g_punctMs > 0) printf(" | Punct %.0f", g_punctMs);
                    printf(" | LLM %.0f | Paste %.0f = Total %.0fms\n",
                           g_llmMs, pasteMs,
                           g_vadMs + g_asrDecodeMs + g_punctMs + g_llmMs + pasteMs);
                } else {
                    const char* backend = (g_config.asrBackend == L"baidu") ? "Baidu" : "Volcengine";
                    printf("  Pipeline: %s %.0f | LLM %.0f | Paste %.0f = Total %.0fms\n",
                           backend, g_cloudApiMs, g_llmMs, pasteMs,
                           g_cloudApiMs + g_llmMs + pasteMs);
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
        if (wParam == kVolcWatchdogTimer) {
            KillTimer(hwnd, kVolcWatchdogTimer);
            if (g_recording) {
                SetTimer(hwnd, kVolcWatchdogTimer, 18000, nullptr);
                return 0;
            }
            if (g_volcThread.joinable()) {
                VolcDebugLog("Watchdog: volc thread still running after 18s, force aborting");
                g_volcSession.forceAbort = true;
                HINTERNET hWs = g_volcSession.hWebSocket;
                if (hWs) { WinHttpCloseHandle(hWs); g_volcSession.hWebSocket = nullptr; }
                HINTERNET hConn = g_volcSession.hConnect;
                if (hConn) { WinHttpCloseHandle(hConn); g_volcSession.hConnect = nullptr; }
                HINTERNET hSess = g_volcSession.hSession;
                if (hSess) { WinHttpCloseHandle(hSess); g_volcSession.hSession = nullptr; }
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
        g_volcStreaming.store(false);
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
        if (g_volcThread.joinable()) {
            if (g_volcThread.get_id() != std::this_thread::get_id()) {
                g_volcThread.join();
            }
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
    InitializeCriticalSection(&g_volcAudioCs);
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
        DeleteCriticalSection(&g_volcAudioCs);
        return 0;
    }

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"Failed to register window classes.", kAppName, MB_OK | MB_ICONERROR);
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
        DeleteCriticalSection(&g_volcAudioCs);
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
        DeleteCriticalSection(&g_volcAudioCs);
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
    DeleteCriticalSection(&g_volcAudioCs);
    return static_cast<int>(msg.wParam);
}
