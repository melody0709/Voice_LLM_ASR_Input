# 云端 ASR 架构重构验收记录

> 状态：基本完工，后续优化暂缓。
>
> 当前重点不是继续大搬家，而是确认 Qwen / 火山 / 百度在新架构下稳定运行。本文保留最终架构、火山引擎深度审查结论、已验证项和少量残余风险。

---

## 1. 当前架构

### Batch ASR

适用：本地、百度、Qwen batch fallback。

```text
StopAudioCapture()
-> CreateBatchAsrSession()
-> Finish()
-> DispatchAsrFinalText()
```

现状：

- 本地、百度、Qwen batch fallback 走 `IAsrSession` / `BatchAsrSessionBase`。
- 百度已接入 `BatchVadTrimmer`，上传前只裁剪头尾静音，不裁剪中间停顿。
- 百度已接入公共 HTTP retry：transient 网络错误会重发同一段 PCM；token/auth 错误会清 token 后再试一次。

### Streaming ASR

适用：Qwen true streaming、火山引擎。

```text
StartRecordingSession()
-> Create*StreamingSession()
-> optional StreamingVadTrimmer
-> audio callback -> EnqueuePcmChunk()
-> StopInput()
-> session worker final / retry / dispatch
```

现状：

- Qwen / 火山都走 `IStreamingAsrSession`。
- Qwen / 火山共用 `StreamingAsrSessionBase`：status HUD、partial callback、final dispatch。
- Qwen / 火山共用 `PendingPcmBuffer`。
- Qwen / 火山共用 `StreamingVadTrimmer`，内部使用 `VadTrimCore`。
- streaming VAD 与 batch VAD 共用同一个头尾裁剪状态机：保留中间停顿，只裁剪真正的开头和结尾静音。

---

## 2. 火山引擎最终审查

### 结论

火山引擎重构后的流程与重构前方案基本一致。协议层没有被改写，主要变化是把原来散在 `main.cpp`、`wasapi_capture.cpp`、`engine.cpp` 里的外层状态收进 session 和公共 VAD 管线。

没有发现会改变火山服务端协议、三模式语义、final/retry 语义或连接复用生命周期的阻断问题。

### 协议层

- 当前 `src/asr/volcengine_asr.h` 与重构前 `src/volcengine_asr.h` 内容哈希一致。
- `OpenSession()` / `SendAudio()` / `ReceiveResult()` / `DrainReceiveBuffer()` / `CloseSession()` / `PrewarmConnection()` / `ClosePersistentConnection()` 行为未改。
- WebSocket frame 编解码、三模式参数、extra params、corpus、context JSON 写入仍由原协议层负责。

### 外层流程对照

| 旧流程 | 新流程 | 审查结论 |
| --- | --- | --- |
| `g_volcThread` worker lambda 在 `main.cpp` | `VolcengineStreamingSession::WorkerLoop()` | 逻辑搬迁，流程一致 |
| `g_volcPendingAudio + g_volcAudioCs` | `PendingPcmBuffer pendingAudio_` | 线程安全入队/取出语义一致 |
| `g_volcStreaming` 控制停止输入 | `streaming_` + `StopInput()` | 语义一致 |
| `kVolcWatchdogTimer` | `kStreamingWatchdogTimer` | timer 统一，火山 timeout 文案保持 `ASR failed: VolcEngine timeout` |
| `BuildVolcContextJson()` | session 内 `BuildContextJson()` | 拼装逻辑一致，并加了 mutex 保护 history |
| `RetryVolcRecognitionOnce()` | session 内 `RetryRecognitionOnce()` | 3 次 open、6400 bytes chunk、final drain 保持一致 |
| `g_volcRecognitionHistory` | session 文件内全局 + mutex | 保留跨 session history，过滤逻辑走 `IsUsableAsrTextForContext()` |
| `g_volcSession` 全局 | 仍保留全局 | 正确，保护 `hSession/hConnect` 预热和跨 session 复用 |

### 三模式行为

仍保持旧逻辑：

- `bigmodel_nostream`：中间 chunk 不主动取 final，依赖 drain thread/final drain。
- `bigmodel_async`：drain thread 读取 partial/final。
- `standard`：发送 chunk 后同步 `ReceiveResult()`，结束时 `CloseSession()`。

### VAD 行为

旧版火山 VAD：

- 在 WASAPI 回调里用火山专属 `g_volcVadState` 状态机裁剪。
- waveIn 路径没有火山专属 VAD trim，只直写 pending audio。

新版公共 VAD：

- WASAPI 和 waveIn 都可以走 `StreamingVadTrimmer`。
- Qwen 和火山共用同一套 streaming VAD。
- 仍是头尾裁剪，不裁剪中间停顿。
- 状态机参数保持旧策略：前置保留 20 块、尾部静音阈值 20 块。
- `DetectedSpeech()` 替代旧 `g_volcVadState == 0 && g_volcVadDoTrim` 做 no-speech 判断。

这是一个实现方式变化，但用户可见语义是等价或更完整：waveIn 现在也能使用公共 streaming VAD。

