# 从 AriaType 得到的改进灵感

> 分析 AriaType (https://github.com/joe223/AriaType) 源码后的改进建议。
> AriaType 是用 Rust/Tauri 写的桌面语音输入工具，功能定位与 VoxType 高度重合。

---

## P0: 必须修/做

### 1. 静默期间保活包 (Keepalive)

**问题**：火山引擎服务端有 5s 空闲超时，如果用户说话中间停顿 >5s，WebSocket 会被服务端关闭。

**AriaType 做法**：`stream_processor.rs` 在 VAD 检测到静默时，每 4s 发送一个空 chunk（全零 PCM 3200 bytes）保持连接活跃。

**VoxType 实现**：
```
drainThread 或主线程中：距上次发送 >3.5s 且仍在录音时，
发送 3200 bytes 全零 PCM 包 (SendAudio, isLast=false)
```

**位置**：`main.cpp` 行 476-502 的主循环内

**难度**：低 | **收益**：高（防止长停顿断连）

---

### 2. 停止时 flush 尾部音频

**问题**：VAD gate 在录音期间会丢弃非语音 chunk。用户停止录音时，最后一点语音（"了、吧、嗯"等尾部音节）可能被 VAD 判定为静默而丢弃。

**AriaType 做法**：`process_chunk_for_stop_flush()` 绕过 VAD gating，确保尾部音频不被丢。

**VoxType 实现**：
```
StopRecordingSession 中：
  收集 g_volcPendingAudio 里的最后 2-3 个 chunk
  强制发送（即使 VAD 判定为静默）
```

**位置**：`main.cpp` `StopRecordingSession()` 附近

**难度**：低 | **收益**：中（减少尾部吞字）

---

## P1: 提升稳定性

### 3. 设置迁移框架

**问题**：VoxType 的 Config 结构体在多次迭代中新增了许多字段。旧版 config.json 缺少字段时，代码用默认值兜底，但不会主动补充缺失字段。

**AriaType 做法**：`migrate_cloud_settings() / migrate_to_profiles_map()` 等迁移函数在启动时检测旧格式并自动转换。

**VoxType 实现**：
```cpp
// engine.cpp LoadConfig 末尾：
// if (version < 2) migrate_add_default_fields();
// if (version < 3) migrate_separate_extra_params();
// config.version = CURRENT_VERSION;
// SaveConfig();
```

**位置**：`engine.cpp` `LoadConfig()`

**难度**：低 | **收益**：高（防止配置损坏/不兼容）

---

### 4. Atomic state machine 替代分散的 flag

**问题**：VoxType 用 `g_recording`、`g_volcStreaming`、`g_volcSession.connected` 等多个分散的 bool flag 来管理状态，容易出现不一致。

**AriaType 做法**：`RecordingStateMachine` eq `RecordingState::Idle | RecordingActive | Transcribing | Error`，加上 `can_transition_to()` 状态转换守卫。

**VoxType 实现**：
```cpp
enum class VolcState { Idle, Connecting, Recording, Draining, Done };
std::atomic<VolcState> g_volcState{VolcState::Idle};
// OpenSession → Connecting, send loop → Recording, cleanup → Draining
```

**位置**：`globals.h` + 各状态点

**难度**：中 | **收益**：中（减少竞态条件）

---

### 5. forceAbort 超时后不用 close handle 中断

**问题**：`WinHttpCloseHandle` 在另一个线程正在 `WinHttpWebSocketReceive` 时调用是 UB。虽然目前靠 join 时序避免，但脆弱。

**替代方案**：用 `CancelIoEx(hWebSocket, NULL)` 代替 `WinHttpCloseHandle` 来中断阻塞的 Receive。这样可以安全中断而不销毁句柄，后续还能正常 CloseSession。

**位置**：`volcengine_asr.h` / `main.cpp`

**难度**：中 | **收益**：中（消除 UB）

---

## P2: 体验提升

### 6. 窗口上下文感知

**AriaType 做法**：
1. 截图当前窗口 → OCR → 提取术语/关键词
2. 置信度分级（>0.80 全量，>0.65 保守，<0.65 最小）
3. `to_stt_prompt_hint()`：术语列表用作 ASR corpus context
4. `to_polish_context()`：结构化 markdown 给 LLM polish

**VoxType 第一步**（无需 OCR/截图权限）：
```
capture_active_window_info():
  → GetWindowText(GetForegroundWindow()) 获取窗口标题
  → GetWindowModuleFileName 获取进程名（"Weixin.exe", "chrome.exe" 等）
  → BuildVolcContextJson() 中添加 window_title / process_name
```

**VoxType 第二步**（可选，OCR）：
```
OCR 用 Windows.Media.Ocr 或 Tesseract
→ 提取可见文本中的术语
→ 添加到 Extra Params 的 corpus
```

**位置**：新增 `window_context.cpp/.h`，集成到 `BuildVolcContextJson()`

**难度**：第一步低 / 第二步高 | **收益**：中-高（显著提升特定场景准确率）

---

### 7. LLM Polish 模板系统

**AriaType 做法**：
- 内置模板：filler（去语气词）、standard、concise、formal、creative
- 自定义模板：`CustomPolishTemplate { id, name, system_prompt }`
- 多 backends：Qwen/LFM/Gemma GGUF 本地模型 + OpenAI/Anthropic 云端
- 关联到快捷键 profile：dictate 用 `standard`，riff 用 `creative`

**VoxType 当前**：只有一个 LLM system prompt，不支持模板选择。

**VoxType 实现**：
```
Settings 增加：
  - polish_template 下拉选择（none/filler/standard/concise/formal/creative/custom）
  - custom_polish_prompt 文本编辑框
LLM 调用时用对应 prompt 替换当前硬编码的 system prompt
```

**位置**：`settings.cpp` 增加控件 + `main.cpp` 替换 prompt

**难度**：低 | **收益**：中（用户可自定义纠错风格）

---

### 8. 录音保活调试

**AriaType 做法**：
- 录音保存到 `recordings/{YYYYMMDD_HHMMSS}_{task_id}.wav`
- 成功转录后删除（历史记录里已保存文本）
- 失败时保留录音用于调试
- `raw_audio_buffer` 是 VAD 前的原始 PCM，不受 VAD 决策影响

**VoxType 当前**：没有录音保存功能，出了问题难以复现。

**VoxType 实现**：
```
Config 新增 debug.save_recording (bool)
开启时：
  每次录音保存到 recordings/ 目录
  成功后自动清理
  失败时保留 + 日志提示文件路径
```

**位置**：新增录音保存逻辑

**难度**：低 | **收益**：中（调试利器）

---

## P3: 重构/基建

### 9. `RecordingConsumer` 统一抽象

**AriaType 做法**：
```rust
#[async_trait]
pub trait RecordingConsumer: Send + Sync {
    async fn send_chunk(&self, pcm: Vec<i16>) -> Result<(), String>;
    async fn finish(&self) -> Result<String, String>;
    fn set_partial_callback(&mut self, callback: PartialResultCallback) {}
}
```

本地模型和云端 ASR 都实现这个 trait，`capture.rs` 里统一调用。

**VoxType 当前**：本地 ASR、百度、火山引擎各有独立的调用路径和线程模型。

**VoxType 实现**：C++ 虚基类：
```cpp
class AsrConsumer {
public:
    virtual ~AsrConsumer() = default;
    virtual bool SendChunk(const std::vector<BYTE>& pcm) = 0;
    virtual std::wstring Finish() = 0;
    virtual void SetPartialCallback(std::function<void(std::wstring)> cb) {}
};
```

**位置**：新建 `asr_consumer.h`，逐步迁移各 backend

**难度**：高 | **收益**：高（新 backend 接入成本极低）

---

### 10. 结构化日志替代 printf

**AriaType 做法**：`tracing` crate 的 `info!(task_id, provider, duration_ms, ...)` 结构化字段。函数入口用 `#[instrument]` 自动记录参数和耗时。

**VoxType 当前**：`VolcDebugLog("...%d...%ls...", val1, val2)` 风格。

**VoxType 实现**：引入 `spdlog` 或自己写简单的结构化日志宏：
```cpp
// 改前
VolcDebugLog("SendAudio: %zu bytes, isLast=%d", size, isLast);

// 改后
LOG_INF("send_audio")("bytes", size)("is_last", isLast);
// → "[17:31:44.008] [INF] send_audio: bytes=6400 is_last=0"
```

**难度**：低 | **收益**：中（日志分析更方便）

---

## 实施优先级建议

| 迭代 | 内容 | 估算 |
|------|------|------|
| 1 | keepalive + flush + 录音保活 | 2-3 天 |
| 2 | 设置迁移 + window title 上下文 | 2 天 |
| 3 | polish 模板系统 | 1-2 天 |
| 4 | state machine + CancelIoEx | 3-4 天 |
| 5 | AsrConsumer 抽象 | 5-7 天 |
| 6 | 结构化日志、OCR 上下文 | 3-5 天 |

---

## 与 AriaType 的哲学差异

| 方面 | AriaType | VoxType |
|------|----------|---------|
| 语言 | Rust（类型安全，async/await） | C++（性能，Win32 原生） |
| 跨平台 | macOS + Windows（cpal + tokio） | Windows only（WASAPI + WinHTTP） |
| 抽象风格 | 组合 trait + enum dispatch | 散落全局变量 + switch-case |
| 模型策略 | 本地优先，云端可选 | 可选本地/百度/火山 |
| 纠错 | 丰富模板系统 + 本地 GGUF | 单 system prompt |
| 窗口感知 | OCR 全链路 | 无 |
| 日志 | 结构化 tracing | printf-style |

VoxType 不必照着 AriaType 的 Rust 架构重构，但可以在**具体可落地的特性**（keepalive、flush、模板、窗口标题）上快速跟进，这些 bug fix 之外的体验优化能明显提升用户感知质量。
