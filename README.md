<p align="center">
  <img src="../src/app.ico" width="64" alt="Voice LLM ASR Input icon" />
</p>

<h1 align="center">Voice LLM ASR Input</h1>

<p align="center">
  <strong>Local voice input for Windows. Press, speak, paste.</strong><br/>
  Windows 11 local voice input tool — hold to speak, release to paste, no cloud required
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011-blue?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/github/license/melody0709/Voice_LLM_ASR_Input" alt="License" />
  <img src="https://img.shields.io/github/v/release/melody0709/Voice_LLM_ASR_Input" alt="Release" />
  <img src="https://img.shields.io/badge/CPU-only-green" alt="CPU Only" />
</p>

<p align="center">
  Current version: <code>v0.2.2</code> &nbsp;|&nbsp; 🇨🇳 <a href="doc/README_zh.md">中文版</a>
</p>

<!-- TODO: Replace with actual demo GIF -->
<!-- Record a GIF showing: press CapsLock → HUD appears with volume bars → speak → release → text pasted into editor -->
<!-- Recommended tool: ScreenToGif (https://www.screentogif.com/) -->
<!-- <p align="center"><img src="docs/demo.gif" width="600" alt="Demo" /></p> -->

---

## Highlights

- **Press and Speak** — Default CapsLock long-press to record, release to auto-paste to current window; short press toggles Caps Lock normally
- **100% Local** — C++ directly calls sherpa-onnx, no Python, no cloud API, all data stays on your machine
- **Real-time HUD** — Bottom floating capsule window during recording, 5 volume bars responding to sound
- **Dual VAD Options** — Silero VAD (lightweight) / FireRed VAD (high precision F1 97.57), intelligently skips silence
- **LLM Correction (Optional)** — Supports DeepSeek / OpenRouter / SiliconFlow and other providers, one-click configuration

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
.\build\VoiceLLMASRInput.exe
```

Right-click the tray icon to open Settings, hold the hotkey to start recording, release to recognize and paste.

---

<details open>
<summary><strong>Settings Guide</strong></summary>

**Recognition tab**
- `ASR model` — Speech recognition model:
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

**Shortcut tab**
- Click the input box then press the hotkey to record
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
  main.cpp          — Tray, Settings, HUD, hotkeys, recording, ASR engine
  firered_vad.h     — FireRed VAD module (header-only)
  llm_refine.h      — LLM correction module (header-only)
  resources.rc / resource.h / app.ico / app.manifest
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

- Recognition after recording, true streaming partial not yet implemented
- Text injection primarily via clipboard + Ctrl+V, admin privilege windows may block
- Model files are large (~3GB), first load takes a few seconds
- LLM correction not fully implemented yet (Punctuation's +L option currently treated as local punctuation)

</details>

<details>
<summary><strong>Changelog</strong></summary>

See [CHANGELOG.md](CHANGELOG.md)

**Recent Updates:**
- **v0.2.2** — GitHub release preparation: directory reorganization, aria2c multi-connection download, new user guidance
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
