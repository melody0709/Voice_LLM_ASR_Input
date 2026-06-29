# Changelog

> 🇬🇧 [English](../CHANGELOG.md)

## Unreleased

- 暂无。

## v0.9.6 (2026-06-29)

### 新增

- **Doubao IME fallback 目标**：Recognition tab 的 `Fallback` 选择器现在可选择 `Doubao IME (Free)`。默认 ASR 后端出现运行类失败后，会用同一段原始 PCM 发起 Doubao IME recorded request。
- **Doubao IME recorded helper**：新增 `doubao_ime_asr::RecognizeRecordedPcm()`，保留固定 20ms frame padding、`RealtimeClient::Finish()` 的 final/partial 合并、有界 recorded retry，以及凭据刷新 side effect。

### 变更

- **Doubao IME 凭据写回**：fallback recorded request 中的注册/token 刷新通过 `AsrSessionResult` 返回 side effect；主窗口在 stale-attempt 检查通过后，复用现有 `kDoubaoImeCredentialsMessage` handler 写回配置。
- **Fallback backend 支持**：`doubao_ime` 已加入 fallback 校验、Settings backend 选项、batch session 创建和云端耗时统计。Volcengine 仍暂不作为 fallback target。

### 验证

- `.\build.bat` 通过。
- `.\tools\doubao_ime_probe.bat` 已通过 recorded helper 路径验证仓库内 16kHz mono WAV（`wav_ok=1`，文本：`开放时间，早上 9 点至下午 5 点。`）。
- 自动化 Settings 冒烟已打开真实 Settings 窗口，并确认 fallback 下拉框包含 `Doubao IME (Free)`。

## v0.9.5 (2026-06-29)

### 新增

- **Fallback ASR 后端**：Recognition tab 新增 `Fallback` 选择器。默认 ASR 后端出现 timeout、网络/传输、鉴权/配置、provider 错误或本地模型加载等运行类失败时，会用同一段原始 PCM 自动重试备用 ASR。
- **Batch fallback 路径**：Local、Baidu、Qwen batch、MiMo batch 支持串行 fallback。第一版 fallback 目标支持 `Local`、`Baidu Cloud`、`Qwen ASR` 和 `MiMo ASR`。
- **Streaming primary fallback 路径**：Qwen、Volcengine、Doubao IME 的 final error 和 watchdog timeout 统一进入主窗口 attempt completion handler，再按需启动 batch fallback。

### 变更

- **Fallback 触发规则**：只对运行类失败触发 fallback；`Too short`、`No speech detected`、过期 attempt 和重复 final 不触发。
- **Fallback 启用时缩短 streaming final 等待**：松手后 primary streaming final 使用 6-12 秒预算，保留未启用 fallback 时的 8-30 秒旧逻辑；Volcengine opening-finalize guard 降为 8 秒。
- **ASR/LLM result metadata**：最终 ASR/LLM 消息携带 result config、fallback 状态、primary backend、primary error 和 attempt id，Debug Mode、LLM refinement、旧结果防串扰不再依赖当前全局配置。
- **Local fallback preload**：启动和 Reload 时，只要 primary 或 fallback 是 Local，就预加载本地模型。
- **Qwen batch VAD trim**：Qwen batch 现在复用共享 batch VAD trimmer，与 Baidu/MiMo 行为一致。

### 验证

- `.\build.bat` 通过。
- `git diff --check` 通过。

## v0.9.4 (2026-06-28)

### 新增

- **豆包输入法实验 ASR 后端**：新增非默认流式云端 ASR provider：`doubao_ime`，界面显示为 `Doubao IME (Free)`。它使用非官方豆包输入法端点，不是火山引擎官方语音协议。
- **豆包输入法客户端/session**：新增设备注册、`asr_config.app_key` token bootstrap、CNG MD5 `x-ss-stub`、WinHTTP WebSocket、手写 protobuf、Opus 20ms 帧编码、partial HUD、final 分发、replay retry、可取消 bootstrap/startup 句柄、瞬态启动重试、auth/token 凭据重置重试。
- **豆包输入法 Settings 子页**：新增 provider 选择、凭据状态、`Test Connection`、`Reset Credentials`。device id/cdid/token 以 `doubao_ime_*` 持久化，token 使用 DPAPI 加密。
- **豆包输入法诊断 probe**：新增 `tools/doubao_ime_probe.bat`，用于编译独立控制台探针，默认复用已保存的豆包输入法凭据，支持 live protocol 检查、可选 WAV 识别检查和可选 streaming 发送/drain 检查；支持 `--streaming` 和 `--fresh`。
- **Vendored Opus**：新增 `third_party/opus` 下的静态 `libopus` 1.6.1 头文件/库和 license，链接 `bcrypt.lib` 用于 CNG hash。

### 变更

- **Streaming 后端识别**：将 Qwen/火山专属判断替换为共享 streaming cloud backend helper，覆盖 Qwen、火山和豆包输入法，用于 Stop/watchdog/debug 行为；本地 streaming VAD 仅保留给 Qwen 和火山引擎。
- **豆包输入法 Test Connection**：Settings 探测现在会发送 20ms Opus `Last` 帧、结束 session、等待服务端完成，并回传凭据变更，不再只测 WebSocket 初始握手。
- **豆包输入法 partial fallback**：tray app 的 drain 线程现在会在明确 final 到来前保留最新非 final 候选文本，与参考实现一致，避免服务端只发 partial 后结束时误走空结果重试。
- **豆包输入法长录音 segment 聚合**：`result_json.results[*].text` 现在会按 result 顺序拼接，不再只保留最后一个 segment，修复长录音被服务端拆段后只粘贴最后一段的问题。
- **豆包输入法云端 VAD 分段累计**：Doubao IME 现在会跨多个 WebSocket 事件累计 final 文本，并把 partial HUD 更新成“本次录音完整预览”。录音中途的云端 VAD final 不再满足松手后的 final 等待；松手后会继续等待 `FinishSession` 之后的 final 或 `SessionFinished`，修复长录音只上屏最后一个云端分段的问题。
- **豆包输入法 partial 窗口重置处理**：长录音现在维护“已提交前缀 + 当前服务端 partial 窗口”。只有明显长度骤降才视为输入法服务清空/重启 partial 窗口；普通服务端修正只替换当前窗口，不再提交进累计文本，避免 final 出现不断增长的重复 partial。
- **豆包输入法清屏 partial HUD**：Doubao IME 的 partial 在三行正文内仍完整实时显示；超过后不再维护历史滚动或旧尾行节流模式，而是清空前文显示，只从当前最后一句重新开始。清屏后的新页会继续正常累积，直到再次超过三行正文才会再次清屏；如果单句本身过长，则从句首向后裁到能放下为止。一次录音内首次清屏后，HUD 会保持固定 4 行高度，避免重新缩回一行再展开；宽度仍为 `min(900 DIP, 75% 屏幕宽度)`。该逻辑只影响 HUD，不改变最终上屏的完整文本。
- **Streaming partial HUD 泛化**：Qwen、火山引擎和 Doubao IME 现在共用受约束的清屏 partial HUD 路径，长 partial 更新时使用同一套宽度上限、清屏分页和固定高度行为。
- **Streaming 云端预采集 VAD**：Qwen 和火山引擎现在会先初始化 streaming VAD，再 replay 预采集头部音频，使 replay 的头部音频和实时回调音频进入同一套 VAD trim 状态。
- **Pending PCM 上限**：`PendingPcmBuffer` 新增默认 120 秒 16kHz mono PCM 上限，避免 streaming 云端后端卡住或重连时无限增长内存；Qwen 和豆包输入法会将溢出作为可重试 transport failure 处理。
- **豆包输入法 Settings 竞态保护**：`Test Connection` 结果现在携带 generation id，旧后台测试结果不能在 `Reset Credentials` 或更新一次测试后覆盖凭据。
- **Streaming 云端失败状态文案**：录音中 transport 丢失时现在显示 `Buffering...` 而不是 `Reconnecting...`，因为当前策略是缓存音频并在松手后 replay，不是在同一次按住期间打开替代 WebSocket。
- **豆包输入法绕过本地 VAD**：Doubao IME 现在忽略本地 `Enable VAD`，直接上传原始 PCM 编码后的 Opus；Qwen 和火山引擎继续保留 streaming VAD trim 路径。

