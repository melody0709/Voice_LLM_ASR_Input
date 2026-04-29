#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <commctrl.h>
#include <d2d1.h>
#include <dwrite.h>
#include <windowsx.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "resource.h"

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr wchar_t kAppName[] = L"Voice LLM ASR Input";
constexpr wchar_t kMainClass[] = L"VoiceLLMASRInput.Main";
constexpr wchar_t kSettingsClass[] = L"VoiceLLMASRInput.Settings";
constexpr wchar_t kHudClass[] = L"VoiceLLMASRInput.Hud";
constexpr wchar_t kHotkeyEditClass[] = L"VoiceLLMASRInput.HotkeyEdit";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kReloadMessage = WM_APP + 2;
constexpr UINT kAsrResultMessage = WM_APP + 3;
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kHudHideTimer = 1;
constexpr UINT_PTR kCapsLockLongPressTimer = 2;
constexpr UINT_PTR kHudAnimationTimer = 3;
constexpr UINT kCapsLockLongPressMs = 300;
constexpr int kWorkerPort = 18088;
constexpr int kHudMinWidth = 300;
constexpr int kHudMinHeight = 56;
constexpr int kHudScreenMarginX = 80;
constexpr int kHudScreenMarginY = 96;
constexpr float kHudLeftPad = 22.0f;
constexpr float kHudWaveWidth = 52.0f;
constexpr float kHudGap = 14.0f;
constexpr float kHudRightPad = 22.0f;
constexpr float kHudTextSlack = 18.0f;

constexpr UINT ID_TRAY_VERSION = 1001;
constexpr UINT ID_TRAY_SETTINGS = 1002;
constexpr UINT ID_TRAY_RELOAD = 1003;
constexpr UINT ID_TRAY_QUIT = 1004;

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

struct Config {
    std::wstring modelId = L"firered_ctc";
    std::wstring modelDir;
    std::wstring threads = L"auto";
    bool enableVad = true;
    bool enablePartial = true;
    std::wstring postprocess = L"itn";
    std::wstring hotkey = L"CapsLock";
};

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
std::mutex g_workerMutex;
PROCESS_INFORMATION g_workerProcess = {};
bool g_workerStarted = false;
std::vector<HWND> g_recognitionControls;
std::vector<HWND> g_shortcutControls;

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

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    return result;
}

std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    return result;
}

std::string EscapeJson(const std::wstring& value) {
    std::string utf8 = WideToUtf8(value);
    std::string out;
    out.reserve(utf8.size() + 8);
    for (char c : utf8) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += c; break;
        }
    }
    return out;
}

bool IsModifierKey(UINT vk) {
    return vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL ||
           vk == VK_MENU || vk == VK_LMENU || vk == VK_RMENU ||
           vk == VK_SHIFT || vk == VK_LSHIFT || vk == VK_RSHIFT ||
           vk == VK_LWIN || vk == VK_RWIN;
}

UINT NormalizedKeyFromWParam(WPARAM wParam) {
    UINT vk = static_cast<UINT>(wParam);
    UINT ch = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR);
    if (ch != 0) {
        UINT key = ch & 0xFFFF;
        if (key >= L'a' && key <= L'z') key -= 32;
        if ((key >= L'A' && key <= L'Z') || (key >= L'0' && key <= L'9')) return key;
    }
    return vk;
}

std::wstring KeyName(UINT key) {
    if (key >= L'A' && key <= L'Z') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= L'0' && key <= L'9') return std::wstring(1, static_cast<wchar_t>(key));
    if (key >= VK_F1 && key <= VK_F24) return L"F" + std::to_wstring(key - VK_F1 + 1);
    switch (key) {
    case VK_CAPITAL: return L"CapsLock";
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Delete";
    case VK_INSERT: return L"Insert";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_LEFT: return L"Left";
    case VK_UP: return L"Up";
    case VK_RIGHT: return L"Right";
    case VK_DOWN: return L"Down";
    case VK_OEM_1: return L";";
    case VK_OEM_PLUS: return L"=";
    case VK_OEM_COMMA: return L",";
    case VK_OEM_MINUS: return L"-";
    case VK_OEM_PERIOD: return L".";
    case VK_OEM_2: return L"/";
    case VK_OEM_3: return L"`";
    case VK_OEM_4: return L"[";
    case VK_OEM_5: return L"\\";
    case VK_OEM_6: return L"]";
    case VK_OEM_7: return L"'";
    default:
        wchar_t buf[16] = {};
        swprintf_s(buf, L"0x%02X", key);
        return buf;
    }
}

std::wstring HotkeyToString(const HotkeyConfig& hotkey) {
    if (hotkey.IsEmpty()) return L"(None)";
    std::wstring result;
    if (hotkey.ctrl) result += L"Ctrl + ";
    if (hotkey.alt) result += L"Alt + ";
    if (hotkey.shift) result += L"Shift + ";
    if (hotkey.win) result += L"Win + ";
    result += KeyName(hotkey.key);
    return result;
}

bool EqualsIgnoreCase(std::wstring a, std::wstring b) {
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return a == b;
}

