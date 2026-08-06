#pragma once

#include "globals.h"
#include "selection_context.h"

void SetStatus(HWND hwnd, const std::wstring& text);
void SetClipboardText(const std::wstring& text);
void SendCtrlV();
void SendUnicodeText(const std::wstring& text);
void PasteTextImeAware(const std::wstring& text);
bool ReplaceSelectionTextImeAware(const SelectionContext& selection,
                                  const std::wstring& text,
                                  std::wstring* error = nullptr);
bool IsCapsLockOn();
void SendCapsLockTap();
void RestoreCapsLockState();

void AddRecognitionControl(HWND hwnd);
void AddShortcutControl(HWND hwnd);
void AddLlmControl(HWND hwnd);
void AddPromptControl(HWND hwnd);
void AddCloudAsrControl(HWND hwnd);
void AddBaiduControl(HWND hwnd);
void AddVolcengineControl(HWND hwnd);
void ShowCloudSubPage(HWND hwnd, int providerIdx);
void ShowSettingsPage(HWND hwnd, int page);
void LayoutSettingsWindow(HWND hwnd);
void HideSettingsWindow(HWND hwnd);

HWND CreateLabel(HWND parent, int x, int y, int w, int h, const wchar_t* text);
HWND CreateCombo(HWND parent, int id, int x, int y, int w, int h);
HWND CreateButton(HWND parent, int id, int x, int y, int w, int h, const wchar_t* text);
void BrowseModelDirectory(HWND hwnd);
void RefreshProviderDropdown(HWND hwnd);
void LoadSettingsControls(HWND hwnd);
std::wstring ComboText(HWND combo);
void SaveSettingsControls(HWND hwnd);
void TestLlmConnection(HWND hwnd);
void DeleteProviderFromStore(const std::wstring& name);

constexpr int IDC_INPUT_EDIT = 3001;

struct InputDlgData {
    const wchar_t* title;
    std::wstring result;
    bool ok = false;
};

LRESULT CALLBACK InputWndProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
bool ShowInputDialog(HWND parent, const wchar_t* title, std::wstring& out);

constexpr int IDC_VOLC_EXTRA_EDIT = 3002;
constexpr int IDC_VOLC_EXTRA_HOTWORDS = 3003;
constexpr int IDC_VOLC_EXTRA_CONTEXT = 3004;
constexpr int IDC_VOLC_EXTRA_RESET = 3005;

struct VolcExtraDlgData {
    std::wstring text;
    bool ok = false;
};

LRESULT CALLBACK VolcExtraWndProc(HWND hDlg, UINT msg, WPARAM wParam, LPARAM lParam);
bool ShowVolcExtraDialog(HWND parent, std::wstring& out);

LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
void ShowSettingsWindow(HWND owner);