### 验证状态

- `.\build.bat` 已通过。
- `.\tools\doubao_ime_probe.bat` 已用保存凭据通过 live endpoint 验证（`config_credentials=1`、`protocol_ok=1`、`changed=0`），并成功识别仓库中的 16kHz mono WAV（`wav_ok=1`，文本：`开放时间，早上 9 点至下午 5 点。`）。
- `.\tools\doubao_ime_probe.bat --streaming` 已通过实时发送/drain 路径验证（`streaming_ok=1`、`partial_count=6`、`final_count=1`、`session_finished=1`，文本：`开放时间，早上 9 点至下午 5 点。`）。
- partial fallback 修复后已重新运行上述 live probe 和 `.\build.bat`，均仍通过。
- Settings generation guard、预采集 VAD replay、pending buffer 上限修复后已重新运行 `.\build.bat`、`git diff --check`、`.\tools\doubao_ime_probe.bat --streaming`，均通过；`git diff --check` 仅有既有 CRLF 提示。
- 长录音 segment 聚合修复后已重新运行 `.\build.bat`、`git diff --check`、`.\tools\doubao_ime_probe.bat --streaming`，均通过；`git diff --check` 仅有既有 CRLF 提示。
- 跨事件云端 VAD 分段累计修复后已重新运行 `.\build.bat`、`git diff --check`、`.\tools\doubao_ime_probe.bat --streaming`，均通过；`git diff --check` 仅有既有 CRLF 提示。
- 收紧 partial 窗口重置判断后已重新运行 `.\build.bat` 和 `.\tools\doubao_ime_probe.bat --streaming`，WAV 和 streaming probe 都返回预期样例文本且没有重复。
- 豆包输入法清屏 HUD 调整后已重新运行 `.\build.bat`。
- 将清屏 partial HUD 路径泛化到 Qwen、火山引擎和 Doubao IME 后，已重新运行 `.\build.bat` 和 `git diff --check`；构建通过，`git diff --check` 仅有既有 CRLF 提示。
- 自动化桌面冒烟已打开真实 Settings 窗口，切到 `Cloud ASR` -> `Doubao IME (Free)`，确认当前 DPI 下凭据/Test/Reset 控件可见，且 Settings 内 `Test Connection` 返回 OK。
- 仍待人工桌面冒烟：热键/麦克风录音、partial HUD、final 粘贴、too-short 处理、断网/服务端关闭后的 watchdog 恢复，以及额外 DPI 检查。

## v0.9.3 (2026-06-15)

### 修复

- **火山引擎快速录音丢头部音频（回归）**：云端 ASR 架构重构后，`StartRecordingSession` 改为先等待上一个 session 的 worker 线程 join，再启动 WASAPI 录音。如果旧 worker 卡在 3 秒的 `OpenSession` hard timeout 中，UI 线程阻塞 3 秒且 WASAPI 未启动，用户头几个字丢失。修复：将 `StartAudioCapture()` 移到 `AbortAndResetActiveStreamingSession()` 之前，并新增 `ReplayPreCapturedAudio()` 将积攒的头部音频补发给新 session。
- **Streaming VAD 双重处理**：WASAPI 回调在检查 `g_activeStreamingSession` 之前无条件调用 `StreamingVadTrimmer::ProcessPcm16()`。当 session 为 null（join 等待窗口期）时，VAD trimmer 状态机被推进但音频未入队。后续 `ReplayPreCapturedAudio` 用新 trimmer 处理同一段音频会导致裁剪结果不一致。修复：将 `ProcessPcm16()` 移入 `g_activeStreamingSession` 检查内。
- **火山引擎 3 秒连接过期销毁 TCP+TLS 连接池**：`EnsureConnection` 仅 3 秒不活跃就销毁 `hSession`，每次录音间隔超过 3 秒都要重新 DNS + TCP + TLS 握手。改为 300 秒（5 分钟），快速连续录音可复用连接池。
- **火山引擎 NO_PROXY 绕过系统代理**：`WinHttpOpen` 使用 `WINHTTP_ACCESS_TYPE_NO_PROXY`，绕过系统/VPN 代理设置，代理用户可能无法连接。改为 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`（无代理时行为完全一致）。
- **火山引擎 2 秒 WinHTTP 超时过于激进**：`WinHttpSetTimeouts` 为 `2000, 2000, 2000, 2000`。改为 `3000, 3000, 5000, 5000`，给 DNS/TLS 更多时间。hard timeout watchdog（`kHardTimeoutMs = 3000`）不变，UI 阻塞上限仍为 3 秒。

### 变更

- **火山引擎 activeReq 快速取消**：`VolcSession` 新增 `std::atomic<HINTERNET> activeReq` 追踪 `OpenSessionImpl` 中的 `hReq`。`Abort()` 通过 `exchange(nullptr)` 立即关闭 `activeReq`，使 `WinHttpSendRequest` 瞬间返回 `ERROR_WINHTTP_OPERATION_CANCELLED`，消除连接超时时 3 秒的 UI 冻结。`OpenSessionImpl` 全部 7 个退出路径均使用 `activeReq.exchange(nullptr)` 防止双重关闭。
- **火山引擎 g_volcSession 移入 static**：`g_volcSession` 和 `g_volcKeepAlive` 从全局作用域移入 `volcengine_streaming_session.cpp` 的 `static s_volcSession`。暴露 `VolcengineResetForNewSession()`、`VolcenginePrewarmConnection()`、`VolcengineClosePersistentConnection()`、`VolcengineForceAbortAndCloseAll()` 四个接口。`globals.h` 不再包含 `volcengine_asr.h`。
- **Qwen/火山 Stop 逻辑去重**：`StopRecordingSession` 中 Qwen 和火山两个几乎相同的分支合并为 `(qwen || volcengine)` 单一分支，HUD 文案用 `AsrBackendDisplayName()` 统一生成。
- **Qwen/火山启动顺序统一**：两个 streaming 后端统一为 `session->Start()` → `ReplayPreCapturedAudio()` → `g_activeStreamingSession = session` → `StartStreamingVadTrimmerForCloud()`。

## v0.9.2 (2026-06-13)

### 修复

- **HUD DPI 适配渲染**：HUD 常量重命名为 `*Dip` 后缀并改为 `float` 类型，明确 DIP 语义。`PositionHud()` 改用 `GetDpiForMonitor()` 取目标显示器 DPI，替代 `DpiScaleForWindow()`。底部偏移从硬编码 `48px` 改为 `DipToPx(kHudBottomMarginDip, scale)`。`CreateWindowExW` 初始尺寸按 monitor DPI 换算。新增 `WM_DPICHANGED` 处理以在 DPI 变化时重新布局。D2D render target DPI 在创建和每次绘制前通过 `SetDpi()` 同步。
- **火山引擎 WebSocket 双关竞争**：新增 `AtomicTakeWebSocket()`，使用 `InterlockedExchangePointer` 原子取走 `g_volcSession.hWebSocket` 所有权。所有关闭点（Abort、VAD no-speech、async drain）统一使用此函数，确保只有一个线程关闭句柄。
- **火山引擎 `asyncPartial` 数据竞争**：新增 `std::mutex asyncMutex` 保护 worker 和 drainThread 之间的 `asyncPartial` 读写。比较和赋值现在在同一个 `lock_guard` 作用域内。
- **Qwen `activeClient_` UAF 及句柄双关**：将 `std::atomic<RealtimeClient*>` 替换为 mutex 保护的 `SetActiveClient/ClearActiveClient/AbortActiveClient` 辅助函数，确保 UI 线程的 `Abort()` 调用不会与 worker 的 client 销毁竞争。`QwenConnection::hWebSocket` 改为 `std::atomic<HINTERNET>` 并通过 `TakeWebSocket()` 实现单所有者关闭。`connected` 改为 `std::atomic<bool>`。
- **重试路径 abort 检查**：Qwen 和火山引擎的重试条件在进入可能长时间阻塞的重试前检查 `abort_.load()`。Qwen `RetryRecognitionOnce` 在 `client.Finish()` 前也检查 `abort_`。
- **`mimo_asr.cpp/.h` 未纳入版本控制**：将之前未跟踪的文件加入 git。

## v0.9.1 (2026-06-10)

### 变更

- **MiMo ASR 后端**：新增小米 MiMo ASR（`mimo-v2.5-asr`）批量云端后端。PCM 会封装为 WAV 后发送到 OpenAI-compatible `/chat/completions` 接口，默认 Token Plan Base URL 为 `https://token-plan-ams.xiaomimimo.com/v1`。
- **云端 ASR 架构稳定**：Qwen 和火山引擎 streaming 编排已收进 `IStreamingAsrSession`，并共用 status、partial、final dispatch 外壳。
- **共享 VAD trim 管线**：新增 streaming/batch 共用 VAD trim core，Qwen、火山引擎、百度、MiMo 可复用同一套头尾静音裁剪行为，同时保留中间停顿。
- **火山引擎重构等价审查**：确认火山协议层未改写，重构后的外层流程保留三模式、final drain、empty-final retry、watchdog timeout 文案和 prewarm/reuse 生命周期。
- **百度 retry 加固**：新增 transient 失败同 PCM 重试，以及 auth/token 错误后的 token refresh retry。
- **源码目录分类**：源码已按 `src/app`、`src/asr`、`src/audio`、`src/ui`、`src/core` 分组，并同步刷新架构和审查文档。

