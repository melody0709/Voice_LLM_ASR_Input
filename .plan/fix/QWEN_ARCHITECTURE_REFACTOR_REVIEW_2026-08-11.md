# Qwen 三模型架构与官方文档复审

日期：2026-08-11

## 最终结论

当前三模型不需要推倒重写，也不需要建立第四个 Qwen 后端：

- `qwen-audio-3.0-asr-flash`：HTTP batch，继续复用 `IAsrSession` / `BatchAsrSessionBase`。
- `qwen-audio-3.0-asr-flash-streaming`：DashScope `inference` WebSocket task 协议，继续复用 `IStreamingAsrSession`。
- `qwen3-asr-flash-realtime`：独立 `realtime` WebSocket session 协议，继续使用单独的 `IStreamingAsrSession` 实现。

三路的 endpoint、鉴权、音频格式、请求顺序和结果类型已经基本对应官方文档。当前不应再合并协议层，也不应把 provider 逻辑放回 `main.cpp` 或音频回调。

底层可靠性修复已完成：两个 WebSocket client 已统一使用薄异步 WinHTTP transport，取消与超时经过本地可控 WebSocket 测试；Qwen3 Manual 模式增加 55 秒客户端安全边界；日志已能关联 attempt/task/session/phase/audio bytes；三套协议已有离线回归断言。交付状态应表述为：

- 协议兼容性：通过。
- 核心输入法能力：通过。
- 可选官方能力：部分覆盖。
- WebSocket 可靠性：通过本地可控生命周期测试。

## 审查依据

本地官方文档：

- `doc/qwen/Qwen-Audio-3.0-ASR-Flash.md`
- `doc/qwen/Qwen-Audio-3.0-ASR-Flash-Streaming.md`
- `doc/qwen/提升识别准确率.md`

关键证据：

- HTTP 同步 endpoint、请求和响应：`Qwen-Audio-3.0-ASR-Flash.md:45-75,302-320`
- Audio 3 Streaming endpoint：`Qwen-Audio-3.0-ASR-Flash-Streaming.md:65-85,978-1125`
- Qwen3 Realtime endpoint、事件和 Manual 模式：`Qwen-Audio-3.0-ASR-Flash-Streaming.md:528-843,2221-2234`
- Audio 3 partial/final：`Qwen-Audio-3.0-ASR-Flash-Streaming.md:877-892`
- 连接复用规则：`Qwen-Audio-3.0-ASR-Flash-Streaming.md:3289-3306`
- HTTP/Streaming context 与 `continue-task`：`提升识别准确率.md:416-526`
- 每轮 context 400 字符、超出部分从末尾截断：`提升识别准确率.md:471-475`
- 生产重连与 heartbeat 建议：`Qwen-Audio-3.0-ASR-Flash-Streaming.md:3798-3808,3866-3868`

当前代码核对基于 2026-08-11 工作树，并已运行：

```powershell
.\build.bat --test
```

结果：主程序构建成功，`qwen_free_protocol_test` 和 `qwen_audio_json_test` 均通过。

## 三模型当前状态

| 模型 | 官方协议 | 当前实现 | 结论 |
| --- | --- | --- | --- |
| `qwen-audio-3.0-asr-flash` | HTTPS `.../api/v1/services/aigc/multimodal-generation/generation`；`X-DashScope-SSE: disable`；WAV/Data URL；同步结果 | endpoint、Bearer、WAV/Base64、`input.messages`、双层 `output.output.sentence.text` / `output.text` 兼容路径均已接入 | 核心协议通过；同步 batch 没有 partial 是正常行为 |
| `qwen-audio-3.0-asr-flash-streaming` | WSS `.../api-ws/v1/inference`；`run-task` → `task-started` → binary PCM → `finish-task` → `task-finished` | 流程、PCM16/16 kHz、context、partial/final、no-speech、bounded replay 已接入；已迁移到公共异步 WebSocket transport | 核心协议和 Abort/deadline 生命周期测试通过 |
| `qwen3-asr-flash-realtime` | WSS `.../api-ws/v1/realtime?model=...`；`session.update`、Base64 append、Manual commit、`session.finish` | 独立协议实现；partial/completed/session.finished 已区分；每次录音新建连接；已迁移到公共异步 WebSocket transport；Manual turn 在 55 秒自动结束输入 | 核心协议、长 turn 边界和 transport 生命周期测试通过 |

## 已确认正确的设计

### 路由与分层

- HTTP 模型走 batch session；两个 WebSocket 模型走各自的 streaming session。
- Audio 3 `run-task` 协议没有与 Qwen3 `session.update` 协议混用。
- 音频回调只负责采集、VAD trim 和 PCM 入队，没有承载 provider 网络协议。
- fallback、replay、no-speech、partial/final dispatch 和 Watchdog 继续复用项目公共路径。

### 北京地域与鉴权

- 三个模型可以使用同一个北京地域 API Key，前提是该 Key 已开通三个模型权限。
- 三个模型可以共享相同 Workspace 域名前缀，但 endpoint 路径不能合并。
- Audio 3 Streaming 必须使用 `/api-ws/v1/inference`。
- Qwen3 Realtime 必须使用 `/api-ws/v1/realtime`。
- 当前统一发送 `Authorization: Bearer ...` 没有问题。官方示例里的 `bearer`/`Bearer` 大小写差异不是需要拆分 header 构造的协议差异，也不是握手失败的首要嫌疑。

