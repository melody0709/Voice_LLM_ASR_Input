# Changelog

> 🇨🇳 [中文版](doc/CHANGELOG_zh.md)

## v0.8.2 (2026-05-18)

### Fixed

- **Volcengine nostream/async result loss** (critical): After sending the last audio chunk, drainThread was killed before reading the server's final response. Added `drainFinalDone` atomic flag — main thread now waits for drainThread to complete its final drain (up to 5s) before closing the WebSocket. Final drain uses `ReceiveResult(3000ms)` instead of 1ms polling, giving the server enough time to respond
- **Volcengine nostream/async long recording truncation** (critical): For recordings >15s, the main thread's wait condition checked `asyncPartial.empty()` — once a partial result arrived, it stopped waiting and killed drainThread, losing all text after the 15s mark. Wait condition now checks `drainFinalDone` instead, ensuring the complete result is captured
- **WinHttpCloseHandle deadlock** (critical): Main thread called `WinHttpCloseHandle(hWebSocket)` while drainThread was blocked on `WinHttpWebSocketReceive` on the same handle. WinHTTP is not thread-safe — concurrent access causes internal deadlock. Fixed by joining drainThread before closing the WebSocket handle in both normal and no-speech code paths
- **Volcengine nostream/async logic deadlock**: `asyncDrainDone` was set after waiting for `drainFinalDone`, but drainThread's main loop exits when `asyncDrainDone` becomes true — creating a circular wait. Fixed by setting `asyncDrainDone = true` before the wait loop
- **Volcengine short audio false timeout**: When the server closes the connection after short audio, the main thread waited 5s for `drainFinalDone` but drainThread was stuck in `WinHttpWebSocketReceive` (WinHTTP timeout unreliable). Now drainThread's main loop also checks `g_volcSession.connected`, exiting promptly when the server closes. `forceAbort` is only set to true when the connection is still alive (real timeout), preventing false "VolcEngine timeout" messages
- **HUD kHudHideTimer race condition**: After a previous recording's "No speech detected" HUD started its hide timer, quickly pressing the hotkey for a new recording could have its HUD hidden by the stale timer. Fixed with dual protection: `KillTimer(kHudHideTimer)` at recording start, plus `g_recording` check in the timer handler

## v0.8.0.1 (2026-05-17)

### Fixed

- **Volcengine nostream no-speech hang**: When VAD detects no speech in nostream mode, `WinHttpWebSocketReceive` with 1ms timeout does not actually return (Windows WinHTTP minimum timeout granularity is much larger). The drainThread blocks indefinitely, causing `drainThread.join()` to hang until the 18s watchdog triggers. Fixed by closing the WebSocket handle before joining drainThread, which forces `WinHttpWebSocketReceive` to return immediately with `ERROR_WINHTTP_OPERATION_CANCELLED`

## v0.8.0 (2026-05-17)

### Added

- **Streaming VAD for local ASR**: VAD now runs during recording (in the WASAPI capture thread) instead of after. Speech segments are collected in real-time and passed directly to ASR on release, skipping the redundant VAD step. Supports both Silero VAD and FireRed VAD
- **Streaming VAD for Volcengine ASR**: FireRed VAD runs during Volcengine recording with a 3-state machine (PreSpeech → InSpeech → PossibleTail) for intelligent audio trimming. Pre-speech silence is buffered and trimmed; tail silence is held until speech resumes or recording ends. When no speech is detected, returns "No speech detected" without sending audio to the server
- **FireRed VAD streaming API**: Added `StreamVadPostprocessor` class with `GetConcatenatedSamples()`, `HasSpeech()`, `Flush()`, and `Reset()` methods for real-time VAD processing. `GetConcatenatedSamples()` merges VAD segments with proper overlap handling
- **HUD speech detection visual feedback**: Volume bars only animate when audio level exceeds threshold (0.04). Bar color changes from idle gradient to active gradient only when speech has been detected (`g_hudHasSpoken` flag)
- **"No speech detected" HUD timing**: Shows for 1500ms (vs 200ms for normal results, 2200ms for errors)

### Fixed

- **Volcengine nostream/async long recording hang** (critical): When recording exceeds ~15s, TCP receive buffer fills up because `SendAudio` only sends without reading responses. This causes `WinHttpWebSocketSend` to block indefinitely. Fixed by:
  - Adding `drainThread` for both async and nostream modes (previously only async had it)
  - `SendAudio(isLast=true)` now skips receive in async/nostream modes, letting drainThread handle the final response
  - Send isLast frame *before* joining drainThread (was reversed before — joining first stopped the reader, causing TCP buffer overflow during the join)
  - Removed nostream drain loop that competed with drainThread for the same WebSocket handle
