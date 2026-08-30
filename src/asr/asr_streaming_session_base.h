#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "asr_dispatcher.h"
#include "asr_streaming_session.h"
#include "asr_diagnostics.h"

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
        auto* message = new std::wstring(status);
        if (!PostMessageW(targetWindow_, kHudUpdateMessage,
                          static_cast<WPARAM>(config_.asrAttemptId),
                          reinterpret_cast<LPARAM>(message))) {
            delete message;
        }
    }

    void NotifyPartial(const std::wstring& text, bool isFinal) const {
        if (partialCallback_) {
            partialCallback_(text, isFinal, partialUserData_);
        }
    }

    void DispatchFinal(std::wstring text,
                       bool bundledPostProcessApplied = false) {
        if (finalCallback_) {
            finalCallback_(std::move(text), config_,
                           bundledPostProcessApplied, finalUserData_);
            return;
        }
        AsrFinalMetadata metadata;
        metadata.bundledPostProcessApplied = bundledPostProcessApplied;
        DispatchAsrFinalText(targetWindow_, std::move(text), config_, refineFn_,
                             lastRawAsrText_, metadata);
    }

    const Config& ConfigRef() const { return config_; }

    // Shared diagnostic stage helpers used by streaming providers.
    // 带附加行为的后端（doubao 的 opus 标注、qwen_free 的 replay 阶段路由）
    // 保留各自的本地版本。
    audio_diagnostics::StageMetadata PrimaryStage(
        std::wstring reason = {}) const {
        return asr_diagnostics::MakeStageMetadata(config_, std::move(reason));
    }

    audio_diagnostics::StageMetadata RetryStage(unsigned index,
                                                 std::wstring reason) const {
        return asr_diagnostics::MakeRetryStageMetadata(
            config_, index, std::move(reason));
    }

    void CompletePrimary(audio_diagnostics::StageTerminal terminal) {
        audio_diagnostics::CompleteStage(
            config_.asrAttemptId, config_.asrDiagnosticStageKind,
            config_.asrDiagnosticStageIndex, terminal);
    }

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
