#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mmsystem.h>

#include <atomic>
#include <string>
#include <vector>

#include "sherpa-onnx/c-api/cxx-api.h"
#include "firered_vad.h"
#include "llm_refine.h"
#include "baidu_asr.h"
#include "volcengine_asr.h"
#include "wasapi_capture.h"
#include "resource.h"

constexpr wchar_t kAppName[] = L"VoxType";
constexpr wchar_t kMainClass[] = L"VoxType.Main";
constexpr wchar_t kSettingsClass[] = L"VoxType.Settings";
constexpr wchar_t kHudClass[] = L"VoxType.Hud";
constexpr wchar_t kHotkeyEditClass[] = L"VoxType.HotkeyEdit";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kReloadMessage = WM_APP + 2;
constexpr UINT kAsrResultMessage = WM_APP + 3;
constexpr UINT kLlmResultMessage = WM_APP + 4;
constexpr UINT kPreloadDoneMessage = WM_APP + 5;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kHudHideTimer = 1;
constexpr UINT_PTR kCapsLockLongPressTimer = 2;
constexpr UINT_PTR kHudAnimationTimer = 3;
constexpr UINT_PTR kVolcWatchdogTimer = 4;
constexpr UINT kCapsLockLongPressMs = 300;
constexpr int kHudMinWidth = 300;
constexpr int kHudMinHeight = 56;
constexpr int kHudScreenMarginX = 80;
constexpr int kHudScreenMarginY = 96;
constexpr float kHudLeftPad = 22.0f;
constexpr float kHudWaveWidth = 52.0f;
constexpr float kHudGap = 14.0f;
constexpr float kHudRightPad = 22.0f;
constexpr float kHudTextSlack = 18.0f;

namespace UiStyle {
constexpr int Margin = 12;
constexpr int ContentLeft = 42;
constexpr int InputLeft = 188;
constexpr int LabelWidth = 130;
constexpr int RowHeight = 52;
constexpr int FirstRowY = 76;
constexpr int LabelYOffset = 6;
constexpr int LabelH = 30;
constexpr int EditH = 32;
constexpr int ComboH = 150;
constexpr int BtnH = 34;
constexpr int ActionBtnH = 36;
constexpr int CheckH = 26;
constexpr int HotkeyEditH = 38;
constexpr int InputW = 480;
constexpr int InputWFull = 580;
constexpr int ComboW = 250;
constexpr int SideBtnW = 92;
constexpr int SideBtnX = 682;
constexpr int SmallBtnW = 62;
constexpr int SmallBtnX = 532;
constexpr int ActionBtnW = 140;
constexpr int FooterBtnW = 84;
constexpr int FooterHeight = 78;
constexpr int FooterMinTop = 640;
constexpr COLORREF BgColor = RGB(246, 248, 251);
constexpr COLORREF ControlBgColor = RGB(255, 255, 255);
constexpr COLORREF TextColor = RGB(30, 41, 59);
constexpr COLORREF InputTextColor = RGB(17, 24, 39);
constexpr COLORREF DividerColor = RGB(226, 232, 240);
constexpr COLORREF HintTextColor = RGB(120, 130, 145);
constexpr int RowInputY(int row) { return FirstRowY + row * RowHeight; }
constexpr int RowLabelY(int row) { return FirstRowY + LabelYOffset + row * RowHeight; }
// UiStyle constants are designed for 150% DPI (144 dpi).
// Scale = DpiScaleForWindow * 96/144; S() converts design px to physical px.
extern float Scale;
}

constexpr UINT ID_TRAY_VERSION = 1001;
constexpr UINT ID_TRAY_SETTINGS = 1002;
constexpr UINT ID_TRAY_RELOAD = 1003;
constexpr UINT ID_TRAY_QUIT = 1004;
constexpr UINT ID_TRAY_DEBUG_MODE = 1005;
constexpr UINT ID_TRAY_FORCE_UNICODE = 1006;

