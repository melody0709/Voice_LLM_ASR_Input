# Changelog

> 🇨🇳 [中文版](doc/CHANGELOG_zh.md)

## v0.9.25 (2026-08-24, dev)

### Added

- **Shared diagnostic audio capture**: Added provider-neutral capture attempts spanning Local, Baidu, all Qwen transports, Volcengine, MiMo, Doubao IME, Qwen IME Free, internal provider retries, and configured fallback. Saved groups contain canonical capture/provider-input WAVs, SHA-256 deduplication, signal/device/VAD metrics, and exact stage terminals without transcript, context, secrets, or raw provider JSON.
- **Bounded Settings controls**: General now exposes `Off`, `Failures only`, and `All recordings`, plus immediate folder-open and confirmed managed-delete actions. Installed/Portable data stays local under `diagnostics\audio`; retention is capped at 20 groups, 100 MiB, and 7 days while unknown files are preserved.
- **Generic ASR replay tool**: Added `tools\asr_audio_replay.bat` and the developer-only `asr_audio_replay` CMake target for canonical WAV validation, PCM hash/metrics, Local replay, configured primary/fallback comparison, and explicitly selected production batch/streaming cloud sessions. Cloud upload and transcript console output remain opt-in.

### Changed

- **Diagnostic I/O lifecycle**: WAV/JSON hashing, atomic writes, retention, and Settings deletion are serialized outside WASAPI/waveIn callbacks and UI hot paths; application shutdown waits for pending diagnostic writers.
- **Attempt-safe diagnostics**: Streaming sessions now receive the immutable per-recording config/attempt ID, fallback-provider retries use a collision-free fallback stage namespace, and replay resolves assets/config from the canonical runtime payload.
- **LLM provider refresh**: Kept the official DeepSeek preset on `deepseek-v4-flash` with thinking disabled for low-latency correction, replaced OpenRouter's retired `qwen/qwen3-4b` preset with `qwen/qwen3.5-9b`, and kept SiliconFlow on the current documented `Qwen/Qwen3.6-35B-A3B` model while switching to the top-level `enable_thinking=false` parameter.
- **Conservative preset migration**: Exact retired DeepSeek/OpenRouter aliases and obsolete SiliconFlow parameter shapes are migrated only when the configured endpoint is still the corresponding official preset endpoint; custom proxy endpoints and unrelated custom model choices are left unchanged.
- **Bounded LLM networking**: API Base URLs and full `/chat/completions` URLs now share canonical path construction, receive timeout is 15 seconds while resolve/connect/send remain capped at 5 seconds, and response bodies are bounded to 1 MiB.

### Fixed

- **Tray icon recovery**: Re-register the notification-area icon after Explorer broadcasts `TaskbarCreated`, so VoxType remains accessible when Explorer or the Windows shell restarts.
- **Settings diagnostics layout**: Widened both Diagnostics action buttons and slightly increased the Settings height while reserving a complete footer area, preventing English labels and the bottom Save/Close buttons from being clipped.
- **Capture-start evidence**: When WASAPI/waveIn cannot produce the first PCM sample, `Failures only` now writes a JSON-only manifest with attempted/terminal capture backend, exact startup phase and error code, plus available device/format metadata; no empty WAV is fabricated.
- **Diagnostic privacy and shutdown integrity**: Enabling recording diagnostics now activates only the structured runtime lifecycle log, not Qwen/Volcengine verbose files or Volcengine DNS/TCP probes. Diagnostic writers remain joinable, are drained completely on exit, and completed writer handles are reaped between recordings.
- **Managed-file boundary**: Retention and confirmed deletion now recognize only exact capture/input artifact names. Unknown same-prefix WAV files and WAV-like `.tmp` files are preserved, while stale module-owned temp files are still removed.
- **Replay path fidelity**: The BAT wrapper no longer uses delayed expansion, so quoted WAV/runtime paths containing `!` reach the replay executable unchanged.
- **Malformed provider-store preservation**: Saving settings no longer replaces an invalid multi-provider JSON store with `{}`; the original bytes and unrelated provider entries are retained while legacy active-provider fields continue to save normally.
- **Connection-test parity**: `Test Connection` now sends the current Extra Params through the same request builder as real correction and rejects malformed merged JSON locally before network I/O.
- **Provider credential/config isolation**: Extra Params are persisted per provider, visible edits are captured before a provider switch, and a provider without stored settings no longer inherits another provider's API key, endpoint, model, or Extra Params. The dropdown now restores the configured provider, while startup migration still preserves legacy single-provider fields.
- **Provider store parsing**: Provider save/load/list/delete now uses JSON-aware top-level member parsing, so braces or provider-like names inside stored string values cannot corrupt or cross-load another provider entry.
- **OpenAI-compatible response handling**: Responses are read specifically from `choices[0].message.content`, with JSON escape/Unicode decoding, optional text-content array support, and explicit failure for empty or malformed HTTP 200 bodies.

### Tests

- Added `audio_diagnostics_test` coverage for canonical WAV headers, PCM signal metrics, SHA-256, save policy, JSON-only zero-PCM capture failures, stage-input deduplication, VAD metadata merging, JSON privacy, strict managed-temp/deletion boundaries, and 20-group retention.
- Added `llm_refine_test` coverage for provider presets and migration boundaries, malformed-store preservation, request/Extra Params validation, endpoint normalization, response parsing, Unicode escapes, and bounded timeout policy. `build.bat --test` now runs it with the existing offline protocol suites.

## v0.9.24 (2026-08-10, dev)

### Added

- **Qwen Audio 3 profiles**: Added model-profile routing for the legacy DashScope realtime model, `qwen-audio-3.0-asr-flash` HTTP batch recognition, and `qwen-audio-3.0-asr-flash-streaming` binary-PCM realtime recognition. New installs default to Audio 3 streaming while existing Qwen configuration is preserved.
- **Qwen Audio settings**: Added a model dropdown, Beijing Workspace HTTP/streaming endpoints, language hints, vocabulary ID, Audio 3 immediate-vocabulary JSON, semantic punctuation, sentence silence, multi-threshold, heartbeat, and speech-noise-threshold controls.
- **Qwen Audio 3 optimization**: Focused-field context now uses the documented 400-character limit with UTF-16 boundary protection; Audio 3 Streaming adds opt-in one-shot `continue-task` context refresh, special-word filtering, effective-language Settings feedback, and a bounded 60-second idle WebSocket reuse manager. Qwen3 Realtime remains Manual and non-reusable.

### Fixed

