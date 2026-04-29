# Win11 本地 CPU 语音输入法计划

## Summary
- 目标做成“托盘常驻语音输入助手”，v1 不做真正 TSF/IME 内核，先用全局热键录音、HUD 显示、最终文本粘贴到当前光标位置，风险最低、可快速可用。
- ASR 推荐主线改为 `sherpa-onnx + FireRedASR2 int8 ONNX`：官方文档支持 Windows CPU、本地离线推理，并已有 FireRedASR2 int8 模型。Qwen3-ASR 官方流式主要走 vLLM，不适合作为纯 CPU 首选；可把 `antirez/qwen-asr` 作为实验后端。
- 原 Markdown 的 Win32、WASAPI、Direct2D、DWM、托盘、热键、剪贴板注入方向保留，但把 ASR 协议和后端做成可替换。

## Key Changes
- 建立两个进程：
  - `VoiceInput.exe`：C++ Win32 前端，负责托盘、全局按键、WASAPI 录音、HUD、设置、文本注入。
  - `asr-worker`：本地 ASR 服务，监听 `127.0.0.1:18088`，只接收本机连接，负责 VAD、ASR、标点、热词和后处理。
- WebSocket 协议固定为：
  - `start`: `{ "type":"start", "sample_rate":16000, "format":"s16le", "lang":"zh-CN", "backend":"auto" }`
  - `audio`: 二进制 PCM chunk，建议 20ms 采集、100ms 批量发送。
  - `partial`: `{ "type":"partial", "text":"..." }`
  - `final`: `{ "type":"final", "text":"...", "confidence":0.0-1.0, "backend":"..." }`
  - `error`: `{ "type":"error", "code":"...", "message":"..." }`
- ASR 后端优先级：
  - 默认：`sherpa-onnx FireRedASR2 AED int8`，适合中文、英文、方言、纯 CPU。
  - 低资源备用：`sherpa-onnx streaming Paraformer zh-en int8` 或 `SenseVoice int8`，用于更低内存/更快响应机器。
  - 实验：`antirez/qwen-asr` 的 Qwen3-ASR 0.6B CPU 后端，先做离线短句和流式可行性测试，不作为默认。
  - 不推荐默认使用 Qwen3-ASR 官方 Python/vLLM 流式路径，因为官方说明流式当前只支持 vLLM，偏 GPU 服务端部署。
- LLM 纠错作为可选后处理，默认关闭；保留极保守 prompt，只允许修正明显错字、谐音、技术词大小写，不允许润色。
- 输入注入流程保留剪贴板备份/恢复，但增加降级策略：优先 `Ctrl+V`，失败时尝试 `SendInput` Unicode 文本；IME 状态切换只在目标窗口可取得 IMM context 时启用。

## Implementation Plan
- 先做 C++ 前端骨架：单实例、托盘菜单、设置 JSON、全局热键、WASAPI 16k mono pipeline、HUD RMS 波形。
- 再做 ASR worker 原型：先用 `sherpa-onnx` CLI/库验证 FireRedASR2 int8 CPU 推理，再封装 WebSocket 服务。
- 接入实时链路：按住热键启动 session，HUD 显示音量和 partial；松开后发送 stop，等待 final，再执行可选 LLM 纠错和文本注入。
- 设置项固定包含：热键、ASR backend、模型目录、线程数、是否启用 VAD、是否启用 LLM、API Base URL、API Key、LLM model、热词表。
- 打包时模型与程序分离：程序安装包小，首次启动引导选择或下载模型目录；配置保存在 `%APPDATA%\VoiceLLMASRInput\config.json`。

## Test Plan
- ASR 基准：普通话、英文夹杂、Python/JSON/API 等技术词、短句 2-10 秒、长句 30-60 秒、安静/键盘噪声场景。
- 性能门槛：默认后端在 16GB RAM、普通 x64 CPU 上可用；按键松开后 final 目标延迟小于 1.5 秒，内存目标小于 3GB，低资源后端小于 1GB。
- Windows 行为：Win11 多显示器、DPI 缩放、前台 UWP/Win32/浏览器/管理员窗口、中文输入法开启状态、剪贴板含文本/图片/空内容。
- 稳定性：ASR worker 崩溃自动重连；模型缺失给设置页错误；网络 LLM 超时不阻塞最终输入；热键被占用时提示换键。

## Assumptions And Sources
- 默认用户主要输入中文，偶尔中英混杂和技术词；机器目标为 Win11 x64、纯 CPU、16GB RAM 起步。
- FireRedASR2S 官方强调中文、英文、20+ 中文方言和 VAD/Punc 能力；但原仓库是 Python/PyTorch 风格，不是最轻的 Windows CPU 嵌入方案：[FireRedASR2S](https://github.com/FireRedTeam/FireRedASR2S)。
- sherpa-onnx 官方说明本地离线、ONNX Runtime、Windows CPU 构建，并提供 FireRedASR2 int8 ONNX 模型：[sherpa-onnx](https://k2-fsa.github.io/sherpa/onnx/index.html)、[FireRedASR2 ONNX](https://k2-fsa.github.io/sherpa/onnx/FireRedAsr/pretrained.html)。
- Qwen3-ASR 官方说明流式当前只支持 vLLM；所以纯 CPU 桌面输入法不把它作为默认：[Qwen3-ASR](https://github.com/QwenLM/Qwen3-ASR)。
- `antirez/qwen-asr` 提供 Qwen3-ASR 的 C 语言 CPU 推理和 0.6B/1.7B 支持，但较新，适合作为实验后端：[antirez/qwen-asr](https://github.com/antirez/qwen-asr)。
