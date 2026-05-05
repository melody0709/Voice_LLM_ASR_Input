# Changelog

> 🇨🇳 [中文版](doc/CHANGELOG_zh.md)

## v0.5.0 (2026-05-05)

### Added

- **Cloud ASR UI Overhaul**: Merged "Baidu ASR" and "Volcengine ASR" tabs into a single "Cloud ASR" tab with Provider dropdown
  - Provider ComboBox switches between "百度智能云" and "火山引擎（豆包）" with dynamic control visibility
  - Section title label updates dynamically based on selected provider
- **ASR Backend Selector moved to Recognition tab**: Now at the top of Recognition tab as a global setting
- **Shortcut settings merged into Recognition tab**: Shortcut tab removed, hotkey config now at bottom of Recognition tab with separator line
- **Volcengine ASR Mode reorder**: "File Recognition (nostream)" now listed first (recommended default)
- **Volcengine Model Version cleanup**: Removed BigASR 1.0 options, only Seed-ASR 2.0 (duration/concurrent) remain
- **Cloud ASR Report**: Added `.trae/documents/cloud_asr_report.md` with protocol details, debugging guide, and pitfall records

### Changed

- Settings tabs reduced from 6 to 4: `Recognition` / `LLM` / `LLM Prompt` / `Cloud ASR`
- Default Volcengine mode changed to `bigmodel_nostream` (File Recognition)

### Fixed

- **Volcengine nostream empty result**: `SendAudio(isLast=true)` return value was discarded; now saved to `lastPartial` as fallback
- **ReceiveResult timeout not applied**: `timeoutMs` parameter was ignored, always used 2000ms; now dynamically set via `WinHttpSetOption`
- **Volcengine async mode timeout**: `bigmodel_async` mode caused 8-second timeout because `ReceiveResult` blocked audio sending; fixed with send/receive thread separation
  - Sending thread: only sends audio packets, never blocks on receive
  - Drain thread: continuously drains WebSocket receive buffer, updates HUD with partial results
- **Baidu App ID removed**: Confirmed unused by Baidu REST API, removed from BaiduConfig, UI, and config.json

### Removed

- Removed `IDC_BAIDU_APP_ID` control and `baiduAppId` config field
- Removed "Shortcut" tab (merged into Recognition)
- Removed BigASR 1.0 model version options

## v0.4.0 (2026-05-04)

### Added

- **Volcengine (豆包) Streaming ASR Integration**: Added Volcengine BigModel streaming ASR via WebSocket binary protocol
  - New `src/volcengine_asr.h` header-only module for real-time streaming ASR with WinHTTP WebSocket
  - Streaming mode: audio chunks sent during recording, partial results displayed in real-time HUD
  - Multi-vendor Cloud ASR UI: "Local (sherpa-onnx)" / "Baidu Cloud" / "Volcano Engine" backend selector
  - Settings now shows dynamic provider-specific fields (API Key, Resource ID, Language) based on Cloud Provider selection
  - WebSocket binary protocol: custom 4-byte frame header + payload (supporting full client request, audio-only, and server response frames)
  - X-Api-Key authentication (single key, no OAuth2 needed for Volcengine)
  - Test Connection button for quick WebSocket upgrade verification
  - API Key DPAPI encrypted in config file

### Changed

- `src/main.cpp`: Extended Config with `cloudProvider`, `volcApiKey`, `volcResourceId`, `volcLanguage`
- Cloud ASR tab restructured: ASR Backend (3 options) + Cloud Provider (2 options) with dynamic UI visibility
- `RecognizeAsync()` now routes to local/Baidu/Volcengine backends
- `StartRecordingSession()`: Volcengine path spawns WebSocket connect + streaming thread
- `StopRecordingSession()`: Volcengine path gracefully closes WS and returns final accumulated text
- `WaveInProc`: Added volcengine audio buffer push for real-time streaming
- Version bumped to v0.4.0

## v0.3.0 (2026-05-04)

### Added