## v0.9.0 (2026-06-09)

### 新增

- **Qwen ASR 后端**：新增 DashScope Qwen ASR Realtime WebSocket 支持，默认模型为 `qwen3-asr-flash-realtime`
- **Qwen 真实边录边发链路**：录音回调将 16k/16-bit/mono PCM 非阻塞追加到 Qwen pending queue；Qwen worker 在按住热键期间按配置 chunk 持续发送，不再等录音结束后才一次性发送
- **Qwen partial HUD**：独立 receive drain thread 解析 `conversation.item.input_audio_transcription.text`（`text + stash`），开启 partial 后可在 HUD 中实时显示中间结果
- **Qwen 稳定性层**：新增 active-client abort、录音/Finalize watchdog、连接 hard timeout、replay PCM buffer、空 final retry、timeout/failure retry，以及断连后继续缓冲到松手的 replay 机制
- **统一 ASR 架构模块**：新增 `IAsrSession`、`BatchAsrSessionBase`、`AsrResultDispatcher`、ASR 结果归一化/错误分类，以及云端 replay/finalize-timeout 公共辅助
- **Qwen Settings 子页**：新增 Qwen API Key、Base URL、Model、Language、Chunk ms 和 Test Connection 控件

### 变更

- **Qwen turn detection 固定 Manual**：产品配置现在始终使用 Manual（`turn_detection: null`），匹配按住说话/松开上屏场景。Server VAD 控件和持久化的 `qwen_turn_detection` / `qwen_vad_*` 字段已移除，因为 Server VAD 会把一次热键录音切成多个 item
- **Cloud ASR 配置扩展**：ASR Backend 和 Cloud Provider 选择器加入 `Qwen ASR (DashScope)`
- **统一 ASR final 分发**：本地、百度、Qwen fallback 和云端路径复用 final 结果归一化、no-speech 处理、LLM 门控和 raw ASR 记录策略
- **构建文件同步**：`build.bat` 和 `CMakeLists.txt` 已加入新的 ASR 架构模块和 Qwen 模块，并同步链接 `winhttp` / `crypt32`
- **文档刷新**：README、中文 README、更新日志和云端 ASR 架构文档已同步 Qwen ASR、Manual turn detection 和共享 ASR 分层

## v0.8.7 (2026-06-06)

### 新增

- **火山引擎空结果重试识别**：async/nostream 模式返回空文本且 replay PCM 可用时，自动新建会话重发全量 PCM 进行二次识别。重试 OpenSession 支持最多 3 次总尝试，带 `RebuildConnection` + 递增延迟（500ms、1000ms）
- **断连音频缓冲**：录音期间 WebSocket 断开时（`bufferUntilStop`），继续从采集线程缓冲音频直到录音停止。缓冲音频追加到 `replayPcm` 用于重试，避免音频数据丢失
- **自适应 finalize 超时**：`ComputeVolcFinalizeTimeoutMs()` 根据录音时长和 PCM 大小计算超时（`audioMs * 0.8 + 6000ms`，限制 8–30s），替代硬编码 18s。录音停止时 watchdog 定时器用此值重设
- **`IsOperationalAsrError()` 统一错误分类**：将散落的 `rfind(L"ASR failed:", 0)` / `rfind(L"VolcEngine error", 0)` 检查收拢为单一函数，同时匹配 `VolcEngine timeout`、`VolcEngine connect failed`、`[VolcEngine error:` 前缀。统一用于历史过滤、LLM 门控和 HUD 错误显示
- **`CloseVolcSessionHandles()` 辅助函数**：按序安全关闭 WebSocket、hConnect、hSession 并重置 `connected` 标志。用于重试路径和连接清理
- **火山引擎故障诊断日志**：新增自适应 finalize 超时、重试触发/跳过原因、replay buffer 上限、断连缓冲、重试成功/失败、服务端 close 但无文本、运行错误跳过粘贴等关键日志

### 变更

- **初始连接重试逻辑重构**：`goto openSessionOk` 替换为清晰的 while 循环。新增 `lastError` 提前退出（服务器拒绝凭据时无需重试）。非流式模式限制 3 次重试；流式模式允许更多。递增延迟：500/1000/2000/3000ms
- **新录音会话清除 `lastError`**：`StartRecordingSession()` 新增 `g_volcSession.lastError.clear()`，避免上次会话的残留错误阻止重试
- **录音停止时动态调整 watchdog 超时**：`StopRecordingSession()` 现在用自适应 finalize 超时重设 watchdog 定时器，而非沿用 18s 录音阶段值
- **HUD 重连提示简化**：从 "Reconnecting... (1/3)" 改为 "Reconnecting... Volcano Engine"，显示更简洁
- **无文本 close 处理明确化**：服务端正常 close 但无文本时，重试后显示 `No speech detected`，不再误报 `ASR failed: VolcEngine timeout`。短音频（≤3s）遇到 close 但无文本时跳过自动重试，避免重复云端调用
- **移除 close 后 drain 噪音日志**：drainThread 在 `ReceiveResult` 收到 close frame 并标记连接断开后，不再继续调用 `DrainReceiveBuffer()`，避免误导性的 WinHTTP 4317 日志

## v0.8.6 (2026-05-22)

### 新增

- **火山引擎连接复用**：录音结束后保留 `hSession+hConnect`，不再每次关闭。录音间隔 ≤3s 时复用现有连接，OpenSession 延迟从 ~1.8s 降到 ~0.4s。间隔超过 3s 时自动重建全新 TCP+TLS 连接
- **`RebuildConnection()` 辅助函数**：同时关闭 `hConnect` 和 `hSession`（清空 WinHTTP 连接池）并从头重建。用于内部重试和外部 reconnect 逻辑
- **`PrewarmConnection()`**：启动时预建立 `hSession+hConnect`，首次录音无需支付完整连接建立开销
- **`ClosePersistentConnection()`**：显式关闭 `hSession+hConnect`（用于后端切换和程序退出）
- **`OpenSessionImpl` 时间判断内部重试**：WebSocket 升级步骤快速失败（耗时 <2s）时自动重建连接并重试一次。超时型失败（≥2s）跳过内部重试直接返回，留时间给外部 reconnect 循环
- **递增外部重试策略**：重试延迟从固定 1500ms×2 改为递增 500/1000/2000ms×3，每次重试前调用 `RebuildConnection()` 确保干净连接

### 变更

