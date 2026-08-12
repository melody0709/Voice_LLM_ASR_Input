#pragma once

#include "input_context.h"

#include <string>

namespace qwen_context {

// The provider documents a per-context text budget of 400 characters.  The
// shared input-field reader already keeps the tail of the focused control;
// keep the final provider payload bounded as a second line of defence.
inline std::wstring SanitizeText(const InputContextResult& result,
                                 size_t maxCharacters = 400) {
    if (result.isPassword || result.timedOut || result.inputFieldText.empty()) {
        return {};
    }
    if (result.inputFieldText.size() <= maxCharacters) {
        return result.inputFieldText;
    }
    // The provider documents truncating excess text from the end, so retain
    // the first maxCharacters rather than silently changing the context to a
    // suffix.
    return result.inputFieldText.substr(0, maxCharacters);
}

inline std::wstring CaptureInputFieldText(InputContextResult* diagnostics = nullptr) {
    InputContextResult result = input_context::GetInputFieldContext();
    if (diagnostics) *diagnostics = result;
    return SanitizeText(result);
}

} // namespace qwen_context
