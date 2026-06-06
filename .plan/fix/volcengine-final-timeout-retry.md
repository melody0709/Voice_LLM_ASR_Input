# Fix: VolcEngine 长录音 final 超时、错误误粘贴、自动重试

状态：第一阶段已完成

完成日期：2026-06-05

## 结论

这次已完成的是“先把体验兜住”的稳定性修复：

- 错误不会再被粘贴到用户输入框。
- 长录音松手后不会被录音阶段的 18s watchdog 立刻抢杀。
- final 空结果时会用本次音频自动重试一次。
- 服务端正常关闭但没有返回文本时，按无可识别语音处理，不再误报 timeout。
- 服务端正常关闭且短音频无文本时，不再自动 retry，避免重复调用浪费。
- 录音期间连接异常时，会继续缓存音频到松手，避免用户已经说过的话丢失。
- 日志增加了足够的关键节点，下次可以判断失败发生在哪个阶段。

这次没有完成的是“完整实时自愈架构”：

- 录音途中掉线后，不会立刻新建 session 并实时 catch up。
- `SendAudio` / `ReceiveResult` 还没有结构化错误状态。
- WebSocket 句柄所有权还没有重构成 generation 状态机。

## 已完成

### 1. 错误不再误粘贴

文件：`src/main.cpp`

新增 `IsOperationalAsrError`，识别以下错误：

- `ASR failed:`
- `Baidu ASR error:`
- `Baidu ASR failed:`
- `VolcEngine timeout`
- `VolcEngine connect failed`
- `VolcEngine error`
- `[VolcEngine error:`

效果：

- 错误只显示在 HUD。
- 错误不会进入 `PasteTextImeAware`。
- 错误不会进入 VolcEngine 历史上下文。
- `VolcEngine timeout` 不会再被输入到微信、编辑器或浏览器输入框。

相关日志：

```log
PasteTextImeAware: skipped operational error '...'
```

### 2. 松手后重置 finalize watchdog

文件：`src/main.cpp`

原问题：

18s watchdog 从火山线程启动时开始计时。长录音会消耗这 18 秒，导致用户松手后 final drain 只剩很短时间。

本次实现：

```text
录音中: 18s watchdog 只防线程卡死，并在仍录音时续期
松手后: 按本次 PCM 时长重新设置 finalize watchdog
```

timeout 公式：

```cpp
audioMs = pcmBytes / 32.0
finalizeTimeout = clamp(audioMs * 0.8 + 6000, 8000, 30000)
```

效果：

- 17 秒录音不会再只给 final result 约 0.7 秒。
- 长录音有更合理的最终等待窗口。

相关日志：

```log
Volc watchdog: finalize timeout reset to ...ms (recording=...ms, pcm=...)
```

### 3. 建立 replay buffer

文件：`src/main.cpp`

本次在火山发送路径中维护本次 utterance 的 replay buffer：

- 真实语音 chunk 进入 replay。
- keepalive 静音不进入 replay。
- replay 最大保留 120 秒 PCM，约 3.8MB。

效果：

- final 失败时有本次音频可重放。
- 用户不用立刻重新说一遍。
- keepalive 静音不会污染重试音频。

相关日志：

```log
Volc replay: disabled, buffer limit exceeded ...
```

### 4. 初始连接失败时录音中继续重试

文件：`src/main.cpp`

原问题：

初始 `OpenSession` 失败后只做固定 3 次 retry。

本次实现：

只要用户还在录音，就继续带 backoff 重试：

```text
500ms -> 1000ms -> 2000ms -> 3000ms -> 3000ms...
```

效果：

- 用户刚开始说话时，如果火山连接还没建好，后台会继续尝试恢复。
- 连接恢复后会继续进入正常发送流程。

相关日志：

```log
Volc thread: attempt ... failed, retrying in ...ms
Volc thread: OpenSession recovered after ... retries
```

### 5. 发送中途掉线时继续缓存到松手

文件：`src/main.cpp`

原问题：

`SendAudio` 中途失败后，worker 容易提前 break 并进入结束流程。

本次实现：

- 如果发送中途连接丢失，而用户仍在录音，继续把 `g_volcPendingAudio` drain 到 replay buffer。
- 等用户松手后，再用 replay buffer 走 final retry。

效果：