std::wstring Trim(std::wstring value) {
    const size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

HotkeyConfig HotkeyFromString(const std::wstring& text) {
    HotkeyConfig hotkey;
    hotkey.key = 0;
    if (Trim(text).empty() || EqualsIgnoreCase(Trim(text), L"(None)")) return hotkey;
    size_t start = 0;
    while (start <= text.size()) {
        size_t plus = text.find(L'+', start);
        std::wstring token = Trim(text.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        if (EqualsIgnoreCase(token, L"Ctrl") || EqualsIgnoreCase(token, L"Control")) hotkey.ctrl = true;
        else if (EqualsIgnoreCase(token, L"Alt")) hotkey.alt = true;
        else if (EqualsIgnoreCase(token, L"Shift")) hotkey.shift = true;
        else if (EqualsIgnoreCase(token, L"Win") || EqualsIgnoreCase(token, L"Windows")) hotkey.win = true;
        else if (EqualsIgnoreCase(token, L"CapsLock")) hotkey.key = VK_CAPITAL;
        else if (EqualsIgnoreCase(token, L"Space")) hotkey.key = VK_SPACE;
        else if (EqualsIgnoreCase(token, L"Tab")) hotkey.key = VK_TAB;
        else if (EqualsIgnoreCase(token, L"Enter")) hotkey.key = VK_RETURN;
        else if (EqualsIgnoreCase(token, L"Esc") || EqualsIgnoreCase(token, L"Escape")) hotkey.key = VK_ESCAPE;
        else if (EqualsIgnoreCase(token, L"Backspace")) hotkey.key = VK_BACK;
        else if (EqualsIgnoreCase(token, L"Delete")) hotkey.key = VK_DELETE;
        else if (EqualsIgnoreCase(token, L"Insert")) hotkey.key = VK_INSERT;
        else if (EqualsIgnoreCase(token, L"Home")) hotkey.key = VK_HOME;
        else if (EqualsIgnoreCase(token, L"End")) hotkey.key = VK_END;
        else if (EqualsIgnoreCase(token, L"Page Up")) hotkey.key = VK_PRIOR;
        else if (EqualsIgnoreCase(token, L"Page Down")) hotkey.key = VK_NEXT;
        else if (EqualsIgnoreCase(token, L"Left")) hotkey.key = VK_LEFT;
        else if (EqualsIgnoreCase(token, L"Up")) hotkey.key = VK_UP;
        else if (EqualsIgnoreCase(token, L"Right")) hotkey.key = VK_RIGHT;
        else if (EqualsIgnoreCase(token, L"Down")) hotkey.key = VK_DOWN;
        else if (token.size() == 1) {
            wchar_t c = token[0];
            if (c >= L'a' && c <= L'z') c = static_cast<wchar_t>(c - 32);
            hotkey.key = static_cast<UINT>(c);
        } else if (token.size() > 1 && (token[0] == L'F' || token[0] == L'f')) {
            int f = _wtoi(token.c_str() + 1);
            if (f >= 1 && f <= 24) hotkey.key = VK_F1 + f - 1;
        }
        if (plus == std::wstring::npos) break;
        start = plus + 1;
    }
    return hotkey;
}

HotkeyConfig CurrentConfiguredHotkey() {
    return HotkeyFromString(g_config.hotkey);
}

bool ModifiersMatch(const HotkeyConfig& hotkey) {
    const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    return ctrl == hotkey.ctrl && alt == hotkey.alt && shift == hotkey.shift && win == hotkey.win;
}

std::wstring AppDataDir() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        result = path;
        CoTaskMemFree(path);
    }
    if (result.empty()) {
        wchar_t fallback[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, fallback, MAX_PATH);
        result = fallback;
        const size_t slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos) result.resize(slash);
    }
    result += L"\\VoiceLLMASRInput";
    CreateDirectoryW(result.c_str(), nullptr);
    return result;
}

std::wstring ConfigPath() {
    return AppDataDir() + L"\\config.json";
}

std::wstring AppRootDir() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir = modulePath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    if (dir.size() >= 6 && dir.substr(dir.size() - 6) == L"\\build") {
        dir.resize(dir.size() - 6);
    }
    return dir;
}

std::wstring DefaultModelDir(const std::wstring& modelId) {
    const std::wstring base = AppRootDir() + L"\\models\\";
    if (modelId == L"firered_aed") {
        return base + L"sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26";
    }
    if (modelId == L"sensevoice") {
        return base + L"sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17";
    }
    return base + L"sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25";
}

std::wstring TempWavPath() {
    return AppDataDir() + L"\\last_recording.wav";
}

std::wstring QuoteArg(const std::wstring& value) {
    std::wstring out = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'"') out += L"\\\"";
        else out += ch;
    }
    out += L"\"";
    return out;
}

std::wstring ExtractJsonString(const std::string& json, const std::string& key, const std::wstring& fallback) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return fallback;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return fallback;
    std::string value;
    bool escape = false;
    for (++pos; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escape) {
            switch (c) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(c); break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            break;
        } else {
            value.push_back(c);
        }
    }
    return Utf8ToWide(value);
}

bool ExtractJsonBool(const std::string& json, const std::string& key, bool fallback) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return fallback;
    const size_t valueStart = json.find_first_not_of(" \t\r\n", pos + 1);
    if (valueStart == std::string::npos) return fallback;
    if (json.compare(valueStart, 4, "true") == 0) return true;
    if (json.compare(valueStart, 5, "false") == 0) return false;
    return fallback;
}

void LoadConfig() {
    std::ifstream file(ConfigPath(), std::ios::binary);
    if (!file) return;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();
    g_config.modelId = ExtractJsonString(json, "model_id", g_config.modelId);
    g_config.modelDir = ExtractJsonString(json, "model_dir", g_config.modelDir);
    g_config.threads = ExtractJsonString(json, "threads", g_config.threads);
    g_config.enableVad = ExtractJsonBool(json, "enable_vad", g_config.enableVad);
    g_config.enablePartial = ExtractJsonBool(json, "enable_partial", g_config.enablePartial);
    g_config.postprocess = ExtractJsonString(json, "postprocess", g_config.postprocess);
    g_config.hotkey = ExtractJsonString(json, "hotkey", g_config.hotkey);
    if (g_config.modelDir.empty()) {
        g_config.modelDir = DefaultModelDir(g_config.modelId);
    }
}

void SaveConfig() {
    std::ofstream file(ConfigPath(), std::ios::binary | std::ios::trunc);
    file << "{\n"
         << "  \"model_id\": \"" << EscapeJson(g_config.modelId) << "\",\n"
         << "  \"model_dir\": \"" << EscapeJson(g_config.modelDir) << "\",\n"
         << "  \"threads\": \"" << EscapeJson(g_config.threads) << "\",\n"
         << "  \"enable_vad\": " << (g_config.enableVad ? "true" : "false") << ",\n"
         << "  \"enable_partial\": " << (g_config.enablePartial ? "true" : "false") << ",\n"
         << "  \"postprocess\": \"" << EscapeJson(g_config.postprocess) << "\",\n"
         << "  \"hotkey\": \"" << EscapeJson(g_config.hotkey) << "\"\n"
         << "}\n";
}

std::wstring ModelDisplayName(const std::wstring& modelId) {
    if (modelId == L"firered_aed") return L"FireRedASR2 AED";
    if (modelId == L"sensevoice") return L"SenseVoiceSmall";
    return L"FireRedASR2 CTC";
}

int ModelIndex(const std::wstring& modelId) {
    if (modelId == L"firered_aed") return 1;
    if (modelId == L"sensevoice") return 2;
    return 0;
}

std::wstring ModelIdFromIndex(int index) {
    if (index == 1) return L"firered_aed";
    if (index == 2) return L"sensevoice";
    return L"firered_ctc";
}

HFONT MakeFont(int pointSize, int weight) {
    HDC hdc = GetDC(nullptr);
    const int height = -MulDiv(pointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);
    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                       OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                       DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
}

void CreateUiResources() {
    if (!g_uiFont) g_uiFont = MakeFont(9, FW_NORMAL);
    if (!g_titleFont) g_titleFont = MakeFont(14, FW_SEMIBOLD);
    if (!g_sectionFont) g_sectionFont = MakeFont(9, FW_SEMIBOLD);
    if (!g_settingsBgBrush) g_settingsBgBrush = CreateSolidBrush(RGB(246, 248, 251));
    if (!g_cardBrush) g_cardBrush = CreateSolidBrush(RGB(255, 255, 255));
    if (!g_controlBgBrush) g_controlBgBrush = CreateSolidBrush(RGB(255, 255, 255));
    if (!g_d2dFactory) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_d2dFactory);
    }
    if (!g_dwriteFactory) {
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(&g_dwriteFactory));
    }
    if (g_dwriteFactory && !g_hudTextFormat) {
        if (SUCCEEDED(g_dwriteFactory->CreateTextFormat(
                L"Segoe UI",
                nullptr,
                DWRITE_FONT_WEIGHT_SEMI_BOLD,
                DWRITE_FONT_STYLE_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL,
                15.0f,
                L"",
                &g_hudTextFormat))) {
            g_hudTextFormat->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            g_hudTextFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            g_hudTextFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }
}

