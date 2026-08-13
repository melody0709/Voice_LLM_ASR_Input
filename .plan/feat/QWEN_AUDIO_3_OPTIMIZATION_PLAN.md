# Qwen Audio 3 / Settings 优化实施计划

> 状态：已实施（源码、Settings、离线协议回归已完成；真实云端 smoke 需用户 API Key）  
> 创建日期：2026-08-13  
> 适用范围：`qwen-audio-3.0-asr-flash-streaming`、Qwen Audio 3 HTTP batch 以及对应 Settings 体验  
> 研究依据：`doc/qwen/Qwen-Audio-3.0-ASR-Flash-Streaming.md`、`doc/qwen/Qwen-Audio-3.0-ASR-Flash.md`、`doc/qwen/提升识别准确率.md`

## 1. 结论先行

当前 Audio 3 Streaming 的基础接入已经可用，协议方向没有需要推倒重来的问题。首要工作不是重新设计 WebSocket，而是修正上下文长度和 Settings 语义，再以低风险方式补齐官方能力。

推荐实施顺序：

1. **P0：修正 Qwen context 的实际 200 字符上限，并消除 Language/Language hints 的误导。**
2. **P1：增加 `continue-task` 动态 context 的协议和 session 支持，但默认关闭，仅允许用户显式开启；首版限制为单次更新并做去抖。**
3. **P1：增加 Audio 3 Streaming `special_word_filter`，作为 Advanced 设置，默认空列表。**
4. **P2：实现 Audio 3 Streaming 跨录音 WebSocket 复用，严格遵守 `task-finished`、task failure 丢弃、60 秒空闲断开等规则。**
5. **P3：时间戳、Qwen3 emotion、HTTP SSE partial 作为独立后续项目，不混入本轮。**

### 1.1 已有能力：保持，不重写

以下能力已在当前代码中接入或验证，后续只补测试和边界保护：

- 初始 `input.context`；
- `vocabulary_id` 和即时 `vocabulary`；
- 即时 vocabulary 权重校验、最多 2000 项以及 weight=50 最多 50 项；
- `semantic_punctuation_enabled`、`max_sentence_silence`、`multi_threshold_mode_enabled`；
- `heartbeat`、`speech_noise_threshold`；
- Streaming 重连和 PCM replay；
- HTTP batch 与 Audio 3 WebSocket 的 transport 分离；
- Qwen3 Realtime 的 Manual + `input_audio_buffer.commit` 录音模式。

### 1.2 明确不做

- 不为了“官方对齐”强制把 Qwen3 Realtime 改成 server VAD。VoxType 的按住快捷键录音场景使用 Manual 是合理的。
- 不把 HTTP SSE partial 当成 Audio 3 WebSocket；如将来需要 HTTP 响应分段，必须另做 SSE parser。
- 不在本轮把时间戳、emotion 改造成普通文本结果字段。
- 不在 WASAPI/audio callback 中执行 UI Automation、读取焦点文本或发送网络请求。
- 不把输入框全文变成长时间历史上下文，也不自动上传未明确开启的输入内容。
- 不直接引入复杂的跨 provider 连接池或通用 send/drain/retry 模板。

## 2. 当前审查发现

### 2.1 Context 存在真实上限不一致

官方 Audio 3 每轮 context 上限为 400 字符，超出时应从末尾截取。`src/asr/qwen_context.h` 和 Settings 提示已经按 400 描述，但实际输入框读取链路在 `src/core/input_context.h` 多处使用 `TakeLastN(text, 200)`，因此 Qwen 请求通常最多拿到 200 个字符。

这会同时影响 HTTP batch 和 Streaming。修复应当是 **Qwen 专用读取上限 400**，不能把火山引擎现有 200 字符行为无条件改成 400。

### 2.2 Language 与 Language hints 的有效语义不清晰