- **Qwen finalization integrity**: Qwen Audio 3 streaming now recovers text after an incomplete finalize only for peer-close/timeout with at least one `sentence_end=true` result; explicit `task-failed` events and pending partials remain failures. Qwen3 Realtime waits for `session.finished` and only degrades to an already completed transcript on peer-close/timeout, so a later provider error cannot be skipped.
- **Qwen document compliance**: HTTP batch responses are read through the documented JSON paths, `sample_rate` uses the documented string type, immediate-vocabulary terms enforce the documented length/segment limits, and automatic-language realtime sessions omit the optional `input_audio_transcription` object.
- **Qwen Audio streaming initialization**: Empty optional vocabulary settings are now omitted from `run-task` instead of producing an incomplete JSON value. The request shape also follows the documented `parameters` then `input` layout, allowing the task to start and emit partial results.
- **Qwen Audio profile migration**: Existing config files without the new model selector stay on the legacy realtime model; only installations without a config file default to Audio 3 streaming.
- **Qwen Audio recovery boundaries**: Server-side `task-failed` events no longer trigger a blind PCM replay; replay-budget exhaustion keeps the live session until key release, and pending-audio overflow is reported explicitly instead of being silently dropped.
- **Qwen Audio no-speech normalization**: The HTTP model's `HTTP 400 ASR_RESPONSE_HAVE_NO_WORDS` silence response now becomes the shared `No speech detected` result instead of an operational-error HUD or fallback trigger.
- **Runtime capture failure handling**: WASAPI and `waveIn` device/driver failures that occur after recording starts now stop the current attempt on the UI thread, invalidate stale provider results, abort the active cloud session, cancel its watchdog, and show a microphone error without sending truncated PCM to fallback.

## v0.9.23 (2026-08-06, dev)

### Changed

- **Qwen native authentication boundary**: Removed embedded provider-specific signing material and the known-invalid generic-HMAC fallback. ASR/LLM now fail before network I/O when native authentication cannot be produced, allowing normal fallback orchestration to take over.
- **Reverse archive boundary**: Shareable, redacted `reverse/` notes and tools are now tracked through the archive's own ignore rules, while sensitive evidence and the external `reverse-skill/` library remain ignored.

### Fixed

- **Selection clipboard preservation**: Selection capture and rewrite replacement retain and restore the complete OLE clipboard data object, preserving images, files, HTML, RTF, and private formats.
- **Recording startup latency**: Audio capture starts before UI Automation/`WM_COPY` selection probing, preventing a slow target control from clipping the beginning of speech.
- **Qwen DLL compatibility**: Version-dependent `unet.dll` RVAs are called only for the verified SHA-256 fingerprint. Settings tests always validate the currently entered Shell Path and report when another module is already active.
- **Settings responsiveness**: Qwen device-identity status probing now runs off the UI thread with stale-result suppression.
- **Qwen config/thread safety**: UTDID overrides are DPAPI-encrypted with plaintext migration, runtime logging uses an atomic enable snapshot, and the redundant `qwen_free_enabled` field was removed.
- **Native output lifetime**: Native signer buffers are released when confirmed to belong to the process heap; unknown allocator ownership is left untouched to avoid heap corruption.
- **Qwen replay cancellation**: Internal receiver shutdown no longer leaves the external sticky-cancel flag set, so transport recovery can reconnect and replay PCM.
- **Qwen final/timeout integrity**: Empty finals cannot be hidden by an older partial, replay accepts only a non-empty clean final (or an explicitly clean empty result), and receive polling restores the configured WinHTTP timeout before every return path.
- **Protocol parser hardening**: Invalid JSON escapes/numbers are rejected instead of exposing partial fields; duplicate root control flags take precedence over array-nested diagnostics.
- **Conservative Qwen post-processing default**: New or incomplete Qwen IME Free configs leave bundled LLM polishing disabled until the user explicitly enables `Polish (auto)`.

## v0.9.22 (2026-08-05, dev)

### Changed

- **Reverse-engineering archive privacy**: Redacted real UTDID values, signature samples, candidate keys, and encrypted device samples from shareable documentation and offline regression fixtures; original local evidence remains outside the published index.
- **Probe status documentation**: Recorded the original Shell FFI observations and clearly separated incomplete local-loopback probes from confirmed protocol conclusions.

### Fixed

- **Qwen Free transport recovery**: Provider setup/connect now runs off the hotkey path; transient connect, receive, close, PCM-send, stop, and final-timeout failures buffer until key release and replay the complete bounded PCM recording through one fresh WebSocket session.
- **Qwen Free terminal-result integrity**: A stale partial or WebSocket close can no longer masquerade as a successful final; replay is accepted only after all PCM is sent and a clean final frame is received. Empty finals from recordings of at least three seconds receive one bounded replay attempt.
- **Qwen Free watchdog and HUD recovery**: Finalization time now reserves the original final wait, reconnect/replay wait, and bundled LLM wait; recovery exposes `Buffering`, `Reconnecting`, and `Retrying` HUD states, while watchdog/rewrite failures are classified as operational errors instead of pasted text.
- **Qwen Free WinHTTP lifecycle**: Fixed the receiver-thread join race and stopped the 50 ms receive-poll timeout from also becoming the WebSocket send timeout.
- **Hotkey dispatch reliability**: Right Alt, CapsLock long press, and other configured recording hotkeys now post start/stop commands to the main window; repeated keydown events no longer flood duplicate start commands.
- **Selection-rewrite fallback safety**: Selection-rewrite intent is now fixed from the selection captured at recording start. Automatic DashScope fallback is suppressed for that attempt, and a target window that later disappears is reported as `Rewrite skipped`/rewrite failure instead of degrading to ordinary paste.
- **Recovery-policy regressions**: Added offline coverage for Qwen watchdog/rewrite error classification, retryable transport failures, timestamp-safe HTTP status matching, and clean replay-final requirements.

## v0.9.21 (2026-08-05, dev)

### Fixed

- **Qwen selection-rewrite terminal response parsing**: Rewrite responses that contain both `processing`/loading and `complete` messages now use only terminal content for selection replacement; escaped response envelopes are also handled, and processing-only responses are rejected safely.

## v0.9.20 (2026-08-05, dev)

### Changed

- **Qwen Free bundled post-processing settings**: `Polish (auto)` is now the single editable switch for the original `VoiceInputWrite` response. Punctuation and correction are shown as read-only included capabilities instead of misleading independent toggles.
- **Legacy configuration compatibility**: Existing `qwen_free_punct` and `qwen_free_correct` values are normalized to the bundled post-processing switch during load/save, so older configurations keep their effective behavior.

### Added

- **Bundled post-processing regression coverage**: Offline tests now verify disabled, punctuation-only, and correction-only legacy configurations are normalized correctly.

## v0.9.19 (2026-08-05, dev)

### Fixed

- **Qwen connection-test error classification**: Settings now distinguishes an ASR handshake failure from a bundled LLM post-processing failure instead of labeling both as an ASR failure.

## v0.9.18 (2026-08-05, dev)

### Fixed

- **Qwen Free pending-audio overflow**: A stalled ASR connection no longer silently drops microphone audio after the bounded pending-PCM queue fills; the overflow now enters the existing transport-failure/replay path.
- **Qwen connection-test error classification**: Settings now distinguishes an ASR handshake failure from a bundled LLM post-processing failure instead of labeling both as an ASR failure.
- **Portable/MSI documentation link**: The runtime payload now includes `doc/README_zh.md`, matching the Chinese README link shipped at the payload root.

