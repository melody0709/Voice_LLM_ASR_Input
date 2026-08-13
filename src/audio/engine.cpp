#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "engine.h"
#include "asr_streaming_session.h"
#include "streaming_vad_trimmer.h"
#include "utils.h"
#include "qwen_free_postprocess.h"
#include "qwen_audio_profile.h"
#include "qwen_special_word_filter.h"
#include "asr_runtime_log.h"

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

static constexpr int kCurrentConfigVersion = 12;

namespace {

constexpr wchar_t kPortableFlagName[] = L"portable.flag";

std::wstring CurrentExecutableDirectory() {
    for (DWORD capacity = MAX_PATH; capacity <= 32768; capacity *= 2) {
        std::vector<wchar_t> buffer(capacity, L'\0');
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), capacity);
        if (length == 0) break;
        if (length < capacity - 1) {
            std::wstring path(buffer.data(), length);
            const size_t slash = path.find_last_of(L"\\/");
            return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
        }
    }
    return {};
}

bool PathExistsAsFile(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool PathStartsWithIgnoreCase(const std::wstring& path, const std::wstring& prefix) {
    if (path.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (towlower(path[i]) != towlower(prefix[i])) return false;
    }
    return path.size() == prefix.size() ||
        prefix.empty() ||
        prefix.back() == L'\\' ||
        prefix.back() == L'/' ||
        path[prefix.size()] == L'\\' ||
        path[prefix.size()] == L'/';
}

std::wstring ParentDirectory(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring DirectoryLeafName(const std::wstring& path) {
    const size_t end = path.find_last_not_of(L"\\/");
    if (end == std::wstring::npos) return {};
    const size_t slash = path.find_last_of(L"\\/", end);
    const size_t start = slash == std::wstring::npos ? 0 : slash + 1;
    return path.substr(start, end - start + 1);
}

std::wstring DevelopmentRootConfigPath() {
    const std::wstring runtimeDir = RuntimeAssetDir();
    const std::wstring runDir = ParentDirectory(runtimeDir);
    const std::wstring buildDir = ParentDirectory(runDir);
    if (!EqualsIgnoreCase(DirectoryLeafName(runtimeDir), L"x64-release") ||
        !EqualsIgnoreCase(DirectoryLeafName(runDir), L"run") ||
        !EqualsIgnoreCase(DirectoryLeafName(buildDir), L"build")) {
        return {};
    }

    const std::wstring repositoryRoot = ParentDirectory(buildDir);
    return repositoryRoot.empty() ? std::wstring() : repositoryRoot + L"\\config.json";
}

void EnsureDirectory(const std::wstring& path) {
    if (!path.empty()) CreateDirectoryW(path.c_str(), nullptr);
}

void MigrateLegacyConfigIfNeeded() {
    if (IsPortableMode()) return;

    const std::wstring destination = ConfigPath();
    if (PathExistsAsFile(destination)) return;

    const std::wstring adjacentConfig = RuntimeAssetDir() + L"\\config.json";
    if (PathExistsAsFile(adjacentConfig) && CopyFileW(adjacentConfig.c_str(), destination.c_str(), TRUE)) {
        // Preserve the original file in case the user wants to keep using an
        // older Portable copy. DPAPI secrets remain usable for this user.
        return;
    }

    // The canonical development payload lives at build\run\x64-release,
    // while pre-CMake development builds kept config.json at the repository
    // root. This is deliberately an exact layout check, not a disk scan.
    const std::wstring developmentConfig = DevelopmentRootConfigPath();
    if (!developmentConfig.empty() && PathExistsAsFile(developmentConfig)) {
        CopyFileW(developmentConfig.c_str(), destination.c_str(), TRUE);
    }
}

bool ShouldFallbackFromLegacyModelDir(const std::wstring& modelDir) {
    if (modelDir.empty() || !ModelDirExists(modelDir)) return true;

    // Portable payloads still own their adjacent models. In a normal install,
    // a model under the current runtime directory is a legacy app-owned path,
    // not a user-selected model directory.
    return !IsPortableMode() &&
        PathStartsWithIgnoreCase(modelDir, RuntimeAssetDir() + L"\\models");
}

} // namespace

bool EqualsIgnoreCase(std::wstring a, std::wstring b) {
    std::transform(a.begin(), a.end(), a.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    std::transform(b.begin(), b.end(), b.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return a == b;
}

std::wstring AppRootDir() {
    return CurrentExecutableDirectory();
}

std::wstring RuntimeAssetDir() {
    return AppRootDir();
}

bool IsPortableMode() {
    return PathExistsAsFile(RuntimeAssetDir() + L"\\" + kPortableFlagName);
}

std::wstring MutableDataDir() {
    if (IsPortableMode()) {
        return RuntimeAssetDir();
    }

    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        result = path;
        CoTaskMemFree(path);
    }
    if (result.empty()) {
        // The executable directory is the only safe fallback when Windows
        // cannot resolve the current user's data folder.
        return RuntimeAssetDir();
    }

    result += L"\\VoxType";
    EnsureDirectory(result);
    return result;
}

std::wstring AppDataDir() {
    return MutableDataDir();
}

std::wstring DownloadedModelRoot() {
    return MutableDataDir() + L"\\models";
}

std::wstring LogDir() {
    const std::wstring result = MutableDataDir() + L"\\log";
    EnsureDirectory(result);
    return result;
}

std::wstring ConfigPath() {
    return MutableDataDir() + L"\\config.json";
}

std::wstring DefaultModelDir(const std::wstring& modelId) {
    const std::wstring base = DownloadedModelRoot() + L"\\";
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
    const std::wstring base = DownloadedModelRoot() + L"\\";
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
    const std::wstring scriptPath = RuntimeAssetDir() + L"\\download_models.ps1";

    if (GetFileAttributesW(scriptPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(hwnd,
            L"download_models.ps1 not found.\n\n"
            L"Please download models manually from:\n"
            L"https://github.com/k2-fsa/sherpa-onnx/releases",
            L"Script Not Found", MB_OK | MB_ICONWARNING);
        return false;
    }

    const std::wstring aria2Path = RuntimeAssetDir() + L"\\aria2c.exe";
    const std::wstring cmd = L"-ExecutionPolicy Bypass -NoExit -File \"" + scriptPath +
        L"\" -Destination \"" + DownloadedModelRoot() +
        L"\" -Aria2Path \"" + aria2Path + L"\"";

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.hwnd = nullptr;
    sei.lpVerb = L"open";
    sei.lpFile = L"powershell.exe";
    sei.lpParameters = cmd.c_str();
    const std::wstring workingDirectory = RuntimeAssetDir();
    sei.lpDirectory = workingDirectory.c_str();
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
    if (modelId == L"mimo") return L"MiMo ASR";
    if (modelId == L"doubao_ime") return L"Doubao IME";
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

float ExtractJsonFloat(const std::string& json, const std::string& key, float fallback) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return fallback;
    const size_t valueStart = json.find_first_not_of(" \t\r\n", pos + 1);
    if (valueStart == std::string::npos) return fallback;
    if (json[valueStart] == '"') {
        size_t end = json.find('"', valueStart + 1);
        if (end == std::string::npos) return fallback;
        try {
            return std::stof(json.substr(valueStart + 1, end - valueStart - 1));
        } catch (...) {
            return fallback;
        }
    }
    size_t valueEnd = json.find_first_of(",}\r\n", valueStart);
    if (valueEnd == std::string::npos) valueEnd = json.size();
    try {
        return std::stof(json.substr(valueStart, valueEnd - valueStart));
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
    MigrateLegacyConfigIfNeeded();
    std::ifstream file(ConfigPath(), std::ios::binary);
    if (!file) return;
    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string json = buffer.str();
    bool migratePlaintextQwenUtdid = false;
    g_config.modelId = Utf8ToWide(ExtractJsonString(json, "model_id", WideToUtf8(g_config.modelId)));
    g_config.modelDir = Utf8ToWide(ExtractJsonString(json, "model_dir", WideToUtf8(g_config.modelDir)));
    g_config.threads = Utf8ToWide(ExtractJsonString(json, "threads", WideToUtf8(g_config.threads)));
    g_config.enableVad = ExtractJsonBool(json, "enable_vad", g_config.enableVad);
    g_config.vadModel = Utf8ToWide(ExtractJsonString(json, "vad_model", WideToUtf8(g_config.vadModel)));
    g_config.vadThreshold = ExtractJsonFloat(json, "vad_threshold", g_config.vadThreshold);
    g_config.vadMinSilence = ExtractJsonInt(json, "vad_min_silence", g_config.vadMinSilence);
    g_config.vadMinSpeech = ExtractJsonInt(json, "vad_min_speech", g_config.vadMinSpeech);
    g_config.vadPadStart = ExtractJsonInt(json, "vad_pad_start", g_config.vadPadStart);
    g_config.vadSmoothWindow = ExtractJsonInt(json, "vad_smooth_window", g_config.vadSmoothWindow);
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
    g_config.forceUnicodeInput = ExtractJsonBool(json, "force_unicode_input", false);
    g_config.asrBackend = Utf8ToWide(ExtractJsonString(json, "asr_backend", WideToUtf8(g_config.asrBackend)));
    g_config.fallbackAsrBackend = Utf8ToWide(ExtractJsonString(json, "fallback_asr_backend", "none"));
    if (g_config.fallbackAsrBackend.empty()) g_config.fallbackAsrBackend = L"none";
    g_config.baiduApiKey = Utf8ToWide(ExtractJsonString(json, "baidu_api_key", ""));
    g_config.baiduSecretKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "baidu_secret_key", "")));
    g_config.baiduDevPid = ExtractJsonInt(json, "baidu_dev_pid", 1537);
    g_config.cloudProvider = Utf8ToWide(ExtractJsonString(json, "cloud_provider", "volcengine"));
    if (g_config.cloudProvider.empty()) g_config.cloudProvider = L"volcengine";
    g_config.volcApiKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "volc_api_key", "")));
    g_config.volcResourceId = Utf8ToWide(ExtractJsonString(json, "volc_resource_id", "volc.seedasr.sauc.duration"));
    g_config.volcMode = Utf8ToWide(ExtractJsonString(json, "volc_mode", "bigmodel_nostream"));
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
    g_config.volcEnableInputContext = ExtractJsonBool(json, "volc_enable_input_context", false);
    g_config.volcEnableMusicFc = ExtractJsonBool(json, "volc_enable_music_fc", false);
    g_config.volcEnablePoiFc = ExtractJsonBool(json, "volc_enable_poi_fc", false);
    g_config.volcForceToSpeechTime = ExtractJsonInt(json, "volc_force_to_speech_time", 0);
    g_config.volcHotwordsId = Utf8ToWide(ExtractJsonString(json, "volc_hotwords_id", ""));
    g_config.volcHotwordsName = Utf8ToWide(ExtractJsonString(json, "volc_hotwords_name", ""));
    g_config.volcCorrectTableId = Utf8ToWide(ExtractJsonString(json, "volc_correct_table_id", ""));
    g_config.volcCorrectTableName = Utf8ToWide(ExtractJsonString(json, "volc_correct_table_name", ""));
    const bool hasPersistedQwenModel = json.find("\"qwen_model\"") != std::string::npos;
    const bool hasPersistedQwenTransport = json.find("\"qwen_transport\"") != std::string::npos;
    g_config.qwenApiKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "qwen_api_key", "")));
    g_config.qwenBaseUrl = Utf8ToWide(ExtractJsonString(json, "qwen_base_url", WideToUtf8(g_config.qwenBaseUrl)));
    g_config.qwenModel = Utf8ToWide(ExtractJsonString(json, "qwen_model", WideToUtf8(g_config.qwenModel)));
    g_config.qwenTransport = Utf8ToWide(ExtractJsonString(json, "qwen_transport", WideToUtf8(g_config.qwenTransport)));
    g_config.qwenHttpBaseUrl = Utf8ToWide(ExtractJsonString(json, "qwen_http_base_url", WideToUtf8(g_config.qwenHttpBaseUrl)));
    g_config.qwenAudioStreamingBaseUrl = Utf8ToWide(ExtractJsonString(json, "qwen_audio_streaming_base_url", WideToUtf8(g_config.qwenAudioStreamingBaseUrl)));
    g_config.qwenLanguage = Utf8ToWide(ExtractJsonString(json, "qwen_language", ""));
    g_config.qwenChunkMs = ExtractJsonInt(json, "qwen_chunk_ms", g_config.qwenChunkMs);
    g_config.qwenLanguageHints = Utf8ToWide(ExtractJsonString(
        json, "qwen_language_hints", WideToUtf8(g_config.qwenLanguageHints)));
    g_config.qwenVocabularyId = Utf8ToWide(ExtractJsonString(json, "qwen_vocabulary_id", ""));
    g_config.qwenVocabulary = Utf8ToWide(ExtractJsonString(json, "qwen_vocabulary", ""));
    g_config.qwenSemanticPunctuation = ExtractJsonBool(json, "qwen_semantic_punctuation", g_config.qwenSemanticPunctuation);
    g_config.qwenMaxSentenceSilenceMs = std::clamp(ExtractJsonInt(json, "qwen_max_sentence_silence", g_config.qwenMaxSentenceSilenceMs), 200, 6000);
    g_config.qwenMultiThresholdMode = ExtractJsonBool(json, "qwen_multi_threshold", g_config.qwenMultiThresholdMode);
    g_config.qwenHeartbeat = ExtractJsonBool(json, "qwen_heartbeat", g_config.qwenHeartbeat);
    g_config.qwenSpeechNoiseThresholdEnabled = ExtractJsonBool(json, "qwen_speech_noise_threshold_enabled", g_config.qwenSpeechNoiseThresholdEnabled);
    g_config.qwenSpeechNoiseThreshold = std::clamp(ExtractJsonFloat(json, "qwen_speech_noise_threshold", g_config.qwenSpeechNoiseThreshold), -1.0f, 1.0f);
    g_config.qwenEnableInputContext = ExtractJsonBool(json, "qwen_enable_input_context", g_config.qwenEnableInputContext);
    g_config.qwenEnableContinueContext = ExtractJsonBool(json, "qwen_enable_continue_context", g_config.qwenEnableContinueContext);
    // Dynamic refresh must never bypass the primary focused-field context
    // opt-in, including when loading an older hand-edited config file.
    g_config.qwenEnableContinueContext =
        g_config.qwenEnableContinueContext && g_config.qwenEnableInputContext;
    g_config.qwenSpecialWordReplaceList = Utf8ToWide(ExtractJsonString(
        json, "qwen_special_word_replace", WideToUtf8(g_config.qwenSpecialWordReplaceList)));
    g_config.qwenSpecialWordEmptyList = Utf8ToWide(ExtractJsonString(
        json, "qwen_special_word_empty", WideToUtf8(g_config.qwenSpecialWordEmptyList)));
    g_config.qwenSystemReservedFilter = ExtractJsonBool(
        json, "qwen_system_reserved_filter", g_config.qwenSystemReservedFilter);
    {
        qwen_special_word_filter::Config normalized;
        std::wstring filterError;
        if (qwen_special_word_filter::Normalize(
                g_config.qwenSpecialWordReplaceList,
                g_config.qwenSpecialWordEmptyList,
                g_config.qwenSystemReservedFilter,
                normalized, &filterError)) {
            g_config.qwenSpecialWordReplaceList =
                qwen_special_word_filter::JoinLines(normalized.replaceWords);
            g_config.qwenSpecialWordEmptyList =
                qwen_special_word_filter::JoinLines(normalized.emptyWords);
        } else {
            g_config.qwenSpecialWordReplaceList.clear();
            g_config.qwenSpecialWordEmptyList.clear();
            g_config.qwenSystemReservedFilter = false;
        }
    }

    // Older builds used the public DashScope endpoint for the realtime
    // profile.  Migrate only that exact legacy default; preserve any explicit
    // user-customized endpoint.
    constexpr wchar_t kOldQwenRealtimeBaseUrl[] =
        L"wss://dashscope.aliyuncs.com/api-ws/v1/realtime";
    constexpr wchar_t kOldQwenHttpBaseUrl[] =
        L"https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation";
    constexpr wchar_t kOldQwenAudioStreamingBaseUrl[] =
        L"wss://dashscope.aliyuncs.com/api-ws/v1/inference";
    if (g_config.qwenBaseUrl.empty() || g_config.qwenBaseUrl == kOldQwenRealtimeBaseUrl) {
        g_config.qwenBaseUrl = kQwenBeijingRealtimeBaseUrl;
    }
    if (g_config.qwenHttpBaseUrl.empty()) g_config.qwenHttpBaseUrl = kQwenBeijingHttpBaseUrl;
    if (g_config.qwenHttpBaseUrl == kOldQwenHttpBaseUrl) g_config.qwenHttpBaseUrl = kQwenBeijingHttpBaseUrl;
    if (g_config.qwenAudioStreamingBaseUrl.empty()) g_config.qwenAudioStreamingBaseUrl = kQwenBeijingAudioStreamingBaseUrl;
    if (g_config.qwenAudioStreamingBaseUrl == kOldQwenAudioStreamingBaseUrl) {
        g_config.qwenAudioStreamingBaseUrl = kQwenBeijingAudioStreamingBaseUrl;
    }
    qwen_audio_profile::NormalizePersistedProfile(
        g_config.qwenModel,
        g_config.qwenTransport,
        hasPersistedQwenModel,
        hasPersistedQwenTransport);
    g_config.qwenChunkMs = std::clamp(g_config.qwenChunkMs, 20, 1000);
    g_config.mimoApiKey = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "mimo_api_key", "")));
    g_config.mimoBaseUrl = Utf8ToWide(ExtractJsonString(json, "mimo_base_url", WideToUtf8(g_config.mimoBaseUrl)));
    g_config.mimoModel = Utf8ToWide(ExtractJsonString(json, "mimo_model", WideToUtf8(g_config.mimoModel)));
    g_config.mimoLanguage = Utf8ToWide(ExtractJsonString(json, "mimo_language", WideToUtf8(g_config.mimoLanguage)));
    if (g_config.mimoBaseUrl.empty()) g_config.mimoBaseUrl = L"https://token-plan-ams.xiaomimimo.com/v1";
    if (g_config.mimoModel.empty()) g_config.mimoModel = L"mimo-v2.5-asr";
    if (g_config.mimoLanguage != L"zh" && g_config.mimoLanguage != L"en") g_config.mimoLanguage = L"auto";
    g_config.doubaoImeDeviceId = Utf8ToWide(ExtractJsonString(json, "doubao_ime_device_id", ""));
    g_config.doubaoImeCdid = Utf8ToWide(ExtractJsonString(json, "doubao_ime_cdid", ""));
    g_config.doubaoImeToken = llm::DecryptString(Utf8ToWide(ExtractJsonString(json, "doubao_ime_token", "")));
    // QwenFree (千问 IME 免费后端，A1 纯协议还原)
    g_config.qwenFreePolishEnabled = ExtractJsonBool(json, "qwen_free_polish", false);
    g_config.qwenFreePunctEnabled = ExtractJsonBool(json, "qwen_free_punct", false);
    g_config.qwenFreeCorrectEnabled = ExtractJsonBool(json, "qwen_free_correct", false);
    // Keep the persisted key and implementation for future research, but
    // temporarily disable selection rewrite regardless of the stored value.
    (void)ExtractJsonBool(json, "qwen_free_rewrite", false);
    g_config.qwenFreeRewriteEnabled = false;
    g_config.qwenFreeDebugLog = ExtractJsonBool(json, "qwen_free_debug_log", false);
    g_config.qwenFreeShellPath = Utf8ToWide(ExtractJsonString(json, "qwen_free_shell_path", ""));
    {
        const std::wstring storedUtdid = Utf8ToWide(
            ExtractJsonString(json, "qwen_free_utdid_override", ""));
        const std::wstring decryptedUtdid = llm::DecryptString(storedUtdid);
        // One-time migration from versions that stored the debug identity as
        // plaintext. Invalid encrypted data is rejected later by UTDID shape
        // validation and is never copied into logs.
        g_config.qwenFreeUtdidOverride = decryptedUtdid.empty()
            ? storedUtdid : decryptedUtdid;
        migratePlaintextQwenUtdid =
            storedUtdid.size() == 24 && decryptedUtdid.empty() &&
            std::all_of(storedUtdid.begin(), storedUtdid.end(), [](wchar_t ch) {
                return (ch >= L'0' && ch <= L'9') ||
                       (ch >= L'A' && ch <= L'Z') ||
                       (ch >= L'a' && ch <= L'z');
            });
    }
    NormalizeQwenFreePostProcessConfig(g_config);
    asr_runtime_log::SetQwenFreeEnabled(
        g_config.qwenFreeDebugLog &&
        (g_config.asrBackend == L"qwen_free" ||
         g_config.fallbackAsrBackend == L"qwen_free"));
    g_config.audioBackend = Utf8ToWide(ExtractJsonString(json, "audio_backend", WideToUtf8(g_config.audioBackend)));
    g_config.audioDeviceId = Utf8ToWide(ExtractJsonString(json, "audio_device_id", ""));
    g_config.configVersion = ExtractJsonInt(json, "config_version", 0);

    if (g_config.configVersion < 1) {
        if (g_config.volcMode.empty()) g_config.volcMode = L"bigmodel_nostream";
        if (g_config.volcResourceId.empty()) g_config.volcResourceId = L"volc.seedasr.sauc.duration";
        if (g_config.cloudProvider.empty()) g_config.cloudProvider = L"volcengine";
    }
    if (g_config.configVersion < 2) {
        g_config.vadThreshold = 0.15f;
        g_config.vadMinSilence = 500;
        g_config.vadMinSpeech = 30;
        g_config.vadPadStart = 150;
    }
    if (g_config.configVersion < 9 &&
        ShouldFallbackFromLegacyModelDir(g_config.modelDir)) {
        g_config.modelDir = DefaultModelDir(g_config.modelId);
    }
    if (g_config.configVersion < 12 && g_config.qwenLanguageHints.empty()) {
        g_config.qwenLanguageHints = kQwenDefaultLanguageHints;
    }
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

    if (g_config.configVersion < kCurrentConfigVersion ||
        migratePlaintextQwenUtdid) {
        g_config.configVersion = kCurrentConfigVersion;
        SaveConfig();
    }
}