当前默认 `qwenLanguageHints = L"zh,en,yue"`，构造请求时非空 hints 优先于 `qwenLanguage`。因此 UI 显示 `Language = Auto` 时，默认仍会发送 `zh,en,yue`；用户修改 Language 也可能看不到效果。

本计划采用兼容优先策略：第一阶段不悄悄改变已有配置的识别行为，先把 UI 改成明确表达“hints 覆盖 Language”，并显示有效发送值。若产品最终需要真正的 Auto，再以单独迁移方案把新安装默认 hints 设为空，不能和其他协议改动混在一次发布中。

### 2.3 官方能力尚未使用

- Streaming 任务执行中可用 `continue-task` 更新 context，当前只有任务开始快照。
- Audio 3 Streaming 支持 `special_word_filter`，当前没有 Config/UI 接口。
- Audio 3 Streaming/Fun-ASR 支持连接复用，当前每次录音新建连接。

上述三项主要影响识别准确率、合规控制和首包延迟，不是当前核心崩溃或协议 bug，应按阶段实施。

## 3. 总体设计原则

1. 继续使用现有 `IAsrSession` / `IStreamingAsrSession`、`StreamingAsrSessionBase`、`PendingPcmBuffer`、`asr_dispatcher` 和公共 VAD。
2. Provider 协议差异只留在 `qwen_audio_streaming.*`、`qwen_audio_streaming_session.*` 和 `qwen_audio_http.*`；`main.cpp` 只负责编排生命周期和投递快照。
3. 所有新增 Config 字段必须同步 `src/app/globals.h`、`src/audio/engine.cpp` 的 LoadConfig/SaveConfig、`src/ui/settings.cpp`。
4. 每个新增能力先有纯 JSON/配置单元测试，再做真实服务 smoke test；不能把 API Key 写入测试或日志。
5. 任何“可能上传更多用户文本”的能力都必须有显式开关、长度上限、失败降级和 Settings 隐私提示。

## 4. 分阶段实施计划

### Phase 0：建立回归基线（P0，实施前置）

目标：在修改协议和 Settings 前固定当前行为，避免把已有可用路径误判为新 bug。

工作项：

- 盘点三种 Qwen transport 的请求快照：model、endpoint、language、vocabulary、context、VAD 参数和 task 生命周期。
- 扩展 `tests/qwen_audio_json_test.cpp`，或新增同目录测试文件，覆盖已有字段的序列化/互斥规则。
- 增加配置迁移样例：无配置、旧版只有 `qwen_model/qwen_base_url`、新版完整配置、未知 transport。
- 记录当前 Settings 的 DPI/裁切基线；后续 UI 改动必须用规范运行载荷 `build/run/x64-release/VoxType.exe` 验证。

验收：旧配置仍能加载；旧版 Qwen3 Realtime 的请求和停止行为不变；测试不依赖网络密钥。

### Phase 1：修正 Qwen context 上限和有效语义（P0）

#### 1A. 读取链路

涉及：

- `src/core/input_context.h`
- `src/asr/qwen_context.h`
- `src/app/main.cpp`

实施：

- 将通用 `TakeLastN` 改为可传入上限，或新增 Qwen 专用的 `TakeLastNUnicode(text, 400)`；默认行为继续保留火山等现有消费者的 200 上限。
- 截断时至少保证 UTF-16 surrogate pair 不被拆开；控制字符、密码框、UIA 超时和读取失败的现有排除逻辑保持不变。
- 录音开始时读取一次当前焦点文本，生成不可变 snapshot 传入 Qwen session；音频回调只入队 PCM。
- 统一在 `qwen_context.h` 暴露 `kQwenContextMaxChars = 400` 和规范化函数，HTTP/Streaming 使用同一套长度和空值规则。

#### 1B. HTTP/Streaming 请求

涉及：

- `src/asr/qwen_audio_http.cpp`
- `src/asr/qwen_audio_streaming.cpp`

实施：