## v0.9.17 (2026-08-05, dev)

### Fixed

- **Qwen ASR binary framing hardening**: Shared the production/test encoder for the original two length-prefixed WebSocket segments and added regression coverage for PCM commits, empty-PCM control frames, big-endian lengths, and invalid buffers.

## v0.9.16 (2026-08-05, dev)

### Fixed

- **Qwen error-body privacy**: ASR and LLM diagnostics now retain only response byte counts plus safe structured error fields; raw remote bodies are no longer copied into HUD text or persistent debug logs.

## v0.9.15 (2026-08-05, dev)

### Fixed

- **Qwen ASR query completeness**: Restored the required `version=2` field in both the signed query content and the final WebSocket URL, preserving the original `ve → version → sign` order and preventing avoidable authentication/upgrade rejection.

## v0.9.14 (2026-08-05, dev)

### Fixed

- **Qwen debug-log privacy**: ASR debug logs now record response metadata instead of raw JSON, preventing recognized speech from being copied into local diagnostics; error responses without a message are reduced to a generic error plus code/size metadata.

### Added

- **Offline Qwen protocol regression target**: `build.bat --test` now builds and runs JSON/HMAC tests under `build/artifacts/tests` without changing the canonical runtime payload.

## v0.9.13 (2026-08-05, dev)

### Fixed

- **Qwen error-diagnostic privacy**: WebSocket failure messages and debug logs no longer include the signed URL, WSG signature, or encrypted UTDID; diagnostics retain only non-sensitive lengths and transport details.
- **Qwen finalization watchdog**: Reserved time for both the ASR final response and bundled LLM post-processing, preventing a slow healthy polish/rewrite request from being aborted as a streaming timeout.

## v0.9.12 (2026-08-05, dev)

### Fixed

- **Qwen protocol string conversion safety**: Fixed UTF-8 conversion helpers that allocated one byte/character less than the Win32 conversion API was told to write, preventing an out-of-bounds NUL write in Qwen ASR/LLM diagnostics and UTDID handling.
- **Qwen connection-test result semantics**: The Settings probe now checks ASR and, when post-processing is enabled, Qwen LLM as well; an LLM failure is no longer reported as an overall successful connection test.
- **Qwen LLM response validation**: A HTTP 200 response without output text is now treated as an invalid post-processing result and safely falls back to the raw ASR text.
- **Qwen selection rewrite first closed loop**: Added UI Automation/`WM_COPY` selection capture, `VoiceInputRewrite` instruction routing, safe selection replacement, and WeChat `WM_CHAR` compatibility. If the target window, process, focus, or selection changes while recognition is running, replacement is skipped instead of touching another control.
- **Selection rewrite fallback safety**: When a Qwen Free rewrite attempt has a live selection, ordinary ASR fallback is suppressed so a spoken rewrite instruction cannot be pasted as plain text into the selected content.
- **Streaming fallback routing**: Fallback now chooses the backend's native streaming or batch session. `Qwen IME (Free)` can be used as a real fallback target instead of silently entering the local-ASR default branch, and Qwen IME transport errors are classified consistently for fallback decisions.
- **Qwen response parsing hardening**: ASR and LLM control-frame parsing now shares a nesting-aware JSON string/boolean reader with escape and Unicode handling, avoiding false field matches inside quoted transcript content.
- **Qwen UTDID display privacy**: Settings now masks the device fingerprint and shows only a short summary; the complete value remains internal for protocol authentication.
- **Architecture documentation sync**: Documented the shipped Qwen IME Free A1/A2 streaming path, bundled post-processing semantics, selection-rewrite safety boundary, fallback support, and persisted settings.

## v0.9.11 (2026-08-04, dev)

### Changed

- **Qianwen IME free backend switched to A1 pure protocol replay**: The `qwen_free` provider no longer calls `QianwenShellEmbedded.dll` ABI directly (Path B); instead it implements the full ASR WebSocket + LLM HTTP protocol from scratch using the reverse-engineered WSG signing format and an embedded protocol secret, and reuses UTDID from the locally installed Qianwen IME's `UTDID.dll` cache or registry. VoxType now performs WASAPI capture locally and feeds PCM chunks to its own protocol layer, just like the `qwen`, `volcengine`, and `doubao_ime` streaming backends.
- **Path B ABI loader removed**: Deleted `qwen_free_asr.h/.cpp` (the `QwenFreeAbi` / `QwenFreeAbiLoader` Path B infrastructure). Removed the `qwen_free_use_path_b` config field and the "Path B: ABI direct" checkbox from Settings. The Settings "Test Connection" button now probes UTDID acquisition instead of ABI loading.

### Added

- **Protocol layer modules**: `qwen_free_proto_sign.{h,cpp}` (HMAC-SHA1 + WSG sign), `qwen_free_proto_utdid.{h,cpp}` (UTDID acquisition via DLL/registry/override), `qwen_free_proto_asr.{h,cpp}` (WebSocket ASR), `qwen_free_proto_llm.{h,cpp}` (HTTP LLM polish/punctuate/correct). All four are pure C++ with no Qianwen DLL dependency except `UTDID.dll` for device fingerprint reuse.

### Fixed

- **`qwen_free` recording pipeline**: Stop skipping local WASAPI capture for `qwen_free`. The previous Path B path let Qianwen IME capture audio internally, which caused microphone conflicts and bypassed VoxType's `too_short` / `no_speech` guards. With A1, VoxType captures audio itself and applies the same VAD trim + watchdog flow used by other streaming backends.
- **Fallback to dashscope qwen on UTDID failure**: If UTDID acquisition fails (no Qianwen IME installed) and the user has configured `qwenApiKey`, `qwen_free` now auto-falls back to the dashscope `qwen` streaming backend instead of hard-failing.
- **WebSocket upgrade diagnostics**: `qwen_free_proto_asr` now queries the HTTP status code before `WinHttpWebSocketCompleteUpgrade` and, on a non-101 response, reads up to 1 KB of the error body and reports `WebSocket upgrade failed (HTTP <code>): <body>` instead of the unhelpful generic `WinHttpWebSocketCompleteUpgrade failed: 4317` (`ERROR_WINHTTP_OPERATION_CANCELLED`). This surfaces server-side rejection reasons such as bad WSG signature, invalid UTDID, or missing `kps_wg`.
- **WSG signature format corrected**: Based on Ghidra reverse-engineering of `unet.dll` (`UNetCrypt::SignWithNumber` call chain) and a redacted signature sample captured from the Qianwen IME process, the signing content now uses URL-appearance order (not alphabetical), the URL field name is `sign` (not `sign_wg`), and the signature is placed at the URL end (after `version`). The embedded secret is used as the HMAC-SHA1 key, not appended to the content.

## v0.9.10 (2026-08-04, dev)

### Added

