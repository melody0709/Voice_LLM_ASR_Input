#include "audio_diagnostics.h"

#include <windows.h>

#include <cmath>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++g_failures;
}

bool Near(double actual, double expected, double tolerance) {
    return std::abs(actual - expected) <= tolerance;
}

uint16_t ReadLe16(const std::vector<BYTE>& value, size_t offset) {
    if (offset + 2 > value.size()) return 0;
    return static_cast<uint16_t>(value[offset]) |
           (static_cast<uint16_t>(value[offset + 1]) << 8);
}

uint32_t ReadLe32(const std::vector<BYTE>& value, size_t offset) {
    if (offset + 4 > value.size()) return 0;
    return static_cast<uint32_t>(value[offset]) |
           (static_cast<uint32_t>(value[offset + 1]) << 8) |
           (static_cast<uint32_t>(value[offset + 2]) << 16) |
           (static_cast<uint32_t>(value[offset + 3]) << 24);
}

std::vector<BYTE> SamplesToBytes(const std::vector<int16_t>& samples) {
    std::vector<BYTE> bytes(samples.size() * sizeof(int16_t));
    if (!bytes.empty()) {
        std::memcpy(bytes.data(), samples.data(), bytes.size());
    }
    return bytes;
}

bool WriteBytes(const std::wstring& path, const void* data, DWORD bytes) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, data, bytes, &written, nullptr) != FALSE &&
                    written == bytes;
    CloseHandle(file);
    return ok;
}

bool SetOldWriteTime(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    FILETIME now = {};
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER value = {};
    value.LowPart = now.dwLowDateTime;
    value.HighPart = now.dwHighDateTime;
    constexpr ULONGLONG kTwoDays = 2ULL * 24 * 60 * 60 * 10000000;
    value.QuadPart -= kTwoDays;
    FILETIME old = {};
    old.dwLowDateTime = value.LowPart;
    old.dwHighDateTime = value.HighPart;
    const bool ok = SetFileTime(file, nullptr, nullptr, &old) != FALSE;
    CloseHandle(file);
    return ok;
}

std::string ReadUtf8File(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size = {};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 ||
        size.QuadPart > 16 * 1024 * 1024) {
        CloseHandle(file);
        return {};
    }
    std::string result(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const bool ok = result.empty() ||
        (ReadFile(file, result.data(), static_cast<DWORD>(result.size()),
                  &read, nullptr) != FALSE && read == result.size());
    CloseHandle(file);
    return ok ? result : std::string();
}

std::vector<std::wstring> FindFiles(const std::wstring& directory,
                                    const wchar_t* pattern) {
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW((directory + L"\\" + pattern).c_str(),
                                   &data);
    if (search == INVALID_HANDLE_VALUE) return files;
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            files.push_back(data.cFileName);
        }
    } while (FindNextFileW(search, &data));
    FindClose(search);
    return files;
}

void CleanupDirectory(const std::wstring& directory) {
    for (const std::wstring& name : FindFiles(directory, L"*")) {
        DeleteFileW((directory + L"\\" + name).c_str());
    }
    RemoveDirectoryW(directory.c_str());
}

std::wstring MakeTempDirectory() {
    wchar_t temp[MAX_PATH] = {};
    const DWORD length = GetTempPathW(MAX_PATH, temp);
    if (length == 0 || length >= MAX_PATH) return {};
    std::wstring path(temp, length);
    path += L"VoxType-audio-diagnostics-test-";
    path += std::to_wstring(GetCurrentProcessId());
    path += L"-";
    path += std::to_wstring(GetTickCount64());
    if (!CreateDirectoryW(path.c_str(), nullptr)) return {};
    return path;
}

size_t CountOccurrences(const std::string& value, const std::string& needle) {
    size_t count = 0;
    size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        ++count;
        offset += needle.size();
    }
    return count;
}

void AddSavedAttempt(uint64_t attemptId,
                     const std::vector<BYTE>& pcm,
                     const std::wstring& backend = L"local") {
    audio_diagnostics::AttemptMetadata metadata;
    metadata.attemptId = attemptId;
    metadata.mode = L"all";
    metadata.backend = backend;
    metadata.model = L"test-model";
    metadata.transport = L"test";
    audio_diagnostics::BeginAttempt(metadata);

    audio_diagnostics::CaptureSnapshot capture;
    capture.recordingMs = pcm.size() * 1000.0 / 32000.0;
    capture.pcmBytes = pcm.size();
    audio_diagnostics::AttachCapture(
        attemptId, std::make_shared<const std::vector<BYTE>>(pcm), capture);

    audio_diagnostics::FinalResult result;
    result.kind = audio_diagnostics::FinalKind::UsableText;
    result.reason = "test_success";
    const auto decision = audio_diagnostics::FinalizeAttempt(attemptId, result);
    Expect(decision.save, "all mode schedules a non-empty recording for persistence");
}

} // namespace

