<p align="center">
  <img src="./src/app/app.ico" width="64" alt="VoxType icon" />
</p>

<h1 align="center">VoxType</h1>

<p align="center">
  <strong>Voice typing for Windows. Press, speak, paste.</strong><br/>
  Windows voice typing tool — hold to speak, release to paste, local & cloud ASR
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011-blue?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/github/license/melody0709/VoxType" alt="License" />
  <img src="https://img.shields.io/github/v/release/melody0709/VoxType" alt="Release" />
  <img src="https://img.shields.io/badge/CPU-only-green" alt="CPU Only" />
</p>

<p align="center">
  Current version: <code>v0.9.9</code> &nbsp;|&nbsp; 🇨🇳 <a href="doc/README_zh.md">中文版</a>
</p>

https://github.com/user-attachments/assets/36243dc2-cfc8-41fb-b0cf-6e558f02cd5e

---

## Highlights

- **Press and Speak** — Default CapsLock long-press to record, release to auto-paste to current window; short press toggles Caps Lock normally
- **Local-first** — C++ directly calls sherpa-onnx without Python; optional cloud ASR / LLM providers can be enabled explicitly
- **Real-time HUD** — Bottom floating capsule window during recording, 5 volume bars responding to sound
- **Dual VAD Options** — Silero VAD (lightweight) / FireRed VAD (high precision F1 97.57), intelligently skips silence
- **LLM Correction (Optional)** — Supports DeepSeek / OpenRouter / SiliconFlow and other providers, one-click configuration
- **Cloud ASR (Optional)** — Supports Volcano Engine (Doubao), Baidu Cloud, Qwen ASR (`qwen3-asr-flash-realtime`), Xiaomi MiMo ASR (`mimo-v2.5-asr`), and experimental Doubao IME ASR as alternative backends, with optional fallback ASR

## Quick Start

### 1. Download Models

```powershell
.\download_models.ps1
```

Interactive menu for model selection, auto-downloads and extracts to `models/` directory (~3GB disk space).

> Silero VAD and FireRed VAD are built-in, no download needed.

### 2. Build

```powershell
.\build.bat
```

Requires Visual Studio 2022 (C++ desktop development workload).

### 3. Run

```powershell
.\build\run\x64-release\VoxType.exe
```

Right-click the tray icon to open Settings, hold the hotkey to start recording, release to recognize and paste.

### 4. Package for distribution

```powershell
.\build.bat --package
```

This creates verified assets under `build\packages`: a Portable `.7z` and an
x64 per-machine MSI. Both originate from the same canonical runtime payload;
the packager re-extracts and hashes each result before publishing it.

- The MSI defaults to `Program Files\VoxType`. Choose **Advanced...** during
  setup to select another directory. That selection is retained by later MSI
  upgrades.
- Installed builds store `config.json`, downloaded ASR/punctuation models, and
  logs in `%LOCALAPPDATA%\VoxType`, so upgrades do not write into Program
  Files or remove user data.
- Portable builds include `portable.flag` and keep those files beside the
  extracted executable instead.
- Packages without a configured release certificate are deliberately named
  `-unsigned`; `--require-signing` refuses to publish without the signing
  environment variables documented in `packaging/windows/UPGRADE_CONTRACT.md`.

---

<details open>
<summary><strong>Settings Guide</strong></summary>

**General tab**
- `Start VoxType when I sign in to Windows` registers the current user's
  Windows Run entry. It is off by default and can be safely enabled for either
  the MSI or Portable build; saving after moving a Portable folder corrects its
  stored executable path.