- 中途网络抖动不再直接让本次录音作废。
- 用户继续说的话会保留下来，用于松手后的重试。

相关日志：

```log
Volc thread: connection lost while recording, buffering until stop
Volc thread: buffered ... bytes after connection loss (replay=..., available=...)
```

### 6. final 空结果自动重试一次

文件：`src/main.cpp`

触发条件：

- 当前模式是 `bigmodel_nostream` 或 `bigmodel_async`。
- 原 session final 结果为空。
- replay buffer 可用且非空。
- 原 session 不是“短音频正常 close 但无文本”。

重试流程：

```text
显示 Retrying... Volcano Engine
新建 VolcEngine session
重放 replay PCM
发送 isLast
等待 final result
成功则正常粘贴
失败则只显示 ASR failed，不粘贴
```

效果：

- final timeout 或 final 空结果时，会自动尝试补救。
- 用户不需要第一时间重新说一遍。
- 如果 retry 后服务端正常 close 但仍无文本，显示 `No speech detected`，不再误报 timeout。
- 如果原 session 已经正常 close 且音频不超过 3 秒，直接显示 `No speech detected`，不再 retry。

相关日志：

```log
Volc retry: final text empty, replay available ...
Volc retry: attempt 1 started ...
Volc retry: got text ...
Volc retry: succeeded
Volc retry: closed without text
Volc retry: failed
Volc retry: skipped (server closed without text, short audio ...)
Volc retry: skipped ...
```

### 7. 故障分析日志增强

文件：`src/main.cpp`

新增关键日志后，下次如果还出错，可以判断：

- 是否是录音阶段 watchdog 还是 finalize watchdog。
- 初始连接有没有在录音期间恢复。
- 是否发生了发送中途掉线。
- 掉线后缓存了多少音频。
- replay buffer 是否可用，是否超过上限。
- final retry 有没有触发。
- retry 失败发生在 OpenSession、发送、还是 final drain。
- 错误有没有被正确拦截，没有误粘贴。

## 待优化

### 1. 完整实时自愈状态机

当前状态：未完成。

本次实现是“掉线后缓存到松手，再 final retry”。它能兜住用户已经说过的话，但还不是完整实时自愈。

目标状态：

```text
Connecting
Streaming
Reconnecting
CatchingUp
Finalizing
Done / Failed
```

价值：

- 录音途中掉线后，连接恢复即可立刻新建 session。
- 新 session 从 replay buffer 开头重放并追上当前录音位置。
- 松手后的等待时间更短。

风险：

- WinHTTP WebSocket 句柄不能随便跨线程 close/receive。
- 需要 session generation，旧 drainThread 只能访问旧句柄，新 drainThread 只能访问新句柄。

### 2. 结构化 `SendAudio` / `ReceiveResult` 结果

当前状态：未完成。

当前仍主要靠 `std::wstring` 传递结果，空字符串可能代表：

- 没有文本
- timeout
- cancelled
- close frame
- send failed
- server error

建议引入：

```cpp
enum class VolcIoStatus {
    Ok,
    Empty,
    Timeout,
    Cancelled,
    Closed,
    RecoverableError,
    FatalError
};
```

价值：

- retry 判断更准确。
- fatal 错误不会误重试。
- HUD 可以显示更准确原因。

### 3. WebSocket 句柄生命周期收归 supervisor

当前状态：未完成。

当前 `SendAudio` 失败时仍会在 helper 内 close `hWebSocket`。这对现有简单流程能工作，但不适合完整实时自愈。

目标：

- `SendAudio` 只返回状态，不负责 session 生命周期。
- session supervisor 统一处理 close、join drainThread、rebuild connection。
- 所有 close 都带 generation 和原因日志。

价值：

- 降低 WinHTTP 并发 close/receive 卡死风险。
- 为实时重连 catch up 打基础。

### 4. primary final drain 使用自适应 deadline

当前状态：未完成。

本次已重置 finalize watchdog，也给 retry 使用自适应 timeout。但原 session 的 drainThread final drain 内部仍是固定 5 秒。

可以继续优化为：

- 原 session final drain 也使用 `ComputeVolcFinalizeTimeoutMs`。
- 超过自适应 deadline 后再进入 replay retry。

价值：

