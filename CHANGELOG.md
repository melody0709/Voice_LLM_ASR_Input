# Changelog

> 🇨🇳 [中文版](doc/CHANGELOG_zh.md)

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
