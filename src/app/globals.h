#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <mmsystem.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <thread>

#include "sherpa-onnx/c-api/cxx-api.h"
#include "firered_vad.h"
#include "llm_refine.h"
#include "baidu_asr.h"
#include "audio_diagnostics.h"
#include "wasapi_capture.h"
#include "input_context.h"
#include "qwen_free_postprocess.h"
#include "resource.h"

class IStreamingAsrSession;
class StreamingVadTrimmer;

constexpr wchar_t kAppName[] = L"VoxType";
constexpr wchar_t kMainClass[] = L"VoxType.Main";
constexpr wchar_t kSettingsClass[] = L"VoxType.Settings";
constexpr wchar_t kHudClass[] = L"VoxType.Hud";
constexpr wchar_t kHotkeyEditClass[] = L"VoxType.HotkeyEdit";
// Beijing DashScope workspace endpoint supplied for the Audio 3 models.
// The API key remains user-configured and is never embedded in the binary.
constexpr wchar_t kQwenBeijingHttpBaseUrl[] =
    L"https://llm-c6rtn7zy4nw0u39k.cn-beijing.maas.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
constexpr wchar_t kQwenBeijingAudioStreamingBaseUrl[] =
    L"wss://llm-c6rtn7zy4nw0u39k.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference";
constexpr wchar_t kQwenBeijingRealtimeBaseUrl[] =
    L"wss://llm-c6rtn7zy4nw0u39k.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime";
constexpr wchar_t kQwenDefaultLanguageHints[] = L"zh,en,yue";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kReloadMessage = WM_APP + 2;
constexpr UINT kAsrResultMessage = WM_APP + 3;
constexpr UINT kLlmResultMessage = WM_APP + 4;
constexpr UINT kPreloadDoneMessage = WM_APP + 5;
constexpr UINT kHudUpdateMessage = WM_APP + 6;
constexpr UINT kDoubaoImeCredentialsMessage = WM_APP + 7;
constexpr UINT kDoubaoImeSettingsRefreshMessage = WM_APP + 8;
constexpr UINT kHudUpdateWithOptionsMessage = WM_APP + 9;
// Main-window only. Settings uses WM_APP + 10 locally for test results.
constexpr UINT kAsrAttemptFinalMessage = WM_APP + 10;
constexpr UINT kHotkeyRecordingMessage = WM_APP + 11;
// Posted by the capture worker when the microphone/driver fails after a
// recording has already started.  The generation in wParam invalidates stale
// messages from a previous recording; lParam carries the native error code.
constexpr UINT kAudioCaptureErrorMessage = WM_APP + 12;
constexpr UINT kWaveInCaptureErrorMessage = WM_APP + 13;
constexpr WPARAM kHotkeyRecordingStart = 1;
constexpr WPARAM kHotkeyRecordingStop = 2;
constexpr WPARAM kHotkeyCapsLockRecordingStop = 3;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kHudHideTimer = 1;
constexpr UINT_PTR kCapsLockLongPressTimer = 2;
constexpr UINT_PTR kHudAnimationTimer = 3;
constexpr UINT_PTR kStreamingWatchdogTimer = 4;
constexpr UINT kCapsLockLongPressMs = 300;
// HUD layout constants — all in DIP; convert to physical pixels via DipToPx().
constexpr float kHudMinWidthDip = 300.0f;
constexpr float kHudMinHeightDip = 56.0f;
constexpr float kHudScreenMarginXDip = 80.0f;
constexpr float kHudScreenMarginYDip = 96.0f;
constexpr float kHudBottomMarginDip = 32.0f;
constexpr float kHudLeftPad = 22.0f;
constexpr float kHudWaveWidth = 52.0f;
constexpr float kHudGap = 14.0f;
constexpr float kHudRightPad = 22.0f;
constexpr float kHudTextSlack = 18.0f;
constexpr float kStreamingPartialHudMaxWidthDip = 900.0f;
constexpr float kStreamingPartialHudMaxScreenFraction = 0.75f;
constexpr int kStreamingPartialHudMaxLines = 4;
constexpr size_t kStreamingPartialHudTailChars = 132;