void DeleteUiResources() {
    SafeRelease(g_hudBrush);
    SafeRelease(g_hudRenderTarget);
    SafeRelease(g_hudTextFormat);
    SafeRelease(g_dwriteFactory);
    SafeRelease(g_d2dFactory);
    if (g_uiFont) DeleteObject(g_uiFont);
    if (g_titleFont) DeleteObject(g_titleFont);
    if (g_sectionFont) DeleteObject(g_sectionFont);
    if (g_settingsBgBrush) DeleteObject(g_settingsBgBrush);
    if (g_cardBrush) DeleteObject(g_cardBrush);
    if (g_controlBgBrush) DeleteObject(g_controlBgBrush);
    g_uiFont = nullptr;
    g_titleFont = nullptr;
    g_sectionFont = nullptr;
    g_settingsBgBrush = nullptr;
    g_cardBrush = nullptr;
    g_controlBgBrush = nullptr;
}

void SetStatus(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS), text.c_str());
}

void AddTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = kTrayMessage;
    nid.hIcon = g_appIcon;
    wcscpy_s(nid.szTip, L"Voice LLM ASR Input");
    Shell_NotifyIconW(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &nid);
}

float CalculateAudioLevel(const BYTE* data, DWORD bytes) {
    if (!data || bytes < sizeof(int16_t)) return 0.0f;

    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const size_t count = bytes / sizeof(int16_t);
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double v = static_cast<double>(samples[i]) / 32768.0;
        sum += v * v;
    }

    const double rms = std::sqrt(sum / static_cast<double>(count));
    const double db = 20.0 * std::log10(std::max(rms, 1e-6));
    const double normalized = (db + 50.0) / 40.0;
    return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

float CurrentHudLevel() {
    const float raw = g_recording ? g_audioLevel.load() : 0.0f;
    const float factor = raw > g_hudSmoothedLevel ? 0.4f : 0.15f;
    g_hudSmoothedLevel += (raw - g_hudSmoothedLevel) * factor;
    return std::clamp(g_hudSmoothedLevel, 0.0f, 1.0f);
}

float DpiScaleForWindow(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) dpi = GetDpiForSystem();
    return static_cast<float>(dpi) / 96.0f;
}

int DipToPx(float value, float scale) {
    return static_cast<int>(std::ceil(value * scale));
}

DWRITE_TEXT_METRICS MeasureHudText(const std::wstring& text, float maxWidth, DWRITE_WORD_WRAPPING wrapping) {
    DWRITE_TEXT_METRICS metrics = {};
    if (!g_dwriteFactory || !g_hudTextFormat || text.empty()) return metrics;

    IDWriteTextLayout* layout = nullptr;
    const HRESULT hr = g_dwriteFactory->CreateTextLayout(
        text.c_str(),
        static_cast<UINT32>(text.size()),
        g_hudTextFormat,
        maxWidth,
        1000.0f,
        &layout);
    if (FAILED(hr) || !layout) return metrics;

    layout->SetWordWrapping(wrapping);
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics;
}

HudSize IdealHudSize(const std::wstring& text, const RECT& workArea, float scale) {
    const float textX = kHudLeftPad + kHudWaveWidth + kHudGap;
    const float workWidthDip = static_cast<float>(std::max(1L, workArea.right - workArea.left)) / scale;
    const float workHeightDip = static_cast<float>(std::max(1L, workArea.bottom - workArea.top)) / scale;
    const float maxWidthDip = std::max(static_cast<float>(kHudMinWidth), workWidthDip - static_cast<float>(kHudScreenMarginX));
    const float maxHeightDip = std::max(static_cast<float>(kHudMinHeight), workHeightDip - static_cast<float>(kHudScreenMarginY));
    const float maxTextWidth = maxWidthDip - textX - kHudRightPad;
    const DWRITE_TEXT_METRICS singleLineMetrics =
        MeasureHudText(text, 4096.0f, DWRITE_WORD_WRAPPING_NO_WRAP);
    const float singleLineWidthDip =
        textX + singleLineMetrics.widthIncludingTrailingWhitespace + kHudRightPad + kHudTextSlack;

    if (singleLineWidthDip <= maxWidthDip) {
        return {
            std::clamp(singleLineWidthDip, static_cast<float>(kHudMinWidth), maxWidthDip),
            static_cast<float>(kHudMinHeight),
        };
    }

    const DWRITE_TEXT_METRICS metrics = MeasureHudText(text, maxTextWidth, DWRITE_WORD_WRAPPING_WRAP);

    const float measuredWidthDip = std::max(static_cast<float>(kHudMinWidth), maxWidthDip);
    const float measuredHeightDip = metrics.height + 30.0f;
    return {
        std::clamp(measuredWidthDip, static_cast<float>(kHudMinWidth), maxWidthDip),
        std::clamp(measuredHeightDip, static_cast<float>(kHudMinHeight), maxHeightDip),
    };
}

void RemoveTrayIcon(HWND hwnd) {
    NOTIFYICONDATAW nid = {};
    nid.cbSize = sizeof(nid);
    nid.hWnd = hwnd;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

void PositionHud(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(monitor, &mi);
    const float scale = DpiScaleForWindow(hwnd);
    const HudSize hud = IdealHudSize(g_hudText, mi.rcWork, scale);
    const int width = DipToPx(hud.widthDip, scale);
    const int height = DipToPx(hud.heightDip, scale);
    const int x = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - width) / 2;
    const int y = mi.rcWork.bottom - height - 48;
    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, height, height);
    if (region && !SetWindowRgn(hwnd, region, TRUE)) {
        DeleteObject(region);
    }
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW | SWP_NOACTIVATE);
}

void ShowHud(const std::wstring& text) {
    g_hudText = text;
    if (!g_hudWindow) {
        g_hudWindow = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED,
            kHudClass,
            kAppName,
            WS_POPUP,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            kHudMinWidth,
            kHudMinHeight,
            nullptr,
            nullptr,
            g_instance,
            nullptr);
        SetLayeredWindowAttributes(g_hudWindow, 0, 242, LWA_ALPHA);
    }
    PositionHud(g_hudWindow);
    if (g_recording) {
        SetTimer(g_hudWindow, kHudAnimationTimer, 33, nullptr);
    } else {
        KillTimer(g_hudWindow, kHudAnimationTimer);
    }
    InvalidateRect(g_hudWindow, nullptr, TRUE);
}

