#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audio_diagnostics {

constexpr DWORD kTargetSampleRate = 16000;
constexpr WORD kTargetChannels = 1;
constexpr WORD kTargetBitsPerSample = 16;
constexpr size_t kFailureNoSpeechMinPcmBytes = 3u * 32000u;
constexpr size_t kFailureOperationalMinPcmBytes = 8000u;
constexpr size_t kRetentionMaxGroups = 20;
constexpr uint64_t kRetentionMaxBytes = 100ull * 1024ull * 1024ull;
constexpr DWORD kRetentionMaxAgeDays = 7;

enum class StageKind {
    Primary,
    InternalRetry,
    Fallback,
};

const char* StageKindName(StageKind kind);
StageKind RetryStageKind(StageKind parentKind);
unsigned RetryStageIndex(StageKind parentKind,
                         unsigned parentIndex,
                         unsigned retryIndex);

enum class FinalKind {
    UsableText,
    NoSpeech,
    TooShort,
    OperationalError,
    Cancelled,
    CaptureFailure,
};

const char* FinalKindName(FinalKind kind);

struct CaptureDeviceInfo {
    std::wstring backend;
    std::wstring deviceName;
    std::wstring deviceId;
    bool usedDefaultDevice = true;
    DWORD nativeSampleRate = 0;
    WORD nativeChannels = 0;
    WORD nativeBitsPerSample = 0;
    bool nativeIsFloat = false;
};

struct Pcm16Metrics {
    uint64_t sampleCount = 0;
    double rmsDbfs = -120.0;
    double peakDbfs = -120.0;
    double zeroRatio = 0.0;
    double nearSilentRatio = 0.0;
    double clippingRatio = 0.0;
};

struct CaptureSnapshot {
    CaptureDeviceInfo device;
    uint64_t nativeFrames = 0;
    uint64_t outputSamples = 0;
    size_t pcmBytes = 0;
    double recordingMs = 0.0;
    Pcm16Metrics output;
    uint64_t silentPackets = 0;
    uint64_t silentFrames = 0;
    uint64_t discontinuities = 0;
    double maxCallbackGapMs = 0.0;
    double firstNonSilentDelayMs = -1.0;
    std::vector<double> nativeChannelRmsDbfs;
    double downmixRmsDbfs = -120.0;
};

struct AttemptMetadata {
    uint64_t attemptId = 0;
    std::wstring mode = L"off";
    std::wstring backend;
    std::wstring model;
    std::wstring transport;
};

struct StageMetadata {
    StageKind kind = StageKind::Primary;
    unsigned index = 0;
    std::wstring backend;
    std::wstring model;
    std::wstring transport;
    std::wstring reason;
    std::wstring encoding = L"pcm_s16le";
    bool vadEnabled = false;
    bool vadActive = false;
    bool vadDetectedSpeech = false;
    std::wstring vadModel;
    size_t vadInputBytes = 0;
    size_t vadOutputBytes = 0;
    size_t sentBytes = 0;
    size_t networkBytes = 0;
};

struct StageTerminal {
    std::string terminal;
    std::string reason;
    std::string providerCode;
    std::string errorCategory;
    size_t textChars = 0;
    size_t committedTextChars = 0;
    double elapsedMs = 0.0;
};

struct FinalResult {
    FinalKind kind = FinalKind::UsableText;
    std::string reason;
    std::string terminal;
    bool userCancelled = false;
};

struct SaveDecision {
    bool save = false;
    std::string reason;
};

using LogCallback = void (*)(const std::string& line);

void SetLogCallback(LogCallback callback);

std::wstring NormalizeMode(std::wstring mode);
std::wstring DiagnosticAudioDir();
bool EnsureDiagnosticAudioDir(std::wstring* error = nullptr);
bool DeleteManagedRecordings(size_t* deletedGroups = nullptr,
                             std::wstring* error = nullptr);

void ResetCapture(const CaptureDeviceInfo& device = {});
void SetCaptureDeviceInfo(const CaptureDeviceInfo& device);
void RecordWasapiFloatPacket(const float* interleaved,
                             UINT32 frames,
                             UINT32 channels,
                             const float* downmixedMono,
                             bool silentPacket);
void RecordWasapiPcm16Packet(const int16_t* interleaved,
                             UINT32 frames,
                             UINT32 channels,
                             const float* downmixedMono,
                             bool silentPacket);
void RecordWaveInPcm16(const BYTE* data, size_t bytes);
void RecordOutputPcm16(const BYTE* data, size_t bytes);
void RecordCaptureDiscontinuity();
CaptureSnapshot FreezeCapture(double recordingMs, size_t pcmBytes);
void CancelCapture();

void BeginAttempt(const AttemptMetadata& metadata);
void AttachCapture(uint64_t attemptId,
                   std::shared_ptr<const std::vector<BYTE>> pcm,
                   const CaptureSnapshot& capture);
void RegisterStageInput(uint64_t attemptId,
                        const StageMetadata& metadata,
                        const std::vector<BYTE>& pcm);
void AppendStageInput(uint64_t attemptId,
                      const StageMetadata& metadata,
                      const BYTE* data,
                      size_t bytes,
                      size_t networkBytes = 0);
void UpdateStageMetadata(uint64_t attemptId,
                         const StageMetadata& metadata);
void CompleteStage(uint64_t attemptId,
                   StageKind kind,
                   unsigned index,
                   const StageTerminal& terminal);
void CompleteStageIfMissing(uint64_t attemptId,
                            const StageMetadata& metadata,
                            const StageTerminal& terminal);
SaveDecision FinalizeAttempt(uint64_t attemptId, const FinalResult& result);
void DiscardAttempt(uint64_t attemptId, const char* reason = "discarded");
bool WaitForPendingWrites(DWORD timeoutMs = 5000);

Pcm16Metrics CalculatePcm16Metrics(const BYTE* data, size_t bytes);
std::vector<BYTE> BuildPcm16MonoWav(const BYTE* data, size_t bytes);
std::string Sha256Hex(const BYTE* data, size_t bytes);
SaveDecision ShouldSaveDiagnosticAudio(const std::wstring& mode,
                                       const FinalResult& result,
                                       size_t pcmBytes,
                                       double recordingMs,
                                       bool contradictoryStages = false);

// Test-only override. Passing an empty path restores the normal data directory.
void SetDirectoryOverrideForTesting(const std::wstring& path);

} // namespace audio_diagnostics
