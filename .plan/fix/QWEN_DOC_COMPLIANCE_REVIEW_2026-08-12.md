# Qwen 三模型与官方文档符合性复审（第二轮）

日期：2026-08-12
审查依据：`doc/qwen/Qwen-Audio-3.0-ASR-Flash.md`、`doc/qwen/Qwen-Audio-3.0-ASR-Flash-Streaming.md`、`doc/qwen/提升识别准确率.md`
对照代码：`qwen_audio_http.cpp`、`qwen_audio_streaming.cpp`、`qwen_audio_streaming_session.cpp`、`qwen_asr.cpp`、`qwen_streaming_session.cpp`、`asr_session.cpp`、`qwen_audio_json.h`、`qwen_audio_profile.h`

## 总体结论

三路实现的核心协议与官方文档**逐条匹配**，2026-08-11 复审结论仍然成立。本轮未发现协议级错误；发现 2 个健壮性 bug（已识别文本可能被丢弃）和若干低优先级对齐/优化项。

## 逐模型核对

### 1. qwen-audio-3.0-asr-flash（HTTP batch）— 匹配

| 官方要求 | 代码 | 结论 |
| --- | --- | --- |
| `POST /api/v1/services/aigc/multimodal-generation/generation` | `ParseEndpoint` 强制校验该路径 | ✓ |
| `X-DashScope-SSE: disable` | `qwen_audio_http.cpp:249` | ✓ |
| `input_audio` + Data URL（`data:audio/wav;base64,`） | `BuildRequestImpl` | ✓ |
| 上限 10MB / 5 分钟 | `kMaxPcmBytes` ≈ 4.1 分钟 PCM，保守落在限额内 | ✓ |
| 响应双路径 `output.output.sentence.text` / `output.text` | `ParseResponseTextForTest` 首个 `"text"` 键即嵌套 sentence.text | ✓ |
| context：`user/input_text` 消息置于音频消息之前 | `BuildRequestImpl` 顺序正确 | ✓ |
| 每轮 context ≤400 字符、超出从末尾截断 | 保留前 400 字符 | ✓ |
| vocabulary 权重 [1,5] 或 50；超级热词 ≤50；总数 ≤2000 | `qwen_audio_json::IsValidVocabulary` 完整校验 | ✓ |
| 无语音错误 `ASR_RESPONSE_HAVE_NO_WORDS` 走 no-speech 路径 | `IsNoSpeechResponseImpl` | ✓ |

唯一偏差：官方 cURL 示例 `"sample_rate": "16000"` 为**字符串**，代码发送数字 `16000`（`BuildRequestImpl:169`）。大概率兼容，低风险。

### 2. qwen-audio-3.0-asr-flash-streaming（inference WS）— 匹配

| 官方要求 | 代码 | 结论 |
| --- | --- | --- |
| `wss://.../api-ws/v1/inference` | `ParseUrl` 强制校验 | ✓ |
| run-task：header.action/task_id/streaming=duplex；payload task_group=audio/task=asr/function=recognition | `BuildRunTaskMessageImpl` | ✓ |
| format=pcm、sample_rate=16000（数字）、二进制 PCM 帧 | `SendAudio` | ✓ |
| finish-task → task-finished 流程 | `BuildFinishTaskMessageImpl` / drain 循环 | ✓ |
| partial/final 用 `sentence_end` 区分 | `TranscriptAccumulator` | ✓ |
| task-failed 错误分类（稳定配置错误不重试，瞬态重试） | `IsRetryableTaskFailure` | ✓ |
| 连接复用规则（task-finished 后才可新 run-task、不同 task_id、失败连接不可复用、60s 空闲断开） | 当前每次录音新建连接，不违反协议（复用是可选优化） | ✓ |

证据缺口（前次已标注，本轮确认仍存在）：`multi_threshold_mode_enabled`、`speech_noise_threshold` 不在本地三份文档中；`heartbeat` 在本地文档中仅出现在 Paraformer 容错建议上下文（链接指向 Paraformer SDK 页）。

### 3. qwen3-asr-flash-realtime（realtime WS）— 匹配

