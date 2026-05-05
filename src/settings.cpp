#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "settings.h"
#include "engine.h"
#include "hotkey.h"
#include "hud.h"

#include <algorithm>
#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <string>
#include <thread>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

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

void AddShortcutControl(HWND hwnd) {
    if (hwnd) g_shortcutControls.push_back(hwnd);
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

void ShowCloudSubPage(HWND hwnd, int providerIdx) {
    g_cloudProviderIdx = providerIdx;
    for (HWND c : g_baiduControls) ShowWindow(c, providerIdx == 0 ? SW_SHOW : SW_HIDE);
    for (HWND c : g_volcengineControls) ShowWindow(c, providerIdx == 1 ? SW_SHOW : SW_HIDE);
    if (g_cloudSectionLabel) {
        SetWindowTextW(g_cloudSectionLabel, providerIdx == 0
            ? L"\u2500\u2500 \u767E\u5EA6\u667A\u80FD\u4E91 \u2500\u2500"
            : L"\u2500\u2500 \u706B\u5C71\u5F15\u64CE\uFF08\u8C46\u5305\uFF09 \u2500\u2500");
    }
}

void ShowSettingsPage(HWND hwnd, int page) {
    for (HWND control : g_recognitionControls) {
        ShowWindow(control, page == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_shortcutControls) {
        ShowWindow(control, page == 0 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_llmControls) {
        ShowWindow(control, page == 1 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_promptControls) {
        ShowWindow(control, page == 2 ? SW_SHOW : SW_HIDE);
    }
    for (HWND control : g_cloudAsrControls) {
        ShowWindow(control, page == 3 ? SW_SHOW : SW_HIDE);
    }
    if (page == 3) {
        ShowCloudSubPage(hwnd, g_cloudProviderIdx);
    } else {
        for (HWND c : g_baiduControls) ShowWindow(c, SW_HIDE);
        for (HWND c : g_volcengineControls) ShowWindow(c, SW_HIDE);
    }
    InvalidateRect(hwnd, nullptr, TRUE);
}

void LayoutSettingsWindow(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int margin = UiStyle::Margin;
    const int footerHeight = UiStyle::FooterHeight;
    const int footerTop = (rc.bottom - footerHeight > UiStyle::FooterMinTop) ? rc.bottom - footerHeight : UiStyle::FooterMinTop;
    const int tabBottom = footerTop - 24;
    HWND tab = GetDlgItem(hwnd, IDC_SETTINGS_TAB);
    if (tab) {
        MoveWindow(tab, margin, 16, rc.right - margin * 2, tabBottom - 16, TRUE);
    }
    HWND status = GetDlgItem(hwnd, IDC_STATUS);
    if (status) {
        MoveWindow(status, margin, footerTop + 28, rc.right - margin * 2 - 300, 32, TRUE);
    }
    HWND save = GetDlgItem(hwnd, IDC_SAVE);
    HWND close = GetDlgItem(hwnd, IDC_CANCEL);
    if (save) MoveWindow(save, rc.right - margin - 192, footerTop + 22, UiStyle::FooterBtnW, UiStyle::ActionBtnH, TRUE);
    if (close) MoveWindow(close, rc.right - margin - UiStyle::FooterBtnW, footerTop + 22, UiStyle::FooterBtnW, UiStyle::ActionBtnH, TRUE);
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
    ComboBox_AddString(backendCombo, L"Baidu Cloud");
    ComboBox_AddString(backendCombo, L"Volcano Engine");
    int backendIdx = 0;
    if (g_config.asrBackend == L"baidu") backendIdx = 1;
    else if (g_config.asrBackend == L"volcengine") backendIdx = 2;
    ComboBox_SetCurSel(backendCombo, backendIdx);

    HWND cloudProviderCombo = GetDlgItem(hwnd, IDC_CLOUD_PROVIDER);
    ComboBox_AddString(cloudProviderCombo, L"\u767E\u5EA6\u667A\u80FD\u4E91");
    ComboBox_AddString(cloudProviderCombo, L"\u706B\u5C71\u5F15\u64CE\uFF08\u8C46\u5305\uFF09");
    int cloudIdx = (g_config.cloudProvider == L"volcengine") ? 1 : 0;
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

    HWND volcModeCombo = GetDlgItem(hwnd, IDC_VOLC_MODE);
    ComboBox_AddString(volcModeCombo, L"File Recognition (nostream)");
    ComboBox_AddString(volcModeCombo, L"Streaming (bigmodel)");
    ComboBox_AddString(volcModeCombo, L"Streaming Optimized (async)");
    int modeIdx = 0;
    if (g_config.volcMode == L"bigmodel") modeIdx = 1;
    else if (g_config.volcMode == L"bigmodel_async") modeIdx = 2;
    ComboBox_SetCurSel(volcModeCombo, modeIdx);

    HWND volcResCombo = GetDlgItem(hwnd, IDC_VOLC_RESOURCE);
    ComboBox_AddString(volcResCombo, L"Seed-ASR 2.0 (duration)");
    ComboBox_AddString(volcResCombo, L"Seed-ASR 2.0 (concurrent)");
    int resIdx = 0;
    if (g_config.volcResourceId == L"volc.seedasr.sauc.concurrent") resIdx = 1;
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

    wchar_t llmPrompt[2048] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PROMPT), llmPrompt, 2048);
    g_config.llmPrompt = llmPrompt;

    wchar_t llmExtra[1024] = {};
    GetWindowTextW(GetDlgItem(hwnd, IDC_LLM_EXTRA), llmExtra, 1024);
    g_config.llmExtraParams = llmExtra;

    g_config.llmProvider = ComboText(GetDlgItem(hwnd, IDC_LLM_PROVIDER));

    g_config.enableLlmDebug = Button_GetCheck(GetDlgItem(hwnd, IDC_LLM_DEBUG)) == BST_CHECKED;

    {
        int sel = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_ASR_BACKEND));
        if (sel == 1) g_config.asrBackend = L"baidu";
        else if (sel == 2) g_config.asrBackend = L"volcengine";
        else g_config.asrBackend = L"local";
    }
    {
        int cloudIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CLOUD_PROVIDER));
        g_config.cloudProvider = (cloudIdx == 1) ? L"volcengine" : L"baidu";
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
    {
        int modeIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_MODE));
        if (modeIdx == 1) g_config.volcMode = L"bigmodel";
        else if (modeIdx == 2) g_config.volcMode = L"bigmodel_async";
        else g_config.volcMode = L"bigmodel_nostream";
    }
    {
        int resIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_RESOURCE));
        if (resIdx == 1) g_config.volcResourceId = L"volc.seedasr.sauc.concurrent";
        else g_config.volcResourceId = L"volc.seedasr.sauc.duration";
    }
    {
        int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_LANGUAGE));
        if (langIdx >= 0 && langIdx < 9) g_config.volcLanguage = kVolcLanguages[langIdx];
    }

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
        PostQuitMessage(0);
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
        wc.lpszClassName = L"VoiceLLMInputDlg";
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
                               L"VoiceLLMInputDlg", title,
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
    }
    if (data.ok) { out = data.result; return true; }
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
        SetTextColor(hdc, UiStyle::TextColor);
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
        SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(g_appIcon));
        SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(g_appIcon));
        g_recognitionControls.clear();
        g_shortcutControls.clear();
        g_llmControls.clear();
        g_promptControls.clear();
        g_cloudAsrControls.clear();
        g_baiduControls.clear();
        g_volcengineControls.clear();

        HWND tab = CreateWindowW(WC_TABCONTROLW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                                UiStyle::Margin, 16, 786, 330, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_SETTINGS_TAB)), g_instance, nullptr);
        ApplyUiFont(tab);
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = const_cast<LPWSTR>(L"Recognition");
        TabCtrl_InsertItem(tab, 0, &item);
        item.pszText = const_cast<LPWSTR>(L"LLM");
        TabCtrl_InsertItem(tab, 1, &item);
        item.pszText = const_cast<LPWSTR>(L"LLM Prompt");
        TabCtrl_InsertItem(tab, 2, &item);
        item.pszText = const_cast<LPWSTR>(L"Cloud ASR");
        TabCtrl_InsertItem(tab, 3, &item);

        HWND control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(0), UiStyle::LabelWidth, UiStyle::LabelH, L"ASR Backend");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_ASR_BACKEND, UiStyle::InputLeft, UiStyle::RowInputY(0), 330, UiStyle::ComboH));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(1), UiStyle::LabelWidth, UiStyle::LabelH, L"ASR model");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_MODEL, UiStyle::InputLeft, UiStyle::RowInputY(1), 330, 180));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(2), UiStyle::LabelWidth, UiStyle::LabelH, L"Model folder");
        AddRecognitionControl(control);
        HWND modelDir = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        UiStyle::InputLeft, UiStyle::RowInputY(2), UiStyle::InputW, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_MODEL_DIR)), g_instance, nullptr);
        ApplyUiFont(modelDir);
        AddRecognitionControl(modelDir);
        AddRecognitionControl(CreateButton(hwnd, IDC_BROWSE, UiStyle::SideBtnX, UiStyle::RowInputY(2) - 1, UiStyle::SideBtnW, UiStyle::BtnH, L"Browse..."));
        AddRecognitionControl(CreateButton(hwnd, IDC_DOWNLOAD_MODELS, UiStyle::SideBtnX, UiStyle::RowInputY(2) + UiStyle::EditH + 7, UiStyle::SideBtnW, UiStyle::BtnH, L"Download"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(3), UiStyle::LabelWidth, UiStyle::LabelH, L"Threads");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_THREADS, UiStyle::InputLeft, UiStyle::RowInputY(3), 130, UiStyle::ComboH));
        HWND vad = CreateWindowW(L"BUTTON", L"Enable VAD", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                 358, UiStyle::RowInputY(3) + 4, 140, UiStyle::LabelH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VAD)), g_instance, nullptr);
        HWND partial = CreateWindowW(L"BUTTON", L"Partial result", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                     518, UiStyle::RowInputY(3) + 4, 160, UiStyle::LabelH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PARTIAL)), g_instance, nullptr);
        ApplyUiFont(vad);
        ApplyUiFont(partial);
        AddRecognitionControl(vad);
        AddRecognitionControl(partial);

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(4), UiStyle::LabelWidth, UiStyle::LabelH, L"VAD model");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_VAD_MODEL, UiStyle::InputLeft, UiStyle::RowInputY(4), UiStyle::ComboW, UiStyle::ComboH));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(5), UiStyle::LabelWidth, UiStyle::LabelH, L"Punctuation");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateCombo(hwnd, IDC_POSTPROCESS, UiStyle::InputLeft, UiStyle::RowInputY(5), UiStyle::ComboW, UiStyle::ComboH));

        const int shortcutY = UiStyle::RowInputY(5) + 30;
        HWND shortcutGroup = CreateWindowW(L"BUTTON", L"Shortcut", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                                           30, shortcutY, 788, 170, hwnd, nullptr, g_instance, nullptr);
        ApplyUiFont(shortcutGroup);
        AddRecognitionControl(shortcutGroup);

        control = CreateLabel(hwnd, UiStyle::ContentLeft, shortcutY + 28, UiStyle::LabelWidth, UiStyle::LabelH, L"Hold hotkey");
        AddRecognitionControl(control);
        AddRecognitionControl(CreateHotkeyEdit(hwnd, IDC_HOTKEY, UiStyle::InputLeft, shortcutY + 20, 300, UiStyle::HotkeyEditH, CurrentConfiguredHotkey()));
        control = CreateLabel(hwnd, UiStyle::ContentLeft, shortcutY + 68, 740, UiStyle::LabelH, L"Click the field, then press the key or key combination to use while recording.");
        AddRecognitionControl(control);
        control = CreateLabel(hwnd, UiStyle::ContentLeft, shortcutY + 98, 740, UiStyle::LabelH, L"Esc cancels recording a shortcut. Backspace/Delete clears it.");
        AddRecognitionControl(control);

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(0), UiStyle::LabelWidth, UiStyle::LabelH, L"Provider");
        AddLlmControl(control);
        AddLlmControl(CreateCombo(hwnd, IDC_LLM_PROVIDER, UiStyle::InputLeft, UiStyle::RowInputY(0), 480, 400));
        AddLlmControl(CreateButton(hwnd, IDC_LLM_PROVIDER_ADD, UiStyle::SideBtnX, UiStyle::RowInputY(0) - 1, 44, UiStyle::BtnH, L"+"));
        AddLlmControl(CreateButton(hwnd, IDC_LLM_PROVIDER_DEL, UiStyle::SideBtnX + 48, UiStyle::RowInputY(0) - 1, 44, UiStyle::BtnH, L"\u2212"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(1), UiStyle::LabelWidth, UiStyle::LabelH, L"API Base URL");
        AddLlmControl(control);
        HWND llmEndpoint = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                           UiStyle::InputLeft, UiStyle::RowInputY(1), UiStyle::InputWFull, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_ENDPOINT)), g_instance, nullptr);
        ApplyUiFont(llmEndpoint);
        AddLlmControl(llmEndpoint);

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(2), UiStyle::LabelWidth, UiStyle::LabelH, L"API Key");
        AddLlmControl(control);
        HWND llmKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                      UiStyle::InputLeft, UiStyle::RowInputY(2), UiStyle::InputW, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_KEY)), g_instance, nullptr);
        ApplyUiFont(llmKey);
        AddLlmControl(llmKey);
        AddLlmControl(CreateButton(hwnd, IDC_LLM_SHOW_KEY, UiStyle::SideBtnX, UiStyle::RowInputY(2) - 1, UiStyle::SideBtnW, UiStyle::BtnH, L"Show"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(3), UiStyle::LabelWidth, UiStyle::LabelH, L"Model");
        AddLlmControl(control);
        HWND llmModel = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        UiStyle::InputLeft, UiStyle::RowInputY(3), UiStyle::InputWFull, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_MODEL)), g_instance, nullptr);
        ApplyUiFont(llmModel);
        AddLlmControl(llmModel);

        AddLlmControl(CreateButton(hwnd, IDC_LLM_TEST, UiStyle::InputLeft, UiStyle::RowInputY(4), UiStyle::ActionBtnW, UiStyle::ActionBtnH, L"Test Connection"));
        HWND llmDebug = CreateWindowW(L"BUTTON", L"Log refine before/after", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                      348, UiStyle::RowInputY(4) + 6, 220, UiStyle::CheckH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_DEBUG)), g_instance, nullptr);
        ApplyUiFont(llmDebug);
        AddLlmControl(llmDebug);

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(5), UiStyle::LabelWidth, UiStyle::LabelH, L"Extra Params");
        AddLlmControl(control);
        HWND llmExtra = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                                        UiStyle::InputLeft, UiStyle::RowInputY(5), UiStyle::InputW, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_EXTRA)), g_instance, nullptr);
        ApplyUiFont(llmExtra);
        AddLlmControl(llmExtra);
        AddLlmControl(CreateButton(hwnd, IDC_LLM_EXTRA_RESET, UiStyle::SideBtnX, UiStyle::RowInputY(5) - 1, UiStyle::SideBtnW, UiStyle::BtnH, L"Reset"));
        {
            HWND hint = CreateWindowW(L"STATIC",
                L"JSON snippet merged into request body. e.g. \"thinking\":{\"type\":\"disabled\"}",
                WS_CHILD | WS_VISIBLE, UiStyle::InputLeft, UiStyle::RowInputY(5) + UiStyle::EditH + 2, UiStyle::InputW, 20, hwnd, nullptr, g_instance, nullptr);
            ApplyUiFont(hint);
            AddLlmControl(hint);
        }

        AddPromptControl(CreateButton(hwnd, IDC_LLM_PRESET1, UiStyle::InputLeft, UiStyle::RowInputY(0), 120, UiStyle::EditH, L"Basic Fix"));
        AddPromptControl(CreateButton(hwnd, IDC_LLM_PRESET2, UiStyle::InputLeft + 140, UiStyle::RowInputY(0), 120, UiStyle::EditH, L"Deep Fix"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(1), UiStyle::LabelWidth, UiStyle::LabelH, L"System Prompt");
        AddPromptControl(control);
        HWND llmPrompt = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                                          WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN,
                                          UiStyle::InputLeft, UiStyle::RowInputY(1), UiStyle::InputWFull, 340, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LLM_PROMPT)), g_instance, nullptr);
        ApplyUiFont(llmPrompt);
        AddPromptControl(llmPrompt);

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(0), UiStyle::LabelWidth, UiStyle::LabelH, L"Provider");
        AddCloudAsrControl(control);
        AddCloudAsrControl(CreateCombo(hwnd, IDC_CLOUD_PROVIDER, UiStyle::InputLeft, UiStyle::RowInputY(0), 300, UiStyle::ComboH));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(1), 200, UiStyle::LabelH, L"\u2500\u2500 \u767E\u5EA6\u667A\u80FD\u4E91 \u2500\u2500");
        AddCloudAsrControl(control);
        g_cloudSectionLabel = control;

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(2), UiStyle::LabelWidth, UiStyle::LabelH, L"API Key");
        AddBaiduControl(control);
        HWND baiduApiKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                           UiStyle::InputLeft, UiStyle::RowInputY(2), 330, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BAIDU_API_KEY)), g_instance, nullptr);
        ApplyUiFont(baiduApiKey);
        AddBaiduControl(baiduApiKey);
        AddBaiduControl(CreateButton(hwnd, IDC_BAIDU_SHOW_API_KEY, UiStyle::SmallBtnX, UiStyle::RowInputY(2) - 1, UiStyle::SmallBtnW, UiStyle::BtnH, L"Show"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(3), UiStyle::LabelWidth, UiStyle::LabelH, L"Secret Key");
        AddBaiduControl(control);
        HWND baiduSecretKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                              UiStyle::InputLeft, UiStyle::RowInputY(3), 330, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BAIDU_SECRET_KEY)), g_instance, nullptr);
        ApplyUiFont(baiduSecretKey);
        AddBaiduControl(baiduSecretKey);
        AddBaiduControl(CreateButton(hwnd, IDC_BAIDU_SHOW_KEY, UiStyle::SmallBtnX, UiStyle::RowInputY(3) - 1, UiStyle::SmallBtnW, UiStyle::BtnH, L"Show"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(4), UiStyle::LabelWidth, UiStyle::LabelH, L"Language Model");
        AddBaiduControl(control);
        AddBaiduControl(CreateCombo(hwnd, IDC_BAIDU_DEV_PID, UiStyle::InputLeft, UiStyle::RowInputY(4), UiStyle::ComboW, UiStyle::ComboH));

        AddBaiduControl(CreateButton(hwnd, IDC_BAIDU_TEST, UiStyle::InputLeft, UiStyle::RowInputY(5), UiStyle::ActionBtnW, UiStyle::ActionBtnH, L"Test Connection"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(2), UiStyle::LabelWidth, UiStyle::LabelH, L"API Key (X-Api-Key)");
        AddVolcengineControl(control);
        HWND volcApiKey = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_PASSWORD,
                                          UiStyle::InputLeft, UiStyle::RowInputY(2), 330, UiStyle::EditH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_API_KEY)), g_instance, nullptr);
        ApplyUiFont(volcApiKey);
        AddVolcengineControl(volcApiKey);
        AddVolcengineControl(CreateButton(hwnd, IDC_VOLC_SHOW_KEY, UiStyle::SmallBtnX, UiStyle::RowInputY(2) - 1, UiStyle::SmallBtnW, UiStyle::BtnH, L"Show"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(3), UiStyle::LabelWidth, UiStyle::LabelH, L"ASR Mode");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateCombo(hwnd, IDC_VOLC_MODE, UiStyle::InputLeft, UiStyle::RowInputY(3), 300, UiStyle::ComboH));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(4), UiStyle::LabelWidth, UiStyle::LabelH, L"Model Version");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateCombo(hwnd, IDC_VOLC_RESOURCE, UiStyle::InputLeft, UiStyle::RowInputY(4), 300, UiStyle::ComboH));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowLabelY(5), UiStyle::LabelWidth, UiStyle::LabelH, L"Language (optional)");
        AddVolcengineControl(control);
        AddVolcengineControl(CreateCombo(hwnd, IDC_VOLC_LANGUAGE, UiStyle::InputLeft, UiStyle::RowInputY(5), UiStyle::ComboW, UiStyle::ComboH));

        AddVolcengineControl(CreateButton(hwnd, IDC_VOLC_TEST, UiStyle::InputLeft, UiStyle::RowInputY(6), UiStyle::ActionBtnW, UiStyle::ActionBtnH, L"Test Connection"));

        control = CreateLabel(hwnd, UiStyle::ContentLeft, UiStyle::RowInputY(6) + 42, 640, 50,
            L"Cloud ASR sends audio to remote servers.\nAll keys are encrypted with DPAPI locally.");
        AddCloudAsrControl(control);

        HWND status = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                    UiStyle::Margin * 2, 530, 520, UiStyle::LabelH, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), g_instance, nullptr);
        ApplyUiFont(status);
        CreateButton(hwnd, IDC_SAVE, 626, 522, UiStyle::FooterBtnW, UiStyle::ActionBtnH, L"Save");
        CreateButton(hwnd, IDC_CANCEL, 726, 522, UiStyle::FooterBtnW, UiStyle::ActionBtnH, L"Close");
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
                    std::wstring newDir = DefaultModelDir(g_config.modelId);
                    SetWindowTextW(GetDlgItem(hwnd, IDC_MODEL_DIR), newDir.c_str());
                    g_config.modelDir = newDir;
                    MessageBoxW(hwnd, L"Download complete!", L"Success", MB_OK | MB_ICONINFORMATION);
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
        case IDC_LLM_PRESET1:
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PROMPT), llm::kPresetBasicFix);
            return 0;
        case IDC_LLM_PRESET2:
            SetWindowTextW(GetDlgItem(hwnd, IDC_LLM_PROMPT), llm::kPresetDeepFix);
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
        case IDC_CLOUD_PROVIDER:
            if (HIWORD(wParam) == CBN_SELCHANGE) {
                int idx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_CLOUD_PROVIDER));
                ShowCloudSubPage(hwnd, idx);
                return 0;
            }
            break;
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
            if (modeIdx == 1) vcfg.mode = L"bigmodel";
            else if (modeIdx == 2) vcfg.mode = L"bigmodel_async";
            else vcfg.mode = L"bigmodel_nostream";
            int resIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_RESOURCE));
            if (resIdx == 1) vcfg.resourceId = L"volc.seedasr.sauc.concurrent";
            else vcfg.resourceId = L"volc.seedasr.sauc.duration";
            int langIdx = ComboBox_GetCurSel(GetDlgItem(hwnd, IDC_VOLC_LANGUAGE));
            if (langIdx >= 0 && langIdx < 9) vcfg.language = kVolcLanguages[langIdx];
            SetStatus(hwnd, L"Testing Volcano Engine ASR connection...");
            std::thread([hwnd, vcfg]() {
                volc_asr::TestResult result = volc_asr::TestConnection(vcfg);
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
            680,
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
