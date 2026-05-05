#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "hotkey.h"
#include "engine.h"
#include "settings.h"

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "user32.lib")

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

void ApplyUiFont(HWND hwnd, HFONT font) {
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
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, kHotkeyEditClass, nullptr, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
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