### 连接复用

保留旧设计：

- `g_volcSession` 仍是全局。
- 启动时仍调用 `volc_asr::PrewarmConnection(g_volcSession)`。
- 退出时仍调用 `volc_asr::ClosePersistentConnection(g_volcSession)`。
- `CloseSession()` 仍按 `g_volcKeepAlive` 决定是否保留 `hSession/hConnect`。

这点不能随便再收进普通 session，否则会破坏 Settings 保存后的预热连接。

---

## 3. 已验证状态

### Qwen

- 正常识别已验证。
- partial HUD 已验证。
- 断网错误已验证，例如 `Qwen ASR error: WinHttpSendRequest failed (err=12007)`。

### 火山引擎

- `bigmodel_nostream`、`bigmodel_async`、`standard` 基本可用。
- partial HUD 正常。
- watchdog/timeout、empty final retry 使用中暂未见明显问题。
- VAD trim 开启后已验证生效，例如 `55KB -> 41KB (74%)`。
- 公共 VAD 后曾出现的 no-speech 误判、HUD 灰色和音频条不动问题已修复。

### 百度

- 已完成 batch VAD、同 PCM retry、token refresh retry。
- 建议后续有空再人工验证：正常识别、VAD trim、Test Connection、断网 retry、token 失效 retry。

---

## 4. 当前残余风险

- 本次审查运行 `build.bat` 时，编译阶段已完成，但链接失败：`build\VoxType.exe` 正在运行并占用文件。没有强杀用户当前 VoxType 进程。
- `VolcengineStreamingSession` 仍依赖全局 `g_volcSession` 和 `g_streamingVadTrimmer`。这是当前为了连接复用和公共 VAD 生命周期保留的设计，不是立即问题。
- `volcengine_asr.h` 仍然很大，可以后续机械拆薄，但不要在没有强需求时改协议行为。
- `settings.cpp` 仍很大，后续 UI 维护成本高，但和本轮火山稳定性无关。

---

## 5. 后续优化 Backlog

这些不是当前必须做的任务，只是保留路线图，方便以后慢慢推进。

### P0：回归验证和日志基线

- 火山三模式继续多跑真实场景：`bigmodel_nostream`、`bigmodel_async`、`standard`。
- 火山 Debug Mode 继续观察关键日志：`OpenSession`、`SendAudio`、`ReceiveResult`、final drain、empty final retry、Prewarm/reuse。
- Qwen 继续补测：VAD 开/关、missing key、错误 key、final timeout、empty final retry。
- 百度继续补测：正常识别、VAD trim、Test Connection、断网 retry、token 失效 retry。

### P1：拆薄大文件

- `src/asr/baidu_asr.h`：拆成声明头 + `.cpp` 实现，保留现有 HTTP retry 和 token retry 行为。
- `src/asr/volcengine_asr.h`：只做机械拆分，不重写协议行为。可按 frame codec、session client、test connection、config/json helper 拆。
- `src/ui/settings.cpp`：按 Recognition / Cloud ASR / LLM / VAD 页面拆分，降低 UI 维护成本。
- `src/app/main.cpp`：继续抽 recording controller、debug reporter、streaming watchdog helper。

### P2：配置模型迁移

- 保留 `asrBackend = local | baidu | volcengine | qwen` 作为当前选择字段。
- 新增 provider map / provider json 保存云端 ASR 配置。
- `LoadConfig()` 兼容旧字段并迁移。
- `SaveConfig()` 过渡期同时保存新旧字段。
- Settings 先半动态，不要一次性 schema 化火山高级参数。

### P3：流式外壳继续收敛

- `main.cpp` 中 Qwen / 火山启动分支可以再抽 `StartStreamingAsrSession()` helper。
- watchdog reset、timeout 文案、HUD status 可继续收进小 helper。
- debug 输出可以继续统一云端 pipeline / VAD trim / retry 格式。
- 不建议把 Qwen / 火山 send loop、drain thread、retry 做成复杂模板，协议差异太大。

### P4：VAD 和音频性能

- 音频回调里仍有 `vector::insert` 和锁内入队，后续可评估 ring buffer 或分块池。
- `PendingPcmBuffer::DrainTo()` 当前用 `erase()`，对长时间录音不是最优，但目前 chunk 小、风险可控。
- `StreamingVadTrimmer` / `BatchVadTrimmer` 的 VAD 推理调用还有少量重复，后续可以再收敛，但不急。

---

## 6. 暂停继续优化

本轮目标已经达成：

- `main.cpp` 不再承载 Qwen / 火山的大段 worker 逻辑。
- 火山外层状态已收敛到 `VolcengineStreamingSession`。
- 火山 pending PCM 已收进 `PendingPcmBuffer`。
- Qwen / 火山 / 百度都已接入公共 VAD 能力。
- 火山协议层保持原样，连接预热和复用保留。

后续暂不继续推进 provider map、继续拆 `volcengine_asr.h`、继续抽象 send/drain/retry 模板。下一步更适合保持现状，多跑真实场景，等出现具体问题再定点修。