namespace UiStyle {
constexpr int SettingsWindowW = 850;
constexpr int SettingsWindowH = 740;
// Conservative caption/frame allowance used by the static layout validator.
constexpr int SettingsWindowNonClientReserveH = 48;
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
constexpr int GeneralGroupX = 30;
constexpr int GeneralGroupW = 788;
constexpr int GeneralShortcutGroupH = 170;
constexpr int GeneralStartupGroupGap = 16;
constexpr int GeneralStartupGroupH = 112;
constexpr int GeneralStartupCheckOffsetY = 24;
constexpr int GeneralStartupHintOffsetY = 62;
constexpr int GeneralStartupCheckW = 430;
constexpr int GeneralStartupHintW = 700;
constexpr int GeneralDiagnosticsGroupGap = 16;
constexpr int GeneralDiagnosticsGroupH = 166;
constexpr int GeneralDiagnosticsModeOffsetY = 24;
constexpr int GeneralDiagnosticsActionsOffsetY = 70;
constexpr int GeneralDiagnosticsHintOffsetY = 116;
constexpr int GeneralDiagnosticsModeLabelW = 128;
constexpr int GeneralDiagnosticsModeW = 220;
constexpr int GeneralDiagnosticsOpenButtonW = 230;
constexpr int GeneralDiagnosticsDeleteButtonW = 250;
constexpr int GeneralDiagnosticsButtonGap = 12;
constexpr int GeneralDiagnosticsHintW = 730;
constexpr int InputW = 480;
constexpr int InputWFull = 580;
constexpr int ComboW = 250;
constexpr int PrimaryBackendComboW = 258;
constexpr int FallbackLabelX = 472;
constexpr int FallbackComboX = 548;
constexpr int FallbackComboW = 210;
constexpr int SideBtnW = 92;
constexpr int SideBtnX = 682;
constexpr int SmallBtnW = 88;
constexpr int SmallBtnX = 532;
constexpr int ActionBtnW = 140;
constexpr int FooterBtnW = 84;
constexpr int FooterHeight = 68;
constexpr int FooterMinTop = 632;
constexpr int CloudAsrHintY = 48;
constexpr int CloudAsrHintW = 740;
constexpr int QwenHintH = 24;
constexpr int QwenHint2LineH = 48;
constexpr int QwenLanguageY = 280;
constexpr int QwenLanguageHintY = 314;
constexpr int QwenChunkY = 366;
constexpr int QwenChunkHintY = 400;
constexpr int QwenLanguageHintsY = 452;
constexpr int QwenLanguageHintsHintY = 486;
constexpr int QwenAdvancedButtonY = 538;
constexpr int QwenAdvancedHintY = 578;
constexpr int QwenAdvancedDialogW = 720;
constexpr int QwenAdvancedDialogH = 860;
// QwenAdvancedDialogH is the outer window height. Reserve room for the
// caption/frame before validating client-area controls.
constexpr int QwenAdvancedDialogNonClientReserveH = 48;
constexpr int QwenAdvancedDialogLeft = 24;
constexpr int QwenAdvancedDialogLabelW = 620;
constexpr int QwenAdvancedDialogInputLeft = 170;
constexpr int QwenAdvancedDialogInputW = 500;
constexpr int QwenAdvancedDialogVocabIdLabelY = 28;
constexpr int QwenAdvancedDialogVocabIdY = 56;
constexpr int QwenAdvancedDialogVocabIdHintY = 92;
constexpr int QwenAdvancedDialogVocabJsonLabelY = 146;
constexpr int QwenAdvancedDialogVocabJsonY = 174;
constexpr int QwenAdvancedDialogVocabJsonH = 96;
constexpr int QwenAdvancedDialogVocabJsonHintY = 276;
constexpr int QwenAdvancedDialogStreamingGroupY = 330;
constexpr int QwenAdvancedDialogStreamingGroupH = 430;
constexpr int QwenAdvancedDialogStreamingRow1Y = 362;
constexpr int QwenAdvancedDialogStreamingHint1Y = 396;
constexpr int QwenAdvancedDialogStreamingRow2Y = 448;
constexpr int QwenAdvancedDialogStreamingHint2Y = 482;
constexpr int QwenAdvancedDialogNoiseY = 534;
constexpr int QwenAdvancedDialogNoiseHintY = 568;
constexpr int QwenAdvancedDialogContinueY = 604;
constexpr int QwenAdvancedDialogContinueHintY = 632;
constexpr int QwenAdvancedDialogSpecialLabelY = 682;
constexpr int QwenAdvancedDialogSpecialY = 710;
constexpr int QwenAdvancedDialogSpecialH = 42;
constexpr int QwenAdvancedDialogFooterY = 764;
constexpr int QwenFreeShellPathW = 400;
constexpr int QwenFreeShellBrowseGap = 12;
constexpr int QwenFreeOptionCheckW = 200;
constexpr int QwenFreeOptionGap = 12;
constexpr int QwenFreeRewriteCheckW = 280;
constexpr int QwenFreeDebugCheckW = 150;
constexpr COLORREF BgColor = RGB(246, 248, 251);
constexpr COLORREF ControlBgColor = RGB(255, 255, 255);
constexpr COLORREF TextColor = RGB(30, 41, 59);
constexpr COLORREF InputTextColor = RGB(17, 24, 39);
constexpr COLORREF DividerColor = RGB(226, 232, 240);
constexpr COLORREF HintTextColor = RGB(120, 130, 145);
constexpr int RowInputY(int row) { return FirstRowY + row * RowHeight; }
constexpr int RowLabelY(int row) { return FirstRowY + LabelYOffset + row * RowHeight; }
constexpr int GeneralShortcutGroupY = RowInputY(0);
constexpr int GeneralStartupGroupY = GeneralShortcutGroupY + GeneralShortcutGroupH + GeneralStartupGroupGap;
constexpr int GeneralDiagnosticsGroupY = GeneralStartupGroupY + GeneralStartupGroupH + GeneralDiagnosticsGroupGap;
// UiStyle constants are designed for 150% DPI (144 dpi).
// Scale = DpiScaleForWindow * 96/144; S() converts design px to physical px.
extern float Scale;
}

