#pragma once

// The Qianwen IME VoiceInputWrite endpoint returns one bundled post-processed
// result.  Keep the three historical flags as a compatibility view, but give
// callers one canonical enabled/disabled semantic.
namespace qwen_free_postprocess {

struct Flags {
    bool polish = true;
    bool punctuate = true;
    bool correct = true;
};

constexpr bool Enabled(const Flags& flags) noexcept {
    return flags.polish || flags.punctuate || flags.correct;
}

constexpr Flags Normalize(const Flags& flags) noexcept {
    const bool enabled = Enabled(flags);
    return Flags{enabled, enabled, enabled};
}

} // namespace qwen_free_postprocess
