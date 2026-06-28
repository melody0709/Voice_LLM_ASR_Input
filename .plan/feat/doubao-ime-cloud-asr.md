# Doubao IME Free Cloud ASR

## Summary

- Add a new experimental cloud ASR provider: `doubao_ime`, displayed as `Doubao IME (Free)`.
- Implement it as a streaming provider only. It is non-default and explicitly documented as unofficial and unstable.
- Reimplement the protocol in C++ based on `yyyzl/push-2-talk@725aab0561f040f6e3203e73256b08a7a3d9ae43`, not by reusing `volcengine_asr.h`.
- Use the Doubao IME endpoint `frontier-audio-ime-ws.doubao.com`; this is distinct from the official Volcengine `openspeech.bytedance.com` ASR protocol.

## Current Progress Snapshot 2026-06-28

Done:

- Provider implementation, streaming session integration, credential persistence, Settings controls, build wiring, vendored Opus, and diagnostic tooling are in the working tree.
- Retry/cancellation hardening from the review pass has been implemented: cancellable bootstrap/startup handles, transient startup retry, auth/token reset retry, pending PCM buffering during reconnect, replay without synthetic final silence, and Settings refresh after credential writeback.
- Doubao IME `Test Connection` now exercises the real protocol path by sending a 20ms silent Opus `Last` frame, finishing the session, and waiting for server completion.
- Runtime drain now mirrors the reference project's fallback behavior: before a confirmed final result arrives, the latest partial candidate is retained and can be used when `SessionFinished`/close arrives without a final flag.
- Long-recording handling now accumulates Doubao IME cloud-side VAD segments across multiple WebSocket events. HUD partials are rendered as full-session previews, and mid-recording cloud final events no longer end the post-stop final wait.
- Very long partial handling now tracks a committed prefix plus the current service partial window. Only a clear length drop is treated as the IME service clearing/restarting the partial window; normal service-side revisions replace the active window instead of being committed, avoiding duplicated growing partials.
- Doubao IME partial HUD is now display-only clear-page: the final paste path still keeps the full accumulated text. While the partial fits within three body lines it is shown live in full; after it exceeds that, the HUD clears previous display text and restarts from the current last sentence. The cleared page then accumulates normally until it exceeds three body lines again. Width remains capped to `min(900 DIP, 75% screen width)`, and after the first clear in a recording, fixed four-line HUD height is kept for the rest of that recording to avoid shrinking back to one line. This is intentionally Doubao-only for now.

Verified:

- `.\build.bat` passes after the Doubao IME changes.
- `.\tools\doubao_ime_probe.bat` passes against the live endpoint using saved credentials: `config_credentials=1`, `protocol_ok=1`, `changed=0`.
- The same probe successfully recognized the bundled 16kHz mono speech WAV: `wav_ok=1`, text returned as `开放时间，早上 9 点至下午 5 点。`.
- `.\tools\doubao_ime_probe.bat --streaming` passed the live send/drain path with partial/final events: `streaming_ok=1`, `partial_count=6`, `final_count=1`, `session_finished=1`, text returned as `开放时间，早上 9 点至下午 5 点。`.
- After the reliability review fixes, `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` pass. `git diff --check` reports only existing CRLF warnings.
- After the cross-event cloud-VAD segment accumulation fix, `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` pass. `git diff --check` reports only existing CRLF warnings.
- After tightening the partial-window reset heuristic, `.\build.bat` and `.\tools\doubao_ime_probe.bat --streaming` pass. The WAV and streaming probes both return the expected sample text without duplication.
- After the Doubao-only clear-page HUD tuning, `.\build.bat` passes.
- Automated desktop smoke opened `VoxType Settings`, clicked the `Cloud ASR` tab, selected `Doubao IME (Free)`, confirmed the Doubao controls are visible, and ran the in-app `Test Connection` to `Connection OK. Doubao IME realtime protocol is ready.` Current-DPI screenshot: `build/settings-doubao-ime-click.png`.

