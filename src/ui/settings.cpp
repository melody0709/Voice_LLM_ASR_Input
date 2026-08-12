#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "settings.h"
#include "engine.h"
#include "hotkey.h"
#include "hud.h"
#include "startup_registration.h"
#include "doubao_ime_asr.h"
#include "mimo_asr.h"
#include "qwen_asr.h"
#include "qwen_audio_http.h"
#include "qwen_audio_json.h"
#include "qwen_audio_streaming.h"
#include "qwen_free_proto_asr.h"
#include "qwen_free_proto_llm.h"
#include "qwen_free_postprocess.h"
#include "qwen_free_proto_unet.h"
#include "qwen_free_proto_utdid.h"
#include "volcengine_asr.h"

#include <algorithm>
#include <atomic>
#include <commctrl.h>
#include <cstdint>
#include <imm.h>
#include <windowsx.h>
#include <winhttp.h>
#include <shlobj.h>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "imm32.lib")

#ifndef WM_DPICHANGED
#define WM_DPICHANGED 0x02E0
#endif

float UiStyle::Scale = 1.0f;

namespace {
void UpdateUiScale(HWND hwnd) {
    UiStyle::Scale = DpiScaleForWindow(hwnd) * 96.0f / 144.0f;
}
int S(int px) {
    return DipToPx(static_cast<float>(px), UiStyle::Scale);
}

constexpr wchar_t kSettingsHintProperty[] = L"VoxType.SettingsHint";

void MarkSettingsHint(HWND hwnd) {
    if (hwnd) SetPropW(hwnd, kSettingsHintProperty, reinterpret_cast<HANDLE>(1));
}

bool IsSettingsHint(HWND hwnd) {
    return hwnd && GetPropW(hwnd, kSettingsHintProperty) != nullptr;
}

void OpenAsrDebugLog(HWND hwnd, const wchar_t* fileName) {
    if (!fileName || !*fileName) return;
    wchar_t tempPath[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, tempPath);
    if (length == 0 || length >= MAX_PATH) {
        MessageBoxW(hwnd, L"Unable to resolve the temporary log directory.", L"Open log", MB_OK | MB_ICONERROR);
        return;
    }
    std::wstring path(tempPath, length);
    path += fileName;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        MessageBoxW(hwnd, L"Unable to create or open the debug log file.", L"Open log", MB_OK | MB_ICONERROR);
        return;
    }
    CloseHandle(file);
    const HINSTANCE result = ShellExecuteW(hwnd, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(hwnd, L"No application is associated with the debug log file.", L"Open log", MB_OK | MB_ICONERROR);
    }
}

struct QwenLanguageOption {
    const wchar_t* label;
    const wchar_t* code;
};

constexpr QwenLanguageOption kQwenLanguages[] = {
    {L"Auto", L""},
    {L"Chinese (zh)", L"zh"},
    {L"Cantonese (yue)", L"yue"},
    {L"English (en)", L"en"},
    {L"Japanese (ja)", L"ja"},
    {L"German (de)", L"de"},
    {L"Korean (ko)", L"ko"},
    {L"Russian (ru)", L"ru"},
    {L"French (fr)", L"fr"},
    {L"Portuguese (pt)", L"pt"},
    {L"Arabic (ar)", L"ar"},
    {L"Italian (it)", L"it"},
    {L"Spanish (es)", L"es"},
    {L"Hindi (hi)", L"hi"},
    {L"Indonesian (id)", L"id"},
    {L"Thai (th)", L"th"},
    {L"Turkish (tr)", L"tr"},
    {L"Ukrainian (uk)", L"uk"},
    {L"Vietnamese (vi)", L"vi"},
    {L"Czech (cs)", L"cs"},
    {L"Danish (da)", L"da"},
    {L"Filipino (fil)", L"fil"},
    {L"Finnish (fi)", L"fi"},
    {L"Icelandic (is)", L"is"},
    {L"Malay (ms)", L"ms"},
    {L"Norwegian (no)", L"no"},
    {L"Polish (pl)", L"pl"},
    {L"Swedish (sv)", L"sv"},
};

int QwenLanguageIndexFromCode(const std::wstring& code) {
    constexpr int count = static_cast<int>(sizeof(kQwenLanguages) / sizeof(kQwenLanguages[0]));
    for (int i = 0; i < count; ++i) {
        if (code == kQwenLanguages[i].code) return i;
    }
    return 0;
}

const wchar_t* QwenLanguageCodeFromIndex(int index) {
    constexpr int count = static_cast<int>(sizeof(kQwenLanguages) / sizeof(kQwenLanguages[0]));
    if (index >= 0 && index < count) {
        return kQwenLanguages[index].code;
    }
    return L"";
}

bool IsQwenAudioHttpModel(const std::wstring& model) {
    return model == L"qwen-audio-3.0-asr-flash";
}

bool IsQwenAudioStreamingModel(const std::wstring& model) {
    return model == L"qwen-audio-3.0-asr-flash-streaming";
}

std::wstring s_qwenUiModel;
std::wstring s_qwenUiHttpUrl;
std::wstring s_qwenUiAudioStreamingUrl;
std::wstring s_qwenUiLegacyUrl;
HWND s_qwenChunkContextHint = nullptr;

struct QwenAdvancedDialogData {
    bool streaming = false;
    bool ok = false;
    std::wstring vocabularyId;
    std::wstring vocabulary;
    bool semanticPunctuation = false;
    std::wstring maxSentenceSilence;
    bool multiThreshold = false;
    bool heartbeat = false;
    bool speechNoiseEnabled = false;
    std::wstring speechNoiseThreshold;
};

std::wstring QwenModelFromControl(HWND hwnd) {
    wchar_t model[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MODEL), model, 256);
    return model;
}

std::wstring QwenControlText(HWND hwnd, int id, size_t capacity = 1024) {
    std::wstring value(capacity, L'\0');
    const int length = GetWindowTextW(GetDlgItem(hwnd, id), value.data(), static_cast<int>(value.size()));
    if (length <= 0) return {};
    value.resize(static_cast<size_t>(length));
    return value;
}

void StoreQwenProfileUrl(HWND hwnd, const std::wstring& model) {
    const std::wstring url = QwenControlText(hwnd, IDC_QWEN_BASE_URL, 2048);
    if (url.empty()) return;
    if (IsQwenAudioHttpModel(model)) s_qwenUiHttpUrl = url;
    else if (IsQwenAudioStreamingModel(model)) s_qwenUiAudioStreamingUrl = url;
    else s_qwenUiLegacyUrl = url;
}

std::wstring NormalizeQwenLanguageHints(const std::wstring& raw) {
    std::wstring normalized;
    size_t start = 0;
    size_t count = 0;
    while (start <= raw.size() && count < 4) {
        const size_t end = raw.find_first_of(L",;", start);
        std::wstring item = Trim(raw.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (!item.empty()) {
            if (item == L"fil") item = L"tl";
            bool valid = item.size() <= 16;
            for (wchar_t ch : item) valid = valid && ((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-');
            if (valid) {
                if (!normalized.empty()) normalized += L',';
                normalized += item;
                ++count;
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return normalized;
}

bool ValidateQwenHints(const std::wstring& raw, std::wstring& error) {
    const std::wstring text = Trim(raw);
    if (text.empty()) return true;
    size_t start = 0;
    size_t count = 0;
    while (start <= text.size()) {
        const size_t end = text.find_first_of(L",;", start);
        std::wstring item = Trim(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        if (item.empty()) {
            error = L"Language hints must contain 1–4 non-empty language codes.";
            return false;
        }
        if (item == L"fil") item = L"tl";
        if (item.size() > 16) {
            error = L"Each language hint must be at most 16 characters.";
            return false;
        }
        for (wchar_t ch : item) {
            if (!((ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') || ch == L'-')) {
                error = L"Language hints may contain only letters and hyphens.";
                return false;
            }
        }
        if (++count > 4) {
            error = L"Audio 3 supports at most 4 language hints.";
            return false;
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return true;
}

bool ValidateQwenEndpoint(const std::wstring& raw,
                          const std::wstring& scheme,
                          const std::wstring& path,
                          std::wstring& error) {
    URL_COMPONENTSW parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    const std::wstring original = Trim(raw);
    if (original.empty() || original.rfind(scheme + L"://", 0) != 0) {
        error = L"Qwen Base URL is invalid.";
        return false;
    }
    std::wstring url = original;
    const std::wstring parsedScheme = (scheme == L"wss") ? L"https" :
        (scheme == L"ws" ? L"http" : scheme);
    if (scheme == L"wss" || scheme == L"ws") url.replace(0, scheme.size(), parsedScheme);
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &parts)) {
        error = L"Qwen Base URL is invalid.";
        return false;
    }
    const std::wstring actualScheme(parts.lpszScheme, parts.dwSchemeLength);
    if (actualScheme != parsedScheme || parts.dwHostNameLength == 0) {
        error = L"Qwen Base URL must use " + scheme + L" and include a host.";
        return false;
    }
    std::wstring actualPath(parts.lpszUrlPath, parts.dwUrlPathLength);
    while (actualPath.size() > 1 && actualPath.back() == L'/') actualPath.pop_back();
    if (actualPath != path) {
        error = L"Qwen Base URL path must be " + path + L".";
        return false;
    }
    return true;
}

bool ValidateQwenControls(HWND hwnd, std::wstring& error) {
    if (ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CLOUD_PROVIDER)) != 2) return true;

    if (QwenControlText(hwnd, IDC_QWEN_API_KEY, 1024).empty()) {
        error = L"Qwen API Key cannot be empty when Qwen ASR is selected.";
        return false;
    }
    const std::wstring model = QwenModelFromControl(hwnd);
    if (!IsQwenAudioHttpModel(model) && !IsQwenAudioStreamingModel(model) &&
        model != L"qwen3-asr-flash-realtime") {
        error = L"Select a supported Qwen ASR model.";
        return false;
    }

    const std::wstring url = QwenControlText(hwnd, IDC_QWEN_BASE_URL, 2048);
    if (IsQwenAudioHttpModel(model)) {
        if (!ValidateQwenEndpoint(url, L"https",
                L"/api/v1/services/aigc/multimodal-generation/generation", error)) return false;
    } else if (IsQwenAudioStreamingModel(model)) {
        if (!ValidateQwenEndpoint(url, L"wss", L"/api-ws/v1/inference", error)) return false;
    } else {
        std::wstring wssError;
        std::wstring wsError;
        if (!ValidateQwenEndpoint(url, L"wss", L"/api-ws/v1/realtime", wssError) &&
            !ValidateQwenEndpoint(url, L"ws", L"/api-ws/v1/realtime", wsError)) {
            error = wssError.empty() ? wsError : wssError;
            return false;
        }
    }

    const std::wstring hints = QwenControlText(hwnd, IDC_QWEN_LANGUAGE_HINTS, 1024);
    if (!ValidateQwenHints(hints, error)) return false;
    const std::wstring vocabulary = QwenControlText(hwnd, IDC_QWEN_VOCABULARY, 8192);
    if (!qwen_audio_json::IsValidVocabulary(vocabulary, &error)) return false;

    if (IsQwenAudioStreamingModel(model)) {
        const std::wstring silence = QwenControlText(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE, 32);
        const int silenceMs = _wtoi(silence.c_str());
        if (silenceMs < 200 || silenceMs > 6000) {
            error = L"Max sentence silence must be between 200 and 6000 ms.";
            return false;
        }
        const bool semantic = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
        const bool multi = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED;
        if (semantic && multi) {
            error = L"Semantic punctuation and multi-threshold mode cannot both be enabled.";
            return false;
        }
        if (Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED) {
            const std::wstring noise = QwenControlText(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD, 32);
            const double value = _wtof(noise.c_str());
            if (value < -1.0 || value > 1.0) {
                error = L"Speech noise threshold must be between -1.0 and 1.0.";
                return false;
            }
        }
    }
    return true;
}

void ApplyQwenModelProfile(HWND hwnd, const std::wstring& model, bool preserveUrl) {
    const bool http = IsQwenAudioHttpModel(model);
    const bool isStreamingTransport = !http;
    HWND base = GetDlgItem(hwnd, IDC_QWEN_BASE_URL);
    if (base && !preserveUrl) {
        const std::wstring& value = http ? s_qwenUiHttpUrl
            : (IsQwenAudioStreamingModel(model) ? s_qwenUiAudioStreamingUrl : s_qwenUiLegacyUrl);
        SetWindowTextW(base, value.c_str());
    }
    HWND chunk = GetDlgItem(hwnd, IDC_QWEN_CHUNK_MS);
    if (chunk) EnableWindow(chunk, isStreamingTransport ? TRUE : FALSE);
    if (s_qwenChunkContextHint) {
        const wchar_t* hint = http
            ? L"HTTP batch ignores Chunk ms. Context sends the focused field's text (≤400 chars) to the cloud."
            : (IsQwenAudioStreamingModel(model)
                ? L"100–300 ms recommended. Context sends the focused field's text (≤400 chars) to the cloud."
                : L"Realtime upload latency; 100–300 ms recommended.");
        SetWindowTextW(s_qwenChunkContextHint, hint);
    }
    const bool audio3 = http || IsQwenAudioStreamingModel(model);
    for (HWND control : g_qwenAudio3Controls) {
        ShowWindow(control, audio3 && g_cloudProviderIdx == 2 ? SW_SHOW : SW_HIDE);
    }
    const bool audioStreaming = audio3 && IsQwenAudioStreamingModel(model);
    for (HWND control : g_qwenAudioStreamingOnlyControls) {
        ShowWindow(control, audioStreaming && g_cloudProviderIdx == 2 ? SW_SHOW : SW_HIDE);
    }
    if (audio3) {
        EnableWindow(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD),
                     Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED ? FALSE : TRUE);
        EnableWindow(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT),
                     IsQwenAudioStreamingModel(model) ? TRUE : FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE),
                     IsQwenAudioStreamingModel(model) ? TRUE : FALSE);
        EnableWindow(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD),
                     IsQwenAudioStreamingModel(model) &&
                     Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED ? TRUE : FALSE);
    }
}

struct MimoLanguageOption {
    const wchar_t* label;
    const wchar_t* code;
};

constexpr MimoLanguageOption kMimoLanguages[] = {
    {L"Auto", L"auto"},
    {L"Chinese (zh)", L"zh"},
    {L"English (en)", L"en"},
};

int MimoLanguageIndexFromCode(const std::wstring& code) {
    constexpr int count = static_cast<int>(sizeof(kMimoLanguages) / sizeof(kMimoLanguages[0]));
    for (int i = 0; i < count; ++i) {
        if (code == kMimoLanguages[i].code) return i;
    }
    return 0;
}

const wchar_t* MimoLanguageCodeFromIndex(int index) {
    constexpr int count = static_cast<int>(sizeof(kMimoLanguages) / sizeof(kMimoLanguages[0]));
    if (index >= 0 && index < count) {
        return kMimoLanguages[index].code;
    }
    return mimo_asr::kDefaultLanguage;
}

struct DoubaoImeTestMessage {
    doubao_ime_asr::TestResult result;
    uint64_t generation = 0;
};

std::atomic<uint64_t> g_doubaoImeTestGeneration{0};
constexpr UINT kDoubaoImeTestResultMessage = WM_APP + 11;

struct QwenFreeTestMessage {
    qwen_free_proto_asr::TestResult result;
    uint64_t generation = 0;
    bool asrOk = false;
    bool llmOk = false;
    std::wstring llmMessage;
    DWORD llmElapsedMs = 0;
};

std::atomic<uint64_t> g_qwenFreeTestGeneration{0};
constexpr UINT kQwenFreeTestResultMessage = WM_APP + 12;

struct QwenFreeStatusMessage {
    uint64_t generation = 0;
    std::wstring text;
};

std::atomic<uint64_t> g_qwenFreeStatusGeneration{0};
constexpr UINT kQwenFreeStatusResultMessage = WM_APP + 13;

struct BackendOption {
    const wchar_t* id;
    const wchar_t* label;
    bool primarySupported;
    bool fallbackSupported;
};

constexpr BackendOption kBackendOptions[] = {
    {L"local", L"Local (sherpa-onnx)", true, true},
    {L"volcengine", L"Volcano Engine", true, false},
    {L"baidu", L"Baidu Cloud", true, true},
    {L"qwen", L"Qwen ASR", true, true},
    {L"mimo", L"MiMo ASR", true, true},
    {L"doubao_ime", L"Doubao IME (Free)", true, true},
    {L"qwen_free", L"Qwen IME (Free)", true, true},
};

constexpr int kBackendOptionCount = static_cast<int>(sizeof(kBackendOptions) / sizeof(kBackendOptions[0]));
constexpr DWORD_PTR kDisabledBackendItem = static_cast<DWORD_PTR>(-1);

bool BackendSupported(const BackendOption& option, bool fallback) {
    return fallback ? option.fallbackSupported : option.primarySupported;
}

void PopulateBackendCombo(HWND combo, const std::wstring& selectedBackend, bool fallback) {
    if (!combo) return;
    ComboBox_ResetContent(combo);

    int selectedIndex = -1;
    if (fallback) {
        int item = ComboBox_AddString(combo, L"Disabled");
        ComboBox_SetItemData(combo, item, kDisabledBackendItem);
        if (selectedBackend.empty() || selectedBackend == L"none") {
            selectedIndex = item;
        }
    }

    for (int i = 0; i < kBackendOptionCount; ++i) {
        const BackendOption& option = kBackendOptions[i];
        if (!BackendSupported(option, fallback)) continue;
        int item = ComboBox_AddString(combo, option.label);
        ComboBox_SetItemData(combo, item, static_cast<DWORD_PTR>(i));
        if (selectedBackend == option.id) {
            selectedIndex = item;
        }
    }

    if (selectedIndex < 0) selectedIndex = 0;
    ComboBox_SetCurSel(combo, selectedIndex);
}

std::wstring BackendIdFromCombo(HWND combo, bool fallback) {
    if (!combo) return fallback ? L"none" : L"local";
    const int sel = ComboBox_GetCurSel(combo);
    if (sel < 0) return fallback ? L"none" : L"local";
    const DWORD_PTR data = ComboBox_GetItemData(combo, sel);
    if (fallback && data == kDisabledBackendItem) return L"none";
    if (data < static_cast<DWORD_PTR>(kBackendOptionCount)) {
        const BackendOption& option = kBackendOptions[static_cast<int>(data)];
        if (BackendSupported(option, fallback)) return option.id;
    }
    return fallback ? L"none" : L"local";
}
}

void SetStatus(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS), text.c_str());
}

std::wstring DescribeWin32Error(DWORD error) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result = length && message ? std::wstring(message, length) : L"Unknown Windows error";
    if (message) LocalFree(message);
    while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) result.pop_back();
    return result;
}

void RefreshStartupRegistrationControl(HWND hwnd, bool reportError) {
    const StartupRegistrationState startup = QueryVoxTypeStartupRegistration();
    HWND checkbox = GetDlgItem(hwnd, IDC_START_WITH_WINDOWS);
    if (!checkbox) return;

    if (!startup.Succeeded()) {
        Button_SetCheck(checkbox, BST_UNCHECKED);
        if (reportError) {
            SetStatus(hwnd, L"Could not read Windows startup setting (" +
                std::to_wstring(startup.error) + L"): " + DescribeWin32Error(startup.error));
        }
        return;
    }

    Button_SetCheck(checkbox, startup.registered ? BST_CHECKED : BST_UNCHECKED);
    if (reportError && startup.registered && !startup.pointsToCurrentExecutable) {
        SetStatus(hwnd, L"Startup is enabled for a moved copy. Save will update it to this VoxType folder.");
    }
}

bool SaveStartupRegistrationControl(HWND hwnd) {
    const bool enable = Button_GetCheck(GetDlgItem(hwnd, IDC_START_WITH_WINDOWS)) == BST_CHECKED;
    const DWORD error = SetVoxTypeStartupRegistration(enable);
    if (error == ERROR_SUCCESS) return true;

    const std::wstring detail = L"Windows startup setting was not changed (" +
        std::to_wstring(error) + L"): " + DescribeWin32Error(error);
    SetStatus(hwnd, detail);
    MessageBoxW(hwnd, detail.c_str(), L"VoxType Settings", MB_OK | MB_ICONERROR);
    return false;
}

std::wstring DoubaoImeCredentialStatusText() {
    if (g_config.doubaoImeDeviceId.empty()) {
        return L"Not registered. First use or Test Connection will register automatically.";
    }
    std::wstring id = g_config.doubaoImeDeviceId;
    if (id.size() > 22) {
        id = id.substr(0, 10) + L"..." + id.substr(id.size() - 8);
    }
    return L"Registered device: " + id;
}

void RefreshDoubaoImeStatus(HWND hwnd) {
    HWND status = GetDlgItem(hwnd, IDC_DOUBAO_IME_STATUS);
    if (status) {
        SetWindowTextW(status, DoubaoImeCredentialStatusText().c_str());
    }
}

