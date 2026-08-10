#include "wasapi_capture.h"
#include "asr_streaming_session.h"
#include "globals.h"
#include "engine.h"
#include "streaming_vad_trimmer.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mmdevapi.lib")

WasapiCapture::WasapiCapture() {}

WasapiCapture::~WasapiCapture() {
    Stop();
    Release();
}

void WasapiCapture::Release() {
    if (m_event) { CloseHandle(m_event); m_event = nullptr; }
    if (m_mixFormat) { CoTaskMemFree(m_mixFormat); m_mixFormat = nullptr; }
    if (m_captureClient) { m_captureClient->Release(); m_captureClient = nullptr; }
    if (m_audioClient) { m_audioClient->Release(); m_audioClient = nullptr; }
    if (m_device) { m_device->Release(); m_device = nullptr; }
    if (m_enumerator) { m_enumerator->Release(); m_enumerator = nullptr; }
    if (m_comInitialized) { CoUninitialize(); m_comInitialized = false; }
}

bool WasapiCapture::InitCOM() {
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;
    m_comInitialized = true;
    return true;
}

bool WasapiCapture::InitEnumerator() {
    HRESULT hr = CoCreateInstance(
        __uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
        __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
    return SUCCEEDED(hr);
}

bool WasapiCapture::FindDevice(const std::wstring& deviceId) {
    if (!deviceId.empty()) {
        HRESULT hr = m_enumerator->GetDevice(deviceId.c_str(), &m_device);
        if (SUCCEEDED(hr) && m_device) return true;
        printf("[WASAPI] Device ID not found, using default\n");
    }
    HRESULT hr = m_enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &m_device);
    return SUCCEEDED(hr) && m_device;
}

bool WasapiCapture::InitAudioClient() {
    HRESULT hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_audioClient);
    if (FAILED(hr)) { printf("[WASAPI] Activate failed: 0x%08lx\n", hr); return false; }

    hr = m_audioClient->GetMixFormat(&m_mixFormat);
    if (FAILED(hr)) { printf("[WASAPI] GetMixFormat failed: 0x%08lx\n", hr); return false; }

    m_nativeSampleRate = m_mixFormat->nSamplesPerSec;
    m_nativeChannels = m_mixFormat->nChannels;
    m_nativeBits = m_mixFormat->wBitsPerSample;
    m_nativeIsFloat = (m_mixFormat->wFormatTag == WAVE_FORMAT_IEEE_FLOAT) ||
                      (m_mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
                       reinterpret_cast<WAVEFORMATEXTENSIBLE*>(m_mixFormat)->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);

    m_resampleRatio = (double)kWasapiTargetSampleRate / (double)m_nativeSampleRate;

    REFERENCE_TIME hnsBufferDuration = 500000; // 50ms buffer
    hr = m_audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        hnsBufferDuration, 0, m_mixFormat, NULL);
    if (FAILED(hr)) { printf("[WASAPI] Initialize failed: 0x%08lx\n", hr); return false; }

    m_event = CreateEventEx(NULL, NULL, 0, EVENT_ALL_ACCESS);
    if (!m_event) { printf("[WASAPI] CreateEvent failed\n"); return false; }

    hr = m_audioClient->SetEventHandle(m_event);
    if (FAILED(hr)) { printf("[WASAPI] SetEventHandle failed: 0x%08lx\n", hr); return false; }

    hr = m_audioClient->GetBufferSize(&m_bufferFrames);
    if (FAILED(hr)) { printf("[WASAPI] GetBufferSize failed: 0x%08lx\n", hr); return false; }

    return true;
}

bool WasapiCapture::InitCaptureClient() {
    HRESULT hr = m_audioClient->GetService(__uuidof(IAudioCaptureClient), (void**)&m_captureClient);
    return SUCCEEDED(hr);
}

float WasapiCapture::CalculateAudioLevelFloat(const float* data, UINT32 frames, UINT32 channels) {
    if (!data || frames == 0) return 0.0f;
    double sum = 0.0;
    const UINT32 totalSamples = frames * channels;
    for (UINT32 i = 0; i < totalSamples; ++i) {
        double v = static_cast<double>(data[i]);
        sum += v * v;
    }
    const double rms = std::sqrt(sum / static_cast<double>(totalSamples));
    const double db = 20.0 * std::log10(std::max(rms, 1e-6));
    const double normalized = (db + 50.0) / 40.0;
    return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

bool WasapiCapture::Init(const std::wstring& deviceId) {
    if (!InitCOM()) { printf("[WASAPI] InitCOM failed\n"); return false; }
    if (!InitEnumerator()) { printf("[WASAPI] InitEnumerator failed\n"); return false; }
    if (!FindDevice(deviceId)) { printf("[WASAPI] FindDevice failed\n"); return false; }

    IPropertyStore* props = nullptr;
    if (SUCCEEDED(m_device->OpenPropertyStore(STGM_READ, &props))) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
            m_deviceName = varName.pwszVal;
            PropVariantClear(&varName);
        }
        props->Release();
    }

    if (!InitAudioClient()) return false;
    if (!InitCaptureClient()) return false;

    return true;
}