namespace qwen_asr { class RealtimeClient; }

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
constexpr int IDC_START_WITH_WINDOWS = 2014;
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
constexpr int IDC_ASR_FALLBACK_BACKEND = 2110;
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
constexpr int IDC_VOLC_ENABLE_INPUT_CONTEXT = 2065;
constexpr int IDC_VOLC_ENABLE_MUSIC_FC = 2056;
constexpr int IDC_VOLC_HOTWORDS_ID = 2057;
constexpr int IDC_VOLC_FORCE_TO_SPEECH_TIME = 2058;
constexpr int IDC_VOLC_ENABLE_POI_FC = 2059;
constexpr int IDC_VOLC_HOTWORDS_NAME = 2062;
constexpr int IDC_VOLC_CORRECT_TABLE_ID = 2063;
constexpr int IDC_VOLC_CORRECT_TABLE_NAME = 2064;
constexpr int IDC_VAD_THRESHOLD = 2070;
constexpr int IDC_VAD_MIN_SILENCE = 2071;
constexpr int IDC_VAD_MIN_SPEECH = 2072;
constexpr int IDC_VAD_PAD_START = 2073;
constexpr int IDC_VAD_SMOOTH_WINDOW = 2074;
constexpr int IDC_QWEN_API_KEY = 2080;
constexpr int IDC_QWEN_SHOW_KEY = 2081;
constexpr int IDC_QWEN_BASE_URL = 2082;
constexpr int IDC_QWEN_MODEL = 2083;
constexpr int IDC_QWEN_LANGUAGE = 2084;
constexpr int IDC_QWEN_TEST = 2085;
constexpr int IDC_QWEN_CHUNK_MS = 2089;
constexpr int IDC_QWEN_MODEL_COMBO = 2096;
constexpr int IDC_QWEN_HTTP_BASE_URL = 2097;
constexpr int IDC_QWEN_AUDIO_STREAMING_BASE_URL = 2098;
constexpr int IDC_QWEN_VOCABULARY_ID = 2099;
constexpr int IDC_QWEN_VOCABULARY = 2120;
constexpr int IDC_QWEN_SEMANTIC_PUNCTUATION = 2121;
constexpr int IDC_QWEN_MAX_SENTENCE_SILENCE = 2122;
constexpr int IDC_QWEN_MULTI_THRESHOLD = 2123;
constexpr int IDC_QWEN_HEARTBEAT = 2124;
constexpr int IDC_QWEN_SPEECH_NOISE_THRESHOLD = 2125;
constexpr int IDC_QWEN_LANGUAGE_HINTS = 2126;
constexpr int IDC_QWEN_SPEECH_NOISE_ENABLE = 2127;
constexpr int IDC_QWEN_OPEN_LOG = 2128;
constexpr int IDC_VOLC_OPEN_LOG = 2129;
constexpr int IDC_QWEN_INPUT_CONTEXT = 2130;
constexpr int IDC_QWEN_LANGUAGE_HINTS_RESET = 2131;
constexpr int IDC_QWEN_ADVANCED = 2132;
constexpr int IDC_QWEN_CONTINUE_CONTEXT = 2133;
constexpr int IDC_QWEN_SPECIAL_REPLACE = 2134;
constexpr int IDC_QWEN_SPECIAL_EMPTY = 2135;
constexpr int IDC_QWEN_SYSTEM_FILTER = 2136;
constexpr int IDC_MIMO_API_KEY = 2090;
constexpr int IDC_MIMO_SHOW_KEY = 2091;
constexpr int IDC_MIMO_BASE_URL = 2092;
constexpr int IDC_MIMO_MODEL = 2093;
constexpr int IDC_MIMO_LANGUAGE = 2094;
constexpr int IDC_MIMO_TEST = 2095;
constexpr int IDC_DOUBAO_IME_STATUS = 2100;
constexpr int IDC_DOUBAO_IME_TEST = 2101;
constexpr int IDC_DOUBAO_IME_RESET = 2102;
// QwenFree (千问 IME 免费后端，A1 纯协议还原)
constexpr int IDC_QWEN_FREE_ENABLE = 2200;
constexpr int IDC_QWEN_FREE_POLISH = 2202;
constexpr int IDC_QWEN_FREE_PUNCT = 2203;
constexpr int IDC_QWEN_FREE_CORRECT = 2204;
constexpr int IDC_QWEN_FREE_REWRITE = 2205;
constexpr int IDC_QWEN_FREE_DEBUG = 2206;
constexpr int IDC_QWEN_FREE_SHELL_PATH = 2207;
constexpr int IDC_QWEN_FREE_BROWSE = 2208;
constexpr int IDC_QWEN_FREE_STATUS = 2209;
constexpr int IDC_QWEN_FREE_UTDID = 2210;
constexpr int IDC_QWEN_FREE_TEST = 2211;
constexpr int IDC_DIAGNOSTIC_AUDIO_MODE = 2212;
constexpr int IDC_DIAGNOSTIC_AUDIO_OPEN_FOLDER = 2213;
constexpr int IDC_DIAGNOSTIC_AUDIO_DELETE = 2214;
constexpr int IDC_DIAGNOSTIC_AUDIO_HINT = 2215;

