# Qwen Audio 代码审查修复方案

## 目标

根据逐条代码复核结果，修复真实的并发/重试问题，完成低风险代码清理，并明确不对已经确认是误报的项做破坏性修改。

## 审查结论与处理决策

### 不修改的误报/正确现状

1. `WasapiCapture` generation：`StopAudioCapture()` 先递增全局 generation，随后 `WasapiCapture::Stop()` 设置 `m_running=false`、唤醒并 `join()` 采集线程；下一次 `StartAudioCapture()` 只有在旧线程退出后才会覆盖 `m_captureGeneration`。旧消息还会在主线程再次经过 generation 守卫。因此不清零 `m_captureGeneration` 也不会产生跨录音误匹配。
2. `BatchAsrSessionBase::Abort()` 不清理 `pcm_`：这是为避免 `Abort()` 与 `Finish()` 数据竞争而保留的正确设计，session 析构时释放 PCM。
3. HTTP `CloudHttpCancellation`：它是整个 batch session 的粘性取消状态，只在 `Start()` 重置；普通 transient retry 不会把它置为 aborted。重试之间 Reset 会破坏用户取消语义。
4. WASAPI `ReportRuntimeFailure()` 不需要 `SetEvent()`：该函数由采集线程自身调用，随后所有调用点都会退出采集循环；`Stop()` 中的 `SetEvent()` 专门用于唤醒仍在等待的采集线程。

### 实际修复项

1. **WebSocket 生命周期保护**
   - 保留官方允许的“一条 Send 线程 + 一条 Receive 线程”并发，不增加会破坏双工的统一大锁。
   - 在 `Client` 内增加操作生命周期锁：Send、Receive、Finish 和连接阶段的每个 WinHTTP 调用进入受保护区；`Abort()`/`Close()` 取得独占锁后再关闭句柄，保证不会与 in-flight WinHTTP 调用并发关闭。
   - 额外使用发送/接收侧 mutex，防止未来出现两个并发 Send 或两个并发 Receive。
   - `Connect()` 中使用内部关闭路径，避免在已持有生命周期锁时递归调用公共 `Close()`。
2. **不可重试连接错误**
   - `connectRetryable == false` 时立即退出初始连接循环，不再浪费第二次连接往返。
3. **去重与接口准确性**
   - 删除 `qwen_audio_streaming.cpp` 中未使用且与 JSON 公共层重复的 `AppendUtf8()`。
   - 从 `qwen_audio_http::Config` 移除 HTTP 协议不支持的 streaming 专用字段及其赋值。
   - 将 Settings 中 `streaming` 改名为 `isStreamingTransport`，避免误解为 Audio 3 专属模式。
   - 统一 `qwen_audio_streaming.h` 的 `Event` 字段缩进。

## 预期行为

- Send/Receive 仍可并行，partial/final/no-speech 行为不改变。
- Abort/Close 不会在 WinHTTP Send/Receive/Connect 尚未返回时关闭句柄。
- 用户取消仍是 sticky，不会被 batch retry 复活。
- 不可重试错误只进行一次初始连接尝试；可重试错误仍保留一次重连。
- HTTP batch 请求 JSON 不会出现未实现的 streaming 参数。

## 验证标准

1. `qwen_audio_json_test` 通过，覆盖 HTTP JSON、streaming JSON、事件解析、no-speech 与 replay/profile 现有测试。
2. `.\build.bat --test` 通过，规范运行载荷可生成。
3. `git diff --check` 通过。
4. 复审 `Client::Connect/SendAudio/Poll/Finish/Abort/Close` 的锁顺序，确认不存在递归锁、Send/Receive 互相阻塞或对象析构期间悬空指针。

## 实施结果

- [x] WebSocket 操作生命周期锁、发送/接收侧锁已加入；Send/Receive 仍保持双工并发，Abort 只发取消信号，worker/drain 退出后由 Close 独占释放句柄。
- [x] 不可重试的初始连接错误不再执行第二次连接。
- [x] 删除 HTTP 配置中的 streaming 专用字段及其无效赋值。
- [x] 删除重复的 streaming `AppendUtf8()`，统一 Event 字段缩进，并修正 Settings 变量命名。
- [x] 增加 HTTP 请求不携带 streaming-only 参数的离线回归断言。
- [x] `build.bat --test` 与 `git diff --check` 通过。

## 后续诊断增强

- [x] 新增 `%TEMP%\qwen_audio_debug.log`，仅在全局 Debug Mode 开启时写入，并复用 5 MB / 2 份归档轮转。
- [x] 握手日志记录 endpoint host/path、model、连接阶段、WinHTTP 错误码、HTTP 状态码、WebSocket upgrade、`run-task`/`task-started` 和重试结果；不记录 API Key、PCM 或完整 transcript。
- [x] Qwen Model 右侧新增 `Open log` 按钮，打开 `qwen_audio_debug.log`。
- [x] Volcano Engine Model 右侧新增 `Open log` 按钮，打开现有 `volc_asr_debug.log`。
