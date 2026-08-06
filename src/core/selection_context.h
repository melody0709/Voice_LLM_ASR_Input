#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <objidl.h>

#include <algorithm>
#include <cwctype>
#include <cstring>
#include <string>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "ole32.lib")

// A snapshot of the text selection that existed when a recognition attempt
// started.  The HWNDs are deliberately kept with the text so a delayed cloud
// result cannot be pasted into whichever application happens to be focused
// later.
struct SelectionContext {
    bool hasSelection = false;
    bool capturedWithUiAutomation = false;
    bool capturedWithClipboard = false;
    std::wstring selectedText;
    HWND targetWindow = nullptr;  // top-level foreground window
    HWND focusWindow = nullptr;   // focused editor/control, when available
    DWORD processId = 0;
    DWORD focusThreadId = 0;
    std::string error;

    bool HasCapturedSelection() const {
        return hasSelection && !selectedText.empty();
    }

    bool Usable() const {
        return HasCapturedSelection() && IsWindow(targetWindow);
    }
};

namespace selection_context {

constexpr size_t kMaxSelectionChars = 200000;

inline HWND TopLevelWindow(HWND hwnd) {
    if (!hwnd) return nullptr;
    HWND root = GetAncestor(hwnd, GA_ROOT);
    return root ? root : hwnd;
}

inline bool HasNonWhitespace(const std::wstring& text) {
    return std::any_of(text.begin(), text.end(), [](wchar_t ch) {
        return !iswspace(ch);
    });
}

inline bool ReadClipboardText(std::wstring& out) {
    out.clear();
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!OpenClipboard(nullptr)) {
            Sleep(5);
            continue;
        }
        HANDLE handle = GetClipboardData(CF_UNICODETEXT);
        if (handle) {
            const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(handle));
            if (text) {
                out.assign(text);
                GlobalUnlock(handle);
            }
        }
        CloseClipboard();
        return handle != nullptr;
    }
    return false;
}

// Keep the original IDataObject alive while WM_COPY temporarily replaces the
// clipboard. Unlike copying only CF_UNICODETEXT, this preserves images, file
// lists, HTML/RTF and application-private formats without guessing how each
// clipboard handle must be cloned.
class ClipboardSnapshot {
public:
    ClipboardSnapshot() {
        const HRESULT hr = OleInitialize(nullptr);
        shouldUninitialize_ = SUCCEEDED(hr);
        if (FAILED(hr) && hr != S_FALSE) return;
        readable_ = SUCCEEDED(OleGetClipboard(&dataObject_)) && dataObject_;
        if (!readable_ && OpenClipboard(nullptr)) {
            wasEmpty_ = CountClipboardFormats() == 0;
            readable_ = wasEmpty_;
            CloseClipboard();
        }
    }

    ~ClipboardSnapshot() {
        Restore();
        if (dataObject_) dataObject_->Release();
        if (shouldUninitialize_) OleUninitialize();
    }

    ClipboardSnapshot(const ClipboardSnapshot&) = delete;
    ClipboardSnapshot& operator=(const ClipboardSnapshot&) = delete;

    bool Readable() const { return readable_; }

    void Restore() {
        if (restored_) return;
        restored_ = true;
        if (!readable_) return;
        if (wasEmpty_) {
            if (OpenClipboard(nullptr)) {
                EmptyClipboard();
                CloseClipboard();
            }
            return;
        }
        if (!dataObject_) return;
        if (SUCCEEDED(OleSetClipboard(dataObject_))) {
            // Materialize delayed-rendered formats so releasing our snapshot
            // cannot invalidate the restored clipboard contents.
            OleFlushClipboard();
        }
    }

private:
    IDataObject* dataObject_ = nullptr;
    bool readable_ = false;
    bool restored_ = false;
    bool wasEmpty_ = false;
    bool shouldUninitialize_ = false;
};

inline bool SetClipboardText(const std::wstring& text) {
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (!OpenClipboard(nullptr)) {
            Sleep(5);
            continue;
        }
        EmptyClipboard();
        const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (!memory) {
            CloseClipboard();
            return false;
        }
        void* destination = GlobalLock(memory);
        if (!destination) {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }
        memcpy(destination, text.c_str(), bytes);
        GlobalUnlock(memory);
        if (!SetClipboardData(CF_UNICODETEXT, memory)) {
            GlobalFree(memory);
            CloseClipboard();
            return false;
        }
        CloseClipboard();
        return true;
    }
    return false;
}

