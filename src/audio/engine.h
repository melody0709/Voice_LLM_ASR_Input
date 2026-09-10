#pragma once

#include "globals.h"

#include <mutex>
#include <memory>
#include <vector>
#include <string>

template <typename T>
void SafeRelease(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool EqualsIgnoreCase(std::wstring a, std::wstring b);

// Runtime assets are immutable files deployed beside the executable. Mutable
// data lives under LocalAppData unless a Portable payload explicitly opts in.
bool IsPortableMode();
std::wstring RuntimeAssetDir();
// Developer tools that live outside the installed payload can point runtime
// asset/config resolution at the canonical VoxType runtime before LoadConfig.
bool SetRuntimeAssetDirOverrideForProcess(const std::wstring& directory);
std::wstring MutableDataDir();
std::wstring DownloadedModelRoot();
std::wstring LogDir();

std::wstring AppDataDir();
std::wstring AppRootDir();
std::wstring ConfigPath();
std::wstring DefaultModelDir(const std::wstring& modelId);
bool ModelDirExists(const std::wstring& dir);
bool AnyModelDirExists();
bool RunModelDownloader(HWND hwnd);
std::wstring ModelDisplayName(const std::wstring& modelId);
int ModelIndex(const std::wstring& modelId);
std::wstring ModelIdFromIndex(int index);

std::string ExtractJsonString(const std::string& json, const std::string& key, const std::string& fallback);
bool ExtractJsonBool(const std::string& json, const std::string& key, bool fallback);
int ExtractJsonInt(const std::string& json, const std::string& key, int fallback);

void SaveCurrentProvider();
bool LoadProviderFromStore(const std::wstring& name);
int FindPresetIndex(const std::wstring& name);
bool ApplyPreset(int index, bool preserveLegacyFields = false);
void LoadConfig();
void SaveConfig();

float CalculateAudioLevel(const BYTE* data, DWORD bytes);
float DpiScaleForWindow(HWND hwnd);
int DipToPx(float value, float scale);

void CALLBACK WaveInProc(HWAVEIN waveIn, UINT msg, DWORD_PTR, DWORD_PTR param1, DWORD_PTR);
struct AudioCaptureStartFailure {
    std::wstring attemptedBackends;
    std::wstring terminalBackend;
    std::wstring phase;
    DWORD code = ERROR_SUCCESS;
    std::wstring deviceName;
    std::wstring deviceId;
    bool usedDefaultDevice = true;
    DWORD nativeSampleRate = 0;
    WORD nativeChannels = 0;
    WORD nativeBitsPerSample = 0;
    bool nativeIsFloat = false;
};

bool StartAudioCapture(std::wstring& error,
                       AudioCaptureStartFailure* failure = nullptr);
// Pauses PCM accumulation and returns what was collected.  The device stays
// open (keep-alive) so a subsequent StartAudioCapture can resume instantly;
// the device is torn down by CloseAudioCapture once the keep-alive expires.
std::vector<BYTE> StopAudioCapture();
// Full teardown of the capture device (keep-alive expiry, capture failure,
// settings reload, application exit).
void CloseAudioCapture();
int ResolveThreads(const std::wstring& threads);

struct HiResTimer {
    LARGE_INTEGER freq_;
    LARGE_INTEGER start_;
    HiResTimer() {
        QueryPerformanceFrequency(&freq_);
        QueryPerformanceCounter(&start_);
    }
    double ElapsedMs() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return static_cast<double>(now.QuadPart - start_.QuadPart) * 1000.0 / static_cast<double>(freq_.QuadPart);
    }
};

struct VadResult {
    bool hasSpeech = false;
    std::vector<float> samples;
};

class AsrEngine {
public:
    void Lock() { lock_.lock(); }
    void Unlock() { lock_.unlock(); }

    std::unique_ptr<sherpa_onnx::cxx::OfflineRecognizer> recognizer;
    std::unique_ptr<sherpa_onnx::cxx::VoiceActivityDetector> vad;
    std::unique_ptr<sherpa_onnx::cxx::OfflinePunctuation> punctuation;
    std::unique_ptr<firered_vad::FireRedVad> fireRedVad;
    std::string recognizerKey;
    std::string vadKey;
    std::string fireRedVadKey;
    std::string punctKey;

    std::string MakeKey(const std::wstring& modelId, const std::wstring& modelDir, int threads);
    bool EnsureRecognizer(const Config& config);
    bool EnsurePunctuation(int threads);
    VadResult ApplyVad(const std::vector<float>& samples, const Config& config, int threads);
    bool EnsureVadForConfig(const Config& config, int threads);
    std::wstring Recognize(const std::vector<float>& samples, int sampleRate, const Config& config);
    void Reload();

private:
    std::mutex lock_;
    bool EnsureVad(int threads, const Config& config);
    bool EnsureFireRedVad(const Config& config);
};

std::vector<float> PcmToFloat(const std::vector<BYTE>& pcm);

void PreloadAsrEngine(const Config& config);
