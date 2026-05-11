#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "engine.h"
#include "utils.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <fstream>
#include <sstream>
#include <thread>
#include <shlobj.h>
#include <knownfolders.h>
#include <delayimp.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "winmm.lib")

bool EqualsIgnoreCase(std::wstring a, std::wstring b) {
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return a == b;
}

std::wstring AppDataDir() {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &path))) {
        result = path;
        CoTaskMemFree(path);
    }
    if (result.empty()) {
        wchar_t fallback[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, fallback, MAX_PATH);
        result = fallback;
        const size_t slash = result.find_last_of(L"\\/");
        if (slash != std::wstring::npos) result.resize(slash);
    }
    result += L"\\VoxType";
    CreateDirectoryW(result.c_str(), nullptr);
    return result;
}

std::wstring AppRootDir() {
    wchar_t modulePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    std::wstring dir = modulePath;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir.resize(slash);
    if (dir.size() >= 6 && dir.substr(dir.size() - 6) == L"\\build") {
        dir.resize(dir.size() - 6);
    }
    return dir;
}

std::wstring ConfigPath() {
    return AppRootDir() + L"\\config.json";
}

std::wstring DefaultModelDir(const std::wstring& modelId) {
    const std::wstring base = AppRootDir() + L"\\models\\";
    if (modelId == L"firered_aed") {
        return base + L"sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26";
    }
    if (modelId == L"sensevoice") {
        return base + L"sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17";
    }
    return base + L"sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25";
}

bool ModelDirExists(const std::wstring& dir) {
    if (dir.empty()) return false;
    DWORD attr = GetFileAttributesW(dir.c_str());
    return (attr != INVALID_FILE_ATTRIBUTES) && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool AnyModelDirExists() {
    const std::wstring base = AppRootDir() + L"\\models\\";
    const std::wstring dirs[] = {
        L"sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25",
        L"sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26",
        L"sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17"
    };
    for (const auto& d : dirs) {
        if (ModelDirExists(base + d)) return true;
    }
    return false;
}

bool RunModelDownloader(HWND hwnd) {
    std::wstring scriptPath = AppRootDir() + L"\\download_models.ps1";

    if (GetFileAttributesW(scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd,
            L"download_models.ps1 not found.\n\n"
            L"Please download models manually from:\n"
            L"https://github.com/k2-fsa/sherpa-onnx/releases",
            L"Script Not Found", MB_OK | MB_ICONWARNING);
        return false;
    }

    std::wstring cmd = L"-ExecutionPolicy Bypass -NoExit -File \"" + scriptPath + L"\"";

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"open";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = cmd.c_str();
    sei.lpDirectory = AppRootDir().c_str();
    sei.nShow = SW_SHOWNORMAL;

    if (!ShellExecuteExW(&sei)) {
        MessageBoxW(hwnd, L"Failed to start download.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    if (sei.hProcess) {
        std::thread([hProcess = sei.hProcess, hwnd, modelId = g_config.modelId]() {
            WaitForSingleObject(hProcess, INFINITE);
            CloseHandle(hProcess);
            std::wstring modelDir = DefaultModelDir(modelId);
            LPARAM lParam = ModelDirExists(modelDir)
                ? reinterpret_cast<LPARAM>(new std::wstring(std::move(modelDir)))
                : 0;
            PostMessageW(hwnd, WM_APP + 20, 0, lParam);
        }).detach();
    } else {
        PostMessageW(hwnd, WM_APP + 20, 0, 0);
    }

    return true;
}

std::wstring ModelDisplayName(const std::wstring& modelId) {
    if (modelId == L"firered_aed") return L"FireRedASR2 AED";
    if (modelId == L"sensevoice") return L"SenseVoiceSmall";
    if (modelId == L"baidu") return L"Baidu Cloud";
    if (modelId == L"volcengine") return L"Volcano Engine";
    return L"FireRedASR2 CTC";
}

int ModelIndex(const std::wstring& modelId) {
    if (modelId == L"firered_aed") return 1;
    if (modelId == L"sensevoice") return 2;
    return 0;
}

std::wstring ModelIdFromIndex(int index) {
    if (index == 1) return L"firered_aed";
    if (index == 2) return L"sensevoice";
    return L"firered_ctc";
}

std::string ExtractJsonString(const std::string& json, const std::string& key, const std::string& fallback) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return fallback;
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) return fallback;
    std::string value;
    bool escape = false;
    for (++pos; pos < json.size(); ++pos) {
        const char c = json[pos];
        if (escape) {
            switch (c) {
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(c); break;
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            break;
        } else {
            value.push_back(c);
        }
    }
    return value;
}

bool ExtractJsonBool(const std::string& json, const std::string& key, bool fallback) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return fallback;
    const size_t valueStart = json.find_first_not_of(" \t\r\n", pos + 1);
    if (valueStart == std::string::npos) return fallback;
    if (json.compare(valueStart, 4, "true") == 0) return true;
    if (json.compare(valueStart, 5, "false") == 0) return false;
    if (json.compare(valueStart, 1, "1") == 0) return true;
    if (json.compare(valueStart, 1, "0") == 0) return false;
    return fallback;
}

