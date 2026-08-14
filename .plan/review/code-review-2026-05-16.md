# VoxType 代码审查报告

审查日期：2026-05-16
审查范围：src/ 全部源文件
审查目标：Bug、竞态条件、内存问题、逻辑错误、优化机会

---

## 一、Bug（需修复）

### BUG-01: 火山引擎 VAD 状态机 `g_volcVadState` 无线程同步保护 ✅ 已修复

**严重性**: 高
**文件**: `wasapi_capture.cpp:267-300`, `main.cpp:516`

`g_volcVadState` 在 CaptureThread 中写入，在火山引擎线程中读取，两个线程没有同步机制。C++ 标准下属于数据竞争。

**修复**: `g_volcVadState` 改为 `std::atomic<int>`（globals.h 声明 + main.cpp 定义 + VolcDebugLog 传参加 `.load()`）

**修复验证**: 编译通过。atomic 的 `operator=` 和 `operator==` 自动使用 `memory_order_seq_cst`，wasapi_capture.cpp 中的赋值和比较无需改动。

### BUG-02: `ReceiveResult` 中 `s_lastRecvError` 是 static 非 atomic 变量 ✅ 已修复

**严重性**: 低
**文件**: `volcengine_asr.h:272`

`static DWORD s_lastRecvError` 在 `ReceiveResult` 中读写，该函数可能被 drainThread + 主发送循环两个线程调用。

**修复**: 改为 `static std::atomic<DWORD> s_lastRecvError{0}`，读写改用 `.load()` / `.store()`

**修复验证**: 编译通过。`<atomic>` 已在 volcengine_asr.h 中 include。

### BUG-03: `llm_refine.h` 的 `ParseResponse` 中 `std::wstring::npos` 与 `std::string` 比较 ✅ 已修复

**严重性**: 中
**文件**: `llm_refine.h:167`

`pos` 来自 `std::string::find()`，却与 `std::wstring::npos` 比较。虽然值相同（都是 `SIZE_MAX`），但语义不正确，是复制粘贴的疏忽。

**修复**: `wstring::npos` → `string::npos`

**修复验证**: 编译通过。

### BUG-04: `baidu_asr.h` 的 `ExtractJsonStr` 不处理转义字符 ✅ 已修复

**严重性**: 高（功能影响最大）
**文件**: `baidu_asr.h:28-40`

百度 ASR 的 `ExtractJsonStr` 用简单的 `find('"')` 找结束引号，不处理 `\"` 转义。语音识别结果中包含引号并不罕见，JSON 中含转义引号会截断结果。

**修复**: 重写 `ExtractJsonStr`，采用与火山引擎版本相同的转义处理逻辑（统计连续反斜杠，偶数个后的 `"` 是结束符；正向扫描跳过转义序列）。

**修复验证**: 编译通过。关键场景验证：
- `\\"` (2反斜杠+引号): 正向跳过 `\\`，到 `"` 时反向计数 2 个反斜杠（偶数）→ break ✓
- `\\\"` (3反斜杠+引号): 正向跳过 `\\`，到 `\` 时跳过 `"`，引号被正确跳过（3=奇数，是转义引号）✓
- `\\\\"` (4反斜杠+引号): 正向跳过 `\\`，到 `\\` 时跳过，到 `"` 时反向计数 4 个反斜杠（偶数）→ break ✓

### BUG-05: `g_streamingVadSamples` 的竞态条件 ✅ 已修复

**严重性**: 中
**文件**: `main.cpp:292-296`, `main.cpp:620-645`

如果用户快速连续按快捷键，第二次 `StartRecordingSession` 可能在第一次 `RecognizeAsync` 线程还没读取 `g_streamingVadSamples` 时就清空了它。

**修复**: 在 `RecognizeAsync` 中，启动线程前将 `g_streamingVadSamples` move 到局部变量 `localStreamingVadSamples`，通过 init-capture 传入线程。线程内使用 `localStreamingVadSamples`，不再访问全局变量。

**修复验证**: 编译通过。流程正确：UI 线程 StopRecordingSession 写入 → RecognizeAsync 同步 move 到局部 → 启动线程 → 线程内使用局部变量。竞态窗口消除。

### BUG-06: 火山引擎 nostream/async 模式长录音卡死 ✅ 已修复

**严重性**: 高（功能完全不可用）
**文件**: `volcengine_asr.h:696-723`, `main.cpp:447-530`

