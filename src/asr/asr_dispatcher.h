#pragma once

#include "globals.h"

#include <string>

using AsrLlmRefineFn = void (*)(const std::wstring& asrText, const Config& config);

void DispatchAsrFinalText(HWND targetWindow,
                          std::wstring text,
                          const Config& config,
                          AsrLlmRefineFn refineFn,
                          std::wstring* lastRawAsrText);
