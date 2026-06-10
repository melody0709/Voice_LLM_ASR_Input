#pragma once

#include "globals.h"

void AddTrayIcon(HWND hwnd);
void RemoveTrayIcon(HWND hwnd);
float CurrentHudLevel();
DWRITE_TEXT_METRICS MeasureHudText(const std::wstring& text, float maxWidth, DWRITE_WORD_WRAPPING wrapping);
HudSize IdealHudSize(const std::wstring& text, const RECT& workArea, float scale);
void PositionHud(HWND hwnd);
void ShowHud(const std::wstring& text);
void HideHud();
HFONT MakeFont(int pointSize, int weight);
void CreateUiResources();
void DeleteUiResources();
bool EnsureHudRenderTarget(HWND hwnd);
void DrawHudDirect2D(HWND hwnd);
LRESULT CALLBACK HudWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