int ExtractJsonInt(const std::string& json, const std::string& key, int fallback) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return fallback;
    const size_t valueStart = json.find_first_not_of(" \t\r\n", pos + 1);
    if (valueStart == std::string::npos) return fallback;
    // Support both quoted string and raw number
    if (json[valueStart] == '"') {
        size_t end = json.find('"', valueStart + 1);
        if (end == std::string::npos) return fallback;
        try {
            return std::stoi(json.substr(valueStart + 1, end - valueStart - 1));
        } catch (...) {
            return fallback;
        }
    }
    size_t valueEnd = json.find_first_of(",}\r\n", valueStart);
    if (valueEnd == std::string::npos) valueEnd = json.size();
    try {
        return std::stoi(json.substr(valueStart, valueEnd - valueStart));
    } catch (...) {
        return fallback;
    }
}

void SaveCurrentProvider() {
    const std::wstring& name = g_config.llmProvider;
    if (name.empty()) return;
    std::string key = llm::WideToUtf8(name);
    std::string json = llm::WideToUtf8(g_config.llmProvidersJson);
    if (json.empty()) json = "{}";
    std::string encKey = llm::WideToUtf8(llm::EncryptString(g_config.llmApiKey));
    std::string entry = "{"
        "\"endpoint\":\"" + EscapeJson(g_config.llmEndpoint) + "\","
        "\"api_key\":\"" + encKey + "\","
        "\"model\":\"" + EscapeJson(g_config.llmModel) + "\"}";
    std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos != std::string::npos) {
        size_t objStart = json.find('{', pos);
        if (objStart != std::string::npos) {
            int depth = 0;
            size_t objEnd = objStart;
            for (; objEnd < json.size(); ++objEnd) {
                if (json[objEnd] == '{') depth++;
                else if (json[objEnd] == '}') { depth--; if (depth == 0) break; }
            }
            json.replace(objStart, objEnd - objStart + 1, entry);
        }
    } else {
        if (json == "{}") {
            json = "{" + marker + ":" + entry + "}";
        } else {
            json.pop_back();
            json += "," + marker + ":" + entry + "}";
        }
    }
    g_config.llmProvidersJson = llm::Utf8ToWide(json);
}

void LoadProviderFromStore(const std::wstring& name) {
    std::string json = llm::WideToUtf8(g_config.llmProvidersJson);
    std::string key = llm::WideToUtf8(name);
    size_t pos = json.find("\"" + key + "\"");
    if (pos == std::string::npos) return;
    std::string section = json.substr(pos);
    g_config.llmEndpoint = Utf8ToWide(ExtractJsonString(section, "endpoint", WideToUtf8(g_config.llmEndpoint)));
    g_config.llmApiKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(section, "api_key", "")));
    g_config.llmModel = Utf8ToWide(ExtractJsonString(section, "model", WideToUtf8(g_config.llmModel)));
}

int FindPresetIndex(const std::wstring& name) {
    for (int i = 0; i < llm::kProviderPresetCount; ++i) {
        if (name == llm::kProviderPresets[i].name) return i;
    }
    return -1;
}

void ApplyPreset(int index) {
    if (index < 0 || index >= llm::kProviderPresetCount) return;
    const auto& p = llm::kProviderPresets[index];
    g_config.llmProvider = p.name;
    g_config.llmEndpoint = p.url;
    g_config.llmModel = p.defaultModel;
    g_config.llmExtraParams = p.extraParams;
    LoadProviderFromStore(p.name);
}