- **Qianwen IME free backend (reverse engineering)**: Added a new cloud ASR provider `qwen_free` that reuses the locally installed Qianwen IME's voice backend via its public C ABI (`QianwenShellEmbedded.dll`). No API key required; works headless and reuses Qianwen IME's WSG auth, ASR WebSocket endpoint, and LLM polish/punctuate/correct pipeline. See [reverse/](reverse/) for the reverse-engineering report and integration blueprint.
- **Reverse-engineering workspace**: `reverse/` directory with `README.md`, `INVENTORY.md`, `PLAN.md`, and `work/` containing `FINDINGS.md`, `PROTOCOL.md`, `INTEGRATION.md` plus all dumpbin/strings/memory-scan artifacts.
- **In-progress integration**: `qwen_free_asr.h/.cpp` ABI loader, `qwen_free_streaming_session.h/.cpp` IStreamingAsrSession implementation, and `qwen_free_llm.h/.cpp` LLM post-processing scaffold integrated behind the `qwen_free` provider switch.

## v0.9.9 (2026-07-28)

### Fixed

- **HUD recording-level animation for batch ASR**: The Local (sherpa-onnx), Baidu, MiMo, and other batch backends now arm the shared HUD animation after recording becomes active, so the input-level bar lights up and moves consistently with streaming backends.
- **Preserved cloud recording startup reliability**: This restores the HUD animation without reverting the capture-first startup order introduced in v0.9.3 to protect the first audio segment sent to cloud streaming backends.

### Verification

- `build.bat --package` passes; the Portable and MSI artifacts are verified against the canonical runtime manifest.

## v0.9.8 (2026-07-27)

### Added

- **Canonical CMake/Ninja release pipeline**: `build.bat` now configures and builds the x64 Release preset, installs the exact runtime payload to `build\run\x64-release`, validates its manifest, and can produce verified Portable and MSI artifacts under `build\packages`.
- **MSI installer with folder selection and upgrades**: The x64 per-machine installer defaults to `Program Files\VoxType`, offers an Advanced folder picker, records a user-selected directory in HKLM, and restores it before Major Upgrade removes the prior MSI. Permanent UpgradeCode/ProductCode/component identity inputs and a documented upgrade contract are now committed.
- **Portable release format**: Portable `.7z` packages include `portable.flag`, retain configuration/models/logs beside the executable, and are re-extracted and hash-checked before publication.
- **Start with Windows setting**: The General tab can create, update, or remove the current user's quoted `HKCU\...\Run\VoxType` entry. The registry is the source of truth, and a moved Portable copy is corrected on Save.

### Changed

- **Installed-data boundary**: Normal installed builds now keep `config.json`, downloaded models, punctuation models, and logs in `%LOCALAPPDATA%\VoxType`; only immutable runtime assets remain in the install directory. Existing beside-EXE `config.json` is copied once when appropriate.
- **Release safety**: Public MSI versioning uses only `APP_VERSION_MAJOR.MINOR.PATCH`; same-version package names are never overwritten, and existing verified artifacts are reused only when their complete input digest matches.

### Verification

- `build.bat`, `build.bat --package-portable`, and `build.bat --package-msi` pass. Portable extraction and MSI administrative extraction both match the canonical runtime manifest.

## v0.9.7 (2026-07-12)

### Added

- **Structured ASR runtime audit**: Debug Mode now writes `%TEMP%\voxtype_asr_runtime.log` events for attempt start, recording stop, primary final classification, fallback start/final, stale discard, and too-short/no-speech skips. Events contain backend, result kind, normalized failure reason, source, elapsed time, and PCM byte counts, but never recognized text or raw provider errors.

### Fixed

- **Fallback after an early streaming failure**: If a streaming primary exhausts startup/connect retries and reports its final failure while the hotkey is still held, the final is now deferred until release. VoxType then stores the complete raw PCM and runs the configured fallback normally instead of ending the attempt without fallback audio.
- **Streaming session start-failure routing**: Synchronous Qwen, Volcengine, and Doubao IME `Start()` failures now enter the same classified completion path; fallback is allowed only when enough PCM has already been captured.
- **Volcengine debug-log privacy**: Removed request/response hex, init JSON, transcript payloads, final text, raw provider errors, and proxy-address values from persistent Volcengine diagnostics. The existing invalid `%ls` formatter used with a narrow mode string was also corrected.
- **Debug flag data race**: The cross-thread debug logging gate is now atomic.

### Changed

- **Bounded diagnostics**: `%TEMP%\voxtype_asr_runtime.log` and `%TEMP%\volc_asr_debug.log` now include full local date/time with milliseconds and PID, rotate at 5 MiB, and retain two archives (`.1` and `.2`). Both files are written only while Debug Mode is enabled.
- **Retry policy unchanged**: The primary retry/replay budgets, fallback trigger classification, and fallback-enabled 6–12 second post-release streaming wait remain unchanged; v0.9.7 improves correctness and observability around those decisions.

### Verification

- `.\build.bat`, `.\tools\doubao_ime_probe.bat`, runtime version/resource inspection, sensitive-log-pattern scans, and `git diff --check` pass.

## v0.9.6 (2026-06-29)

### Added

- **Doubao IME fallback target**: `Doubao IME (Free)` can now be selected in the Recognition-tab `Fallback` selector. It replays the same raw PCM through a recorded Doubao IME request after the primary backend ends in an operational failure.
- **Doubao IME recorded helper**: Added `doubao_ime_asr::RecognizeRecordedPcm()` with fixed 20ms frame padding, final/partial merge through `RealtimeClient::Finish()`, bounded recorded retry, and credential refresh side effects.

### Changed

- **Doubao IME credential writeback**: Recorded fallback registration/token refresh now returns credential side effects through `AsrSessionResult`; the main window applies them via the existing `kDoubaoImeCredentialsMessage` handler after stale-attempt checks pass.
- **Fallback backend support**: `doubao_ime` is now included in fallback validation, Settings backend options, batch-session creation, and cloud timing metrics. Volcengine remains unavailable as a fallback target.

### Verification

- `.\build.bat` passes after enabling Doubao IME fallback.
- `.\tools\doubao_ime_probe.bat` passes through the recorded helper path against the bundled 16kHz mono speech WAV (`wav_ok=1`, text: `开放时间，早上 9 点至下午 5 点。`).
- Automated Settings smoke opened the real Settings window and verified the fallback combo contains `Doubao IME (Free)`.

## v0.9.5 (2026-06-29)

### Added

- **Fallback ASR backend**: Added a Recognition-tab `Fallback` selector. When the primary ASR backend ends in an operational failure, VoxType retries the same raw PCM with the configured fallback backend before dispatching the final ASR result.
- **Batch fallback path**: Local, Baidu, Qwen batch, and MiMo batch recognition now support serial fallback. The first fallback release supports fallback targets `Local`, `Baidu Cloud`, `Qwen ASR`, and `MiMo ASR`.
- **Streaming fallback path**: Qwen, Volcengine, and Doubao IME streaming final errors and watchdog timeouts now route through a main-window attempt completion handler, which can launch batch fallback with the stored raw PCM.

### Changed