nostream/async 模式下 `SendAudio` 只发不收，服务器响应堆积在 WinHTTP 接收缓冲区 → TCP 流控 → `WinHttpWebSocketSend` 阻塞 → 发送循环卡死 → watchdog 超时。录音超过约 15 秒必然触发。

**修复**（3 处关键改动）：
1. nostream 模式也启动 drainThread（之前只有 async 有），持续读取服务器响应
2. async/nostream 模式 `SendAudio(isLast=true)` 跳过接收，只发送帧，让 drainThread 负责读取最终结果（避免主线程和 drainThread 同时读 WebSocket）
3. 发送 isLast 后再 join drainThread（之前是先 join 再发 isLast，导致 join 期间无人读取响应）

**修复验证**: 编译通过。async 模式 partial + 最终结果正常，nostream 模式 ≥15s partial 正常显示。

### BUG-07: watchdog 录音期间误杀会话 ✅ 已修复

**严重性**: 高
**文件**: `main.cpp:813-831`

watchdog 18s 计时器从录音开始时启动，用户录 15s+ 时 watchdog 在录音期间就触发，强制终止正常工作的会话。

**修复**: watchdog 触发时，如果 `g_recording == true`（用户还在录音），重置 18s 计时器继续等待；只有录音结束后 volc 线程仍卡 18s 才强制终止。

**修复验证**: 编译通过。用户录 24s+ 不再被 watchdog 误杀。

---

## 二、代码质量

### QUAL-01: `g_volcVadSilentCount` 非 atomic 但仅单线程访问

**严重性**: 低
**文件**: `globals.h:88`

`g_volcVadSilentCount` 为普通 `int`，但目前只在 CaptureThread 的 `g_volcAudioCs` 临界区内访问，不存在实际数据竞争。如果未来需要在其他线程读取，应改为 `std::atomic<int>`。

### QUAL-02: `g_volcVadPreBuffer` 的 `push_back(std::move(frameData))` 风格

**严重性**: 低
**文件**: `wasapi_capture.cpp:277`

move 后 `frameData` 变为空，但后续代码不再使用（if/else 分支），实际无问题。建议在 move 后显式标记，使意图更清晰。

### QUAL-03: 全局变量过多

项目使用了大量全局变量（`globals.h` 中声明了 40+ 个全局变量），包括 UI 句柄、线程状态、VAD 状态、配置等。这增加了耦合度，使代码难以测试和重构。

**建议**: 逐步将相关全局变量封装为结构体/类，如：
- `VadStateMachine`（封装 `g_volcVadState`, `g_volcVadSilentCount`, `g_volcVadPreBuffer`, `g_volcVadTailBuffer`）
- `RecordingState`（封装 `g_recording`, `g_recordingMs`, `g_audioLevel` 等）
- `DebugMetrics`（封装 `g_vadMs`, `g_asrDecodeMs` 等）

### QUAL-04: 三个不同的 `ExtractJsonStr` 实现 ✅ 已修复

- `volcengine_asr.h` — 已删除本地版本，改用 utils.h 通用版本
- `baidu_asr.h` — 已删除本地版本，改用 utils.h 通用版本
- `engine.cpp` 的 `ExtractJsonString` — 保留不动（语义不同：解码转义 `\n`→换行符，而通用版保留原始转义序列）

**修复**: 在 `utils.h` 中新增通用 `ExtractJsonStr`（支持转义引号，统计连续反斜杠），删除 `volcengine_asr.h` 和 `baidu_asr.h` 中的本地重复实现。火山引擎路径的 VolcDebugLog 日志移至调用点。

**修复验证**: 编译通过。三处调用均正确使用 utils.h 版本。

### QUAL-05: `VOLC_DEBUG_LOG` 宏始终为 1 ✅ 已修复

**文件**: `volcengine_asr.h:21`

生产版本中调试日志始终开启，会写入 `%TEMP%\volc_asr_debug.log`，影响性能且可能泄露用户数据。

**修复**: 采用方案 C（编译期保留 + 运行时检查）。新增 `extern bool g_enableDebugMode`，`VolcDebugLog` 入口处 `if (!g_enableDebugMode) return;`。`g_enableDebugMode` 与 `g_config.enableDebugMode` 同步，同步点：LoadConfig 后、托盘菜单切换后、Settings Save 后（kReloadMessage）。

**修复验证**: 编译通过。Debug Mode 未勾选时，56 处 VolcDebugLog 调用均在第一行返回，零 I/O 开销。

### QUAL-06: `goto openSessionOk` 在火山引擎线程中

