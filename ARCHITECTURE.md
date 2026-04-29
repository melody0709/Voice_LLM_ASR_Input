# 架构说明

本文描述当前实现，而不是最终理想设计。更长期的研究计划见 `PLAN.md`。

## 总览

```mermaid
flowchart LR
    User["用户长按快捷键"] --> Frontend["VoiceLLMASRInput.exe<br/>Win32 托盘前端"]
    Frontend --> Recorder["waveIn 录音<br/>16kHz mono PCM"]
    Recorder --> Wav["last_recording.wav"]
    Frontend --> Worker["asr_worker.py<br/>127.0.0.1:18088"]
    Wav --> Worker
    Worker --> VAD["Silero VAD<br/>trim/no speech"]
    VAD --> ASR["sherpa-onnx ASR<br/>FireRed/SenseVoice"]
    ASR --> Punct["CT-Transformer 标点"]
    Punct --> Worker
    Worker --> Frontend
    Frontend --> Inject["剪贴板 + Ctrl+V"]
```

## 进程

### `VoiceLLMASRInput.exe`

主进程。职责：

- 单实例运行。
- 注册托盘图标。
- 显示 Settings。
- 监听全局快捷键。
- 采集麦克风音频。
- 管理 ASR worker 生命周期。
- 向 worker 发送识别请求。
- 将最终文本注入当前应用。

### `asr_worker.py`

子进程。由 C++ 主进程隐藏启动。职责：

- 监听本机 TCP。
- 加载并缓存当前 ASR 模型。
- 加载并缓存标点模型。
- 读取 WAV。
- 返回识别文本和耗时信息。

worker 的存在是为了避免每次录音都重新启动 Python 和加载 ONNX 模型。第一次使用某个模型仍然需要加载，后续同模型会复用。

## 主要模块

### 托盘和主窗口

主窗口是隐藏 Win32 窗口，用来接收托盘消息、菜单命令和 worker 结果。

托盘菜单：

- `Settings...`
- `Reload ASR Worker`
- `Quit`

### Settings

Settings 是普通 Win32 窗口，目前分两个 tab：

- `Recognition`: 模型、模型目录、线程、VAD、Partial、Postprocess。
- `Shortcut`: 长按录音快捷键，默认 `CapsLock`。

打开 Settings 时：

1. 调用 `UninstallKeyboardHook()` 暂停全局快捷键监听。
2. 用户可以录入 `CapsLock` 或其他组合键。
3. 关闭窗口时调用 `InstallKeyboardHook()` 恢复监听。

底部 `Status / Save / Close` 由 `LayoutSettingsWindow()` 根据客户区高度动态定位，避免裁切。

### 快捷键监听

使用 `WH_KEYBOARD_LL`。

普通热键当前行为：

- `WM_KEYDOWN` / `WM_SYSKEYDOWN`: 开始录音。
- `WM_KEYUP` / `WM_SYSKEYUP`: 停止录音并提交 ASR。
- 匹配配置中的主键和修饰键。
- 录音期间保存 `g_activeHotkeyKey`，避免松开主键时因修饰键已释放导致无法停止。

`CapsLock` 是特殊默认热键：

- 物理 `CapsLock` 按下时先拦截，不立即触发系统大小写切换。
- 300ms 内松开视为短按，程序补发一次 `CapsLock`，让系统正常切换大小写。
- 按住超过 300ms 视为长按，开始录音；松开后停止录音并恢复按下前的 Caps Lock 状态。
- 补发的 `CapsLock` 注入事件会被 hook 放行，避免递归拦截。

### 录音

当前使用 `waveIn`：

- 采样率：16000
- 声道：mono
- 位深：16-bit PCM
- buffer：4 个约 100ms buffer

停止录音后写入：

```text
%APPDATA%\VoiceLLMASRInput\last_recording.wav
```

短于约 8000 bytes 的录音会被判定为 `Too short`。

### VAD

`Enable VAD` 开启时，worker 在 ASR 前运行 Silero VAD：

```text
models/silero_vad.int8.onnx
```

当前策略：