- **Fallback trigger rules**: Fallback only runs for operational failures such as timeout, network/transport errors, auth/config errors, provider errors, and local model-load errors. `Too short`, `No speech detected`, stale attempts, and duplicate finals do not trigger fallback.
- **Streaming final wait with fallback**: When fallback is enabled, post-release streaming final wait uses a shorter 6-12 second budget instead of the legacy 8-30 second adaptive wait. Volcengine's opening-finalize guard is reduced from 10 seconds to 8 seconds in fallback-enabled sessions.
- **ASR/LLM result metadata**: Final ASR and LLM messages now carry the result config, fallback state, primary backend, primary error, and attempt id so Debug Mode, LLM refinement, and stale-result guards do not rely on the current global config after settings changes.
- **Local fallback preload**: Startup and ASR reload now preload the local model when either the primary backend or fallback backend is Local.
- **Qwen batch VAD trim**: Qwen batch recognition now uses the shared batch VAD trimmer, matching Baidu and MiMo behavior.

### Verification

- `.\build.bat` passes after implementing fallback ASR.
- Temporary strategy test passed for fallback result classification, fallback eligibility, and the new streaming final wait curve.
- Automated tray-app smoke confirmed the real process creates the main window and Settings contains both primary ASR backend and fallback backend combo controls.

## v0.9.4 (2026-06-28)

### Added

- **Doubao IME experimental ASR backend**: Added `doubao_ime` as a non-default streaming cloud ASR provider shown as `Doubao IME (Free)`. It uses the unofficial Doubao input-method endpoint, not the Volcengine official speech protocol.
- **Doubao IME client/session**: Added device registration, `asr_config.app_key` token bootstrap, CNG MD5 `x-ss-stub`, WinHTTP WebSocket, handwritten protobuf messages, Opus 20ms frame encoding, partial HUD updates, final dispatch, replay retry, cancellable bootstrap/startup handles, transient startup retry, and auth/token credential reset retry.
- **Doubao IME Settings page**: Added provider selection, credential status, `Test Connection`, and `Reset Credentials`. Device id/cdid/token are persisted as `doubao_ime_*`, with token encrypted by DPAPI.
- **Doubao IME diagnostic probe**: Added `tools/doubao_ime_probe.bat`, a standalone live probe that compiles a small console tool for protocol, optional WAV recognition, and optional streaming send/drain verification. It reuses saved Doubao IME credentials by default and supports `--streaming` plus `--fresh`.
- **Vendored Opus**: Added static `libopus` 1.6.1 headers/library under `third_party/opus` with license files and linked `bcrypt.lib` for CNG hashing.

### Changed

- **Streaming backend detection**: Replaced Qwen/Volcengine-only checks with a shared streaming-cloud backend helper covering Qwen, Volcengine, and Doubao IME for Stop/watchdog/debug behavior. Local streaming VAD remains enabled only for Qwen and Volcengine.
- **Doubao IME Test Connection**: The Settings probe now sends a 20ms Opus `Last` frame, finishes the session, waits for server completion, and propagates credential updates instead of only testing the initial WebSocket handshake.
- **Doubao IME partial fallback**: The tray-app drain thread now retains the latest non-final candidate until a confirmed final result arrives, matching the reference behavior and avoiding empty-final retry when the service finishes after partial text only.
- **Doubao IME long-recording segment aggregation**: `result_json.results[*].text` is now concatenated in result order instead of keeping only the last segment, fixing long recordings where the service splits recognition into multiple segments and only the final segment was pasted.
- **Doubao IME cloud-VAD segment accumulation**: Doubao IME now accumulates final text across multiple WebSocket events and shows partial HUD updates as full-session previews. A cloud-side VAD final during recording no longer satisfies the post-stop final wait; after release, the session waits for a post-`FinishSession` final or `SessionFinished`, fixing long recordings where only the last cloud segment reached the input box.
- **Doubao IME partial-window reset handling**: Long recordings now track a committed prefix plus the current service partial window. Only a clear length drop is treated as the IME service clearing/restarting its partial window; normal service-side revisions replace the active window instead of being committed, avoiding duplicated growing partials in the final text.
- **Doubao IME clear-page partial HUD**: Doubao IME partial updates now stay in live full-text mode while they fit within three body lines. After they exceed that limit, the HUD clears previous display text and restarts from the current last sentence, then lets that new page accumulate normally until it exceeds three body lines again. If one sentence alone is too long, the display trims from the front until it fits. After the first clear in a recording, the HUD keeps fixed four-line height for the rest of that recording and keeps width capped to `min(900 DIP, 75% screen width)`. This is display-only and does not change the full final text that gets pasted.
- **Streaming partial HUD generalization**: Qwen, Volcengine, and Doubao IME now share the constrained clear-page partial HUD path, so all streaming cloud providers use the same width cap, page clearing, and fixed-height behavior during long partial updates.
- **Streaming cloud pre-capture VAD**: Qwen and Volcengine now initialize streaming VAD before replaying pre-captured head audio, so the replayed head audio goes through the same VAD trim state as live callback audio.
- **Pending PCM cap**: Added a bounded `PendingPcmBuffer` defaulting to 120 seconds of 16kHz mono PCM to prevent unbounded memory growth while a streaming cloud backend is stalled or reconnecting. Qwen and Doubao IME surface overflow as a retryable transport failure.
- **Doubao IME Settings race guard**: `Test Connection` results now carry a generation id, so stale background test results cannot overwrite credentials after `Reset Credentials` or a newer test.
- **Streaming cloud failure status**: Mid-recording transport loss now shows `Buffering...` instead of `Reconnecting...` because the current strategy buffers audio and replays after release rather than opening a replacement WebSocket during the same hold.
- **Doubao IME local VAD bypass**: Doubao IME now ignores local `Enable VAD` and uploads raw PCM encoded as Opus. Qwen and Volcengine keep the streaming VAD trim path.

### Verification

- `.\build.bat` passes after the Doubao IME integration.
- `.\tools\doubao_ime_probe.bat` passes against the live Doubao IME endpoint using saved credentials (`config_credentials=1`, `protocol_ok=1`, `changed=0`) and successfully recognizes the bundled 16kHz mono speech WAV (`wav_ok=1`, text: `开放时间，早上 9 点至下午 5 点。`).
- `.\tools\doubao_ime_probe.bat --streaming` passes the live send/drain path (`streaming_ok=1`, `partial_count=6`, `final_count=1`, `session_finished=1`, text: `开放时间，早上 9 点至下午 5 点。`).
- Re-ran both checks after the partial-fallback fix: the live probe and `.\build.bat` still pass.
- Re-ran `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` after the Settings generation guard, pre-capture VAD replay, and pending-buffer cap; all pass, with only existing CRLF warnings from `git diff --check`.
- Re-ran `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` after the long-recording segment aggregation fix; all pass, with only existing CRLF warnings from `git diff --check`.
- Re-ran `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` after the cross-event Doubao IME cloud-VAD segment accumulation fix; all pass, with only existing CRLF warnings from `git diff --check`.
- Re-ran `.\build.bat` and `.\tools\doubao_ime_probe.bat --streaming` after tightening the Doubao IME partial-window reset heuristic; both WAV and streaming probe outputs return the expected sample text without duplication.
- Re-ran `.\build.bat` after the Doubao IME clear-page HUD tuning.
- Re-ran `.\build.bat` and `git diff --check` after generalizing the clear-page partial HUD path to Qwen, Volcengine, and Doubao IME; build passes, with only existing CRLF warnings from `git diff --check`.
- Automated tray-app Settings smoke opened the real Settings window, selected `Cloud ASR` -> `Doubao IME (Free)`, verified the credential/test/reset controls are visible at the current DPI, and the Settings `Test Connection` returns OK.
- Manual tray-app smoke testing is still pending for hotkey/microphone recording, partial HUD, final paste, too-short handling, network interruption/watchdog recovery, and additional DPI passes.