- **EnsureConnection 过期阈值**：从 60s 降到 3s（基于实测——火山引擎服务器空闲 ~3s 后关闭 TCP 连接）。过期时同时关闭 `hConnect` 和 `hSession` 以清空 WinHTTP 连接池（只关 `hConnect` 会在池中留下死 TCP 连接，导致后续请求超时）
- **`CloseSession` 条件保留**：`g_volcKeepAlive` 为 true 时保留 `hSession+hConnect` 并更新 `lastUsedTick`；否则关闭两者
- **`main.cpp` 条件句柄清理**：no-speech 路径和 async/nostream 完成路径根据 `g_volcKeepAlive` 条件保留 `hSession+hConnect`，与 `CloseSession` 行为一致
- **所有 `OpenSessionImpl` 失败点同时关闭 `hSession`**：之前只关闭 `hConnect`，导致 WinHTTP 连接池中残留死 TCP 连接。现在所有 7 个失败点（含 init frame 错误）都同时关闭 `hConnect` 和 `hSession`
- **`hReq` 超时缩短**：`WinHttpSetTimeouts(hReq, ...)` 从 3000ms 改为 2000ms，死连接更快失败，给外部 reconnect 循环留更多时间

## v0.8.5 (2026-05-21)

### 新增

- **输入框上下文读取**：新增读取当前输入框文本作为 ASR 上下文的功能，提升识别准确率。采用分层 Fallback 方案：WM_GETTEXT（Edit 控件）→ UIA Value → TextPattern（RangeFromPoint / VisibleRanges）→ TextPattern2（GetCaretRange）→ 父元素遍历 → ElementFromPoint → MSAA（IAccessible）。200ms 超时保护 + 密码框检测
- **Debug 上下文输出**：控制台输出显示成功 Layer、耗时、窗口类名、UIA 控件类型和实际上下文文本。输入框上下文和历史记录上下文均有输出
- **`DebugPrintInputContext()` 辅助函数**：提取共享的 debug 输出代码为辅助函数，ASR 和 LLM 结果分支共用

### 变更

- **上下文逻辑重构**：输入框文本和历史记录不再同时发送——输入框文本优先；输入框文本不可用时历史记录兜底。窗口标题不再作为 context 发送（对 ASR 识别帮助极小，浪费 tokens）
- **上下文开关独立化**："Read input field context" 和 "Use history as context" 改为独立开关，不再嵌套
- **`GetForegroundWindow()` 在 UI 线程捕获**：前台窗口句柄在 UI 线程捕获后传入 UIA 工作线程，避免窗口焦点切换的竞态条件
- **`TryTextPatternVisibleRanges` 内存优化**：`GetText(-1)` 改为 `GetText(500)`，累积超过 400 字符时提前退出，避免大视口（如 Word 缩放 25%）下读取过多文本
- **`inline` 替代 `static` 用于 header-only 全局变量**：`s_triggeredWindows` 和 `s_uiaThreadRunning` 从 `static` 改为 `inline`（C++17），避免多编译单元重复定义问题
- **Settings UI 重组**：移除 "enable_accelerate" 和 "accelerate_score" 控件（实际价值极小）。"Extra Params" 上移到 Row 6。Row 9 现为 "Context" 标签 + 两个复选框一行排列，"Read input field context" 对齐 Name 输入框

### 移除

- **首字加速功能**：从火山引擎请求、配置和 UI 中移除 `enable_accelerate_text` 和 `accelerate_score`。这些参数实际价值极小

## v0.8.4 (2026-05-19)

### 新增

- **kHudUpdateMessage 线程安全 HUD 更新**: 新增自定义窗口消息 `kHudUpdateMessage`，工作线程通过向主 UI 线程投递消息更新 HUD 文本，而不是直接调用 `ShowHud`，避免与 Direct2D 渲染的线程安全问题
- **重连进度 HUD 显示**: 火山引擎重连尝试时，HUD 现在显示进度（如 "Reconnecting... (1/3)"、"Reconnecting... (2/3)"、"Reconnecting... (3/3)"），而不是通用的 "ASR failed: reconnecting..." 消息
- **连接失败 HUD 日志输出**: 火山引擎连接失败时，HUD 现在显示服务器返回的实际错误信息，帮助用户诊断 API 密钥或网络问题

### 修复

- **火山引擎重连未正确清理 WebSocket 句柄**: 重试连接前未关闭旧 WebSocket 句柄，导致多次重试时句柄泄漏。每次重连前添加 `WinHttpCloseHandle`
- **动态分配的 HUD 文本内存泄漏**: 工作线程调用 `ShowHud` 时动态分配的字符串从未释放。现在使用 `PostMessageW` + 堆分配的 `std::wstring`，由 UI 线程在消息处理中释放
- **工作线程的 HUD 更新竞态**: 火山引擎工作线程直接调用 `ShowHud` 可能与 UI 线程产生竞态，导致闪烁或文本过期。所有工作线程的 HUD 更新现在都通过 `kHudUpdateMessage`

### 变更

- **调试模式 `g_enableDebugMode` 在配置重载时同步**: `g_enableDebugMode` 现在在配置加载时立即设置（`wWinMain`、`kReloadMessage`、托盘菜单切换），无需重启即可保持一致
- **移除启动时 "ASR ready: xxx" HUD 提示**: 启动时为云端后端的 HUD 通知已被移除，因为如果用户在启动后立即按热键，可能和 `kHudHideTimer` 冲突导致 HUD 提前消失

## v0.8.3 (2026-05-18)

### 修复

- **火山引擎 no-speech 路径 drainThread.join() 阻塞 17+ 秒**（严重）：VAD 未检测到语音时，`drainThread.join()` 在 `WinHttpCloseHandle(hWebSocket)` 之前调用。由于 `WinHttpWebSocketReceive` 超时不可靠（200ms 可能实际阻塞 17+ 秒），drainThread 无法退出，直到 18 秒 watchdog 强制关闭句柄。修复：先关闭 WebSocket 句柄再 join drainThread——关闭句柄会强制 `WinHttpWebSocketReceive` 立即返回 `ERROR_WINHTTP_OPERATION_CANCELLED`
- **火山引擎正常路径同样的 join-before-close 问题**：async/nostream 正常完成路径也是先 join 再 close。同样修复——先关句柄再 join
- **Watchdog 线程句柄泄漏**：Watchdog 强制关闭句柄后没有 `g_volcThread.join()`，线程句柄处于 joinable 状态。下次 `StartRecordingSession` 调用 `g_volcThread = std::thread(...)` 时会触发 `std::terminate` 崩溃。修复：关闭句柄后添加 `g_volcThread.join()`
- **SendMessage(WM_PASTE) 可能无限阻塞 UI 线程**：`PasteTextImeAware` 使用同步 `SendMessage(focus, WM_PASTE)`——如果目标窗口挂起，UI 线程会永远阻塞。改为 `SendMessageTimeoutW` + `SMTO_ABORTIFHUNG` + 2 秒超时
- **PasteTextImeAware 阻塞时 HUD 隐藏定时器未设置**：`SetTimer(kHudHideTimer)` 在 `PasteTextImeAware` 之后调用。如果粘贴阻塞，定时器永远不会设置，HUD 一直显示。将 `SetTimer(kHudHideTimer)` 移到 `PasteTextImeAware` 之前

### 变更

- **drainThread final drain 改为循环读取（1000ms 超时）**：将单次 `ReceiveResult(3000ms)` 替换为循环 `ReceiveResult(1000ms)`（总计最多 5 秒）。更可靠地处理 nostream 模式的多包响应——每个音频包对应一个响应，最终结果可能在后面的包中
- **添加 PasteTextImeAware 调试日志**：记录开始/完成及耗时，帮助诊断粘贴相关的卡死问题

## v0.8.2 (2026-05-18)

### 修复