constexpr int IDC_MODEL = 2001;
constexpr int IDC_MODEL_DIR = 2002;
constexpr int IDC_BROWSE = 2003;
constexpr int IDC_THREADS = 2004;
constexpr int IDC_VAD = 2005;
constexpr int IDC_PARTIAL = 2006;
constexpr int IDC_POSTPROCESS = 2007;
constexpr int IDC_HOTKEY = 2008;
constexpr int IDC_SAVE = 2009;
constexpr int IDC_CANCEL = 2010;
constexpr int IDC_STATUS = 2011;
constexpr int IDC_SETTINGS_TAB = 2012;
constexpr int IDC_VAD_MODEL = 2013;
constexpr int IDC_LLM_ENDPOINT = 2020;
constexpr int IDC_LLM_KEY = 2021;
constexpr int IDC_LLM_MODEL = 2022;
constexpr int IDC_LLM_TEST = 2023;
constexpr int IDC_LLM_SHOW_KEY = 2024;
constexpr int IDC_LLM_DEBUG = 2025;
constexpr int IDC_LLM_PROMPT = 2026;
constexpr int IDC_LLM_PRESET_COMBO = 2027;
constexpr int IDC_LLM_PRESET_DESC = 2028;
constexpr int IDC_LLM_PROMPT_HINT = 2029;
constexpr int IDC_LLM_PROVIDER = 2030;
constexpr int IDC_LLM_EXTRA = 2031;
constexpr int IDC_LLM_PROVIDER_ADD = 2032;
constexpr int IDC_LLM_PROVIDER_DEL = 2033;
constexpr int IDC_LLM_EXTRA_RESET = 2034;
constexpr int IDC_DOWNLOAD_MODELS = 2035;
constexpr int IDC_ASR_BACKEND = 2036;
constexpr int IDC_BAIDU_API_KEY = 2037;
constexpr int IDC_BAIDU_SECRET_KEY = 2038;
constexpr int IDC_BAIDU_DEV_PID = 2039;
constexpr int IDC_BAIDU_TEST = 2040;
constexpr int IDC_BAIDU_SHOW_KEY = 2041;
constexpr int IDC_CLOUD_PROVIDER = 2042;
constexpr int IDC_VOLC_API_KEY = 2043;
constexpr int IDC_VOLC_RESOURCE = 2044;
constexpr int IDC_VOLC_LANGUAGE = 2045;
constexpr int IDC_VOLC_TEST = 2046;
constexpr int IDC_VOLC_SHOW_KEY = 2047;
constexpr int IDC_BAIDU_SHOW_API_KEY = 2048;
constexpr int IDC_VOLC_MODE = 2049;
constexpr int IDC_VOLC_ENABLE_NONSTREAM = 2050;
constexpr int IDC_VOLC_END_WINDOW_SIZE = 2051;
constexpr int IDC_VOLC_ENABLE_DDC = 2052;
constexpr int IDC_VOLC_EXTRA_PARAMS = 2053;
constexpr int IDC_VOLC_ENABLE_CONTEXT = 2054;
constexpr int IDC_VOLC_CONTEXT_HISTORY = 2055;
constexpr int IDC_VOLC_ENABLE_MUSIC_FC = 2056;
constexpr int IDC_VOLC_HOTWORDS_ID = 2057;
constexpr int IDC_VOLC_FORCE_TO_SPEECH_TIME = 2058;
constexpr int IDC_VOLC_ENABLE_POI_FC = 2059;
constexpr int IDC_VOLC_ENABLE_ACCELERATE = 2060;
constexpr int IDC_VOLC_ACCELERATE_SCORE = 2061;
constexpr int IDC_VOLC_HOTWORDS_NAME = 2062;
constexpr int IDC_VOLC_CORRECT_TABLE_ID = 2063;
constexpr int IDC_VOLC_CORRECT_TABLE_NAME = 2064;

struct Config {
    std::wstring modelId = L"firered_ctc";
    std::wstring modelDir;
    std::wstring threads = L"auto";
    bool enableVad = false;
    std::wstring vadModel = L"firered";
    bool enablePartial = false;
    std::wstring postprocess = L"itn";
    std::wstring hotkey = L"CapsLock";
    std::wstring llmProvider = L"DeepSeek";
    std::wstring llmEndpoint = L"https://api.deepseek.com";
    std::wstring llmApiKey;
    std::wstring llmModel = L"deepseek-v4-flash";
    std::wstring llmPrompt;
    std::wstring llmExtraParams;
    bool enableLlmDebug = false;
    std::wstring llmProvidersJson;
    std::wstring asrBackend = L"local";
    std::wstring baiduApiKey;
    std::wstring baiduSecretKey;
    int baiduDevPid = 1537;
    std::wstring cloudProvider = L"volcengine";
    std::wstring volcApiKey;
    std::wstring volcResourceId = L"volc.seedasr.sauc.duration";
    std::wstring volcMode = L"bigmodel";
    std::wstring volcLanguage;
    bool volcEnableNonstream = false;
    int volcEndWindowSize = 800;
    bool volcEnableDdc = false;
    std::wstring volcExtraParams;
    bool volcEnableContext = false;
    int volcContextHistory = 3;
    bool volcEnableMusicFc = false;
    bool volcEnablePoiFc = false;
    int volcForceToSpeechTime = 0;
    bool volcEnableAccelerate = false;
    int volcAccelerateScore = 0;
    std::wstring volcHotwordsId;
    std::wstring volcHotwordsName;
    std::wstring volcCorrectTableId;
    std::wstring volcCorrectTableName;
    bool enableDebugMode = false;
    bool forceUnicodeInput = false;
    std::wstring audioBackend = L"wasapi";
    std::wstring audioDeviceId;
};