**Recognition tab**
- `ASR Backend` — Select between `Local (sherpa-onnx)`, `Volcano Engine`, `Baidu Cloud`, `Qwen ASR`, `MiMo ASR`, and `Doubao IME (Free)`
- `Fallback` — Optional backup ASR backend: `Local`, `Baidu Cloud`, `Qwen ASR`, `MiMo ASR`, or `Doubao IME (Free)`. If the primary backend ends in an operational failure such as timeout, network error, auth/config error, or model load error, VoxType retries the same raw PCM with the fallback backend before showing the final result. `Too short` and `No speech detected` do not trigger fallback.
- `ASR model` — Speech recognition model (only for Local backend):
  - `FireRedASR2 CTC` — Fast, suitable for daily input
  - `FireRedASR2 AED` — Better quality, more accurate for long sentences
  - `SenseVoiceSmall` — Lightweight model, suitable for low-resource machines
- `Model folder` — Model file storage directory
- `Threads` — Inference thread count, `auto` uses CPU core count (max 8)
- `Enable VAD` — Enable voice activity detection, checks for speech before recording, skips ASR when no voice detected to save time
- `VAD model` — Voice activity detection model (requires Enable VAD):
  - `Silero VAD` — Lightweight and fast, accuracy F1 95.95
  - `FireRed VAD` — High precision (F1 97.57, false alarm rate 2.69%), model only 2.2MB
- `Punctuation` — Post-recognition processing:
  - `Disabled` — No processing, outputs ASR raw text
  - `Auto punctuate` — Local CT-Transformer auto-punctuation (no network required)
  - `Auto punctuate + LLM` — Local punctuation + cloud LLM correction (requires configuring provider and API Key in LLM tab, see LLM Correction section below)
- `Hold hotkey` — Click the input box then press the hotkey to record
  - `Esc` cancels this recording, `Backspace/Delete` clears the hotkey
  - Default CapsLock: Short press toggles Caps Lock, long press 300ms triggers voice input

**LLM tab**
- `Provider` — Provider dropdown, built-in DeepSeek / OpenRouter / SiliconFlow presets, auto-fills fields below on selection
- `API Base URL` — Provider API address (auto-filled by presets)
- `API Key` — API key, stored encrypted with DPAPI in local config
- `Model` — Model name (e.g., `deepseek-v4-flash`)
- `Test Connection` — Test if API connection is working, results shown in Status area below
- `Extra Params` — Additional JSON parameters, merged into request body (format: `"key":"value","key2":"value2"`). Preset providers auto-inject thinking mode disabled parameters, users can add custom fields here
- `[+]` / `[−]` — Add/delete custom providers (presets cannot be deleted)

**LLM Prompt tab**
- `System Prompt` — Multi-line editor, customize LLM correction system prompt (leave empty to use built-in default)
- `Basic Fix` / `Deep Fix` — Preset buttons, one-click fill for different correction intensity System Prompts

**Cloud ASR tab**
- `Provider` — Select between `Volcano Engine (Doubao)`, `Baidu Cloud`, `Qwen ASR (DashScope)`, `MiMo ASR (Xiaomi)`, and `Doubao IME (Free)`, controls below update dynamically
- **Baidu Cloud**: `API Key` / `Secret Key` (DPAPI encrypted) + `Language Model` (Mandarin/English/Cantonese/Sichuanese) + `Test Connection`
- **Volcano Engine (Doubao)**: `API Key` (DPAPI encrypted) + `ASR Mode` + `Model Version` + `Language` + `Test Connection`
  - ASR Mode: `bigmodel_nostream` (recommended, highest accuracy) / `bigmodel_async` (best latency) / `bigmodel` (real-time partial)
  - Model Version: `Seed-ASR 2.0 (duration)` / `Seed-ASR 2.0 (concurrent)` / `BigASR 1.0 (duration)` / `BigASR 1.0 (concurrent)`
  - Hotwords ID/Name, Correct ID/Name — Reference hotword and correction tables from the self-learning platform
  - Use history as context — Sends recent recognition results as dialog context for improved accuracy
  - Read input field context — Reads current input field text as ASR context (UIA/MSAA/WM_GETTEXT layered fallback, input field priority, history fallback)