### partial/final 行为

- HTTP batch 不显示 partial：正常。
- Audio 3 Streaming 使用 `payload.output.sentence.sentence_end=false/true` 区分 partial/final：当前实现正确。
- Qwen3 Realtime 使用 `conversation.item.input_audio_transcription.text` 作为 partial，使用 `...completed` 作为 final：当前实现方向正确。
- 只有 JSON `session.finished` 才是 Qwen3 正常会话结束；WebSocket peer close 不是 `session.finished`。

### context

- HTTP context 位于音频消息之前，结构为 `user/input_text`。证据来自《提升识别准确率》，不是 HTTP 最小调用示例本身。
- Audio 3 Streaming context 位于 `run-task.payload.input.context`。
- 官方 context 支持列表不包含 `qwen3-asr-flash-realtime`，因此不能把 Audio 3 的 `input.context` 塞给 Qwen3。
- 当前代码已在 ASR attempt 开始时捕获一次 context 快照，并传给 HTTP/Audio 3 Streaming。
- 当前代码超过 400 字符时保留前 400 字符，与“超出部分从末尾截断”的官方描述一致。

### 连接复用

- Audio 3 Streaming 官方支持 `task-finished` 后在同一连接上发起新 `run-task`，但这只是可选性能能力。
- 当前 VoxType 每次录音新建 Audio 3 连接，没有违反协议，只是没有利用连接池降低首次握手延迟。
- Qwen3 Realtime 官方明确不支持连接复用；当前代码每次录音新建并关闭连接，没有违规。
- 火山引擎的全局预热/连接复用对象不属于 Qwen，不应据此认定 Qwen3 正在复用连接。

## 本次可靠性修复

- 修正异步 WinHTTP 调用语义：调用被接受后继续等待对应 completion callback，不再提前释放发送缓冲区或提前推进握手阶段。
- 每次 Connect 使用独立 transport state，避免 Connect/Abort 并发时覆盖取消状态或混入上一连接的 callback。
- 本地回环 WebSocket 覆盖黑洞握手 Abort、正常 101、非 101 诊断、peer close、pending Receive Abort 和异步 Send completion。
- Qwen3 Manual 模式通过 `MaxRecordingMs()` 在 55 秒自动停止输入，保守落在官方 60 秒建议之前，并继续走公共 StopInput/finalize 路径。
- Audio 3 日志关联 attempt/task/phase/audio bytes；Qwen3 日志关联 attempt/session/phase/audio bytes；不记录 Key、PCM、context 正文或 transcript。
- 离线测试覆盖 HTTP 双层响应、context 400 字符边界、Audio 3 retryability、Qwen3 请求事件与 partial/final/error 分类。
- WASAPI runtime failure 在终止当前 capture 前捕获 generation，并立即唤醒等待事件，防止失败消息丢失或跨录音误归因。

## 非阻塞证据缺口

本地三份 Markdown 直接或间接支持：

- `language_hints`
- `vocabulary_id`
- `vocabulary`
- `semantic_punctuation_enabled`
- `max_sentence_silence`
- `heartbeat`
- `special_word_filter`

本地三份 Markdown 没有出现：

- `multi_threshold_mode_enabled`
- `speech_noise_threshold`

当前代码会发送后两项，但不把它们描述成“已经由本地官方文档验证”。这是可选参数的证据快照缺口，不影响三模型核心协议与可靠性修复结论。

## 可选官方能力，不属于当前核心 bug

- Audio 3 Streaming 动态 `continue-task` 更新 context：官方《提升识别准确率》明确支持，当前单次录音只发送开始快照。
- Audio 3 Streaming `special_word_filter`：官方支持，当前 Settings/UI 未接入。
- Audio 3 Streaming 连接复用/连接池：官方支持，当前每次录音新建连接。
- Audio 3 时间戳、Qwen3 emotion：当前只消费文本。
- HTTP SSE partial：需要独立 SSE parser，不应伪装成 Audio 3 WebSocket。
- Qwen3 server VAD：官方支持；VoxType 按住说话场景选择 Manual 合法，不需要为了形式对齐强制切换。

## 可选后续工作

1. 保存 `multi_threshold_mode_enabled` / `speech_noise_threshold` 的官方参数页快照。
2. 按产品需求评估 `continue-task`、`special_word_filter` 和 Audio 3 连接池。

## 完成标准

- 黑洞网络、断网、代理失败、服务端不回复或不发 Close 时，Abort/Watchdog 都能在明确期限内完成，不阻塞 UI。
- 只有官方 final event 才提交最终文本；peer close 不会产生假 final 或假 No speech。
- 稳定配置错误只尝试一次；瞬态错误最多执行规定次数的 replay/reconnect。
- Qwen3 Manual 长 turn 有明确停止或分段策略。
- 三套协议有独立离线测试；两个 WebSocket 状态机有可控取消/超时测试。
- debug log 能通过 attempt/task/request/session ID 还原一次生命周期，且不泄露 Key、PCM、context 或 transcript。

## 本轮结果

审查要求中的底层可靠性、Qwen3 长 turn、关联日志和必要离线测试均已完成；没有改变三模型的独立协议边界，也没有把 provider 逻辑移入 `main.cpp` 或音频回调。