- 如果服务端第 6 秒返回 final，原 session 可以直接成功，不必额外重传一次。
- 减少重复调用和云端成本。

### 5. 本地 ASR fallback

当前状态：未完成。

云端重试仍失败时，可以可选切本地 sherpa-onnx：

```text
Cloud failure fallback: Off / Local ASR
```

价值：

- 云端异常时仍可能给用户一个可用结果。

风险：

- 本地模型识别质量和云端不同，不适合默认静默开启。

### 6. Settings 中暴露高级 retry 策略

当前状态：未完成。

可选配置：

- 自动重试次数
- replay 最大时长
- final timeout 上限
- 云端失败 fallback

建议：

- 默认仍保持保守。
- 高级配置折叠到 Cloud ASR 或 Debug 区域。

### 7. 故障注入测试

当前状态：未完成。

建议增加可控测试手段：

- 模拟 OpenSession 失败。
- 模拟 SendAudio 中途失败。
- 模拟 final drain timeout。
- 模拟 replay buffer 超限。
- 模拟错误结果，确认不会粘贴。

价值：

- 后续优化不必完全依赖真实网络波动复现。
- 可以稳定验证 retry 和日志路径。

## 原始事故分析

触发日志：

```log
[15:13:53.137] === OpenSession START ===
[15:13:53.566] === OpenSession OK (total: 421ms) ===
[15:13:53.566] drainThread: started (async=0 nostream=1)
[15:13:53.775] SendAudio: 6400 bytes, isLast=0, seq=2, frame=6412 bytes, async=0 nostream=1
[15:14:10.420] Volc thread: send loop ended, chunks_sent=85, vad_state=1
[15:14:10.420] SendAudio: 0 bytes, isLast=1, seq=0, frame=8 bytes, async=0 nostream=1
[15:14:10.420] Volc thread: waiting for drainThread final drain...
[15:14:10.472] drainThread: main loop exited, doing final drain...
[15:14:11.135] Watchdog: volc thread still running after 18s, force aborting
[15:14:11.197] Volc thread: wait done, drainFinalDone=1, asyncPartial empty (781ms)
[15:14:11.200] PasteTextImeAware: starting (text=18 chars)
```

判断：

- `chunks_sent=85`，每 chunk 6400 bytes。
- 85 * 6400 = 544000 bytes。
- 16kHz 16bit mono 是 32000 bytes/s。
- 这次约发送了 17 秒音频。
- 17 秒主要花在录音和上传，不是 final drain 时间。
- final drain 实际只跑了约 0.7 秒就被 watchdog 杀掉。
- `text=18 chars` 对应 `VolcEngine timeout`，说明错误被误粘贴。

## VolcEngine 当前流程

当前默认模式是 `bigmodel_nostream`。它的用户体验看起来像“按住说话时就在云端识别”，但代码语义更接近“按住时边录边上传，松手后才等最终结果”。

```text
StartRecordingSession
  -> StartAudioCapture 已经启动
  -> g_volcStreaming = true
  -> 启动 volc worker thread
  -> worker 内 OpenSession
  -> 启动 drainThread

WASAPI capture callback
  -> 重采样到 16kHz int16 mono
  -> 可选 FireRed/Silero VAD
  -> 把通过 VAD 的 PCM 追加到 g_volcPendingAudio

volc worker thread
  -> 从 g_volcPendingAudio 取 6400 bytes chunk
  -> SendAudio(chunk, isLast=false)
  -> bigmodel_nostream 下 SendAudio 只发送，不等待文本
  -> 用户松手后 g_volcStreaming = false
  -> flush 剩余 pending audio
  -> SendAudio(empty, isLast=true)
  -> 等 drainThread final drain

drainThread
  -> 录音期间 ReceiveResult(200ms) 读服务端响应，避免 TCP receive buffer 堵满
  -> nostream 模式中间响应通常没有 text
  -> 收到 asyncDrainDone 后进入 final drain
```

关键点：

- `bigmodel_nostream` 中间 chunk 不会稳定返回可粘贴文本。
- 真正可用的最终文本通常要等 `isLast` 之后才出现。

## 验证

已运行：

```powershell
.\build.bat
```

结果：

```text
Build Success: build\VoxType.exe
```

## 修改文件

- `src/main.cpp`
- `.plan/fix/volcengine-final-timeout-retry.md`