- **Watchdog killing session during recording**: The 18s watchdog timer was started at recording begin but could fire while the user was still recording. Now auto-renews when `g_recording` is true, only triggers force-abort after recording ends
- **Unified `ExtractJsonStr`**: Removed duplicate implementations from `baidu_asr.h` and `volcengine_asr.h`, consolidated into `utils.h` with proper escape handling (counting consecutive backslashes before quote)
- **`PostQuitMessage(0)` polluting outer message loop**: Replaced with `IsWindow(dlg)` check in `settings.cpp` (2 occurrences)
- **`g_streamingVadReady`/`g_volcVadDoTrim` thread safety**: Changed from plain `bool` to `std::atomic<bool>` for cross-thread access
- **`s_lastRecvError` thread safety**: Changed to `std::atomic<DWORD>` in `volcengine_asr.h`
- **`VolcDebugLog` overhead**: Now checks `g_enableDebugMode` before formatting log messages, avoiding unnecessary string operations when debug mode is off
- **`ReceiveResult` connection state tracking**: Now sets `sess->connected = false` on error or zero-byte read, allowing proper connection state detection
- **`AddVolcRecognitionHistory` filtering**: Now also skips "No speech detected" entries from recognition history

### Changed

- **Removed startup "ASR ready" HUD display**: The HUD notification on ASR preload completion conflicted with `kHudHideTimer` when the user pressed the hotkey immediately after startup, causing the HUD to disappear prematurely
- **Local ASR with streaming VAD**: When no speech segments are detected, shows "No speech detected" instead of running ASR on silence
- **Volcengine ASR early exit**: When VAD detects no speech in PreSpeech state, closes session early without sending audio to server, saving API costs

## v0.7.5 (2026-05-15)

### Fixed

- **Volcengine WebSocket hang on exit**: Added `forceAbort` atomic flag to `VolcSession`. When the recording session is cancelled (e.g., Esc key, window close), `forceAbort` is set and all pending WebSocket operations (`OpenSession`, `SendAudio`, `ReceiveResult`, `CloseSession`) exit immediately instead of blocking on network I/O
- **WebSocket close handshake hang**: `WebSocketCloseGracefully` now skips the close handshake when `forceAbort` is set, directly closing the handle to avoid waiting for a server response that may never come
- **Stale connection reuse**: `EnsureConnection` now checks `lastUsedTick`; connections idle for more than 30 seconds are automatically rebuilt instead of reused, preventing "connection reset" errors from a server-side timeout
- **WinHTTP proxy setting**: Changed from `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` to `WINHTTP_ACCESS_TYPE_NO_PROXY` in both `EnsureConnection` and `TestConnection`. The default proxy type causes unnecessary PAC/auto-detect delays on machines without a proxy configured

### Added

- **Connection prewarm**: New `PrewarmConnection()` function establishes TCP/TLS connection in advance before recording starts, eliminating the ~1.4s connection setup latency on first press. Called from the main thread after settings save or on startup when Volcengine backend is selected
- **`lastUsedTick` field in VolcSession**: Tracks when the connection was last used, enabling automatic expiry and rebuild of stale connections

### Changed

- **Simplified OpenSession**: Removed the retry loop (was 2 attempts with connection rebuild). Now single-attempt with `forceAbort` checks at key points (before `SendRequest`, during `ReceiveResult`). If the connection fails, it is rebuilt on the next `EnsureConnection` call
- **Connection reuse strategy**: `CloseSession` now closes `hConnect` after each session (only `hSession` is kept alive). Previously both were kept alive, but the server closes the TCP connection after idle timeout, making the cached `hConnect` stale
- **Reduced WinHTTP timeouts**: Connect/send/receive timeouts reduced from 10000ms to 5000ms (session-level) and 3000ms (request-level) for faster failure detection
- **Reduced logging verbosity**: `SendAudio` only logs the first frame (seq=2) and last frame (isLast), instead of every audio chunk. Server response logging consolidated into a single line with payload preview
- **Nostream drain improvements**: Added empty frame counting and `forceAbort` check in the nostream drain loop, preventing infinite waits when the server is unresponsive

## v0.7.4 (2026-05-11)

### Fixed

