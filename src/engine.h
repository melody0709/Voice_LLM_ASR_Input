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
void LoadProviderFromStore(const std::wstring& name);
int FindPresetIndex(const std::wstring& name);
void ApplyPreset(int index);
void LoadConfig();
void SaveConfig();

float CalculateAudioLevel(const BYTE* data, DWORD bytes);
float DpiScaleForWindow(HWND hwnd);
int DipToPx(float value, float scale);

void CALLBACK WaveInProc(HWAVEIN waveIn, UINT msg, DWORD_PTR, DWORD_PTR param1, DWORD_PTR);
bool StartAudioCapture(std::wstring& error);
std::vector<BYTE> StopAudioCapture();
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
    bool EnsureVad(int threads);
    bool EnsureFireRedVad();
    bool EnsurePunctuation(int threads);
    std::wstring Recognize(const std::vector<float>& samples, int sampleRate, const Config& config);
    void Reload();

private:
    std::mutex lock_;
};

std::vector<float> PcmToFloat(const std::vector<BYTE>& pcm);

void PreloadAsrEngine(const Config& config);