std::wstring QwenFreeStatusText(const std::wstring& utdidOverride,
                                const std::wstring& shellPath) {
    // A1 协议还原：检测千问 IME 安装目录 + 尝试获取 UTDID。
    // UTDID 是 ASR/LLM 鉴权的关键设备指纹，无法本地生成。
    // 状态栏只显示摘要，避免把完整设备指纹暴露在截图或用户反馈中。
    const auto maskUtdid = [](const std::string& utdid) {
        if (utdid.size() <= 8) return std::wstring(L"(redacted)");
        return std::wstring(utdid.begin(), utdid.begin() + 4) + L"..." +
               std::wstring(utdid.end() - 4, utdid.end());
    };
    auto r = qwen_free_proto_utdid::GetUtdid(utdidOverride, shellPath);
    if (!r.ok) {
        return L"UTDID unavailable: " + r.error;
    }
    return L"UTDID OK (" + r.source + L"): " + maskUtdid(r.utdid);
}

void RefreshQwenFreeStatus(HWND hwnd) {
    HWND status = GetDlgItem(hwnd, IDC_QWEN_FREE_STATUS);
    if (!status) return;

    SetWindowTextW(status, L"Checking Qwen device identity...");
    const uint64_t generation =
        g_qwenFreeStatusGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
    const std::wstring utdidOverride = g_config.qwenFreeUtdidOverride;
    const std::wstring shellPath = g_config.qwenFreeShellPath;
    std::thread([hwnd, generation, utdidOverride, shellPath]() {
        auto* message = new QwenFreeStatusMessage;
        message->generation = generation;
        message->text = QwenFreeStatusText(utdidOverride, shellPath);
        if (!PostMessageW(hwnd, kQwenFreeStatusResultMessage, 0,
                          reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    }).detach();
}

void SetClipboardText(const std::wstring& text) {
    if (text.empty() || !OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (mem) {
        void* ptr = GlobalLock(mem);
        if (ptr) {
            memcpy(ptr, text.c_str(), bytes);
            GlobalUnlock(mem);
            SetClipboardData(CF_UNICODETEXT, mem);
            mem = nullptr;
        }
        if (mem) GlobalFree(mem);
    }
    CloseClipboard();
}

void SendCtrlV() {
    INPUT inputs[4] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = 'V';
    inputs[2].type = INPUT_KEYBOARD;
    inputs[2].ki.wVk = 'V';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].type = INPUT_KEYBOARD;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

void SendUnicodeText(const std::wstring& text) {
    if (text.empty()) return;
    for (wchar_t ch : text) {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = ch;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = ch;
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        Sleep(1);
    }
}

struct ImeStateGuard {
    HWND  targetWnd = nullptr;
    HIMC  hIMC      = nullptr;
    DWORD savedConv = 0;
    DWORD savedSent = 0;
    BOOL  savedOpen = FALSE;
    bool  active    = false;

    bool Disable() {
        targetWnd = GetForegroundWindow();
        if (!targetWnd) return false;

        HWND focused = GetFocus();
        if (focused && IsChild(targetWnd, focused)) {
            targetWnd = focused;
        }

        hIMC = ImmGetContext(targetWnd);
        if (!hIMC) return false;

        ImmGetConversionStatus(hIMC, &savedConv, &savedSent);
        savedOpen = ImmGetOpenStatus(hIMC);

        if (savedConv == IME_CMODE_ALPHANUMERIC && !savedOpen) {
            ImmReleaseContext(targetWnd, hIMC);
            hIMC = nullptr;
            return false;
        }

        ImmSetOpenStatus(hIMC, FALSE);
        ImmSetConversionStatus(hIMC, IME_CMODE_ALPHANUMERIC, 0);
        Sleep(15);
        active = true;
        return true;
    }

    void Restore() {
        if (!active || !hIMC) return;
        ImmSetConversionStatus(hIMC, savedConv, savedSent);
        ImmSetOpenStatus(hIMC, savedOpen);
        if (targetWnd) ImmReleaseContext(targetWnd, hIMC);
        hIMC = nullptr;
        active = false;
    }

    ~ImeStateGuard() { Restore(); }
};

void PasteTextImeAware(const std::wstring& text) {
    if (text.empty()) return;

    // 强制 Unicode 输入模式（测试用）
    if (g_config.forceUnicodeInput) {
        SendUnicodeText(text);
        return;
    }

    HWND focus = GetFocus();
    if (focus) {
        SetClipboardText(text);
        DWORD_PTR result = 0;
        SendMessageTimeoutW(focus, WM_PASTE, 0, 0, SMTO_ABORTIFHUNG, 2000, &result);
        return;
    }

    HWND fg = GetForegroundWindow();
    if (!fg) return;

    // 通过进程名判断是否是微信
    DWORD pid = 0;
    GetWindowThreadProcessId(fg, &pid);
    wchar_t processName[MAX_PATH] = {};
    HANDLE hProc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hProc) {
        DWORD size = MAX_PATH;
        QueryFullProcessImageNameW(hProc, 0, processName, &size);
        CloseHandle(hProc);
    }

    bool isWeChat = wcsstr(processName, L"WeChat") || wcsstr(processName, L"wechat") ||
                    wcsstr(processName, L"Weixin") || wcsstr(processName, L"weixin");
    if (!isWeChat) {
        SetClipboardText(text);
        ImeStateGuard guard;
        guard.Disable();
        SendCtrlV();
        return;
    }

    // 微信：用 WM_CHAR 绕过 IME
    for (wchar_t ch : text) {
        PostMessageW(fg, WM_CHAR, ch, 0);
        Sleep(1);
    }
}

bool ReplaceSelectionTextImeAware(const SelectionContext& selection,
                                  const std::wstring& text,
                                  std::wstring* error) {
    if (error) error->clear();
    if (text.empty()) {
        if (error) *error = L"empty replacement";
        return false;
    }
    if (!selection.Usable()) {
        if (error) *error = L"selection target unavailable";
        return false;
    }
    if (!selection_context::IsSameForegroundWindow(selection)) {
        if (error) *error = L"selection target is no longer foreground";
        return false;
    }
    if (!selection_context::VerifyCurrentSelection(selection)) {
        if (error) *error = L"selection changed while recognizing";
        return false;
    }

    if (!IsWindow(selection.focusWindow)) {
        if (error) *error = L"selection focus control unavailable";
        return false;
    }
    HWND focus = selection.focusWindow;

    DWORD currentProcessId = 0;
    GetWindowThreadProcessId(selection.targetWindow, &currentProcessId);
    if (selection.processId != 0 && currentProcessId != selection.processId) {
        if (error) *error = L"selection target process changed";
        return false;
    }
    if (selection_context::TopLevelWindow(focus) != selection.targetWindow) {
        if (error) *error = L"selection focus control changed window";
        return false;
    }

    // A user click during recognition must not redirect a rewrite to another
    // editor in the same application window.
    GUITHREADINFO guiInfo = {};
    guiInfo.cbSize = sizeof(guiInfo);
    const DWORD targetThread = GetWindowThreadProcessId(selection.targetWindow, nullptr);
    if (GetGUIThreadInfo(targetThread, &guiInfo) &&
        selection.focusWindow && guiInfo.hwndFocus &&
        guiInfo.hwndFocus != selection.focusWindow) {
        if (error) *error = L"focus changed while recognizing";
        return false;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(selection.targetWindow, &pid);
    wchar_t processName[MAX_PATH] = {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process) {
        DWORD size = MAX_PATH;
        QueryFullProcessImageNameW(process, 0, processName, &size);
        CloseHandle(process);
    }
    const bool isWeChat = wcsstr(processName, L"WeChat") ||
                          wcsstr(processName, L"wechat") ||
                          wcsstr(processName, L"Weixin") ||
                          wcsstr(processName, L"weixin");
    if (isWeChat) {
        // WeChat's Qt editor can reject clipboard paste and Ctrl+V.  WM_CHAR
        // replaces the current selection with the first character and appends
        // the remaining characters without touching the clipboard.
        for (wchar_t ch : text) {
            if (!PostMessageW(focus, WM_CHAR, ch, 0)) {
                if (error) *error = L"WeChat WM_CHAR failed";
                return false;
            }
            Sleep(1);
        }
        return true;
    }

    selection_context::ClipboardSnapshot previousClipboard;
    if (!previousClipboard.Readable()) {
        if (error) *error = L"clipboard snapshot unavailable";
        return false;
    }
    if (!selection_context::SetClipboardText(text)) {
        if (error) *error = L"clipboard unavailable";
        return false;
    }

    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(focus, WM_PASTE, 0, 0,
                            SMTO_ABORTIFHUNG | SMTO_BLOCK,
                            2000, &result)) {
        return true;
    }

    // Some controls do not implement WM_PASTE but accept simulated Ctrl+V.
    // Allow the target queue to consume Ctrl+V before the RAII snapshot
    // restores every original clipboard format.
    ImeStateGuard guard;
    guard.Disable();
    SendCtrlV();
    Sleep(100);
    return true;
}

bool IsCapsLockOn() {
    return (GetKeyState(VK_CAPITAL) & 0x0001) != 0;
}

void SendCapsLockTap() {
    INPUT inputs[2] = {};
    inputs[0].type = INPUT_KEYBOARD;
    inputs[0].ki.wVk = VK_CAPITAL;
    inputs[1].type = INPUT_KEYBOARD;
    inputs[1].ki.wVk = VK_CAPITAL;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, inputs, sizeof(INPUT));
}

void RestoreCapsLockState() {
    if (IsCapsLockOn() != g_capsLockWasOn) {
        SendCapsLockTap();
    }
}

void AddRecognitionControl(HWND hwnd) {
    if (hwnd) g_recognitionControls.push_back(hwnd);
}

void AddGeneralControl(HWND hwnd) {
    if (hwnd) g_generalControls.push_back(hwnd);
}

void AddLlmControl(HWND hwnd) {
    if (hwnd) g_llmControls.push_back(hwnd);
}

void AddPromptControl(HWND hwnd) {
    if (hwnd) g_promptControls.push_back(hwnd);
}

void AddCloudAsrControl(HWND hwnd) {
    if (hwnd) g_cloudAsrControls.push_back(hwnd);
}

void AddBaiduControl(HWND hwnd) {
    if (hwnd) g_baiduControls.push_back(hwnd);
}

void AddVolcengineControl(HWND hwnd) {
    if (hwnd) g_volcengineControls.push_back(hwnd);
}

void AddQwenControl(HWND hwnd) {
    if (hwnd) g_qwenControls.push_back(hwnd);
}

void AddQwenAudio3Control(HWND hwnd) {
    if (hwnd) {
        g_qwenControls.push_back(hwnd);
        g_qwenAudio3Controls.push_back(hwnd);
    }
}

void AddQwenAudioStreamingOnlyControl(HWND hwnd) {
    AddQwenAudio3Control(hwnd);
    if (hwnd) g_qwenAudioStreamingOnlyControls.push_back(hwnd);
}

void AddMimoControl(HWND hwnd) {
    if (hwnd) g_mimoControls.push_back(hwnd);
}

void AddDoubaoImeControl(HWND hwnd) {
    if (hwnd) g_doubaoImeControls.push_back(hwnd);
}

void AddQwenFreeControl(HWND hwnd) {
    if (hwnd) g_qwenFreeControls.push_back(hwnd);
}

void AddVadFireredControl(HWND hwnd) {
    if (hwnd) g_vadFireredControls.push_back(hwnd);
}

void AddVadSileroControl(HWND hwnd) {
    if (hwnd) g_vadSileroControls.push_back(hwnd);
}

void ShowVadSubGroup(int vadModelIdx) {
    for (HWND c : g_vadFireredControls) ShowWindow(c, vadModelIdx == 1 ? SW_SHOW : SW_HIDE);
    for (HWND c : g_vadSileroControls) ShowWindow(c, vadModelIdx == 0 ? SW_SHOW : SW_HIDE);
}

void ShowCloudSubPage(HWND hwnd, int providerIdx) {
    g_cloudProviderIdx = providerIdx;
    for (HWND c : g_baiduControls) ShowWindow(c, providerIdx == 1 ? SW_SHOW : SW_HIDE);
    for (HWND c : g_volcengineControls) ShowWindow(c, providerIdx == 0 ? SW_SHOW : SW_HIDE);
    for (HWND c : g_qwenControls) ShowWindow(c, providerIdx == 2 ? SW_SHOW : SW_HIDE);
    const bool audio3 = providerIdx == 2 &&
        (IsQwenAudioHttpModel(QwenModelFromControl(hwnd)) ||
         IsQwenAudioStreamingModel(QwenModelFromControl(hwnd)));
    for (HWND c : g_qwenAudio3Controls) ShowWindow(c, audio3 ? SW_SHOW : SW_HIDE);
    const bool audioStreaming = audio3 && IsQwenAudioStreamingModel(QwenModelFromControl(hwnd));
    for (HWND c : g_qwenAudioStreamingOnlyControls) ShowWindow(c, audioStreaming ? SW_SHOW : SW_HIDE);
    for (HWND c : g_mimoControls) ShowWindow(c, providerIdx == 3 ? SW_SHOW : SW_HIDE);
    for (HWND c : g_doubaoImeControls) ShowWindow(c, providerIdx == 4 ? SW_SHOW : SW_HIDE);
    for (HWND c : g_qwenFreeControls) ShowWindow(c, providerIdx == 5 ? SW_SHOW : SW_HIDE);
}

void ShowSettingsPage(HWND hwnd, int page) {
    for (HWND control : g_generalControls) {
        ShowWindow(control, page == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_recognitionControls) {
        ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_cloudAsrControls) {
        ShowWindow(control, page == 2 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_llmControls) {
        ShowWindow(control, page == 3 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_promptControls) {
        ShowWindow(control, page == 4 ? SW_SHOW : SW_HIDE);
    }
    if (page == 2) {
        ShowCloudSubPage(hwnd, g_cloudProviderIdx);
    } else {
        for (HWND c : g_baiduControls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_volcengineControls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_qwenControls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_qwenAudio3Controls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_mimoControls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_doubaoImeControls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_qwenFreeControls) ShowWindow(c, SW_HIDE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void LayoutSettingsWindow(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int margin = S(UiStyle::Margin);
    const int footerHeight = S(UiStyle::FooterHeight);
    const int footerTop = (rc.bottom - footerHeight > S(UiStyle::FooterMinTop)) ? rc.bottom - footerHeight : S(UiStyle::FooterMinTop);
    const int tabBottom = footerTop - S(4);
    HWND tab = GetDlgItem(hwnd, IDC_SETTINGS_TAB);
    if (tab) {
        MoveWindow(tab, margin, S(12), rc.right - margin * 2, tabBottom - S(12), TRUE);
    }
    const int availableFooterH = rc.bottom - footerTop;
    const int btnY = footerTop + (availableFooterH - S(UiStyle::ActionBtnH)) / 2;
    HWND status = GetDlgItem(hwnd, IDC_STATUS);
    if (status) {
        MoveWindow(status, margin, btnY + S(4), rc.right - margin * 2 - S(300), S(28), TRUE);
    }
    HWND save = GetDlgItem(hwnd, IDC_SAVE);
    HWND close = GetDlgItem(hwnd, IDC_CANCEL);
    if (save) MoveWindow(save, rc.right - margin - S(192), btnY, S(UiStyle::FooterBtnW), S(UiStyle::ActionBtnH), TRUE);
    if (close) MoveWindow(close, rc.right - margin - S(UiStyle::FooterBtnW), btnY, S(UiStyle::FooterBtnW), S(UiStyle::ActionBtnH), TRUE);
    if (g_cloudAsrHintControl) {
        SetWindowPos(g_cloudAsrHintControl, nullptr,
                     S(UiStyle::ContentLeft), S(UiStyle::CloudAsrHintY),
                     S(UiStyle::CloudAsrHintW), S(UiStyle::LabelH),
                     SWP_NOZORDER);
    }
}

void HideSettingsWindow(HWND hwnd) {
    // Detached status/test workers may finish after the Settings page is
    // hidden or reopened. Invalidate both generations before changing the
    // visible window state so stale results cannot re-enable controls or
    // overwrite a newer page.
    g_qwenFreeTestGeneration.fetch_add(1, std::memory_order_relaxed);
    g_qwenFreeStatusGeneration.fetch_add(1, std::memory_order_relaxed);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_FREE_TEST), TRUE);
    ShowWindow(hwnd, SW_HIDE);
    InstallKeyboardHook();
}

HWND CreateLabel(HWND parent, int x, int y, int w, int h, const wchar_t* text) {
    HWND hwnd = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, g_instance, nullptr);
    ApplyUiFont(hwnd);
    return hwnd;
}

HWND CreateHint(HWND parent, int x, int y, int w, int h, const wchar_t* text) {
    HWND hwnd = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                              x, y, w, h, parent, nullptr, g_instance, nullptr);
    ApplyUiFont(hwnd);
    MarkSettingsHint(hwnd);
    return hwnd;
}

HWND CreateCombo(HWND parent, int id, int x, int y, int w, int h) {
    HWND hwnd = CreateWindowW(L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyUiFont(hwnd);
    return hwnd;
}

HWND CreateButton(HWND parent, int id, int x, int y, int w, int h, const wchar_t* text) {
    HWND hwnd = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyUiFont(hwnd);
    return hwnd;
}

HWND CreateCheckBox(HWND parent, int id, int x, int y, int w, int h, const wchar_t* text) {
    HWND hwnd = CreateWindowW(L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    ApplyUiFont(hwnd);
    return hwnd;
}

void BrowseModelDirectory(HWND hwnd) {
    BROWSEINFOW bi = {};
    bi.hwndOwner = hwnd;
    bi.lpszTitle = L"Select model directory";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH] = {};
    if (SHGetPathFromIDListW(pidl, path)) {
        SetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), path);
    }
    CoTaskMemFree(pidl);
}

static std::wstring s_customPromptBackup;
static bool s_isCustomMode = false;

void LoadSettingsControls(HWND hwnd) {
    if (g_config.modelDir.empty()) {
        g_config.modelDir = DefaultModelDir(g_config.modelId);
    }

    HWND model = GetDlgItem(hwnd, IDC_MODEL);
    ComboBox_AddString(model, L"FireRedASR2 CTC");
    ComboBox_AddString(model, L"FireRedASR2 AED");
    ComboBox_AddString(model, L"SenseVoiceSmall");
    ComboBox_SetCurSel(model, ModelIndex(g_config.modelId));

    SetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), g_config.modelDir.c_str());

    HWND threads = GetDlgItem(hwnd, IDC_THREADS);
    {
        int physical = std::thread::hardware_concurrency();
        if (physical < 1) physical = 4;
        int auto_threads = (std::min)(8, physical);
        std::wstring auto_label = L"auto (" + std::to_wstring(auto_threads) + L")";
        ComboBox_AddString(threads, auto_label.c_str());
        for (int i = 1; i <= 8; i++)
            ComboBox_AddString(threads, std::to_wstring(i).c_str());
        int threadIndex = 0;
        if (g_config.threads == L"auto") threadIndex = 0;
        else {
            int val = _wtoi(g_config.threads.c_str());
            if (val >= 1 && val <= 8) threadIndex = val;
        }
        ComboBox_SetCurSel(threads, threadIndex);
    }

    Button_SetCheck(GetDlgItem(hwnd, IDC_VAD), g_config.enableVad ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_PARTIAL), g_config.enablePartial ? BST_CHECKED : BST_UNCHECKED);

    HWND post = GetDlgItem(hwnd, IDC_POSTPROCESS);
    ComboBox_AddString(post, L"Disabled");
    ComboBox_AddString(post, L"Auto punctuate");
    ComboBox_AddString(post, L"Auto punctuate + LLM");
    int postIndex = 1;
    if (g_config.postprocess == L"none") postIndex = 0;
    else if (g_config.postprocess == L"llm") postIndex = 2;
    ComboBox_SetCurSel(post, postIndex);

    HWND vadModelCombo = GetDlgItem(hwnd, IDC_VAD_MODEL);
    ComboBox_AddString(vadModelCombo, L"Silero VAD");
    ComboBox_AddString(vadModelCombo, L"FireRed VAD");
    int vadModelIndex = 0;
    if (g_config.vadModel == L"firered") vadModelIndex = 1;
    ComboBox_SetCurSel(vadModelCombo, vadModelIndex);

    {
        wchar_t buf[32];
        swprintf(buf, 32, L"%.2f", g_config.vadThreshold);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VAD_THRESHOLD), buf);
    }
    {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", g_config.vadMinSilence);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VAD_MIN_SILENCE), buf);
    }
    {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", g_config.vadMinSpeech);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VAD_MIN_SPEECH), buf);
    }
    {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", g_config.vadPadStart);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VAD_PAD_START), buf);
    }
    {
        wchar_t buf[32];
        swprintf(buf, 32, L"%d", g_config.vadSmoothWindow);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VAD_SMOOTH_WINDOW), buf);
    }
    ShowVadSubGroup(vadModelIndex);

    HWND hotkeyEdit = GetDlgItem(hwnd, IDC_HOTKEY);
    auto* hotkeyState = hotkeyEdit ? reinterpret_cast<HotkeyEditState*>(GetWindowLongPtrW(hotkeyEdit, GWLP_USERDATA)) : nullptr;
    if (hotkeyState) {
        hotkeyState->hotkey = CurrentConfiguredHotkey();
        hotkeyState->original = hotkeyState->hotkey;
        InvalidateRect(hotkeyEdit, nullptr, TRUE);
    }

    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_ENDPOINT), g_config.llmEndpoint.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_KEY), g_config.llmApiKey.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_MODEL), g_config.llmModel.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PROMPT), g_config.llmPrompt.c_str());
    {
        HWND presetCombo = GetDlgItem(hwnd, IDC_LLM_PRESET_COMBO);
        for (int i = 0; i < llm::kPromptPresetCount; ++i) {
            ComboBox_AddString(presetCombo, llm::kPromptPresets[i].name);
        }
        ComboBox_AddString(presetCombo, L"Custom");
        int matchedPreset = -1;
        for (int i = 0; i < llm::kPromptPresetCount; ++i) {
            if (g_config.llmPrompt == llm::kPromptPresets[i].prompt) {
                matchedPreset = i;
                break;
            }
        }
        if (matchedPreset >= 0) {
            ComboBox_SetCurSel(presetCombo, matchedPreset);
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PRESET_DESC), llm::kPromptPresets[matchedPreset].description);
            s_customPromptBackup.clear();
            s_isCustomMode = false;
            SendMessageW(GetDlgItem(hwnd, IDC_LLM_PROMPT), EM_SETREADONLY, TRUE, 0);
        } else {
            ComboBox_SetCurSel(presetCombo, llm::kPromptPresetCount);
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PRESET_DESC), L"Custom prompt");
            s_customPromptBackup = g_config.llmPrompt;
            s_isCustomMode = true;
            SendMessageW(GetDlgItem(hwnd, IDC_LLM_PROMPT), EM_SETREADONLY, FALSE, 0);
        }
    }
    RefreshProviderDropdown(hwnd);
    {
        wchar_t extra[1024] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), extra, 1024);
        if (g_config.llmExtraParams.empty()) {
            int pi = FindPresetIndex(g_config.llmProvider);
            if (pi >= 0) g_config.llmExtraParams = llm::kProviderPresets[pi].extraParams;
        }
        SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), g_config.llmExtraParams.c_str());
    }
    g_llmKeyVisible = false;
    HWND showKeyBtn = GetDlgItem(hwnd, IDC_LLM_SHOW_KEY);
    if (showKeyBtn) SetWindowTextW(showKeyBtn, L"Show");
    HWND keyEdit = GetDlgItem(hwnd, IDC_LLM_KEY);
    if (keyEdit) SendMessageW(keyEdit, EM_SETPASSWORDCHAR, L'\u25CF', 0);
    Button_SetCheck(GetDlgItem(hwnd, IDC_LLM_DEBUG), g_config.enableLlmDebug ? BST_CHECKED : BST_UNCHECKED);

    HWND backendCombo = GetDlgItem(hwnd, IDC_ASR_BACKEND);
    PopulateBackendCombo(backendCombo, g_config.asrBackend, false);
    PopulateBackendCombo(GetDlgItem(hwnd, IDC_ASR_FALLBACK_BACKEND), g_config.fallbackAsrBackend, true);

    HWND cloudProviderCombo = GetDlgItem(hwnd, IDC_CLOUD_PROVIDER);
    ComboBox_AddString(cloudProviderCombo, L"Volcano Engine (Doubao)");
    ComboBox_AddString(cloudProviderCombo, L"Baidu Cloud");
    ComboBox_AddString(cloudProviderCombo, L"Qwen ASR (DashScope)");
    ComboBox_AddString(cloudProviderCombo, L"MiMo ASR (Xiaomi)");
    ComboBox_AddString(cloudProviderCombo, L"Doubao IME (Free)");
    ComboBox_AddString(cloudProviderCombo, L"Qwen IME (Free)");

    int cloudIdx = 0;
    if (g_config.cloudProvider == L"baidu") cloudIdx = 1;
    else if (g_config.cloudProvider == L"qwen") cloudIdx = 2;
    else if (g_config.cloudProvider == L"mimo") cloudIdx = 3;
    else if (g_config.cloudProvider == L"doubao_ime") cloudIdx = 4;
    else if (g_config.cloudProvider == L"qwen_free") cloudIdx = 5;
    ComboBox_SetCurSel(cloudProviderCombo, cloudIdx);
    g_cloudProviderIdx = cloudIdx;

    g_baiduKeyVisible = false;
    HWND showBaiduBtn = GetDlgItem(hwnd, IDC_BAIDU_SHOW_KEY);
    if (showBaiduBtn) SetWindowTextW(showBaiduBtn, L"Show");
    HWND baiduKeyEdit = GetDlgItem(hwnd, IDC_BAIDU_SECRET_KEY);
    if (baiduKeyEdit) SendMessageW(baiduKeyEdit, EM_SETPASSWORDCHAR, L'\u25CF', 0);

    g_baiduApiKeyVisible = false;
    HWND showBaiduApiBtn = GetDlgItem(hwnd, IDC_BAIDU_SHOW_API_KEY);
    if (showBaiduApiBtn) SetWindowTextW(showBaiduApiBtn, L"Show");
    HWND baiduApiKeyEdit = GetDlgItem(hwnd, IDC_BAIDU_API_KEY);
    if (baiduApiKeyEdit) SendMessageW(baiduApiKeyEdit, EM_SETPASSWORDCHAR, L'\u25CF', 0);

    g_volcKeyVisible = false;
    HWND showVolcBtn = GetDlgItem(hwnd, IDC_VOLC_SHOW_KEY);
    if (showVolcBtn) SetWindowTextW(showVolcBtn, L"Show");
    HWND volcKeyEdit = GetDlgItem(hwnd, IDC_VOLC_API_KEY);
    if (volcKeyEdit) SendMessageW(volcKeyEdit, EM_SETPASSWORDCHAR, L'\u25CF', 0);

    g_qwenKeyVisible = false;
    HWND showQwenBtn = GetDlgItem(hwnd, IDC_QWEN_SHOW_KEY);
    if (showQwenBtn) SetWindowTextW(showQwenBtn, L"Show");
    HWND qwenKeyEdit = GetDlgItem(hwnd, IDC_QWEN_API_KEY);
    if (qwenKeyEdit) SendMessageW(qwenKeyEdit, EM_SETPASSWORDCHAR, L'\u25CF', 0);

    g_mimoKeyVisible = false;
    HWND showMimoBtn = GetDlgItem(hwnd, IDC_MIMO_SHOW_KEY);
    if (showMimoBtn) SetWindowTextW(showMimoBtn, L"Show");
    HWND mimoKeyEdit = GetDlgItem(hwnd, IDC_MIMO_API_KEY);
    if (mimoKeyEdit) SendMessageW(mimoKeyEdit, EM_SETPASSWORDCHAR, L'\u25CF', 0);

    SetWindowTextW(GetDlgItem(hwnd, IDC_BAIDU_API_KEY), g_config.baiduApiKey.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_BAIDU_SECRET_KEY), g_config.baiduSecretKey.c_str());

    HWND devPidCombo = GetDlgItem(hwnd, IDC_BAIDU_DEV_PID);
    ComboBox_AddString(devPidCombo, L"Mandarin (1537)");
    ComboBox_AddString(devPidCombo, L"English (1737)");
    ComboBox_AddString(devPidCombo, L"Cantonese (1637)");
    ComboBox_AddString(devPidCombo, L"Sichuanese (1837)");
    int devPidIdx = 0;
    if (g_config.baiduDevPid == 1737) devPidIdx = 1;
    else if (g_config.baiduDevPid == 1637) devPidIdx = 2;
    else if (g_config.baiduDevPid == 1837) devPidIdx = 3;
    ComboBox_SetCurSel(devPidCombo, devPidIdx);

    SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_API_KEY), g_config.volcApiKey.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_API_KEY), g_config.qwenApiKey.c_str());
    HWND qwenModelCombo = GetDlgItem(hwnd, IDC_QWEN_MODEL);
    ComboBox_ResetContent(qwenModelCombo);
    ComboBox_AddString(qwenModelCombo, L"qwen-audio-3.0-asr-flash-streaming");
    ComboBox_AddString(qwenModelCombo, L"qwen-audio-3.0-asr-flash");
    ComboBox_AddString(qwenModelCombo, L"qwen3-asr-flash-realtime");
    int qwenModelIndex = IsQwenAudioStreamingModel(g_config.qwenModel) ? 0 :
        (IsQwenAudioHttpModel(g_config.qwenModel) ? 1 : 2);
    ComboBox_SetCurSel(qwenModelCombo, qwenModelIndex);
    s_qwenUiModel = g_config.qwenModel;
    s_qwenUiHttpUrl = g_config.qwenHttpBaseUrl;
    s_qwenUiAudioStreamingUrl = g_config.qwenAudioStreamingBaseUrl;
    s_qwenUiLegacyUrl = g_config.qwenBaseUrl;
    ApplyQwenModelProfile(hwnd, g_config.qwenModel, false);
    SetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_API_KEY), g_config.mimoApiKey.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_BASE_URL), g_config.mimoBaseUrl.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_MODEL), g_config.mimoModel.c_str());
    RefreshDoubaoImeStatus(hwnd);
    // QwenFree 回填
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_FREE_SHELL_PATH), g_config.qwenFreeShellPath.c_str());
    const auto qwenPostProcess = qwen_free_postprocess::Normalize({
        g_config.qwenFreePolishEnabled,
        g_config.qwenFreePunctEnabled,
        g_config.qwenFreeCorrectEnabled,
    });
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_POLISH), qwenPostProcess.polish ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_PUNCT), qwenPostProcess.punctuate ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_CORRECT), qwenPostProcess.correct ? BST_CHECKED : BST_UNCHECKED);
    HWND qwenRewrite = GetDlgItem(hwnd, IDC_QWEN_FREE_REWRITE);
    Button_SetCheck(qwenRewrite, BST_UNCHECKED);
    EnableWindow(qwenRewrite, FALSE);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_DEBUG), g_config.qwenFreeDebugLog ? BST_CHECKED : BST_UNCHECKED);
    RefreshQwenFreeStatus(hwnd);

    HWND qwenLangCombo = GetDlgItem(hwnd, IDC_QWEN_LANGUAGE);
    for (const auto& lang : kQwenLanguages) {
        ComboBox_AddString(qwenLangCombo, lang.label);
    }
    ComboBox_SetCurSel(qwenLangCombo, QwenLanguageIndexFromCode(g_config.qwenLanguage));
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_LANGUAGE_HINTS), g_config.qwenLanguageHints.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_VOCABULARY_ID), g_config.qwenVocabularyId.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_VOCABULARY), g_config.qwenVocabulary.c_str());
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_INPUT_CONTEXT),
                    g_config.qwenEnableInputContext ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION), g_config.qwenSemanticPunctuation ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD), g_config.qwenMultiThresholdMode ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT), g_config.qwenHeartbeat ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE), g_config.qwenSpeechNoiseThresholdEnabled ? BST_CHECKED : BST_UNCHECKED);
    {
        wchar_t silence[32] = {};
        _itow_s(g_config.qwenMaxSentenceSilenceMs, silence, 10);
        SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE), silence);
    }
    {
        wchar_t threshold[32] = {};
        swprintf_s(threshold, L"%.3f", static_cast<double>(g_config.qwenSpeechNoiseThreshold));
        SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD), threshold);
    }

    HWND mimoLangCombo = GetDlgItem(hwnd, IDC_MIMO_LANGUAGE);
    for (const auto& lang : kMimoLanguages) {
        ComboBox_AddString(mimoLangCombo, lang.label);
    }
    ComboBox_SetCurSel(mimoLangCombo, MimoLanguageIndexFromCode(g_config.mimoLanguage));

    {
        wchar_t buf[32] = {};
        _itow_s(g_config.qwenChunkMs, buf, 10);
        SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_CHUNK_MS), buf);
    }

    HWND volcModeCombo = GetDlgItem(hwnd, IDC_VOLC_MODE);
    ComboBox_AddString(volcModeCombo, L"bigmodel_nostream");
    ComboBox_AddString(volcModeCombo, L"bigmodel_async");
    ComboBox_AddString(volcModeCombo, L"bigmodel");
    int modeIdx = 0;
    if (g_config.volcMode == L"bigmodel_async") modeIdx = 1;
    else if (g_config.volcMode == L"bigmodel") modeIdx = 2;
    ComboBox_SetCurSel(volcModeCombo, modeIdx);

    HWND volcResCombo = GetDlgItem(hwnd, IDC_VOLC_RESOURCE);
    ComboBox_AddString(volcResCombo, L"Seed-ASR 2.0 (duration)");
    ComboBox_AddString(volcResCombo, L"Seed-ASR 2.0 (concurrent)");
    ComboBox_AddString(volcResCombo, L"BigASR 1.0 (duration)");
    ComboBox_AddString(volcResCombo, L"BigASR 1.0 (concurrent)");
    int resIdx = 0;
    if (g_config.volcResourceId == L"volc.seedasr.sauc.concurrent") resIdx = 1;
    else if (g_config.volcResourceId == L"volc.bigasr.sauc.duration") resIdx = 2;
    else if (g_config.volcResourceId == L"volc.bigasr.sauc.concurrent") resIdx = 3;
    ComboBox_SetCurSel(volcResCombo, resIdx);

    HWND volcLangCombo = GetDlgItem(hwnd, IDC_VOLC_LANGUAGE);
    ComboBox_AddString(volcLangCombo, L"Auto (Chinese+English+Dialects)");
    ComboBox_AddString(volcLangCombo, L"English (en-US)");
    ComboBox_AddString(volcLangCombo, L"Japanese (ja-JP)");
    ComboBox_AddString(volcLangCombo, L"Korean (ko-KR)");
    ComboBox_AddString(volcLangCombo, L"French (fr-FR)");
    ComboBox_AddString(volcLangCombo, L"German (de-DE)");
    ComboBox_AddString(volcLangCombo, L"Spanish (es-MX)");
    ComboBox_AddString(volcLangCombo, L"Portuguese (pt-BR)");
    ComboBox_AddString(volcLangCombo, L"Indonesian (id-ID)");
    int langIdx = 0;
    if (g_config.volcLanguage == L"en-US") langIdx = 1;
    else if (g_config.volcLanguage == L"ja-JP") langIdx = 2;
    else if (g_config.volcLanguage == L"ko-KR") langIdx = 3;
    else if (g_config.volcLanguage == L"fr-FR") langIdx = 4;
    else if (g_config.volcLanguage == L"de-DE") langIdx = 5;
    else if (g_config.volcLanguage == L"es-MX") langIdx = 6;
    else if (g_config.volcLanguage == L"pt-BR") langIdx = 7;
    else if (g_config.volcLanguage == L"id-ID") langIdx = 8;
    ComboBox_SetCurSel(volcLangCombo, langIdx);
    EnableWindow(volcLangCombo, modeIdx == 0);

    Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM), g_config.volcEnableNonstream ? BST_CHECKED : BST_UNCHECKED);
    EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM), g_config.volcMode == L"bigmodel_async");
    {
        bool fcEnabled = (g_config.volcMode == L"bigmodel_nostream") ||
            (g_config.volcMode == L"bigmodel_async" && g_config.volcEnableNonstream);
        EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_MUSIC_FC), fcEnabled);
        EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_POI_FC), fcEnabled);
    }
    Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_DDC), g_config.volcEnableDdc ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_MUSIC_FC), g_config.volcEnableMusicFc ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_POI_FC), g_config.volcEnablePoiFc ? BST_CHECKED : BST_UNCHECKED);
    {
        wchar_t ew[32] = {};
        _itow_s(g_config.volcEndWindowSize, ew, 10);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_END_WINDOW_SIZE), ew);
    }
    {
        wchar_t ft[32] = {};
        _itow_s(g_config.volcForceToSpeechTime, ft, 10);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_FORCE_TO_SPEECH_TIME), ft);
    }

    SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_HOTWORDS_ID), g_config.volcHotwordsId.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_HOTWORDS_NAME), g_config.volcHotwordsName.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CORRECT_TABLE_ID), g_config.volcCorrectTableId.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CORRECT_TABLE_NAME), g_config.volcCorrectTableName.c_str());

    Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_CONTEXT), g_config.volcEnableContext ? BST_CHECKED : BST_UNCHECKED);
    {
        wchar_t ch[32] = {};
        _itow_s(g_config.volcContextHistory, ch, 10);
        SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CONTEXT_HISTORY), ch);
    }
    Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_INPUT_CONTEXT), g_config.volcEnableInputContext ? BST_CHECKED : BST_UNCHECKED);

    SetStatus(hwnd, L"Ready.");
    RefreshStartupRegistrationControl(hwnd, true);
}