- `input.context` 非空时发送最多 400 字符；为空、读取失败或功能关闭时不发送该字段。
- 保持 JSON escape、历史记录过滤和“不发送密码内容”的规则。
- 不把输入框 context 自动拼进长历史；输入框文本优先，历史只作明确启用时的兜底。

#### 1C. 测试与验收

- 空字符串、恰好 400、超过 400、包含中英文/emoji/代理项、换行和 JSON 特殊字符。
- 证明 Qwen 可以收到 400，而火山现有 context 仍为 200。
- 证明 UIA 卡顿不会阻塞录音启动；超时只导致 context 缺失，不导致整次识别失败。
- Debug 日志只记录长度、来源和截断标志，不记录原文。

### Phase 2：Settings 语义和校验优化（P0/P1）

涉及：

- `src/ui/settings.cpp`
- `src/app/globals.h`
- `src/audio/engine.cpp`（仅当新增配置或迁移默认值）

#### 2A. 文案和有效值

- `Language` 改为 `Fallback language`，或明确标注 `Language hints (overrides Language)`。
- 在控件旁显示有效发送值：hints 非空时显示 `zh,en,yue`，为空时显示 `Auto`。
- 第一版保留已有 `zh,en,yue` 和用户已保存值，避免升级后识别行为突变；“真正 Auto 默认空 hints”作为独立迁移选项评估。
- `Use input field text as context` 改为 `Use focused field text as ASR context`，提示“录音开始时读取；最多 400 字符；会发送到云端”。
- `Silence ms` 改为 `Max sentence silence (ms)`。
- `Vocabulary JSON` 改为 `Inline vocabulary JSON`，并补充权重 1–5/50、weight=50 最多 50 项、与 Vocabulary ID 合并最多 2000 项的说明。
- Qwen3 Realtime 高级设置增加说明：`Manual turn detection is used for hold-to-talk recording.`

#### 2B. 显示条件和布局

- 仅对支持的 transport 显示 Audio 3 专属参数；HTTP 不显示 streaming-only 字段。
- Semantic punctuation 与 Multi-threshold 继续互斥，并在切换时立即更新 enabled 状态。
- Chunk ms=200、Max sentence silence=1300、Heartbeat 默认关闭、Speech noise threshold 仅 Streaming 生效等当前合理默认值先保持。
- 使用 `src/app/globals.h` 的 `UiStyle` 常量，不固定窗口高度；完成 100%/125%/150% DPI、窄窗口和滚动区域裁切检查。

#### 2C. 隐私与配置兼容

- API Key 继续走现有 DPAPI 保存流程。
- 所有新字段采用追加式迁移；缺失字段使用安全默认，不破坏旧 `qwen_base_url`、`qwen_model`。
- Settings 关闭时不继续拦截录音快捷键；保存后沿用现有 Reload/预加载流程。

### Phase 3：`continue-task` 动态 context（P1，可选开关）

目标：在不影响音频发送时序的前提下，让正在执行的 Audio 3 task 能接收一次或少量更新后的真实专有词/输入框内容。

涉及：

- `src/asr/qwen_audio_streaming.h/.cpp`
- `src/asr/qwen_audio_streaming_session.h/.cpp`
- `src/asr/qwen_context.h`
- `src/app/globals.h`
- `src/audio/engine.cpp`
- `src/ui/settings.cpp`
- `tests/qwen_audio_json_test.cpp` 或新增 streaming protocol test

实施步骤：

1. 在 client 中增加纯协议 helper：构造并发送 `continue-task` context frame；复用同一套 JSON escape、400 字符限制和最多近 5 轮上下文规则。
2. 在 Streaming session worker 中增加串行控制帧队列。音频发送、`continue-task`、`finish-task` 必须由同一发送线程按顺序执行，禁止 UI/audio callback 直接调用 WebSocket。
3. 增加 `qwenEnableContinueContext`（默认关闭）的 Config/UI 开关；只有用户明确开启时才允许动态更新。
4. 首版只支持“显式请求或每次录音至多一次更新”：调用方提交新的 immutable snapshot，session 去重、去抖，只发送最后一次变化。暂不做持续定时轮询焦点文本。
5. 只在收到 `task-started` 后发送更新；连接未 ready、task 已 finish、abort 或 retry 时丢弃旧 update，避免控制帧落到错误 task。
6. 动态 context 必须包含待识别的真实专有词/用户修订文本，不把窗口标题或抽象语义描述当作词表。