void SaveConfig() {
    SaveCurrentProvider();
    NormalizeQwenFreePostProcessConfig(g_config);
    asr_runtime_log::SetQwenFreeEnabled(
        g_config.qwenFreeDebugLog &&
        (g_config.asrBackend == L"qwen_free" ||
         g_config.fallbackAsrBackend == L"qwen_free"));
    std::ofstream file(ConfigPath(), std::ios::binary | std::ios::trunc);
    file << "{\n"
         << "  \"config_version\": " << g_config.configVersion << ",\n"
         << "  \"model_id\": \"" << EscapeJson(g_config.modelId) << "\",\n"
         << "  \"model_dir\": \"" << EscapeJson(g_config.modelDir) << "\",\n"
         << "  \"threads\": \"" << EscapeJson(g_config.threads) << "\",\n"
         << "  \"enable_vad\": " << (g_config.enableVad ? "true" : "false") << ",\n"
         << "  \"vad_model\": \"" << EscapeJson(g_config.vadModel) << "\",\n"
         << "  \"vad_threshold\": " << g_config.vadThreshold << ",\n"
         << "  \"vad_min_silence\": " << g_config.vadMinSilence << ",\n"
         << "  \"vad_min_speech\": " << g_config.vadMinSpeech << ",\n"
         << "  \"vad_pad_start\": " << g_config.vadPadStart << ",\n"
         << "  \"vad_smooth_window\": " << g_config.vadSmoothWindow << ",\n"
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
         << "  \"force_unicode_input\": " << (g_config.forceUnicodeInput ? "true" : "false") << ",\n"
         << "  \"asr_backend\": \"" << EscapeJson(g_config.asrBackend) << "\",\n"
         << "  \"fallback_asr_backend\": \"" << EscapeJson(g_config.fallbackAsrBackend) << "\",\n"
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
         << "  \"volc_enable_input_context\": " << (g_config.volcEnableInputContext ? "1" : "0") << ",\n"
         << "  \"volc_enable_music_fc\": " << (g_config.volcEnableMusicFc ? "1" : "0") << ",\n"
         << "  \"volc_enable_poi_fc\": " << (g_config.volcEnablePoiFc ? "1" : "0") << ",\n"
         << "  \"volc_force_to_speech_time\": " << g_config.volcForceToSpeechTime << ",\n"
         << "  \"volc_hotwords_id\": \"" << EscapeJson(g_config.volcHotwordsId) << "\",\n"
         << "  \"volc_hotwords_name\": \"" << EscapeJson(g_config.volcHotwordsName) << "\",\n"
         << "  \"volc_correct_table_id\": \"" << EscapeJson(g_config.volcCorrectTableId) << "\",\n"
         << "  \"volc_correct_table_name\": \"" << EscapeJson(g_config.volcCorrectTableName) << "\",\n"
         << "  \"qwen_api_key\": \"" << EscapeJson(llm::EncryptString(g_config.qwenApiKey)) << "\",\n"
         << "  \"qwen_base_url\": \"" << EscapeJson(g_config.qwenBaseUrl) << "\",\n"
         << "  \"qwen_http_base_url\": \"" << EscapeJson(g_config.qwenHttpBaseUrl) << "\",\n"
         << "  \"qwen_audio_streaming_base_url\": \"" << EscapeJson(g_config.qwenAudioStreamingBaseUrl) << "\",\n"
         << "  \"qwen_model\": \"" << EscapeJson(g_config.qwenModel) << "\",\n"
         << "  \"qwen_transport\": \"" << EscapeJson(g_config.qwenTransport) << "\",\n"
         << "  \"qwen_language\": \"" << EscapeJson(g_config.qwenLanguage) << "\",\n"
         << "  \"qwen_chunk_ms\": " << g_config.qwenChunkMs << ",\n"
         << "  \"qwen_language_hints\": \"" << EscapeJson(g_config.qwenLanguageHints) << "\",\n"
         << "  \"qwen_vocabulary_id\": \"" << EscapeJson(g_config.qwenVocabularyId) << "\",\n"
         << "  \"qwen_vocabulary\": \"" << EscapeJson(g_config.qwenVocabulary) << "\",\n"
         << "  \"qwen_semantic_punctuation\": " << (g_config.qwenSemanticPunctuation ? "true" : "false") << ",\n"
         << "  \"qwen_max_sentence_silence\": " << g_config.qwenMaxSentenceSilenceMs << ",\n"
         << "  \"qwen_multi_threshold\": " << (g_config.qwenMultiThresholdMode ? "true" : "false") << ",\n"
         << "  \"qwen_heartbeat\": " << (g_config.qwenHeartbeat ? "true" : "false") << ",\n"
         << "  \"qwen_speech_noise_threshold_enabled\": " << (g_config.qwenSpeechNoiseThresholdEnabled ? "true" : "false") << ",\n"
         << "  \"qwen_speech_noise_threshold\": " << g_config.qwenSpeechNoiseThreshold << ",\n"
         << "  \"qwen_enable_input_context\": " << (g_config.qwenEnableInputContext ? "true" : "false") << ",\n"
         << "  \"qwen_enable_continue_context\": " << (g_config.qwenEnableContinueContext ? "true" : "false") << ",\n"
         << "  \"qwen_special_word_replace\": \"" << EscapeJson(g_config.qwenSpecialWordReplaceList) << "\",\n"
         << "  \"qwen_special_word_empty\": \"" << EscapeJson(g_config.qwenSpecialWordEmptyList) << "\",\n"
         << "  \"qwen_system_reserved_filter\": " << (g_config.qwenSystemReservedFilter ? "true" : "false") << ",\n"
         << "  \"mimo_api_key\": \"" << EscapeJson(llm::EncryptString(g_config.mimoApiKey)) << "\",\n"
         << "  \"mimo_base_url\": \"" << EscapeJson(g_config.mimoBaseUrl) << "\",\n"
         << "  \"mimo_model\": \"" << EscapeJson(g_config.mimoModel) << "\",\n"
         << "  \"mimo_language\": \"" << EscapeJson(g_config.mimoLanguage) << "\",\n"
         << "  \"doubao_ime_device_id\": \"" << EscapeJson(g_config.doubaoImeDeviceId) << "\",\n"
         << "  \"doubao_ime_cdid\": \"" << EscapeJson(g_config.doubaoImeCdid) << "\",\n"
         << "  \"doubao_ime_token\": \"" << EscapeJson(llm::EncryptString(g_config.doubaoImeToken)) << "\",\n"
         << "  \"qwen_free_polish\": " << (g_config.qwenFreePolishEnabled ? 1 : 0) << ",\n"
         << "  \"qwen_free_punct\": " << (g_config.qwenFreePunctEnabled ? 1 : 0) << ",\n"
         << "  \"qwen_free_correct\": " << (g_config.qwenFreeCorrectEnabled ? 1 : 0) << ",\n"
         << "  \"qwen_free_rewrite\": " << (g_config.qwenFreeRewriteEnabled ? 1 : 0) << ",\n"
         << "  \"qwen_free_debug_log\": " << (g_config.qwenFreeDebugLog ? 1 : 0) << ",\n"
         << "  \"qwen_free_shell_path\": \"" << EscapeJson(g_config.qwenFreeShellPath) << "\",\n"
         << "  \"qwen_free_utdid_override\": \""
         << EscapeJson(llm::EncryptString(g_config.qwenFreeUtdidOverride)) << "\",\n"
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

        EnterCriticalSection(&g_streamingSessionCs);
        if (g_activeStreamingSession && g_activeStreamingSession->IsRunning()) {
            const bool useStreamingVadTrim = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
            if (useStreamingVadTrim) {
                std::vector<std::vector<BYTE>> streamingOutputs;
                g_streamingVadTrimmer->ProcessPcm16(begin, header->dwBytesRecorded, streamingOutputs);
                for (const auto& chunk : streamingOutputs) {
                    if (!chunk.empty()) {
                        g_activeStreamingSession->EnqueuePcmChunk(chunk.data(), chunk.size());
                    }
                }
            } else {
                g_activeStreamingSession->EnqueuePcmChunk(begin, header->dwBytesRecorded);
            }
        }
        LeaveCriticalSection(&g_streamingSessionCs);
    }

    if (g_captureActive) {
        header->dwBytesRecorded = 0;
        const MMRESULT result = waveInAddBuffer(waveIn, header, sizeof(WAVEHDR));
        if (result != MMSYSERR_NOERROR && g_captureActive) {
            // Do not attempt recovery or blocking cleanup from the waveIn
            // callback.  The main window will stop the capture, invalidate the
            // current generation, abort any streaming ASR session, and show a
            // single device error after this callback returns.
            g_audioCaptureFailureCode.store(static_cast<DWORD>(result), std::memory_order_relaxed);
            g_audioCaptureFailureWasapi.store(false, std::memory_order_relaxed);
            g_audioCaptureFailurePending.store(true, std::memory_order_release);
            g_captureActive = false;
            if (g_mainWindow) {
                const uint64_t generation = g_audioCaptureGeneration.load(std::memory_order_acquire);
                PostMessageW(g_mainWindow,
                             kWaveInCaptureErrorMessage,
                             static_cast<WPARAM>(generation),
                             static_cast<LPARAM>(result));
            }
        }
    }
}