void LoadConfig() {
    std::ifstream file(ConfigPath(), std::ios::binary);
    if (!file) return;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();
    g_config.modelId = Utf8ToWide(ExtractJsonString(json, "model_id", WideToUtf8(g_config.modelId)));
    g_config.modelDir = Utf8ToWide(ExtractJsonString(json, "model_dir", WideToUtf8(g_config.modelDir)));
    g_config.threads = Utf8ToWide(ExtractJsonString(json, "threads", WideToUtf8(g_config.threads)));
    g_config.enableVad = ExtractJsonBool(json, "enable_vad", g_config.enableVad);
    g_config.vadModel = Utf8ToWide(ExtractJsonString(json, "vad_model", WideToUtf8(g_config.vadModel)));
    g_config.enablePartial = ExtractJsonBool(json, "enable_partial", g_config.enablePartial);
    g_config.postprocess = Utf8ToWide(ExtractJsonString(json, "postprocess", WideToUtf8(g_config.postprocess)));
    g_config.hotkey = Utf8ToWide(ExtractJsonString(json, "hotkey", WideToUtf8(g_config.hotkey)));
    g_config.llmProvider = Utf8ToWide(ExtractJsonString(json, "llm_provider", ""));
    g_config.llmProvidersJson = Utf8ToWide(ExtractJsonString(json, "llm_providers_json", ""));
    g_config.llmEndpoint = Utf8ToWide(ExtractJsonString(json, "llm_endpoint", WideToUtf8(g_config.llmEndpoint)));
    g_config.llmApiKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "llm_api_key", "")));
    g_config.llmModel = Utf8ToWide(ExtractJsonString(json, "llm_model", WideToUtf8(g_config.llmModel)));
    g_config.llmPrompt = Utf8ToWide(ExtractJsonString(json, "llm_prompt", ""));
    g_config.enableLlmDebug = ExtractJsonBool(json, "enable_llm_debug", false);
    g_config.enableDebugMode = ExtractJsonBool(json, "enable_debug_mode", false);
    g_config.asrBackend = Utf8ToWide(ExtractJsonString(json, "asr_backend", WideToUtf8(g_config.asrBackend)));
    g_config.baiduApiKey = Utf8ToWide(ExtractJsonString(json, "baidu_api_key", ""));
    g_config.baiduSecretKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "baidu_secret_key", "")));
    g_config.baiduDevPid = ExtractJsonInt(json, "baidu_dev_pid", 1537);
    g_config.cloudProvider = Utf8ToWide(ExtractJsonString(json, "cloud_provider", "volcengine"));
    if (g_config.cloudProvider.empty()) g_config.cloudProvider = L"volcengine";
    g_config.volcApiKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "volc_api_key", "")));
    g_config.volcResourceId = Utf8ToWide(ExtractJsonString(json, "volc_resource_id", "volc.seedasr.sauc.duration"));
    g_config.volcMode = Utf8ToWide(ExtractJsonString(json, "volc_mode", "bigmodel"));
    g_config.volcLanguage = Utf8ToWide(ExtractJsonString(json, "volc_language", ""));
    g_config.volcEnableNonstream = ExtractJsonBool(json, "volc_enable_nonstream", false);
    g_config.volcEndWindowSize = _wtoi(Utf8ToWide(ExtractJsonString(json, "volc_end_window_size", "800")).c_str());
    if (g_config.volcEndWindowSize <= 0) g_config.volcEndWindowSize = 800;
    g_config.volcEnableDdc = ExtractJsonBool(json, "volc_enable_ddc", false);
    g_config.volcExtraParams = Utf8ToWide(ExtractJsonString(json, "volc_extra_params", ""));
    g_config.volcEnableContext = ExtractJsonBool(json, "volc_enable_context", false);
    g_config.volcContextHistory = ExtractJsonInt(json, "volc_context_history", 3);
    if (g_config.volcContextHistory < 1) g_config.volcContextHistory = 3;
    if (g_config.volcContextHistory > 20) g_config.volcContextHistory = 20;
    g_config.volcEnableMusicFc = ExtractJsonBool(json, "volc_enable_music_fc", false);
    g_config.volcEnablePoiFc = ExtractJsonBool(json, "volc_enable_poi_fc", false);
    g_config.volcForceToSpeechTime = ExtractJsonInt(json, "volc_force_to_speech_time", 0);
    g_config.volcEnableAccelerate = ExtractJsonBool(json, "volc_enable_accelerate", false);
    g_config.volcAccelerateScore = ExtractJsonInt(json, "volc_accelerate_score", 0);
    if (g_config.volcAccelerateScore < 0 || g_config.volcAccelerateScore > 20) g_config.volcAccelerateScore = 0;
    g_config.volcHotwordsId = Utf8ToWide(ExtractJsonString(json, "volc_hotwords_id", ""));
    g_config.volcHotwordsName = Utf8ToWide(ExtractJsonString(json, "volc_hotwords_name", ""));
    g_config.volcCorrectTableId = Utf8ToWide(ExtractJsonString(json, "volc_correct_table_id", ""));
    g_config.volcCorrectTableName = Utf8ToWide(ExtractJsonString(json, "volc_correct_table_name", ""));
    g_config.audioBackend = Utf8ToWide(ExtractJsonString(json, "audio_backend", WideToUtf8(g_config.audioBackend)));
    g_config.audioDeviceId = Utf8ToWide(ExtractJsonString(json, "audio_device_id", ""));
    if (g_config.modelDir.empty()) {
        g_config.modelDir = DefaultModelDir(g_config.modelId);
    }
    if (g_config.llmProvider.empty()) {
        if (!g_config.llmEndpoint.empty()) {
            g_config.llmProvider = L"Custom";
            SaveCurrentProvider();
        } else {
            ApplyPreset(0);
        }
    } else {
        int pi = FindPresetIndex(g_config.llmProvider);
        if (pi >= 0) {
            ApplyPreset(pi);
        } else {
            LoadProviderFromStore(g_config.llmProvider);
        }
    }
}