void HideHud() {
    if (g_hudWindow) {
        KillTimer(g_hudWindow, kHudAnimationTimer);
        ShowWindow(g_hudWindow, SW_HIDE);
    }
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

void CALLBACK WaveInProc(HWAVEIN waveIn, UINT msg, DWORD_PTR, DWORD_PTR param1, DWORD_PTR) {
    if (msg != WIM_DATA || waveIn != g_waveIn) return;
    auto* header = reinterpret_cast<WAVEHDR*>(param1);
    if (!header) return;

    if (header->dwBytesRecorded > 0) {
        const BYTE* begin = reinterpret_cast<const BYTE*>(header->lpData);
        g_audioLevel.store(CalculateAudioLevel(begin, header->dwBytesRecorded));

        EnterCriticalSection(&g_audioLock);
        g_audioData.insert(g_audioData.end(), begin, begin + header->dwBytesRecorded);
        LeaveCriticalSection(&g_audioLock);
    }

    if (g_captureActive) {
        header->dwBytesRecorded = 0;
        waveInAddBuffer(waveIn, header, sizeof(WAVEHDR));
    }
}

bool StartAudioCapture(std::wstring& error) {
    if (g_waveIn) return true;

    g_audioLevel.store(0.0f);
    g_hudSmoothedLevel = 0.0f;
    EnterCriticalSection(&g_audioLock);
    g_audioData.clear();
    LeaveCriticalSection(&g_audioLock);

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    MMRESULT result = waveInOpen(&g_waveIn, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(WaveInProc), 0, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        error = L"Microphone open failed";
        g_waveIn = nullptr;
        return false;
    }

    g_waveBuffers.assign(4, std::vector<BYTE>(format.nAvgBytesPerSec / 10));
    ZeroMemory(g_waveHeaders, sizeof(g_waveHeaders));
    g_captureActive = true;

    for (size_t i = 0; i < g_waveBuffers.size(); ++i) {
        g_waveHeaders[i].lpData = reinterpret_cast<LPSTR>(g_waveBuffers[i].data());
        g_waveHeaders[i].dwBufferLength = static_cast<DWORD>(g_waveBuffers[i].size());
        waveInPrepareHeader(g_waveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_waveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
    }

    result = waveInStart(g_waveIn);
    if (result != MMSYSERR_NOERROR) {
        error = L"Microphone start failed";
        g_captureActive = false;
        waveInReset(g_waveIn);
        for (auto& header : g_waveHeaders) waveInUnprepareHeader(g_waveIn, &header, sizeof(WAVEHDR));
        waveInClose(g_waveIn);
        g_waveIn = nullptr;
        return false;
    }

    return true;
}

std::vector<BYTE> StopAudioCapture() {
    if (g_waveIn) {
        g_captureActive = false;
        waveInStop(g_waveIn);
        waveInReset(g_waveIn);
        for (auto& header : g_waveHeaders) {
            waveInUnprepareHeader(g_waveIn, &header, sizeof(WAVEHDR));
        }
        waveInClose(g_waveIn);
        g_waveIn = nullptr;
    }
    g_audioLevel.store(0.0f);

    std::vector<BYTE> data;
    EnterCriticalSection(&g_audioLock);
    data = g_audioData;
    g_audioData.clear();
    LeaveCriticalSection(&g_audioLock);
    g_waveBuffers.clear();
    return data;
}

bool WriteWavFile(const std::wstring& path, const std::vector<BYTE>& pcm) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) return false;

    const uint32_t sampleRate = 16000;
    const uint16_t channels = 1;
    const uint16_t bitsPerSample = 16;
    const uint16_t blockAlign = channels * bitsPerSample / 8;
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t dataSize = static_cast<uint32_t>(pcm.size());
    const uint32_t riffSize = 36 + dataSize;

    file.write("RIFF", 4);
    file.write(reinterpret_cast<const char*>(&riffSize), 4);
    file.write("WAVE", 4);
    file.write("fmt ", 4);
    const uint32_t fmtSize = 16;
    const uint16_t audioFormat = 1;
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&channels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);
    file.write("data", 4);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
    if (!pcm.empty()) file.write(reinterpret_cast<const char*>(pcm.data()), pcm.size());
    return file.good();
}

std::wstring RunProcessCapture(const std::wstring& commandLine, DWORD timeoutMs, DWORD& exitCode) {
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    exitCode = 1;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return L"";
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCommand = commandLine;

    BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, AppRootDir().c_str(), &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return L"Failed to start python ASR script";
    }

    std::string output;
    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        output.append(buffer, buffer + read);
    }
    CloseHandle(readPipe);

    DWORD waitResult = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (waitResult == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 2);
        output += "\nASR timeout";
    }
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return Utf8ToWide(output);
}

void CloseWorkerHandlesLocked() {
    if (g_workerProcess.hThread) {
        CloseHandle(g_workerProcess.hThread);
    }
    if (g_workerProcess.hProcess) {
        CloseHandle(g_workerProcess.hProcess);
    }
    g_workerProcess = {};
    g_workerStarted = false;
}

bool WorkerAliveLocked() {
    if (!g_workerStarted || !g_workerProcess.hProcess) return false;
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(g_workerProcess.hProcess, &exitCode) || exitCode != STILL_ACTIVE) {
        CloseWorkerHandlesLocked();
        return false;
    }
    return true;
}

std::string SendWorkerJson(const std::string& request, DWORD timeoutMs) {
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return {};

    const DWORD timeout = timeoutMs;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kWorkerPort);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(sock);
        return {};
    }

    std::string payload = request;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');

    const char* data = payload.data();
    int remaining = static_cast<int>(payload.size());
    while (remaining > 0) {
        const int sent = send(sock, data, remaining, 0);
        if (sent <= 0) {
            closesocket(sock);
            return {};
        }
        data += sent;
        remaining -= sent;
    }
    shutdown(sock, SD_SEND);

    std::string response;
    char buffer[4096];
    while (true) {
        const int received = recv(sock, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        response.append(buffer, buffer + received);
        if (response.find('\n') != std::string::npos) break;
    }
    closesocket(sock);
    return response;
}

bool PingWorker() {
    const std::string response = SendWorkerJson("{\"cmd\":\"ping\"}", 1000);
    return ExtractJsonBool(response, "ok", false);
}