**文件**: `main.cpp:411-431`

使用 `goto` 跳过错误处理进入正常流程。虽然功能正确，但降低了可读性。

**建议**: 重构为循环 + break 模式。

### QUAL-07: `ExtractJsonStr` (utils.h) 不解码 JSON 转义序列

**严重性**: 低
**文件**: `utils.h:48-71`

统一的 `ExtractJsonStr` 正确处理了 `\\"` 边界情况，但返回引号内的原始内容，**不解码转义序列**。例如 JSON `"hello\nworld"` 返回字面量 `hello\nworld`（反斜杠+n），而非换行符。`engine.cpp` 的 `ExtractJsonString` 则正确解码了 `\n`、`\r`、`\t`、`\"`、`\\`。

对于 ASR 结果通常无影响（语音文本很少包含转义序列），但如果服务器返回含 `\"` 的文本，用户会看到原始反斜杠。

**建议**: 在 utils.h 版本中添加可选的 unescape 参数，或保持现状但在函数注释中明确说明语义差异。

### QUAL-08: `VolcSession::partialText` 是死代码

**严重性**: 低
**文件**: `volcengine_asr.h:124`

`VolcSession::partialText` 字段声明但从未读写，是之前设计的残留。

**建议**: 删除。

### QUAL-09: drainThread 第二轮循环 1ms 超时导致立即退出

**严重性**: 低
**文件**: `main.cpp:461-468`

drainThread 的 "drain remaining" 循环使用 `DrainReceiveBuffer`（1ms 超时），`asyncDrainDone` 设为 true 后第一轮循环退出，第二轮循环几乎总是在第一次迭代就因超时 break。`asyncPartial` 回退路径实际上是死代码——最终结果由 `CloseSession` 的 500ms 重试循环获取。

**建议**: 第二轮循环使用更长超时（500ms），或删除第二轮循环，依赖 `CloseSession` 获取最终结果。

### QUAL-10: `lastPartial` 在 async/nostream 模式下始终为空

**严重性**: 低
**文件**: `main.cpp:527`

async/nostream 模式下 `SendAudio` 始终返回空字符串，`lastPartial` 永远不会被更新。`finalText` 的 `lastPartial` 回退路径是死代码。

**建议**: 可忽略，不影响功能（`asyncPartial` 和 `CloseSession` 覆盖了所有情况）。

---

## 三、潜在问题（建议关注）

### POT-01: `g_asrEngine.Lock()/Unlock()` 在 CaptureThread 中使用，可能造成延迟

**文件**: `wasapi_capture.cpp:255-263, 314-322`

CaptureThread 中每帧（~10ms）都要 Lock/Unlock `g_asrEngine` 的 mutex 来调用 VAD。如果 UI 线程同时持有锁（如 Settings Save → Reload），CaptureThread 会被阻塞，可能导致音频缓冲区溢出或丢帧。

**建议**: 考虑将 VAD 实例独立于 `AsrEngine` 的 mutex，或使用 try_lock + fallback。

### POT-02: `g_volcPendingAudio` 的 `erase(begin, begin+take)` 性能问题

**文件**: `main.cpp:478`

`std::vector::erase` 是 O(n) 操作。在高速录音场景下，频繁 erase 开销不小。

**建议**: 改用 `std::deque<BYTE>` 或维护一个 read offset 来避免频繁移动内存。

### POT-03: `g_audioData` 同样使用 `vector::insert` 追加，无上限控制

**文件**: `wasapi_capture.cpp:239`

如果录音时间很长（如用户忘记松开快捷键），`g_audioData` 会无限增长。

**建议**: 添加最大录音时长限制（如 60s），超过后自动停止。

### POT-04: 火山引擎 `EnsureConnection` 使用 `WINHTTP_ACCESS_TYPE_NO_PROXY`

**文件**: `volcengine_asr.h:422/864`

