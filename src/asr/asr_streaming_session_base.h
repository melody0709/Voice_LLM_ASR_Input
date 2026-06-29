#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "asr_dispatcher.h"
#include "asr_streaming_session.h"

#include <string>
#include <utility>

class StreamingAsrSessionBase : public IStreamingAsrSession {
public:
    StreamingAsrSessionBase(Config config,
                            HWND targetWindow,
                            AsrLlmRefineFn refineFn,
                            std::wstring* lastRawAsrText)
        : config_(std::move(config)),
          targetWindow_(targetWindow),
          refineFn_(refineFn),
          lastRawAsrText_(lastRawAsrText) {}

    void SetPartialCallback(AsrPartialCallback cb, void* userData) override {
        partialCallback_ = cb;
        partialUserData_ = userData;
    }

    void SetFinalCallback(AsrFinalCallback cb, void* userData) override {
        finalCallback_ = cb;
        finalUserData_ = userData;
    }

protected:
    void NotifyStatus(const std::wstring& status) const {
        if (!targetWindow_) return;
        PostMessageW(targetWindow_, kHudUpdateMessage, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(status)));
    }

    void NotifyPartial(const std::wstring& text, bool isFinal) const {
        if (partialCallback_) {
            partialCallback_(text, isFinal, partialUserData_);
        }
    }

    void DispatchFinal(std::wstring text) {
        if (finalCallback_) {
            finalCallback_(std::move(text), config_, finalUserData_);
            return;
        }
        DispatchAsrFinalText(targetWindow_, std::move(text), config_, refineFn_, lastRawAsrText_);
    }

    const Config& ConfigRef() const { return config_; }

    Config config_;
    HWND targetWindow_ = nullptr;
    AsrLlmRefineFn refineFn_ = nullptr;
    std::wstring* lastRawAsrText_ = nullptr;

private:
    AsrPartialCallback partialCallback_ = nullptr;
    void* partialUserData_ = nullptr;
    AsrFinalCallback finalCallback_ = nullptr;
    void* finalUserData_ = nullptr;
};