- **火山引擎 nostream/async 结果丢失**（严重）：发送最后一个音频包后，drainThread 在读取服务器最终响应前就被终止。新增 `drainFinalDone` 原子标志——主线程现在等待 drainThread 完成最终 drain（最多 5 秒）后再关闭 WebSocket。Final drain 使用 `ReceiveResult(3000ms)` 替代 1ms 轮询，给服务器足够时间返回结果
- **火山引擎 nostream/async 长录音截断**（严重）：录音超过 15 秒时，主线程等待条件检查 `asyncPartial.empty()`——一旦收到 partial 结果就停止等待并杀死 drainThread，导致 15 秒之后的文字丢失。等待条件改为检查 `drainFinalDone`，确保捕获完整结果
- **WinHttpCloseHandle 死锁**（严重）：主线程在 drainThread 仍阻塞于同一句柄的 `WinHttpWebSocketReceive` 时调用 `WinHttpCloseHandle(hWebSocket)`。WinHTTP 非线程安全——并发访问导致内部死锁。修复方式：在正常路径和 no-speech 路径中均先 join drainThread 再关闭 WebSocket 句柄
- **火山引擎 nostream/async 逻辑死锁**：`asyncDrainDone` 在等待 `drainFinalDone` 之后才设置，但 drainThread 主循环在 `asyncDrainDone` 为 true 时退出——形成循环等待。修复方式：将 `asyncDrainDone = true` 移到等待循环之前
- **火山引擎短音频误报超时**：短音频后服务器关闭连接，主线程等待 `drainFinalDone` 长达 5 秒，但 drainThread 卡在 `WinHttpWebSocketReceive`（WinHTTP 超时不可靠）。现在 drainThread 主循环也检查 `g_volcSession.connected`，服务器关闭时及时退出。`forceAbort` 仅在连接仍存活时（真超时）设为 true，避免误报 "VolcEngine timeout"
- **HUD kHudHideTimer 竞态条件**：上一次录音的 "No speech detected" HUD 启动隐藏定时器后，快速按热键开始新录音时，旧定时器会把新 HUD 也关掉。双重防护修复：录音开始时 `KillTimer(kHudHideTimer)`，定时器处理函数中检查 `g_recording`

## v0.8.0.1 (2026-05-17)

### 修复

- **火山引擎 nostream 无语音时卡死**：VAD 在 nostream 模式下未检测到语音时，`WinHttpWebSocketReceive` 设置 1ms 超时实际不生效（Windows WinHTTP 最小超时粒度远大于 1ms），drainThread 无限阻塞，导致 `drainThread.join()` 挂死直到 18 秒 watchdog 触发。修复方式：在 join drainThread 之前先关闭 WebSocket 句柄，强制 `WinHttpWebSocketReceive` 立即返回 `ERROR_WINHTTP_OPERATION_CANCELLED`

## v0.8.0 (2026-05-17)

### 新增

- **本地 ASR 流式 VAD**：VAD 现在在录音期间（WASAPI 采集线程中）实时运行，而非录音结束后。语音段实时收集，松开按键后直接传入 ASR，跳过冗余 VAD 步骤。支持 Silero VAD 和 FireRed VAD
- **火山引擎 ASR 流式 VAD**：FireRed VAD 在火山引擎录音期间运行，采用三状态机（PreSpeech → InSpeech → PossibleTail）进行智能音频裁剪。说话前静音缓冲并裁剪；尾部静音保持直到语音恢复或录音结束。未检测到语音时返回 "No speech detected"，不发送音频到服务器
- **FireRed VAD 流式 API**：新增 `StreamVadPostprocessor` 类，提供 `GetConcatenatedSamples()`、`HasSpeech()`、`Flush()`、`Reset()` 方法用于实时 VAD 处理。`GetConcatenatedSamples()` 合并 VAD 段并正确处理重叠
- **HUD 语音检测视觉反馈**：音量条仅在音频电平超过阈值（0.04）时动画。条形颜色仅在检测到语音时从空闲渐变切换为活跃渐变（`g_hudHasSpoken` 标志）
- **"No speech detected" HUD 显示时长**：显示 1500ms（正常结果 200ms，错误 2200ms）

### 修复

- **火山引擎 nostream/async 长录音卡死**（严重）：录音超过约 15 秒时，TCP 接收缓冲区满导致 `WinHttpWebSocketSend` 无限阻塞。修复方式：
  - async 和 nostream 模式均启动 `drainThread`（此前仅 async 有）
  - `SendAudio(isLast=true)` 在 async/nostream 模式下跳过接收，由 drainThread 处理最终响应
  - 先发送 isLast 帧再 join drainThread（此前顺序相反——先 join 停止了读取线程，导致 join 期间 TCP 缓冲区溢出）
  - 删除与 drainThread 竞争同一 WebSocket 句柄的 nostream drain 循环
- **Watchdog 录音期间误杀会话**：18 秒 watchdog 计时器从录音开始启动，但可能在用户仍在录音时触发。现在 `g_recording` 为 true 时自动续期，仅在录音结束后触发 force-abort
- **统一 `ExtractJsonStr`**：移除 `baidu_asr.h` 和 `volcengine_asr.h` 中的重复实现，合并到 `utils.h`，正确处理转义引号（通过计算连续反斜杠数量判断）
- **`PostQuitMessage(0)` 污染外层消息循环**：在 `settings.cpp` 中替换为 `IsWindow(dlg)` 检查（2 处）
- **`g_streamingVadReady`/`g_volcVadDoTrim` 线程安全**：从普通 `bool` 改为 `std::atomic<bool>`，修复跨线程访问
- **`s_lastRecvError` 线程安全**：在 `volcengine_asr.h` 中改为 `std::atomic<DWORD>`
- **`VolcDebugLog` 开销**：现在在格式化日志消息前检查 `g_enableDebugMode`，避免调试模式关闭时的不必要字符串操作
- **`ReceiveResult` 连接状态追踪**：在错误或零字节读取时设置 `sess->connected = false`，允许正确的连接状态检测
- **`AddVolcRecognitionHistory` 过滤**：现在也跳过 "No speech detected" 条目

### 变更

- **移除启动时 "ASR ready" HUD 显示**：ASR 预加载完成时的 HUD 通知与 `kHudHideTimer` 冲突，当用户启动后立即按热键时会导致 HUD 提前消失
- **本地 ASR 流式 VAD 无语音处理**：未检测到语音段时显示 "No speech detected"，而非对静音运行 ASR
- **火山引擎 ASR 提前退出**：VAD 在 PreSpeech 状态未检测到语音时提前关闭会话，不发送音频到服务器，节省 API 费用

## v0.7.5 (2026-05-15)

### 修复

