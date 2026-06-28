#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "doubao_ime_asr.h"
#include "llm_refine.h"
#include "utils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

constexpr wchar_t kDefaultWav[] =
    L"models\\sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17\\test_wavs\\zh.wav";

uint16_t ReadU16(const char* p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(p[0]) |
                                 (static_cast<unsigned char>(p[1]) << 8));
}

uint32_t ReadU32(const char* p) {
    return static_cast<uint32_t>(static_cast<unsigned char>(p[0]) |
                                 (static_cast<unsigned char>(p[1]) << 8) |
                                 (static_cast<unsigned char>(p[2]) << 16) |
                                 (static_cast<unsigned char>(p[3]) << 24));
}

bool FileExists(const wchar_t* path) {
    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::wstring ConfigPath() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir = modulePath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    if (dir.size() >= 12 && dir.substr(dir.size() - 12) == L"\\build\\tools") {
        dir.resize(dir.size() - 12);
    } else if (dir.size() >= 6 && dir.substr(dir.size() - 6) == L"\\build") {
        dir.resize(dir.size() - 6);
    }
    return dir + L"\\config.json";
}

std::string ReadFileUtf8(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

bool LoadConfigCredentials(doubao_ime_asr::DoubaoImeConfig& cfg) {
    const std::string json = ReadFileUtf8(ConfigPath());
    if (json.empty()) return false;

    cfg.deviceId = ExtractJsonStr(json, "doubao_ime_device_id");
    cfg.cdid = ExtractJsonStr(json, "doubao_ime_cdid");
    cfg.token = llm::DecryptString(ExtractJsonStr(json, "doubao_ime_token"));
    return !cfg.deviceId.empty() && !cfg.cdid.empty() && !cfg.token.empty();
}

bool LoadWavPcm16Mono16k(const wchar_t* path, std::vector<BYTE>& pcm, std::wstring& error) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = L"failed to open wav";
        return false;
    }
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 12 || std::string(bytes.data(), 4) != "RIFF" ||
        std::string(bytes.data() + 8, 4) != "WAVE") {
        error = L"invalid wav header";
        return false;
    }

    bool fmtOk = false;
    size_t pos = 12;
    while (pos + 8 <= bytes.size()) {
        const std::string id(bytes.data() + pos, 4);
        const uint32_t size = ReadU32(bytes.data() + pos + 4);
        pos += 8;
        if (pos + size > bytes.size()) break;

        if (id == "fmt ") {
            if (size < 16) {
                error = L"invalid fmt chunk";
                return false;
            }
            const uint16_t audioFormat = ReadU16(bytes.data() + pos);
            const uint16_t channels = ReadU16(bytes.data() + pos + 2);
            const uint32_t sampleRate = ReadU32(bytes.data() + pos + 4);
            const uint16_t bitsPerSample = ReadU16(bytes.data() + pos + 14);
            fmtOk = audioFormat == 1 && channels == 1 && sampleRate == 16000 && bitsPerSample == 16;
            if (!fmtOk) {
                error = L"wav must be PCM 16kHz mono 16-bit";
                return false;
            }
        } else if (id == "data") {
            if (!fmtOk) {
                error = L"data chunk before valid fmt";
                return false;
            }
            pcm.assign(reinterpret_cast<const BYTE*>(bytes.data() + pos),
                       reinterpret_cast<const BYTE*>(bytes.data() + pos + size));
            if (pcm.empty()) {
                error = L"empty wav data";
                return false;
            }
            return true;
        }

        pos += size + (size & 1u);
    }
    error = L"missing wav data";
    return false;
}