struct Config {
    int configVersion = 0;
    std::wstring modelId = L"firered_ctc";
    std::wstring modelDir;
    std::wstring threads = L"auto";
    bool enableVad = false;
    std::wstring vadModel = L"firered";
    float vadThreshold = 0.15f;
    int vadMinSilence = 500;
    int vadMinSpeech = 30;
    int vadPadStart = 150;
    int vadSmoothWindow = 5;
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
    std::wstring fallbackAsrBackend = L"none";
    std::wstring baiduApiKey;
    std::wstring baiduSecretKey;
    int baiduDevPid = 1537;
    std::wstring cloudProvider = L"volcengine";
    std::wstring volcApiKey;
    std::wstring volcResourceId = L"volc.seedasr.sauc.duration";
    std::wstring volcMode = L"bigmodel_nostream";
    std::wstring volcLanguage;
    bool volcEnableNonstream = false;
    int volcEndWindowSize = 800;
    bool volcEnableDdc = false;
    std::wstring volcExtraParams;
    bool volcEnableContext = false;
    int volcContextHistory = 3;
    bool volcEnableInputContext = false;
    bool volcEnableMusicFc = false;
    bool volcEnablePoiFc = false;
    int volcForceToSpeechTime = 0;
    std::wstring volcHotwordsId;
    std::wstring volcHotwordsName;
    std::wstring volcCorrectTableId;
    std::wstring volcCorrectTableName;
    std::wstring qwenApiKey;
    std::wstring qwenBaseUrl = kQwenBeijingRealtimeBaseUrl;
    std::wstring qwenHttpBaseUrl = kQwenBeijingHttpBaseUrl;
    std::wstring qwenAudioStreamingBaseUrl = kQwenBeijingAudioStreamingBaseUrl;
    std::wstring qwenModel = L"qwen-audio-3.0-asr-flash-streaming";
    std::wstring qwenTransport = L"audio_streaming";
    std::wstring qwenLanguage;
    int qwenChunkMs = 100;
    std::wstring qwenLanguageHints = kQwenDefaultLanguageHints;
    std::wstring qwenVocabularyId;
    std::wstring qwenVocabulary;
    bool qwenSemanticPunctuation = false;
    int qwenMaxSentenceSilenceMs = 1300;
    bool qwenMultiThresholdMode = false;
    bool qwenHeartbeat = false;
    bool qwenSpeechNoiseThresholdEnabled = false;
    float qwenSpeechNoiseThreshold = 0.0f;
    // Audio 3-only optional context refresh. It is deliberately opt-in:
    // when enabled, the streaming worker may send one final focused-field
    // snapshot with continue-task before finish-task.
    bool qwenEnableContinueContext = false;
    // Audio 3-only special-word filter lists. Each field is newline-delimited
    // and remains empty unless the user explicitly configures filtering.
    std::wstring qwenSpecialWordReplaceList;
    std::wstring qwenSpecialWordEmptyList;
    bool qwenSystemReservedFilter = false;
    // Sending the focused input-field text is opt-in.  Password controls and
    // failed/timed-out UI Automation reads are always excluded.
    bool qwenEnableInputContext = false;
    // Runtime-only snapshot captured when an ASR attempt begins. Never persist
    // this field: it may contain sensitive text from the focused control.
    std::wstring qwenInputContextSnapshot;
    bool qwenInputContextSnapshotCaptured = false;
    // Runtime-only correlation id. It is copied into provider sessions and
    // logs, but must never be loaded from or saved to the config file.
    uint64_t asrAttemptId = 0;
    std::wstring mimoApiKey;
    std::wstring mimoBaseUrl = L"https://token-plan-ams.xiaomimimo.com/v1";
    std::wstring mimoModel = L"mimo-v2.5-asr";
    std::wstring mimoLanguage = L"auto";
    std::wstring doubaoImeDeviceId;
    std::wstring doubaoImeCdid;
    std::wstring doubaoImeToken;
    // === QwenFree (千问 IME 免费后端，A1 纯协议还原) ===
    // VoiceInputWrite is one bundled post-processing request.  The punctuate
    // and correct fields remain for config-file compatibility with earlier
    // builds, but are normalized to the same value as qwenFreePolishEnabled.
    bool qwenFreePolishEnabled = false;   // bundled polish / punctuation / correction
    bool qwenFreePunctEnabled = false;    // compatibility mirror
    bool qwenFreeCorrectEnabled = false;  // compatibility mirror
    bool qwenFreeRewriteEnabled = false;  // 选中文本改写（需选区）
    bool qwenFreeDebugLog = false;        // 本地 Qwen 协议诊断日志
    std::wstring qwenFreeShellPath;       // 手动覆盖千问 IME 安装目录
    std::wstring qwenFreeUtdidOverride;   // 调试用 UTDID 覆盖（通常留空）
    bool enableDebugMode = false;
    bool forceUnicodeInput = false;
    std::wstring audioBackend = L"wasapi";
    std::wstring audioDeviceId;
    std::wstring diagnosticAudioMode = L"off";
    // Runtime-only ASR diagnostic routing. These fields identify whether a
    // provider session is the primary request or a configured fallback and
    // must never be persisted in config.json.
    audio_diagnostics::StageKind asrDiagnosticStageKind =
        audio_diagnostics::StageKind::Primary;
    unsigned asrDiagnosticStageIndex = 0;
};