- **WeChat Chinese IME paste issue**: WeChat (`Weixin.exe`) uses a custom Qt control where `GetFocus()` returns NULL and IME intercepts Ctrl+V. Detect WeChat by process name and use `WM_CHAR` character-by-character input to bypass IME; other apps use clipboard + Ctrl+V + IMM32 temporary English mode switch
- **IMM32 input method state switching**: Added `ImeStateGuard` RAII struct to temporarily switch IME to English mode before sending Ctrl+V, then auto-restore afterward

### Added

- **Unicode SendInput fallback**: Added `SendUnicodeText()` function using `KEYEVENTF_UNICODE` flag for character-by-character input, completely bypassing IME
- **Force Unicode Input menu**: Tray right-click menu now has "Force Unicode Input" option; when checked, all apps use Unicode SendInput for paste, useful for compatibility testing
- **Two-layer paste injection strategy**: `PasteTextImeAware()` implements smart paste: prefers `WM_PASTE` (when `GetFocus()` succeeds), WeChat uses `WM_CHAR`, other apps use clipboard + Ctrl+V + IMM32 switch

## v0.7.3 (2026-05-11)

### Added

- **WASAPI Shared Mode capture**: Audio input upgraded from MME `waveIn` to WASAPI Shared Mode with custom resampling. Captures at system mix format (typically 48kHz/32bit float/stereo) and resamples to 16kHz/16bit/mono via linear interpolation. Automatic fallback to `waveIn` if WASAPI initialization fails
- **Debug Mode**: Tray right-click checkbox to open a CMD console with real-time per-stage timing output. Covers VAD, ASR decode, punctuation, cloud API, LLM refine, and paste injection. Total excludes recording duration. P0 §3 of the optimization plan
- **Config fields**: `audio_backend` (wasapi/waveIn) and `audio_device_id` (WASAPI device ID, empty=default) persisted in config.json

### Changed

- **`HiResTimer` moved** from `engine.cpp` to `engine.h` for reuse across modules

### Fixed

- **WASAPI lifecycle bug**: `Stop()` only stops the capture thread but does not release resources; added `Release()` for proper cleanup. Without this, the second recording would hang because `IsInitialized()` remained true
- **WASAPI resample phase drift**: Updated phase to `m_resamplePhase -= written / m_resampleRatio` to prevent drift with non-integer sample rate ratios
- **Baidu ASR response parsing**: `err_no` field parsed as integer instead of string comparison; added diagnostic `printf` on empty/error responses; fixed `WinHttpReadData` to only append bytes actually read (not full buffer)
- **Debug timing state leak**: `g_vadModelName` now cleared in `StopRecordingSession()` alongside other timing variables
- **Redundant buffer clear removed**: `WasapiCapture::Start()` no longer duplicates `g_audioData`/`g_audioLevel` clear already done by `StartAudioCapture()`

## v0.7.2 (2026-05-09)

### Fixed

- **Volcengine thread not stopped on exit**: `WM_DESTROY` now sets `g_volcStreaming = false` and joins the volcengine thread, preventing a zombie thread spinning in `Sleep(20)` after the main window closes
- **Baidu ASR token cache data race**: `GetAccessToken` now uses `std::mutex` + `lock_guard` to protect `s_cachedToken` / `s_tokenExpiresAt`, fixing concurrent read/write UB from `Recognize` and `TestConnection` threads
- **`VolcDebugLog` thread safety**: Added `static std::mutex` to serialize log file writes and `logPath` initialization, preventing interleaved lines and init races
- **`g_volcAudioCs` resource leak**: Added `DeleteCriticalSection(&g_volcAudioCs)` to all `wWinMain` exit paths (normal exit, single-instance early return, `RegisterWindowClasses` failure, `CreateWindowExW` failure)
- **SSL certificate verification re-enabled**: Removed `SECURITY_FLAG_IGNORE_*` overrides from `baidu_asr.h` (`Recognize` and `TestConnection`) and `llm_refine.h` (`SendRequestRaw`), restoring proper HTTPS certificate validation for all cloud API connections
- **`AsrEngine::lock` encapsulation**: Changed `std::mutex lock` from public to private (`lock_`), added `Lock()`/`Unlock()` methods; `PreloadAsrEngine` uses the new API instead of direct member access

### Changed