## v0.9.3 (2026-06-15)

### Fixed

- **Volcengine rapid recording head audio loss (regression)**: The cloud ASR architecture refactor changed `StartRecordingSession` to start WASAPI audio capture *after* waiting for the previous session's worker thread to join. If the previous worker was stuck in a 3-second `OpenSession` hard timeout, the UI thread blocked for 3 seconds with WASAPI not yet running, causing the user's first few words to be lost. Fixed by moving `StartAudioCapture()` before `AbortAndResetActiveStreamingSession()` and adding `ReplayPreCapturedAudio()` to flush the buffered head audio into the new session.
- **Streaming VAD double-processing**: The WASAPI callback called `StreamingVadTrimmer::ProcessPcm16()` unconditionally before checking `g_activeStreamingSession`. When the session was null (during the join-wait window), the VAD trimmer state machine was advanced but no audio was enqueued. A subsequent `ReplayPreCapturedAudio` with a fresh trimmer would process the same audio again, causing inconsistent trim results. Fixed by moving `ProcessPcm16()` inside the `g_activeStreamingSession` check.
- **Volcengine 3-second connection expiry destroying TCP+TLS pool**: `EnsureConnection` expired the `hSession` handle after only 3 seconds of inactivity, forcing a full DNS + TCP + TLS handshake for every recording after a short pause. Changed to 300 seconds (5 minutes) so keep-alive connections are reused across rapid recordings.
- **Volcengine NO_PROXY bypassing system proxy**: `WinHttpOpen` used `WINHTTP_ACCESS_TYPE_NO_PROXY`, which bypassed system/VPN proxy settings and could fail for users behind a proxy. Changed to `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` (no-op when no proxy is configured).
- **Volcengine 2-second WinHTTP timeouts too aggressive**: `WinHttpSetTimeouts` was `2000, 2000, 2000, 2000` for both session and request handles. Changed to `3000, 3000, 5000, 5000` to give DNS/TLS more time. The hard timeout watchdog (`kHardTimeoutMs = 3000`) is unchanged to cap UI blocking at 3 seconds.

### Changed

- **Volcengine activeReq fast cancel**: Added `std::atomic<HINTERNET> activeReq` to `VolcSession` that tracks the active `hReq` during `OpenSessionImpl`. `Abort()` now immediately closes `activeReq` via `exchange(nullptr)`, causing `WinHttpSendRequest` to return instantly with `ERROR_WINHTTP_OPERATION_CANCELLED`. This eliminates the 3-second UI freeze that occurred when aborting during a connection attempt. All 7 exit paths in `OpenSessionImpl` use `activeReq.exchange(nullptr)` to prevent double-close.
- **Volcengine g_volcSession moved to static**: `g_volcSession` and `g_volcKeepAlive` moved from global scope into `volcengine_streaming_session.cpp` as `static s_volcSession`. Exposed via `VolcengineResetForNewSession()`, `VolcenginePrewarmConnection()`, `VolcengineClosePersistentConnection()`, and `VolcengineForceAbortAndCloseAll()`. `globals.h` no longer includes `volcengine_asr.h`.
- **Qwen/Volcengine Stop logic deduplicated**: `StopRecordingSession` had two nearly identical branches for Qwen and Volcengine. Merged into a single `(qwen || volcengine)` branch with `AsrBackendDisplayName()` for HUD text.
- **Qwen/Volcengine startup order unified**: Both streaming backends now follow the same order: `session->Start()` → `ReplayPreCapturedAudio()` → `g_activeStreamingSession = session` → `StartStreamingVadTrimmerForCloud()`.

## v0.9.2 (2026-06-13)

### Fixed

- **HUD DPI-aware rendering**: HUD constants renamed to `*Dip` suffix with `float` type for clear DIP semantics. `PositionHud()` now uses `GetDpiForMonitor()` instead of `DpiScaleForWindow()` to correctly size on the target monitor. Bottom margin converted from hardcoded `48px` to `DipToPx(kHudBottomMarginDip, scale)`. `CreateWindowExW` initial size uses monitor DPI. `WM_DPICHANGED` handler added to re-layout on DPI change. D2D render target DPI synced via `SetDpi()` on creation and before each draw.
- **Volcengine WebSocket double-close race**: Added `AtomicTakeWebSocket()` using `InterlockedExchangePointer` to atomically take ownership of `g_volcSession.hWebSocket`. All close sites (Abort, VAD no-speech, async drain) now use this function, ensuring only one thread closes the handle.
- **Volcengine `asyncPartial` data race**: Added `std::mutex asyncMutex` to protect `asyncPartial` reads/writes across worker and drainThread. Comparison and assignment are now in the same `lock_guard` scope.
- **Qwen `activeClient_` UAF and handle double-close**: Replaced `std::atomic<RealtimeClient*>` with mutex-protected `SetActiveClient/ClearActiveClient/AbortActiveClient` helpers, ensuring the UI thread's `Abort()` call never races with worker's client destruction. `QwenConnection::hWebSocket` changed to `std::atomic<HINTERNET>` with `TakeWebSocket()` for single-owner close. `connected` changed to `std::atomic<bool>`.
- **Retry path abort check**: Both Qwen and Volcengine retry conditions now check `abort_.load()` before entering potentially long-blocking retry attempts. Qwen `RetryRecognitionOnce` also checks `abort_` before `client.Finish()`.
- **`mimo_asr.cpp/.h` missing from git**: Added previously untracked files to version control.

## v0.9.1 (2026-06-10)

### Changed

- **MiMo ASR backend**: Added Xiaomi MiMo ASR (`mimo-v2.5-asr`) as a batch cloud backend. PCM is wrapped as WAV and sent to the OpenAI-compatible `/chat/completions` endpoint, with default Token Plan Base URL `https://token-plan-ams.xiaomimimo.com/v1`.
- **Cloud ASR architecture stabilized**: Moved Qwen and Volcengine streaming orchestration behind `IStreamingAsrSession`, with shared status/partial/final dispatch helpers.
- **Shared VAD trim pipeline**: Added common streaming/batch VAD trim core so Qwen, Volcengine, Baidu, and MiMo can reuse the same head/tail silence trimming behavior while preserving middle pauses.
- **Volcengine refactor parity review**: Verified the Volcengine protocol layer is unchanged and the refactored outer flow preserves three modes, final drain, empty-final retry, watchdog timeout text, and prewarm/reuse lifecycle.
- **Baidu retry hardening**: Added same-PCM retry for transient failures and token refresh retry for auth/token errors.
- **Source tree organization**: Grouped source files under `src/app`, `src/asr`, `src/audio`, `src/ui`, and `src/core`, and refreshed architecture/review documentation.