- **火山引擎 WebSocket 退出时挂死**：在 `VolcSession` 中新增 `forceAbort` 原子标志。录音会话被取消时（如按 Esc、关闭窗口），设置 `forceAbort` 后所有阻塞中的 WebSocket 操作（`OpenSession`、`SendAudio`、`ReceiveResult`、`CloseSession`）立即退出，不再等待网络 I/O
- **WebSocket 关闭握手挂死**：`WebSocketCloseGracefully` 在 `forceAbort` 设置时跳过关闭握手，直接关闭句柄，避免等待可能永远不会来的服务端响应
- **过期连接复用**：`EnsureConnection` 现在检查 `lastUsedTick`，空闲超过 30 秒的连接自动重建而非复用，防止服务端超时后出现的 "connection reset" 错误
- **WinHTTP 代理设置**：`EnsureConnection` 和 `TestConnection` 中从 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` 改为 `WINHTTP_ACCESS_TYPE_NO_PROXY`。默认代理类型在未配置代理的机器上会导致不必要的 PAC/自动检测延迟

### 新增

- **连接预热**：新增 `PrewarmConnection()` 函数，在录音开始前提前建立 TCP/TLS 连接，消除首次按热键时约 1.4 秒的连接建立延迟。在设置保存后或启动时火山引擎后端被选中时调用
- **`lastUsedTick` 字段**：`VolcSession` 新增 `lastUsedTick` 字段，记录连接最后使用时间，用于自动过期和重建陈旧连接

### 变更

- **简化 OpenSession**：移除重试循环（原为 2 次尝试+连接重建）。改为单次尝试，在关键点（`SendRequest` 前、`ReceiveResult` 中）检查 `forceAbort`。连接失败时由下次 `EnsureConnection` 重建
- **连接复用策略调整**：`CloseSession` 现在每次会话后关闭 `hConnect`（仅保留 `hSession` 存活）。此前两者均保持存活，但服务端在空闲超时后会关闭 TCP 连接，导致缓存的 `hConnect` 失效
- **缩短 WinHTTP 超时**：连接/发送/接收超时从 10000ms 缩短为 5000ms（会话级）和 3000ms（请求级），加快故障检测
- **精简日志输出**：`SendAudio` 仅记录首帧（seq=2）和末帧（isLast），而非每个音频块。服务端响应日志合并为一行并附带 payload 预览
- **Nostream drain 改进**：在 nostream drain 循环中增加空帧计数和 `forceAbort` 检查，防止服务端无响应时无限等待

## v0.7.4 (2026-05-11)

### 修复

- **微信中文输入法粘贴问题**：微信（`Weixin.exe`）使用自定义 Qt 控件，`GetFocus()` 返回 NULL 且 IME 拦截 Ctrl+V。通过进程名检测微信，使用 `WM_CHAR` 逐字符发送绕过 IME；其他应用使用剪贴板 + Ctrl+V + IMM32 临时切换英文模式
- **IMM32 输入法状态切换**：添加 `ImeStateGuard` RAII 结构体，在发送 Ctrl+V 前临时切换输入法到英文模式，发送后自动恢复

### 新增

- **Unicode SendInput fallback**：添加 `SendUnicodeText()` 函数，使用 `KEYEVENTF_UNICODE` 标志逐字符发送，完全绕过 IME
- **Force Unicode Input 菜单**：托盘右键菜单新增 "Force Unicode Input" 选项，勾选后所有应用使用 Unicode SendInput 方式粘贴，用于测试兼容性
- **剪贴板注入双层策略**：`PasteTextImeAware()` 函数实现智能粘贴：优先使用 `WM_PASTE`（`GetFocus()` 有效时），微信用 `WM_CHAR`，其他应用用剪贴板 + Ctrl+V + IMM32 切换

## v0.7.3 (2026-05-11)

### 新增

- **WASAPI Shared Mode 录音**：音频输入从 MME `waveIn` 升级到 WASAPI Shared Mode + 自定义重采样。以系统混合格式（通常 48kHz/32bit float/立体声）捕获，通过线性插值重采样到 16kHz/16bit/单声道。WASAPI 初始化失败时自动 fallback 到 `waveIn`
- **Debug Mode**：托盘右键 checkbox 打开 CMD 控制台，每次识别实时打印各阶段耗时。覆盖 VAD、ASR 解码、标点、云端 API、LLM 纠错、粘贴注入。Total 不含录制时长。对应优化方案 P0 §3
- **Config 字段**：`audio_backend`（wasapi/waveIn）和 `audio_device_id`（WASAPI 设备 ID，空=默认）持久化到 config.json

### 变更

- **`HiResTimer` 移至 `engine.h`**：供跨模块复用

### 修复

- **WASAPI 生命周期 bug**：`Stop()` 仅停线程不清资源，新增 `Release()` 完整清理。缺少此步会导致第二次录音卡死
- **WASAPI 重采样相位漂移**：相位更新改为 `m_resamplePhase -= written / m_resampleRatio`，修复非整数倍采样率比下的越界问题
- **百度 ASR 响应解析**：`err_no` 字段改为整数解析；空响应/错误响应增加诊断 `printf`；`WinHttpReadData` 仅追加实际读取的字节
- **Debug 计时状态清理**：`StopRecordingSession()` 中增补 `g_vadModelName.clear()`
- **移除重复清空**：`WasapiCapture::Start()` 不再重复清空 `g_audioData`/`g_audioLevel`（`StartAudioCapture()` 已统一处理）

## v0.7.2 (2026-05-09)

### 修复

- **退出时火山引擎线程未被停止**：`WM_DESTROY` 现在设置 `g_volcStreaming = false` 并 `join` 火山引擎线程，避免主窗口关闭后线程在 `Sleep(20)` 中空转
- **百度 ASR Token 缓存数据竞争**：`GetAccessToken` 添加 `std::mutex` + `lock_guard` 保护 `s_cachedToken` / `s_tokenExpiresAt`，修复 `Recognize` 和 `TestConnection` 线程并发读写的未定义行为
- **`VolcDebugLog` 线程安全**：添加 `static std::mutex` 序列化日志文件写入和 `logPath` 初始化，防止行交错和初始化竞争
- **`g_volcAudioCs` 资源泄漏**：在 `wWinMain` 所有退出路径（正常退出、单实例早退、`RegisterWindowClasses` 失败、`CreateWindowExW` 失败）添加 `DeleteCriticalSection(&g_volcAudioCs)`
- **SSL 证书验证恢复**：移除 `baidu_asr.h`（`Recognize` 和 `TestConnection`）和 `llm_refine.h`（`SendRequestRaw`）中的 `SECURITY_FLAG_IGNORE_*` 覆盖，恢复所有云端 API 连接的正常 HTTPS 证书验证
- **`AsrEngine::lock` 封装**：`std::mutex lock` 从 public 改为 private（`lock_`），添加 `Lock()`/`Unlock()` 方法；`PreloadAsrEngine` 使用新 API 而非直接访问成员

### 变更

- **工具函数去重**：`WideToUtf8`、`Utf8ToWide`、`EscapeJson`、`Trim` 统一到新建的 `src/utils.h`；移除 `engine.cpp`、`llm_refine.h`、`baidu_asr.h`、`volcengine_asr.h` 中的 4 份副本
- **模型下载器不再阻塞 UI**：`RunModelDownloader` 改为异步启动 PowerShell，完成后通过 `WM_APP + 20` 回传 Settings；下载期间按钮禁用并显示状态文本，完成后恢复
- **托盘菜单标志清理**：移除 `MF_GRAYED` 旁冗余的 `MF_DISABLED`（前者已隐含禁用状态）
- **HotkeyEdit 绘制优化**：非捕捉状态使用 `GetSysColorBrush(COLOR_WINDOW)` 替代每次 `WM_PAINT` 创建/销毁 `CreateSolidBrush(RGB(255,255,255))`

## v0.7.1 (2026-05-09)

### 性能优化

- **`bigmodel_nostream` 加速**：跳过中间音频 chunk 的 `ReceiveResult`（非流式模式下服务端对每个 chunk 返回空文本），只在最终 `isLast` chunk 后 drain 响应。会话耗时从 ~7-8s 降至 ~1.5-2s
- **WinHTTP 连接复用**：跨录音会话保持 `hSession` + `hConnect`（TCP/TLS），第二次录音起省掉 ~1.4s TLS 握手。连接失效时自动重试

### 修复

- **`ExtractJsonStr` 转义处理**：正确处理 `\\"`（转义反斜杠+引号），通过计算连续反斜杠数量判断引号是否为真实结束符
- **`ExtractJsonBool` 空白处理**：补全 `\n`/`\r` 跳过，与 `ExtractJsonStr` 行为一致
- **线程安全**：`VolcSession::connected` 从 `volatile bool` 改为 `std::atomic<bool>`

## v0.7.0 (2026-05-09)

### 新增

- **火山引擎 ASR 全参数支持**：官方 API 的所有 `request` 和 `corpus` 字段均可在 Settings 中配置
  - `end_window_size` / `force_to_speech_time` — VAD 分句和强制判停时间
  - `enable_ddc` — 语义顺滑（去除语气词和重复词）
  - `enable_nonstream` — `bigmodel_async` 模式下开启二遍识别（流式 + 非流式重识别，准确率更高）
  - `enable_music_fc` / `enable_poi_fc` — 音乐和 POI function call
  - `enable_accelerate_text` + `accelerate_score` — 首字返回加速（v0.8.5 已移除）
  - `language` — 语言选择（仅 `bigmodel_nostream` 模式生效，符合 API 规范）
- **热词与替换词表**：Cloud ASR tab 新增 `Hotwords ID/Name` 和 `Correct ID/Name` 字段
  - `boosting_table_id` / `boosting_table_name` — 引用自学习平台热词词表
  - `correct_table_id` / `correct_table_name` — 引用替换词词表，用于专业术语纠正