void SaveConfig() {
    SaveCurrentProvider();
    std::ofstream file(ConfigPath(), std::ios::binary | std::ios::trunc);
    file << "{\n"
         << "  \"model_id\": \"" << EscapeJson(g_config.modelId) << "\",\n"
         << "  \"model_dir\": \"" << EscapeJson(g_config.modelDir) << "\",\n"
         << "  \"threads\": \"" << EscapeJson(g_config.threads) << "\",\n"
         << "  \"enable_vad\": " << (g_config.enableVad ? "true" : "false") << ",\n"
         << "  \"vad_model\": \"" << EscapeJson(g_config.vadModel) << "\",\n"
         << "  \"enable_partial\": " << (g_config.enablePartial ? "true" : "false") << ",\n"
         << "  \"postprocess\": \"" << EscapeJson(g_config.postprocess) << "\",\n"
         << "  \"hotkey\": \"" << EscapeJson(g_config.hotkey) << "\",\n"
         << "  \"llm_provider\": \"" << EscapeJson(g_config.llmProvider) << "\",\n"
         << "  \"llm_providers_json\": \"" << EscapeJson(g_config.llmProvidersJson) << "\",\n"
         << "  \"llm_endpoint\": \"" << EscapeJson(g_config.llmEndpoint) << "\",\n"
         << "  \"llm_api_key\": \"" << EscapeJson(llm::EncryptString(g_config.llmApiKey)) << "\",\n"
         << "  \"llm_model\": \"" << EscapeJson(g_config.llmModel) << "\",\n"
         << "  \"llm_prompt\": \"" << EscapeJson(g_config.llmPrompt) << "\",\n"
         << "  \"enable_llm_debug\": " << (g_config.enableLlmDebug ? "true" : "false") << ",\n"
         << "  \"enable_debug_mode\": " << (g_config.enableDebugMode ? "true" : "false") << ",\n"
         << "  \"asr_backend\": \"" << EscapeJson(g_config.asrBackend) << "\",\n"
         << "  \"baidu_api_key\": \"" << EscapeJson(g_config.baiduApiKey) << "\",\n"
         << "  \"baidu_secret_key\": \"" << EscapeJson(llm::EncryptString(g_config.baiduSecretKey)) << "\",\n"
         << "  \"baidu_dev_pid\": " << g_config.baiduDevPid << ",\n"
         << "  \"cloud_provider\": \"" << EscapeJson(g_config.cloudProvider) << "\",\n"
         << "  \"volc_api_key\": \"" << EscapeJson(llm::EncryptString(g_config.volcApiKey)) << "\",\n"
         << "  \"volc_resource_id\": \"" << EscapeJson(g_config.volcResourceId) << "\",\n"
         << "  \"volc_mode\": \"" << EscapeJson(g_config.volcMode) << "\",\n"
         << "  \"volc_language\": \"" << EscapeJson(g_config.volcLanguage) << "\",\n"
         << "  \"volc_enable_nonstream\": " << (g_config.volcEnableNonstream ? "1" : "0") << ",\n"
         << "  \"volc_end_window_size\": " << g_config.volcEndWindowSize << ",\n"
         << "  \"volc_enable_ddc\": " << (g_config.volcEnableDdc ? "1" : "0") << ",\n"
         << "  \"volc_extra_params\": \"" << EscapeJson(g_config.volcExtraParams) << "\",\n"
         << "  \"volc_enable_context\": " << (g_config.volcEnableContext ? "1" : "0") << ",\n"
         << "  \"volc_context_history\": " << g_config.volcContextHistory << ",\n"
         << "  \"volc_enable_music_fc\": " << (g_config.volcEnableMusicFc ? "1" : "0") << ",\n"
         << "  \"volc_enable_poi_fc\": " << (g_config.volcEnablePoiFc ? "1" : "0") << ",\n"
         << "  \"volc_force_to_speech_time\": " << g_config.volcForceToSpeechTime << ",\n"
         << "  \"volc_enable_accelerate\": " << (g_config.volcEnableAccelerate ? "1" : "0") << ",\n"
         << "  \"volc_accelerate_score\": " << g_config.volcAccelerateScore << ",\n"
         << "  \"volc_hotwords_id\": \"" << EscapeJson(g_config.volcHotwordsId) << "\",\n"
         << "  \"volc_hotwords_name\": \"" << EscapeJson(g_config.volcHotwordsName) << "\",\n"
         << "  \"volc_correct_table_id\": \"" << EscapeJson(g_config.volcCorrectTableId) << "\",\n"
         << "  \"volc_correct_table_name\": \"" << EscapeJson(g_config.volcCorrectTableName) << "\",\n"
         << "  \"audio_backend\": \"" << EscapeJson(g_config.audioBackend) << "\",\n"
         << "  \"audio_device_id\": \"" << EscapeJson(g_config.audioDeviceId) << "\"\n"
         << "}\n";
}