bool StartAudioCapture(std::wstring& error) {
    if (g_waveIn || g_wasapiCapture.IsInitialized()) return true;

    const uint64_t generation =
        g_audioCaptureGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
    g_audioCaptureFailureCode.store(0, std::memory_order_relaxed);
    g_audioCaptureFailureWasapi.store(false, std::memory_order_relaxed);
    g_audioCaptureFailurePending.store(false, std::memory_order_release);

    g_audioLevel.store(0.0f);
    g_hudSmoothedLevel = 0.0f;
    EnterCriticalSection(&g_audioLock);
    g_audioData.clear();
    LeaveCriticalSection(&g_audioLock);

    if (g_config.audioBackend == L"wasapi") {
        g_wasapiCapture.SetCaptureGeneration(generation);
        if (g_wasapiCapture.Init(g_config.audioDeviceId)) {
            if (g_wasapiCapture.Start(error)) {
                g_captureActive = true;
                return true;
            }
            printf("[Audio] WASAPI Start failed: %ls\n", error.c_str());
            g_wasapiCapture.Release();
            error.clear();
        } else {
            g_wasapiCapture.Release();
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
    // Invalidate runtime-failure messages before stopping either capture API.
    // WASAPI and waveIn may still have one callback in flight while their
    // handles are being released; the generation guard prevents that callback
    // from terminating a subsequent recording.
    g_audioCaptureGeneration.fetch_add(1, std::memory_order_acq_rel);
    g_captureActive = false;
    if (g_wasapiCapture.IsInitialized()) {
        g_wasapiCapture.Stop();
        g_wasapiCapture.Release();
    } else if (g_waveIn) {
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

bool AsrEngine::EnsureVad(int threads, const Config& config) {
    if (!TryLoadAsrDlls()) return false;
    const std::string key = "vad|" + std::to_string(threads) + "|" + std::to_string(config.vadThreshold) + "|" + std::to_string(config.vadMinSilence) + "|" + std::to_string(config.vadMinSpeech);
    if (vad && vadKey == key) return true;

    const std::wstring vadPath = RuntimeAssetDir() + L"\\models\\silero_vad.int8.onnx";
    if (GetFileAttributesW(vadPath.c_str()) == INVALID_FILE_ATTRIBUTES) return false;

    sherpa_onnx::cxx::VadModelConfig vc;
    vc.silero_vad.model = WideToUtf8(vadPath);
    vc.silero_vad.threshold = config.vadThreshold;
    vc.silero_vad.min_silence_duration = static_cast<float>(config.vadMinSilence) / 1000.0f;
    vc.silero_vad.min_speech_duration = static_cast<float>(config.vadMinSpeech) / 1000.0f;
    vc.silero_vad.max_speech_duration = 20.0f;
    vc.silero_vad.window_size = 512;
    vc.sample_rate = 16000;
    vc.num_threads = threads;

    auto v = sherpa_onnx::cxx::VoiceActivityDetector::Create(vc, 600.0f);
    if (!v.Get()) return false;
    vad = std::make_unique<sherpa_onnx::cxx::VoiceActivityDetector>(std::move(v));
    vadKey = key;
    return true;
}

bool AsrEngine::EnsureFireRedVad(const Config& config) {
    if (!TryLoadAsrDlls()) return false;
    const std::string key = "firered_vad|" + std::to_string(config.vadThreshold) + "|" + std::to_string(config.vadMinSilence) + "|" + std::to_string(config.vadMinSpeech) + "|" + std::to_string(config.vadPadStart) + "|" + std::to_string(config.vadSmoothWindow);
    if (fireRedVad && fireRedVadKey == key) return true;

    firered_vad::FireRedVadConfig cfg;
    cfg.modelPath = WideToUtf8(RuntimeAssetDir() + L"\\models\\fireredvad_stream_vad_with_cache.onnx");
    cfg.threshold = config.vadThreshold;
    cfg.minSilenceMs = config.vadMinSilence;
    cfg.minSpeechMs = config.vadMinSpeech;
    cfg.padStartMs = config.vadPadStart;
    cfg.smoothWindowSize = config.vadSmoothWindow;
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

    const std::wstring punctPath = DownloadedModelRoot() +
        L"\\sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8\\model.int8.onnx";
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

VadResult AsrEngine::ApplyVad(const std::vector<float>& samples, const Config& config, int threads) {
    VadResult result;
    if (config.vadModel == L"firered") {
        if (!EnsureFireRedVad(config)) return result;
        fireRedVad->Reset();
        int nSamples = static_cast<int>(samples.size());
        fireRedVad->Process(samples.data(), nSamples);
        fireRedVad->Flush();
        auto concat = fireRedVad->GetConcatenatedSamples(samples.data(), nSamples);
        if (!concat.empty()) {
            result.hasSpeech = true;
            result.samples = std::move(concat);
        }
    } else {
        if (!EnsureVad(threads, config)) return result;
        vad->Reset();
        const size_t windowSize = 512;
        for (size_t i = 0; i < samples.size(); i += windowSize) {
            size_t end = std::min(i + windowSize, samples.size());
            vad->AcceptWaveform(samples.data() + i, static_cast<int32_t>(end - i));
        }
        vad->Flush();
        while (!vad->IsEmpty()) {
            auto seg = vad->Front();
            result.samples.insert(result.samples.end(),
                                   seg.samples.begin(), seg.samples.end());
            result.hasSpeech = true;
            vad->Pop();
        }
    }
    return result;
}

bool AsrEngine::EnsureVadForConfig(const Config& config, int threads) {
    if (config.vadModel == L"firered") return EnsureFireRedVad(config);
    return EnsureVad(threads, config);
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
        std::lock_guard<std::mutex> g(lock_);
        VadResult vr = ApplyVad(workSamples, config, threads);
        double ms = tVad.ElapsedMs();
        if (config.enableDebugMode) {
            g_vadMs = ms;
            g_vadModelName = (config.vadModel == L"firered") ? L"FireRed" : L"Silero";
        }
        if (!vr.hasSpeech) return L"";
        if (!vr.samples.empty()) workSamples = std::move(vr.samples);
        if (config.enableDebugMode) g_vadTrimmedSamples = workSamples.size();
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
    if (config.enableVad) g_asrEngine.EnsureVadForConfig(config, threads);
    if (config.postprocess == L"itn" || config.postprocess == L"punct" || config.postprocess == L"llm") {
        g_asrEngine.EnsurePunctuation(threads);
    }
    g_asrEngine.Unlock();
}