std::wstring ComboText(HWND combo) {
    wchar_t buffer[128] = {};
    const int index = ComboBox_GetCurSel(combo);
    if (index >= 0) {
        ComboBox_GetLBText(combo, index, buffer);
    }
    return buffer;
}

void SaveSettingsControls(HWND hwnd) {
    std::wstring qwenValidationError;
    if (!ValidateQwenControls(hwnd, qwenValidationError)) {
        SetStatus(hwnd, qwenValidationError);
        MessageBoxW(hwnd, qwenValidationError.c_str(), L"Qwen Settings", MB_OK | MB_ICONERROR);
        return;
    }
    // Keep the registry-backed setting transactional with the normal config:
    // a startup registration failure must not commit any of the UI changes.
    if (!SaveStartupRegistrationControl(hwnd)) return;

    g_qwenFreeTestGeneration.fetch_add(1, std::memory_order_relaxed);
    g_qwenFreeStatusGeneration.fetch_add(1, std::memory_order_relaxed);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_FREE_TEST), TRUE);

    bool fallbackAdjusted = false;
    g_config.modelId = ModelIdFromIndex(ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_MODEL)));

    wchar_t modelDir[MAX_PATH] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), modelDir, MAX_PATH);
    g_config.modelDir = modelDir;

    g_config.threads = ComboText(GetDlgItem(hwnd, IDC_THREADS));
    if (g_config.threads.empty()) g_config.threads = L"auto";
    if (g_config.threads.substr(0, 4) == L"auto") g_config.threads = L"auto";
    g_config.enableVad = Button_GetCheck(GetDlgItem(hwnd, IDC_VAD)) == BST_CHECKED;
    {
        int vadIdx = (int)SendMessageW(GetDlgItem(hwnd, IDC_VAD_MODEL), CB_GETCURSEL, 0, 0);
        g_config.vadModel = (vadIdx == 1) ? L"firered" : L"silero";
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VAD_THRESHOLD), buf, 32);
        float v = (float)_wtof(buf);
        if (v > 0.0f && v <= 1.0f) g_config.vadThreshold = v;
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VAD_MIN_SILENCE), buf, 32);
        int v = _wtoi(buf);
        if (v > 0) g_config.vadMinSilence = v;
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VAD_MIN_SPEECH), buf, 32);
        int v = _wtoi(buf);
        if (v > 0) g_config.vadMinSpeech = v;
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VAD_PAD_START), buf, 32);
        int v = _wtoi(buf);
        if (v >= 0) g_config.vadPadStart = v;
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VAD_SMOOTH_WINDOW), buf, 32);
        int v = _wtoi(buf);
        if (v > 0) g_config.vadSmoothWindow = v;
    }
    g_config.enablePartial = Button_GetCheck(GetDlgItem(hwnd, IDC_PARTIAL)) == BST_CHECKED;
    {
        int postIdx = (int)SendMessageW(GetDlgItem(hwnd, IDC_POSTPROCESS), CB_GETCURSEL, 0, 0);
        if (postIdx == 0) g_config.postprocess = L"none";
        else if (postIdx == 2) g_config.postprocess = L"llm";
        else g_config.postprocess = L"itn";
    }

    HotkeyConfig hotkey = GetHotkeyFromEdit(hwnd, IDC_HOTKEY);
    g_config.hotkey = HotkeyToString(hotkey);

    wchar_t llmEndpoint[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_ENDPOINT), llmEndpoint, 512);
    g_config.llmEndpoint = llmEndpoint;

    wchar_t llmKey[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_KEY), llmKey, 512);
    g_config.llmApiKey = llmKey;

    wchar_t llmModel[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_MODEL), llmModel, 256);
    g_config.llmModel = llmModel;

    {
        HWND promptEdit = GetDlgItem(hwnd, IDC_LLM_PROMPT);
        int len = GetWindowTextLengthW(promptEdit);
        std::wstring prompt(len + 1, L'\0');
        GetWindowTextW(promptEdit, &prompt[0], len + 1);
        prompt.resize(len);
        g_config.llmPrompt = prompt;
    }

    wchar_t llmExtra[1024] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), llmExtra, 1024);
    g_config.llmExtraParams = llmExtra;

    g_config.llmProvider = ComboText(GetDlgItem(hwnd, IDC_LLM_PROVIDER));

    g_config.enableLlmDebug = Button_GetCheck(GetDlgItem(hwnd, IDC_LLM_DEBUG)) == BST_CHECKED;

    {
        g_config.asrBackend = BackendIdFromCombo(GetDlgItem(hwnd, IDC_ASR_BACKEND), false);
        g_config.fallbackAsrBackend = BackendIdFromCombo(GetDlgItem(hwnd, IDC_ASR_FALLBACK_BACKEND), true);
        if (g_config.fallbackAsrBackend == g_config.asrBackend) {
            g_config.fallbackAsrBackend = L"none";
            PopulateBackendCombo(GetDlgItem(hwnd, IDC_ASR_FALLBACK_BACKEND), g_config.fallbackAsrBackend, true);
            fallbackAdjusted = true;
        }
    }
    {
        int cloudIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CLOUD_PROVIDER));
        if (cloudIdx == 1) g_config.cloudProvider = L"baidu";
        else if (cloudIdx == 2) g_config.cloudProvider = L"qwen";
        else if (cloudIdx == 3) g_config.cloudProvider = L"mimo";
        else if (cloudIdx == 4) g_config.cloudProvider = L"doubao_ime";
        else if (cloudIdx == 5) g_config.cloudProvider = L"qwen_free";
        else g_config.cloudProvider = L"volcengine";
    }
    wchar_t baiduApiKey[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_BAIDU_API_KEY), baiduApiKey, 256);
    g_config.baiduApiKey = baiduApiKey;
    wchar_t baiduSecretKey[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_BAIDU_SECRET_KEY), baiduSecretKey, 256);
    g_config.baiduSecretKey = baiduSecretKey;
    {
        int devPidIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_BAIDU_DEV_PID));
        int pids[] = {1537, 1737, 1637, 1837};
        if (devPidIdx >= 0 && devPidIdx < 4) g_config.baiduDevPid = pids[devPidIdx];
    }

    wchar_t volcApiKey[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_API_KEY), volcApiKey, 256);
    g_config.volcApiKey = volcApiKey;

    wchar_t qwenApiKey[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_API_KEY), qwenApiKey, 512);
    g_config.qwenApiKey = qwenApiKey;
    wchar_t qwenBaseUrl[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_BASE_URL), qwenBaseUrl, 512);
    const std::wstring selectedQwenModel = QwenModelFromControl(hwnd);
    const std::wstring selectedQwenUrl = qwenBaseUrl[0] ? qwenBaseUrl :
        (IsQwenAudioHttpModel(selectedQwenModel) ? g_config.qwenHttpBaseUrl :
         (IsQwenAudioStreamingModel(selectedQwenModel) ? g_config.qwenAudioStreamingBaseUrl : qwen_asr::kDefaultBaseUrl));
    StoreQwenProfileUrl(hwnd, selectedQwenModel);
    if (IsQwenAudioHttpModel(selectedQwenModel)) {
        s_qwenUiHttpUrl = selectedQwenUrl;
        g_config.qwenTransport = L"audio_http";
    } else if (IsQwenAudioStreamingModel(selectedQwenModel)) {
        s_qwenUiAudioStreamingUrl = selectedQwenUrl;
        g_config.qwenTransport = L"audio_streaming";
    } else {
        s_qwenUiLegacyUrl = selectedQwenUrl;
        g_config.qwenTransport = L"legacy_realtime";
    }
    g_config.qwenHttpBaseUrl = s_qwenUiHttpUrl;
    g_config.qwenAudioStreamingBaseUrl = s_qwenUiAudioStreamingUrl;
    g_config.qwenBaseUrl = s_qwenUiLegacyUrl;
    wchar_t qwenModel[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MODEL), qwenModel, 256);
    g_config.qwenModel = qwenModel[0] ? qwenModel : L"qwen-audio-3.0-asr-flash-streaming";
    {
        int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_QWEN_LANGUAGE));
        g_config.qwenLanguage = QwenLanguageCodeFromIndex(langIdx);
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_CHUNK_MS), buf, 32);
        g_config.qwenChunkMs = std::clamp(_wtoi(buf), 20, 1000);
    }
    wchar_t qwenHints[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_LANGUAGE_HINTS), qwenHints, 512);
    g_config.qwenLanguageHints = NormalizeQwenLanguageHints(qwenHints);
    wchar_t qwenVocabId[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_VOCABULARY_ID), qwenVocabId, 512);
    g_config.qwenVocabularyId = qwenVocabId;
    wchar_t qwenVocabulary[4096] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_VOCABULARY), qwenVocabulary, 4096);
    g_config.qwenVocabulary = qwenVocabulary;
    g_config.qwenSemanticPunctuation = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
    g_config.qwenMultiThresholdMode = !g_config.qwenSemanticPunctuation &&
        Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED;
    g_config.qwenHeartbeat = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT)) == BST_CHECKED;
    g_config.qwenSpeechNoiseThresholdEnabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED;
    wchar_t qwenSilence[32] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE), qwenSilence, 32);
    g_config.qwenMaxSentenceSilenceMs = std::clamp(_wtoi(qwenSilence), 200, 6000);
    wchar_t qwenNoise[32] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD), qwenNoise, 32);
    g_config.qwenSpeechNoiseThreshold = std::clamp(static_cast<float>(_wtof(qwenNoise)), -1.0f, 1.0f);
    g_config.qwenEnableInputContext =
        Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_INPUT_CONTEXT)) == BST_CHECKED;

    wchar_t mimoApiKey[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_API_KEY), mimoApiKey, 512);
    g_config.mimoApiKey = mimoApiKey;
    wchar_t mimoBaseUrl[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_BASE_URL), mimoBaseUrl, 512);
    g_config.mimoBaseUrl = mimoBaseUrl[0] ? mimoBaseUrl : mimo_asr::kDefaultBaseUrl;
    wchar_t mimoModel[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_MODEL), mimoModel, 256);
    g_config.mimoModel = mimoModel[0] ? mimoModel : mimo_asr::kDefaultModel;
    {
        int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_MIMO_LANGUAGE));
        g_config.mimoLanguage = MimoLanguageCodeFromIndex(langIdx);
    }

    // QwenFree (千问 IME 免费后端，A1 纯协议还原)
    g_config.qwenFreePolishEnabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_POLISH)) == BST_CHECKED;
    g_config.qwenFreePunctEnabled = g_config.qwenFreePolishEnabled;
    g_config.qwenFreeCorrectEnabled = g_config.qwenFreePolishEnabled;
    // Keep the setting/API for future research, but do not activate the
    // currently disabled selection-rewrite path.
    g_config.qwenFreeRewriteEnabled = false;
    g_config.qwenFreeDebugLog = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_DEBUG)) == BST_CHECKED;
    {
        wchar_t qpath[1024] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_FREE_SHELL_PATH), qpath, 1024);
        g_config.qwenFreeShellPath = qpath;
    }

    {
        int modeIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_MODE));
        if (modeIdx == 1) g_config.volcMode = L"bigmodel_async";
        else if (modeIdx == 2) g_config.volcMode = L"bigmodel";
        else g_config.volcMode = L"bigmodel_nostream";
    }
    {
        int resIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_RESOURCE));
        if (resIdx >= 0 && resIdx < 4) g_config.volcResourceId = kVolcResources[resIdx].resourceId;
    }
    {
        int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_LANGUAGE));
        if (langIdx >= 0 && langIdx < 9) g_config.volcLanguage = kVolcLanguages[langIdx];
    }

    g_config.volcEnableNonstream = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM)) == BST_CHECKED;
    g_config.volcEnableDdc = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_DDC)) == BST_CHECKED;
    g_config.volcEnableMusicFc = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_MUSIC_FC)) == BST_CHECKED;
    g_config.volcEnablePoiFc = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_POI_FC)) == BST_CHECKED;
    {
        wchar_t ew[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_END_WINDOW_SIZE), ew, 32);
        int val = _wtoi(ew);
        g_config.volcEndWindowSize = val > 0 ? val : 800;
    }
    {
        wchar_t ft[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_FORCE_TO_SPEECH_TIME), ft, 32);
        int val = _wtoi(ft);
        g_config.volcForceToSpeechTime = (val >= 1) ? val : 0;
    }
    {
        wchar_t hw[512] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_HOTWORDS_ID), hw, 512);
        g_config.volcHotwordsId = hw;
    }
    {
        wchar_t hw[512] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_HOTWORDS_NAME), hw, 512);
        g_config.volcHotwordsName = hw;
    }
    {
        wchar_t ct[512] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CORRECT_TABLE_ID), ct, 512);
        g_config.volcCorrectTableId = ct;
    }
    {
        wchar_t ct[512] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CORRECT_TABLE_NAME), ct, 512);
        g_config.volcCorrectTableName = ct;
    }
    g_config.volcEnableContext = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_CONTEXT)) == BST_CHECKED;
    {
        wchar_t ch[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CONTEXT_HISTORY), ch, 32);
        int val = _wtoi(ch);
        g_config.volcContextHistory = (val >= 1 && val <= 20) ? val : 3;
    }
    g_config.volcEnableInputContext = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_INPUT_CONTEXT)) == BST_CHECKED;

    SaveConfig();
    SetStatus(hwnd, fallbackAdjusted
        ? L"Saved. Fallback disabled because it matches ASR Backend."
        : L"Saved. ASR engine reloaded.");
    PostMessageW(g_mainWindow, kReloadMessage, 0, 0);
}

