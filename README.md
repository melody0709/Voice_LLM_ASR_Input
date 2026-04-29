# Voice LLM ASR Input

Windows 11 本地 CPU 语音输入工具。当前 v1 目标不是完整 TSF/IME，而是一个托盘常驻程序：按住快捷键录音，松开后本地 ASR 识别，把最终文本粘贴到当前输入位置。

## 当前能力

- Win32 托盘常驻程序，带托盘图标、程序图标、进程图标。
- Settings 支持切换 ASR 模型、模型目录、线程数、VAD、Partial、Postprocess。
- Shortcut tab 支持录入长按快捷键，默认 `CapsLock`。
- 打开 Settings 时暂停全局快捷键监听，方便重新录入 `CapsLock`。
- 常驻 `asr_worker.py`，避免每次录音后重新启动 Python 和加载模型。
- 本地 TCP JSON line 通信：`127.0.0.1:18088`。
- 支持三种 ASR 模型：
  - FireRedASR2 CTC int8 ONNX
  - FireRedASR2 AED int8 ONNX
  - SenseVoiceSmall int8 ONNX
- 支持本地标点后处理：
  - `sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`

## 快速开始

1. 安装 Python 依赖：

```powershell
pip install sherpa-onnx numpy
```

2. 确认模型放在 `models/` 下：

```text
models/
  sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25/
  sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26/
  sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17/
  sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/
```

3. 构建：

```powershell
.\build.bat
```

4. 运行：

```powershell
.\build\VoiceLLMASRInput.exe
```

运行后右键托盘图标打开 `Settings...`。按住设置的快捷键开始录音，松开后识别并粘贴。

## Settings

- `Recognition` tab：
  - `ASR model`: FireRedASR2 CTC / FireRedASR2 AED / SenseVoiceSmall
  - `Model folder`: 当前模型目录
  - `Threads`: `auto`, `1`, `2`, `3`, `4`
  - `Enable VAD`: 当前 UI 预留，后续做录音切段/VAD
  - `Partial result`: 当前 UI 预留，后续做流式/模拟流式
  - `Postprocess`: `none`, `itn`, `llm`
- `Shortcut` tab：
  - 点击输入框后按快捷键。
  - `Esc` 取消本次录入。
  - `Backspace/Delete` 清空快捷键。

当前 `itn` 会启用本地 CT-Transformer 标点模型。`llm` 暂时按本地标点处理，真正的保守 LLM 纠错还没有接入。

## 重要文件

- `main.cpp`: Win32 托盘、Settings、快捷键监听、录音、worker 通信、文本粘贴。
- `asr_worker.py`: 常驻 ASR worker，负责模型加载、识别、标点后处理。
- `asr_cli.py`: 旧的一次性 ASR CLI，主要用于调试/对照。
- `PLAN.md`: 研究计划、模型取舍和后续路线。
- `ARCHITECTURE.md`: 当前架构说明。
- `AGENTS.md`: 后续开发代理/协作者注意事项。
- `build.bat`: Visual Studio 2022 编译脚本。
- `resources.rc`, `resource.h`, `app.ico`, `app.manifest`: Windows 资源和图标。

## 当前限制

- 录音后才识别，尚未实现真正流式 partial。
- 文本注入当前以剪贴板 + `Ctrl+V` 为主。
- 管理员权限窗口可能拦截普通权限程序输入。
- VAD/Partial 设置目前主要是 UI 和配置预留。
- 保守 LLM 纠错尚未实现。
- 模型文件较大，默认不纳入 git。

## 推荐测试

- FireRedASR2 AED：质量较好，长句无标点时建议开启 `Postprocess = itn`。
- FireRedASR2 CTC：速度更快，适合默认输入体验测试。
- SenseVoiceSmall：轻量 fallback，适合低资源机器测试。

测试时记录：

- 模型、线程数、音频时长。
- 首次加载耗时和第二次复用耗时。
- 松开快捷键到文本上屏耗时。
- 标点质量、长句断句、专有名词识别。
