#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <uiautomation.h>
#include <oleacc.h>

#include <string>
#include <set>
#include <thread>
#include <future>
#include <atomic>
#include <chrono>
#include <algorithm>
#include <limits>

#pragma comment(lib, "uiautomationcore.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "oleaut32.lib")

struct InputContextResult {
    int successLayer = -1;
    std::wstring windowTitle;
    std::wstring inputFieldText;
    double elapsedMs = 0.0;
    bool timedOut = false;
    std::string failReason;
    int textLength = 0;
    std::string controlType;
    std::string focusWindowClass;
    bool isPassword = false;
};

namespace input_context {

inline const char* LayerName(int layer) {
    switch (layer) {
        case -1: return "NONE";
        case 0:  return "L0_TITLE";
        case 1:  return "L1_WM_GETTEXT";
        case 2:  return "L2_UIA_VALUE";
        case 3:  return "L2.5_TEXTPATTERN";
        case 4:  return "L2.7_MSAA";
        case 5:  return "L3_TEXTPATTERN2";
        default: return "UNKNOWN";
    }
}

inline std::wstring TruncateForDisplay(const std::wstring& text, size_t maxLen = 80) {
    if (text.size() <= maxLen) return text;
    return L"..." + text.substr(text.size() - maxLen + 3);
}

inline bool IsHighSurrogate(wchar_t ch) {
    return ch >= 0xD800 && ch <= 0xDBFF;
}

inline bool IsLowSurrogate(wchar_t ch) {
    return ch >= 0xDC00 && ch <= 0xDFFF;
}

// `std::wstring` is UTF-16 on Windows.  UI Automation reports character
// counts in a way that is usually compatible with UTF-16 code units, but a
// suffix/prefix cut must still avoid returning a lone surrogate.  Keeping
// these helpers here lets all provider-specific limits share the same safe
// boundary behavior.
inline std::wstring TakeFirstN(const std::wstring& text, size_t n) {
    if (text.size() <= n) return text;
    size_t end = n;
    if (end > 0 && end < text.size() && IsHighSurrogate(text[end - 1]) &&
        IsLowSurrogate(text[end])) {
        --end;
    }
    return text.substr(0, end);
}

inline std::wstring TakeLastN(const std::wstring& text, size_t n) {
    if (text.size() <= n) return text;
    size_t start = text.size() - n;
    if (start > 0 && start < text.size() && IsLowSurrogate(text[start]) &&
        IsHighSurrogate(text[start - 1])) {
        ++start;
    }
    return text.substr(start);
}

inline std::set<HWND> s_triggeredWindows;

inline void EnsureAccessibilityTree(HWND hwnd) {
    if (!hwnd) return;
    if (s_triggeredWindows.count(hwnd)) return;
    SendMessageW(hwnd, WM_GETOBJECT, 0, (LPARAM)0xFFFFFFFC);
    s_triggeredWindows.insert(hwnd);
}

inline std::wstring GetForegroundWindowTitle() {
    HWND fgWnd = GetForegroundWindow();
    if (!fgWnd) return L"";
    wchar_t title[256] = {};
    GetWindowTextW(fgWnd, title, 256);
    return title;
}

inline std::string GetControlTypeName(CONTROLTYPEID ct) {
    switch (ct) {
        case UIA_EditControlTypeId: return "Edit";
        case UIA_DocumentControlTypeId: return "Document";
        case UIA_TextControlTypeId: return "Text";
        case UIA_WindowControlTypeId: return "Window";
        case UIA_PaneControlTypeId: return "Pane";
        case UIA_CustomControlTypeId: return "Custom";
        default: return "Type" + std::to_string(ct);
    }
}

inline bool IsPasswordElement(IUIAutomationElement* pElement) {
    VARIANT var;
    HRESULT hr = pElement->GetCurrentPropertyValue(UIA_IsPasswordPropertyId, &var);
    if (SUCCEEDED(hr) && var.vt == VT_BOOL && var.boolVal == VARIANT_TRUE) {
        VariantClear(&var);
        return true;
    }
    VariantClear(&var);
    return false;
}

inline bool TryGetValueText(IUIAutomationElement* pElement,
                            size_t maxTextCharacters,
                            InputContextResult& result) {
    if (IsPasswordElement(pElement)) {
        result.isPassword = true;
        result.failReason = "PASSWORD";
        return false;
    }

    VARIANT varValue;
    HRESULT hr = pElement->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &varValue);
    if (SUCCEEDED(hr) && varValue.vt == VT_BSTR && varValue.bstrVal && varValue.bstrVal[0] != L'\0') {
        std::wstring text = varValue.bstrVal;
        VariantClear(&varValue);
        result.textLength = static_cast<int>(text.size());
        result.inputFieldText = TakeLastN(text, maxTextCharacters);
        result.successLayer = 2;
        return true;
    }
    VariantClear(&varValue);
    return false;
}

