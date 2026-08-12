# Qwen 三模型底层可靠性重构计划

## 目标

在不合并三种官方协议的前提下，修复 Qwen Audio HTTP、Qwen Audio 3 Streaming、Qwen3 ASR Realtime 三条路径的连接可靠性、配置迁移、重试和结果生命周期问题，并复用项目已有的输入框上下文能力。

## 官方协议基线

- `qwen-audio-3.0-asr-flash`：HTTPS multimodal-generation，同步请求可关闭 SSE；HTTP SSE 增量输出保留为后续扩展。
- `qwen-audio-3.0-asr-flash-streaming`：Workspace WSS `/api-ws/v1/inference`，`run-task -> task-started -> binary audio -> finish-task -> task-finished`。
- `qwen3-asr-flash-realtime`：Workspace WSS `/api-ws/v1/realtime?model=...`，`OpenAI-Beta: realtime=v1`，`session.update`、Base64 `input_audio_buffer.append`、`input_audio_buffer.commit`、`session.finish`。
- HTTP 上下文放在 `input.messages` 的音频消息之前；Audio 3 Streaming 上下文放在 `run-task.payload.input.context`。Qwen3 Realtime 保持其独立的 `session.update` 上下文/会话协议。

## 实施步骤

### 1. Endpoint 与配置迁移

- 增加北京 Workspace 的 legacy realtime 默认常量。
- 旧配置中仍为通用 `dashscope.aliyuncs.com` 默认值时，迁移到对应 Workspace 地址；用户明确自定义的其他域名不覆盖。
- 保存、设置页切换和运行时构建配置统一使用三个 profile 的独立 URL。
- 日志记录 profile、model、host、path 和 endpoint 迁移结果，不记录 API Key。

### 2. 共用输入上下文快照

- 新增只读的 Qwen context builder，复用 `input_context::GetInputFieldContext()` 和现有识别历史。
- 在录音开始对应的云端 worker 中只采集一次上下文，避免 UI/网络线程重复读取前台窗口。
- HTTP 按官方 `input.messages` 结构发送 `input_text` / `text` 消息；Streaming 按官方 `input.context` 发送。
- 空上下文不改变原有请求；长度和条数按官方限制裁剪。

### 3. Audio 3 Streaming 传输安全

- `Client::Connect()` 清空每次尝试的错误状态，修复重试被误判为 invalid endpoint。
- 删除无效的 WebSocket receive timeout option；使用 receive loop 的 deadline、Abort 和 handle 生命周期控制超时。
- 保持 send/receive 双工，但让 Close/Abort 等待所有 WinHTTP 操作退出；发送、接收、关闭不发生 use-after-close。
- 失败日志增加 retryable、request phase、endpoint profile 和实际 WinHTTP 错误；减少重复噪音。
- 保留 replay buffer，确保连接/发送/接收失败后从同一录音数据重试。

### 4. Qwen3 Realtime 传输安全与官方对齐

- 增加 `OpenAI-Beta: realtime=v1` 和 `modalities:["text"]`。
- 清理无效 receive timeout option，补齐 send/receive/close 的并发保护。
- 修复 watchdog 关闭 request handle 与 WinHTTP 操作并发的生命周期风险。
- Manual/VAD 仍由现有配置决定；Manual 模式继续发送 commit + session.finish。
- 发送音频时按实时 chunk 节奏限制，避免 fallback 一次性灌入导致服务端背压；超过官方 Manual 模式时长上限时明确失败并记录。

### 5. Audio 3 HTTP 对齐与边界

- 保持官方 Data URL、WAV、16 kHz PCM 转换。
- 将 Base64 请求限制调整为官方 10 MB 输入约束，并在错误中区分本地大小拒绝和服务端拒绝。
- 保留 HTTP no-speech 语义，不触发错误 fallback。
- 暂不把 HTTP SSE 强行改成 Streaming session；如后续需要 partial，单独增加 SSE parser 和测试。

### 6. 测试与验证

- 增加 profile 迁移、context JSON、HTTP request、Streaming run-task、Qwen3 session.update/header、超时/重试状态测试。
- 运行 `build.bat --test`，检查 Release 构建、运行载荷布局、离线协议测试和 Settings 多 DPI 布局。
- 使用真实北京 Workspace 做三模型连接测试；Debug 日志验证不会出现 API Key、音频或上下文敏感内容泄露。

## 完成标准

- 三个模型仍通过各自官方协议，不共享错误的消息格式。
- 旧配置不会再把 Audio 3 或 Qwen3 连接到旧通用默认域名。
- 单次网络超时不会导致第二次重试被错误判定为 endpoint 错误。
- Abort、Watchdog、Close、send、receive 在连接失败和录音切换时无句柄竞态。
- 已有输入框上下文仅在用户启用相应配置时发送，且不会改变无上下文请求行为。

## 执行结果（2026-08-11）

已完成：

- 三个 Qwen 模型仍按三个独立协议实现，路由以 `qwen_model` 为权威，避免手工配置中 transport 与模型不一致时走错后端。
- 北京 Workspace HTTP / Audio 3 Streaming / Qwen3 Realtime 默认地址、旧 `dashscope.aliyuncs.com` 配置迁移和 Settings 三套 URL 记忆已完成。
- Audio HTTP 和 Audio 3 Streaming 已接入可选的输入框上下文；录音开始对应的 worker 只采集一次，密码框、超时和读取失败不会上传。
- HTTP 使用 `input.messages`，Streaming 使用 `run-task.payload.input.context`；Qwen3 Realtime 保持独立 `session.update` 协议，不混入 Audio 3 上下文字段。
- Audio HTTP 的 Data URL 10 MiB 上限按 Base64 padding 做了保守预算，no-speech 错误继续走共享空结果路径。
- Audio 3 Streaming 握手现在显式校验 HTTP 101，并记录 status / WinHTTP error / retryable / phase；非 101 的鉴权或参数错误不会盲目重试。
- 两个 Qwen WebSocket 的 `Abort()` 不再直接关闭正在使用的 WinHTTP 句柄；接收使用受控的短超时，worker/drain 退出后再由独占 `Close()` 释放资源。
- send、receive、finish、Close 之间保留方向锁和 operation shared/exclusive 锁；重放发送使用与主流相同的实时节奏，避免服务端背压。
- Qwen3 Realtime 已补齐 `OpenAI-Beta: realtime=v1`、`modalities:["text"]`、session-ready/final 有界等待、取消检查和非重试服务端错误识别。
- WASAPI Stop/generation、运行时失败唤醒、Qwen HTTP/Streaming/Realtime debug log 和 Settings 的日志打开按钮均已保留并通过构建验证。
- 离线协议测试覆盖 profile 迁移、HTTP/Streaming context JSON、WAV/Base64、no-speech、partial/final 解析和 replay buffer 边界。

验证结果：

- `.\build.bat --test`：Release 构建、canonical runtime payload、96/144/192/288 DPI Settings 布局、`qwen_free_protocol_test`、`qwen_audio_json_test` 全部通过。
- `git diff --check`：通过。

有意保留的范围边界：

- `continue-task` 动态上下文更新尚未启用；本实现按录音开始时的输入框快照发送，避免录音过程中反复读取前台控件。
- HTTP SSE partial 尚未改成 Streaming session；HTTP 模型继续使用同步请求，避免把两套官方协议混成一个传输层。
- 真实 Workspace 的网络握手仍需在用户机器上复测；开启全局 Debug Mode 后，日志按钮可直接打开 `%TEMP%\\qwen_audio_debug.log`，不会记录 API Key、音频或上下文正文。