bool StartWorkerProcess() {
    if (PingWorker()) return true;

    {
        std::lock_guard<std::mutex> guard(g_workerMutex);
        if (!WorkerAliveLocked()) {
            const std::wstring script = AppRootDir() + L"\\asr_worker.py";
            std::wstring command =
                L"python " + QuoteArg(script) +
                L" --host 127.0.0.1 --port " + std::to_wstring(kWorkerPort);

            STARTUPINFOW si = { sizeof(si) };
            PROCESS_INFORMATION pi = {};
            std::wstring mutableCommand = command;
            BOOL ok = CreateProcessW(
                nullptr,
                mutableCommand.data(),
                nullptr,
                nullptr,
                FALSE,
                CREATE_NO_WINDOW,
                nullptr,
                AppRootDir().c_str(),
                &si,
                &pi);
            if (!ok) return false;
            g_workerProcess = pi;
            g_workerStarted = true;
        }
    }

    const DWORD started = GetTickCount();
    while (GetTickCount() - started < 7000) {
        if (PingWorker()) return true;
        {
            std::lock_guard<std::mutex> guard(g_workerMutex);
            if (g_workerStarted && !WorkerAliveLocked()) return false;
        }
        Sleep(150);
    }
    return PingWorker();
}

void StopWorkerProcess() {
    bool shouldShutdown = false;
    {
        std::lock_guard<std::mutex> guard(g_workerMutex);
        shouldShutdown = g_workerStarted;
    }
    if (shouldShutdown) {
        SendWorkerJson("{\"cmd\":\"shutdown\"}", 1000);
    }

    std::lock_guard<std::mutex> guard(g_workerMutex);
    if (g_workerStarted && g_workerProcess.hProcess) {
        DWORD waitResult = WaitForSingleObject(g_workerProcess.hProcess, 2000);
        if (waitResult == WAIT_TIMEOUT) {
            TerminateProcess(g_workerProcess.hProcess, 0);
            WaitForSingleObject(g_workerProcess.hProcess, 1000);
        }
    }
    CloseWorkerHandlesLocked();
}

bool ReloadWorker() {
    if (!StartWorkerProcess()) return false;
    const std::string response = SendWorkerJson("{\"cmd\":\"reload\"}", 2000);
    return ExtractJsonBool(response, "ok", false);
}

std::string BuildRecognizeRequest(const Config& config, const std::wstring& wavPath) {
    const std::wstring modelDir = config.modelDir.empty() ? DefaultModelDir(config.modelId) : config.modelDir;
    return std::string("{\"cmd\":\"recognize\"") +
        ",\"model_id\":\"" + EscapeJson(config.modelId) + "\"" +
        ",\"model_dir\":\"" + EscapeJson(modelDir) + "\"" +
        ",\"threads\":\"" + EscapeJson(config.threads) + "\"" +
        ",\"enable_vad\":" + std::string(config.enableVad ? "true" : "false") +
        ",\"postprocess\":\"" + EscapeJson(config.postprocess) + "\"" +
        ",\"wav\":\"" + EscapeJson(wavPath) + "\"}";
}

std::wstring ExtractAsrText(const std::wstring& output, bool& ok) {
    const std::string utf8 = WideToUtf8(output);
    ok = ExtractJsonBool(utf8, "ok", false);
    if (ok) {
        return ExtractJsonString(utf8, "text", L"");
    }
    return ExtractJsonString(utf8, "error", L"ASR failed");
}

void RecognizeAsync(const std::wstring& wavPath) {
    const Config config = g_config;
    std::thread([config, wavPath]() {
        if (!StartWorkerProcess()) {
            PostMessageW(g_mainWindow, kAsrResultMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(L"ASR failed: worker did not start")));
            return;
        }

        std::string response = SendWorkerJson(BuildRecognizeRequest(config, wavPath), 120000);
        if (response.empty()) {
            StopWorkerProcess();
            if (StartWorkerProcess()) {
                response = SendWorkerJson(BuildRecognizeRequest(config, wavPath), 120000);
            }
        }

        bool ok = false;
        ok = ExtractJsonBool(response, "ok", false);
        std::wstring text = ok ? ExtractJsonString(response, "text", L"") : ExtractJsonString(response, "error", L"ASR worker pending");
        if (!ok) {
            text = L"ASR failed: " + text;
        }
        PostMessageW(g_mainWindow, kAsrResultMessage, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
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
    ShowHud(L"Listening... " + ModelDisplayName(g_config.modelId));
}

void StopRecordingSession() {
    if (!g_recording) return;
    g_recording = false;
    const std::vector<BYTE> pcm = StopAudioCapture();
    if (pcm.size() < 8000) {
        ShowHud(L"Too short");
        SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
        return;
    }

    const std::wstring wavPath = TempWavPath();
    if (!WriteWavFile(wavPath, pcm)) {
        ShowHud(L"Failed to save recording");
        SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
        return;
    }

    ShowHud(L"Recognizing... " + ModelDisplayName(g_config.modelId));
    RecognizeAsync(wavPath);
}

void ResetCapsLockHotkeyState() {
    if (g_mainWindow) KillTimer(g_mainWindow, kCapsLockLongPressTimer);
    g_activeHotkeyKey = 0;
    g_capsLockHotkeyPending = false;
    g_capsLockLongPressActive = false;
}

void StartCapsLockHotkeyPress() {
    if (g_activeHotkeyKey == VK_CAPITAL) return;
    g_activeHotkeyKey = VK_CAPITAL;
    g_capsLockHotkeyPending = true;
    g_capsLockLongPressActive = false;
    g_capsLockWasOn = IsCapsLockOn();
    if (g_mainWindow) SetTimer(g_mainWindow, kCapsLockLongPressTimer, kCapsLockLongPressMs, nullptr);
}

void ActivateCapsLockLongPress() {
    if (g_activeHotkeyKey != VK_CAPITAL || !g_capsLockHotkeyPending || g_capsLockLongPressActive) return;
    g_capsLockHotkeyPending = false;
    g_capsLockLongPressActive = true;
    StartRecordingSession();
}

void FinishCapsLockHotkeyPress() {
    if (g_activeHotkeyKey != VK_CAPITAL) return;
    if (g_mainWindow) KillTimer(g_mainWindow, kCapsLockLongPressTimer);

    const bool wasLongPress = g_capsLockLongPressActive;
    const bool wasShortPress = g_capsLockHotkeyPending && !g_capsLockLongPressActive;
    ResetCapsLockHotkeyState();

    if (wasLongPress) {
        StopRecordingSession();
        RestoreCapsLockState();
    } else if (wasShortPress) {
        SendCapsLockTap();
    }
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
    if (code == HC_ACTION) {
        const auto* event = reinterpret_cast<KBDLLHOOKSTRUCT*>(lParam);
        const HotkeyConfig hotkey = CurrentConfiguredHotkey();
        if (event && event->vkCode == VK_CAPITAL && (event->flags & LLKHF_INJECTED)) {
            return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
        }
        if (event && event->vkCode == hotkey.key && (ModifiersMatch(hotkey) || g_activeHotkeyKey == event->vkCode)) {
            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                if (hotkey.key == VK_CAPITAL) {
                    StartCapsLockHotkeyPress();
                    return 1;
                }
                g_activeHotkeyKey = event->vkCode;
                StartRecordingSession();
                return 1;
            }
            if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                if (hotkey.key == VK_CAPITAL) {
                    FinishCapsLockHotkeyPress();
                    return 1;
                }
                g_activeHotkeyKey = 0;
                StopRecordingSession();
                return 1;
            }
        }
    }
    return CallNextHookEx(g_keyboardHook, code, wParam, lParam);
}