- 只处理 16kHz 音频。
- 检出语音段后，只使用第一段开始到最后一段结束的包围区间，并保留 padding。
- 中间停顿不会被删除，避免长句被裁碎导致漏字。
- 如果长录音中 VAD 检出的语音占比异常小，会自动回退使用原始音频。
- 没检测到语音时直接返回空文本，不加载 ASR 模型。
- 响应里包含 `vad_ms`、`vad_segments`、`speech_ms`，用于性能测试。

### Worker 通信

C++ 使用 Winsock 连接：

```text
127.0.0.1:18088
```

协议是 JSON line：一个连接发送一条 JSON，以 `\n` 结尾；worker 返回一条 JSON，以 `\n` 结尾。

支持命令：

- `ping`
- `reload`
- `shutdown`
- `recognize`

`reload` 会清空 ASR 模型缓存和标点模型缓存。

### 模型适配

`asr_worker.py` 根据 `model_id` 创建不同 recognizer：

| model_id | 模型 | 文件 |
| --- | --- | --- |
| `firered_ctc` | FireRedASR2 CTC int8 | `model.int8.onnx`, `tokens.txt` |
| `firered_aed` | FireRedASR2 AED int8 | `encoder.int8.onnx`, `decoder.int8.onnx`, `tokens.txt` |
| `sensevoice` | SenseVoiceSmall int8 | `model.int8.onnx`, `tokens.txt` |

当前只缓存一个 ASR recognizer。切换模型后会加载新模型。

### 标点后处理

FireRedASR2 AED/CTC 输出常常没有标点，因此 worker 增加本地标点模型：

```text
models/sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8/model.int8.onnx
```

当请求中的 `postprocess` 为 `itn`、`punct` 或 `llm` 时启用。当前 `llm` 还没有真正接 LLM，只复用本地标点。

### 文本注入

当前实现：

1. 打开剪贴板。
2. 写入 `CF_UNICODETEXT`。
3. 发送 `Ctrl+V`。

后续可以补：

- 粘贴后恢复原剪贴板。
- 注入失败时 fallback 到 Unicode `SendInput`。
- 管理员权限窗口检测和提示。

## 配置

配置保存到：

```text
%APPDATA%\VoiceLLMASRInput\config.json
```

当前结构是扁平 JSON：

```json
{
  "model_id": "firered_aed",
  "model_dir": "D:\\...\\models\\sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26",
  "threads": "4",
  "enable_vad": true,
  "enable_partial": true,
  "postprocess": "itn",
  "hotkey": "CapsLock"
}
```

`PLAN.md` 中有更完整的未来配置结构，但当前代码使用上面的 v1 扁平结构。

## 后续架构演进

### 真实流式

当前是“录完再识别”。要接近 macOS 参考项目的体验，需要改为：

```mermaid
flowchart LR
    Mic["麦克风 PCM chunk"] --> Worker["Streaming worker"]
    Worker --> Partial["partial result"]
    Worker --> Final["final result"]
    Partial --> HUD["HUD 实时显示"]
    Final --> Inject["最终文本注入"]
```

可能路线：

- 继续用 offline 模型做模拟 partial。
- 更换/新增 streaming ASR 模型。
- worker 协议升级为 WebSocket 或长连接二进制流。

### 保守纠错

建议顺序：

1. 用户词库/术语替换。
2. 中文拼写纠错模型。
3. 可选本地小 LLM。

LLM 必须默认关闭，并加入：

- 超时。
- 改动比例限制。
- JSON 输出校验。
- 数字、路径、URL、代码保护。
- 失败直接使用 ASR 原文。

### Worker 形态

当前 Python worker 适合快速实验。稳定后可以考虑：

- 继续 Python，但增加日志和模型下载管理。
- C++ worker 直接链接 sherpa-onnx，减少 Python 依赖。
- Rust/C++ service，前端保持薄 UI。

## 风险点

- Win32 UI 在高 DPI 下容易裁切文字，布局要留足高度。
- 模型加载必须永远在 worker 里，不能阻塞 UI 线程。
- 模型大，内存占用需要实测。
- 全局快捷键不能在 Settings 打开时拦截用户录入。
- 剪贴板注入对部分高权限窗口可能失败。
- 标点模型会改断句，但不能修正 ASR 错字。