- **Qwen ASR (DashScope)**: `API Key` (DPAPI encrypted) + `Base URL` + `Model` + `Language` + `Chunk ms` + `Test Connection`
  - Default model: `qwen3-asr-flash-realtime`
  - Turn detection is fixed to Manual for push-to-talk usage; Server VAD settings are intentionally hidden
- **MiMo ASR (Xiaomi)**: `API Key` (DPAPI encrypted) + `Base URL` + `Model` + `Language` + `Test Connection`
  - Default Base URL: `https://token-plan-ams.xiaomimimo.com/v1`
  - Default model: `mimo-v2.5-asr`; audio is uploaded as WAV via `/chat/completions`
- **Doubao IME (Free)**: no API key field. The experimental provider registers a Doubao IME-style device, stores device credentials with DPAPI-encrypted token, encodes PCM to Opus, and uses the unofficial `frontier-audio-ime-ws.doubao.com` WebSocket protocol. Availability and terms are not guaranteed.
  - Doubao IME bypasses local VAD and relies on the IME service's own segmentation. Partial HUD updates and final text are accumulated across cloud-side segments during one hotkey hold, so long recordings are pasted as one combined result after release. When selected as Fallback, Doubao IME replays the same raw PCM through a recorded request and writes refreshed credentials back to the saved config.
  - Streaming partial HUD for Qwen, Volcano Engine, and Doubao IME is display-only clear-page: it shows live text within three body lines, then clears previous HUD text and restarts from the current last sentence; the new page keeps accumulating until it exceeds three body lines again, while the final paste text stays complete.
  - Diagnostic probe: run `.\tools\doubao_ime_probe.bat` to compile a small console probe that reuses saved Doubao IME credentials when available, performs a live protocol check, and when the bundled sample wav exists, performs a real speech recognition check. Add `--streaming` to send WAV frames with a live drain thread and validate partial/final streaming behavior; add `--fresh` to force temporary re-registration.
- Cloud ASR backends handle recognition remotely; when VAD is enabled, Qwen and Volcano Engine use local streaming VAD trim, batch cloud backends use batch VAD trim before upload, and Doubao IME uploads raw PCM/Opus without local VAD. Local punctuation models are still bypassed for cloud backends

</details>

<details>
<summary><strong>Supported Models</strong></summary>

| Model | Type | Features |
|---|---|---|
| FireRedASR2 CTC int8 | ASR | Fast, suitable for daily input |
| FireRedASR2 AED int8 | ASR | Better quality, more accurate for long sentences |
| SenseVoiceSmall int8 | ASR | Lightweight, suitable for low-resource machines |
| CT-Transformer Punctuation int8 | Post-processing | Chinese/English punctuation auto-completion |

