#pragma once

#include <string>

namespace qwen_audio_profile {

inline constexpr wchar_t kLegacyModel[] = L"qwen3-asr-flash-realtime";
inline constexpr wchar_t kHttpModel[] = L"qwen-audio-3.0-asr-flash";
inline constexpr wchar_t kStreamingModel[] = L"qwen-audio-3.0-asr-flash-streaming";

inline constexpr wchar_t kLegacyTransport[] = L"legacy_realtime";
inline constexpr wchar_t kHttpTransport[] = L"audio_http";
inline constexpr wchar_t kStreamingTransport[] = L"audio_streaming";

inline bool IsSupportedModel(const std::wstring& model) {
    return model == kLegacyModel || model == kHttpModel || model == kStreamingModel;
}

inline void NormalizePersistedProfile(std::wstring& model,
                                      std::wstring& transport,
                                      bool hasPersistedModel,
                                      bool hasPersistedTransport) {
    // A config file predating the model selector must keep the pre-Audio-3
    // behavior. The Config struct's streaming default is only for a fresh
    // install, where LoadConfig() is not entered because the file is absent.
    if (!hasPersistedModel) {
        model = kLegacyModel;
        transport = kLegacyTransport;
        return;
    }

    if (model == kHttpModel) {
        transport = kHttpTransport;
    } else if (model == kStreamingModel) {
        transport = kStreamingTransport;
    } else if (model == kLegacyModel) {
        transport = kLegacyTransport;
    } else {
        // Unknown model names must not inherit an unrelated transport.
        model = kLegacyModel;
        transport = kLegacyTransport;
        return;
    }

    (void)hasPersistedTransport;
}

} // namespace qwen_audio_profile