bool RunWavProbe(const wchar_t* path,
                 const doubao_ime_asr::Credentials& credentials,
                 std::wstring& text,
                 std::wstring& error) {
    std::vector<BYTE> pcm;
    if (!LoadWavPcm16Mono16k(path, pcm, error)) {
        return false;
    }

    doubao_ime_asr::DoubaoImeConfig cfg;
    cfg.deviceId = credentials.deviceId;
    cfg.cdid = credentials.cdid;
    cfg.token = credentials.token;

    doubao_ime_asr::RealtimeClient client(cfg);
    if (!client.Connect(error)) {
        error = doubao_ime_asr::ErrorText(error);
        return false;
    }

    const size_t frameBytes = client.FrameBytes();
    if (frameBytes == 0) {
        error = L"invalid frame size";
        client.Close();
        return false;
    }

    for (size_t offset = 0; offset < pcm.size(); offset += frameBytes) {
        const size_t bytes = (std::min)(frameBytes, pcm.size() - offset);
        std::vector<BYTE> frame(pcm.begin() + static_cast<ptrdiff_t>(offset),
                                pcm.begin() + static_cast<ptrdiff_t>(offset + bytes));
        if (frame.size() < frameBytes) frame.resize(frameBytes, 0);
        const bool isLast = offset + bytes >= pcm.size();
        if (!client.SendPcmFrame(frame.data(), frame.size(), isLast, error)) {
            error = doubao_ime_asr::ErrorText(error);
            client.Close();
            return false;
        }
    }

    if (!client.Finish(15000, text, error)) {
        error = doubao_ime_asr::ErrorText(error);
        client.Close();
        return false;
    }
    client.Close();
    return !text.empty();
}

struct StreamingWavProbeStats {
    std::wstring text;
    std::wstring latestPartial;
    size_t partialCount = 0;
    size_t finalCount = 0;
    bool sessionFinished = false;
};

