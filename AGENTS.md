# AGENTS.md

本文件给后续继续开发本项目的 Codex/开发者使用。请优先阅读 `README.md`、`ARCHITECTURE.md`、`PLAN.md`，再改代码。

## 项目目标

做一个 Windows 11 本地 CPU 语音输入工具：

- 托盘常驻。
- 按住快捷键录音，松开后识别。
- 本地 ASR，不依赖云端。
- 支持多模型切换和后处理实验。
- 先做稳定可用的工具，不急着做完整 TSF/IME。

## 当前技术栈

- 前端：单文件 Win32 C++，主要在 `main.cpp`。
- 后端：Python `asr_worker.py`，使用 `sherpa-onnx` 和 `numpy`。
- 通信：本机 TCP JSON line，默认 `127.0.0.1:18088`。
- 构建：`build.bat` 调用 Visual Studio 2022 `cl` 和 `rc`。
- 模型目录：`models/`，不提交到 git。

## 开发约定

- 手工编辑文件时优先使用 `apply_patch`。
- 不要删除用户已有改动；工作区可能是 dirty 的。
- 不要把 `models/`、`build/`、`__pycache__/` 加入 git。
- 不要把模型文件打进 exe 或资源文件。
- C++ 新增依赖时同步更新：
  - `#pragma comment(lib, "...")`
  - `build.bat`
  - `CMakeLists.txt`
- Win32 include 顺序要小心：
  - `winsock2.h` 和 `ws2tcpip.h` 必须在 `windows.h` 前。
- UI 修改后必须重新编译，并尽量实际打开 Settings 看是否裁切/重叠。

## 构建

```powershell
.\build.bat
```

如果链接失败并提示不能打开 `build\VoiceLLMASRInput.exe`，通常是旧程序还在运行：

```powershell
Get-Process VoiceLLMASRInput -ErrorAction SilentlyContinue | Stop-Process -Force
.\build.bat
```

## 当前实现脉络

### `main.cpp`

负责：

- 单实例互斥。
- 托盘菜单。
- Settings 窗口和 tab UI。
- 自绘 hotkey edit 控件。
- 打开 Settings 时暂停全局键盘 hook，关闭后恢复。
- `WH_KEYBOARD_LL` 长按快捷键录音。
- `waveIn` 采集 16kHz mono PCM。
- WAV 写入 `%APPDATA%\VoiceLLMASRInput\last_recording.wav`。
- 启动/停止/重载 `asr_worker.py`。
- 通过 TCP 发送 JSON 请求到 worker。
- 收到文本后写剪贴板并发送 `Ctrl+V`。

### `asr_worker.py`

负责：

- 监听 `127.0.0.1:18088`。
- 支持 `ping`、`reload`、`shutdown`、`recognize`。
- 缓存当前 ASR recognizer，避免重复加载模型。
- 缓存标点模型。
- 根据 `model_id` 创建对应 `sherpa_onnx.OfflineRecognizer`。
- 对 `<sil>`、`<blk>` 做空文本过滤。
- `postprocess in {"itn", "punct", "llm"}` 时调用本地标点模型。

## 已知 UI 注意点

- Settings 不要再放页面大标题，标题栏已有窗口名。
- 当前 Settings 使用 tab：
  - `Recognition`
  - `Shortcut`
- 底部 `Status / Save / Close` 通过 `LayoutSettingsWindow()` 按客户区底部动态定位。
- 高 DPI 下 Win32 控件容易裁字；控件高度宁可留大一点。
- tab 字体已故意设小，避免占用空间。

## Worker 协议

请求以 UTF-8 JSON 加换行发送。每次连接处理一个请求。

示例：

```json
{
  "cmd": "recognize",
  "model_id": "firered_aed",
  "model_dir": "D:\\...\\models\\sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26",
  "threads": "4",
  "postprocess": "itn",
  "wav": "C:\\Users\\...\\last_recording.wav"
}
```

响应：

```json
{
  "ok": true,
  "text": "识别结果。",
  "model_id": "firered_aed",
  "loaded": false,
  "load_ms": 0,
  "decode_ms": 850,
  "postprocess": "punctuation",
  "punct_ms": 4,
  "punct_loaded": false
}
```

## 后续优先级建议

1. 做稳定的 Settings UI 和快捷键体验。
2. 加性能日志：load/decode/punct/total latency。
3. 补 VAD，减少长录音尾部等待和空音频识别。
4. 研究 partial：先模拟流式，再评估是否换 streaming 模型。
5. 做用户词库/术语替换。
6. 再接保守 LLM 纠错，必须默认关闭并带超时/回退。

## 不要轻易做的事

- 不要直接把 LLM 接成默认纠错，容易乱改用户意思。
- 不要让 UI 线程加载模型或等待 ASR。
- 不要在 Settings 打开时继续拦截录音快捷键。
- 不要依赖固定窗口高度放底部按钮。
- 不要为了美观牺牲控件可读性，高 DPI 下要优先留空间。