void InstallKeyboardHook() {
    if (!g_keyboardHook) {
        g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, g_instance, 0);
    }
}

void UninstallKeyboardHook() {
    if (g_keyboardHook) {
        UnhookWindowsHookEx(g_keyboardHook);
        g_keyboardHook = nullptr;
    }
    ResetCapsLockHotkeyState();
}

void ApplyUiFont(HWND hwnd, HFONT font = nullptr) {
    if (hwnd) SendMessageW(hwnd, WM_SETFONT, reinterpret_cast<WPARAM>(font ? font : g_uiFont), TRUE);
}

LRESULT CALLBACK HotkeyEditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<HotkeyEditState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(new HotkeyEditState()));
        return TRUE;
    case WM_NCDESTROY:
        delete state;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS;
    case WM_SETFOCUS:
        if (state) {
            state->capturing = true;
            state->original = state->hotkey;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_KILLFOCUS:
        if (state) {
            state->capturing = false;
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;
    case WM_LBUTTONDOWN:
        SetFocus(hwnd);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (!state) break;
        if (wParam == VK_ESCAPE) {
            state->hotkey = state->original;
            state->capturing = false;
            SetFocus(GetParent(hwnd));
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        if (wParam == VK_BACK || wParam == VK_DELETE) {
            state->hotkey = HotkeyConfig{};
            state->hotkey.key = 0;
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;
        }
        if (IsModifierKey(static_cast<UINT>(wParam))) return 0;
        {
            HotkeyConfig hotkey;
            hotkey.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            hotkey.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            hotkey.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            hotkey.win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
            hotkey.key = NormalizedKeyFromWParam(wParam);
            state->hotkey = hotkey;
            state->capturing = false;
            InvalidateRect(hwnd, nullptr, TRUE);
            SetFocus(GetParent(hwnd));
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);

        HBRUSH bg = CreateSolidBrush(state && state->capturing ? RGB(255, 252, 223) : RGB(255, 255, 255));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        HPEN border = CreatePen(PS_SOLID, 1, state && state->capturing ? RGB(0, 120, 215) : RGB(205, 213, 224));
        HGDIOBJ oldPen = SelectObject(hdc, border);
        HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
        SelectObject(hdc, oldBrush);
        SelectObject(hdc, oldPen);
        DeleteObject(border);

        std::wstring text = L"CapsLock";
        COLORREF color = RGB(25, 31, 40);
        if (state) {
            text = state->capturing && state->hotkey.IsEmpty() ? L"Press shortcut..." : HotkeyToString(state->hotkey);
            if (state->capturing) color = RGB(80, 92, 108);
        }
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, color);
        HFONT font = reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
        HGDIOBJ oldFont = font ? SelectObject(hdc, font) : nullptr;
        RECT textRc = rc;
        textRc.left += 10;
        textRc.right -= 10;
        DrawTextW(hdc, text.c_str(), -1, &textRc, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);
        if (oldFont) SelectObject(hdc, oldFont);
        EndPaint(hwnd, &ps);
        return 0;
    }
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

HWND CreateHotkeyEdit(HWND parent, int id, int x, int y, int w, int h, const HotkeyConfig& initial) {
    HWND edit = CreateWindowExW(0, kHotkeyEditClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                                x, y, w, h, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_instance, nullptr);
    auto* state = reinterpret_cast<HotkeyEditState*>(GetWindowLongPtrW(edit, GWLP_USERDATA));
    if (state) {
        state->hotkey = initial;
        state->original = initial;
    }
    ApplyUiFont(edit);
    return edit;
}

HotkeyConfig GetHotkeyFromEdit(HWND parent, int id) {
    HWND edit = GetDlgItem(parent, id);
    auto* state = edit ? reinterpret_cast<HotkeyEditState*>(GetWindowLongPtrW(edit, GWLP_USERDATA)) : nullptr;
    return state ? state->hotkey : CurrentConfiguredHotkey();
}

void AddRecognitionControl(HWND hwnd) {
    if (hwnd) g_recognitionControls.push_back(hwnd);
}

void AddShortcutControl(HWND hwnd) {
    if (hwnd) g_shortcutControls.push_back(hwnd);
}

void ShowSettingsPage(HWND hwnd, int page) {
    for (HWND control : g_recognitionControls) {
        ShowWindow(control, page == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_shortcutControls) {
        ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void LayoutSettingsWindow(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int margin = 24;
    const int footerHeight = 78;
    const int footerTop = (rc.bottom - footerHeight > 390) ? rc.bottom - footerHeight : 390;
    const int tabBottom = footerTop - 24;
    HWND tab = GetDlgItem(hwnd, IDC_SETTINGS_TAB);
    if (tab) {
        MoveWindow(tab, margin, 28, rc.right - margin * 2, tabBottom - 28, TRUE);
    }
    HWND status = GetDlgItem(hwnd, IDC_STATUS);
    if (status) {
        MoveWindow(status, margin, footerTop + 28, rc.right - margin * 2 - 300, 32, TRUE);
    }
    HWND save = GetDlgItem(hwnd, IDC_SAVE);
    HWND close = GetDlgItem(hwnd, IDC_CANCEL);
    if (save) MoveWindow(save, rc.right - margin - 192, footerTop + 22, 84, 36, TRUE);
    if (close) MoveWindow(close, rc.right - margin - 84, footerTop + 22, 84, 36, TRUE);
}

void HideSettingsWindow(HWND hwnd) {
    ShowWindow(hwnd, SW_HIDE);
    InstallKeyboardHook();
}

HWND CreateLabel(HWND parent, int x, int y, int w, int h, const wchar_t* text) {
    HWND hwnd = CreateWindowW(L"STATIC", text, WS_CHILD | WS_VISIBLE, x, y, w, h, parent, nullptr, g_instance, nullptr);
    ApplyUiFont(hwnd);
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
    ComboBox_AddString(post, L"none");
    ComboBox_AddString(post, L"itn");
    ComboBox_AddString(post, L"llm");
    int postIndex = 1;
    if (g_config.postprocess == L"none") postIndex = 0;
    else if (g_config.postprocess == L"llm") postIndex = 2;
    ComboBox_SetCurSel(post, postIndex);

    HWND hotkeyEdit = GetDlgItem(hwnd, IDC_HOTKEY);
    auto* hotkeyState = hotkeyEdit ? reinterpret_cast<HotkeyEditState*>(GetWindowLongPtrW(hotkeyEdit, GWLP_USERDATA)) : nullptr;
    if (hotkeyState) {
        hotkeyState->hotkey = CurrentConfiguredHotkey();
        hotkeyState->original = hotkeyState->hotkey;
        InvalidateRect(hotkeyEdit, nullptr, TRUE);
    }
    SetStatus(hwnd, L"Ready. Save reloads the local ASR worker settings.");
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
    g_config.modelId = ModelIdFromIndex(ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_MODEL)));

    wchar_t modelDir[MAX_PATH] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), modelDir, MAX_PATH);
    g_config.modelDir = modelDir;

    g_config.threads = ComboText(GetDlgItem(hwnd, IDC_THREADS));
    if (g_config.threads.empty()) g_config.threads = L"auto";
    if (g_config.threads.substr(0, 4) == L"auto") g_config.threads = L"auto";
    g_config.enableVad = Button_GetCheck(GetDlgItem(hwnd, IDC_VAD)) == BST_CHECKED;
    g_config.enablePartial = Button_GetCheck(GetDlgItem(hwnd, IDC_PARTIAL)) == BST_CHECKED;
    g_config.postprocess = ComboText(GetDlgItem(hwnd, IDC_POSTPROCESS));
    if (g_config.postprocess.empty()) g_config.postprocess = L"itn";

    HotkeyConfig hotkey = GetHotkeyFromEdit(hwnd, IDC_HOTKEY);
    g_config.hotkey = HotkeyToString(hotkey);

    SaveConfig();
    SetStatus(hwnd, L"Saved. Worker reload requested.");
    PostMessageW(g_mainWindow, kReloadMessage, 0, 0);
}