void TestLlmConnection(HWND hwnd) {
    wchar_t endpoint[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_ENDPOINT), endpoint, 512);
    wchar_t apiKey[512] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_KEY), apiKey, 512);
    wchar_t model[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_MODEL), model, 256);

    if (wcslen(endpoint) == 0 || wcslen(apiKey) == 0 || wcslen(model) == 0) {
        SetStatus(hwnd, L"Please fill in all LLM fields.");
        return;
    }

    SetStatus(hwnd, L"Testing connection...");
    llm::RequestConfig cfg;
    cfg.endpoint = endpoint;
    cfg.apiKey = apiKey;
    cfg.model = model;
    std::thread([hwnd, cfg]() {
        llm::TestResult result = llm::TestConnection(cfg);
        PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1,
            reinterpret_cast<LPARAM>(new std::wstring(result.message)));
    }).detach();
}

void RefreshProviderDropdown(HWND hwnd) {
    HWND combo = GetDlgItem(hwnd, IDC_LLM_PROVIDER);
    std::wstring current = ComboText(combo);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (int i = 0; i < llm::kProviderPresetCount; ++i) {
        ComboBox_AddString(combo, llm::kProviderPresets[i].name);
    }
    std::string provJson = llm::WideToUtf8(g_config.llmProvidersJson);
    size_t pos = 1;
    while (pos < provJson.size()) {
        size_t q1 = provJson.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = provJson.find('"', q1 + 1);
        if (q2 == std::string::npos) break;
        std::string name = provJson.substr(q1 + 1, q2 - q1 - 1);
        bool isPreset = false;
        for (int i = 0; i < llm::kProviderPresetCount; ++i) {
            if (name == llm::WideToUtf8(llm::kProviderPresets[i].name)) { isPreset = true; break; }
        }
        if (!isPreset) {
            ComboBox_AddString(combo, llm::Utf8ToWide(name).c_str());
        }
        size_t objStart = provJson.find('{', q2);
        if (objStart == std::string::npos) break;
        int depth = 0;
        size_t objEnd = objStart;
        for (; objEnd < provJson.size(); ++objEnd) {
            if (provJson[objEnd] == '{') depth++;
            else if (provJson[objEnd] == '}') { depth--; if (depth == 0) break; }
        }
        pos = objEnd + 1;
    }
    int sel = 0;
    int count = (int)SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (int i = 0; i < count; ++i) {
        wchar_t buf[128] = {};
        ComboBox_GetLBText(combo, i, buf);
        if (current == buf) { sel = i; break; }
    }
    ComboBox_SetCurSel(combo, sel);
    bool isPresetSel = FindPresetIndex(ComboText(combo)) >= 0;
    HWND delBtn = GetDlgItem(hwnd, IDC_LLM_PROVIDER_DEL);
    if (delBtn) EnableWindow(delBtn, !isPresetSel);
    HWND resetBtn = GetDlgItem(hwnd, IDC_LLM_EXTRA_RESET);
    if (resetBtn) EnableWindow(resetBtn, isPresetSel);
}

void DeleteProviderFromStore(const std::wstring& name) {
    std::string json = llm::WideToUtf8(g_config.llmProvidersJson);
    std::string key = llm::WideToUtf8(name);
    std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return;
    size_t colon = json.find(':', pos + marker.size());
    if (colon == std::string::npos) return;
    size_t objStart = json.find('{', colon);
    if (objStart == std::string::npos) return;
    int depth = 0;
    size_t objEnd = objStart;
    for (; objEnd < json.size(); ++objEnd) {
        if (json[objEnd] == '{') depth++;
        else if (json[objEnd] == '}') { depth--; if (depth == 0) break; }
    }
    size_t eraseStart = pos;
    if (eraseStart > 0 && json[eraseStart - 1] == ',') eraseStart--;
    size_t eraseEnd = objEnd + 1;
    json.erase(eraseStart, eraseEnd - eraseStart);
    if (json == "{}" || json.empty()) json = "{}";
    g_config.llmProvidersJson = llm::Utf8ToWide(json);
}

LRESULT CALLBACK InputWndProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* data = reinterpret_cast<InputDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        if (data && data->title) SetWindowTextW(hDlg, data->title);
        CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | WS_TABSTOP,
                        12, 12, 260, 28, hDlg,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_INPUT_EDIT)),
                        g_instance, nullptr);
        HWND okBtn = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                                   12, 50, 80, 28, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      104, 50, 80, 28, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), g_instance, nullptr);
        ApplyUiFont(GetDlgItem(hDlg, IDC_INPUT_EDIT));
        ApplyUiFont(okBtn);
        ApplyUiFont(GetDlgItem(hDlg, IDCANCEL));
        SendMessageW(GetDlgItem(hDlg, IDC_INPUT_EDIT), EM_SETSEL, 0, -1);
        SetFocus(GetDlgItem(hDlg, IDC_INPUT_EDIT));
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* data = reinterpret_cast<InputDlgData*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
            if (data) {
                wchar_t buf[256] = {};
                GetWindowTextW(GetDlgItem(hDlg, IDC_INPUT_EDIT), buf, 256);
                data->result = buf;
                data->ok = !data->result.empty();
            }
            DestroyWindow(hDlg);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hDlg);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

bool ShowInputDialog(HWND parent, const wchar_t* title, std::wstring& out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = InputWndProc;
        wc.hInstance = g_instance;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"VoxTypeInputDlg";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    InputDlgData data;
    data.title = title;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 296, h = 130;
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
                               L"VoxTypeInputDlg", title,
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               x, y, w, h, parent, nullptr, g_instance, &data);
    if (!dlg) return false;
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(dlg)) break;
    }
    if (data.ok) { out = data.result; return true; }
    return false;
}

LRESULT CALLBACK VolcExtraWndProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCT*>(lParam);
        auto* data = reinterpret_cast<VolcExtraDlgData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hDlg, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(hDlg, L"Volcengine ASR Extra Params");

        HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                    WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | WS_TABSTOP,
                                    12, 12, 500, 280, hDlg,
                                    reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_EXTRA_EDIT)),
                                    g_instance, nullptr);
        ApplyUiFont(edit);
        if (data && !data->text.empty()) SetWindowTextW(edit, data->text.c_str());

        CreateWindowW(L"BUTTON", L"Filter", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      12, 302, 90, 28, hDlg,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_EXTRA_HOTWORDS)),
                      g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Result", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      110, 302, 90, 28, hDlg,
                      reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_EXTRA_CONTEXT)),
                      g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Reset", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      208, 302, 80, 28, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_EXTRA_RESET)),
                      g_instance, nullptr);

        HWND okBtn = CreateWindowW(L"BUTTON", L"OK", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | WS_TABSTOP,
                                   340, 302, 80, 28, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDOK)), g_instance, nullptr);
        CreateWindowW(L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                      430, 302, 80, 28, hDlg, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDCANCEL)), g_instance, nullptr);
        ApplyUiFont(okBtn);
        ApplyUiFont(GetDlgItem(hDlg, IDCANCEL));
        ApplyUiFont(GetDlgItem(hDlg, IDC_VOLC_EXTRA_HOTWORDS));
        ApplyUiFont(GetDlgItem(hDlg, IDC_VOLC_EXTRA_CONTEXT));
        ApplyUiFont(GetDlgItem(hDlg, IDC_VOLC_EXTRA_RESET));
        SetFocus(edit);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == IDOK) {
            auto* data = reinterpret_cast<VolcExtraDlgData*>(GetWindowLongPtrW(hDlg, GWLP_USERDATA));
            if (data) {
                HWND edit = GetDlgItem(hDlg, IDC_VOLC_EXTRA_EDIT);
                int len = GetWindowTextLengthW(edit);
                if (len > 0) {
                    std::vector<wchar_t> buf(len + 1);
                    GetWindowTextW(edit, buf.data(), len + 1);
                    data->text = buf.data();
                } else {
                    data->text.clear();
                }
                data->ok = true;
            }
            DestroyWindow(hDlg);
            return 0;
        }
        if (LOWORD(wParam) == IDCANCEL) {
            DestroyWindow(hDlg);
            return 0;
        }
        if (LOWORD(wParam) == IDC_VOLC_EXTRA_HOTWORDS) {
            HWND edit = GetDlgItem(hDlg, IDC_VOLC_EXTRA_EDIT);
            SetWindowTextW(edit, L"\"sensitive_words_filter\":\"system_reserved_filter\"");
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            return 0;
        }
        if (LOWORD(wParam) == IDC_VOLC_EXTRA_CONTEXT) {
            HWND edit = GetDlgItem(hDlg, IDC_VOLC_EXTRA_EDIT);
            SetWindowTextW(edit, L"\"result_type\":\"single\",\"vad_segment_duration\":3000");
            SendMessageW(edit, EM_SETSEL, 0, -1);
            SetFocus(edit);
            return 0;
        }
        if (LOWORD(wParam) == IDC_VOLC_EXTRA_RESET) {
            SetWindowTextW(GetDlgItem(hDlg, IDC_VOLC_EXTRA_EDIT), L"");
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hDlg);
        return 0;
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hDlg, msg, wParam, lParam);
}

bool ShowVolcExtraDialog(HWND parent, std::wstring& out) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = VolcExtraWndProc;
        wc.hInstance = g_instance;
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"VoxTypeVolcExtraDlg";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }
    VolcExtraDlgData data;
    data.text = out;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int w = 540, h = 420;
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - h) / 2;
    HWND dlg = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
                               L"VoxTypeVolcExtraDlg", L"Volcengine ASR Extra Params",
                               WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                               x, y, w, h, parent, nullptr, g_instance, &data);
    if (!dlg) return false;
    ShowWindow(dlg, SW_SHOW);
    SetForegroundWindow(dlg);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(dlg, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
        if (!IsWindow(dlg)) break;
    }
    if (data.ok) { out = data.text; return true; }
    return false;
}

void UpdateQwenAdvancedDialogState(HWND hwnd, bool streaming) {
    const bool semantic = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
    const bool noiseEnabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED;
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION), streaming ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE), streaming ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD), streaming && !semantic ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT), streaming ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE), streaming ? TRUE : FALSE);
    EnableWindow(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD),
                 streaming && noiseEnabled ? TRUE : FALSE);
}

bool ReadQwenAdvancedDialog(HWND hwnd, QwenAdvancedDialogData& data, std::wstring& error) {
    data.vocabularyId = QwenControlText(hwnd, IDC_QWEN_VOCABULARY_ID, 512);
    data.vocabulary = QwenControlText(hwnd, IDC_QWEN_VOCABULARY, 8192);
    if (!qwen_audio_json::IsValidVocabulary(data.vocabulary, &error)) return false;

    data.semanticPunctuation = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
    data.maxSentenceSilence = QwenControlText(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE, 32);
    data.multiThreshold = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED;
    data.heartbeat = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT)) == BST_CHECKED;
    data.speechNoiseEnabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED;
    data.speechNoiseThreshold = QwenControlText(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD, 32);

    if (!data.streaming) return true;
    const int silenceMs = _wtoi(data.maxSentenceSilence.c_str());
    if (silenceMs < 200 || silenceMs > 6000) {
        error = L"Silence ms must be between 200 and 6000.";
        return false;
    }
    if (data.semanticPunctuation && data.multiThreshold) {
        error = L"Semantic punctuation and multi-threshold cannot both be enabled.";
        return false;
    }
    if (data.speechNoiseEnabled) {
        const double threshold = _wtof(data.speechNoiseThreshold.c_str());
        if (threshold < -1.0 || threshold > 1.0) {
            error = L"Speech noise threshold must be between -1.0 and 1.0.";
            return false;
        }
    }
    return true;
}