- **Utility functions deduplicated**: `WideToUtf8`, `Utf8ToWide`, `EscapeJson`, `Trim` consolidated into new `src/utils.h`; removed 4 copies from `engine.cpp`, `llm_refine.h`, `baidu_asr.h`, `volcengine_asr.h`
- **Model downloader no longer blocks UI**: `RunModelDownloader` now launches PowerShell asynchronously and posts `WM_APP + 20` back to Settings when done; download button disables during download with status text, re-enables on completion
- **Tray menu flag cleanup**: Removed redundant `MF_DISABLED` alongside `MF_GRAYED` (the latter already implies disabled state)
- **HotkeyEdit paint optimization**: Non-capturing state uses `GetSysColorBrush(COLOR_WINDOW)` instead of creating/destroying a `CreateSolidBrush(RGB(255,255,255))` on every `WM_PAINT`
- **PositionHud region reuse**: Fixed GDI region leak in `PositionHud` by skipping region recreation (`CreateRoundRectRgn` + `SetWindowRgn`) when window dimensions are unchanged

## v0.7.1 (2026-05-09)

### Performance

- **`bigmodel_nostream` speed optimization**: Skip `ReceiveResult` for intermediate audio chunks (server returns empty text for each chunk in non-streaming mode). Only the final `isLast` chunk drains responses. Session time reduced from ~7-8s to ~1.5-2s
- **WinHTTP connection reuse**: `hSession` + `hConnect` (TCP/TLS) are kept alive between recording sessions. Second session onward saves ~1.4s TLS handshake. Connection failure triggers automatic retry with fresh connection

### Fixed

- **`ExtractJsonStr` escape handling**: Correctly handles `\\"` (escaped backslash before end-quote) by counting consecutive backslashes instead of checking only the previous character
- **`ExtractJsonBool` whitespace handling**: Now skips `\n`/`\r` in addition to spaces and tabs, consistent with `ExtractJsonStr`
- **Thread safety**: `VolcSession::connected` changed from `volatile bool` to `std::atomic<bool>`

## v0.7.0 (2026-05-09)

### Added

- **Volcengine ASR full parameter support**: All `request` and `corpus` fields from the official API are now configurable in Settings
  - `end_window_size` / `force_to_speech_time` — VAD segmentation and forced stop timing
  - `enable_ddc` — Semantic smoothing (removes filler words and repetitions)
  - `enable_nonstream` — Two-pass recognition on `bigmodel_async` mode (streaming + offline re-recognition for higher accuracy)
  - `enable_music_fc` / `enable_poi_fc` — Music and POI function call
  - `enable_accelerate_text` + `accelerate_score` — First-token acceleration
  - `language` — Language selection (only effective in `bigmodel_nostream` mode, per API spec)
- **Hotwords & correction tables**: New `Hotwords ID/Name` and `Correct ID/Name` fields in Cloud ASR tab
  - `boosting_table_id` / `boosting_table_name` — Reference hotword tables from the self-learning platform
  - `correct_table_id` / `correct_table_name` — Reference replacement word tables for domain-specific terminology
- **Dialog context**: `Use history as context` checkbox sends recent recognition results as `corpus.context` to improve contextual accuracy
  - Configurable history count (1–20, default 3)
  - Context is serialized as a JSON string per API spec
- **Extra Params dialog**: Dedicated dialog for editing additional `request`-level JSON parameters, with preset templates for `sensitive_words_filter` and `result_type`/`vad_segment_duration`
- **Model Version selector**: New dropdown for `Seed-ASR 2.0 (duration)` / `Seed-ASR 2.0 (concurrent)` / `BigModel 1.0 (duration)` / `BigModel 1.0 (concurrent)`

- **Removed first-start model download dialog**: Cloud-only users are no longer prompted to download models on first launch
- **Download button moved to ASR model row**: Renamed to "Download Local Model", placed next to the ASR model dropdown with 220px width
- **Cloud Provider renamed and reordered**: "Volcengine (Doubao)" → "Volcano Engine (Doubao)", now the default selection; Volcano Engine also moved before Baidu Cloud in the ASR Backend dropdown
- **Cloud mode startup HUD**: When using a cloud ASR backend, the HUD now shows "ASR ready: xxx" on startup

### Fixed

- **`corpus` fields no longer mutually exclusive**: Previously `boosting_table_id` and `context` were sent with `if/else if`, preventing hotwords and dialog context from being used together. Now all `corpus` sub-fields are merged into a single JSON object
- **`context` field format corrected**: The `context` value must be a JSON string (with escaped inner quotes), not a raw JSON object. Sending a raw object caused the server to reject the request and the client to hang after the first recognition
- **`language` parameter now only sent in `bigmodel_nostream` mode**: Per API documentation, the `language` field is only supported in nostream mode; sending it in other modes could cause errors
- **Extra Params `corpus` conflict resolved**: If a user manually included `corpus` in Extra Params, it would conflict with the code-generated `corpus` field, producing invalid JSON. The `corpus` key is now skipped during Extra Params parsing
- **Removed non-standard HTTP headers**: `X-Api-Request-Id` and `X-Api-Sequence: -1` were not in the official API spec and have been removed from both `OpenSession` and `TestConnection`
- **Removed insecure SSL flag overrides**: `SECURITY_FLAG_IGNORE_UNKNOWN_CA` and related flags were unnecessarily bypassing certificate validation; removed for proper HTTPS security

