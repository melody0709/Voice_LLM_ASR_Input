#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "settings.h"
#include "engine.h"
#include "hotkey.h"
#include "hud.h"
#include "mimo_asr.h"
#include "qwen_asr.h"

#include <algorithm>
#include <commctrl.h>
#include <imm.h>
#include <windowsx.h>
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
}

void SetStatus(HWND hwnd, const std::wstring& text) {
    SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS), text.c_str());
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

void AddMimoControl(HWND hwnd) {
    if (hwnd) g_mimoControls.push_back(hwnd);
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
    for (HWND c : g_mimoControls) ShowWindow(c, providerIdx == 3 ? SW_SHOW : SW_HIDE);
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
        for (HWND c : g_mimoControls) ShowWindow(c, SW_HIDE);
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
        RECT tabRc;
        GetWindowRect(tab, &tabRc);
        MapWindowPoints(nullptr, hwnd, reinterpret_cast<LPPOINT>(&tabRc), 2);
        const int hintY = tabRc.bottom - S(UiStyle::LabelH) - S(4);
        SetWindowPos(g_cloudAsrHintControl, nullptr, S(UiStyle::ContentLeft), hintY, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
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
    ComboBox_AddString(backendCombo, L"Local (sherpa-onnx)");
    ComboBox_AddString(backendCombo, L"Volcano Engine");
    ComboBox_AddString(backendCombo, L"Baidu Cloud");
    ComboBox_AddString(backendCombo, L"Qwen ASR");
    ComboBox_AddString(backendCombo, L"MiMo ASR");
    int backendIdx = 0;
    if (g_config.asrBackend == L"volcengine") backendIdx = 1;
    else if (g_config.asrBackend == L"baidu") backendIdx = 2;
    else if (g_config.asrBackend == L"qwen") backendIdx = 3;
    else if (g_config.asrBackend == L"mimo") backendIdx = 4;
    ComboBox_SetCurSel(backendCombo, backendIdx);

    HWND cloudProviderCombo = GetDlgItem(hwnd, IDC_CLOUD_PROVIDER);
    ComboBox_AddString(cloudProviderCombo, L"Volcano Engine (Doubao)");
    ComboBox_AddString(cloudProviderCombo, L"Baidu Cloud");
    ComboBox_AddString(cloudProviderCombo, L"Qwen ASR (DashScope)");
    ComboBox_AddString(cloudProviderCombo, L"MiMo ASR (Xiaomi)");
    int cloudIdx = 0;
    if (g_config.cloudProvider == L"baidu") cloudIdx = 1;
    else if (g_config.cloudProvider == L"qwen") cloudIdx = 2;
    else if (g_config.cloudProvider == L"mimo") cloudIdx = 3;
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
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_BASE_URL), g_config.qwenBaseUrl.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MODEL), g_config.qwenModel.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_API_KEY), g_config.mimoApiKey.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_BASE_URL), g_config.mimoBaseUrl.c_str());
    SetWindowTextW(GetDlgItem(hwnd, IDC_MIMO_MODEL), g_config.mimoModel.c_str());

    HWND qwenLangCombo = GetDlgItem(hwnd, IDC_QWEN_LANGUAGE);
    for (const auto& lang : kQwenLanguages) {
        ComboBox_AddString(qwenLangCombo, lang.label);
    }
    ComboBox_SetCurSel(qwenLangCombo, QwenLanguageIndexFromCode(g_config.qwenLanguage));

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
        int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_ASR_BACKEND));
        if (sel == 1) g_config.asrBackend = L"volcengine";
        else if (sel == 2) g_config.asrBackend = L"baidu";
        else if (sel == 3) g_config.asrBackend = L"qwen";
        else if (sel == 4) g_config.asrBackend = L"mimo";
        else g_config.asrBackend = L"local";
    }
    {
        int cloudIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CLOUD_PROVIDER));
        if (cloudIdx == 1) g_config.cloudProvider = L"baidu";
        else if (cloudIdx == 2) g_config.cloudProvider = L"qwen";
        else if (cloudIdx == 3) g_config.cloudProvider = L"mimo";
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
    g_config.qwenBaseUrl = qwenBaseUrl[0] ? qwenBaseUrl : qwen_asr::kDefaultBaseUrl;
    wchar_t qwenModel[256] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_MODEL), qwenModel, 256);
    g_config.qwenModel = qwenModel[0] ? qwenModel : qwen_asr::kDefaultModel;
    {
        int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_QWEN_LANGUAGE));
        g_config.qwenLanguage = QwenLanguageCodeFromIndex(langIdx);
    }
    {
        wchar_t buf[32] = {};
        GetWindowTextW(GetDlgItem(hwnd, IDC_QWEN_CHUNK_MS), buf, 32);
        g_config.qwenChunkMs = std::clamp(_wtoi(buf), 20, 1000);
    }

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
    SetStatus(hwnd, L"Saved. ASR engine reloaded.");
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
    case WM_CTLCOLORDLG:
        return reinterpret_cast<LRESULT>(g_settingsBgBrush);
    case WM_CTLCOLORSTATIC: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND ctl = reinterpret_cast<HWND>(lParam);
        if (ctl == GetDlgItem(hwnd, IDC_LLM_PROMPT_HINT)) {
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
        g_mimoControls.clear();
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
        AddRecognitionControl(CreateCombo(hwnd, IDC_ASR_BACKEND, S(UiStyle::InputLeft), S(UiStyle::RowInputY(0)), S(330), S(UiStyle::ComboH)));

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

        const int shortcutY = S(UiStyle::RowInputY(0));
        HWND shortcutGroup = CreateWindowW(L"BUTTON", L"Shortcut", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                           S(30), shortcutY, S(788), S(170), hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(shortcutGroup);
        AddGeneralControl(shortcutGroup);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), shortcutY + S(28), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Hold hotkey");
        AddGeneralControl(control);
        AddGeneralControl(CreateHotkeyEdit(hwnd, IDC_HOTKEY, S(UiStyle::InputLeft), shortcutY + S(20), S(300), S(UiStyle::HotkeyEditH), CurrentConfiguredHotkey()));
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), shortcutY + S(68), S(740), S(UiStyle::LabelH), L"Click the field, then press the key or key combination to use while recording.");
        AddGeneralControl(control);
        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), shortcutY + S(98), S(740), S(UiStyle::LabelH), L"Esc cancels recording a shortcut. Backspace/Delete clears it.");
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
                                           S(UiStyle::InputLeft), S(UiStyle::RowInputY(2)), S(480), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_BASE_URL)), g_instance, nullptr);
        ApplyUiFont(qwenBaseUrl);
        AddQwenControl(qwenBaseUrl);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(3)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Model");
        AddQwenControl(control);
        HWND qwenModel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", qwen_asr::kDefaultModel, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                         S(UiStyle::InputLeft), S(UiStyle::RowInputY(3)), S(330), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_MODEL)), g_instance, nullptr);
        ApplyUiFont(qwenModel);
        AddQwenControl(qwenModel);

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(4)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Language");
        AddQwenControl(control);
        AddQwenControl(CreateCombo(hwnd, IDC_QWEN_LANGUAGE, S(UiStyle::InputLeft), S(UiStyle::RowInputY(4)), S(UiStyle::ComboW), S(UiStyle::ComboH)));

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowLabelY(5)), S(UiStyle::LabelWidth), S(UiStyle::LabelH), L"Chunk ms");
        AddQwenControl(control);
        HWND qwenChunkMs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_NUMBER,
                                           S(UiStyle::InputLeft), S(UiStyle::RowInputY(5)), S(80), S(UiStyle::EditH), hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_QWEN_CHUNK_MS)), g_instance, nullptr);
        ApplyUiFont(qwenChunkMs);
        AddQwenControl(qwenChunkMs);

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

        control = CreateLabel(hwnd, S(UiStyle::ContentLeft), S(UiStyle::RowInputY(9)) + S(30), S(640), S(UiStyle::LabelH),
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
        case IDC_QWEN_TEST: {
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