int main() {
    using namespace audio_diagnostics;

    {
        const BYTE oddPcm[] = {0x34, 0x12, 0x78};
        const std::vector<BYTE> wav = BuildPcm16MonoWav(oddPcm, sizeof(oddPcm));
        Expect(wav.size() == 46, "WAV writer truncates an odd trailing PCM byte");
        Expect(wav.size() >= 44 && std::memcmp(wav.data(), "RIFF", 4) == 0 &&
                   std::memcmp(wav.data() + 8, "WAVEfmt ", 8) == 0 &&
                   std::memcmp(wav.data() + 36, "data", 4) == 0,
               "WAV writer emits RIFF/WAVE/fmt/data headers");
        Expect(ReadLe32(wav, 4) == 38 && ReadLe32(wav, 16) == 16 &&
                   ReadLe16(wav, 20) == 1 && ReadLe16(wav, 22) == 1 &&
                   ReadLe32(wav, 24) == 16000 && ReadLe32(wav, 28) == 32000 &&
                   ReadLe16(wav, 32) == 2 && ReadLe16(wav, 34) == 16 &&
                   ReadLe32(wav, 40) == 2,
               "WAV header describes 16 kHz mono PCM16 with exact data size");
        Expect(wav[44] == 0x34 && wav[45] == 0x12,
               "WAV payload preserves complete PCM16 samples");
        const std::vector<BYTE> empty = BuildPcm16MonoWav(nullptr, 0);
        Expect(empty.size() == 44 && ReadLe32(empty, 4) == 36 &&
                   ReadLe32(empty, 40) == 0,
               "WAV writer supports an empty canonical header");
    }

    {
        const std::vector<BYTE> silence = SamplesToBytes(std::vector<int16_t>(160, 0));
        const Pcm16Metrics metrics = CalculatePcm16Metrics(silence.data(), silence.size());
        Expect(metrics.sampleCount == 160 && metrics.rmsDbfs == -120.0 &&
                   metrics.peakDbfs == -120.0 && metrics.zeroRatio == 1.0 &&
                   metrics.nearSilentRatio == 1.0 && metrics.clippingRatio == 0.0,
               "PCM metrics classify digital silence exactly");

        std::vector<int16_t> sineSamples;
        sineSamples.reserve(160);
        constexpr double kPi = 3.14159265358979323846;
        for (int i = 0; i < 160; ++i) {
            sineSamples.push_back(static_cast<int16_t>(
                std::lround(16384.0 * std::sin(2.0 * kPi * i / 16.0))));
        }
        const std::vector<BYTE> sine = SamplesToBytes(sineSamples);
        const Pcm16Metrics sineMetrics = CalculatePcm16Metrics(sine.data(), sine.size());
        Expect(Near(sineMetrics.rmsDbfs, -9.031, 0.08) &&
                   Near(sineMetrics.peakDbfs, -6.021, 0.02),
               "PCM metrics report expected RMS and peak for a half-scale sine");

        const std::vector<BYTE> clipped = SamplesToBytes({32767, -32768, 0, 1});
        const Pcm16Metrics clippedMetrics =
            CalculatePcm16Metrics(clipped.data(), clipped.size());
        Expect(Near(clippedMetrics.clippingRatio, 0.5, 1e-9) &&
                   Near(clippedMetrics.zeroRatio, 0.25, 1e-9) &&
                   Near(clippedMetrics.nearSilentRatio, 0.5, 1e-9),
               "PCM metrics count clipped, zero, and near-silent samples");
    }

    {
        const char abc[] = "abc";
        Expect(Sha256Hex(reinterpret_cast<const BYTE*>(abc), 3) ==
                   "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
               "SHA-256 matches the standard abc vector");
        Expect(Sha256Hex(nullptr, 0) ==
                   "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
               "SHA-256 supports an empty input");
    }

    {
        Expect(RetryStageKind(StageKind::Primary) == StageKind::InternalRetry &&
                   RetryStageIndex(StageKind::Primary, 0, 1) == 1,
               "primary provider retry uses the internal-retry namespace");
        Expect(RetryStageKind(StageKind::Fallback) == StageKind::Fallback &&
                   RetryStageIndex(StageKind::Fallback, 0, 1) == 1,
               "fallback provider retry cannot overwrite a primary retry stage");
    }

    {
        FinalResult result;
        result.kind = FinalKind::NoSpeech;
        Expect(!ShouldSaveDiagnosticAudio(L"off", result, 200000, 5000).save,
               "off mode never persists audio");
        Expect(ShouldSaveDiagnosticAudio(L"failures", result,
                                         kFailureNoSpeechMinPcmBytes, 3000).save,
               "failures mode saves a substantive no-speech result");
        Expect(!ShouldSaveDiagnosticAudio(L"failures", result, 1000, 100).save,
               "failures mode ignores a short no-speech result");
        result.kind = FinalKind::TooShort;
        Expect(!ShouldSaveDiagnosticAudio(L"failures", result, 200000, 5000).save,
               "failures mode excludes explicit too-short recordings");
        result.kind = FinalKind::OperationalError;
        result.reason = "timeout";
        Expect(ShouldSaveDiagnosticAudio(L"failures", result,
                                         kFailureOperationalMinPcmBytes, 500).save,
               "failures mode saves operational failures with useful PCM");
        result.reason = "auth_or_config";
        Expect(!ShouldSaveDiagnosticAudio(L"failures", result, 200000, 5000).save,
               "failures mode excludes auth/config errors without diagnostic value");
        result.kind = FinalKind::CaptureFailure;
        result.reason = "capture_start_open";
        Expect(ShouldSaveDiagnosticAudio(L"failures", result, 0, 0).save,
               "failures mode saves capture startup metadata without PCM");
        result.kind = FinalKind::UsableText;
        result.reason.clear();
        Expect(!ShouldSaveDiagnosticAudio(L"failures", result, 200000, 5000).save &&
                   ShouldSaveDiagnosticAudio(L"failures", result, 200000, 5000, true).save,
               "failures mode saves only contradictory successful stage outcomes");
        result.userCancelled = true;
        Expect(!ShouldSaveDiagnosticAudio(L"all", result, 200000, 5000).save,
               "all mode still respects explicit user cancellation");
    }

    const std::wstring directory = MakeTempDirectory();
    Expect(!directory.empty(), "temporary diagnostics directory is created");
    if (!directory.empty()) {
        SetDirectoryOverrideForTesting(directory);

        {
            AttemptMetadata failureMetadata;
            failureMetadata.attemptId = 900;
            failureMetadata.mode = L"failures";
            failureMetadata.backend = L"local";
            failureMetadata.transport = L"capture";
            BeginAttempt(failureMetadata);

            CaptureSnapshot failedCapture;
            failedCapture.device.backend = L"wavein";
            failedCapture.device.deviceName = L"Unavailable test microphone";
            failedCapture.device.deviceId = L"failed-device-id-must-not-appear";
            AttachCapture(900,
                          std::make_shared<const std::vector<BYTE>>(),
                          failedCapture);

            StageMetadata failureStage;
            failureStage.kind = StageKind::Primary;
            failureStage.backend = L"audio_capture";
            failureStage.transport = L"wasapi,wavein";
            failureStage.reason = L"capture_start_open";
            StageTerminal failureTerminal;
            failureTerminal.terminal = "capture_start_failure";
            failureTerminal.reason = "capture_start_open";
            failureTerminal.providerCode = "2";
            failureTerminal.errorCategory = "capture";
            CompleteStageIfMissing(900, failureStage, failureTerminal);

            FinalResult captureFailure;
            captureFailure.kind = FinalKind::CaptureFailure;
            captureFailure.reason = "capture_start_open";
            captureFailure.terminal = "capture_start_failure";
            Expect(FinalizeAttempt(900, captureFailure).save,
                   "zero-PCM capture failure is queued for persistence");
            Expect(WaitForPendingWrites(15000),
                   "zero-PCM capture failure writer drains");

            const std::vector<std::wstring> failureManifests =
                FindFiles(directory, L"*.json");
            Expect(failureManifests.size() == 1 &&
                       FindFiles(directory, L"*.wav").empty(),
                   "capture startup failure writes one JSON manifest and no empty WAV");
            if (failureManifests.size() == 1) {
                const std::string json = ReadUtf8File(
                    directory + L"\\" + failureManifests[0]);
                Expect(json.find("\"result_kind\": \"capture_failure\"") !=
                           std::string::npos &&
                           json.find("\"backend\": \"wavein\"") !=
                           std::string::npos &&
                           json.find("\"transport\": \"wasapi,wavein\"") !=
                           std::string::npos &&
                           json.find("\"audio_artifacts\": [\n  ]") !=
                           std::string::npos &&
                           json.find("failed-device-id-must-not-appear") ==
                           std::string::npos,
                       "capture failure manifest records the terminal without raw device ID or artifacts");
            }
            size_t deletedFailureGroups = 0;
            std::wstring deleteFailureError;
            Expect(DeleteManagedRecordings(&deletedFailureGroups,
                                           &deleteFailureError) &&
                       deletedFailureGroups == 1,
                   "capture failure fixture is removed before retention tests");
        }

        std::vector<int16_t> samples(320);
        for (size_t i = 0; i < samples.size(); ++i) {
            samples[i] = static_cast<int16_t>((i % 32) * 700 - 10000);
        }
        const std::vector<BYTE> pcm = SamplesToBytes(samples);

        AttemptMetadata metadata;
        metadata.attemptId = 1001;
        metadata.mode = L"all";
        metadata.backend = L"qwen";
        metadata.model = L"diagnostic-test-model";
        metadata.transport = L"audio_streaming";
        BeginAttempt(metadata);

        CaptureSnapshot capture;
        capture.device.backend = L"wasapi";
        capture.device.deviceName = L"Test Microphone";
        capture.device.deviceId = L"raw-device-id-must-not-appear";
        capture.device.nativeSampleRate = 48000;
        capture.device.nativeChannels = 2;
        capture.device.nativeBitsPerSample = 32;
        capture.device.nativeIsFloat = true;
        capture.recordingMs = 1250.0;
        capture.output = CalculatePcm16Metrics(pcm.data(), pcm.size());
        capture.outputSamples = capture.output.sampleCount;
        AttachCapture(1001, std::make_shared<const std::vector<BYTE>>(pcm), capture);

        StageMetadata primary;
        primary.kind = StageKind::Primary;
        primary.index = 0;
        primary.backend = L"qwen";
        primary.model = L"diagnostic-test-model";
        primary.transport = L"audio_streaming";
        primary.vadEnabled = true;
        primary.vadActive = true;
        primary.vadDetectedSpeech = true;
        primary.vadModel = L"test-vad";
        RegisterStageInput(1001, primary, pcm);

        StageMetadata laterChunk;
        laterChunk.kind = StageKind::Primary;
        laterChunk.index = 0;
        laterChunk.backend = L"qwen";
        UpdateStageMetadata(1001, laterChunk);
        StageTerminal primaryTerminal;
        primaryTerminal.terminal = "task_finished_empty";
        CompleteStage(1001, StageKind::Primary, 0, primaryTerminal);

        StageMetadata retry = primary;
        retry.kind = StageKind::InternalRetry;
        retry.index = 1;
        retry.reason = L"empty_final";
        RegisterStageInput(1001, retry, pcm);
        StageTerminal retryTerminal;
        retryTerminal.terminal = "provider_no_words";
        retryTerminal.providerCode = "ASR_RESPONSE_HAVE_NO_WORDS";
        CompleteStage(1001, StageKind::InternalRetry, 1, retryTerminal);

        FinalResult finalResult;
        finalResult.kind = FinalKind::NoSpeech;
        finalResult.reason = "empty_after_retry";
        finalResult.terminal = "provider_no_words";
        Expect(FinalizeAttempt(1001, finalResult).save,
               "full diagnostic attempt is queued for persistence");
        Expect(WaitForPendingWrites(15000),
               "full diagnostic attempt writer drains");

        const std::vector<std::wstring> manifests = FindFiles(directory, L"*.json");
        const std::vector<std::wstring> wavFiles = FindFiles(directory, L"*.wav");
        Expect(manifests.size() == 1, "one attempt produces one JSON manifest");
        Expect(wavFiles.size() == 1,
               "capture, primary, and retry reuse one WAV when PCM hashes match");
        if (manifests.size() == 1) {
            const std::string json = ReadUtf8File(directory + L"\\" + manifests[0]);
            Expect(!json.empty() &&
                       json.find("raw-device-id-must-not-appear") == std::string::npos &&
                       json.find("\"device_id_sha256\"") != std::string::npos,
                   "manifest hashes the stable device ID instead of storing it raw");
            Expect(json.find("\"transcript\"") == std::string::npos &&
                       json.find("\"context\"") == std::string::npos &&
                       json.find("\"api_key\"") == std::string::npos &&
                       json.find("\"token\"") == std::string::npos,
                   "manifest omits transcript, context, key, and token fields");
            Expect(CountOccurrences(json, "\"input_artifact\": \"capture\"") == 2,
                   "primary and retry stages both reference the deduplicated capture artifact");
            Expect(json.find("\"vad_enabled\": true") != std::string::npos &&
                       json.find("\"vad_active\": true") != std::string::npos &&
                       json.find("\"vad_detected_speech\": true") != std::string::npos,
                   "later streaming metadata cannot erase an observed VAD state");
        }

        const std::wstring unknownPath = directory + L"\\developer-notes.keep";
        const char unknown[] = "preserve me";
        Expect(WriteBytes(unknownPath, unknown, sizeof(unknown) - 1),
               "unknown retention sentinel is created");
        const std::wstring unknownTempPath =
            directory + L"\\20260824-010203.004-attempt999-developer-notes-capture.wav.tmp";
        const std::wstring managedTempPath =
            directory + L"\\20260824-010203.004-attempt999-no_speech-capture.wav.tmp";
        Expect(WriteBytes(unknownTempPath, unknown, sizeof(unknown) - 1) &&
                   SetOldWriteTime(unknownTempPath),
               "old unknown WAV temp fixture is created");
        Expect(WriteBytes(managedTempPath, unknown, sizeof(unknown) - 1) &&
                   SetOldWriteTime(managedTempPath),
               "old managed WAV temp fixture is created");
        for (size_t i = 0; i < kRetentionMaxGroups + 3; ++i) {
            AddSavedAttempt(2000 + i, pcm);
        }
        Expect(WaitForPendingWrites(30000),
               "retention fixture writers drain");
        const std::vector<std::wstring> retainedManifests =
            FindFiles(directory, L"*.json");
        const size_t retainedGroups = retainedManifests.size();
        Expect(retainedGroups == kRetentionMaxGroups,
               "retention keeps exactly the newest managed group limit");
        Expect(GetFileAttributesW(unknownPath.c_str()) != INVALID_FILE_ATTRIBUTES,
               "retention preserves unknown files in the diagnostics directory");
        Expect(GetFileAttributesW(unknownTempPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
                   GetFileAttributesW(managedTempPath.c_str()) == INVALID_FILE_ATTRIBUTES,
               "temp cleanup removes only strictly managed old WAV temp files");

        std::wstring unknownWavPath;
        if (!retainedManifests.empty()) {
            std::wstring prefix = retainedManifests[0];
            prefix.resize(prefix.size() - 5);
            unknownWavPath = directory + L"\\" + prefix + L"-developer-notes.wav";
            Expect(WriteBytes(unknownWavPath, unknown, sizeof(unknown) - 1),
                   "same-prefix unknown WAV fixture is created");
        }

        size_t deletedGroups = 0;
        std::wstring deleteError;
        Expect(DeleteManagedRecordings(&deletedGroups, &deleteError) &&
                   deletedGroups == retainedGroups,
               "managed delete removes all retained recording groups");
        Expect(GetFileAttributesW(unknownPath.c_str()) != INVALID_FILE_ATTRIBUTES &&
                   FindFiles(directory, L"*.json").empty() &&
                   FindFiles(directory, L"*.wav").size() == 1 &&
                   !unknownWavPath.empty() &&
                   GetFileAttributesW(unknownWavPath.c_str()) != INVALID_FILE_ATTRIBUTES,
               "managed delete preserves unknown same-prefix WAVs and removes only managed artifacts");

        DeleteFileW(unknownPath.c_str());
        DeleteFileW(unknownTempPath.c_str());
        if (!unknownWavPath.empty()) DeleteFileW(unknownWavPath.c_str());
        Expect(WaitForPendingWrites(INFINITE),
               "all diagnostic writer threads are joined before test teardown");
        SetDirectoryOverrideForTesting(L"");
        CleanupDirectory(directory);
    }

    if (g_failures == 0) {
        std::cout << "audio_diagnostics_test: PASS\n";
        return 0;
    }
    return 1;
}