inline bool TryTextPatternFromPoint(IUIAutomationElement* pElement, POINT pt,
                                    size_t maxTextCharacters,
                                    InputContextResult& result) {
    IUIAutomationTextPattern* pTP = nullptr;
    HRESULT hr = pElement->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pTP));
    if (FAILED(hr) || !pTP) return false;

    IUIAutomationTextRange* pRange = nullptr;
    hr = pTP->RangeFromPoint(pt, &pRange);
    if (SUCCEEDED(hr) && pRange) {
        int moved = 0;
        pRange->Move(TextUnit_Character, -100, &moved);
        BSTR text = nullptr;
        const int readCount = static_cast<int>((std::min)(
            maxTextCharacters, static_cast<size_t>((std::numeric_limits<int>::max)())));
        pRange->GetText(readCount, &text);
        pRange->Release();
        pTP->Release();

        if (text && text[0] != L'\0') {
            std::wstring wtext = text;
            SysFreeString(text);
            result.textLength = static_cast<int>(wtext.size());
            result.inputFieldText = TakeLastN(wtext, maxTextCharacters);
            result.successLayer = 3;
            return true;
        }
        if (text) SysFreeString(text);
        return false;
    }
    pTP->Release();
    return false;
}

inline bool TryTextPatternVisibleRanges(IUIAutomationElement* pElement,
                                        size_t maxTextCharacters,
                                        InputContextResult& result) {
    IUIAutomationTextPattern* pTP = nullptr;
    HRESULT hr = pElement->GetCurrentPatternAs(UIA_TextPatternId, IID_PPV_ARGS(&pTP));
    if (FAILED(hr) || !pTP) return false;

    IUIAutomationTextRangeArray* pRanges = nullptr;
    hr = pTP->GetVisibleRanges(&pRanges);
    pTP->Release();
    if (FAILED(hr) || !pRanges) return false;

    int count = 0;
    pRanges->get_Length(&count);
    if (count == 0) {
        pRanges->Release();
        return false;
    }

    std::wstring allText;
    for (int i = 0; i < count; i++) {
        IUIAutomationTextRange* pRange = nullptr;
        pRanges->GetElement(i, &pRange);
        if (!pRange) continue;
        BSTR text = nullptr;
        const size_t readLimit = (std::max)(maxTextCharacters, static_cast<size_t>(1));
        const int readCount = static_cast<int>((std::min)(
            readLimit, static_cast<size_t>((std::numeric_limits<int>::max)())));
        pRange->GetText(readCount, &text);
        if (text) {
            allText += text;
            SysFreeString(text);
        }
        pRange->Release();
        if (allText.size() >= maxTextCharacters) break;
    }
    pRanges->Release();

    if (allText.empty()) return false;
    result.textLength = static_cast<int>(allText.size());
    result.inputFieldText = TakeLastN(allText, maxTextCharacters);
    result.successLayer = 3;
    return true;
}

inline bool TryTextPattern2Caret(IUIAutomationElement* pElement,
                                 size_t maxTextCharacters,
                                 InputContextResult& result) {
    IUIAutomationTextPattern2* pTP2 = nullptr;
    HRESULT hr = pElement->GetCurrentPatternAs(UIA_TextPattern2Id, IID_PPV_ARGS(&pTP2));
    if (FAILED(hr) || !pTP2) return false;

    IUIAutomationTextRange* pCaretRange = nullptr;
    BOOL isActive = FALSE;
    hr = pTP2->GetCaretRange(&isActive, &pCaretRange);
    if (FAILED(hr) || !pCaretRange) {
        pTP2->Release();
        return false;
    }

    int moved = 0;
    pCaretRange->Move(TextUnit_Character, -100, &moved);
    BSTR text = nullptr;
    const int readCount = static_cast<int>((std::min)(
        maxTextCharacters, static_cast<size_t>((std::numeric_limits<int>::max)())));
    pCaretRange->GetText(readCount, &text);
    pCaretRange->Release();
    pTP2->Release();

    if (text && text[0] != L'\0') {
        std::wstring wtext = text;
        SysFreeString(text);
        result.textLength = static_cast<int>(wtext.size());
        result.inputFieldText = TakeLastN(wtext, maxTextCharacters);
        result.successLayer = 5;
        return true;
    }
    if (text) SysFreeString(text);
    return false;
}