float CalculateAudioLevel(const BYTE* data, DWORD bytes) {
    if (!data || bytes < sizeof(int16_t)) return 0.0f;

    const auto* samples = reinterpret_cast<const int16_t*>(data);
    const size_t count = bytes / sizeof(int16_t);
    double sum = 0.0;
    for (size_t i = 0; i < count; ++i) {
        const double v = static_cast<double>(samples[i]) / 32768.0;
        sum += v * v;
    }

    const double rms = std::sqrt(sum / static_cast<double>(count));
    const double db = 20.0 * std::log10(std::max(rms, 1e-6));
    const double normalized = (db + 50.0) / 40.0;
    return static_cast<float>(std::clamp(normalized, 0.0, 1.0));
}

float DpiScaleForWindow(HWND hwnd) {
    UINT dpi = hwnd ? GetDpiForWindow(hwnd) : 0;
    if (dpi == 0) dpi = GetDpiForSystem();
    return static_cast<float>(dpi) / 96.0f;
}

int DipToPx(float value, float scale) {
    return static_cast<int>(std::ceil(value * scale));
}

void CALLBACK WaveInProc(HWAVEIN waveIn, UINT msg, DWORD_PTR, DWORD_PTR param1, DWORD_PTR) {
    if (msg != WIM_DATA || waveIn != g_waveIn) return;
    auto* header = reinterpret_cast<WAVEHDR*>(param1);
    if (!header) return;

    if (header->dwBytesRecorded > 0) {
        const BYTE* begin = reinterpret_cast<const BYTE*>(header->lpData);
        g_audioLevel.store(CalculateAudioLevel(begin, header->dwBytesRecorded));

        EnterCriticalSection(&g_audioLock);
        g_audioData.insert(g_audioData.end(), begin, begin + header->dwBytesRecorded);
        LeaveCriticalSection(&g_audioLock);

        if (g_volcStreaming && g_volcSession.connected) {
            EnterCriticalSection(&g_volcAudioCs);
            g_volcPendingAudio.insert(g_volcPendingAudio.end(), begin, begin + header->dwBytesRecorded);
            LeaveCriticalSection(&g_volcAudioCs);
        }
    }

    if (g_captureActive) {
        header->dwBytesRecorded = 0;
        waveInAddBuffer(waveIn, header, sizeof(WAVEHDR));
    }
}