inline bool TryReadSelectionWithUiAutomation(HWND focusWindow,
                                             std::wstring& selectedText,
                                             std::string& error) {
    selectedText.clear();
    error.clear();

    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool shouldUninitialize = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        error = "COM_INIT_FAILED";
        return false;
    }

    IUIAutomation* automation = nullptr;
    IUIAutomationElement* focused = nullptr;
    IUIAutomationTextPattern* textPattern = nullptr;
    IUIAutomationTextRangeArray* ranges = nullptr;
    bool ok = false;

    do {
        HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                       CLSCTX_INPROC_SERVER,
                                       IID_PPV_ARGS(&automation));
        if (FAILED(hr) || !automation) {
            error = "UIA_CREATE_FAILED";
            break;
        }
        hr = automation->GetFocusedElement(&focused);
        if (FAILED(hr) || !focused) {
            error = "UIA_NO_FOCUS";
            break;
        }

        UIA_HWND nativeWindowValue = nullptr;
        focused->get_CurrentNativeWindowHandle(&nativeWindowValue);
        HWND nativeWindow = static_cast<HWND>(nativeWindowValue);
        if (focusWindow && nativeWindow &&
            TopLevelWindow(nativeWindow) != TopLevelWindow(focusWindow)) {
            error = "UIA_FOCUS_MISMATCH";
            break;
        }

        hr = focused->GetCurrentPatternAs(UIA_TextPatternId,
                                          IID_PPV_ARGS(&textPattern));
        if (FAILED(hr) || !textPattern) {
            error = "UIA_TEXT_PATTERN_UNAVAILABLE";
            break;
        }
        hr = textPattern->GetSelection(&ranges);
        if (FAILED(hr) || !ranges) {
            error = "UIA_SELECTION_UNAVAILABLE";
            break;
        }

        int count = 0;
        ranges->get_Length(&count);
        for (int index = 0; index < count; ++index) {
            IUIAutomationTextRange* range = nullptr;
            if (FAILED(ranges->GetElement(index, &range)) || !range) continue;
            BSTR text = nullptr;
            if (SUCCEEDED(range->GetText(-1, &text)) && text) {
                selectedText.append(text, SysStringLen(text));
                SysFreeString(text);
            }
            range->Release();
        }

        if (selectedText.size() > kMaxSelectionChars) {
            selectedText.clear();
            error = "SELECTION_TOO_LARGE";
            break;
        }
        if (!HasNonWhitespace(selectedText)) {
            selectedText.clear();
            error = "NO_NONWHITESPACE_SELECTION";
            break;
        }
        ok = true;
    } while (false);

    if (ranges) ranges->Release();
    if (textPattern) textPattern->Release();
    if (focused) focused->Release();
    if (automation) automation->Release();
    if (shouldUninitialize) CoUninitialize();
    return ok;
}

inline bool TryReadSelectionWithClipboard(HWND targetWindow,
                                          HWND focusWindow,
                                          std::wstring& selectedText,
                                          std::string& error) {
    selectedText.clear();
    error.clear();
    ClipboardSnapshot snapshot;
    if (!snapshot.Readable()) {
        error = "CLIPBOARD_BUSY";
        return false;
    }

    HWND copyTarget = IsWindow(focusWindow) ? focusWindow : targetWindow;
    DWORD_PTR ignored = 0;
    bool sent = false;
    if (copyTarget) {
        sent = SendMessageTimeoutW(copyTarget, WM_COPY, 0, 0,
                                   SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                   500, &ignored) != 0;
    }
    if (!sent && targetWindow && targetWindow != copyTarget) {
        sent = SendMessageTimeoutW(targetWindow, WM_COPY, 0, 0,
                                   SMTO_ABORTIFHUNG | SMTO_BLOCK,
                                   500, &ignored) != 0;
    }

    bool ok = sent && ReadClipboardText(selectedText) &&
              selectedText.size() <= kMaxSelectionChars &&
              HasNonWhitespace(selectedText);
    snapshot.Restore();
    if (!ok) {
        selectedText.clear();
        error = sent ? "WM_COPY_NO_TEXT" : "WM_COPY_FAILED";
    }
    return ok;
}

inline SelectionContext Capture() {
    SelectionContext result;
    HWND foreground = GetForegroundWindow();
    result.targetWindow = TopLevelWindow(foreground);
    if (!result.targetWindow) {
        result.error = "NO_FOREGROUND_WINDOW";
        return result;
    }
    result.focusThreadId = GetWindowThreadProcessId(result.targetWindow,
                                                    &result.processId);

    GUITHREADINFO guiInfo = {};
    guiInfo.cbSize = sizeof(guiInfo);
    if (GetGUIThreadInfo(result.focusThreadId, &guiInfo) && guiInfo.hwndFocus) {
        result.focusWindow = guiInfo.hwndFocus;
    } else {
        result.focusWindow = foreground;
    }

    std::string uiaError;
    if (TryReadSelectionWithUiAutomation(result.focusWindow,
                                         result.selectedText, uiaError)) {
        result.hasSelection = true;
        result.capturedWithUiAutomation = true;
        return result;
    }

    std::string copyError;
    if (TryReadSelectionWithClipboard(result.targetWindow,
                                      result.focusWindow,
                                      result.selectedText, copyError)) {
        result.hasSelection = true;
        result.capturedWithClipboard = true;
        return result;
    }

    result.error = copyError.empty() ? uiaError : copyError;
    if (result.error.empty()) result.error = "SELECTION_NOT_AVAILABLE";
    return result;
}

inline bool IsSameForegroundWindow(const SelectionContext& context) {
    if (!context.Usable()) return false;
    return TopLevelWindow(GetForegroundWindow()) == context.targetWindow;
}

inline bool VerifyCurrentUiAutomationSelection(const SelectionContext& context) {
    if (!context.capturedWithUiAutomation) return true;
    std::wstring current;
    std::string error;
    if (!TryReadSelectionWithUiAutomation(context.focusWindow, current, error)) {
        return false;
    }
    return current == context.selectedText;
}

inline bool VerifyCurrentClipboardSelection(const SelectionContext& context) {
    if (!context.capturedWithClipboard) return true;
    if (!context.Usable()) return false;

    std::wstring current;
    std::string error;
    if (!TryReadSelectionWithClipboard(context.targetWindow,
                                       context.focusWindow,
                                       current, error)) {
        return false;
    }
    return current == context.selectedText;
}

inline bool VerifyCurrentSelection(const SelectionContext& context) {
    return VerifyCurrentUiAutomationSelection(context) &&
           VerifyCurrentClipboardSelection(context);
}

} // namespace selection_context