验收：

- 离线测试验证 `run-task`、PCM、`continue-task`、`finish-task` 的合法顺序和字段；重复 snapshot 不发送。
- 网络抖动/重连时不会并发写 WebSocket；失败后可按现有 PCM replay 策略结束或重试。
- 开关关闭时请求字节流与当前版本一致；开启但读取失败时识别仍继续。

### Phase 4：Audio 3 Streaming `special_word_filter`（P1/P2）

目标：提供受控的敏感词替换/删除能力，不让用户直接编辑整段协议 JSON。

涉及：

- `src/app/globals.h`
- `src/audio/engine.cpp`
- `src/ui/settings.cpp`
- `src/asr/qwen_audio_streaming.*`
- `tests/qwen_audio_json_test.cpp`

建议配置模型：

- `qwenSpecialWordReplaceList`：命中后替换为 `*`；
- `qwenSpecialWordEmptyList`：命中后直接删除；
- `qwenSystemReservedFilter`：是否启用系统保留词过滤；
- 列表为空时不发送 `special_word_filter`。

实施规则：

- Advanced Dialog 提供多行输入或结构化列表，自动 trim、去重、过滤空项。
- 总词条数最多 32；超限在保存前报错，不静默截断。
- 只在 `qwen-audio-3.0-asr-flash-streaming` profile 发送；Qwen3 Realtime、HTTP batch 不发送。
- JSON 结构固定为官方字段：`filter_with_signed.word_list`、`filter_with_empty.word_list`、`system_reserved_filter`。
- Settings 显示“该过滤在云端生效”的隐私/合规说明；日志只记录数量，不记录词条。

验收：

- 覆盖空列表、重复项、32/33 项、Unicode 词条、JSON escape。
- 旧配置加载后列表为空且请求不变；Streaming 服务端拒绝该参数时给出可读错误并保留原始音频重试策略。
- 高 DPI 下 Advanced Dialog 不裁切，保存/取消行为一致。

### Phase 5：Audio 3 Streaming 连接复用（P2）

目标：降低连续短录音的握手延迟，不改变识别结果和 task 语义。

适用范围：仅 `qwen-audio-3.0-asr-flash-streaming`/兼容 Audio 3 Streaming；**Qwen3 Realtime 不复用**。

涉及：

- `src/asr/qwen_audio_streaming.*`
- `src/asr/qwen_audio_streaming_session.*`
- 必要时新增 `src/asr/qwen_audio_connection_manager.*`
- `src/app/main.cpp`（只接入生命周期）
- 连接/回归测试与 Debug 日志

设计约束：

- 单 provider idle manager 即可，不做跨 provider 全局池。
- 只有收到 `task-finished` 且 task 成功时才把 WebSocket/底层连接放回 idle；task failure、协议错误、abort、鉴权失败立即丢弃。
- 每个新 task 使用唯一 `task_id`；复用前检查 model、API key、endpoint、workspace/profile 仍相同。
- Settings 修改连接身份时递增复用 epoch；正在收尾的旧 task 即使晚于保存完成，也不得重新进入 idle pool。
- session 将 active-client 清理与 idle manager ownership transfer 串行化，`Abort()` 不会持有悬空指针跨越转移窗口。
- 空闲超过官方 60 秒或进程退出时主动关闭；Settings 修改 endpoint/model/key 时使旧连接失效。
- 连接所有权集中在 manager/session，避免 UI 线程和音频回调同时关闭句柄；沿用现有 worker join 生命周期。
- 当前默认启用并保留内部 kill switch；复用只优化首包延迟，不作为准确率修复。