Still pending manual UI smoke:

- Real hotkey + microphone recording, partial HUD, final paste, and LLM correction gate.
- Doubao IME now intentionally bypasses local VAD; only the generic too-short guard remains.
- Network interruption/server-close watchdog recovery from the tray app.
- Additional DPI Settings layout checks beyond the current desktop DPI.

## Implementation Scope

- Vendor static `libopus` 1.6.1 under `third_party/opus`, including license files.
- Add `bcrypt.lib` for CNG MD5 used by `x-ss-stub`.
- Add `src/asr/doubao_ime_asr.h/.cpp`:
  - Device registration via `log.snssdk.com/service/2/device_register/`.
  - Token bootstrap via `is.snssdk.com/service/settings/v3/`.
  - WinHTTP WebSocket connection to `frontier-audio-ime-ws.doubao.com/ocean/api/v1/ws?aid=401734&device_id=...`.
  - Headers: Doubao IME Android user-agent, `proto-version: v2`, `x-custom-keepalive: true`.
  - Handwritten protobuf request/response codec.
  - Opus encoding for 16kHz/mono/20ms PCM frames.
  - Result extraction from `result_json.results[*].text`.
- Add `src/asr/doubao_ime_streaming_session.h/.cpp`:
  - Implements `IStreamingAsrSession`.
  - Uses `PendingPcmBuffer`, `CloudAsrReplayBuffer`, partial HUD callback, adaptive finalize timeout, and final dispatch.
  - Keeps network/bootstrap work on the worker thread.
  - Handles auth/token failure by clearing credentials and retrying without losing buffered head audio.
  - Retries transient startup failures while recording continues into `PendingPcmBuffer`.
- Add config fields:
  - `doubao_ime_device_id`
  - `doubao_ime_cdid`
  - `doubao_ime_token` encrypted with DPAPI
- Add Settings UI:
  - Backend/provider selector entries.
  - Credential status.
  - `Test Connection`.
  - `Reset Credentials`.
- Update docs and build files.

## Test Plan

- Build with `.\build.bat`.
- Fresh config: select `doubao_ime`, record once, expect auto registration, recognition, and persisted credentials.
- Restart reuse: verify saved credentials avoid repeat registration.
- Invalid token: corrupt token, record once, expect clear/re-register retry and preserved head audio.
- Streaming: verify partial HUD when enabled and final paste after release.
- VAD: verify too-short and no-speech paths.
- Failure: disconnect network/server close should not hang; watchdog should recover.
- Regression: record once with local, Volcengine, Qwen, Baidu, and MiMo.

## Status

- Implemented in code and build files.
- `.\build.bat` passes locally.
- Live Doubao IME protocol behavior has been verified with a silent protocol probe, a real speech WAV sample, and the `--streaming` send/drain probe. Manual hotkey/microphone UI recording still needs a final smoke test because the endpoint is unofficial and may change.

## Review Snapshot 2026-06-28

Static review against `yyyzl/push-2-talk@725aab0561f040f6e3203e73256b08a7a3d9ae43` shows the core protocol has been reproduced correctly:

- Endpoint/AID/user-agent align with the reference Doubao IME implementation.
- Device registration and settings token bootstrap use the same hosts, params, `body=null`, and MD5 `x-ss-stub` shape.
- WebSocket protocol flow matches the reference: `StartTask` -> `TaskStarted` -> `StartSession` -> `SessionStarted` -> Opus `TaskRequest` frames -> final `TaskRequest` with `FrameState::Last` -> `FinishSession` -> `SessionFinished`.
- Handwritten protobuf field numbers match the reference request/response layout.
- Start-session payload matches the reference `speech_opus`, 16kHz mono, punctuation, and `extra` flags.
- Integration follows this project's streaming-session architecture instead of adding provider protocol logic directly to `main.cpp`.
- Build verification has passed with `.\build.bat`.

Gaps found during review (addressed in the implementation update below):