LRESULT CALLBACK QwenAdvancedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* data = reinterpret_cast<QwenAdvancedDialogData*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        data = reinterpret_cast<QwenAdvancedDialogData*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(data));
        SetWindowTextW(hwnd, L"Qwen ASR Advanced Settings");

        HWND control = CreateLabel(hwnd, S(UiStyle::QwenAdvancedDialogLeft),
                                   S(UiStyle::QwenAdvancedDialogVocabIdLabelY),
                                   S(UiStyle::QwenAdvancedDialogLabelW), S(UiStyle::LabelH),
                                   L"Vocabulary ID");
        HWND vocabId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                       S(UiStyle::QwenAdvancedDialogInputLeft), S(UiStyle::QwenAdvancedDialogVocabIdY),
                                       S(UiStyle::QwenAdvancedDialogInputW), S(UiStyle::EditH), hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_VOCABULARY_ID)),
                                       g_instance, nullptr);
        ApplyUiFont(vocabId);
        control = CreateHint(hwnd, S(UiStyle::QwenAdvancedDialogInputLeft),
                             S(UiStyle::QwenAdvancedDialogVocabIdHintY),
                             S(UiStyle::QwenAdvancedDialogInputW), S(UiStyle::QwenHint2LineH),
                             L"Optional. Its target model must match the selected ASR model.");

        control = CreateLabel(hwnd, S(UiStyle::QwenAdvancedDialogLeft),
                              S(UiStyle::QwenAdvancedDialogVocabJsonLabelY),
                              S(UiStyle::QwenAdvancedDialogLabelW), S(UiStyle::LabelH),
                              L"Vocabulary JSON");
        HWND vocabulary = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                          WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_MULTILINE |
                                              ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                                          S(UiStyle::QwenAdvancedDialogInputLeft), S(UiStyle::QwenAdvancedDialogVocabJsonY),
                                          S(UiStyle::QwenAdvancedDialogInputW), S(UiStyle::QwenAdvancedDialogVocabJsonH),
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_VOCABULARY)),
                                          g_instance, nullptr);
        ApplyUiFont(vocabulary);
        control = CreateHint(hwnd, S(UiStyle::QwenAdvancedDialogInputLeft),
                             S(UiStyle::QwenAdvancedDialogVocabJsonHintY),
                             S(UiStyle::QwenAdvancedDialogInputW), S(UiStyle::QwenHint2LineH),
                             L"Optional. Example: {\"VoxType\":4,\"Qwen\":4} — weights 1–5 or 50.");

        const wchar_t* groupTitle = data && data->streaming
            ? L"Streaming recognition"
            : L"Streaming recognition (not used by this model)";
        HWND group = CreateWindowW(L"BUTTON", groupTitle, WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                   S(UiStyle::QwenAdvancedDialogLeft), S(UiStyle::QwenAdvancedDialogStreamingGroupY),
                                   S(UiStyle::QwenAdvancedDialogLabelW), S(UiStyle::QwenAdvancedDialogStreamingGroupH),
                                   hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(group);

        HWND semantic = CreateCheckBox(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION,
                                       S(UiStyle::QwenAdvancedDialogInputLeft), S(UiStyle::QwenAdvancedDialogStreamingRow1Y),
                                       S(220), S(UiStyle::CheckH), L"Semantic punctuation");
        control = CreateLabel(hwnd, S(420), S(UiStyle::QwenAdvancedDialogStreamingRow1Y) + S(4),
                              S(105), S(UiStyle::LabelH), L"Silence ms");
        HWND silence = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                       WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                                       S(530), S(UiStyle::QwenAdvancedDialogStreamingRow1Y), S(100), S(UiStyle::EditH),
                                       hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_MAX_SENTENCE_SILENCE)),
                                       g_instance, nullptr);
        ApplyUiFont(silence);
        control = CreateHint(hwnd, S(UiStyle::QwenAdvancedDialogInputLeft),
                             S(UiStyle::QwenAdvancedDialogStreamingHint1Y), S(470), S(UiStyle::QwenHint2LineH),
                             L"Semantic punctuation splits by meaning. Silence ms finalizes after silence (200–6000).");

        HWND multi = CreateCheckBox(hwnd, IDC_QWEN_MULTI_THRESHOLD,
                                    S(UiStyle::QwenAdvancedDialogInputLeft), S(UiStyle::QwenAdvancedDialogStreamingRow2Y),
                                    S(170), S(UiStyle::CheckH), L"Multi-threshold");
        HWND heartbeat = CreateCheckBox(hwnd, IDC_QWEN_HEARTBEAT,
                                        S(350), S(UiStyle::QwenAdvancedDialogStreamingRow2Y),
                                        S(150), S(UiStyle::CheckH), L"Heartbeat");
        control = CreateHint(hwnd, S(UiStyle::QwenAdvancedDialogInputLeft),
                             S(UiStyle::QwenAdvancedDialogStreamingHint2Y), S(470), S(UiStyle::QwenHint2LineH),
                             L"For noisy audio; can't combine with semantic punctuation. Heartbeat keeps the connection alive.");

        HWND noiseEnable = CreateCheckBox(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE,
                                          S(UiStyle::QwenAdvancedDialogInputLeft), S(UiStyle::QwenAdvancedDialogNoiseY),
                                          S(220), S(UiStyle::CheckH), L"Speech noise threshold");
        HWND noise = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                     S(420), S(UiStyle::QwenAdvancedDialogNoiseY), S(100), S(UiStyle::EditH),
                                     hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_SPEECH_NOISE_THRESHOLD)),
                                     g_instance, nullptr);
        ApplyUiFont(noise);
        control = CreateHint(hwnd, S(530), S(UiStyle::QwenAdvancedDialogNoiseY) + S(4),
                             S(135), S(UiStyle::QwenHintH), L"-1.0 to 1.0");
        control = CreateHint(hwnd, S(UiStyle::QwenAdvancedDialogInputLeft),
                             S(UiStyle::QwenAdvancedDialogNoiseHintY), S(480), S(UiStyle::QwenHintH),
                             L"Only enable for difficult recording environments.");

        HWND okButton = CreateButton(hwnd, IDOK, S(500), S(UiStyle::QwenAdvancedDialogFooterY),
                                     S(UiStyle::FooterBtnW), S(UiStyle::ActionBtnH), L"OK");
        HWND cancelButton = CreateButton(hwnd, IDCANCEL, S(596), S(UiStyle::QwenAdvancedDialogFooterY),
                                         S(UiStyle::FooterBtnW), S(UiStyle::ActionBtnH), L"Cancel");
        SendMessageW(okButton, BM_SETSTYLE, BS_DEFPUSHBUTTON, TRUE);

        if (data) {
            SetWindowTextW(vocabId, data->vocabularyId.c_str());
            SetWindowTextW(vocabulary, data->vocabulary.c_str());
            Button_SetCheck(semantic, data->semanticPunctuation ? BST_CHECKED : BST_UNCHECKED);
            SetWindowTextW(silence, data->maxSentenceSilence.c_str());
            Button_SetCheck(multi, data->multiThreshold ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(heartbeat, data->heartbeat ? BST_CHECKED : BST_UNCHECKED);
            Button_SetCheck(noiseEnable, data->speechNoiseEnabled ? BST_CHECKED : BST_UNCHECKED);
            SetWindowTextW(noise, data->speechNoiseThreshold.c_str());
            UpdateQwenAdvancedDialogState(hwnd, data->streaming);
        }
        SetFocus(vocabId);
        return 0;
    }
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        SetTextColor(hdc, IsSettingsHint(control) ? UiStyle::HintTextColor : UiStyle::TextColor);
        SetBkColor(hdc, UiStyle::BgColor);
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, UiStyle::InputTextColor);
        SetBkColor(hdc, UiStyle::ControlBgColor);
        return reinterpret_cast<LRESULT>(g_controlBgBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, UiStyle::BgColor);
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_QWEN_SEMANTIC_PUNCTUATION:
            if (HIWORD(wParam) == BN_CLICKED && data) {
                if (Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED) {
                    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD), BST_UNCHECKED);
                }
                UpdateQwenAdvancedDialogState(hwnd, data->streaming);
                return 0;
            }
            break;
        case IDC_QWEN_MULTI_THRESHOLD:
            if (HIWORD(wParam) == BN_CLICKED && data) {
                if (Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED) {
                    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION), BST_UNCHECKED);
                }
                UpdateQwenAdvancedDialogState(hwnd, data->streaming);
                return 0;
            }
            break;
        case IDC_QWEN_SPEECH_NOISE_ENABLE:
            if (HIWORD(wParam) == BN_CLICKED && data) {
                UpdateQwenAdvancedDialogState(hwnd, data->streaming);
                return 0;
            }
            break;
        case IDOK:
            if (data) {
                std::wstring error;
                if (!ReadQwenAdvancedDialog(hwnd, *data, error)) {
                    MessageBoxW(hwnd, error.c_str(), L"Invalid Qwen Advanced Settings", MB_OK | MB_ICONERROR);
                    return 0;
                }
                data->ok = true;
            }
            DestroyWindow(hwnd);
            return 0;
        case IDCANCEL:
            DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowQwenAdvancedDialog(HWND parent, QwenAdvancedDialogData& data) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = QwenAdvancedWndProc;
        wc.hInstance = g_instance;
        wc.hbrBackground = g_settingsBgBrush;
        wc.lpszClassName = L"VoxTypeQwenAdvancedDlg";
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT work = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    RECT parentRect = {};
    GetWindowRect(parent, &parentRect);
    const int width = S(UiStyle::QwenAdvancedDialogW);
    const int height = S(UiStyle::QwenAdvancedDialogH);
    int x = parentRect.left + ((parentRect.right - parentRect.left) - width) / 2;
    int y = parentRect.top + ((parentRect.bottom - parentRect.top) - height) / 2;
    const int workLeft = static_cast<int>(work.left);
    const int workTop = static_cast<int>(work.top);
    const int workRight = static_cast<int>(work.right);
    const int workBottom = static_cast<int>(work.bottom);
    x = std::clamp(x, workLeft, (std::max)(workLeft, workRight - width));
    y = std::clamp(y, workTop, (std::max)(workTop, workBottom - height));

    HWND dialog = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_DLGMODALFRAME,
                                  L"VoxTypeQwenAdvancedDlg", L"Qwen ASR Advanced Settings",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                                  x, y, width, height, parent, nullptr, g_instance, &data);
    if (!dialog) return false;
    EnableWindow(parent, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    MSG msg = {};
    while (IsWindow(dialog) && GetMessageW(&msg, nullptr, 0, 0)) {
        if (IsDialogMessageW(dialog, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    EnableWindow(parent, TRUE);
    SetForegroundWindow(parent);
    return data.ok;
}

void EditQwenAdvancedSettings(HWND hwnd) {
    QwenAdvancedDialogData data;
    data.streaming = IsQwenAudioStreamingModel(QwenModelFromControl(hwnd));
    data.vocabularyId = QwenControlText(hwnd, IDC_QWEN_VOCABULARY_ID, 512);
    data.vocabulary = QwenControlText(hwnd, IDC_QWEN_VOCABULARY, 8192);
    data.semanticPunctuation = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
    data.maxSentenceSilence = QwenControlText(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE, 32);
    data.multiThreshold = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED;
    data.heartbeat = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT)) == BST_CHECKED;
    data.speechNoiseEnabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED;
    data.speechNoiseThreshold = QwenControlText(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD, 32);
    if (!ShowQwenAdvancedDialog(hwnd, data)) return;

    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_VOCABULARY_ID), data.vocabularyId.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_VOCABULARY), data.vocabulary.c_str());
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION),
                    data.semanticPunctuation ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE), data.maxSentenceSilence.c_str());
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD),
                    data.multiThreshold ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT), data.heartbeat ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE),
                    data.speechNoiseEnabled ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD), data.speechNoiseThreshold.c_str());
    ApplyQwenModelProfile(hwnd, QwenModelFromControl(hwnd), true);
    SetStatus(hwnd, L"Qwen advanced settings updated. Click Save to apply.");
}

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc, g_settingsBgBrush);
        return 1;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_settingsBgBrush);

        HPEN line = CreatePen(PS_SOLID, 1, UiStyle::DividerColor);
        HGDIOBJ oldPen = SelectObject(hdc, line);
        const int footerTop = (rc.bottom - UiStyle::FooterHeight > UiStyle::FooterMinTop) ? rc.bottom - UiStyle::FooterHeight : UiStyle::FooterMinTop;
        MoveToEx(hdc, 24, footerTop, nullptr);
        LineTo(hdc, rc.right - 24, footerTop);
        SelectObject(hdc, oldPen);
        DeleteObject(line);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        LayoutSettingsWindow(hwnd);
        return 0;
    case kDoubaoImeSettingsRefreshMessage:
        RefreshDoubaoImeStatus(hwnd);
        return 0;
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        if (ctl == GetDlgItem(hwnd, IDC_LLM_PROMPT_HINT) || IsSettingsHint(ctl)) {
            SetTextColor(hdc, UiStyle::HintTextColor);
        } else {
            SetTextColor(hdc, UiStyle::TextColor);
        }
        SetBkColor(hdc, UiStyle::BgColor);
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, UiStyle::InputTextColor);
        SetBkColor(hdc, UiStyle::ControlBgColor);
        return reinterpret_cast<LRESULT>(g_controlBgBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, UiStyle::BgColor);
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    }
    case WM_CREATE: {
        UpdateUiScale(hwnd);
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
        g_recognitionControls.clear();
        g_generalControls.clear();
        g_llmControls.clear();
        g_promptControls.clear();
        g_cloudAsrControls.clear();
        g_baiduControls.clear();
        g_volcengineControls.clear();
        g_qwenControls.clear();
        g_qwenAudio3Controls.clear();
        g_qwenAudioStreamingOnlyControls.clear();
        s_qwenChunkContextHint = nullptr;
        g_mimoControls.clear();
        g_doubaoImeControls.clear();
        g_qwenFreeControls.clear();
        g_vadFireredControls.clear();
        g_vadSileroControls.clear();

        HWND tab = CreateWindowW(WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                S(UiStyle::Margin), S(16), S(786), S(330), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_TAB)), g_instance, nullptr);
        ApplyUiFont(tab);
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(L"General");
        TabCtrl_InsertItem(tab, 0, &item);
        item.pszText = const_cast<LPWSTR>(L"Recognition");
        TabCtrl_InsertItem(tab, 1, &item);
        item.pszText = const_cast<LPWSTR>(L"Cloud ASR");
        TabCtrl_InsertItem(tab, 2, &item);
        item.pszText = const_cast<LPWSTR>(L"LLM");
        TabCtrl_InsertItem(tab, 3, &item);
        item.pszText = const_cast<LPWSTR>(L"LLM Prompt");
        TabCtrl_InsertItem(tab, 4, &item);

        HWND control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(0)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"ASR Backend");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_ASR_BACKEND, S(UiStyle::InputLeft), S(UiStyle::RowInputY(0)), S(UiStyle::PrimaryBackendComboW), S(UiStyle::ComboH)));
        control = CreateLabel(hwnd, S(UiStyle::FallbackLabelX), S(UiStyle::RowLabelY(0)), S(68), S(UiStyle::LabelH), L"Fallback");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_ASR_FALLBACK_BACKEND, S(UiStyle::FallbackComboX), S(UiStyle::RowInputY(0)), S(UiStyle::FallbackComboW), S(UiStyle::ComboH)));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"ASR model");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_MODEL, S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(330), S(180)));
        AddRecognitionControl(CreateButton(hwnd, IDC_DOWNLOAD_MODELS, S(UiStyle::InputLeft) + S(350), S(UiStyle::RowInputY(1)) - S(1), S(220), S(UiStyle::BtnH), L"Download Local Model"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Model folder");
        AddRecognitionControl(control);
        HWND modelDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(UiStyle::InputW), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODEL_DIR)), g_instance, nullptr);
        ApplyUiFont(modelDir);
        AddRecognitionControl(modelDir);
        AddRecognitionControl(CreateButton(hwnd, IDC_BROWSE, S(UiStyle::SideBtnX), S(UiStyle::RowInputY(2)) - S(1), S(UiStyle::SideBtnW), S(UiStyle::BtnH), L"Browse..."));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Threads");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_THREADS, S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(130), S(UiStyle::ComboH)));
        HWND vad = CreateWindowW(L"BUTTON", L"Enable VAD", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 S(358), S(UiStyle::RowInputY(3)) + S(4), S(140), S(UiStyle::LabelH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD)), g_instance, nullptr);
        HWND partial = CreateWindowW(L"BUTTON", L"Partial result", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     S(518), S(UiStyle::RowInputY(3)) + S(4), S(160), S(UiStyle::LabelH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PARTIAL)), g_instance, nullptr);
        ApplyUiFont(vad);
        ApplyUiFont(partial);
        AddRecognitionControl(vad);
        AddRecognitionControl(partial);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(4)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"VAD model");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_VAD_MODEL, S(UiStyle::InputLeft), S(UiStyle::RowInputY(4)), S(UiStyle::ComboW), S(UiStyle::ComboH)));

        const int vadGroupY = S(UiStyle::RowInputY(5));
        HWND vadGroup = CreateWindowW(L"BUTTON", L"VAD Parameters", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                       S(30), vadGroupY, S(788), S(170), hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(vadGroup);
        AddRecognitionControl(vadGroup);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), vadGroupY + S(28), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Threshold");
        AddRecognitionControl(control);
        HWND vadThreshold = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                             S(UiStyle::InputLeft), vadGroupY + S(20), S(100), S(UiStyle::EditH), hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD_THRESHOLD)), g_instance, nullptr);
        ApplyUiFont(vadThreshold);
        AddRecognitionControl(vadThreshold);
        control = CreateLabel(hwnd, S(UiStyle::InputLeft) + S(108), vadGroupY + S(28), S(80), S(UiStyle::LabelH), L"(0.0~1.0)");
        AddRecognitionControl(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), vadGroupY + S(68), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Min silence");
        AddRecognitionControl(control);
        HWND vadMinSilence = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                              S(UiStyle::InputLeft), vadGroupY + S(60), S(100), S(UiStyle::EditH), hwnd,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD_MIN_SILENCE)), g_instance, nullptr);
        ApplyUiFont(vadMinSilence);
        AddRecognitionControl(vadMinSilence);
        control = CreateLabel(hwnd, S(UiStyle::InputLeft) + S(108), vadGroupY + S(68), S(80), S(UiStyle::LabelH), L"ms");
        AddRecognitionControl(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), vadGroupY + S(108), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Min speech");
        AddRecognitionControl(control);
        HWND vadMinSpeech = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                             S(UiStyle::InputLeft), vadGroupY + S(100), S(100), S(UiStyle::EditH), hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD_MIN_SPEECH)), g_instance, nullptr);
        ApplyUiFont(vadMinSpeech);
        AddRecognitionControl(vadMinSpeech);
        control = CreateLabel(hwnd, S(UiStyle::InputLeft) + S(108), vadGroupY + S(108), S(80), S(UiStyle::LabelH), L"ms");
        AddRecognitionControl(control);

        control = CreateLabel(hwnd, S(420), vadGroupY + S(68), S(100), S(UiStyle::LabelH), L"Pad start");
        AddRecognitionControl(control);
        AddVadFireredControl(control);
        HWND vadPadStart = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                            S(530), vadGroupY + S(60), S(100), S(UiStyle::EditH), hwnd,
                                            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD_PAD_START)), g_instance, nullptr);
        ApplyUiFont(vadPadStart);
        AddRecognitionControl(vadPadStart);
        AddVadFireredControl(vadPadStart);
        control = CreateLabel(hwnd, S(638), vadGroupY + S(68), S(80), S(UiStyle::LabelH), L"ms");
        AddRecognitionControl(control);
        AddVadFireredControl(control);

        control = CreateLabel(hwnd, S(420), vadGroupY + S(108), S(100), S(UiStyle::LabelH), L"Smooth win");
        AddRecognitionControl(control);
        AddVadFireredControl(control);
        HWND vadSmoothWin = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                             S(530), vadGroupY + S(100), S(100), S(UiStyle::EditH), hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD_SMOOTH_WINDOW)), g_instance, nullptr);
        ApplyUiFont(vadSmoothWin);
        AddRecognitionControl(vadSmoothWin);
        AddVadFireredControl(vadSmoothWin);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(9)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Punctuation");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_POSTPROCESS, S(UiStyle::InputLeft), S(UiStyle::RowInputY(9)), S(UiStyle::ComboW), S(UiStyle::ComboH)));

        const int shortcutY = S(UiStyle::GeneralShortcutGroupY);
        HWND shortcutGroup = CreateWindowW(L"BUTTON", L"Shortcut", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                           S(UiStyle::GeneralGroupX), shortcutY, S(UiStyle::GeneralGroupW), S(UiStyle::GeneralShortcutGroupH), hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(shortcutGroup);
        AddGeneralControl(shortcutGroup);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), shortcutY + S(28), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Hold hotkey");
        AddGeneralControl(control);
        AddGeneralControl(CreateHotkeyEdit(hwnd, IDC_HOTKEY, S(UiStyle::InputLeft), shortcutY + S(20), S(300), S(UiStyle::HotkeyEditH), CurrentConfiguredHotkey()));
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), shortcutY + S(68), S(740), S(UiStyle::LabelH), L"Click the field, then press the key or key combination to use while recording.");
        AddGeneralControl(control);
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), shortcutY + S(98), S(740), S(UiStyle::LabelH), L"Esc cancels recording a shortcut. Backspace/Delete clears it.");
        AddGeneralControl(control);

        const int startupY = S(UiStyle::GeneralStartupGroupY);
        HWND startupGroup = CreateWindowW(L"BUTTON", L"Startup", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                          S(UiStyle::GeneralGroupX), startupY, S(UiStyle::GeneralGroupW), S(UiStyle::GeneralStartupGroupH), hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(startupGroup);
        AddGeneralControl(startupGroup);
        HWND startupCheckbox = CreateWindowW(L"BUTTON", L"Start VoxType when I sign in to Windows", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                             S(UiStyle::ContentLeft), startupY + S(UiStyle::GeneralStartupCheckOffsetY), S(UiStyle::GeneralStartupCheckW), S(UiStyle::CheckH), hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_START_WITH_WINDOWS)), g_instance, nullptr);
        ApplyUiFont(startupCheckbox);
        AddGeneralControl(startupCheckbox);
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), startupY + S(UiStyle::GeneralStartupHintOffsetY), S(UiStyle::GeneralStartupHintW), S(UiStyle::LabelH),
                              L"Uses your Windows account startup list. Moving a Portable copy is corrected when you save.");
        AddGeneralControl(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(0)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Provider");
        AddLlmControl(control);
        AddLlmControl(CreateCombo(hwnd, IDC_LLM_PROVIDER, S(UiStyle::InputLeft), S(UiStyle::RowInputY(0)), S(480), S(400)));
        AddLlmControl(CreateButton(hwnd, IDC_LLM_PROVIDER_ADD, S(UiStyle::SideBtnX), S(UiStyle::RowInputY(0)) - S(1), S(44), S(UiStyle::BtnH), L"+"));
        AddLlmControl(CreateButton(hwnd, IDC_LLM_PROVIDER_DEL, S(UiStyle::SideBtnX) + S(48), S(UiStyle::RowInputY(0)) - S(1), S(44), S(UiStyle::BtnH), L"\u2212"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"API Base URL");
        AddLlmControl(control);
        HWND llmEndpoint = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                           S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(UiStyle::InputWFull), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_ENDPOINT)), g_instance, nullptr);
        ApplyUiFont(llmEndpoint);
        AddLlmControl(llmEndpoint);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"API Key");
        AddLlmControl(control);
        HWND llmKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                      S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(UiStyle::InputW), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_KEY)), g_instance, nullptr);
        ApplyUiFont(llmKey);
        AddLlmControl(llmKey);
        AddLlmControl(CreateButton(hwnd, IDC_LLM_SHOW_KEY, S(UiStyle::SideBtnX), S(UiStyle::RowInputY(2)) - S(1), S(UiStyle::SideBtnW), S(UiStyle::BtnH), L"Show"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Model");
        AddLlmControl(control);
        HWND llmModel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(UiStyle::InputWFull), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_MODEL)), g_instance, nullptr);
        ApplyUiFont(llmModel);
        AddLlmControl(llmModel);

        AddLlmControl(CreateButton(hwnd, IDC_LLM_TEST, S(UiStyle::InputLeft), S(UiStyle::RowInputY(4)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));
        HWND llmDebug = CreateWindowW(L"BUTTON", L"Log refine before/after", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      S(348), S(UiStyle::RowInputY(4)) + S(6), S(220), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_DEBUG)), g_instance, nullptr);
        ApplyUiFont(llmDebug);
        AddLlmControl(llmDebug);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(5)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Extra Params");
        AddLlmControl(control);
        HWND llmExtra = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        S(UiStyle::InputLeft), S(UiStyle::RowInputY(5)), S(UiStyle::InputW), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_EXTRA)), g_instance, nullptr);
        ApplyUiFont(llmExtra);
        AddLlmControl(llmExtra);
        AddLlmControl(CreateButton(hwnd, IDC_LLM_EXTRA_RESET, S(UiStyle::SideBtnX), S(UiStyle::RowInputY(5)) - S(1), S(UiStyle::SideBtnW), S(UiStyle::BtnH), L"Reset"));
        {
            HWND hint = CreateWindowW(L"STATIC",
                L"JSON snippet merged into request body. e.g. \"thinking\":{\"type\":\"disabled\"}",
                WS_CHILD | WS_VISIBLE, S(UiStyle::InputLeft), S(UiStyle::RowInputY(5)) + S(UiStyle::EditH) + S(2), S(UiStyle::InputW), S(20), hwnd, nullptr, g_instance, nullptr);
            ApplyUiFont(hint);
            AddLlmControl(hint);
        }

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(0)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Preset");
        AddPromptControl(control);
        AddPromptControl(CreateCombo(hwnd, IDC_LLM_PRESET_COMBO, S(UiStyle::InputLeft), S(UiStyle::RowInputY(0)), S(220), S(200)));

        control = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                S(UiStyle::ContentLeft), S(UiStyle::RowInputY(0)) + S(UiStyle::EditH) + S(4),
                                S(UiStyle::InputWFull) + S(UiStyle::InputLeft) - S(UiStyle::ContentLeft), S(40),
                                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_PRESET_DESC)), g_instance, nullptr);
        ApplyUiFont(control);
        AddPromptControl(control);

        const int groupBoxY = S(UiStyle::RowInputY(0)) + S(UiStyle::EditH) + S(4) + S(40) + S(8);
        HWND promptGroup = CreateWindowW(L"BUTTON", L"System Prompt",
                                          WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                          S(30), groupBoxY, S(788), S(400),
                                          hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(promptGroup);
        AddPromptControl(promptGroup);

        const int promptEditY = groupBoxY + S(28);
        const int editWidth = S(788) - (S(UiStyle::ContentLeft) - S(30)) - S(8);
        const int editHeight = S(400) - S(28) - S(8) - S(48) - S(4);
        HWND llmPrompt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                                          S(UiStyle::ContentLeft), promptEditY,
                                          editWidth, editHeight,
                                          hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_PROMPT)), g_instance, nullptr);
        ApplyUiFont(llmPrompt);
        AddPromptControl(llmPrompt);

        control = CreateWindowW(L"STATIC",
                                L"Note: System Prompt only takes effect when Punctuation is set to \"Auto punctuate + LLM\" in the Recognition tab.",
                                WS_CHILD | WS_VISIBLE,
                                S(UiStyle::ContentLeft), promptEditY + editHeight + S(4),
                                editWidth, S(48),
                                hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_PROMPT_HINT)), g_instance, nullptr);
        ApplyUiFont(control);
        AddPromptControl(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(0)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Provider");
        AddCloudAsrControl(control);
        AddCloudAsrControl(CreateCombo(hwnd, IDC_CLOUD_PROVIDER, S(UiStyle::InputLeft), S(UiStyle::RowInputY(0)), S(300), S(UiStyle::ComboH)));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"API Key");
        AddBaiduControl(control);
        HWND baiduApiKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                           S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BAIDU_API_KEY)), g_instance, nullptr);
        ApplyUiFont(baiduApiKey);
        AddBaiduControl(baiduApiKey);
        AddBaiduControl(CreateButton(hwnd, IDC_BAIDU_SHOW_API_KEY, S(UiStyle::SmallBtnX), S(UiStyle::RowInputY(1)) - S(1), S(UiStyle::SmallBtnW), S(UiStyle::BtnH), L"Show"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Secret Key");
        AddBaiduControl(control);
        HWND baiduSecretKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                              S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BAIDU_SECRET_KEY)), g_instance, nullptr);
        ApplyUiFont(baiduSecretKey);
        AddBaiduControl(baiduSecretKey);
        AddBaiduControl(CreateButton(hwnd, IDC_BAIDU_SHOW_KEY, S(UiStyle::SmallBtnX), S(UiStyle::RowInputY(2)) - S(1), S(UiStyle::SmallBtnW), S(UiStyle::BtnH), L"Show"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Language Model");
        AddBaiduControl(control);
        AddBaiduControl(CreateCombo(hwnd, IDC_BAIDU_DEV_PID, S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(UiStyle::ComboW), S(UiStyle::ComboH)));

        AddBaiduControl(CreateButton(hwnd, IDC_BAIDU_TEST, S(500), S(UiStyle::RowInputY(0)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"API Key");
        AddQwenControl(control);
        HWND qwenApiKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                          S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_API_KEY)), g_instance, nullptr);
        ApplyUiFont(qwenApiKey);
        AddQwenControl(qwenApiKey);
        AddQwenControl(CreateButton(hwnd, IDC_QWEN_SHOW_KEY, S(UiStyle::SmallBtnX), S(UiStyle::RowInputY(1)) - S(1), S(UiStyle::SmallBtnW), S(UiStyle::BtnH), L"Show"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Base URL");
        AddQwenControl(control);
        HWND qwenBaseUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                           S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(UiStyle::InputWFull), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_BASE_URL)), g_instance, nullptr);
        ApplyUiFont(qwenBaseUrl);
        AddQwenControl(qwenBaseUrl);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Model");
        AddQwenControl(control);
        HWND qwenModel = CreateCombo(hwnd, IDC_QWEN_MODEL, S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(420), S(UiStyle::ComboH));
        AddQwenControl(qwenModel);
        AddQwenControl(CreateButton(hwnd, IDC_QWEN_OPEN_LOG, S(620), S(UiStyle::RowInputY(3)), S(110), S(UiStyle::ActionBtnH), L"Open log"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::QwenLanguageY) + S(UiStyle::LabelYOffset), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Language");
        AddQwenControl(control);
        AddQwenControl(CreateCombo(hwnd, IDC_QWEN_LANGUAGE, S(UiStyle::InputLeft), S(UiStyle::QwenLanguageY), S(UiStyle::ComboW), S(UiStyle::ComboH)));
        control = CreateHint(hwnd, S(UiStyle::InputLeft), S(UiStyle::QwenLanguageHintY),
                             S(UiStyle::InputWFull), S(UiStyle::QwenHint2LineH),
                             L"Fallback when Language hints is blank.");
        AddQwenControl(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::QwenChunkY) + S(UiStyle::LabelYOffset), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Chunk ms");
        AddQwenControl(control);
        HWND qwenChunkMs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                           S(UiStyle::InputLeft), S(UiStyle::QwenChunkY), S(80), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_CHUNK_MS)), g_instance, nullptr);
        ApplyUiFont(qwenChunkMs);
        AddQwenControl(qwenChunkMs);

        HWND qwenInputContext = CreateCheckBox(hwnd, IDC_QWEN_INPUT_CONTEXT,
                                                S(300), S(UiStyle::QwenChunkY), S(300), S(UiStyle::CheckH),
                                                L"Use input field text as context");
        AddQwenAudio3Control(qwenInputContext);
        control = CreateHint(hwnd, S(UiStyle::InputLeft), S(UiStyle::QwenChunkHintY),
                             S(UiStyle::InputWFull), S(UiStyle::QwenHint2LineH),
                             L"100–300 ms recommended. Context sends the focused field's text (≤400 chars) to the cloud.");
        s_qwenChunkContextHint = control;
        AddQwenControl(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::QwenLanguageHintsY) + S(UiStyle::LabelYOffset), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Language hints");
        AddQwenAudio3Control(control);
        HWND qwenHints = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         S(UiStyle::InputLeft), S(UiStyle::QwenLanguageHintsY), S(UiStyle::InputW), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_LANGUAGE_HINTS)), g_instance, nullptr);
        ApplyUiFont(qwenHints); AddQwenAudio3Control(qwenHints);
        AddQwenAudio3Control(CreateButton(hwnd, IDC_QWEN_LANGUAGE_HINTS_RESET,
                                          S(UiStyle::SideBtnX), S(UiStyle::QwenLanguageHintsY) - S(1),
                                          S(UiStyle::SideBtnW), S(UiStyle::BtnH), L"Reset"));
        control = CreateHint(hwnd, S(UiStyle::InputLeft), S(UiStyle::QwenLanguageHintsHintY),
                             S(UiStyle::InputWFull), S(UiStyle::QwenHint2LineH),
                             L"Up to 4 codes, e.g. zh,en,yue. Blank uses the Language setting.");
        AddQwenAudio3Control(control);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::QwenAdvancedButtonY) + S(UiStyle::LabelYOffset), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Advanced");
        AddQwenAudio3Control(control);
        AddQwenAudio3Control(CreateButton(hwnd, IDC_QWEN_ADVANCED,
                                          S(UiStyle::InputLeft), S(UiStyle::QwenAdvancedButtonY),
                                          S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Advanced..."));
        control = CreateHint(hwnd, S(UiStyle::InputLeft), S(UiStyle::QwenAdvancedHintY),
                             S(UiStyle::InputWFull), S(UiStyle::QwenHint2LineH),
                             L"Hotwords, semantic punctuation, VAD thresholds, heartbeat.");
        AddQwenAudio3Control(control);

        HWND qwenVocabId = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_VOCABULARY_ID)), g_instance, nullptr);
        HWND qwenVocabulary = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | ES_MULTILINE,
                                              0, 0, 0, 0, hwnd,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_VOCABULARY)), g_instance, nullptr);
        HWND qwenSemantic = CreateWindowW(L"BUTTON", nullptr, WS_CHILD | BS_AUTOCHECKBOX,
                                          0, 0, 0, 0, hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_SEMANTIC_PUNCTUATION)), g_instance, nullptr);
        HWND qwenSilence = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL | ES_NUMBER,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_MAX_SENTENCE_SILENCE)), g_instance, nullptr);
        HWND qwenMulti = CreateWindowW(L"BUTTON", nullptr, WS_CHILD | BS_AUTOCHECKBOX,
                                       0, 0, 0, 0, hwnd,
                                       reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_MULTI_THRESHOLD)), g_instance, nullptr);
        HWND qwenHeartbeat = CreateWindowW(L"BUTTON", nullptr, WS_CHILD | BS_AUTOCHECKBOX,
                                           0, 0, 0, 0, hwnd,
                                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_HEARTBEAT)), g_instance, nullptr);
        HWND qwenNoiseEnable = CreateWindowW(L"BUTTON", nullptr, WS_CHILD | BS_AUTOCHECKBOX,
                                             0, 0, 0, 0, hwnd,
                                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_SPEECH_NOISE_ENABLE)), g_instance, nullptr);
        HWND qwenNoise = CreateWindowExW(0, L"EDIT", nullptr, WS_CHILD | ES_AUTOHSCROLL,
                                         0, 0, 0, 0, hwnd,
                                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_SPEECH_NOISE_THRESHOLD)), g_instance, nullptr);
        ApplyUiFont(qwenVocabId);
        ApplyUiFont(qwenVocabulary);
        ApplyUiFont(qwenSemantic);
        ApplyUiFont(qwenSilence);
        ApplyUiFont(qwenMulti);
        ApplyUiFont(qwenHeartbeat);
        ApplyUiFont(qwenNoiseEnable);
        ApplyUiFont(qwenNoise);

        AddQwenControl(CreateButton(hwnd, IDC_QWEN_TEST, S(500), S(UiStyle::RowInputY(0)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"API Key");
        AddMimoControl(control);
        HWND mimoApiKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                          S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MIMO_API_KEY)), g_instance, nullptr);
        ApplyUiFont(mimoApiKey);
        AddMimoControl(mimoApiKey);
        AddMimoControl(CreateButton(hwnd, IDC_MIMO_SHOW_KEY, S(UiStyle::SmallBtnX), S(UiStyle::RowInputY(1)) - S(1), S(UiStyle::SmallBtnW), S(UiStyle::BtnH), L"Show"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Base URL");
        AddMimoControl(control);
        HWND mimoBaseUrl = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", mimo_asr::kDefaultBaseUrl, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                           S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(480), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MIMO_BASE_URL)), g_instance, nullptr);
        ApplyUiFont(mimoBaseUrl);
        AddMimoControl(mimoBaseUrl);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Model");
        AddMimoControl(control);
        HWND mimoModel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", mimo_asr::kDefaultModel, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MIMO_MODEL)), g_instance, nullptr);
        ApplyUiFont(mimoModel);
        AddMimoControl(mimoModel);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(4)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Language");
        AddMimoControl(control);
        AddMimoControl(CreateCombo(hwnd, IDC_MIMO_LANGUAGE, S(UiStyle::InputLeft), S(UiStyle::RowInputY(4)), S(UiStyle::ComboW), S(UiStyle::ComboH)));

        AddMimoControl(CreateButton(hwnd, IDC_MIMO_TEST, S(500), S(UiStyle::RowInputY(0)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Credentials");
        AddDoubaoImeControl(control);
        HWND doubaoStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                          S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)) + S(4),
                                          S(500), S(UiStyle::LabelH), hwnd,
                                          reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_DOUBAO_IME_STATUS)),
                                          g_instance, nullptr);
        ApplyUiFont(doubaoStatus);
        AddDoubaoImeControl(doubaoStatus);
        AddDoubaoImeControl(CreateButton(hwnd, IDC_DOUBAO_IME_TEST, S(500), S(UiStyle::RowInputY(0)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));
        AddDoubaoImeControl(CreateButton(hwnd, IDC_DOUBAO_IME_RESET, S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(170), S(UiStyle::ActionBtnH), L"Reset Credentials"));
        control = CreateLabel(hwnd, S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(560), S(UiStyle::LabelH),
                              L"Experimental unofficial Doubao IME endpoint. No API key is required.");
        AddDoubaoImeControl(control);

        // === QwenFree (千问 IME 免费后端) ===
        // Row 0: Test Connection (right-aligned, 与 doubao_ime 一致)
        AddQwenFreeControl(CreateButton(hwnd, IDC_QWEN_FREE_TEST, S(500), S(UiStyle::RowInputY(0)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));
        // Row 1: Shell path label + edit + browse
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Shell Path");
        AddQwenFreeControl(control);
        HWND qwenFreePath = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                             S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(UiStyle::QwenFreeShellPathW), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_FREE_SHELL_PATH)), g_instance, nullptr);
        ApplyUiFont(qwenFreePath);
        AddQwenFreeControl(qwenFreePath);
        AddQwenFreeControl(CreateButton(hwnd, IDC_QWEN_FREE_BROWSE,
                                         S(UiStyle::InputLeft + UiStyle::QwenFreeShellPathW + UiStyle::QwenFreeShellBrowseGap),
                                         S(UiStyle::RowInputY(1)) - S(1), S(UiStyle::SmallBtnW), S(UiStyle::BtnH), L"Browse..."));
        // Row 2: Status
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Status");
        AddQwenFreeControl(control);
        HWND qwenFreeStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                              S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)) + S(4),
                                              S(500), S(UiStyle::LabelH), hwnd,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_FREE_STATUS)),
                                              g_instance, nullptr);
        ApplyUiFont(qwenFreeStatus);
        AddQwenFreeControl(qwenFreeStatus);
        // Row 3: Qwen VoiceInputWrite 后处理选项。
        // 原版 VoiceInputWrite 会将标点、纠错和整理作为同一轮后处理；
        // 因此这里保留兼容配置，但不要暗示它们已经是三个独立请求。
        AddQwenFreeControl(CreateCheckBox(hwnd, IDC_QWEN_FREE_POLISH,
                                          S(UiStyle::ContentLeft), S(UiStyle::RowInputY(3)),
                                          S(UiStyle::QwenFreeOptionCheckW), S(UiStyle::CheckH), L"Polish (auto)"));
        HWND qwenPunct = CreateCheckBox(hwnd, IDC_QWEN_FREE_PUNCT,
                                        S(UiStyle::ContentLeft + UiStyle::QwenFreeOptionCheckW + UiStyle::QwenFreeOptionGap),
                                        S(UiStyle::RowInputY(3)), S(UiStyle::QwenFreeOptionCheckW), S(UiStyle::CheckH), L"Punctuation included");
        EnableWindow(qwenPunct, FALSE);
        AddQwenFreeControl(qwenPunct);
        HWND qwenCorrect = CreateCheckBox(hwnd, IDC_QWEN_FREE_CORRECT,
                                          S(UiStyle::ContentLeft + (UiStyle::QwenFreeOptionCheckW + UiStyle::QwenFreeOptionGap) * 2),
                                          S(UiStyle::RowInputY(3)), S(UiStyle::QwenFreeOptionCheckW), S(UiStyle::CheckH), L"Correction included");
        EnableWindow(qwenCorrect, FALSE);
        AddQwenFreeControl(qwenCorrect);
        // Row 4: More options
        HWND qwenRewrite = CreateCheckBox(hwnd, IDC_QWEN_FREE_REWRITE,
                                          S(UiStyle::ContentLeft), S(UiStyle::RowInputY(4)),
                                          S(UiStyle::QwenFreeRewriteCheckW), S(UiStyle::CheckH), L"Rewrite selection (experimental)");
        Button_SetCheck(qwenRewrite, BST_UNCHECKED);
        EnableWindow(qwenRewrite, FALSE);
        AddQwenFreeControl(qwenRewrite);
        AddQwenFreeControl(CreateCheckBox(hwnd, IDC_QWEN_FREE_DEBUG,
                                          S(UiStyle::ContentLeft + UiStyle::QwenFreeRewriteCheckW + UiStyle::QwenFreeOptionGap),
                                          S(UiStyle::RowInputY(4)), S(UiStyle::QwenFreeDebugCheckW), S(UiStyle::CheckH), L"Debug log"));
        // Row 5: hint label (与 doubao_ime 的提示行风格一致)
        control = CreateLabel(hwnd, S(UiStyle::InputLeft), S(UiStyle::RowInputY(5)), S(560), S(UiStyle::LabelH),
                              L"VoiceInputWrite bundles punctuation and correction; only Polish is configurable. Selection rewrite is temporarily disabled.");
        AddQwenFreeControl(control);


        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(1)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"API Key (X-Api-Key)");
        AddVolcengineControl(control);
        HWND volcApiKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                          S(UiStyle::InputLeft), S(UiStyle::RowInputY(1)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_API_KEY)), g_instance, nullptr);
        ApplyUiFont(volcApiKey);
        AddVolcengineControl(volcApiKey);
        AddVolcengineControl(CreateButton(hwnd, IDC_VOLC_SHOW_KEY, S(UiStyle::SmallBtnX), S(UiStyle::RowInputY(1)) - S(1), S(UiStyle::SmallBtnW), S(UiStyle::BtnH), L"Show"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(2)), S(130), S(UiStyle::LabelH), L"Model");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateCombo(hwnd, IDC_VOLC_RESOURCE, S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(480), S(UiStyle::ComboH)));
        AddVolcengineControl(CreateButton(hwnd, IDC_VOLC_OPEN_LOG, S(680), S(UiStyle::RowInputY(2)), S(100), S(UiStyle::ActionBtnH), L"Open log"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(130), S(UiStyle::LabelH), L"ASR Mode");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateCombo(hwnd, IDC_VOLC_MODE, S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(220), S(UiStyle::ComboH)));

        control = CreateLabel(hwnd, S(420), S(UiStyle::RowLabelY(3)), S(80), S(UiStyle::LabelH), L"Language");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateCombo(hwnd, IDC_VOLC_LANGUAGE, S(505), S(UiStyle::RowInputY(3)), S(240), S(UiStyle::ComboH)));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowInputY(4)) + S(2), S(UiStyle::LabelWidth) + S(10), S(UiStyle::LabelH), L"end_window_size");
        AddVolcengineControl(control);
        HWND volcEndWindow = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                             S(UiStyle::InputLeft) + S(10), S(UiStyle::RowInputY(4)), S(80), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_END_WINDOW_SIZE)), g_instance, nullptr);
        ApplyUiFont(volcEndWindow);
        AddVolcengineControl(volcEndWindow);
        control = CreateLabel(hwnd, S(UiStyle::InputLeft) + S(96), S(UiStyle::RowInputY(4)) + S(2), S(30), S(UiStyle::LabelH), L"ms");
        AddVolcengineControl(control);

        control = CreateLabel(hwnd, S(360), S(UiStyle::RowInputY(4)) + S(2), S(180), S(UiStyle::LabelH), L"force_to_speech_time");
        AddVolcengineControl(control);
        HWND volcForceSpeech = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                               S(550), S(UiStyle::RowInputY(4)), S(60), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_FORCE_TO_SPEECH_TIME)), g_instance, nullptr);
        ApplyUiFont(volcForceSpeech);
        AddVolcengineControl(volcForceSpeech);
        control = CreateLabel(hwnd, S(618), S(UiStyle::RowInputY(4)) + S(2), S(30), S(UiStyle::LabelH), L"ms");
        AddVolcengineControl(control);

        HWND volcDdc = CreateWindowW(L"BUTTON", L"enable_ddc", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     S(UiStyle::ContentLeft), S(UiStyle::RowInputY(5)) + S(6), S(120), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_DDC)), g_instance, nullptr);
        ApplyUiFont(volcDdc);
        AddVolcengineControl(volcDdc);

        HWND volcNonstream = CreateWindowW(L"BUTTON", L"enable_nonstream", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                           S(195), S(UiStyle::RowInputY(5)) + S(6), S(170), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_NONSTREAM)), g_instance, nullptr);
        ApplyUiFont(volcNonstream);
        AddVolcengineControl(volcNonstream);

        HWND volcMusicFc = CreateWindowW(L"BUTTON", L"enable_music_fc", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                         S(395), S(UiStyle::RowInputY(5)) + S(6), S(150), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_MUSIC_FC)), g_instance, nullptr);
        ApplyUiFont(volcMusicFc);
        AddVolcengineControl(volcMusicFc);

        HWND volcPoiFc = CreateWindowW(L"BUTTON", L"enable_poi_fc", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                       S(575), S(UiStyle::RowInputY(5)) + S(6), S(130), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_POI_FC)), g_instance, nullptr);
        ApplyUiFont(volcPoiFc);
        AddVolcengineControl(volcPoiFc);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowInputY(6)) + S(2), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Extra Params");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateButton(hwnd, IDC_VOLC_EXTRA_PARAMS, S(UiStyle::InputLeft), S(UiStyle::RowInputY(6)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Edit Params"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowInputY(7)) + S(2), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Hotwords ID");
        AddVolcengineControl(control);
        HWND volcHotwordsId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                              S(UiStyle::InputLeft), S(UiStyle::RowInputY(7)), S(260), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_HOTWORDS_ID)), g_instance, nullptr);
        ApplyUiFont(volcHotwordsId);
        AddVolcengineControl(volcHotwordsId);
        control = CreateLabel(hwnd, S(462), S(UiStyle::RowInputY(7)) + S(2), S(48), S(UiStyle::LabelH), L"Name");
        AddVolcengineControl(control);
        HWND volcHotwordsName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                S(542), S(UiStyle::RowInputY(7)), S(230), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_HOTWORDS_NAME)), g_instance, nullptr);
        ApplyUiFont(volcHotwordsName);
        AddVolcengineControl(volcHotwordsName);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowInputY(8)) + S(2), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Correct ID");
        AddVolcengineControl(control);
        HWND volcCorrectTableId = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                   S(UiStyle::InputLeft), S(UiStyle::RowInputY(8)), S(260), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_CORRECT_TABLE_ID)), g_instance, nullptr);
        ApplyUiFont(volcCorrectTableId);
        AddVolcengineControl(volcCorrectTableId);
        control = CreateLabel(hwnd, S(462), S(UiStyle::RowInputY(8)) + S(2), S(48), S(UiStyle::LabelH), L"Name");
        AddVolcengineControl(control);
        HWND volcCorrectTableName = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                                     S(542), S(UiStyle::RowInputY(8)), S(230), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_CORRECT_TABLE_NAME)), g_instance, nullptr);
        ApplyUiFont(volcCorrectTableName);
        AddVolcengineControl(volcCorrectTableName);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowInputY(9)) + S(2), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Context");
        AddVolcengineControl(control);

        HWND volcEnableContext = CreateWindowW(L"BUTTON", L"Use history as context", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                               S(UiStyle::InputLeft), S(UiStyle::RowInputY(9)) + S(6), S(210), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_CONTEXT)), g_instance, nullptr);
        ApplyUiFont(volcEnableContext);
        AddVolcengineControl(volcEnableContext);

        HWND volcContextHistory = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                                  S(UiStyle::InputLeft) + S(220), S(UiStyle::RowInputY(9)), S(44), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_CONTEXT_HISTORY)), g_instance, nullptr);
        ApplyUiFont(volcContextHistory);
        AddVolcengineControl(volcContextHistory);
        control = CreateLabel(hwnd, S(UiStyle::InputLeft) + S(270), S(UiStyle::RowInputY(9)) + S(2), S(80), S(UiStyle::LabelH), L"history");
        AddVolcengineControl(control);

        HWND volcEnableInputContext = CreateWindowW(L"BUTTON", L"Read input field context", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                     S(542), S(UiStyle::RowInputY(9)) + S(6), S(280), S(UiStyle::CheckH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_INPUT_CONTEXT)), g_instance, nullptr);
        ApplyUiFont(volcEnableInputContext);
        AddVolcengineControl(volcEnableInputContext);

        AddVolcengineControl(CreateButton(hwnd, IDC_VOLC_TEST, S(500), S(UiStyle::RowInputY(0)), S(UiStyle::ActionBtnW), S(UiStyle::ActionBtnH), L"Test Connection"));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::CloudAsrHintY), S(UiStyle::CloudAsrHintW), S(UiStyle::LabelH),
            L"Cloud ASR sends audio to remote servers. Keys are encrypted with DPAPI locally.");
        AddCloudAsrControl(control);
        g_cloudAsrHintControl = control;

        HWND status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    S(UiStyle::Margin) * 2, S(UiStyle::FooterMinTop) + S(21), S(520), S(UiStyle::LabelH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
        ApplyUiFont(status);
        CreateButton(hwnd, IDC_SAVE, S(626), S(UiStyle::FooterMinTop) + S(21), S(UiStyle::FooterBtnW), S(UiStyle::ActionBtnH), L"Save");
        CreateButton(hwnd, IDC_CANCEL, S(726), S(UiStyle::FooterMinTop) + S(21), S(UiStyle::FooterBtnW), S(UiStyle::ActionBtnH), L"Close");
        LoadSettingsControls(hwnd);
        ShowSettingsPage(hwnd, 0);
        LayoutSettingsWindow(hwnd);
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDC_MODEL:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                const std::wstring modelId = ModelIdFromIndex(ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_MODEL)));
                SetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), DefaultModelDir(modelId).c_str());
                return 0;
            }
            break;
        case IDC_QWEN_MODEL:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                if (!s_qwenUiModel.empty()) StoreQwenProfileUrl(hwnd, s_qwenUiModel);
                s_qwenUiModel = QwenModelFromControl(hwnd);
                ApplyQwenModelProfile(hwnd, s_qwenUiModel, false);
                return 0;
            }
            break;
        case IDC_QWEN_SEMANTIC_PUNCTUATION:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
                if (enabled) Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD), BST_UNCHECKED);
                EnableWindow(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD), enabled ? FALSE : TRUE);
                return 0;
            }
            break;
        case IDC_QWEN_MULTI_THRESHOLD:
            if (HIWORD(wParam) == BN_CLICKED &&
                Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED) {
                Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION), BST_UNCHECKED);
                return 0;
            }
            break;
        case IDC_QWEN_SPEECH_NOISE_ENABLE:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool streamingModel = IsQwenAudioStreamingModel(QwenModelFromControl(hwnd));
                const bool enabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED;
                EnableWindow(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD), streamingModel && enabled ? TRUE : FALSE);
                return 0;
            }
            break;
        case IDC_LLM_PROVIDER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                std::wstring prov = ComboText(GetDlgItem(hwnd, IDC_LLM_PROVIDER));
                if (!prov.empty()) {
                    g_config.llmProvider = prov;
                    int pi = FindPresetIndex(prov);
                    if (pi >= 0) {
                        ApplyPreset(pi);
                    } else {
                        LoadProviderFromStore(prov);
                    }
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_ENDPOINT), g_config.llmEndpoint.c_str());
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_KEY), g_config.llmApiKey.c_str());
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_MODEL), g_config.llmModel.c_str());
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), g_config.llmExtraParams.c_str());
                    HWND delBtn = GetDlgItem(hwnd, IDC_LLM_PROVIDER_DEL);
                    if (delBtn) EnableWindow(delBtn, FindPresetIndex(prov) < 0);
                    HWND resetBtn = GetDlgItem(hwnd, IDC_LLM_EXTRA_RESET);
                    if (resetBtn) EnableWindow(resetBtn, FindPresetIndex(prov) >= 0);
                }
                return 0;
            }
            break;
        case IDC_LLM_PROVIDER_ADD: {
            std::wstring name;
            if (ShowInputDialog(hwnd, L"Add Provider", name)) {
                bool exists = FindPresetIndex(name) >= 0;
                if (!exists) {
                    std::string json = llm::WideToUtf8(g_config.llmProvidersJson);
                    std::string key = llm::WideToUtf8(name);
                    if (json.find("\"" + key + "\"") != std::string::npos) exists = true;
                }
                if (exists) {
                    SetStatus(hwnd, L"Provider name already exists.");
                } else {
                    g_config.llmEndpoint.clear();
                    g_config.llmApiKey.clear();
                    g_config.llmModel.clear();
                    g_config.llmExtraParams.clear();
                    g_config.llmProvider = name;
                    SaveCurrentProvider();
                    RefreshProviderDropdown(hwnd);
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_ENDPOINT), L"");
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_KEY), L"");
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_MODEL), L"");
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), L"");
                    EnableWindow(GetDlgItem(hwnd, IDC_LLM_PROVIDER_DEL), TRUE);
                    SetStatus(hwnd, (L"Added provider: " + name).c_str());
                }
            }
            return 0;
        }
        case IDC_LLM_PROVIDER_DEL: {
            std::wstring prov = ComboText(GetDlgItem(hwnd, IDC_LLM_PROVIDER));
            if (prov.empty() || FindPresetIndex(prov) >= 0) return 0;
            DeleteProviderFromStore(prov);
            ApplyPreset(0);
            RefreshProviderDropdown(hwnd);
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_ENDPOINT), g_config.llmEndpoint.c_str());
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_KEY), g_config.llmApiKey.c_str());
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_MODEL), g_config.llmModel.c_str());
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), g_config.llmExtraParams.c_str());
            EnableWindow(GetDlgItem(hwnd, IDC_LLM_PROVIDER_DEL), FALSE);
            SetStatus(hwnd, (L"Deleted provider: " + prov).c_str());
            return 0;
        }
        case IDC_BROWSE:
            BrowseModelDirectory(hwnd);
            return 0;
        case IDC_DOWNLOAD_MODELS: {
            int ret = MessageBoxW(hwnd,
                L"Download ASR models? (~2.2GB)\n\n"
                L"This will open a PowerShell window.",
                L"Download Models",
                MB_YESNO | MB_ICONQUESTION);

            if (ret == IDYES) {
                if (RunModelDownloader(hwnd)) {
                    EnableWindow(GetDlgItem(hwnd, IDC_DOWNLOAD_MODELS), FALSE);
                    SetStatus(hwnd, L"Downloading... close PowerShell window when done.");
                }
            }
            return 0;
        }
        case IDC_SAVE:
            SaveSettingsControls(hwnd);
            return 0;
        case IDC_CANCEL:
            HideSettingsWindow(hwnd);
            return 0;
        case IDC_LLM_TEST:
            TestLlmConnection(hwnd);
            return 0;
        case IDC_LLM_SHOW_KEY: {
            g_llmKeyVisible = !g_llmKeyVisible;
            HWND keyEdit = GetDlgItem(hwnd, IDC_LLM_KEY);
            if (keyEdit) {
                SendMessageW(keyEdit, EM_SETPASSWORDCHAR, g_llmKeyVisible ? 0 : L'\u25CF', 0);
                InvalidateRect(keyEdit, nullptr, TRUE);
            }
            HWND btn = GetDlgItem(hwnd, IDC_LLM_SHOW_KEY);
            if (btn) SetWindowTextW(btn, g_llmKeyVisible ? L"Hide" : L"Show");
            return 0;
        }
        case IDC_QWEN_FREE_POLISH:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled =
                    Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_POLISH)) == BST_CHECKED;
                Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_PUNCT),
                                enabled ? BST_CHECKED : BST_UNCHECKED);
                Button_SetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_CORRECT),
                                enabled ? BST_CHECKED : BST_UNCHECKED);
                return 0;
            }
            break;
        case IDC_LLM_PRESET_COMBO:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_LLM_PRESET_COMBO));
                HWND promptEdit = GetDlgItem(hwnd, IDC_LLM_PROMPT);
                if (sel >= 0 && sel < llm::kPromptPresetCount) {
                    // Save custom content only when switching FROM Custom to preset
                    if (s_isCustomMode) {
                        int len = GetWindowTextLengthW(promptEdit);
                        std::wstring current(len + 1, L'\0');
                        GetWindowTextW(promptEdit, &current[0], len + 1);
                        current.resize(len);
                        s_customPromptBackup = current;
                        s_isCustomMode = false;
                    }
                    // Set preset prompt and readonly
                    SetWindowTextW(promptEdit, llm::kPromptPresets[sel].prompt);
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PRESET_DESC), llm::kPromptPresets[sel].description);
                    SendMessageW(promptEdit, EM_SETREADONLY, TRUE, 0);
                } else if (sel == llm::kPromptPresetCount) {
                    // Switch to Custom: restore backup and allow editing
                    s_isCustomMode = true;
                    SetWindowTextW(promptEdit, s_customPromptBackup.c_str());
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PRESET_DESC), L"Custom prompt");
                    SendMessageW(promptEdit, EM_SETREADONLY, FALSE, 0);
                }
            }
            return 0;
        case IDC_LLM_PROMPT:
            if (HIWORD(wParam) == EN_CHANGE) {
                HWND promptEdit = GetDlgItem(hwnd, IDC_LLM_PROMPT);
                int len = GetWindowTextLengthW(promptEdit);
                std::wstring current(len + 1, L'\0');
                GetWindowTextW(promptEdit, &current[0], len + 1);
                current.resize(len);
                HWND combo = GetDlgItem(hwnd, IDC_LLM_PRESET_COMBO);
                int matchedPreset = -1;
                for (int i = 0; i < llm::kPromptPresetCount; ++i) {
                    if (current == llm::kPromptPresets[i].prompt) {
                        matchedPreset = i;
                        break;
                    }
                }
                if (matchedPreset >= 0) {
                    ComboBox_SetCurSel(combo, matchedPreset);
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PRESET_DESC), llm::kPromptPresets[matchedPreset].description);
                } else {
                    ComboBox_SetCurSel(combo, llm::kPromptPresetCount);
                    SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PRESET_DESC), L"Custom prompt");
                }
            }
            return 0;
        case IDC_LLM_EXTRA_RESET: {
            int pi = FindPresetIndex(g_config.llmProvider);
            if (pi >= 0) {
                SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), llm::kProviderPresets[pi].extraParams);
            }
            return 0;
        }
        case IDC_ASR_BACKEND:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                return 0;
            }
            break;
        case IDC_VAD_MODEL:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int idx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VAD_MODEL));
                ShowVadSubGroup(idx);
                return 0;
            }
            break;
        case IDC_CLOUD_PROVIDER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int idx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CLOUD_PROVIDER));
                ShowCloudSubPage(hwnd, idx);
                return 0;
            }
            break;
        case IDC_VOLC_MODE:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int modeIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_MODE));
                HWND langCombo = GetDlgItem(hwnd, IDC_VOLC_LANGUAGE);
                if (langCombo) EnableWindow(langCombo, modeIdx == 0);
                EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM), modeIdx == 1);
                {
                    bool fcEnabled = (modeIdx == 0) ||
                        (modeIdx == 1 && Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM)) == BST_CHECKED);
                    EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_MUSIC_FC), fcEnabled);
                    EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_POI_FC), fcEnabled);
                }
                return 0;
            }
            break;
        case IDC_VOLC_ENABLE_NONSTREAM: {
            int modeIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_MODE));
            bool fcEnabled = (modeIdx == 0) ||
                (modeIdx == 1 && Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM)) == BST_CHECKED);
            EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_MUSIC_FC), fcEnabled);
            EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_POI_FC), fcEnabled);
            return 0;
        }
        case IDC_VOLC_EXTRA_PARAMS: {
            std::wstring params = g_config.volcExtraParams;
            if (ShowVolcExtraDialog(hwnd, params)) {
                g_config.volcExtraParams = params;
            }
            return 0;
        }
        case IDC_BAIDU_SHOW_KEY: {
            g_baiduKeyVisible = !g_baiduKeyVisible;
            HWND keyEdit = GetDlgItem(hwnd, IDC_BAIDU_SECRET_KEY);
            if (keyEdit) {
                SendMessageW(keyEdit, EM_SETPASSWORDCHAR, g_baiduKeyVisible ? 0 : L'\u25CF', 0);
                InvalidateRect(keyEdit, nullptr, TRUE);
            }
            HWND btn = GetDlgItem(hwnd, IDC_BAIDU_SHOW_KEY);
            if (btn) SetWindowTextW(btn, g_baiduKeyVisible ? L"Hide" : L"Show");
            return 0;
        }
        case IDC_BAIDU_SHOW_API_KEY: {
            g_baiduApiKeyVisible = !g_baiduApiKeyVisible;
            HWND keyEdit = GetDlgItem(hwnd, IDC_BAIDU_API_KEY);
            if (keyEdit) {
                SendMessageW(keyEdit, EM_SETPASSWORDCHAR, g_baiduApiKeyVisible ? 0 : L'\u25CF', 0);
                InvalidateRect(keyEdit, nullptr, TRUE);
            }
            HWND btn = GetDlgItem(hwnd, IDC_BAIDU_SHOW_API_KEY);
            if (btn) SetWindowTextW(btn, g_baiduApiKeyVisible ? L"Hide" : L"Show");
            return 0;
        }
        case IDC_VOLC_SHOW_KEY: {
            g_volcKeyVisible = !g_volcKeyVisible;
            HWND keyEdit = GetDlgItem(hwnd, IDC_VOLC_API_KEY);
            if (keyEdit) {
                SendMessageW(keyEdit, EM_SETPASSWORDCHAR, g_volcKeyVisible ? 0 : L'\u25CF', 0);
                InvalidateRect(keyEdit, nullptr, TRUE);
            }
            HWND btn = GetDlgItem(hwnd, IDC_VOLC_SHOW_KEY);
            if (btn) SetWindowTextW(btn, g_volcKeyVisible ? L"Hide" : L"Show");
            return 0;
        }
        case IDC_QWEN_SHOW_KEY: {
            g_qwenKeyVisible = !g_qwenKeyVisible;
            HWND keyEdit = GetDlgItem(hwnd, IDC_QWEN_API_KEY);
            if (keyEdit) {
                SendMessageW(keyEdit, EM_SETPASSWORDCHAR, g_qwenKeyVisible ? 0 : L'\u25CF', 0);
                InvalidateRect(keyEdit, nullptr, TRUE);
            }
            HWND btn = GetDlgItem(hwnd, IDC_QWEN_SHOW_KEY);
            if (btn) SetWindowTextW(btn, g_qwenKeyVisible ? L"Hide" : L"Show");
            return 0;
        }
        case IDC_MIMO_SHOW_KEY: {
            g_mimoKeyVisible = !g_mimoKeyVisible;
            HWND keyEdit = GetDlgItem(hwnd, IDC_MIMO_API_KEY);
            if (keyEdit) {
                SendMessageW(keyEdit, EM_SETPASSWORDCHAR, g_mimoKeyVisible ? 0 : L'\u25CF', 0);
                InvalidateRect(keyEdit, nullptr, TRUE);
            }
            HWND btn = GetDlgItem(hwnd, IDC_MIMO_SHOW_KEY);
            if (btn) SetWindowTextW(btn, g_mimoKeyVisible ? L"Hide" : L"Show");
            return 0;
        }
        case IDC_BAIDU_TEST: {
            baidu_asr::BaiduConfig bcfg;
            wchar_t tmp[256] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_BAIDU_API_KEY), tmp, 256);
            bcfg.apiKey = tmp;
            GetWindowTextW(GetDlgItem(hwnd, IDC_BAIDU_SECRET_KEY), tmp, 256);
            bcfg.secretKey = tmp;
            int devPidIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_BAIDU_DEV_PID));
            int pids[] = {1537, 1737, 1637, 1837};
            if (devPidIdx >= 0 && devPidIdx < 4) bcfg.devPid = pids[devPidIdx];
            SetStatus(hwnd, L"Testing Baidu ASR connection...");
            std::thread([hwnd, bcfg]() {
                baidu_asr::TestResult result = baidu_asr::TestConnection(bcfg);
                PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1,
                    reinterpret_cast<LPARAM>(new std::wstring(result.message)));
            }).detach();
            return 0;
        }
        case IDC_VOLC_TEST: {
            volc_asr::VolcConfig vcfg;
            wchar_t tmp[256] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_API_KEY), tmp, 256);
            vcfg.apiKey = tmp;
            int modeIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_MODE));
            if (modeIdx == 1) vcfg.mode = L"bigmodel_async";
            else if (modeIdx == 2) vcfg.mode = L"bigmodel";
            else vcfg.mode = L"bigmodel_nostream";
            int resIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_RESOURCE));
            if (resIdx >= 0 && resIdx < 4) vcfg.resourceId = kVolcResources[resIdx].resourceId;
            int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_LANGUAGE));
            if (langIdx >= 0 && langIdx < 9) vcfg.language = kVolcLanguages[langIdx];
            vcfg.enableMusicFc = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_MUSIC_FC)) == BST_CHECKED;
            vcfg.enablePoiFc = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_POI_FC)) == BST_CHECKED;
            {
                wchar_t hw[512] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_HOTWORDS_ID), hw, 512);
                vcfg.hotwordsId = hw;
            }
            {
                wchar_t hw[512] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_HOTWORDS_NAME), hw, 512);
                vcfg.hotwordsName = hw;
            }
            {
                wchar_t ct[512] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CORRECT_TABLE_ID), ct, 512);
                vcfg.correctTableId = ct;
            }
            {
                wchar_t ct[512] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_CORRECT_TABLE_NAME), ct, 512);
                vcfg.correctTableName = ct;
            }
            SetStatus(hwnd, L"Testing Volcano Engine ASR connection...");
            std::thread([hwnd, vcfg]() {
                volc_asr::TestResult result = volc_asr::TestConnection(vcfg);
                PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1,
                    reinterpret_cast<LPARAM>(new std::wstring(result.message)));
            }).detach();
            return 0;
        }
        case IDC_VOLC_OPEN_LOG:
            OpenAsrDebugLog(hwnd, L"volc_asr_debug.log");
            return 0;
        case IDC_QWEN_TEST: {
            const std::wstring selectedModel = QwenModelFromControl(hwnd);
            if (IsQwenAudioHttpModel(selectedModel)) {
                qwen_audio_http::Config cfg;
                wchar_t tmp[1024] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_API_KEY), tmp, 1024); cfg.apiKey = tmp;
                GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_BASE_URL), tmp, 1024); cfg.baseUrl = tmp;
                cfg.model = selectedModel;
                cfg.languageHints = NormalizeQwenLanguageHints(QwenControlText(hwnd, IDC_QWEN_LANGUAGE_HINTS));
                cfg.vocabularyId = QwenControlText(hwnd, IDC_QWEN_VOCABULARY_ID);
                cfg.vocabulary = QwenControlText(hwnd, IDC_QWEN_VOCABULARY, 4096);
                SetStatus(hwnd, L"Testing Qwen Audio HTTP connection...");
                std::thread([hwnd, cfg]() {
                    auto result = qwen_audio_http::TestConnection(cfg);
                    PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1, reinterpret_cast<LPARAM>(new std::wstring(result.message)));
                }).detach();
                return 0;
            }
            if (IsQwenAudioStreamingModel(selectedModel)) {
                qwen_audio_streaming::Config cfg;
                wchar_t tmp[1024] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_API_KEY), tmp, 1024); cfg.apiKey = tmp;
                GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_BASE_URL), tmp, 1024); cfg.baseUrl = tmp;
                cfg.model = selectedModel;
                cfg.languageHints = NormalizeQwenLanguageHints(QwenControlText(hwnd, IDC_QWEN_LANGUAGE_HINTS));
                cfg.vocabularyId = QwenControlText(hwnd, IDC_QWEN_VOCABULARY_ID);
                cfg.vocabulary = QwenControlText(hwnd, IDC_QWEN_VOCABULARY, 4096);
                cfg.semanticPunctuation = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SEMANTIC_PUNCTUATION)) == BST_CHECKED;
                cfg.multiThresholdMode = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_MULTI_THRESHOLD)) == BST_CHECKED;
                cfg.heartbeat = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_HEARTBEAT)) == BST_CHECKED;
                cfg.speechNoiseThresholdEnabled = Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_SPEECH_NOISE_ENABLE)) == BST_CHECKED;
                cfg.speechNoiseThreshold = std::clamp(static_cast<float>(_wtof(QwenControlText(hwnd, IDC_QWEN_SPEECH_NOISE_THRESHOLD, 32).c_str())), -1.0f, 1.0f);
                cfg.maxSentenceSilenceMs = std::clamp(_wtoi(QwenControlText(hwnd, IDC_QWEN_MAX_SENTENCE_SILENCE, 32).c_str()), 200, 6000);
                SetStatus(hwnd, L"Testing Qwen Audio streaming connection...");
                std::thread([hwnd, cfg]() {
                    auto result = qwen_audio_streaming::TestConnection(cfg);
                    PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1, reinterpret_cast<LPARAM>(new std::wstring(result.message)));
                }).detach();
                return 0;
            }
            qwen_asr::QwenConfig qcfg;
            wchar_t tmp[512] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_API_KEY), tmp, 512);
            qcfg.apiKey = tmp;
            GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_BASE_URL), tmp, 512);
            qcfg.baseUrl = tmp[0] ? tmp : qwen_asr::kDefaultBaseUrl;
            wchar_t model[256] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MODEL), model, 256);
            qcfg.model = model[0] ? model : qwen_asr::kDefaultModel;
            int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_QWEN_LANGUAGE));
            qcfg.language = QwenLanguageCodeFromIndex(langIdx);
            qcfg.turnDetection = L"manual";
            {
                wchar_t buf[32] = {};
                GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_CHUNK_MS), buf, 32);
                qcfg.chunkMs = std::clamp(_wtoi(buf), 20, 1000);
            }

            SetStatus(hwnd, L"Testing Qwen ASR connection...");
            std::thread([hwnd, qcfg]() {
                qwen_asr::TestResult result = qwen_asr::TestConnection(qcfg);
                PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1,
                    reinterpret_cast<LPARAM>(new std::wstring(result.message)));
            }).detach();
            return 0;
        }
        case IDC_QWEN_OPEN_LOG:
            OpenAsrDebugLog(hwnd, L"qwen_audio_debug.log");
            return 0;
        case IDC_QWEN_LANGUAGE_HINTS_RESET:
            SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_LANGUAGE_HINTS), kQwenDefaultLanguageHints);
            SetStatus(hwnd, L"Language hints reset to zh,en,yue. Click Save to apply.");
            return 0;
        case IDC_QWEN_ADVANCED:
            EditQwenAdvancedSettings(hwnd);
            return 0;
        case IDC_MIMO_TEST: {
            mimo_asr::MimoConfig mcfg;
            wchar_t tmp[512] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_API_KEY), tmp, 512);
            mcfg.apiKey = tmp;
            GetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_BASE_URL), tmp, 512);
            mcfg.baseUrl = tmp[0] ? tmp : mimo_asr::kDefaultBaseUrl;
            wchar_t model[256] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_MODEL), model, 256);
            mcfg.model = model[0] ? model : mimo_asr::kDefaultModel;
            int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_MIMO_LANGUAGE));
            mcfg.language = MimoLanguageCodeFromIndex(langIdx);

            SetStatus(hwnd, L"Testing MiMo ASR connection...");
            std::thread([hwnd, mcfg]() {
                mimo_asr::TestResult result = mimo_asr::TestConnection(mcfg);
                PostMessageW(hwnd, WM_APP + 10, result.ok ? 0 : 1,
                    reinterpret_cast<LPARAM>(new std::wstring(result.message)));
            }).detach();
            return 0;
        }
        case IDC_DOUBAO_IME_TEST: {
            doubao_ime_asr::DoubaoImeConfig dcfg;
            dcfg.deviceId = g_config.doubaoImeDeviceId;
            dcfg.cdid = g_config.doubaoImeCdid;
            dcfg.token = g_config.doubaoImeToken;
            const uint64_t generation = g_doubaoImeTestGeneration.fetch_add(1) + 1;
            EnableWindow(GetDlgItem(hwnd, IDC_DOUBAO_IME_TEST), FALSE);

            SetStatus(hwnd, L"Testing Doubao IME ASR connection...");
            std::thread([hwnd, dcfg, generation]() {
                auto* msg = new DoubaoImeTestMessage;
                msg->generation = generation;
                msg->result = doubao_ime_asr::TestConnection(dcfg);
                if (!PostMessageW(hwnd, kDoubaoImeTestResultMessage, msg->result.ok ? 0 : 1,
                                  reinterpret_cast<LPARAM>(msg))) {
                    delete msg;
                }
            }).detach();
            return 0;
        }
        case IDC_DOUBAO_IME_RESET:
            g_doubaoImeTestGeneration.fetch_add(1);
            EnableWindow(GetDlgItem(hwnd, IDC_DOUBAO_IME_TEST), TRUE);
            g_config.doubaoImeDeviceId.clear();
            g_config.doubaoImeCdid.clear();
            g_config.doubaoImeToken.clear();
            SaveConfig();
            RefreshDoubaoImeStatus(hwnd);
            SetStatus(hwnd, L"Doubao IME credentials reset.");
            return 0;
        case IDC_QWEN_FREE_BROWSE: {
            wchar_t buffer[MAX_PATH] = {};
            BROWSEINFOW bi{};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = buffer;
            bi.lpszTitle = L"Select Qianwen IME voice directory (qianwen_shell_*)";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t pathBuffer[MAX_PATH] = {};
                if (SHGetPathFromIDListW(pidl, pathBuffer)) {
                    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_FREE_SHELL_PATH), pathBuffer);
                }
                CoTaskMemFree(pidl);
            }
            return 0;
        }
        case IDC_QWEN_FREE_TEST: {
            const uint64_t generation =
                g_qwenFreeTestGeneration.fetch_add(1, std::memory_order_relaxed) + 1;
            EnableWindow(GetDlgItem(hwnd, IDC_QWEN_FREE_TEST), FALSE);
            wchar_t shellPathBuffer[1024] = {};
            GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_FREE_SHELL_PATH), shellPathBuffer, 1024);
            const std::wstring shellPath = shellPathBuffer;
            const std::wstring utdidOverride = g_config.qwenFreeUtdidOverride;
            // Test the values currently visible in Settings.  The user may
            // intentionally probe a changed checkbox state before pressing
            // Save; using g_config here would silently test the previous
            // persisted state and could skip (or unexpectedly add) the LLM
            // probe.
            const bool llmRequired =
                Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_POLISH)) == BST_CHECKED ||
                Button_GetCheck(GetDlgItem(hwnd, IDC_QWEN_FREE_REWRITE)) == BST_CHECKED;
            SetStatus(hwnd, llmRequired
                ? L"Testing Qianwen IME (Free) UTDID, ASR and LLM..."
                : L"Testing Qianwen IME (Free) UTDID and ASR...");
            std::thread([hwnd, generation, shellPath, utdidOverride, llmRequired]() {
                auto* msg = new QwenFreeTestMessage;
                msg->generation = generation;
                auto utdid = qwen_free_proto_utdid::GetUtdid(utdidOverride, shellPath);
                if (!utdid.ok) {
                    msg->result.message = L"UTDID acquisition failed: " + utdid.error;
                } else {
                    std::wstring unetError;
                    if (!qwen_free_proto_unet::Initialize(shellPath, unetError)) {
                        msg->result.message =
                            L"Native signer initialization failed: " + unetError;
                    } else {
                        qwen_free_proto_asr::AsrConfig cfg;
                        cfg.utdid = utdid.utdid;
                        msg->result = qwen_free_proto_asr::TestConnection(cfg);
                        msg->asrOk = msg->result.ok;
                    }
                    if (msg->result.ok) {
                        if (llmRequired) {
                            qwen_free_proto_llm::LlmConfig llmCfg;
                            llmCfg.utdid = utdid.utdid;
                            llmCfg.shellPath = shellPath;
                            const auto llm = qwen_free_proto_llm::PolishText(
                                llmCfg, L"连接测试", 0.0, 0);
                            msg->llmOk = llm.ok;
                            msg->llmElapsedMs = llm.elapsedMs;
                            msg->llmMessage = llm.ok
                                ? L"LLM OK (" + std::to_wstring(llm.elapsedMs) + L"ms)"
                                : L"LLM unavailable: " +
                                  (llm.error.empty() ? L"unknown error" : llm.error);
                            if (!msg->llmOk) {
                                // ASR 已通过，但启用的后处理不可用；不要把整体探测误报成 OK。
                                msg->result.ok = false;
                            }
                        } else {
                            msg->llmOk = true;
                            msg->llmMessage = L"LLM skipped (post-processing disabled)";
                        }
                        msg->result.message = L"UTDID OK (" + utdid.source + L"); " +
                                              msg->result.message + L" (" +
                                              std::to_wstring(msg->result.elapsedMs) + L"ms); " +
                                              msg->llmMessage;
                    }
                }
                if (!PostMessageW(hwnd, kQwenFreeTestResultMessage,
                                  msg->result.ok ? 0 : 1,
                                  reinterpret_cast<LPARAM>(msg))) {
                    delete msg;
                }
            }).detach();
            return 0;
        }
        default:
            break;
        }
        return 0;
    case WM_NOTIFY: {
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->idFrom == IDC_SETTINGS_TAB && hdr->code == TCN_SELCHANGE) {
            ShowSettingsPage(hwnd, TabCtrl_GetCurSel(GetDlgItem(hwnd, IDC_SETTINGS_TAB)));
            return 0;
        }
        return 0;
    }
    case WM_APP + 10: {
        std::unique_ptr<std::wstring> msg(reinterpret_cast<std::wstring*>(lParam));
        if (wParam == 0) {
            SetStatus(hwnd, msg ? msg->c_str() : L"OK");
        } else {
            std::wstring shortMsg = L"Connection failed";
            if (msg) {
                size_t nl = msg->find(L'\n');
                shortMsg = L"Connection failed: " + (nl != std::wstring::npos ? msg->substr(0, nl) : *msg);
                MessageBoxW(hwnd, msg->c_str(), L"Connection Test Failed",
                            MB_ICONERROR | MB_OK);
            }
            SetStatus(hwnd, shortMsg.c_str());
        }
        return 0;
    }
    case kQwenFreeTestResultMessage: {
        std::unique_ptr<QwenFreeTestMessage> msg(reinterpret_cast<QwenFreeTestMessage*>(lParam));
        if (!msg || msg->generation !=
                        g_qwenFreeTestGeneration.load(std::memory_order_relaxed)) {
            return 0;
        }
        EnableWindow(GetDlgItem(hwnd, IDC_QWEN_FREE_TEST), TRUE);
        RefreshQwenFreeStatus(hwnd);
        if (msg->result.ok) {
            SetStatus(hwnd, msg->result.message);
        } else {
            const std::wstring detail = !msg->result.message.empty()
                ? msg->result.message
                : L"Qwen IME ASR connection failed.";
            const bool llmFailure = msg->asrOk && !msg->llmOk;
            SetStatus(hwnd, llmFailure
                ? L"Connection failed: Qwen IME (Free) LLM"
                : L"Connection failed: Qwen IME (Free) ASR");
            MessageBoxW(hwnd, detail.c_str(), L"Connection Test Failed", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case kQwenFreeStatusResultMessage: {
        std::unique_ptr<QwenFreeStatusMessage> message(
            reinterpret_cast<QwenFreeStatusMessage*>(lParam));
        if (message &&
            message->generation ==
                g_qwenFreeStatusGeneration.load(std::memory_order_relaxed)) {
            HWND status = GetDlgItem(hwnd, IDC_QWEN_FREE_STATUS);
            if (status) SetWindowTextW(status, message->text.c_str());
        }
        return 0;
    }
    case kDoubaoImeTestResultMessage: {
        std::unique_ptr<DoubaoImeTestMessage> msg(reinterpret_cast<DoubaoImeTestMessage*>(lParam));
        if (!msg || msg->generation != g_doubaoImeTestGeneration.load()) {
            return 0;
        }
        EnableWindow(GetDlgItem(hwnd, IDC_DOUBAO_IME_TEST), TRUE);
        if (msg && msg->result.credentialsChanged) {
            g_config.doubaoImeDeviceId = msg->result.credentials.deviceId;
            g_config.doubaoImeCdid = msg->result.credentials.cdid;
            g_config.doubaoImeToken = msg->result.credentials.token;
            SaveConfig();
            RefreshDoubaoImeStatus(hwnd);
        }
        if (msg && msg->result.ok) {
            std::wstring status = msg->result.message.empty() ? std::wstring(L"Connection OK.") : msg->result.message;
            SetStatus(hwnd, status);
        } else {
            std::wstring detail = msg ? msg->result.message : L"Connection failed";
            SetStatus(hwnd, L"Connection failed: Doubao IME ASR");
            MessageBoxW(hwnd, detail.c_str(), L"Connection Test Failed", MB_ICONERROR | MB_OK);
        }
        return 0;
    }
    case WM_APP + 20: {
        EnableWindow(GetDlgItem(hwnd, IDC_DOWNLOAD_MODELS), TRUE);
        if (lParam) {
            std::unique_ptr<std::wstring> dir(reinterpret_cast<std::wstring*>(lParam));
            std::wstring newDir = std::move(*dir);
            SetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), newDir.c_str());
            g_config.modelDir = newDir;
            SetStatus(hwnd, L"Download complete");
            MessageBoxW(hwnd, L"Download complete!", L"Success", MB_OK | MB_ICONINFORMATION);
        } else {
            SetStatus(hwnd, L"Download failed or model directory not found");
        }
        return 0;
    }
    case WM_DPICHANGED:
        UpdateUiScale(hwnd);
        {
            RECT* suggested = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                         suggested->right - suggested->left,
                         suggested->bottom - suggested->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutSettingsWindow(hwnd);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_DESTROY:
        g_qwenFreeTestGeneration.fetch_add(1, std::memory_order_relaxed);
        g_qwenFreeStatusGeneration.fetch_add(1, std::memory_order_relaxed);
        s_qwenChunkContextHint = nullptr;
        if (g_settingsWindow == hwnd) g_settingsWindow = nullptr;
        return 0;
    case WM_CLOSE:
        HideSettingsWindow(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void ShowSettingsWindow(HWND owner) {
    UninstallKeyboardHook();
    if (!g_settingsWindow) {
        UpdateUiScale(nullptr);
        g_settingsWindow = CreateWindowExW(
            WS_EX_APPWINDOW,
            kSettingsClass,
            L"VoxType Settings",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            S(850),
            S(720),
            owner,
            nullptr,
            g_instance,
            nullptr);
    }
    RefreshStartupRegistrationControl(g_settingsWindow, true);
    RECT rc;
    GetWindowRect(g_settingsWindow, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT work;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = work.left + (work.right - work.left - w) / 2;
    int y = work.top + (work.bottom - work.top - h) / 2;
    SetWindowPos(g_settingsWindow, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(g_settingsWindow, SW_SHOW);
    SetForegroundWindow(g_settingsWindow);
}