bool RunStreamingWavProbe(const wchar_t* path,
                          const doubao_ime_asr::Credentials& credentials,
                          StreamingWavProbeStats& stats,
                          std::wstring& error) {
    std::vector<BYTE> pcm;
    if (!LoadWavPcm16Mono16k(path, pcm, error)) {
        return false;
    }

    doubao_ime_asr::DoubaoImeConfig cfg;
    cfg.deviceId = credentials.deviceId;
    cfg.cdid = credentials.cdid;
    cfg.token = credentials.token;

    doubao_ime_asr::RealtimeClient client(cfg);
    if (!client.Connect(error)) {
        error = doubao_ime_asr::ErrorText(error);
        return false;
    }

    const size_t frameBytes = client.FrameBytes();
    if (frameBytes == 0) {
        error = L"invalid frame size";
        client.Close();
        return false;
    }

    std::mutex stateMutex;
    std::atomic<bool> drainStop{false};
    std::atomic<bool> drainFailed{false};
    std::wstring drainError;

    std::thread drainThread([&]() {
        while (!drainStop.load()) {
            doubao_ime_asr::RealtimeEvent ev;
            std::wstring receiveError;
            if (!client.PollEvent(200, ev, receiveError)) {
                if (!drainStop.load()) {
                    std::lock_guard<std::mutex> lock(stateMutex);
                    drainError = receiveError;
                    drainFailed.store(true);
                }
                break;
            }
            std::lock_guard<std::mutex> lock(stateMutex);
            if (!ev.partialText.empty() && ev.partialText != stats.latestPartial) {
                stats.latestPartial = ev.partialText;
                ++stats.partialCount;
                if (stats.finalCount == 0) stats.text = ev.partialText;
            }
            if (ev.transcriptionCompleted) {
                stats.text = ev.finalText;
                ++stats.finalCount;
            }
            if (ev.sessionFinished) {
                stats.sessionFinished = true;
                break;
            }
        }
    });

    auto stopDrain = [&](bool abortClient) {
        drainStop.store(true);
        if (abortClient) client.Abort();
        if (drainThread.joinable()) drainThread.join();
    };

    for (size_t offset = 0; offset < pcm.size() && !drainFailed.load(); offset += frameBytes) {
        const size_t bytes = (std::min)(frameBytes, pcm.size() - offset);
        std::vector<BYTE> frame(pcm.begin() + static_cast<ptrdiff_t>(offset),
                                pcm.begin() + static_cast<ptrdiff_t>(offset + bytes));
        if (frame.size() < frameBytes) frame.resize(frameBytes, 0);
        const bool isLast = offset + bytes >= pcm.size();
        if (!client.SendPcmFrame(frame.data(), frame.size(), isLast, error)) {
            error = doubao_ime_asr::ErrorText(error);
            stopDrain(true);
            client.Close();
            return false;
        }
        Sleep(static_cast<DWORD>((std::max)(cfg.frameMs, 1)));
    }

    if (drainFailed.load()) {
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            error = drainError.empty() ? L"receive failed" : drainError;
        }
        stopDrain(true);
        client.Close();
        error = doubao_ime_asr::ErrorText(error);
        return false;
    }

    if (!client.SendFinishSession(error)) {
        error = doubao_ime_asr::ErrorText(error);
        stopDrain(true);
        client.Close();
        return false;
    }

    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        if (drainFailed.load()) break;
        {
            std::lock_guard<std::mutex> lock(stateMutex);
            if (stats.sessionFinished) break;
        }
        Sleep(50);
    }

    if (drainFailed.load()) {
        std::lock_guard<std::mutex> lock(stateMutex);
        error = drainError.empty() ? L"receive failed" : drainError;
    }
    bool timedOut = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex);
        timedOut = !stats.sessionFinished;
    }

    stopDrain(timedOut || drainFailed.load());
    client.Close();

    if (drainFailed.load()) {
        error = doubao_ime_asr::ErrorText(error);
        return false;
    }
    if (timedOut) {
        error = doubao_ime_asr::ErrorText(L"timed out waiting for SessionFinished");
        return false;
    }
    return !stats.text.empty();
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    bool protocolOnly = false;
    bool fresh = false;
    bool streamingProbe = false;
    const wchar_t* wavPath = nullptr;
    for (int i = 1; i < argc; ++i) {
        if (wcscmp(argv[i], L"--protocol-only") == 0) {
            protocolOnly = true;
        } else if (wcscmp(argv[i], L"--fresh") == 0) {
            fresh = true;
        } else if (wcscmp(argv[i], L"--streaming") == 0) {
            streamingProbe = true;
        } else {
            wavPath = argv[i];
        }
    }

    doubao_ime_asr::DoubaoImeConfig cfg;
    const bool loadedConfigCredentials = !fresh && LoadConfigCredentials(cfg);
    printf("[doubao] config_credentials=%d config_path=%s\n",
           loadedConfigCredentials ? 1 : 0,
           WideToUtf8(ConfigPath()).c_str());
    printf("[doubao] protocol probe...\n");
    doubao_ime_asr::TestResult probe = doubao_ime_asr::TestConnection(cfg);
    printf("[doubao] protocol_ok=%d changed=%d device_id_len=%zu cdid_len=%zu token_len=%zu\n",
           probe.ok ? 1 : 0,
           probe.credentialsChanged ? 1 : 0,
           probe.credentials.deviceId.size(),
           probe.credentials.cdid.size(),
           probe.credentials.token.size());
    printf("[doubao] protocol_message=%s\n", WideToUtf8(probe.message).c_str());
    if (!probe.ok) return 2;

    if (protocolOnly) return 0;

    if (!wavPath) {
        wavPath = FileExists(kDefaultWav) ? kDefaultWav : nullptr;
    }
    if (!wavPath) {
        printf("[doubao] wav_probe=skipped default wav missing\n");
        return 0;
    }

    printf("[doubao] wav probe: %s\n", WideToUtf8(wavPath).c_str());
    std::wstring text;
    std::wstring error;
    if (!RunWavProbe(wavPath, probe.credentials, text, error)) {
        printf("[doubao] wav_ok=0 message=%s\n", WideToUtf8(error).c_str());
        return 3;
    }
    printf("[doubao] wav_ok=1 text_len=%zu text=%s\n", text.size(), WideToUtf8(text).c_str());

    if (streamingProbe) {
        printf("[doubao] streaming wav probe: %s\n", WideToUtf8(wavPath).c_str());
        StreamingWavProbeStats stats;
        std::wstring streamingError;
        if (!RunStreamingWavProbe(wavPath, probe.credentials, stats, streamingError)) {
            printf("[doubao] streaming_ok=0 message=%s partial_count=%zu final_count=%zu session_finished=%d latest_partial=%s text=%s\n",
                   WideToUtf8(streamingError).c_str(),
                   stats.partialCount,
                   stats.finalCount,
                   stats.sessionFinished ? 1 : 0,
                   WideToUtf8(stats.latestPartial).c_str(),
                   WideToUtf8(stats.text).c_str());
            return 4;
        }
        printf("[doubao] streaming_ok=1 partial_count=%zu final_count=%zu session_finished=%d text_len=%zu text=%s\n",
               stats.partialCount,
               stats.finalCount,
               stats.sessionFinished ? 1 : 0,
               stats.text.size(),
               WideToUtf8(stats.text).c_str());
    }
    return 0;
}