不使用系统代理，企业内网环境会连接失败。而百度 ASR 和 LLM 都用 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`。

**建议**: 统一使用 `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`，或在配置中添加代理选项。

### POT-05: `PasteTextImeAware` 中微信检测使用 `OpenProcess` 可能失败

**文件**: `settings.cpp:164`

如果前台窗口是管理员权限进程而 VoxType 是普通权限，`OpenProcess` 会失败，导致微信场景下走 Ctrl+V 路径而非 WM_CHAR 路径，输入失败。

**建议**: 添加 `OpenProcess` 失败时的 fallback 逻辑（如尝试 WM_CHAR）。

### POT-06: `ShowInputDialog` 和 `ShowVolcExtraDialog` 使用 `PostQuitMessage(0)` ✅ 已修复

**文件**: `settings.cpp:950, 1078`

两个自定义对话框在 `WM_DESTROY` 中调用 `PostQuitMessage(0)`，会向线程消息队列投递 `WM_QUIT`。如果内层消息循环已退出，`WM_QUIT` 会留在队列中被外层主循环消费，导致应用意外退出。

**修复**: 删除两处 `PostQuitMessage(0)`，改为在消息循环中 `DispatchMessageW` 后检查 `IsWindow(dlg)`，对话框销毁后自然退出循环。不向消息队列投递 `WM_QUIT`，避免污染外层循环。

**修复验证**: 编译通过。对话框 OK/Cancel/Close 均走 `DestroyWindow` → `WM_DESTROY` → 消息循环检测 `!IsWindow(dlg)` → 退出。

### POT-07: `RefineWithLlmAsync` 中 detached 线程写入全局变量 `g_llmMs`

**文件**: `main.cpp:240`

detached 线程写入全局变量 `g_llmMs`，而 UI 线程可能同时读取它（debug 输出）。虽然 `double` 的写入在 x86 上是原子的（8 字节对齐），但严格来说存在数据竞争。

**建议**: 改为 `std::atomic<double>` 或通过 PostMessage 传递。

### POT-08: watchdog/WM_DESTROY 关闭 WinHTTP 句柄时可能和 volc 线程竞争

**严重性**: 中
**文件**: `main.cpp:824-831, 870-876`

watchdog 和 `WM_DESTROY` 处理器在主线程设置 `forceAbort = true` 后立即关闭 `hWebSocket`/`hConnect`/`hSession`。但 volc 线程可能正在使用这些句柄（`WinHttpWebSocketSend`/`Receive`），存在 use-after-free 风险。

虽然 `forceAbort` 先设置，但 volc 线程检查 `forceAbort` 和使用句柄之间存在时间窗口。实际表现为偶发崩溃（概率低但非零）。

**建议**: 设置 `forceAbort` 后只关闭 `hSession`（会级联取消所有子句柄的操作），或等待 volc 线程退出后再关闭句柄。

### POT-09: `g_volcSession.hWebSocket` 非 atomic，drainThread 和 volc 线程同时访问

**严重性**: 低
**文件**: `main.cpp:454`, `volcengine_asr.h:687`

drainThread 在循环条件中读取 `g_volcSession.hWebSocket`，而 volc 线程的 `SendAudio` 可能在发送失败时设置 `hWebSocket = nullptr`。x86 上指针大小的读写通常是原子的，但 C++ 标准下是未定义行为。更关键的是 drainThread 可能读到非空但已被关闭的句柄。

**建议**: 将 `hWebSocket` 改为 `std::atomic<HINTERNET>`，或在 drainThread 中增加 `forceAbort` 检查。

### POT-10: `g_volcVadState` TOCTOU — WASAPI 线程可能在 volc 线程检查后更新状态

**严重性**: 低
**文件**: `main.cpp:512`, `wasapi_capture.cpp:267-298`

"No speech detected" 检查读取 `g_volcVadState == 0`，但 WASAPI 线程可能正在处理最后一个音频块并即将更新 `g_volcVadState` 为 1。`StopRecordingSession` 设置 `g_streamingVadReady = false` 后 WASAPI 线程会停止 VAD 处理，但当前正在处理的块可能还没完成。

**建议**: 在 `StopRecordingSession` 中先 `StopAudioCapture()`（join WASAPI 线程），再检查 `g_volcVadState`。当前顺序是先设 `g_volcVadReady = false` 再 `StopAudioCapture()`，WASAPI 线程可能在 `g_volcVadReady = false` 后仍完成当前帧的 VAD 处理。

---

## 四、优化建议

### OPT-01: 火山引擎路径中 `floatBuf` 每帧分配可优化

**文件**: `wasapi_capture.cpp:250-252, 309-311`

每帧（~10ms，约 160 个采样）都分配一个 `std::vector<float> floatBuf(written)`。可以将其提升为 CaptureThread 的线程局部变量，复用内存。

### OPT-02: 本地 ASR 路径的冗余 `PcmToFloat` 转换

**文件**: `main.cpp:298`, `engine.cpp` Recognize 内部

当 `g_streamingVadSamples` 为空时，`RecognizeAsync` 先做 `PcmToFloat(pcm)`，然后 `Recognize()` 内部可能再做一次 VAD。微小开销（<1ms），可忽略。

### OPT-03: `g_volcRecognitionHistory` 无并发保护

**文件**: `main.cpp:99, 191-202, 556`

在火山引擎线程中读写，在 UI 线程中读取。如果用户在火山引擎线程还在写时按了快捷键，可能产生竞态。

**建议**: 添加 mutex 保护，或在 `BuildVolcContextJson` 中拷贝一份。

### OPT-04: `VolcDebugLog` 每次调用都 fopen/fclose

**文件**: `volcengine_asr.h:24-44`

高频调用时 I/O 开销不小。

**建议**: 改为保持文件打开（线程局部），或使用内存缓冲区批量写入。

### OPT-05: Settings 窗口每次打开都重新创建所有控件

**文件**: `settings.cpp:1174-1557`

窗口只创建一次（`g_settingsWindow` 复用），如果配置在两次打开之间发生了变化，控件显示的值可能过期。

**建议**: 在 `ShowSettingsWindow` 时重新调用 `LoadSettingsControls` 刷新控件值。

### OPT-06: `SendUnicodeText` 中 `Sleep(1)` 可能过短

**文件**: `settings.cpp:89`

某些应用可能处理不过来 1ms 间隔的 WM_CHAR 消息，导致字符丢失。

**建议**: 可考虑将间隔改为可配置，或根据目标应用动态调整。

### OPT-07: `DrainReceiveBuffer` 1ms 超时导致 drainThread 忙等待

**严重性**: 低
**文件**: `volcengine_asr.h:713-715`

drainThread 的循环用 1ms 超时调用 `ReceiveResult`，无数据时形成忙等待循环（调用 → 1ms 超时 → 循环），浪费 CPU。20-50ms 超时可以显著降低 CPU 使用率，同时保持 partial 更新的响应性。

**建议**: 将 `DrainReceiveBuffer` 超时改为 20ms。注意：0.7.1 版本用 1ms 且 async 模式正常工作，改为 50ms 时曾导致 async 模式超时，需要谨慎测试。

### OPT-08: `g_volcSentBytes` 非 atomic，跨线程读取

**严重性**: 低
**文件**: `main.cpp:443, 753`

`g_volcSentBytes` 是普通 `size_t`，在 volc 线程中写入（line 443），在主线程 debug 输出中读取（line 753）。虽然 `PostMessageW` 提供隐式内存屏障，但严格来说应改为 `std::atomic<size_t>`。

**建议**: 改为 `std::atomic<size_t>`，与 `g_volcVadState` 等保持一致。

---

## 五、安全性

### SEC-01: API Key 在内存中以明文 `std::wstring` 存储

`g_config.volcApiKey`、`g_config.baiduApiKey`、`g_config.baiduSecretKey`、`g_config.llmApiKey` 在内存中是明文。虽然保存到磁盘时使用了 DPAPI 加密，但运行时内存中可被其他进程读取。

**建议**: 对于高安全场景，可考虑使用 `SecureZeroMemory` 在使用后清除内存，或使用 Windows Credential Store。

### SEC-02: `VolcDebugLog` 将 API Key 前 12 字符写入日志

**文件**: `volcengine_asr.h:444, main.cpp:990-991`

API Key 的部分内容被写入日志文件和显示在 UI 中。

**建议**: 只显示 Key 的前 4 个字符 + "..." 或使用哈希摘要。

---

## 六、修复后新发现

### NEW-01: 未使用的 lambda capture `doVadFlag` ✅ 已修复

**严重性**: 低
**文件**: `main.cpp:403-404`

`doVadFlag` 被捕获到火山引擎线程的 lambda 中，但在 lambda 体内从未使用。VAD 逻辑已移到 CaptureThread，volc 线程只用 `g_volcVadState` 和 `g_volcVadDoTrim`（全局变量）。

**修复**: 删除 `bool doVadFlag = doVad;`，lambda capture 改为 `[vcfg, config]()`。

**修复验证**: 编译通过。

### NEW-02: `g_streamingVadReady` 和 `g_volcVadDoTrim` 跨线程无原子保护 ✅ 已修复

**严重性**: 低
**文件**: `globals.h`, `main.cpp:398-399`, `wasapi_capture.cpp:245/308`

两个都是 plain bool，与 BUG-01 同属一类问题。但写操作只发生在录音会话边界，实际竞态窗口极小。

**修复**: 两个变量均改为 `std::atomic<bool>`（globals.h 声明 + main.cpp 定义）。VolcDebugLog variadic 传参加 `.load()`。

**修复验证**: 编译通过。if 条件中的隐式 `operator bool` 无需改动。

---

## 七、本次会话额外修复

### FIX-HUD: 删除启动时 "ASR ready" HUD 显示 ✅ 已修复

**文件**: `main.cpp:681-686, 706-708`

启动时显示 "ASR ready" 的 `kHudHideTimer` 会和录音流程的 HUD 生命周期冲突——用户在 "ASR ready" 还没消失时按快捷键，`kHudHideTimer` 会提前隐藏录音中的 HUD。

**修复**: 删除 WM_CREATE 和 kPreloadDoneMessage 中的 `ShowHud` + `SetTimer`。托盘图标本身表示程序在运行。

### FIX-VAD: 本地 ASR 流式 VAD 无语音时直接返回 ✅ 已修复

**文件**: `main.cpp:648-653`

流式 VAD 工作后如果 `g_streamingVadSamples` 为空（全程静音），仍会走全量 PCM → ASR → 返回 "(empty result)"，浪费 ~1s ASR 时间。

**修复**: `g_streamingVadSamples.empty()` 时直接显示 "No speech detected" 并 return，不启动 ASR 线程。与火山引擎路径的 `g_volcVadState == 0` 判断对称。

**注意**: 轻声说话时 VAD 可能检测到音量但不够组成完整语音段（`segments` 为空），此时也会返回 "No speech detected"。待后续优化。

---

## 八、总结

| 类别 | 数量 | 已修复 | 未修复 | 最高严重性 |
|------|------|--------|--------|-----------|
| Bug | 7 | 7 | 0 | — (全部已修复) |
| 修复后新发现 | 2 | 2 | 0 | — (全部已修复) |
| 本次额外修复 | 2 | 2 | 0 | — (全部已修复) |
| 代码质量 | 10 | 2 | 8 | 低 |
| 潜在问题 | 10 | 1 | 9 | 中（POT-04, POT-08） |
| 优化建议 | 8 | 0 | 8 | 低 |
| 安全性 | 2 | 0 | 2 | 中 |

**已修复清单**：
1. ✅ **BUG-01**: `g_volcVadState` → `std::atomic<int>`
2. ✅ **BUG-02**: `s_lastRecvError` → `std::atomic<DWORD>`
3. ✅ **BUG-03**: `wstring::npos` → `string::npos`
4. ✅ **BUG-04**: 百度 `ExtractJsonStr` 支持转义引号
5. ✅ **BUG-05**: `g_streamingVadSamples` 启动线程前 move 到局部变量
6. ✅ **BUG-06**: 火山引擎 nostream/async 模式长录音卡死（drainThread + isLast 流程重构）
7. ✅ **BUG-07**: watchdog 录音期间误杀会话（g_recording 时自动续期）
8. ✅ **QUAL-04**: 三个 `ExtractJsonStr` 统一到 utils.h
9. ✅ **QUAL-05**: `VolcDebugLog` 运行时检查 `g_enableDebugMode`
10. ✅ **NEW-01**: 删除未使用的 `doVadFlag` lambda capture
11. ✅ **NEW-02**: `g_streamingVadReady`/`g_volcVadDoTrim` → `std::atomic<bool>`
12. ✅ **POT-06**: `PostQuitMessage(0)` 改为 `IsWindow(dlg)` 检查
13. ✅ **FIX-HUD**: 删除启动时 "ASR ready" HUD 显示
14. ✅ **FIX-VAD**: 本地 ASR 流式 VAD 无语音时直接返回 "No speech detected"

**下一步建议优先级**（未修复项）：
1. POT-08: watchdog/WM_DESTROY 关闭 WinHTTP 句柄时的 use-after-free 风险
2. POT-04: 火山引擎代理支持（`WINHTTP_ACCESS_TYPE_DEFAULT_PROXY`）
3. POT-07: `g_llmMs` detached 线程写入 → `std::atomic<double>` 或 PostMessage
4. POT-05: `OpenProcess` 失败时的微信检测 fallback
5. QUAL-03: 全局变量过多，逐步封装为结构体/类
6. OPT-03: `g_volcRecognitionHistory` 并发保护
7. FIX-VAD 改进: 轻声说话被误判为 "No speech detected"（需用 `HasSpeech()` 判断）