验收：

- 连续快速录音能复用；收到 `task-finished` 后第二个 task 正常启动。
- task failure、取消、断网、API Key 变化、超过 60 秒空闲均不会复用坏连接。
- 与“不复用”基线对比：文本、partial/final 顺序、stop/join/watchdog 行为一致。
- 日志记录连接命中/失效原因和年龄，不记录密钥或音频。

### Phase 6：后续独立能力（P3，暂不排入本轮）

- 时间戳：先定义 `AsrResult` 扩展和 UI 消费方，再决定是否暴露给用户。
- Qwen3 emotion：单独设计可选 metadata，不污染普通文本结果。
- HTTP SSE partial：实现独立 SSE parser、断行/事件聚合和取消语义；不得复用 Audio 3 WebSocket parser。

## 5. Settings 现状评价

| 设置项 | 当前判断 | 计划动作 |
|---|---|---|
| Chunk ms=200 | 合法，处于官方建议 100–300ms | 保持，补范围提示 |
| Semantic punctuation | 已接入 | 保持，与 Multi-threshold 互斥 |
| Multi-threshold | 已接入 | 保持，仅在语义断句关闭时生效 |
| Silence ms=1300 | 合法且适合短句 | 保持，改成更准确的字段名 |
| Heartbeat | 默认关闭适合短按录音 | 保持，补“长录音/网络保活”说明 |
| Speech noise threshold | 仅 Streaming 显示/生效 | 保持，补范围与风险提示 |
| Vocabulary ID + inline vocabulary | 可以同时使用 | 补 2000/50/权重限制说明 |
| Language + language hints | 有效值容易被误解 | 优先改名、显示 effective value；不立即破坏旧默认 |
| Input field context | 功能方向正确，实际只有 200 | 修正到 Qwen 400，并明确录音开始读取和云端传输 |
| Base URL/Workspace | 可用但缺少区域语义 | 增加 region/workspace 与隐私说明，不自动猜测 endpoint |
| Qwen3 VAD | Manual 合法 | 增加场景说明，不强制切 server VAD |

Settings 总体结论：控件和参数大体 OK，主要问题是“用户看到的语义”和“实际发送的有效值”不完全一致，以及 context 上限提示与实现不一致。优先修文案、effective value、校验和隐私提示，不建议为追求界面统一而新增大量开关。

## 6. 文件影响矩阵

| 阶段 | 主要文件 | 变更性质 |
|---|---|---|
| Phase 0 | `tests/*`、`.plan/feat/*` | 回归基线和协议测试 |
| Phase 1 | `src/core/input_context.h`、`src/asr/qwen_context.h`、`src/app/main.cpp`、`src/asr/qwen_audio_http.cpp`、`src/asr/qwen_audio_streaming.cpp` | Qwen 专用 400 字符 context、快照和序列化 |
| Phase 2 | `src/ui/settings.cpp`、必要时 `src/app/globals.h`/`src/audio/engine.cpp` | 文案、提示、迁移和布局 |
| Phase 3 | `src/asr/qwen_audio_streaming.*`、`src/asr/qwen_audio_streaming_session.*`、Config 三处、测试 | continue-task 控制帧与显式动态更新 |
| Phase 4 | Config 三处、`qwen_audio_streaming.*`、Settings Advanced、测试 | special_word_filter |
| Phase 5 | Qwen streaming client/session、可选 connection manager、`main.cpp` 生命周期 | 单 provider 连接复用 |
| Phase 6 | 独立后续计划 | 时间戳、emotion、HTTP SSE |

## 7. 测试与发布验收清单

### 自动化