bool StartAudioCapture(std::wstring& error) {
    if (g_waveIn || g_wasapiCapture.IsInitialized()) return true;

    g_audioLevel.store(0.0f);
    g_hudSmoothedLevel = 0.0f;
    EnterCriticalSection(&g_audioLock);
    g_audioData.clear();
    LeaveCriticalSection(&g_audioLock);

    if (g_config.audioBackend == L"wasapi") {
        if (g_wasapiCapture.Init(g_config.audioDeviceId)) {
            if (g_wasapiCapture.Start(error)) {
                g_captureActive = true;
                return true;
            }
            printf("[Audio] WASAPI Start failed: %ls\n", error.c_str());
            g_wasapiCapture.Release();
            error.clear();
        } else {
            printf("[Audio] WASAPI Init failed, falling back to waveIn\n");
        }
    }

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = 1;
    format.nSamplesPerSec = 16000;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    MMRESULT result = waveInOpen(&g_waveIn, WAVE_MAPPER, &format, reinterpret_cast<DWORD_PTR>(WaveInProc), 0, CALLBACK_FUNCTION);
    if (result != MMSYSERR_NOERROR) {
        error = L"Microphone open failed";
        g_waveIn = nullptr;
        return false;
    }

    g_waveBuffers.assign(4, std::vector<BYTE>(format.nAvgBytesPerSec / 10));
    ZeroMemory(g_waveHeaders, sizeof(g_waveHeaders));
    g_captureActive = true;

    for (size_t i = 0; i < g_waveBuffers.size(); ++i) {
        g_waveHeaders[i].lpData = reinterpret_cast<LPSTR>(g_waveBuffers[i].data());
        g_waveHeaders[i].dwBufferLength = static_cast<DWORD>(g_waveBuffers[i].size());
        waveInPrepareHeader(g_waveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
        waveInAddBuffer(g_waveIn, &g_waveHeaders[i], sizeof(WAVEHDR));
    }

    result = waveInStart(g_waveIn);
    if (result != MMSYSERR_NOERROR) {
        error = L"Microphone start failed";
        g_captureActive = false;
        waveInReset(g_waveIn);
        for (auto& header : g_waveHeaders) waveInUnprepareHeader(g_waveIn, &header, sizeof(WAVEHDR));
        waveInClose(g_waveIn);
        g_waveIn = nullptr;
        return false;
    }

    return true;
}

std::vector<BYTE> StopAudioCapture() {
    if (g_wasapiCapture.IsInitialized()) {
        g_captureActive = false;
        g_wasapiCapture.Stop();
        g_wasapiCapture.Release();
    } else if (g_waveIn) {
        g_captureActive = false;
        waveInStop(g_waveIn);
        waveInReset(g_waveIn);
        for (auto& header : g_waveHeaders) {
            waveInUnprepareHeader(g_waveIn, &header, sizeof(WAVEHDR));
        }
        waveInClose(g_waveIn);
        g_waveIn = nullptr;
    }
    g_audioLevel.store(0.0f);

    std::vector<BYTE> data;
    EnterCriticalSection(&g_audioLock);
    data = g_audioData;
    g_audioData.clear();
    LeaveCriticalSection(&g_audioLock);
    g_waveBuffers.clear();
    return data;
}

int ResolveThreads(const std::wstring& threads) {
    if (threads == L"auto" || threads.empty()) {
        int n = static_cast<int>(std::thread::hardware_concurrency());
        return std::clamp(n < 1 ? 4 : n, 1, 8);
    }
    return std::clamp(_wtoi(threads.c_str()), 1, 8);
}

static bool TryLoadAsrDlls() {
    __try {
        HMODULE h = LoadLibraryW(L"sherpa-onnx-cxx-api.dll");
        if (h) { FreeLibrary(h); return true; }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string AsrEngine::MakeKey(const std::wstring& modelId, const std::wstring& modelDir, int threads) {
    return WideToUtf8(modelId) + "|" + WideToUtf8(modelDir) + "|" + std::to_string(threads);
}

bool AsrEngine::EnsureRecognizer(const Config& config) {
    if (!TryLoadAsrDlls()) return false;
    const std::wstring modelDir = config.modelDir.empty() ? DefaultModelDir(config.modelId) : config.modelDir;
    const int threads = ResolveThreads(config.threads);
    const std::string key = MakeKey(config.modelId, modelDir, threads);
    if (recognizer && recognizerKey == key) return true;

    sherpa_onnx::cxx::OfflineRecognizerConfig rc;
    rc.model_config.tokens = WideToUtf8(modelDir) + "\\tokens.txt";
    rc.model_config.num_threads = threads;
    rc.model_config.debug = false;

    if (config.modelId == L"firered_ctc") {
        rc.model_config.fire_red_asr_ctc.model = WideToUtf8(modelDir) + "\\model.int8.onnx";
    } else if (config.modelId == L"firered_aed") {
        rc.model_config.fire_red_asr.encoder = WideToUtf8(modelDir) + "\\encoder.int8.onnx";
        rc.model_config.fire_red_asr.decoder = WideToUtf8(modelDir) + "\\decoder.int8.onnx";
    } else if (config.modelId == L"sensevoice") {
        rc.model_config.sense_voice.model = WideToUtf8(modelDir) + "\\model.int8.onnx";
        rc.model_config.sense_voice.use_itn = true;
    }

    auto r = sherpa_onnx::cxx::OfflineRecognizer::Create(rc);
    if (!r.Get()) return false;
    recognizer = std::make_unique<sherpa_onnx::cxx::OfflineRecognizer>(std::move(r));
    recognizerKey = key;
    return true;
}

bool AsrEngine::EnsureVad(int threads) {
    if (!TryLoadAsrDlls()) return false;
    const std::string key = "vad|" + std::to_string(threads);
    if (vad && vadKey == key) return true;

    const std::wstring vadPath = AppRootDir() + L"\\models\\silero_vad.int8.onnx";
    if (GetFileAttributesW(vadPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    sherpa_onnx::cxx::VadModelConfig vc;
    vc.silero_vad.model = WideToUtf8(vadPath);
    vc.silero_vad.threshold = 0.5f;
    vc.silero_vad.min_silence_duration = 0.25f;
    vc.silero_vad.min_speech_duration = 0.25f;
    vc.silero_vad.max_speech_duration = 30.0f;
    vc.silero_vad.window_size = 512;
    vc.sample_rate = 16000;
    vc.num_threads = threads;

    auto v = sherpa_onnx::cxx::VoiceActivityDetector::Create(vc, 600.0f);
    if (!v.Get()) return false;
    vad = std::make_unique<sherpa_onnx::cxx::VoiceActivityDetector>(std::move(v));
    vadKey = key;
    return true;
}

bool AsrEngine::EnsureFireRedVad() {
    if (!TryLoadAsrDlls()) return false;
    const std::string key = "firered_vad";
    if (fireRedVad && fireRedVadKey == key) return true;

    firered_vad::FireRedVadConfig cfg;
    cfg.modelPath = WideToUtf8(AppRootDir() + L"\\models\\fireredvad_stream_vad_with_cache.onnx");
    if (GetFileAttributesW(Utf8ToWide(cfg.modelPath).c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    auto v = firered_vad::FireRedVad::Create(cfg);
    if (!v) return false;
    fireRedVad = std::move(v);
    fireRedVadKey = key;
    return true;
}

bool AsrEngine::EnsurePunctuation(int threads) {
    if (!TryLoadAsrDlls()) return false;
    const std::string key = "punct|" + std::to_string(threads);
    if (punctuation && punctKey == key) return true;

    const std::wstring punctPath = AppRootDir() +
        L"\\models\\sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8\\model.int8.onnx";
    if (GetFileAttributesW(punctPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    sherpa_onnx::cxx::OfflinePunctuationConfig pc;
    pc.model.ct_transformer = WideToUtf8(punctPath);
    pc.model.num_threads = threads;

    auto p = sherpa_onnx::cxx::OfflinePunctuation::Create(pc);
    if (!p.Get()) return false;
    punctuation = std::make_unique<sherpa_onnx::cxx::OfflinePunctuation>(std::move(p));
    punctKey = key;
    return true;
}

std::wstring AsrEngine::Recognize(const std::vector<float>& samples, int sampleRate, const Config& config) {
    const int threads = ResolveThreads(config.threads);

    {
        std::lock_guard<std::mutex> g(lock_);
        if (!EnsureRecognizer(config)) return L"ASR failed: model load error";
    }

    std::vector<float> workSamples = samples;

    if (config.enableVad && workSamples.size() > 0) {
        HiResTimer tVad;
        if (config.vadModel == L"firered") {
            std::lock_guard<std::mutex> g(lock_);
            if (EnsureFireRedVad()) {
                fireRedVad->Reset();
                bool hasSpeech = fireRedVad->Process(workSamples.data(), static_cast<int>(workSamples.size()));
                double ms = tVad.ElapsedMs();
                if (config.enableDebugMode) {
                    g_vadMs = ms;
                    g_vadModelName = L"FireRed";
                }
                if (!hasSpeech) return L"";
            }
        } else {
            std::lock_guard<std::mutex> g(lock_);
            if (EnsureVad(threads)) {
                vad->Reset();
                const size_t windowSize = 512;
                for (size_t i = 0; i < workSamples.size(); i += windowSize) {
                    size_t end = std::min(i + windowSize, workSamples.size());
                    vad->AcceptWaveform(workSamples.data() + i, static_cast<int32_t>(end - i));
                }
                vad->Flush();

                double ms = tVad.ElapsedMs();
                if (config.enableDebugMode) {
                    g_vadMs = ms;
                    g_vadModelName = L"Silero";
                }

                if (!vad->IsEmpty()) {
                    auto seg = vad->Front();
                    workSamples = std::move(seg.samples);
                }
            }
        }
    }

    if (workSamples.empty()) return L"";

    std::wstring text;
    {
        HiResTimer tAsr;
        std::lock_guard<std::mutex> g(lock_);
        auto stream = recognizer->CreateStream();
        stream.AcceptWaveform(sampleRate, workSamples.data(), static_cast<int32_t>(workSamples.size()));
        recognizer->Decode(&stream);
        auto result = recognizer->GetResult(&stream);
        text = Utf8ToWide(result.text);
        double ms = tAsr.ElapsedMs();
        if (config.enableDebugMode) g_asrDecodeMs = ms;
    }

    if (text == L"<sil>" || text == L"<blk>") return L"";

    if (config.postprocess == L"itn" || config.postprocess == L"punct" || config.postprocess == L"llm") {
        HiResTimer tPunct;
        std::lock_guard<std::mutex> g(lock_);
        if (EnsurePunctuation(threads)) {
            std::string utf8 = WideToUtf8(text);
            std::string punctuated = punctuation->AddPunctuation(utf8);
            text = Utf8ToWide(punctuated);
        }
        double ms = tPunct.ElapsedMs();
        if (config.enableDebugMode) g_punctMs = ms;
    }

    return text;
}

void AsrEngine::Reload() {
    std::lock_guard<std::mutex> g(lock_);
    recognizer.reset();
    vad.reset();
    punctuation.reset();
    fireRedVad.reset();
    recognizerKey.clear();
    vadKey.clear();
    fireRedVadKey.clear();
    punctKey.clear();
}

std::vector<float> PcmToFloat(const std::vector<BYTE>& pcm) {
    const size_t count = pcm.size() / 2;
    std::vector<float> samples(count);
    const auto* raw = reinterpret_cast<const int16_t*>(pcm.data());
    for (size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<float>(raw[i]) / 32768.0f;
    }
    return samples;
}

void PreloadAsrEngine(const Config& config) {
    const int threads = ResolveThreads(config.threads);
    g_asrEngine.Lock();
    g_asrEngine.EnsureRecognizer(config);
    if (config.enableVad) {
        if (config.vadModel == L"firered") {
            g_asrEngine.EnsureFireRedVad();
        } else {
            g_asrEngine.EnsureVad(threads);
        }
    }
    if (config.postprocess == L"itn" || config.postprocess == L"punct" || config.postprocess == L"llm") {
        g_asrEngine.EnsurePunctuation(threads);
    }
    g_asrEngine.Unlock();
}