bool WasapiCapture::Start(std::wstring& error) {
    if (!m_audioClient) {
        error = L"WASAPI not initialized";
        return false;
    }

    m_resamplePhase = 0.0;
    m_running.store(true);

    HRESULT hr = m_audioClient->Start();
    if (FAILED(hr)) {
        printf("[WASAPI] AudioClient->Start failed: 0x%08lx\n", hr);
        error = L"WASAPI audio start failed";
        m_running.store(false);
        return false;
    }

    m_captureThread = std::thread(&WasapiCapture::CaptureThread, this);
    return true;
}

void WasapiCapture::Stop() {
    m_running.store(false);
    if (m_event) SetEvent(m_event);
    if (m_captureThread.joinable()) m_captureThread.join();
    if (m_audioClient) m_audioClient->Stop();
}

void WasapiCapture::ReportRuntimeFailure(DWORD code) {
    // Stop the worker before notifying the UI.  This makes the failure
    // terminal for the current capture and prevents repeated messages while
    // the main thread is stopping/releasing the WASAPI handles.
    if (!m_running.exchange(false, std::memory_order_acq_rel)) return;

    g_audioCaptureFailureCode.store(code, std::memory_order_relaxed);
    g_audioCaptureFailureWasapi.store(true, std::memory_order_relaxed);
    g_audioCaptureFailurePending.store(true, std::memory_order_release);

    const HWND target = g_mainWindow;
    const uint64_t generation = m_captureGeneration.load(std::memory_order_acquire);
    if (!target || generation == 0) return;

    PostMessageW(target,
                 kAudioCaptureErrorMessage,
                 static_cast<WPARAM>(generation),
                 static_cast<LPARAM>(code));
}