- JSON builder/parser：context、language_hints、vocabulary、special_word_filter、continue-task。
- 配置迁移：旧配置、新配置、缺失字段、未知枚举、超限列表。
- Unicode：中文、英文、emoji、代理项、控制字符和 JSON escape。
- session 状态机：`task-started → result-generated → task-finished`，以及 abort/failure/retry。

### 手工 smoke test

- Audio 3 Streaming：短句、连续停顿、partial/final、停止录音、无语音、网络断开后重试。
- Context：Chrome/Edge、Word、记事本、无法读取的微信等应用；确认失败时不影响识别。
- Settings：100%/125%/150% DPI、窗口缩放、滚动、保存后 Reload、旧配置升级。
- 复用：连续录音、60 秒以内空闲、超过 60 秒空闲、task failure、改 API key/endpoint、退出。

### 构建规则

- 使用项目唯一权威流程 `.\build.bat`；不直接运行 `build/cmake/x64-release/VoxType.exe`。
- UI 修改后必须编译并检查 Settings 裁切/重叠；运行验证只使用 `build/run/x64-release/VoxType.exe`。
- 不修改版本号；若实施阶段产生用户可见行为变化，再按项目规则同步 README/CHANGELOG。

## 8. 兼容、回滚和开关策略

- Phase 1 的 400 字符修正和文案修正默认启用；旧 provider 的 200 字符行为不变。
- `continue-task` 默认关闭，开启后也限制为显式、最多一次更新；出现协议异常可立即关闭开关回退到初始快照。
- `special_word_filter` 默认空列表/关闭；服务端拒绝时不改变普通识别路径。
- 连接复用当前默认启用，并保留编译期 `VOXTYPE_DISABLE_QWEN_AUDIO_CONNECTION_REUSE` kill switch；真实云端 smoke 若发现回归可先关闭该宏回退到每次新建连接。
- 所有新增配置均追加式保存；删除或禁用新字段不会影响旧字段和旧 model。
- 任一阶段出现回归，优先回滚该阶段的 provider helper/session 变更，不回滚已验证的公共 ASR 架构和 VAD。

## 9. 实施顺序与完成定义

本轮已按以下顺序完成源码实施：

1. Phase 0：建立并保持 `qwen_audio_json_test` / `qwen_free_protocol_test` 基线，补充 context、过滤器和控制帧回归。
2. Phase 1：修正 Qwen context 400 和 UTF-16 安全截断，HTTP/Streaming 共用上限。
3. Phase 2：完成 Settings 文案、effective language 提示、限制校验、Advanced 隐藏控件保存链路和 DPI 布局校验。
4. Phase 3：实现显式 opt-in 的单次 `continue-task` 刷新；读取、发送均在 session worker 中串行完成。
5. Phase 4：实现 Audio 3 Streaming `special_word_filter` 的三处 Config 接线、Advanced UI、规范化和 JSON 回归。
6. Phase 5：实现仅 Audio 3 Streaming 的单 provider idle manager：成功 `task-finished` 后复用、唯一 task_id、配置变更失效、60 秒清理；Qwen3 Realtime/HTTP 不进入 manager。
7. Phase 6：时间戳、emotion、HTTP SSE partial 保持独立后续计划。

自动化验收已完成：

- `.\build.bat --test`：完整 VoxType 编译、canonical runtime layout、Settings/Qwen Advanced 96/144/192/288 DPI 布局校验通过。
- `qwen_audio_json_test.exe`：context 400、surrogate 边界、HTTP/Streaming 超长截断、`continue-task`、special filter（空/重复/交叉/32/33）通过。
- `qwen_free_protocol_test.exe`：PASS。

最近一次收尾修复后再次执行 `.\build.bat --test`：完整编译、运行载荷布局、四档 DPI 布局和两组离线协议测试继续通过。

仍需带真实 DashScope API Key 的手工验收：连续短录音复用命中、任务失败/断网、60 秒空闲、修改 API Key/endpoint，以及不同 DPI 下实际视觉裁切。上述项目不应在离线测试中伪造为已完成。
