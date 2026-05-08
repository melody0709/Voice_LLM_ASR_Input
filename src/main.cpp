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
std::vector<HWND> g_recognitionControls;
std::vector<HWND> g_shortcutControls;
std::vector<HWND> g_llmControls;
std::vector<HWND> g_promptControls;
std::vector<HWND> g_cloudAsrControls;
std::vector<HWND> g_baiduControls;
std::vector<HWND> g_volcengineControls;
bool g_hudIsRefining = false;
bool g_llmKeyVisible = false;
bool g_baiduKeyVisible = false;
bool g_baiduApiKeyVisible = false;
bool g_volcKeyVisible = false;
volc_asr::VolcSession g_volcSession;
bool g_volcStreaming = false;
std::thread g_volcThread;
CRITICAL_SECTION g_volcAudioCs;
std::vector<BYTE> g_volcPendingAudio;
AsrEngine g_asrEngine;
int g_cloudProviderIdx = 0;
HWND g_cloudAsrHintControl = nullptr;

static std::deque<std::wstring> g_volcRecognitionHistory;

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

static void AddVolcRecognitionHistory(const std::wstring& text) {
    if (text.empty() || text == L"(empty result)" || text == L"Too short") return;
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
        std::wstring result = llm::Refine(asrText, cfg);
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
            baidu_asr::BaiduConfig bcfg;
            bcfg.apiKey = config.baiduApiKey;
            bcfg.secretKey = config.baiduSecretKey;
            bcfg.devPid = config.baiduDevPid;

            std::wstring text = baidu_asr::Recognize(pcm, bcfg);
            if (text.empty()) {
                text = L"(empty result)";
            }

            bool needLlm = (config.postprocess == L"llm")
                         && !config.llmEndpoint.empty()
                         && !config.llmApiKey.empty()
                         && !text.empty()
                         && text.rfind(L"Baidu ASR error:", 0) != 0;

            if (needLlm) {
                PostMessageW(g_mainWindow, kAsrResultMessage, 1, reinterpret_cast<LPARAM>(new std::wstring(text)));
                RefineWithLlmAsync(text, config);
            } else {
                PostMessageW(g_mainWindow, kAsrResultMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
            }
        }).detach();
        return;
    }

    std::thread([config, pcm]() {
        auto samples = PcmToFloat(pcm);
        std::wstring text = g_asrEngine.Recognize(samples, 16000, config);
        if (text.empty()) {
            text = L"(empty result)";
        }

        bool needLlm = (config.postprocess == L"llm")
                     && !config.llmEndpoint.empty()
                     && !config.llmApiKey.empty()
                     && !text.empty()
                     && text.rfind(L"ASR failed:", 0) != 0;

        if (needLlm) {
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
    g_recording = true;

    if (g_config.asrBackend == L"volcengine") {
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

        g_volcStreaming = true;
        ShowHud(L"Listening... Volcano Engine");

        if (g_volcThread.joinable()) g_volcThread.join();
        g_volcThread = std::thread([vcfg, config]() {
            if (!volc_asr::OpenSession(g_volcSession, vcfg)) {
                g_volcSession.connected = false;
                std::wstring errMsg = L"VolcEngine connect failed";
                if (!g_volcSession.lastError.empty()) {
                    errMsg = g_volcSession.lastError;
                }
                ShowHud(errMsg);
                if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 4000, nullptr);
                return;
            }
            g_volcSession.connected = true;

            constexpr size_t kChunkBytes = 6400;
            bool asyncMode = (vcfg.mode == L"bigmodel_async");
            std::vector<BYTE> chunk;
            chunk.reserve(kChunkBytes);

            std::wstring lastPartial;
            std::wstring asyncPartial;
            bool asyncDrainDone = false;
            std::thread drainThread;

            if (asyncMode) {
                drainThread = std::thread([&]() {
                    while (!asyncDrainDone && g_volcSession.hWebSocket) {
                        std::wstring partial = volc_asr::DrainReceiveBuffer(g_volcSession.hWebSocket);
                        if (!partial.empty() && partial != asyncPartial) {
                            asyncPartial = partial;
                            ShowHud(L"Listening... Volcano Engine\n" + partial);
                        }
                    }
                    while (g_volcSession.hWebSocket) {
                        std::wstring partial = volc_asr::DrainReceiveBuffer(g_volcSession.hWebSocket);
                        if (!partial.empty()) {
                            asyncPartial = partial;
                        } else {
                            break;
                        }
                    }
                });
            }

            while (true) {
                bool hasData = false;
                EnterCriticalSection(&g_volcAudioCs);
                while (!g_volcPendingAudio.empty() && chunk.size() < kChunkBytes) {
                    size_t take = (std::min)(kChunkBytes - chunk.size(), g_volcPendingAudio.size());
                    chunk.insert(chunk.end(), g_volcPendingAudio.begin(), g_volcPendingAudio.begin() + take);
                    g_volcPendingAudio.erase(g_volcPendingAudio.begin(), g_volcPendingAudio.begin() + take);
                    hasData = true;
                }
                bool streaming = g_volcStreaming;
                LeaveCriticalSection(&g_volcAudioCs);

                if (hasData && chunk.size() >= kChunkBytes) {
                    std::wstring partial = volc_asr::SendAudio(g_volcSession, chunk, false, asyncMode);
                    chunk.clear();
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

            if (!chunk.empty()) {
                std::wstring partial = volc_asr::SendAudio(g_volcSession, chunk, false, asyncMode);
                if (!asyncMode && !partial.empty()) lastPartial = partial;
            }

            {
                std::vector<BYTE> empty;
                std::wstring lastResult = volc_asr::SendAudio(g_volcSession, empty, true);
                if (!lastResult.empty()) lastPartial = lastResult;
            }

            if (asyncMode) {
                asyncDrainDone = true;
                if (drainThread.joinable()) drainThread.join();
                if (!asyncPartial.empty()) lastPartial = asyncPartial;
            }

            std::wstring finalText = volc_asr::CloseSession(g_volcSession);
            if (finalText.empty()) finalText = lastPartial;

            if (finalText.empty()) finalText = L"(empty result)";

            bool needLlm = (config.postprocess == L"llm")
                         && !config.llmEndpoint.empty()
                         && !config.llmApiKey.empty()
                         && !finalText.empty()
                         && finalText.rfind(L"VolcEngine error", 0) != 0;

            if (needLlm) {
                PostMessageW(g_mainWindow, kAsrResultMessage, 1,
                             reinterpret_cast<LPARAM>(new std::wstring(finalText)));
                RefineWithLlmAsync(finalText, config);
            } else {
                PostMessageW(g_mainWindow, kAsrResultMessage, 0,
                             reinterpret_cast<LPARAM>(new std::wstring(finalText)));
            }
            AddVolcRecognitionHistory(finalText);
        });
        return;
    }

    std::wstring name = (g_config.asrBackend == L"baidu") ? L"Baidu Cloud" : ModelDisplayName(g_config.modelId);
    ShowHud(L"Listening... " + name);
}

void StopRecordingSession() {
    if (!g_recording) return;
    g_recording = false;

    if (g_config.asrBackend == L"volcengine" && g_volcStreaming) {
        g_volcStreaming = false;
        const std::vector<BYTE> pcm = StopAudioCapture();
        if (pcm.size() < 8000) {
            ShowHud(L"Too short");
            SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
            if (g_volcThread.joinable()) g_volcThread.join();
            volc_asr::CloseSession(g_volcSession);
            return;
        }
        ShowHud(L"Recognizing... Volcano Engine");
        return;
    }

    const std::vector<BYTE> pcm = StopAudioCapture();
    if (pcm.size() < 8000) {
        ShowHud(L"Too short");
        SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
        return;
    }

    std::wstring name = (g_config.asrBackend == L"baidu") ? L"Baidu Cloud" : ModelDisplayName(g_config.modelId);
    ShowHud(L"Recognizing... " + name);
    RecognizeAsync(pcm);
}

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, ID_TRAY_VERSION, APP_VERSION_WSTR);
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD, L"Reload ASR Engine");
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
        if (g_config.asrBackend != L"local") {
            std::wstring name = (g_config.asrBackend == L"baidu") ? L"Baidu Cloud"
                : (g_config.asrBackend == L"volcengine") ? L"Volcengine" : L"Cloud";
            ShowHud(L"ASR ready: " + name);
            if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
        }
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ShowSettingsWindow(hwnd);
        }
        return 0;
    case kReloadMessage:
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
        ShowHud(L"ASR ready: " + ModelDisplayName(g_config.modelId));
        if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1500, nullptr);
        return 0;
    case kAsrResultMessage: {
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
        const std::wstring text = result ? *result : L"ASR failed";
        if (wParam == 1 && !text.empty() && text.rfind(L"ASR failed:", 0) != 0) {
            g_hudIsRefining = true;
            ShowHud(L"Refining...");
        } else {
            g_hudIsRefining = false;
            ShowHud(text.empty() ? L"(empty result)" : text);
            if (!text.empty() && text.rfind(L"ASR failed:", 0) != 0) {
                SetClipboardText(text);
                SendCtrlV();
            }
            if (g_hudWindow) {
                const bool isError = text.rfind(L"ASR failed:", 0) == 0;
                SetTimer(g_hudWindow, kHudHideTimer, isError ? 2200 : 200, nullptr);
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
            SetClipboardText(text);
            SendCtrlV();
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
        default:
            break;
        }
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
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
    InitializeCriticalSection(&g_volcAudioCs);
    InitCommonControls();
    g_appIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_appIcon) {
        g_appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    CreateUiResources();

    LoadConfig();

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

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VoxType.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"VoxType is already running.", kAppName, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
        return 0;
    }

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"Failed to register window classes.", kAppName, MB_OK | MB_ICONERROR);
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
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
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    DeleteUiResources();
    DeleteCriticalSection(&g_audioLock);
    return static_cast<int>(msg.wParam);
}