- **Baidu Cloud ASR Integration**: Added optional cloud ASR backend via Baidu Intelligent Cloud short speech recognition API
  - New `src/baidu_asr.h` header-only module for Baidu OAuth2.0 authentication and REST API calls
  - Huge free quota: 200K~2M calls for standard edition, 50K for express edition
  - RAW mode upload: Audio sent as raw PCM binary, no base64 encoding overhead
  - Token auto-caching with 30-day expiry management (in-memory only)
  - Settings → New "Cloud ASR" tab with:
    - ASR Backend selector: "Local (sherpa-onnx)" / "Baidu Cloud"
    - App ID, API Key, Secret Key fields (Secret Key DPAPI encrypted)
    - Language model dropdown: Mandarin / English / Cantonese / Sichuanese
    - Test Connection button
    - Privacy notice: "Cloud ASR sends audio to Baidu servers"
  - Baidu Cloud ASR returns text with built-in punctuation (no local punct model needed)
  - LLM correction works with both local and cloud ASR backends
  - HUD displays "Baidu Cloud" during recording/recognizing when selected

### Changed

- `src/main.cpp`: Extended Config struct with `asrBackend`, `baiduAppId`, `baiduApiKey`, `baiduSecretKey`, `baiduDevPid`
- `RecognizeAsync()` now routes to Baidu ASR or local sherpa-onnx based on `asrBackend` setting
- Settings window now has 5 tabs (added "Cloud ASR")

## v0.2.2 (2026-05-02)

### Added

- **GitHub Release Preparation**: Directory structure reorganized, source code moved to `src/` directory
  - Runtime DLLs moved to `dll/` directory and committed to git (~20MB)
  - Added `third_party/sherpa-onnx/` headers and import libraries
  - Added `download_models.ps1` model download script
- **Model Download Optimization**: Integrated aria2c multi-connection download (4 parallel connections)
  - Interactive menu for model selection and download
  - Command-line arguments support `-Models 1,3` or `-Models all`
  - Displays download progress and speed
- **New User Guidance**: First-run detection of model directory with download prompt
  - Checks if any ASR model directory exists
  - Settings Recognition tab adds "Download" button
- **Built-in VAD Models**: Silero VAD and FireRed VAD committed to git (~2.5MB)

### Changed

- `.gitignore` updated: Allow `models/silero_vad.int8.onnx` and `models/fireredvad_stream_vad_with_cache.onnx` to be committed
- `build.bat` updated: Copy DLLs from `dll/` directory, get headers from `third_party/sherpa-onnx/`
- `CMakeLists.txt` updated: Source file paths and include directories
- `.clangd` updated: Added `-Isrc` and `-Ithird_party/sherpa-onnx/include`

## v0.2.1 (2026-04-30)

### Added

- **Multi-Provider Preset System**: Settings LLM tab adds Provider dropdown with built-in DeepSeek / OpenRouter / SiliconFlow presets
  - Selecting a preset auto-fills API Base URL, Model, Extra Params (thinking mode disabled parameters)
  - Each provider independently saves API Key (DPAPI encrypted), auto-restores on switch
  - Support adding/deleting custom providers ([+] / [−] buttons)
- **LLM Prompt Independent Tab**: System Prompt split from LLM tab to dedicated "LLM Prompt" tab
  - System Prompt multi-line editor height increased to 340px
  - Basic Fix / Deep Fix preset buttons remain at top of Prompt tab
- **Extra Params Field**: LLM tab adds Extra Params single-line editor
  - Users can input JSON fragments to merge into LLM API request body
  - Preset providers auto-inject thinking mode disabled parameters
  - Hint text below explains usage and example format
- **Unified Thinking Mode Disabled**: `BuildRequestBody` dynamically merges `extraParams`, no longer hardcoded
  - DeepSeek: `"thinking":{"type":"disabled"}`
  - OpenRouter: `"reasoning":{"effort":"none"}`
  - SiliconFlow/Qwen3.6: `"chat_template_kwargs":{"enable_thinking":false}`

### Changed

