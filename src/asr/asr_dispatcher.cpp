#include "asr_dispatcher.h"

#include "asr_result.h"

#include <utility>

void DispatchAsrFinalText(HWND targetWindow,
                          std::wstring text,
                          const Config& config,
                          AsrLlmRefineFn refineFn,
                          std::wstring* lastRawAsrText) {
    text = NormalizeAsrText(std::move(text));
    const bool needLlm = refineFn && ShouldRunLlmRefine(config, text);

    if (needLlm) {
        if (lastRawAsrText) {
            *lastRawAsrText = text;
        }
        PostMessageW(targetWindow, kAsrResultMessage, 1,
                     reinterpret_cast<LPARAM>(new std::wstring(text)));
        refineFn(text, config);
        return;
    }

    PostMessageW(targetWindow, kAsrResultMessage, 0,
                 reinterpret_cast<LPARAM>(new std::wstring(text)));
}