- **对话上下文**：`Use history as context` 复选框将最近识别结果作为 `corpus.context` 发送，提升上下文理解准确率
  - 可配置历史条数（1–20，默认 3）
  - context 按 API 规范序列化为 JSON 字符串
- **Extra Params 对话框**：专用对话框编辑额外的 `request` 级 JSON 参数，提供 `sensitive_words_filter` 和 `result_type`/`vad_segment_duration` 预设模板
- **模型版本选择器**：新增下拉框，支持 `Seed-ASR 2.0 (duration)` / `Seed-ASR 2.0 (concurrent)` / `BigModel 1.0 (duration)` / `BigModel 1.0 (concurrent)`

- **移除首次启动模型下载对话框**：纯云端用户不再被强制弹窗下载模型
- **Download 按钮移至 ASR model 行**：更名为 "Download Local Model"，放在 ASR 模型下拉框右侧，宽度 220px
- **Cloud Provider 更名与排序调整**："Volcengine (Doubao)" → "Volcano Engine (Doubao)"，并设为默认选项；ASR Backend 下拉中 Volcano Engine 也移至 Baidu Cloud 前面
- **云端模式启动 HUD 提示**：使用云端 ASR 后端启动时，HUD 显示 "ASR ready: xxx"

### 修复

- **`corpus` 字段不再互斥**：此前 `boosting_table_id` 和 `context` 使用 `if/else if` 发送，无法同时使用热词和对话上下文。现在所有 `corpus` 子字段合并到同一个 JSON 对象中
- **`context` 字段格式修正**：`context` 的值必须是 JSON 字符串（内层引号转义），而非原始 JSON 对象。发送原始对象会导致服务端拒绝请求，客户端在第一次识别后卡死
- **`language` 参数仅在 `bigmodel_nostream` 模式下发送**：按 API 文档，`language` 字段仅 nostream 模式支持，其他模式发送可能导致错误
- **Extra Params 中 `corpus` 冲突已解决**：用户在 Extra Params 中手动填入 `corpus` 时，会与代码生成的 `corpus` 冲突产生无效 JSON。现在 Extra Params 解析时跳过 `corpus` 键
- **移除非标准 HTTP 头**：`X-Api-Request-Id` 和 `X-Api-Sequence: -1` 不在官方 API 规范中，已从 `OpenSession` 和 `TestConnection` 中移除
- **移除不安全的 SSL 标志覆盖**：`SECURITY_FLAG_IGNORE_UNKNOWN_CA` 等标志不必要地绕过了证书验证，已移除以恢复正确的 HTTPS 安全性

## v0.6.2 (2026-05-06)

### 变更

- **Settings UI 样式统一化**：在 `globals.h` 中引入 `UiStyle` 命名空间，集中管理所有布局常量，替换 `settings.cpp` 和 `hud.cpp` 中散落的魔数
  - 行间距统一为 52px（Recognition、LLM、LLM Prompt、Cloud ASR 所有 tab）——此前 Cloud ASR 仅 36px（过紧），LLM 为 46-64px（不均匀）
  - `RowInputY(row)` / `RowLabelY(row)` 辅助函数自动计算第 N 行的 Y 坐标
  - 所有控件尺寸（高度、宽度）定义为命名常量（`EditH`、`BtnH`、`ComboW` 等）
  - 所有颜色值（`BgColor`、`TextColor`、`DividerColor` 等）定义为命名常量
  - 所有边距/位置值（`Margin`、`ContentLeft`、`InputLeft` 等）定义为命名常量

### 修复

- **`WM_PAINT` footerTop 最小值不一致**：`LayoutSettingsWindow` 用 `460` 但 `WM_PAINT` 用 `390`，现统一使用 `UiStyle::FooterMinTop`（460）
- **`footerHeight` 重复硬编码**：原两处各写 `78`，现统一引用 `UiStyle::FooterHeight`

## v0.6.1 (2026-05-06)

### 变更

- **ASR 模型启动预加载**：当 `asrBackend` 为 `local` 且模型目录存在时，启动时在后台线程预加载模型（ASR + VAD + 标点），消除首次按热键的延迟
- **onnxruntime/sherpa-onnx/kaldi DLL 延迟加载**：`onnxruntime.dll`、`sherpa-onnx-cxx-api.dll`、`kaldi-native-fbank-core.dll` 通过 `/DELAYLOAD` 延迟加载，只在本地 ASR 实际使用时才加载到内存。纯云端模式空闲内存 ~12 MB（从 ~20 MB 降低）
- **Settings Save 后预加载**：切换到本地 ASR 后端或更换模型后，Save 时后台重新预加载新模型
- **预加载完成 HUD 提示**：预加载完成后显示 "ASR ready: \<模型名\>"

### 修复

- **火山引擎 ASR 现在支持 LLM 纠错**：此前火山引擎结果即使 `postprocess` 设为 `Auto punctuate + LLM` 也会跳过 LLM 纠错。现在三个 ASR 后端（本地、百度、火山引擎）统一支持 LLM 纠错
- **`SaveConfig` 现在正确持久化 `llm_endpoint`、`llm_api_key`、`llm_model` 字段**（此前遗漏）
- 移除未使用的 `g_volcFinalText` 全局变量

## v0.6.0 (2026-05-06)

### 变更

- **源码从单文件重构为多模块架构**：`src/main.cpp`（3269 行）拆分为 5 个编译单元，职责清晰分离
  - `src/globals.h` — 共享常量、控件 ID、结构体定义、extern 全局变量声明
  - `src/engine.h` / `src/engine.cpp`（~750 行）— 后端：配置持久化、ASR 引擎、音频采集、工具函数
  - `src/hud.h` / `src/hud.cpp`（~380 行）— HUD 窗口、Direct2D 渲染、托盘图标、UI 资源管理
  - `src/hotkey.h` / `src/hotkey.cpp`（~330 行）— 热键逻辑、CapsLock 长按、键盘 Hook、HotkeyEdit 自绘控件
  - `src/settings.h` / `src/settings.cpp`（~1190 行）— Settings 窗口、控件、加载/保存、Provider 管理、输入对话框
  - `src/main.cpp`（~560 行）— 入口（WinMain）、主窗口过程、录音会话编排、LLM 纠错
- 全局变量在 `main.cpp` 中定义，其他模块通过 `globals.h` 的 `extern` 声明引用
- `build.bat` 更新：`cl` 命令现在编译 5 个源文件
- `CMakeLists.txt` 更新：`add_executable` 包含新 `.cpp` 文件，新增 `winhttp` 和 `crypt32` 链接依赖
- `.clangd` 更新：添加 UTF-8 字符集标志以兼容 sherpa-onnx 头文件

### 修复

- `SaveConfig` 现在正确持久化 `llm_endpoint`、`llm_api_key`、`llm_model` 字段（此前遗漏）
- 移除未使用的 `g_volcFinalText` 全局变量

## v0.5.0 (2026-05-05)

### 新增

- **Cloud ASR UI 重构**：合并 "Baidu ASR" 和 "豆包ASR" 两个 Tab 为一个 "Cloud ASR" Tab，顶部 Provider 下拉框切换
  - Provider ComboBox 切换 "百度智能云" 和 "火山引擎（豆包）"，下方控件动态显示/隐藏
  - 分组标题根据选中的 Provider 动态更新
- **ASR Backend 选择器移至 Recognition Tab**：作为全局设置放在 Recognition Tab 顶部
- **Shortcut 设置合并到 Recognition Tab**：删除独立 Shortcut Tab，快捷键配置移至 Recognition Tab 底部，加分隔线
- **豆包 ASR Mode 排序调整**：File Recognition (nostream) 排第一（推荐默认）
- **豆包 Model Version 清理**：删除 BigASR 1.0 选项，仅保留 Seed-ASR 2.0 (duration/concurrent)
- **Cloud ASR 报告**：新增 `.trae/documents/cloud_asr_report.md`，记录协议细节、调试指南和踩坑记录

### 变更

- Settings Tab 从 6 个减为 4 个：`Recognition` / `LLM` / `LLM Prompt` / `Cloud ASR`
- 豆包默认模式改为 `bigmodel_nostream`（录音文件识别）

### 修复