struct HotkeyConfig {
    bool ctrl = false;
    bool alt = false;
    bool shift = false;
    bool win = false;
    UINT key = VK_CAPITAL;

    bool IsEmpty() const { return key == 0; }
};

struct HotkeyEditState {
    HotkeyConfig hotkey;
    HotkeyConfig original;
    bool capturing = false;
};

struct HudSize {
    float widthDip = static_cast<float>(kHudMinWidth);
    float heightDip = static_cast<float>(kHudMinHeight);
};

struct VolcMapping {
    int comboIdx; const wchar_t* resourceId;
};
constexpr VolcMapping kVolcResources[] = {
    {0, L"volc.seedasr.sauc.duration"},
    {1, L"volc.seedasr.sauc.concurrent"},
    {2, L"volc.bigasr.sauc.duration"},
    {3, L"volc.bigasr.sauc.concurrent"},
};
constexpr const wchar_t* kVolcLanguages[] = {
    L"", L"en-US", L"ja-JP", L"ko-KR", L"fr-FR",
    L"de-DE", L"es-MX", L"pt-BR", L"id-ID",
};

class AsrEngine;

extern HINSTANCE g_instance;
extern HWND g_mainWindow;
extern HWND g_settingsWindow;
extern HWND g_hudWindow;
extern HHOOK g_keyboardHook;
extern HICON g_appIcon;
extern HFONT g_uiFont;
extern HFONT g_titleFont;
extern HFONT g_sectionFont;
extern HBRUSH g_settingsBgBrush;
extern HBRUSH g_cardBrush;
extern HBRUSH g_controlBgBrush;
extern ID2D1Factory* g_d2dFactory;
extern IDWriteFactory* g_dwriteFactory;
extern ID2D1HwndRenderTarget* g_hudRenderTarget;
extern ID2D1SolidColorBrush* g_hudBrush;
extern ID2D1LinearGradientBrush* g_hudBarGradientRec;
extern ID2D1LinearGradientBrush* g_hudBarGradientIdle;
extern ID2D1GradientStopCollection* g_hudBarGradientStopsRec;
extern ID2D1GradientStopCollection* g_hudBarGradientStopsIdle;
extern IDWriteTextFormat* g_hudTextFormat;
extern Config g_config;
extern bool g_recording;
extern UINT g_activeHotkeyKey;
extern bool g_capsLockHotkeyPending;
extern bool g_capsLockLongPressActive;
extern bool g_capsLockWasOn;
extern std::wstring g_hudText;
extern HWAVEIN g_waveIn;
extern WAVEHDR g_waveHeaders[4];
extern std::vector<std::vector<BYTE>> g_waveBuffers;
extern std::vector<BYTE> g_audioData;
extern CRITICAL_SECTION g_audioLock;
extern bool g_captureActive;
extern std::atomic<float> g_audioLevel;
extern float g_hudSmoothedLevel;
extern WasapiCapture g_wasapiCapture;
extern std::vector<HWND> g_recognitionControls;
extern std::vector<HWND> g_shortcutControls;
extern std::vector<HWND> g_llmControls;
extern std::vector<HWND> g_promptControls;
extern std::vector<HWND> g_cloudAsrControls;
extern std::vector<HWND> g_baiduControls;
extern std::vector<HWND> g_volcengineControls;
extern bool g_hudIsRefining;
extern bool g_llmKeyVisible;
extern bool g_baiduKeyVisible;
extern bool g_baiduApiKeyVisible;
extern bool g_volcKeyVisible;
extern volc_asr::VolcSession g_volcSession;
extern std::atomic<bool> g_volcStreaming;
extern std::thread g_volcThread;
extern CRITICAL_SECTION g_volcAudioCs;
extern std::vector<BYTE> g_volcPendingAudio;
namespace volc_asr { extern std::atomic<bool> g_volcKeepAlive; }
extern AsrEngine g_asrEngine;
extern int g_cloudProviderIdx;
extern HWND g_cloudAsrHintControl;

void StartRecordingSession();
void StopRecordingSession();

extern double g_vadMs;
extern double g_asrDecodeMs;
extern double g_punctMs;
extern double g_cloudApiMs;
extern double g_llmMs;
extern std::wstring g_vadModelName;