inline void NormalizeQwenFreePostProcessConfig(Config& config) {
    const auto flags = qwen_free_postprocess::Normalize({
        config.qwenFreePolishEnabled,
        config.qwenFreePunctEnabled,
        config.qwenFreeCorrectEnabled,
    });
    config.qwenFreePolishEnabled = flags.polish;
    config.qwenFreePunctEnabled = flags.punctuate;
    config.qwenFreeCorrectEnabled = flags.correct;
}

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
    float widthDip = kHudMinWidthDip;
    float heightDip = kHudMinHeightDip;
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
extern std::atomic<bool> g_enableDebugMode;
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
extern std::atomic<uint64_t> g_audioCaptureGeneration;
extern std::atomic<bool> g_audioCaptureFailurePending;
extern std::atomic<DWORD> g_audioCaptureFailureCode;
extern std::atomic<bool> g_audioCaptureFailureWasapi;
extern std::atomic<float> g_audioLevel;
extern float g_hudSmoothedLevel;
extern bool g_hudHasSpoken;
extern std::atomic<bool> g_vadDetectedVoice;
extern WasapiCapture g_wasapiCapture;
extern std::vector<HWND> g_recognitionControls;
extern std::vector<HWND> g_generalControls;
extern std::vector<HWND> g_llmControls;
extern std::vector<HWND> g_promptControls;
extern std::vector<HWND> g_cloudAsrControls;
extern std::vector<HWND> g_baiduControls;
extern std::vector<HWND> g_volcengineControls;
extern std::vector<HWND> g_qwenControls;
extern std::vector<HWND> g_qwenAudio3Controls;
extern std::vector<HWND> g_qwenAudioStreamingOnlyControls;
extern std::vector<HWND> g_mimoControls;
extern std::vector<HWND> g_doubaoImeControls;
extern std::vector<HWND> g_qwenFreeControls;
extern std::vector<HWND> g_vadFireredControls;
extern std::vector<HWND> g_vadSileroControls;
extern bool g_hudIsRefining;
extern bool g_llmKeyVisible;
extern bool g_baiduKeyVisible;
extern bool g_baiduApiKeyVisible;
extern bool g_volcKeyVisible;
extern bool g_qwenKeyVisible;
extern bool g_mimoKeyVisible;
extern std::unique_ptr<IStreamingAsrSession> g_activeStreamingSession;
extern std::unique_ptr<StreamingVadTrimmer> g_streamingVadTrimmer;
extern CRITICAL_SECTION g_streamingSessionCs;
extern AsrEngine g_asrEngine;
extern int g_cloudProviderIdx;
extern HWND g_cloudAsrHintControl;

void StartRecordingSession();
void StopRecordingSession();
float ExtractJsonFloat(const std::string& json, const std::string& key, float fallback);

extern double g_vadMs;
extern double g_asrDecodeMs;
extern double g_punctMs;
extern double g_cloudApiMs;
extern double g_llmMs;
extern std::wstring g_vadModelName;
extern size_t g_vadTrimmedSamples;
extern std::vector<float> g_streamingVadSamples;
extern std::atomic<bool> g_streamingVadReady;
extern InputContextResult g_inputContextResult;