| 官方要求 | 代码 | 结论 |
| --- | --- | --- |
| `wss://.../api-ws/v1/realtime?model=...` | `BuildEndpointUrl` / `ParseWebSocketUrl` 强制校验路径 | ✓ |
| 请求头 `Authorization: Bearer` + `OpenAI-Beta: realtime=v1` | 官方 raw WS Python 示例（文档 2232-2235 行）明确携带，代码一致 | ✓ |
| session.update：modalities=[text]、input_audio_format=pcm、sample_rate=16000、turn_detection=null（Manual） | `BuildSessionUpdateMessage` | ✓ |
| append（Base64）→ commit → session.finish 顺序 | `SendFinish` 先 commit 后 finish，server_vad 模式跳过 commit | ✓ |
| partial = `text` + `stash` 拼接 | `ParseMessageFrame`（官方示例 597/732 行确认） | ✓ |
| 只有 `session.finished` 是正常结束；peer close 不算 | 已实现 | ✓ |
| Manual 模式累计音频建议 ≤60s | `kQwenManualMaxRecordingMs = 55000`，保守 | ✓ |
| 官方明确不支持连接复用 | 每次录音新建连接 | ✓ |

## 发现的 Bug

### Bug 1（中）：Qwen3 realtime 已拿到的 final 文本可能被丢弃

`qwen_streaming_session.cpp` 最终等待循环（约 432-450 行）**先检查 `drainFailed` 再检查 `drainCompleted`**。时序问题：

1. `conversation.item.input_audio_transcription.completed` 到达 → `finalText` 有效；
2. 服务端未发 `session.finished` 直接断开 → drain 线程置 `drainFailed`（"peer closed before session.finished"）；
3. 等待循环先看到 `drainFailed` → `failed=true` → 触发整段 replay 重试；
4. 若重试返回**非传输错误**的空结果，走 `else if (!retryResult.transportError) { finalText.clear(); failed=false; }` —— **把已经识别成功的文本清空**，用户拿到空结果。

修复建议：
- 等待循环中先判 `drainCompleted`（已有 final 文本时，缺 `session.finished` 只当警告）；
- replay 重试失败且本地 `finalText` 非空时保留原文本，仅记录警告日志，不清空。

### Bug 2（中低）：Audio 3 streaming 同类问题

`qwen_audio_streaming_session.cpp`：`task-finished` 超时但 `TranscriptAccumulator` 已有 committed 文本（服务端在 task-finished 前已下发 `sentence_end=true` 的最终句）时，当前判 `failed` 并 replay；重试失败则 `DispatchAttempt(true, error)` 丢弃全部已识别文本。

修复建议：`finalText` 非空时把"缺 task-finished"降级为警告，直接采用已积累文本。

## 低优先级对齐/优化项

1. **HTTP `sample_rate` 改字符串** `"16000"`，与官方 cURL 示例严格一致，防服务端严格类型校验。
2. **session.update 无语言时省略 `input_audio_transcription`**：当前发送空对象 `{}`，官方示例是整体省略该字段。
3. **官方新参数未接入**（可选能力，非 bug）：
   - `disfluency_removal_enabled`（去口头禅，Streaming 文档 Go 示例出现）；
   - `special_word_filter`（敏感词过滤，Audio 3 streaming 支持）；
   - `continue-task` 动态更新 context；
   - Audio 3 连接复用（60s 空闲窗口内省握手延迟，降低首包延迟）。
4. **heartbeat 参数**：本地文档仅在 Paraformer 容错上下文提及；按住说话场景录音期间持续有音频流，价值有限，可考虑默认关闭或在 UI 弱化。

## 未变更结论

- 三模型独立协议边界不变；不合并协议层；provider 逻辑不进 `main.cpp`/音频回调。
- HTTP batch 无 partial 是正常行为；HTTP SSE partial 需要独立 SSE parser，不应伪装成 WebSocket。

## 修复状态（2026-08-12 已完成）

### Bug 1 已修复 — `qwen_streaming_session.cpp`

最终等待循环中把 `drainCompleted`/`drainSessionFinished` 检查提到 `drainFailed` 之前。已拿到 `transcription.completed` 的 final 文本后，服务端在 `session.finished` 之前断连不再被降级为失败，避免触发 replay 重试并清空有效文本。补充 `transcript_completed_without_session_finished` 警告日志。

### Bug 2 已修复 — `qwen_audio_streaming_session.cpp`

新增 `finalizePhaseError` 标记，仅在 finish-task 已发送后的 task-finished 缺失/peer-close 时置位。此时若 `TranscriptAccumulator` 已有 committed 文本，恢复采用现有文本（清 failed/error/retry 标志）而非丢弃；finalText 为空时仍走原 replay 重试路径。中流发送/接收失败（`finalizePhaseError==false`）保持原重试行为。补充 `transcript_recovered_despite_incomplete_finalize` 日志。

### 验证

- `./build.bat` 编译通过（两个修改文件均成功编译链接）。
- `./build.bat --test`：`qwen_free_protocol_test` PASS、`qwen_audio_json_test` PASS。
- 运行载荷：`build\run\x64-release\VoxType.exe`（v0.9.24）。

