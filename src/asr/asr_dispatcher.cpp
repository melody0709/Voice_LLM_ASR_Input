#include "asr_dispatcher.h"

#include "asr_result.h"

#include <utility>

void DispatchAsrFinalText(HWND targetWindow,
                          std::wstring text,
                          const Config& config,
                          AsrLlmRefineFn refineFn,
                          std::wstring* lastRawAsrText,
                          const AsrFinalMetadata& metadata) {
    text = NormalizeAsrText(std::move(text));
    const bool needLlm = !metadata.bundledPostProcessApplied &&
                         refineFn && ShouldRunLlmRefine(config, text);

    auto* msg = new AsrFinalMessage;
    msg->attemptId = metadata.attemptId;
    msg->allowCancelledAttempt = metadata.allowCancelledAttempt;
    msg->bundledPostProcessApplied = metadata.bundledPostProcessApplied;
    msg->text = text;
    msg->resultConfig = config;
    msg->usedFallback = metadata.usedFallback;
    msg->primaryBackend = metadata.primaryBackend;
    msg->primaryError = metadata.primaryError;
    msg->fallbackBackend = metadata.fallbackBackend;
    msg->selection = metadata.selection;

    if (needLlm) {
        if (lastRawAsrText) {
            *lastRawAsrText = text;
        }
        AsrFinalMessage llmInput = *msg;
        if (!PostMessageW(targetWindow, kAsrResultMessage, 1,
                          reinterpret_cast<LPARAM>(msg))) {
            delete msg;
            return;
        }
        refineFn(llmInput);
        return;
    }

    if (!PostMessageW(targetWindow, kAsrResultMessage, 0,
                      reinterpret_cast<LPARAM>(msg))) {
        delete msg;
    }
}