void WasapiCapture::CaptureThread() {
    while (m_running.load(std::memory_order_relaxed)) {
        DWORD waitResult = WaitForSingleObject(m_event, 200);
        if (!m_running.load(std::memory_order_relaxed)) break;
        if (waitResult == WAIT_FAILED) {
            ReportRuntimeFailure(GetLastError());
            break;
        }
        if (waitResult != WAIT_OBJECT_0) continue;

        UINT32 packetLength = 0;
        HRESULT hr = m_captureClient->GetNextPacketSize(&packetLength);

        if (FAILED(hr)) {
            ReportRuntimeFailure(static_cast<DWORD>(hr));
            break;
        }

        while (SUCCEEDED(hr) && packetLength > 0 && m_running.load(std::memory_order_relaxed)) {
            BYTE* captureData = nullptr;
            UINT32 numFrames = 0;
            DWORD flags = 0;

            hr = m_captureClient->GetBuffer(&captureData, &numFrames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                ReportRuntimeFailure(static_cast<DWORD>(hr));
                break;
            }

            if (numFrames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                const float* srcFloat = nullptr;
                std::vector<float> monoFloat;

                if (m_nativeIsFloat) {
                    srcFloat = reinterpret_cast<const float*>(captureData);
                } else if (m_nativeBits == 16) {
                    const int16_t* srcI16 = reinterpret_cast<const int16_t*>(captureData);
                    monoFloat.resize(numFrames);
                    if (m_nativeChannels == 1) {
                        for (UINT32 i = 0; i < numFrames; ++i)
                            monoFloat[i] = static_cast<float>(srcI16[i]) / 32768.0f;
                    } else {
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            double sum = 0.0;
                            for (UINT32 ch = 0; ch < m_nativeChannels; ++ch)
                                sum += srcI16[i * m_nativeChannels + ch];
                            monoFloat[i] = static_cast<float>(sum / (m_nativeChannels * 32768.0));
                        }
                    }
                    srcFloat = monoFloat.data();
                } else {
                    hr = m_captureClient->ReleaseBuffer(numFrames);
                    if (FAILED(hr)) {
                        ReportRuntimeFailure(static_cast<DWORD>(hr));
                        break;
                    }
                    hr = m_captureClient->GetNextPacketSize(&packetLength);
                    if (FAILED(hr)) {
                        ReportRuntimeFailure(static_cast<DWORD>(hr));
                        break;
                    }
                    continue;
                }

                std::vector<float> mono;
                if (m_nativeIsFloat) {
                    mono.resize(numFrames);
                    if (m_nativeChannels == 1) {
                        for (UINT32 i = 0; i < numFrames; ++i)
                            mono[i] = srcFloat[i];
                    } else {
                        for (UINT32 i = 0; i < numFrames; ++i) {
                            double sum = 0.0;
                            for (UINT32 ch = 0; ch < m_nativeChannels; ++ch)
                                sum += srcFloat[i * m_nativeChannels + ch];
                            mono[i] = static_cast<float>(sum / m_nativeChannels);
                        }
                    }
                } else {
                    mono.assign(srcFloat, srcFloat + numFrames);
                }

                g_audioLevel.store(CalculateAudioLevelFloat(mono.data(), numFrames, 1));

                const UINT32 outputFrames = static_cast<UINT32>(numFrames * m_resampleRatio) + 2;
                std::vector<BYTE> resampled(outputFrames * sizeof(int16_t));
                int16_t* out = reinterpret_cast<int16_t*>(resampled.data());
                UINT32 written = 0;

                while (true) {
                    double srcPos = m_resamplePhase;
                    UINT32 idx = static_cast<UINT32>(srcPos);
                    if (idx + 1 >= numFrames) break;
                    double frac = srcPos - idx;
                    float sample = static_cast<float>((1.0 - frac) * mono[idx] + frac * mono[idx + 1]);
                    int16_t s16 = static_cast<int16_t>(std::clamp(sample * 32768.0f, -32768.0f, 32767.0f));
                    out[written++] = s16;
                    m_resamplePhase += 1.0 / m_resampleRatio;
                }

                if (written > 0) {
                    const BYTE* begin = reinterpret_cast<const BYTE*>(out);
                    EnterCriticalSection(&g_audioLock);
                    g_audioData.insert(g_audioData.end(), begin, begin + written * sizeof(int16_t));
                    LeaveCriticalSection(&g_audioLock);

                    const size_t bytesWritten = written * sizeof(int16_t);

                    EnterCriticalSection(&g_streamingSessionCs);
                    if (g_activeStreamingSession && g_activeStreamingSession->IsRunning()) {
                        const bool useStreamingVadTrim = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
                        if (useStreamingVadTrim) {
                            std::vector<std::vector<BYTE>> streamingOutputs;
                            g_streamingVadTrimmer->ProcessPcm16(begin, bytesWritten, streamingOutputs);
                            for (const auto& chunk : streamingOutputs) {
                                if (!chunk.empty()) {
                                    g_activeStreamingSession->EnqueuePcmChunk(chunk.data(), chunk.size());
                                }
                            }
                        } else {
                            g_activeStreamingSession->EnqueuePcmChunk(begin, bytesWritten);
                        }
                    }
                    LeaveCriticalSection(&g_streamingSessionCs);

                    if (g_streamingVadReady && !(g_activeStreamingSession && g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive())) {
                        std::vector<float> floatBuf(written);
                        for (UINT32 i = 0; i < written; ++i)
                            floatBuf[i] = static_cast<float>(out[i]) / 32768.0f;

                        bool hasVoice = false;
                        g_asrEngine.Lock();
                        if (g_config.vadModel == L"firered") {
                            g_asrEngine.fireRedVad->Process(floatBuf.data(), static_cast<int>(floatBuf.size()));
                            hasVoice = g_asrEngine.fireRedVad->IsInSpeech();
                        } else {
                            g_asrEngine.vad->AcceptWaveform(floatBuf.data(), static_cast<int32_t>(floatBuf.size()));
                            hasVoice = g_asrEngine.vad->IsDetected();
                        }
                        g_asrEngine.Unlock();
                        if (hasVoice) g_vadDetectedVoice.store(true);
                    }

                    m_resamplePhase -= written / m_resampleRatio;
                }
            }

            hr = m_captureClient->ReleaseBuffer(numFrames);
            if (FAILED(hr)) {
                ReportRuntimeFailure(static_cast<DWORD>(hr));
                break;
            }
            hr = m_captureClient->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                ReportRuntimeFailure(static_cast<DWORD>(hr));
                break;
            }
        }
    }
}

std::vector<WasapiDeviceInfo> WasapiCapture::EnumerateDevices() {
    std::vector<WasapiDeviceInfo> devices;

    HRESULT hrInit = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    bool needUninit = SUCCEEDED(hrInit) && hrInit != RPC_E_CHANGED_MODE;
    bool needUninitFinal = needUninit;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        if (needUninit) CoUninitialize();
        return devices;
    }

    IMMDevice* defaultDevice = nullptr;
    std::wstring defaultId;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eCapture, eConsole, &defaultDevice))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(defaultDevice->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        defaultDevice->Release();
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &collection);
    if (SUCCEEDED(hr)) {
        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            IMMDevice* device = nullptr;
            if (FAILED(collection->Item(i, &device))) continue;

            WasapiDeviceInfo info;
            LPWSTR id = nullptr;
            if (SUCCEEDED(device->GetId(&id))) {
                info.id = id;
                CoTaskMemFree(id);
            }

            IPropertyStore* props = nullptr;
            if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &props))) {
                PROPVARIANT varName;
                PropVariantInit(&varName);
                if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &varName)) && varName.vt == VT_LPWSTR) {
                    info.name = varName.pwszVal;
                    PropVariantClear(&varName);
                }
                props->Release();
            }

            info.isDefault = (info.id == defaultId);
            devices.push_back(std::move(info));
            device->Release();
        }
        collection->Release();
    }

    enumerator->Release();
    if (needUninitFinal) CoUninitialize();
    return devices;
}
