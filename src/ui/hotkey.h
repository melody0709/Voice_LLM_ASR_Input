#pragma once

#include "globals.h"

bool IsModifierKey(UINT vk);
UINT NormalizedKeyFromWParam(WPARAM wParam);
UINT NormalizedKeyFromKeyMessage(WPARAM wParam, LPARAM lParam);
std::wstring KeyName(UINT key);
std::wstring HotkeyToString(const HotkeyConfig& hotkey);
HotkeyConfig HotkeyFromString(const std::wstring& text);
HotkeyConfig CurrentConfiguredHotkey();
bool ModifiersMatch(const HotkeyConfig& hotkey);

void ResetCapsLockHotkeyState();
void StartCapsLockHotkeyPress();
void ActivateCapsLockLongPress();
void FinishCapsLockHotkeyPress();

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wParam, LPARAM lParam);
void InstallKeyboardHook();
void UninstallKeyboardHook();

void ApplyUiFont(HWND hwnd, HFONT font = nullptr);
LRESULT CALLBACK HotkeyEditWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
HWND CreateHotkeyEdit(HWND parent, int id, int x, int y, int w, int h, const HotkeyConfig& initial);
HotkeyConfig GetHotkeyFromEdit(HWND parent, int id);
