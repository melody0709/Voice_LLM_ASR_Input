# Voice LLM ASR Input

Windows 11 本地 CPU 语音输入工具。当前 v1 目标不是完整 TSF/IME，而是一个托盘常驻程序：按住快捷键录音，松开后本地 ASR 识别，把最终文本粘贴到当前输入位置。

当前版本：`v0.1.5`

## 当前能力

- Win32 托盘常驻程序，带托盘图标、程序图标、进程图标。
- Settings 支持切换 ASR 模型、模型目录、线程数、VAD、Partial、Punctuation。
- Shortcut tab 支持录入长按快捷键，默认 `CapsLock`。
- 默认 `CapsLock` 短按正常切换大小写，长按 300ms 后才开始语音输入；语音输入结束后不会改变原来的大小写状态。
- 打开 Settings 时暂停全局快捷键监听，方便重新录入 `CapsLock`。
- **C++ 直接调用 sherpa-onnx**：无需 Python 环境，ASR/VAD/标点全部在进程内完成。
- Direct2D/DirectWrite HUD：录音时显示底部胶囊悬浮窗，5 根音量条由实时 PCM RMS 驱动，并按 DPI 正确缩放。
- Silero VAD int8：启用后先做人声检测，保守裁掉整段头尾静音；无人声时不加载/不运行 ASR。
- FireRed VAD：可选的高精度 VAD（F1 97.57，误报率 2.69%），Settings 中可切换。
- 支持三种 ASR 模型：
  - FireRedASR2 CTC int8 ONNX
  - FireRedASR2 AED int8 ONNX
  - SenseVoiceSmall int8 ONNX
- 支持本地标点后处理：
  - `sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`

## 快速开始

1. 确认模型放在 `models/` 下：

```text
models/
  sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/
  sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26/
  sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/
  sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/
  fireredvad_stream_vad_with_cache.onnx   (可选，FireRed VAD 模型)
  cmvn.ark                                (可选，FireRed VAD CMVN 参数)
```

2. 构建：

```powershell
.\build.bat
```

3. 运行：

```powershell
.\build\VoiceLLMASRInput.exe
```

运行后右键托盘图标打开 `Settings...`。按住设置的快捷键开始录音，松开后识别并粘贴。默认 `CapsLock` 需要长按约 300ms 才会触发录音，短按仍用于大小写切换。

## Settings

- `Recognition` tab：
  - `ASR model`: FireRedASR2 CTC / FireRedASR2 AED / SenseVoiceSmall
  - `Model folder`: 当前模型目录
  - `Threads`: `auto`, `1` ... `8`
  - `Enable VAD`: 使用 Silero VAD 保守裁剪头尾静音，无人声时跳过 ASR
  - `VAD model`: Silero VAD / FireRed VAD
  - `Partial result`: 当前 UI 预留，后续做流式/模拟流式
  - `Punctuation`: `Disabled`, `Auto punctuate`, `Auto punctuate + LLM`
- `Shortcut` tab：
  - 点击输入框后按快捷键。
  - `Esc` 取消本次录入。
  - `Backspace/Delete` 清空快捷键。
  - 默认 `CapsLock` 支持短按大小写切换、长按语音输入。

当前 `Auto punctuate` 会启用本地 CT-Transformer 标点模型。`Auto punctuate + LLM` 暂时按本地标点处理，真正的保守 LLM 纠错还没有接入。

## 重要文件

- `main.cpp`: Win32 托盘、Settings、Direct2D HUD、快捷键监听、录音、ASR 引擎（sherpa-onnx C++ API）、文本粘贴。
- `firered_vad.h`: FireRedVAD 模块（fbank 特征提取 + ONNX 流式推理）。
- `ARCHITECTURE.md`: 当前架构说明。
- `AGENTS.md`: 后续开发代理/协作者注意事项。
- `CHANGELOG.md`: 版本变更记录。
- `build.bat`: Visual Studio 2022 编译脚本。
- `resources.rc`, `resource.h`, `app.ico`, `app.manifest`: Windows 资源和图标。

归档文档在 `.plan/` 目录（不提交 git）：

- `.plan/complete/`: 已完成的研究和规划（FireRedVAD 接入、sherpa-onnx C++ 集成等）。
- `.plan/ongoing/OPTIMIZATION_PLAN.md`: 后续优化路线。

## 当前限制

- 录音后才识别，尚未实现真正流式 partial。
- HUD 已有实时音量动画，但 partial 文本仍未接入真实流式 ASR。
- 文本注入当前以剪贴板 + `Ctrl+V` 为主。
- 管理员权限窗口可能拦截普通权限程序输入。
- Partial 设置目前主要是 UI 和配置预留。
- 保守 LLM 纠错尚未实现。
- 模型文件较大，默认不纳入 git。

## 推荐测试

- FireRedASR2 AED：质量较好，长句无标点时建议开启 `Punctuation = Auto punctuate`。
- FireRedASR2 CTC：速度更快，适合默认输入体验测试。
- SenseVoiceSmall：轻量 fallback，适合低资源机器测试。

测试时记录：

- 模型、线程数、音频时长。
- 首次加载耗时和第二次复用耗时。
- 松开快捷键到文本上屏耗时。
- HUD 是否随音量跳动、默认文本是否完整显示、DPI 缩放下是否有裁切或黑边。
- `CapsLock` 短按/长按是否符合预期，长按前后的大小写状态是否保持一致。
- 标点质量、长句断句、专有名词识别。