## v0.6.2 (2026-05-06)

### Changed

- **Settings UI style unification**: Introduced `UiStyle` namespace in `globals.h` to centralize all layout constants, replacing scattered magic numbers across `settings.cpp` and `hud.cpp`
  - Row spacing unified to 52px across all tabs (Recognition, LLM, LLM Prompt, Cloud ASR) — previously Cloud ASR had 36px (too tight), LLM had 46-64px (uneven)
  - `RowInputY(row)` / `RowLabelY(row)` helper functions for automatic Y coordinate calculation
  - All control sizes (heights, widths) defined as named constants (`EditH`, `BtnH`, `ComboW`, etc.)
  - All color values (`BgColor`, `TextColor`, `DividerColor`, etc.) defined as named constants
  - All margin/position values (`Margin`, `ContentLeft`, `InputLeft`, etc.) defined as named constants

### Fixed

- **`WM_PAINT` footerTop minimum value inconsistency**: `LayoutSettingsWindow` used `460` but `WM_PAINT` used `390`, now both use `UiStyle::FooterMinTop` (460)
- **`footerHeight` duplicated hardcode**: Was `78` in two places, now uses `UiStyle::FooterHeight`

## v0.6.1 (2026-05-06)

### Changed

- **ASR model preload on startup**: When `asrBackend` is `local` and model directory exists, models (ASR + VAD + punctuation) are preloaded in a background thread at startup, eliminating first-press latency
- **DELAYLOAD for onnxruntime/sherpa-onnx/kaldi DLLs**: `onnxruntime.dll`, `sherpa-onnx-cxx-api.dll`, and `kaldi-native-fbank-core.dll` are now delay-loaded — they are only loaded into memory when local ASR is actually used. Cloud-only mode stays at ~12 MB idle memory (down from ~20 MB)
- **Preload after Settings Save**: When switching to local ASR backend or changing models in Settings, the new model is preloaded in the background after Save
- **HUD notification on preload complete**: Shows "ASR ready: \<model\>" when preload finishes

### Fixed

- **Volcengine ASR now supports LLM refine**: Previously, Volcengine results always skipped LLM correction even when `postprocess` was set to `Auto punctuate + LLM`. Now all three ASR backends (local, Baidu, Volcengine) consistently support LLM refine
- **`SaveConfig` now persists `llm_endpoint`, `llm_api_key`, `llm_model`** fields (previously omitted)
- Removed unused `g_volcFinalText` global variable

## v0.6.0 (2026-05-06)

### Changed

- **Source code refactored from single-file to multi-module architecture**: `src/main.cpp` (3269 lines) split into 5 compilation units with clear separation of concerns
  - `src/globals.h` — Shared constants, control IDs, struct definitions, extern global variable declarations
  - `src/engine.h` / `src/engine.cpp` (~750 lines) — Backend: Config persistence, ASR engine, audio capture, utility functions
  - `src/hud.h` / `src/hud.cpp` (~380 lines) — HUD window, Direct2D rendering, tray icon, UI resource management
  - `src/hotkey.h` / `src/hotkey.cpp` (~330 lines) — Hotkey logic, CapsLock long-press, keyboard hook, HotkeyEdit custom control
  - `src/settings.h` / `src/settings.cpp` (~1190 lines) — Settings window, controls, load/save, provider management, input dialog
  - `src/main.cpp` (~560 lines) — Entry point (WinMain), main window procedure, recording session orchestration, LLM refine
- Global variables defined in `main.cpp`, other modules access via `extern` declarations in `globals.h`
- `build.bat` updated: `cl` command now compiles 5 source files
- `CMakeLists.txt` updated: `add_executable` includes new `.cpp` files, added `winhttp` and `crypt32` link dependencies
- `.clangd` updated: Added UTF-8 charset flags for sherpa-onnx header compatibility

### Fixed

- `SaveConfig` now correctly persists `llm_endpoint`, `llm_api_key`, `llm_model` fields (previously omitted)
- Removed unused `g_volcFinalText` global variable

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