inline bool TryReadFromElement(IUIAutomationElement* pElement, POINT pt, bool hasPt,
                               size_t maxTextCharacters,
                               InputContextResult& result) {
    if (!pElement) return false;

    CONTROLTYPEID ct = 0;
    pElement->get_CurrentControlType(&ct);
    result.controlType = GetControlTypeName(ct);

    if (TryGetValueText(pElement, maxTextCharacters, result)) return true;

    if (hasPt && TryTextPatternFromPoint(pElement, pt, maxTextCharacters, result)) return true;

    if (TryTextPatternVisibleRanges(pElement, maxTextCharacters, result)) return true;

    if (TryTextPattern2Caret(pElement, maxTextCharacters, result)) return true;

    return false;
}

inline bool TryWalkParentsForText(IUIAutomation* pAutomation,
                                   IUIAutomationElement* pStart,
                                   POINT pt, bool hasPt,
                                   size_t maxTextCharacters,
                                   InputContextResult& result) {
    IUIAutomationTreeWalker* pWalker = nullptr;
    HRESULT hr = pAutomation->get_ControlViewWalker(&pWalker);
    if (FAILED(hr) || !pWalker) return false;

    IUIAutomationElement* pCurrent = pStart;
    pCurrent->AddRef();

    for (int depth = 0; depth < 8; depth++) {
        IUIAutomationElement* pParent = nullptr;
        hr = pWalker->GetParentElement(pCurrent, &pParent);
        pCurrent->Release();
        pCurrent = nullptr;

        if (FAILED(hr) || !pParent) break;

        CONTROLTYPEID parentCt = 0;
        pParent->get_CurrentControlType(&parentCt);

        if (parentCt == UIA_WindowControlTypeId) {
            pParent->Release();
            pCurrent = nullptr;
            break;
        }

        if (TryReadFromElement(pParent, pt, hasPt, maxTextCharacters, result)) {
            pParent->Release();
            pWalker->Release();
            return true;
        }

        pCurrent = pParent;
    }

    if (pCurrent) pCurrent->Release();
    pWalker->Release();
    return false;
}

