#pragma once

namespace qwen_finalize_policy {

enum class TerminalReason {
    None,
    Timeout,
    PeerClosed,
    TransportFailure,
    ProviderFailure,
};

inline bool IsRecoverableIncompleteFinalize(TerminalReason reason) {
    return reason == TerminalReason::Timeout || reason == TerminalReason::PeerClosed;
}

inline bool CanRecoverAudioStreaming(bool finishTaskSent,
                                     bool taskFinished,
                                     bool hasCommittedText,
                                     TerminalReason reason) {
    return finishTaskSent && !taskFinished && hasCommittedText &&
           IsRecoverableIncompleteFinalize(reason);
}

inline bool CanRecoverRealtime(bool transcriptionCompleted,
                               bool sessionFinished,
                               bool hasFinalText,
                               TerminalReason reason) {
    return transcriptionCompleted && !sessionFinished && hasFinalText &&
           IsRecoverableIncompleteFinalize(reason);
}

inline const char* TerminalReasonName(TerminalReason reason) {
    switch (reason) {
    case TerminalReason::Timeout: return "timeout";
    case TerminalReason::PeerClosed: return "peer_closed";
    case TerminalReason::TransportFailure: return "transport_failure";
    case TerminalReason::ProviderFailure: return "provider_failure";
    case TerminalReason::None:
    default:
        return "none";
    }
}

} // namespace qwen_finalize_policy