Model download links can be found in the `download_models.ps1` script, or manually from [sherpa-onnx releases](https://github.com/k2-fsa/sherpa-onnx/releases).

</details>

<details>
<summary><strong>LLM Correction</strong></summary>

Optional cloud LLM text correction added since v0.2.0. Disabled by default, requires manual enable:

1. Settings → LLM tab → Select provider, enter API Key
2. Settings → Recognition → Punctuation set to `Auto punctuate + LLM`

Features:
- Preset providers auto-inject thinking mode disabled parameters
- Extra Params supports custom JSON fragments merged into request body
- API Key encrypted with DPAPI storage
- Backward compatible with old config format

</details>

<details>
<summary><strong>Project Structure</strong></summary>

```
src/
  app/              — Entry point, globals, Win32 resources
  asr/              — ASR clients, batch/streaming sessions, ASR result dispatch helpers
  audio/            — Local ASR engine, audio capture, WASAPI, FireRed VAD, streaming VAD trim
  ui/               — HUD, hotkey handling, Settings window
  core/             — Shared utilities, LLM correction, input context reading
dll/                — Runtime DLLs (sherpa-onnx, onnxruntime, etc.)
third_party/        — Headers and import libraries
models/             — Model files (not committed to git)
build.bat           — Visual Studio 2022 build script
download_models.ps1 — Model download script
ARCHITECTURE.md     — Detailed architecture description
AGENTS.md           — Development collaborator notes
CHANGELOG.md        — Version change log
```

</details>

<details>
<summary><strong>Known Limitations</strong></summary>

- Local, Baidu, and MiMo results are finalized after recording; Qwen, Volcano Engine, and Doubao IME can show partial HUD during recording, with final text pasted after release
- Text injection primarily via clipboard + Ctrl+V, admin privilege windows may block
- Model files are large (~3GB), first load takes a few seconds

</details>

<details>
<summary><strong>Changelog</strong></summary>

See [CHANGELOG.md](CHANGELOG.md)

**Recent Updates:**
- **v0.9.9** — Restored the shared active-recording HUD animation for Local and other batch ASR backends, so their input-level bar lights up and moves again without changing the v0.9.3 capture-first cloud startup order
- **v0.9.8** — Canonical CMake/Ninja release pipeline with verified Portable and MSI packages, MSI upgrade/folder-selection support, portable data isolation, and Start with Windows setting
- **v0.9.7** — Privacy-safe, bounded ASR diagnostics with structured primary/fallback lifecycle events; streaming primary failures reported before release are now deferred until the full PCM is available, so configured fallback is no longer bypassed
- **v0.9.6** — Doubao IME can now be selected as a fallback ASR target through a recorded-PCM helper, including credential refresh/writeback, cloud timing, Settings support, and raw-PCM replay without local VAD trim
- **v0.9.5** — Recognition-tab fallback ASR backend, serial primary-to-fallback orchestration for batch and streaming failures, shorter fallback-enabled streaming final wait, result metadata for debug/LLM, and local-model preload when Local is configured as fallback
- **v0.9.4** — Experimental Doubao IME (`doubao_ime`) streaming cloud ASR backend, vendored static Opus 1.6.1, credential bootstrap/reset UI, protocol/WAV/streaming diagnostic probe, Doubao long-recording aggregation fixes, and shared clear-page streaming partial HUD for Qwen/Volcengine/Doubao IME
- **v0.9.3** — Volcengine rapid recording head-audio loss fix, streaming VAD double-processing fix, connection reuse/timeout tuning, active request fast cancel, and Qwen/Volcengine startup/stop cleanup
- **v0.9.2** — HUD DPI-aware rendering, Volcengine/Qwen WebSocket double-close and data-race fixes, Qwen activeClient UAF fix, retry abort checks
- **v0.9.1** — Cloud ASR architecture stabilization, Xiaomi MiMo ASR (`mimo-v2.5-asr`) backend, Qwen/Volcengine streaming sessions, shared streaming/batch VAD trim core, Baidu same-PCM retry and token refresh retry, source tree reorganization
- **v0.9.0** — Qwen ASR (`qwen3-asr-flash-realtime`) backend, true streaming send with partial HUD, Manual turn detection by default, Qwen watchdog/replay retry, shared ASR session/dispatcher/result architecture
- **v0.8.7** — Volcengine retry recognition: connection-loss audio buffering + full-PCM retry, adaptive finalize timeout, unified ASR error classification, no-text close handling
- **v0.8.6** — Volcengine connection reuse optimization (fast consecutive recording latency reduced from ~1.8s to ~0.4s), 3s expiry detection + hSession connection pool cleanup, time-gated internal retry, progressive external retry
- **v0.8.5** — Input field context (UIA/MSAA/WM_GETTEXT, read input text as ASR context), context logic refactor (input field priority, history fallback, remove window title), remove accelerate score, settings UI reorganize
- **v0.8.4** — Fix Volcengine no-speech drainThread.join() blocking 17+ seconds (close WebSocket before join), watchdog thread handle leak crash, SendMessage(WM_PASTE) UI thread blocking
- **v0.8.2** — Fix Volcengine nostream/async result loss, long recording truncation, WinHttpCloseHandle deadlock, logic deadlock, short audio false timeout, HUD timer race
- **v0.8.0.1** — Fix Volcengine nostream no-speech hang (WinHttpWebSocketReceive 1ms timeout not working)
- **v0.8.0** — Streaming VAD (real-time speech detection during recording, skip silence), Volcengine nostream/async long recording hang fix, HUD speech detection visual feedback, watchdog auto-renew during recording, code quality fixes
- **v0.7.5** — Volcengine WebSocket hang fix (forceAbort), connection prewarm, connection expiry rebuild, WinHTTP proxy/no-proxy fix, timeout tuning, reduced logging verbosity
- **v0.7.4** — WeChat Chinese IME paste fix (WM_CHAR bypass IME), IMM32 input method state switching, Unicode SendInput fallback, Force Unicode Input menu
- **v0.7.3** — WASAPI Shared Mode capture (48kHz→16kHz resample + waveIn fallback), Debug Mode console with per-stage timing, Baidu ASR response parsing fixes

- **v0.7.2** — Thread safety fixes (volcengine thread join, Baidu token mutex, VolcDebugLog mutex), SSL cert verification restored, `g_volcAudioCs` leak fix, utility functions deduplicated to `utils.h`, model downloader async, `AsrEngine::lock` encapsulation
- **v0.7.1** — `bigmodel_nostream` speed optimization (skip intermediate receives), WinHTTP connection reuse, `ExtractJsonStr` escape fix, thread safety fix
- **v0.7.0** — Volcengine ASR full API parameter support, hotwords/correction tables, dialog context, `corpus` merge fix, `context` format fix
- **v0.6.2** — Settings UI style unification: `UiStyle` namespace, consistent row spacing across all tabs
- **v0.6.1** — ASR model preload on startup, DELAYLOAD for DLLs, idle memory reduced to ~12 MB
- **v0.6.0** — Source code refactored from single-file to multi-module architecture
- **v0.5.0** — Cloud ASR UI overhaul, async mode fix, Shortcut merged into Recognition
- **v0.4.0** — Volcengine (豆包) streaming ASR integration via WebSocket
- **v0.3.0** — Baidu Cloud ASR integration
- **v0.2.1** — Multi-provider preset system, LLM Prompt independent Tab, Extra Params
- **v0.2.0** — FireRedVAD integration, cloud LLM correction module
- **v0.1.4** — C++ direct sherpa-onnx calls, removed Python dependency

</details>

---

## Acknowledgments

- [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) — ASR / VAD / Punctuation inference engine
- [FireRedASR](https://github.com/FireRedTeam/FireRedASR) — High precision Chinese ASR model
- [FireRed VAD](https://github.com/FireRedTeam/FireRedAudio) — High precision VAD model
- [Silero VAD](https://github.com/snakers4/silero-vad) — Lightweight VAD model
- [onnxruntime](https://github.com/microsoft/onnxruntime) — ONNX inference engine
- [Baidu Intelligent Cloud](https://ai.baidu.com/tech/speech/asr) — Baidu Cloud ASR API
- [Volcengine Speech](https://www.volcengine.com/docs/6561/1354869) — Volcengine (豆包) streaming ASR API
- [Alibaba Cloud Model Studio Qwen ASR](https://help.aliyun.com/zh/model-studio/qwen-asr-realtime-interaction-process) — Qwen ASR realtime API
- [Xiaomi MiMo](https://platform.xiaomimimo.com/docs/zh-CN/usage-guide/Speech-Recognition) — MiMo ASR API
- [push-2-talk](https://github.com/yyyzl/push-2-talk) — MIT-licensed Doubao IME protocol research reference
- [Opus](https://opus-codec.org/) — Audio codec used by the Doubao IME experimental provider