## v0.9.0 (2026-06-09)

### Added

- **Qwen ASR backend**: Added DashScope Qwen ASR realtime WebSocket support with default model `qwen3-asr-flash-realtime`
- **True Qwen streaming send path**: Audio capture callbacks append 16k/16-bit/mono PCM into a non-blocking Qwen pending queue while recording; the Qwen worker sends configured chunks during the hotkey hold instead of waiting until recording ends
- **Qwen partial HUD**: Dedicated receive drain thread parses `conversation.item.input_audio_transcription.text` (`text + stash`) and updates the HUD when partial results are enabled
- **Qwen stability layer**: Added active-client abort, recording/finalize watchdog, connect hard timeout, replay PCM buffer, empty-final retry, timeout/failure retry, and connection-loss buffering until stop
- **Shared ASR architecture modules**: Added `IAsrSession`, `BatchAsrSessionBase`, `AsrResultDispatcher`, ASR result normalization/error classification, and common cloud replay/finalize-timeout helpers
- **Qwen Settings page**: Added Qwen API Key, Base URL, Model, Language, Chunk ms, and Test Connection controls

### Changed

- **Qwen turn detection fixed to Manual**: Product configuration now always uses Manual (`turn_detection: null`) for push-to-talk input. Server VAD controls and persisted `qwen_turn_detection` / `qwen_vad_*` fields were removed because Server VAD segments one hotkey hold into multiple items
- **Cloud ASR configuration expanded**: ASR Backend and Cloud Provider selectors now include `Qwen ASR (DashScope)`
- **Unified ASR final dispatch**: Local, Baidu, Qwen fallback, and cloud paths now share final result normalization, no-speech handling, LLM gating, and raw ASR tracking where applicable
- **Build files updated**: `build.bat` and `CMakeLists.txt` now compile and link the new ASR architecture and Qwen modules (`winhttp` + `crypt32`)
- **Documentation refreshed**: README, Chinese README, changelogs, and cloud ASR architecture notes now describe Qwen ASR, Manual turn detection, and the shared ASR layering

## v0.8.7 (2026-06-06)

### Added

- **Volcengine retry recognition on empty result**: When async/nostream mode returns empty final text and replay PCM is available, automatically opens a new session and re-sends the full PCM for a second recognition attempt. Retry OpenSession allows up to 3 total attempts with `RebuildConnection` + progressive delays (500ms, 1000ms)
- **Connection-loss audio buffering**: When WebSocket disconnects during recording (`bufferUntilStop`), continues buffering incoming audio from the capture thread until recording stops. Buffered audio is appended to `replayPcm` for the retry attempt, preventing audio data loss
- **Adaptive finalize timeout**: `ComputeVolcFinalizeTimeoutMs()` calculates timeout based on recording duration and PCM size (`audioMs * 0.8 + 6000ms`, clamped to 8–30s), replacing the hardcoded 18s. Watchdog timer is reset with this value when recording stops
- **`IsOperationalAsrError()` unified error classification**: Consolidates scattered `rfind(L"ASR failed:", 0)` / `rfind(L"VolcEngine error", 0)` checks into a single function, also matching `VolcEngine timeout`, `VolcEngine connect failed`, and `[VolcEngine error:` prefixes. Used consistently for history filtering, LLM gate, and HUD error display
- **`CloseVolcSessionHandles()` helper**: Safely closes WebSocket, hConnect, hSession in order and resets `connected` flag. Used by retry path and connection cleanup
- **Volcengine failure diagnostics logs**: Added explicit logs for adaptive finalize timeout, retry trigger/skip reasons, replay buffer limit, connection-loss buffering, retry success/failure, server close without text, and skipped paste for operational errors

### Changed

- **Initial connection retry logic refactored**: Replaced `goto openSessionOk` with a clean while loop. Added `lastError` early-exit (no point retrying if server rejected credentials). Non-streaming mode limits to 3 retries; streaming mode allows more. Progressive delays: 500/1000/2000/3000ms
- **`lastError` cleared on new recording session**: `g_volcSession.lastError.clear()` added to `StartRecordingSession()` so stale errors from previous sessions don't prevent retries
- **Watchdog timeout dynamically adjusted on recording stop**: `StopRecordingSession()` now kills and resets the watchdog timer with the adaptive finalize timeout instead of keeping the 18s recording-phase value
- **HUD reconnect message simplified**: Changed from "Reconnecting... (1/3)" to "Reconnecting... Volcano Engine" for cleaner display
- **No-text close handling clarified**: If the server closes normally without text, retry now reports `No speech detected` instead of `ASR failed: VolcEngine timeout`. Short audio (≤3s) with close-without-text skips automatic retry to avoid duplicate cloud calls
- **Post-close drain noise removed**: drainThread no longer calls `DrainReceiveBuffer()` after `ReceiveResult` has observed a close frame and marked the session disconnected, avoiding misleading WinHTTP 4317 logs

## v0.8.6 (2026-05-22)

### Added

- **Volcengine connection reuse**: Keep `hSession+hConnect` alive across recording sessions instead of closing them each time. When the interval between recordings is ≤3s, the existing connection is reused, reducing OpenSession latency from ~1.8s to ~0.4s. When the interval exceeds 3s, the connection is rebuilt with a fresh TCP+TLS handshake
- **`RebuildConnection()` helper**: Closes both `hConnect` and `hSession` (clearing the WinHTTP connection pool) and recreates them from scratch. Used by internal retry and external reconnect logic
- **`PrewarmConnection()`**: Pre-establishes `hSession+hConnect` at startup so the first recording doesn't pay the full connection setup cost
- **`ClosePersistentConnection()`**: Explicitly closes `hSession+hConnect` (used by backend switching and program exit)
- **Time-gated internal retry in `OpenSessionImpl`**: When a WebSocket upgrade step fails quickly (<2s elapsed), automatically rebuilds the connection and retries once. Timeout-type failures (≥2s) skip internal retry and return immediately, leaving time for the external reconnect loop
- **Progressive external reconnect**: Reconnect delays changed from fixed 1500ms×2 to progressive 500/1000/2000ms×3, with `RebuildConnection()` called before each retry to ensure a clean connection

### Changed

