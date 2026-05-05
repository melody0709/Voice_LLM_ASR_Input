#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "hud.h"
#include "engine.h"

#include <algorithm>
#include <cmath>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

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

float CurrentHudLevel() {
    const float raw = g_recording ? g_audioLevel.load() : 0.0f;
    const float factor = raw > g_hudSmoothedLevel ? 0.4f : 0.15f;
    g_hudSmoothedLevel += (raw - g_hudSmoothedLevel) * factor;
    return std::clamp(g_hudSmoothedLevel, 0.0f, 1.0f);
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
    if (!g_settingsBgBrush) g_settingsBgBrush = CreateSolidBrush(UiStyle::BgColor);
    if (!g_cardBrush) g_cardBrush = CreateSolidBrush(UiStyle::ControlBgColor);
    if (!g_controlBgBrush) g_controlBgBrush = CreateSolidBrush(UiStyle::ControlBgColor);
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
    SafeRelease(g_hudBarGradientRec);
    SafeRelease(g_hudBarGradientIdle);
    SafeRelease(g_hudBarGradientStopsRec);
    SafeRelease(g_hudBarGradientStopsIdle);
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

    if (!g_hudBarGradientStopsRec && g_hudRenderTarget) {
        D2D1_GRADIENT_STOP stopsRec[] = {
            { 0.0f, D2D1::ColorF(0.102f, 0.420f, 0.541f, 1.0f) },
            { 1.0f, D2D1::ColorF(0.357f, 0.878f, 1.000f, 1.0f) },
        };
        if (FAILED(g_hudRenderTarget->CreateGradientStopCollection(stopsRec, 2, &g_hudBarGradientStopsRec))) {
            return false;
        }
        if (FAILED(g_hudRenderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)),
                g_hudBarGradientStopsRec, &g_hudBarGradientRec))) {
            return false;
        }
    }

    if (!g_hudBarGradientStopsIdle && g_hudRenderTarget) {
        D2D1_GRADIENT_STOP stopsIdle[] = {
            { 0.0f, D2D1::ColorF(0.290f, 0.306f, 0.329f, 0.72f) },
            { 1.0f, D2D1::ColorF(0.616f, 0.639f, 0.671f, 0.72f) },
        };
        if (FAILED(g_hudRenderTarget->CreateGradientStopCollection(stopsIdle, 2, &g_hudBarGradientStopsIdle))) {
            return false;
        }
        if (FAILED(g_hudRenderTarget->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(0, 0), D2D1::Point2F(0, 1)),
                g_hudBarGradientStopsIdle, &g_hudBarGradientIdle))) {
            return false;
        }
    }

    return g_hudRenderTarget && g_hudBrush && g_hudBarGradientRec && g_hudBarGradientIdle && g_hudTextFormat;
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
    const float barWidth = 6.5f;
    const float barGap = 4.0f;
    const float barAreaHeight = std::min(54.0f, height - 8.0f);
    const float barStartX = kHudLeftPad + 2.0f;
    const float weights[] = { 0.5f, 0.8f, 1.0f, 0.75f, 0.55f };
    const float minFraction = 0.24f;
    const double tick = static_cast<double>(GetTickCount64());

    for (int i = 0; i < 5; ++i) {
        const float motion = g_recording ? static_cast<float>(std::sin(tick * 0.012 + i * 1.9) * 0.035) : 0.0f;
        const float fraction = std::clamp(minFraction + (1.0f - minFraction) * level * weights[i] + motion,
                                          minFraction, 1.0f);
        const float h = barAreaHeight * fraction;
        const float x = barStartX + i * (barWidth + barGap);

        const float sweep = std::fmod(static_cast<float>(tick) * 0.0008f + i * 0.15f, 1.0f);
        const float gradTop = centerY - h / 2.0f - sweep * h * 0.3f;
        const float gradBottom = centerY + h / 2.0f + (1.0f - sweep) * h * 0.3f;

        auto* brush = g_recording ? g_hudBarGradientRec : g_hudBarGradientIdle;
        brush->SetStartPoint(D2D1::Point2F(x, gradBottom));
        brush->SetEndPoint(D2D1::Point2F(x, gradTop));

        const D2D1_ROUNDED_RECT bar = D2D1::RoundedRect(
            D2D1::RectF(x, centerY - h / 2.0f, x + barWidth, centerY + h / 2.0f),
            barWidth / 2.0f,
            barWidth / 2.0f);
        g_hudRenderTarget->FillRoundedRectangle(bar, brush);
    }

    g_hudBrush->SetColor(g_hudIsRefining
        ? D2D1::ColorF(0.6f, 0.6f, 0.6f, 0.96f)
        : D2D1::ColorF(0.965f, 0.975f, 0.99f, 0.96f));
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
        SafeRelease(g_hudBarGradientRec);
        SafeRelease(g_hudBarGradientIdle);
        SafeRelease(g_hudBarGradientStopsRec);
        SafeRelease(g_hudBarGradientStopsIdle);
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