bool EnsureHudRenderTarget(HWND hwnd) {
    if (!g_d2dFactory) return false;

    RECT rc;
    GetClientRect(hwnd, &rc);
    const D2D1_SIZE_U size = D2D1::SizeU(
        static_cast<UINT32>(std::max(1L, rc.right - rc.left)),
        static_cast<UINT32>(std::max(1L, rc.bottom - rc.top)));

    if (!g_hudRenderTarget) {
        const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_IGNORE));
        const D2D1_HWND_RENDER_TARGET_PROPERTIES hwndProps =
            D2D1::HwndRenderTargetProperties(hwnd, size, D2D1_PRESENT_OPTIONS_NONE);
        if (FAILED(g_d2dFactory->CreateHwndRenderTarget(props, hwndProps, &g_hudRenderTarget))) {
            return false;
        }
    } else if (g_hudRenderTarget->GetPixelSize().width != size.width ||
               g_hudRenderTarget->GetPixelSize().height != size.height) {
        g_hudRenderTarget->Resize(size);
    }

    if (!g_hudBrush && g_hudRenderTarget) {
        if (FAILED(g_hudRenderTarget->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &g_hudBrush))) {
            return false;
        }
    }
    return g_hudRenderTarget && g_hudBrush && g_hudTextFormat;
}

void DrawHudDirect2D(HWND hwnd) {
    if (!EnsureHudRenderTarget(hwnd)) {
        ValidateRect(hwnd, nullptr);
        return;
    }

    RECT rc;
    GetClientRect(hwnd, &rc);
    const D2D1_SIZE_F renderSize = g_hudRenderTarget->GetSize();
    const float width = renderSize.width;
    const float height = renderSize.height;
    const float radius = height / 2.0f;

    g_hudRenderTarget->BeginDraw();
    const D2D1_COLOR_F bgColor = D2D1::ColorF(0.105f, 0.118f, 0.145f, 0.96f);
    g_hudRenderTarget->Clear(bgColor);

    D2D1_ROUNDED_RECT capsule = D2D1::RoundedRect(
        D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f),
        radius,
        radius);

    g_hudBrush->SetColor(bgColor);
    g_hudRenderTarget->FillRoundedRectangle(capsule, g_hudBrush);
    g_hudBrush->SetColor(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.11f));
    g_hudRenderTarget->DrawRoundedRectangle(capsule, g_hudBrush, 1.0f);

    const float level = g_recording ? CurrentHudLevel() : 0.18f;
    const float centerY = height / 2.0f;
    const float barWidth = 6.0f;
    const float barGap = 4.5f;
    const float barAreaHeight = std::min(40.0f, height - 16.0f);
    const float barStartX = kHudLeftPad + 2.0f;
    const float weights[] = { 0.5f, 0.8f, 1.0f, 0.75f, 0.55f };
    const float minFraction = 0.24f;
    const double tick = static_cast<double>(GetTickCount64());

    g_hudBrush->SetColor(g_recording
        ? D2D1::ColorF(0.32f, 0.82f, 1.0f, 0.95f)
        : D2D1::ColorF(0.62f, 0.66f, 0.72f, 0.72f));

    for (int i = 0; i < 5; ++i) {
        const float motion = g_recording ? static_cast<float>(std::sin(tick * 0.012 + i * 1.9) * 0.035) : 0.0f;
        const float fraction = std::clamp(minFraction + (1.0f - minFraction) * level * weights[i] + motion,
                                          minFraction, 1.0f);
        const float h = barAreaHeight * fraction;
        const float x = barStartX + i * (barWidth + barGap);
        const D2D1_ROUNDED_RECT bar = D2D1::RoundedRect(
            D2D1::RectF(x, centerY - h / 2.0f, x + barWidth, centerY + h / 2.0f),
            barWidth / 2.0f,
            barWidth / 2.0f);
        g_hudRenderTarget->FillRoundedRectangle(bar, g_hudBrush);
    }

    g_hudBrush->SetColor(D2D1::ColorF(0.965f, 0.975f, 0.99f, 0.96f));
    const float textX = kHudLeftPad + kHudWaveWidth + kHudGap;
    const D2D1_RECT_F textRect = D2D1::RectF(textX, 0.0f, width - kHudRightPad, height);
    g_hudRenderTarget->DrawTextW(
        g_hudText.c_str(),
        static_cast<UINT32>(g_hudText.size()),
        g_hudTextFormat,
        textRect,
        g_hudBrush,
        D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);

    const HRESULT hr = g_hudRenderTarget->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        SafeRelease(g_hudBrush);
        SafeRelease(g_hudRenderTarget);
    }
    ValidateRect(hwnd, nullptr);
}

LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_TIMER:
        if (wParam == kHudAnimationTimer) {
            InvalidateRect(hwnd, nullptr, FALSE);
        } else {
            KillTimer(hwnd, static_cast<UINT_PTR>(wParam));
            HideHud();
        }
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_SIZE:
        if (g_hudRenderTarget) {
            const UINT width = LOWORD(lParam);
            const UINT height = HIWORD(lParam);
            if (width > 0 && height > 0) {
                g_hudRenderTarget->Resize(D2D1::SizeU(width, height));
            }
        }
        return 0;
    case WM_PAINT:
        DrawHudDirect2D(hwnd);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
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

        HPEN line = CreatePen(PS_SOLID, 1, RGB(226, 232, 240));
        HGDIOBJ oldPen = SelectObject(hdc, line);
        const int footerTop = (rc.bottom - 78 > 390) ? rc.bottom - 78 : 390;
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
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(30, 41, 59));
        SetBkColor(hdc, RGB(246, 248, 251));
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, RGB(17, 24, 39));
        SetBkColor(hdc, RGB(255, 255, 255));
        return reinterpret_cast<LRESULT>(g_controlBgBrush);
    }
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkColor(hdc, RGB(246, 248, 251));
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    }
    case WM_CREATE: {
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
        g_recognitionControls.clear();
        g_shortcutControls.clear();

        HWND tab = CreateWindowW(WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                24, 28, 786, 330, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_TAB)), g_instance, nullptr);
        ApplyUiFont(tab);
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(L"Recognition");
        TabCtrl_InsertItem(tab, 0, &item);
        item.pszText = const_cast<LPWSTR>(L"Shortcut");
        TabCtrl_InsertItem(tab, 1, &item);

        HWND control = CreateLabel(hwnd, 54, 82, 130, 30, L"ASR model");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_MODEL, 200, 76, 330, 180));

        control = CreateLabel(hwnd, 54, 134, 130, 30, L"Model folder");
        AddRecognitionControl(control);
        HWND modelDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        200, 128, 480, 32, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODEL_DIR)), g_instance, nullptr);
        ApplyUiFont(modelDir);
        AddRecognitionControl(modelDir);
        AddRecognitionControl(CreateButton(hwnd, IDC_BROWSE, 694, 127, 92, 34, L"Browse..."));

        control = CreateLabel(hwnd, 54, 186, 130, 30, L"Threads");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_THREADS, 200, 180, 130, 150));
        HWND vad = CreateWindowW(L"BUTTON", L"Enable VAD", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 370, 184, 140, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD)), g_instance, nullptr);
        HWND partial = CreateWindowW(L"BUTTON", L"Partial result", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     530, 184, 160, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PARTIAL)), g_instance, nullptr);
        ApplyUiFont(vad);
        ApplyUiFont(partial);
        AddRecognitionControl(vad);
        AddRecognitionControl(partial);

        control = CreateLabel(hwnd, 54, 238, 130, 30, L"Postprocess");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_POSTPROCESS, 200, 232, 250, 150));

        control = CreateLabel(hwnd, 54, 82, 130, 30, L"Hold hotkey");
        AddShortcutControl(control);
        AddShortcutControl(CreateHotkeyEdit(hwnd, IDC_HOTKEY, 200, 74, 300, 38, CurrentConfiguredHotkey()));
        control = CreateLabel(hwnd, 54, 134, 640, 30, L"Click the field, then press the key or key combination to use while recording.");
        AddShortcutControl(control);
        control = CreateLabel(hwnd, 54, 174, 640, 30, L"Esc cancels recording a shortcut. Backspace/Delete clears it.");
        AddShortcutControl(control);

        HWND status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    24, 460, 520, 30, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
        ApplyUiFont(status);
        CreateButton(hwnd, IDC_SAVE, 626, 452, 84, 36, L"Save");
        CreateButton(hwnd, IDC_CANCEL, 726, 452, 84, 36, L"Close");
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
        case IDC_BROWSE:
            BrowseModelDirectory(hwnd);
            return 0;
        case IDC_SAVE:
            SaveSettingsControls(hwnd);
            return 0;
        case IDC_CANCEL:
            HideSettingsWindow(hwnd);
            return 0;
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
        g_settingsWindow = CreateWindowExW(
            WS_EX_APPWINDOW,
            kSettingsClass,
            L"Voice LLM ASR Input Settings",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            850,
            580,
            owner,
            nullptr,
            g_instance,
            nullptr);
    }
    // Center on screen
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

void ShowTrayMenu(HWND hwnd) {
    POINT pt;
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, ID_TRAY_VERSION, L"Version: v0.1.3");
    AppendMenuW(menu, MF_STRING, ID_TRAY_SETTINGS, L"Settings...");
    AppendMenuW(menu, MF_STRING, ID_TRAY_RELOAD, L"Reload ASR Worker");
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
        std::thread([]() { StartWorkerProcess(); }).detach();
        return 0;
    case kTrayMessage:
        if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(hwnd);
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ShowSettingsWindow(hwnd);
        }
        return 0;
    case kReloadMessage:
        {
            const std::wstring modelId = g_config.modelId;
            std::thread([modelId]() {
            const bool ok = ReloadWorker();
            const std::wstring text = ok
                ? L"ASR worker reloaded: " + ModelDisplayName(modelId)
                : L"ASR worker reload failed";
            PostMessageW(g_mainWindow, kAsrResultMessage, 1, reinterpret_cast<LPARAM>(new std::wstring(text)));
            }).detach();
        }
        ShowHud(L"Reloading ASR worker...");
        SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
        return 0;
    case kAsrResultMessage: {
        std::unique_ptr<std::wstring> result(reinterpret_cast<std::wstring*>(lParam));
        const std::wstring text = result ? *result : L"ASR failed";
        ShowHud(text.empty() ? L"(empty result)" : text);
        if (wParam == 0 && !text.empty() && text.rfind(L"ASR failed:", 0) != 0) {
            SetClipboardText(text);
            SendCtrlV();
        }
        if (g_hudWindow) {
            const bool isError = text.rfind(L"ASR failed:", 0) == 0;
            const bool isStatusOnly = wParam != 0;
            SetTimer(g_hudWindow, kHudHideTimer, isError ? 2200 : (isStatusOnly ? 1100 : 200), nullptr);
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
        StopWorkerProcess();
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

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    g_instance = instance;
    InitializeCriticalSection(&g_audioLock);
    WSADATA wsaData = {};
    const bool winsockReady = WSAStartup(MAKEWORD(2, 2), &wsaData) == 0;
    if (!winsockReady) {
        MessageBoxW(nullptr, L"Failed to initialize local ASR worker network.", kAppName, MB_OK | MB_ICONERROR);
        DeleteCriticalSection(&g_audioLock);
        return 1;
    }
    InitCommonControls();
    g_appIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP_ICON));
    if (!g_appIcon) {
        g_appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    CreateUiResources();

    LoadConfig();

    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\VoiceLLMASRInput.SingleInstance");
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"Voice LLM ASR Input is already running.", kAppName, MB_OK | MB_ICONINFORMATION);
        CloseHandle(mutex);
        WSACleanup();
        DeleteUiResources();
        DeleteCriticalSection(&g_audioLock);
        return 0;
    }

    if (!RegisterWindowClasses()) {
        MessageBoxW(nullptr, L"Failed to register window classes.", kAppName, MB_OK | MB_ICONERROR);
        WSACleanup();
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
        StopWorkerProcess();
        WSACleanup();
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
    WSACleanup();
    DeleteUiResources();
    DeleteCriticalSection(&g_audioLock);
    return static_cast<int>(msg.wParam);
}