- **EnsureConnection expiry threshold**: Reduced from 60s to 3s based on empirical testing — the Volcengine server closes idle TCP connections after ~3s. Both `hConnect` and `hSession` are closed on expiry to clear the WinHTTP connection pool (closing only `hConnect` leaves dead TCP connections in the pool, causing subsequent requests to time out)
- **`CloseSession` conditional keep-alive**: When `g_volcKeepAlive` is true, `hSession+hConnect` are kept alive and `lastUsedTick` is updated; otherwise both are closed
- **`main.cpp` conditional handle cleanup**: The no-speech path and async/nostream completion path now conditionally keep `hSession+hConnect` based on `g_volcKeepAlive`, matching `CloseSession` behavior
- **All `OpenSessionImpl` failure points close `hSession` too**: Previously only `hConnect` was closed on failure, leaving stale TCP connections in the WinHTTP connection pool. Now all 7 failure points (including init frame error) close both `hConnect` and `hSession`
- **`hReq` timeouts reduced**: `WinHttpSetTimeouts(hReq, ...)` changed from 3000ms to 2000ms, so dead connections fail faster and leave more time for the external reconnect loop

## v0.8.5 (2026-05-21)

### Added

- **Input field context reading**: New feature to read the current input field text as ASR context, improving recognition accuracy. Uses a layered fallback approach: WM_GETTEXT (Edit controls) → UIA Value → TextPattern (RangeFromPoint / VisibleRanges) → TextPattern2 (GetCaretRange) → Parent element walk → ElementFromPoint → MSAA (IAccessible). Protected by 200ms timeout and password field detection
- **Debug context output**: Console output shows which layer succeeded, elapsed time, window class, UIA control type, and the actual context text. Works for both input field context and history context
- **`DebugPrintInputContext()` helper**: Extracted shared debug output code into a helper function, used by both ASR and LLM result branches

### Changed

- **Context logic refactor**: Input field text and history are no longer sent simultaneously — input field text takes priority; history is used as fallback only when input field text is unavailable. Window title is no longer sent as context (it provides minimal ASR benefit and wastes tokens)
- **Independent context switches**: "Read input field context" and "Use history as context" are now independent switches, not nested
- **`GetForegroundWindow()` captured on UI thread**: The foreground window handle is now captured on the UI thread and passed to the UIA worker thread, avoiding race conditions with window focus changes
- **`TryTextPatternVisibleRanges` memory optimization**: Changed `GetText(-1)` to `GetText(500)` with early exit at 400 chars, preventing excessive memory usage with large viewports (e.g., Word at 25% zoom)
- **`inline` instead of `static` for header-only globals**: `s_triggeredWindows` and `s_uiaThreadRunning` changed from `static` to `inline` (C++17) to avoid multiple-definition issues if included from multiple translation units
- **Settings UI reorganize**: Removed "enable_accelerate" and "accelerate_score" controls (minimal practical value). Moved "Extra Params" to Row 6. Row 9 now has "Context" label with both checkboxes on one line, "Read input field context" aligned with Name input boxes

### Removed

- **Accelerate score feature**: Removed `enable_accelerate_text` and `accelerate_score` from Volcengine request, config, and UI. These parameters had minimal practical benefit

## v0.8.4 (2026-05-19)

### Added

- **kHudUpdateMessage for thread-safe HUD updates**: Added a custom window message (`kHudUpdateMessage`) so that HUD text can be updated safely from worker threads by posting a message to the main UI thread instead of calling `ShowHud` directly, avoiding thread safety issues with Direct2D rendering
- **Reconnection HUD progress display**: During Volcengine reconnection attempts, the HUD now shows progress (e.g., "Reconnecting... (1/3)", "Reconnecting... (2/3)", "Reconnecting... (3/3)") instead of the generic "ASR failed: reconnecting..." message
- **Connection failure HUD logging**: Failed Volcengine connection attempts now display the actual error message from the server in the HUD, helping users diagnose API key or network issues

### Fixed

- **Volcengine reconnect not properly cleaning up WebSocket handle**: Before retrying connection, the old WebSocket handle was not closed, causing handle leaks across retries. Added `WinHttpCloseHandle` before each reconnect attempt
- **Dynamically allocated HUD text memory leak**: `ShowHud` was being called from worker threads with dynamically allocated strings that were never freed. Now uses `PostMessageW` with `std::wstring` allocated on the heap, freed by the UI thread in the message handler
- **HUD update race from worker threads**: Direct `ShowHud` calls from the Volcengine worker thread could race with the UI thread, causing flickering or stale text. All HUD updates from worker threads now go through `kHudUpdateMessage`

### Changed

- **Debug mode `g_enableDebugMode` synced on config reload**: `g_enableDebugMode` is now set immediately when config is loaded (in `wWinMain`, `kReloadMessage`, and tray menu toggle), ensuring debug mode state is consistent without requiring a restart
- **Removed startup HUD "ASR ready: xxx" display**: The HUD notification on startup for cloud backends was removed because it could conflict with `kHudHideTimer` if the user pressed the hotkey immediately after startup, causing the HUD to disappear prematurely

## v0.8.3 (2026-05-18)

### Fixed

- **Volcengine no-speech path drainThread.join() blocking 17+ seconds** (critical): When VAD detected no speech, `drainThread.join()` was called before `WinHttpCloseHandle(hWebSocket)`. Since `WinHttpWebSocketReceive` timeout is unreliable (200ms can block 17+ seconds), drainThread couldn't exit until the 18s watchdog force-closed the handle. Fixed by closing the WebSocket handle **before** joining drainThread — closing the handle forces `WinHttpWebSocketReceive` to return immediately with `ERROR_WINHTTP_OPERATION_CANCELLED`
- **Volcengine normal path same join-before-close issue**: The async/nostream normal completion path also called `drainThread.join()` before `WinHttpCloseHandle`. Same fix applied — close handle first, then join
- **Watchdog thread handle leak**: After force-closing handles, the watchdog did not `g_volcThread.join()`, leaving the thread handle in a joinable state. Next `StartRecordingSession` would call `g_volcThread = std::thread(...)` on a joinable thread, causing `std::terminate` crash. Fixed by adding `g_volcThread.join()` after closing handles
- **SendMessage(WM_PASTE) could block UI thread indefinitely**: `PasteTextImeAware` used synchronous `SendMessage(focus, WM_PASTE)` — if the target window hung, the UI thread would block forever. Changed to `SendMessageTimeoutW` with `SMTO_ABORTIFHUNG` and 2-second timeout
- **HUD hide timer not set when PasteTextImeAware blocked**: `SetTimer(kHudHideTimer)` was called after `PasteTextImeAware`. If paste blocked, the timer was never set and HUD stayed visible. Moved `SetTimer(kHudHideTimer)` before `PasteTextImeAware`

### Changed

- **drainThread final drain uses loop with 1000ms timeout**: Replaced single `ReceiveResult(3000ms)` with a loop of `ReceiveResult(1000ms)` calls (up to 5 seconds total). This handles nostream mode's multi-packet responses more reliably — each audio chunk gets one response, and the final result may arrive in a later packet
- **Added PasteTextImeAware debug logging**: Logs start/done with elapsed time to help diagnose paste-related hangs

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
  - `enable_accelerate_text` + `accelerate_score` — First-token acceleration (removed in v0.8.5)
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
