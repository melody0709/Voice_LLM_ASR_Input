#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

constexpr UINT32 kWasapiTargetSampleRate = 16000;
constexpr UINT32 kWasapiTargetChannels = 1;
constexpr UINT32 kWasapiTargetBits = 16;

struct WasapiDeviceInfo {
    std::wstring id;
    std::wstring name;
    bool isDefault = false;
};

class WasapiCapture {
public:
    WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    static std::vector<WasapiDeviceInfo> EnumerateDevices();

    bool Init(const std::wstring& deviceId = L"");
    bool Start(std::wstring& error);
    void SetCaptureGeneration(uint64_t generation) {
        m_captureGeneration.store(generation, std::memory_order_release);
    }
    void Stop();
    void Release();
    bool IsCapturing() const { return m_running.load(); }
    bool IsInitialized() const { return m_audioClient != nullptr; }

    UINT32 GetNativeSampleRate() const { return m_nativeSampleRate; }
    UINT32 GetNativeChannels() const { return m_nativeChannels; }
    UINT32 GetNativeBits() const { return m_nativeBits; }
    bool GetNativeIsFloat() const { return m_nativeIsFloat; }
    std::wstring GetDeviceName() const { return m_deviceName; }
    std::wstring GetDeviceId() const { return m_deviceId; }
    bool UsedDefaultDevice() const { return m_usedDefaultDevice; }

private:
    bool InitCOM();
    bool InitEnumerator();
    bool FindDevice(const std::wstring& deviceId);
    bool InitAudioClient();
    bool InitCaptureClient();
    void CaptureThread();
    void ReportRuntimeFailure(DWORD code);
    float CalculateAudioLevelFloat(const float* data, UINT32 frames, UINT32 channels);

    IMMDeviceEnumerator* m_enumerator{nullptr};
    IMMDevice* m_device{nullptr};
    IAudioClient* m_audioClient{nullptr};
    IAudioCaptureClient* m_captureClient{nullptr};
    WAVEFORMATEX* m_mixFormat{nullptr};
    UINT32 m_bufferFrames{0};

    UINT32 m_nativeSampleRate{0};
    UINT32 m_nativeChannels{0};
    UINT32 m_nativeBits{0};
    bool m_nativeIsFloat{false};
    std::wstring m_deviceName;
    std::wstring m_deviceId;
    bool m_usedDefaultDevice{true};

    double m_resampleRatio{1.0};
    double m_resamplePhase{0.0};

    std::thread m_captureThread;
    std::atomic<bool> m_running{false};
    std::atomic<uint64_t> m_captureGeneration{0};
    HANDLE m_event{nullptr};
    bool m_comInitialized{false};
};