inline InputContextResult ReadInputFieldTextUIA(const std::wstring& windowTitle,
                                                HWND fgWnd,
                                                size_t maxTextCharacters = 200) {
    InputContextResult result;
    result.windowTitle = windowTitle;

    if (!fgWnd) {
        result.failReason = "NO_FGWND";
        return result;
    }

    DWORD fgTid = GetWindowThreadProcessId(fgWnd, nullptr);

    GUITHREADINFO guiInfo = {};
    guiInfo.cbSize = sizeof(guiInfo);
    POINT caretScreenPt = {};
    bool hasCaretPt = false;
    HWND hwndFocus = nullptr;

    if (GetGUIThreadInfo(fgTid, &guiInfo)) {
        hwndFocus = guiInfo.hwndFocus;
        if (guiInfo.hwndCaret) {
            caretScreenPt.x = guiInfo.rcCaret.left;
            caretScreenPt.y = guiInfo.rcCaret.top;
            ClientToScreen(guiInfo.hwndCaret, &caretScreenPt);
            hasCaretPt = true;
        }
    }

    if (hwndFocus) {
        wchar_t className[256] = {};
        GetClassNameW(hwndFocus, className, 256);
        char classNameUtf8[512] = {};
        WideCharToMultiByte(CP_UTF8, 0, className, -1, classNameUtf8, 512, nullptr, nullptr);
        result.focusWindowClass = classNameUtf8;

        if (_wcsicmp(className, L"Edit") == 0) {
            wchar_t buf[4096] = {};
            LRESULT sent = SendMessageTimeoutW(hwndFocus, WM_GETTEXT, 4095,
                                               (LPARAM)buf, SMTO_ABORTIFHUNG, 500, nullptr);
            if (sent != 0 && buf[0] != L'\0') {
                std::wstring text = buf;
                result.textLength = static_cast<int>(text.size());
                result.inputFieldText = TakeLastN(text, maxTextCharacters);
                result.successLayer = 1;
                return result;
            }
        }
    }

    EnsureAccessibilityTree(fgWnd);

    IUIAutomation* pAutomation = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CUIAutomation, nullptr,
                                  CLSCTX_INPROC_SERVER, IID_IUIAutomation,
                                  (void**)&pAutomation);
    if (FAILED(hr) || !pAutomation) {
        result.failReason = "UIA_CREATE_FAILED";
        return result;
    }

    POINT uiaPt = caretScreenPt;
    if (!hasCaretPt) {
        RECT fgRect = {};
        GetWindowRect(fgWnd, &fgRect);
        uiaPt.x = (fgRect.left + fgRect.right) / 2;
        uiaPt.y = fgRect.top + (fgRect.bottom - fgRect.top) * 2 / 3;
    }

    IUIAutomationElement* pFocused = nullptr;
    hr = pAutomation->GetFocusedElement(&pFocused);
    if (SUCCEEDED(hr) && pFocused) {
        if (TryReadFromElement(pFocused, uiaPt, hasCaretPt, maxTextCharacters, result)) {
            pFocused->Release();
            pAutomation->Release();
            return result;
        }

        if (TryWalkParentsForText(pAutomation, pFocused, uiaPt, hasCaretPt,
                                  maxTextCharacters, result)) {
            pFocused->Release();
            pAutomation->Release();
            return result;
        }
        pFocused->Release();
    }

    IUIAutomationElement* pFromPt = nullptr;
    hr = pAutomation->ElementFromPoint(uiaPt, &pFromPt);
    if (SUCCEEDED(hr) && pFromPt) {
        if (TryReadFromElement(pFromPt, uiaPt, hasCaretPt, maxTextCharacters, result)) {
            pFromPt->Release();
            pAutomation->Release();
            return result;
        }

        if (TryWalkParentsForText(pAutomation, pFromPt, uiaPt, hasCaretPt,
                                  maxTextCharacters, result)) {
            pFromPt->Release();
            pAutomation->Release();
            return result;
        }
        pFromPt->Release();
    }

    if (hwndFocus) {
        IAccessible* pAcc = nullptr;
        hr = AccessibleObjectFromWindow(hwndFocus, OBJID_WINDOW,
                                        IID_IAccessible, (void**)&pAcc);
        if (SUCCEEDED(hr) && pAcc) {
            VARIANT varSelf;
            varSelf.vt = VT_I4;
            varSelf.lVal = CHILDID_SELF;
            BSTR value = nullptr;
            hr = pAcc->get_accValue(varSelf, &value);
            if (SUCCEEDED(hr) && value && value[0] != L'\0') {
                std::wstring text = value;
                result.textLength = static_cast<int>(text.size());
                result.inputFieldText = TakeLastN(text, maxTextCharacters);
                result.successLayer = 4;
                SysFreeString(value);
                pAcc->Release();
                pAutomation->Release();
                return result;
            }
            if (value) SysFreeString(value);
            pAcc->Release();
        }
    }

    pAutomation->Release();
    result.failReason = "UIA_EMPTY";
    return result;
}

inline std::atomic<bool> s_uiaThreadRunning{false};

inline InputContextResult GetInputFieldContext(size_t maxTextCharacters = 200) {
    InputContextResult result;
    HWND fgWnd = GetForegroundWindow();
    result.windowTitle = GetForegroundWindowTitle();

    if (s_uiaThreadRunning.exchange(true)) {
        result.failReason = "UIA_BUSY";
        return result;
    }

    auto promise = std::make_shared<std::promise<InputContextResult>>();
    auto future = promise->get_future();

    std::wstring wt = result.windowTitle;
    std::thread worker([promise, wt, fgWnd, maxTextCharacters]() {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        InputContextResult r = ReadInputFieldTextUIA(wt, fgWnd, maxTextCharacters);
        CoUninitialize();
        promise->set_value(std::move(r));
    });
    worker.detach();

    if (future.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout) {
        result.timedOut = true;
        result.failReason = "TIMEOUT";
        s_uiaThreadRunning = false;
        return result;
    }

    InputContextResult uiaResult = future.get();
    s_uiaThreadRunning = false;
    return uiaResult;
}

}