- **豆包 nostream 空结果**：`SendAudio(isLast=true)` 返回值被丢弃，现已保存到 `lastPartial` 作为回退
- **ReceiveResult 超时未生效**：`timeoutMs` 参数被忽略，始终使用 2000ms；现通过 `WinHttpSetOption` 动态设置
- **豆包 async 模式超时**：`bigmodel_async` 模式因 `ReceiveResult` 阻塞音频发送导致 8 秒超时；修复为发送/接收线程分离
  - 发送线程：只发音频包，不阻塞在接收上，保证 200ms 间隔
  - 接收线程（drainThread）：持续排空 WebSocket 接收缓冲区，有部分结果就更新 HUD
- **百度 App ID 移除**：确认百度 REST API 不使用 App ID，从 BaiduConfig、UI、config.json 中移除

### 移除

- 移除 `IDC_BAIDU_APP_ID` 控件和 `baiduAppId` 配置字段
- 移除 "Shortcut" Tab（合并到 Recognition）
- 移除 BigASR 1.0 模型版本选项

## v0.2.2 (2026-05-02)

### Added

- **GitHub 发布准备**：目录结构重组，源代码移入 `src/` 目录
  - 运行时 DLL 移入 `dll/` 目录并打包到 git（约 20MB）
  - 新增 `third_party/sherpa-onnx/` 头文件和导入库
  - 新增 `download_models.ps1` 模型下载脚本
- **模型下载优化**：集成 aria2c 多连接下载（4 连接并行）
  - 支持交互式菜单选择下载模型
  - 支持命令行参数 `-Models 1,3` 或 `-Models all`
  - 显示下载进度和速度
- **新用户引导**：首次启动检测模型目录，提示下载
  - 检查是否有任意 ASR 模型目录存在
  - Settings Recognition tab 新增 "Download" 按钮
- **内置 VAD 模型**：Silero VAD 和 FireRed VAD 打包到 git（约 2.5MB）

### Changed

- `.gitignore` 更新：允许 `models/silero_vad.int8.onnx` 和 `models/fireredvad_stream_vad_with_cache.onnx` 提交
- `build.bat` 更新：从 `dll/` 目录复制 DLL，从 `third_party/sherpa-onnx/` 获取头文件
- `CMakeLists.txt` 更新：源文件路径和 include 目录
- `.clangd` 更新：添加 `-Isrc` 和 `-Ithird_party/sherpa-onnx/include`

## v0.2.1 (2026-04-30)

### Added

- **多供应商预设系统**：Settings LLM tab 新增 Provider 下拉框，内置 DeepSeek / OpenRouter / SiliconFlow 三个预设
  - 选择预设自动填充 API Base URL、Model、Extra Params（关闭思考模式参数）
  - 每个供应商独立保存 API Key（DPAPI 加密），切换时自动恢复
  - 支持添加/删除自定义供应商（[+] / [−] 按钮）
- **LLM Prompt 独立 Tab**：System Prompt 从 LLM tab 拆出到独立的 "LLM Prompt" tab
  - System Prompt 多行编辑器高度增加到 340px
  - Basic Fix / Deep Fix 预设按钮保留在 Prompt tab 顶部
- **Extra Params 字段**：LLM tab 新增 Extra Params 单行编辑框
  - 用户可输入 JSON 片段，合并到 LLM API 请求体
  - 预设供应商自动注入关闭思考模式参数
  - 下方提示文字说明用途和示例格式
- **统一关闭思考模式**：`BuildRequestBody` 动态合并 `extraParams`，不再硬编码
  - DeepSeek: `"thinking":{"type":"disabled"}`
  - OpenRouter: `"reasoning":{"effort":"none"}`
  - SiliconFlow/Qwen3.6: `"chat_template_kwargs":{"enable_thinking":false}`

### Changed

- Settings tab 从 2 个扩展到 4 个：`Recognition` / `Shortcut` / `LLM` / `LLM Prompt`
- `config.json` 结构升级：新增 `llm_provider`、`llm_providers_json`，移除旧的 `llm_endpoint`/`llm_api_key`/`llm_model`
- 向后兼容：首次启动自动将旧配置迁移为 "Custom" 供应商

## v0.2.0 (2026-04-30)

### Added

- **FireRedVAD 接入**：Settings 新增 VAD 模型下拉框，可在 Silero VAD 和 FireRed VAD 之间切换
  - 新增 `src/firered_vad.h` header-only 模块：使用 `kaldi_native_fbank` 提取 80 维 fbank 特征 + `onnxruntime` 加载 DFSMN 流式模型
  - FireRedVAD 准确率显著优于 Silero VAD（F1 97.57 vs 95.95，误报率 2.69% vs 9.41%），模型仅 2.2MB
  - 新增 `third_party/kaldi_native_fbank/` 和 `third_party/onnxruntime/` 依赖
  - `build.bat` 和 `CMakeLists.txt` 同步更新链接配置
  - 运行时新增 DLL：`kaldi-native-fbank-core.dll`

### Fixed

- 修复 FireRedVAD 无法检测语音的问题：音频需要 int16 范围（-32768~32767），而非归一化 float（-1.0~1.0），fbank 特征提取前需乘以 32768

## v0.1.4 (2026-04-30)

### Changed

- **用 C++ 直接调用 sherpa-onnx 替换 Python ASR worker**
  - 移除 `asr_worker.py` 进程和 TCP JSON line 通信
  - 移除 Winsock 依赖（`ws2_32.lib`）
  - 新增 `AsrEngine` 类，直接调用 `sherpa-onnx-cxx-api` 的 `OfflineRecognizer`、`VoiceActivityDetector`、`OfflinePunctuation`
  - 识别流程改为：录音 PCM → C++ 直接调用模型 → 返回文本，无中间进程和网络开销
  - Reload 改为清除模型缓存，下次识别时自动重新加载
- `build.bat` 添加 sherpa-onnx include/lib 路径，自动复制 DLL 到 build 目录
- `CMakeLists.txt` 同步更新链接配置
- 运行时只需 3 个 DLL：`sherpa-onnx-cxx-api.dll`、`sherpa-onnx-c-api.dll`、`onnxruntime.dll`
- 不再需要 Python 环境和 `runtime/` 目录中的 Python 解释器

### Removed

- 移除 Python worker 相关代码：`StartWorkerProcess`、`StopWorkerProcess`、`SendWorkerJson`、`PingWorker` 等
- 移除 `WriteWavFile`（不再需要写临时 WAV 文件）
- 移除 `FindPythonExe`、`QuoteArg` 等辅助函数

## v0.1.3 (2026-04-29)

### Changed

- 版本号更新为 `v0.1.3`
- HUD 从 GDI 固定绘制升级为 Direct2D/DirectWrite 渲染
- HUD 尺寸改为 DPI-aware 的 DIP 计算，按实际文本宽度动态调整
- 5 根录音音量条改为由实时 PCM RMS 驱动，并调大可视尺寸
- 构建链接同步加入 `d2d1.lib` / `dwrite.lib`

### Fixed

- 修复高 DPI 下 HUD 文本被裁切的问题
- 修复 HUD 文本垂直居中不稳定的问题
- 修复 layered window 圆角边缘可能出现黑边的问题

## v0.1.2 (2026-04-29)

### Changed

- 托盘菜单版本号更新为 `v0.1.2`
- 默认 `CapsLock` 快捷键改为 300ms 长按触发语音输入
- 短按 `CapsLock` 交还系统处理，用于正常切换大小写

### Fixed

- 长按 `CapsLock` 语音输入结束后恢复按下前的大小写状态，避免误切换 Caps Lock
- 补发短按 `CapsLock` 时放行注入事件，避免被全局键盘 hook 再次拦截

## v0.1.1 (2026-04-29)

### Changed

- 线程上限从 4 提升至 8，auto 策略改为 `min(8, cpu_count)`
- Settings 线程选项从 1/2/3/4/auto 扩展为 1..8/auto，auto 项显示实际线程数

### Fixed

- Settings 窗口打开时固定在屏幕中央，不再出现在左上角

## v0.1.0 (2026-04-28)

- Initial release