- Settings tabs expanded from 2 to 4: `Recognition` / `Shortcut` / `LLM` / `LLM Prompt`
- `config.json` structure upgraded: Added `llm_provider`, `llm_providers_json`, removed old `llm_endpoint`/`llm_api_key`/`llm_model`
- Backward compatible: First launch auto-migrates old config to "Custom" provider

## v0.2.0 (2026-04-30)

### Added

- **FireRedVAD Integration**: Settings adds VAD model dropdown, switchable between Silero VAD and FireRed VAD
  - Added `src/firered_vad.h` header-only module: Uses `kaldi_native_fbank` for 80-dimensional fbank feature extraction + `onnxruntime` for DFSMN streaming model loading
  - FireRedVAD accuracy significantly better than Silero VAD (F1 97.57 vs 95.95, false alarm rate 2.69% vs 9.41%), model only 2.2MB
  - Added `third_party/kaldi_native_fbank/` and `third_party/onnxruntime/` dependencies
  - `build.bat` and `CMakeLists.txt` sync updated linking configuration
  - Runtime adds DLL: `kaldi-native-fbank-core.dll`

### Fixed

- Fixed FireRedVAD unable to detect speech: Audio must be in int16 range (-32768~32767), not normalized float (-1.0~1.0), fbank feature extraction requires multiplication by 32768

## v0.1.4 (2026-04-30)

### Changed

- **Replaced Python ASR worker with direct C++ sherpa-onnx calls**
  - Removed `asr_worker.py` process and TCP JSON line communication
  - Removed Winsock dependency (`ws2_32.lib`)
  - Added `AsrEngine` class, directly calling `sherpa-onnx-cxx-api`'s `OfflineRecognizer`, `VoiceActivityDetector`, `OfflinePunctuation`
  - Recognition flow changed to: Recording PCM → C++ direct model call → Return text, no intermediate process or network overhead
  - Reload changed to clear model cache, auto-reload on next recognition
- `build.bat` adds sherpa-onnx include/lib paths, auto-copies DLLs to build directory
- `CMakeLists.txt` sync updated linking configuration
- Runtime only needs 3 DLLs: `sherpa-onnx-cxx-api.dll`, `sherpa-onnx-c-api.dll`, `onnxruntime.dll`
- No longer requires Python environment and `runtime/` directory Python interpreter

### Removed

- Removed Python worker related code: `StartWorkerProcess`, `StopWorkerProcess`, `SendWorkerJson`, `PingWorker`, etc.
- Removed `WriteWavFile` (no longer need to write temporary WAV files)
- Removed `FindPythonExe`, `QuoteArg` and other helper functions

## v0.1.3 (2026-04-29)

### Changed

- Version number updated to `v0.1.3`
- HUD upgraded from GDI fixed rendering to Direct2D/DirectWrite rendering
- HUD size changed to DPI-aware DIP calculation, dynamically adjusted based on actual text width
- 5 recording volume bars changed to be driven by real-time PCM RMS, visual size increased
- Build linking sync added `d2d1.lib` / `dwrite.lib`

### Fixed

- Fixed HUD text clipping issue on high DPI
- Fixed HUD text vertical centering instability issue
- Fixed layered window rounded corners potentially showing black edges

## v0.1.2 (2026-04-29)

### Changed

- Tray menu version number updated to `v0.1.2`
- Default `CapsLock` hotkey changed to 300ms long-press to trigger voice input
- Short press `CapsLock` returns to system for normal Caps Lock toggle

### Fixed

- After long-press `CapsLock` voice input ends, restore Caps Lock state before pressing to avoid accidental toggle
- When injecting short-press `CapsLock`, allow the injected event to pass through to avoid being intercepted again by global keyboard hook

## v0.1.1 (2026-04-29)

### Changed

- Thread limit increased from 4 to 8, auto strategy changed to `min(8, cpu_count)`
- Settings thread options expanded from 1/2/3/4/auto to 1..8/auto, auto item shows actual thread count

### Fixed

- Settings window opens centered on screen, no longer appears in top-left corner

## v0.1.0 (2026-04-28)

- Initial release