- Credential bootstrap cancellation is incomplete. `Abort()` closes active WebSocket/request handles, but device registration and token fetch currently use `SendCloudHttpRequest()` without an externally closable handle, so abort/quit/too-short can wait for the HTTP timeout during first-use bootstrap.
- Initial connect retry is too narrow. `ConnectWithCredentialRetry()` retries auth/token failures only; transient registration, token, WebSocket upgrade, `StartTask`, or `StartSession` failures are not retried while preserving already buffered audio.
- `Test Connection` can produce a false positive. It currently verifies `Connect()` only, then sends `FinishSession` without sending an Opus `TaskRequest`/`Last` frame or waiting for `SessionFinished`.
- Result parsing is functional but fragile. The C++ scanner should more strictly mirror the reference's `extra.nonstream_result` lookup rather than searching the whole result object for `nonstream_result`.
- Settings credential status can become stale if credentials are saved by a background recording session while the Settings window is already open.

Planned fixes from that review:

- Add a Doubao-specific cancellable HTTP bootstrap path or extend `CloudHttpRequest` with an optional cancellation/active-handle hook.
- Expand Doubao initial connection retry to handle transient HTTP/WinHTTP/WS startup failures while recording continues into `PendingPcmBuffer`, then replay buffered PCM after reconnect.
- Make `TestConnection()` send at least one 20ms silent Opus frame marked `Last`, call `FinishSession`, and wait for `SessionFinished`.
- Tighten `ExtractTextCandidate()` so `nonstream_result` is read from `extra.nonstream_result`.
- Post a Settings refresh message after main-thread Doubao credential updates when `g_settingsWindow` is live.

## Implementation Update 2026-06-28

The review gaps above have been addressed in code:

- `src/asr/doubao_ime_asr.cpp` now uses a Doubao-specific cancellable HTTP path for device registration and token bootstrap. The active bootstrap session/connect/request handles are published through `DoubaoConnection`, so `Abort()` can close them instead of waiting for the full HTTP timeout.
- WebSocket startup handles are also published through `DoubaoConnection`; abort now closes active request, websocket, connect, and session handles.
- WebSocket upgrade explicitly sends the Doubao IME Android `User-Agent` header in addition to `proto-version: v2` and `x-custom-keepalive: true`.
- `ConnectWithCredentialRetry()` now attempts up to three startup attempts, distinguishes credential refresh from transient startup failures, and keeps recording audio in `PendingPcmBuffer` while reconnecting.
- If credential bootstrap succeeds but WebSocket startup later fails, the updated credentials are synchronized before retry so the client does not repeatedly register fresh devices.
- Replay buffering no longer stores the padded silence added to a final `Last` frame. Retries replay only the real captured PCM.
- `TestConnection()` now sends one 20ms silent Opus frame marked `Last`, sends `FinishSession`, and waits for server completion before reporting success.
- `TestConnection()` now also retries transient failures, clears bad credentials on auth failures, and returns credential changes even if the final probe fails.
- `ExtractTextCandidate()` now reads final `nonstream_result` from `extra.nonstream_result`.
- The streaming drain thread now keeps the latest non-final candidate as a fallback final text until an explicit final result arrives, matching the reference behavior and avoiding unnecessary empty-final retries when the service ends after partial text.
- Main-thread Doubao credential updates now notify an open Settings window via `kDoubaoImeSettingsRefreshMessage`.
- `tools/doubao_ime_probe.bat` / `tools/doubao_ime_probe.cpp` provide a repeatable live diagnostic probe outside the tray app. The probe compiles to `build/tools/doubao_ime_probe.exe`, reuses saved `config.json` Doubao IME credentials when available, runs a protocol check, optionally recognizes a 16kHz mono WAV, and supports `--streaming` to send WAV frames with a live drain thread for partial/final validation. Use `--fresh` to force temporary re-registration.
- The diagnostic probe passed against the live Doubao IME endpoint: fresh registration returned `ok=1`, non-empty device id/cdid/token, and `Connection OK. Doubao IME realtime protocol is ready.` A later config-backed run returned `config_credentials=1` and `changed=0`, confirming credential reuse.
- The diagnostic probe also passed with the repository's 16kHz mono Chinese test wav, returning non-empty text (`text_len=19`).
- `.\build.bat` passes after these changes.
- Follow-up verification after the partial-fallback fix: `.\tools\doubao_ime_probe.bat` still returns `protocol_ok=1`, `wav_ok=1`, and the expected sample text; `.\build.bat` still succeeds.
- Follow-up streaming verification: `.\tools\doubao_ime_probe.bat --streaming` returns `streaming_ok=1`, `partial_count=6`, `final_count=1`, `session_finished=1`, and the expected sample text.
- Follow-up desktop UI verification opened the real Settings window, switched to `Cloud ASR`, selected `Doubao IME (Free)`, verified credential/test/reset controls are visible without obvious overlap at the current DPI, and confirmed the Settings `Test Connection` path returns OK.
- Reliability review follow-up:
  - Doubao IME Settings `Test Connection` now uses a generation id, preventing stale async test results from overwriting credentials after `Reset Credentials` or a newer test.
  - Doubao IME `result_json.results[*].text` is now aggregated in result order, fixing long recordings where the service splits recognition into multiple segments and only the last segment was pasted.
  - Streaming cloud pre-captured head audio now goes through `StreamingVadTrimmer` before replay for Qwen and Volcengine. Doubao IME was later changed to bypass local VAD and upload raw PCM/Opus.
  - `PendingPcmBuffer` now has a default 120-second PCM cap to prevent unbounded memory growth while a streaming backend is stalled; Qwen and Doubao IME surface overflow as a retryable transport failure.
  - Mid-recording transport loss now reports `Buffering...` instead of `Reconnecting...`, matching the actual buffer-then-replay strategy.
  - Doubao IME no longer starts local `StreamingVadTrimmer`; it ignores `Enable VAD` and relies on the free IME endpoint's own behavior.
  - Doubao IME final text is now accumulated across WebSocket events, not only within one `result_json.results[]` payload. This handles cloud-side VAD splitting a long recording into multiple final segments.
  - Doubao IME post-stop waiting now ignores pre-`FinishSession` final events and waits for a post-`FinishSession` final or `SessionFinished`, matching the push-to-talk lifecycle instead of treating cloud-side VAD segment completion as whole-recording completion.
  - Doubao IME partial handling now distinguishes normal partial revisions from service-side partial window resets by requiring a clear length drop before committing the active window. Revisions replace the active window; resets commit the old active window into the final accumulator before displaying the new one.
  - Doubao IME partial HUD now sends the full partial through a constrained HUD update message, then formats it on the UI thread. The formatter uses real HUD line measurement: live full text under three body lines, then a clear-page display that restarts from the current last sentence and continues accumulating until that page exceeds three body lines again. A separate fixed-height latch keeps the HUD at four lines after the first clear in a recording, even if the current display text is only one line. The ASR accumulator and final paste text remain full length; only the HUD display string is shortened.
  - Verification after the segment aggregation fix: `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` pass; `git diff --check` reports only existing CRLF warnings.
  - Verification after the cross-event cloud-VAD accumulation fix: `.\build.bat`, `git diff --check`, and `.\tools\doubao_ime_probe.bat --streaming` pass; `git diff --check` reports only existing CRLF warnings.
  - Verification after tightening the partial-window reset heuristic: `.\build.bat` and `.\tools\doubao_ime_probe.bat --streaming` pass; the probe's non-streaming WAV and streaming paths both return `开放时间，早上 9 点至下午 5 点。`.
  - Verification after the clear-page HUD tuning: `.\build.bat` passes.

Remaining verification:

- Manual hotkey/microphone smoke testing is still useful for UI-level confidence: actual recording, partial HUD, final paste, too-short, network interruption/watchdog recovery, and extra DPI layout passes.
