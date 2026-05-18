# 从 AriaType 得到的改进灵感

> 基于 AriaType (https://github.com/joe223/AriaType) v0.5.2 源码分析。
> AriaType 是 Rust/Tauri 写的桌面语音输入工具，macOS 优先，功能定位与 VoxType 高度重合。
> 本报告已对照源码验证，修正了初版中不准确的描述。
>
> **本地源码**：`.plan/ref/AriaType/`（shallow clone，已加入 `.gitignore`）
>
> 关键源码路径：
> - 火山引擎 WebSocket：`apps/desktop/src-tauri/src/stt_engine/cloud/volcengine_streaming.rs`
> - 音频流处理 + VAD + Keepalive：`apps/desktop/src-tauri/src/audio/stream_processor.rs`
> - 窗口上下文 OCR：`apps/desktop/src-tauri/src/sensors/window_context.rs`
> - 上下文解析：`apps/desktop/src-tauri/src/runtime_context/window.rs`
> - Polish 模板：`apps/desktop/src-tauri/src/polish_engine/templates.rs`
> - Polish 引擎：`apps/desktop/src-tauri/src/polish_engine/mod.rs`
> - STT 引擎 trait：`apps/desktop/src-tauri/src/stt_engine/traits.rs`
> - 设置 + 迁移：`apps/desktop/src-tauri/src/commands/settings/mod.rs`

---

## 源码关键发现

### 1. AriaType 默认使用 `bigmodel_nostream`，不推荐 async

```rust
// volcengine_streaming.rs
// IMPORTANT: Per AGENTS.md Product Priority Order (accuracy > speed), we use nostream mode.
// Bidirectional streaming interfaces (bigmodel_async, bigmodel) have slightly lower accuracy.
pub enum StreamingMode {
    Async,     // NOT RECOMMENDED: Lower accuracy than NoStream
    Standard,  // NOT RECOMMENDED: Lower accuracy than NoStream
    NoStream,  // RECOMMENDED: Highest accuracy, default mode
}
```

**VoxType 当前默认 `bigmodel_async`**。AriaType 认为准确率优先于速度，nostream 模式准确率最高。
VoxType 可以考虑：保留 async 作为可选项，但让用户知道 nostream 更准。

### 2. AriaType 分包 100ms (1600 samples)，不是 200ms

```rust
pub const RECOMMENDED_CHUNK_SAMPLES: usize = 1600; // 100ms at 16kHz
```

VoxType 当前 `kChunkBytes = 6400` (200ms)。官方文档说"双向流式 200ms 最优"，
但 AriaType 选了 100ms。对 nostream 模式，100ms 更快把音频送入服务器，15s 阈值更早触发。

### 3. AriaType 用 `split()` 实现读写分离

```rust
let (sink, stream) = ws_stream.split(); // tokio-tungstenite
```

Rust 的 `tokio-tungstenite` 天然支持 WebSocket 读写分离（Sink + Stream 独立），
线程安全。VoxType 用 WinHTTP 无法做到——这是根本性的架构差异，WinHTTP 的
`WinHttpWebSocketSend` 和 `WinHttpWebSocketReceive` 不能对同一句柄并发调用。

### 4. AriaType 的 Keepalive 不是发全零 PCM

```rust
// stream_processor.rs
const KEEPALIVE_INTERVAL_SECS: u64 = 4;

let needs_keepalive = self.vad_enabled
    && !vad_result
    && self.last_send_time.is_none_or(|t| now.duration_since(t).as_secs() >= 4);
let has_speech = force_send || vad_result || needs_keepalive;
```

保活逻辑在 VAD 层：静默超过 4s 时，`has_speech=true`，静默段的 PCM 正常发送。
不需要构造全零包——静默段本身就是低能量信号，服务器不会误识别。

### 5. AriaType 有 RNNoise 降噪

```rust
const DENOISE_STRENGTH: f32 = 0.5; // 50% dry/wet 混合
```

RNNoise 48kHz 降噪 → 重采样到 16kHz。VoxType 没有降噪功能。

### 6. AriaType 的窗口上下文非常精细

- 截取当前窗口截图 → OCR → 提取术语
- 置信度分级：>0.80 全量上下文，0.65~0.80 保守，<0.65 最小
- 术语评分系统：camelCase 5分、全大写缩写 4分、含符号 3分、CJK 2分
- OCR 噪声过滤：数字字母混合噪声、机器 ID、常见词排除
- 两个输出：`to_stt_prompt_hint()` 给 ASR、`to_polish_context()` 给 LLM

### 7. AriaType 的 Polish 模板实际只有 4 个

```rust
PolishTemplate { id: "filler", ... }   // 去语气词
PolishTemplate { id: "formal", ... }   // 正式风格
PolishTemplate { id: "concise", ... }  // 精简
PolishTemplate { id: "agent", ... }    // AI Agent 提示词格式
```

没有 `standard` 和 `creative`。但支持自定义模板 `CustomPolishTemplate`。
本地 GGUF 模型有 6 个可选：Qwen3.5-0.8B / LFM2.5-1.2B / Qwen3.5-2B / LFM2-2.6B / Qwen3-4B / Gemma-2B-IT。

### 8. AriaType 的 `RecordingConsumer` trait 设计

```rust
#[async_trait]
pub trait RecordingConsumer: Send + Sync {
    async fn send_chunk(&self, pcm_data: Vec<i16>) -> Result<(), String>;
    async fn finish(&self) -> Result<String, String>;
    fn set_partial_callback(&mut self, _callback: PartialResultCallback) {}
}
```

两个实现：`BufferingConsumer`（本地模型，缓冲后批量识别）和 `StreamingConsumer`（云端 STT，实时流式）。

### 9. AriaType 的 `SttContext` 统一上下文传递

```rust
pub struct SttContext {
    pub initial_prompt: Option<String>,
    pub domain: Option<String>,
    pub subdomain: Option<String>,
    pub glossary: Option<String>,
}
```

各引擎自行解释这些字段。VoxType 的上下文信息分散在多个 Config 字段中。

---

## 改进方案（按优先级排序）

### P1: 防止断连 & 减少吞字

#### 1.1 静默期间保活包 (Keepalive) ✅ 已实现 (v0.8.2)

**问题**：火山引擎服务端有 5s 空闲超时，用户说话中间停顿 >5s 时 WebSocket 被关闭。

**AriaType 做法**：`stream_processor.rs` 在 VAD 检测到静默且距上次发送 ≥4s 时，
将 `has_speech` 设为 true，让静默段 PCM 正常发送。不需要构造全零包。

**VoxType 实现**：
```
main.cpp 发送循环中：
  constexpr DWORD kKeepaliveMs = 3500;
  ULONGLONG lastSendTick = GetTickCount64();
  每次发送 chunk 后更新 lastSendTick
  空闲时检查：(GetTickCount64() - lastSendTick >= kKeepaliveMs)
    && g_volcSession.connected && g_volcSession.hWebSocket
  → 构造 kChunkBytes 全零 PCM 包发送
  → 更新 lastSendTick
  → VolcDebugLog("keepalive sent")
```

**位置**：`main.cpp` 发送循环 (行 500-518)

**难度**：低 | **收益**：高（防止长停顿断连）

---

#### 1.2 停止时 flush 尾部音频 ✅ 已实现 (v0.8.2)

**问题**：FireRed VAD 在 `PossibleTail` 状态可能丢弃尾部音节（"了、吧、嗯"）。
VoxType 的 `Flush()` 已部分解决，但 WASAPI 缓冲区中可能还有残余音频未发送。

**AriaType 做法**：`process_chunk_for_stop_flush(force_send=true)` 绕过 VAD gating。

**VoxType 实现**：
```
发送循环结束后、发送 isLast 包之前：
  EnterCriticalSection(&g_volcAudioCs);
  std::vector<BYTE> remaining;
  remaining.swap(g_volcPendingAudio);  // 取出所有残余
  LeaveCriticalSection(&g_volcAudioCs);
  if (!remaining.empty()) sendChunk(remaining);
```

**位置**：`main.cpp` 发送循环结束后 (行 530-541)

**难度**：低 | **收益**：中（减少尾部吞字）

---

### P2: 提升识别质量

#### 2.1 窗口标题上下文（无需 OCR）

**问题**：ASR 在特定应用场景下容易误识别专业术语。

**AriaType 做法**：截图+OCR 全链路，复杂但效果好。

**VoxType 第一步**（低成本，无需截图权限）：
```cpp
// 录音开始时
HWND hwnd = GetForegroundWindow();
wchar_t title[256]; GetWindowTextW(hwnd, title, 256);
DWORD pid; GetWindowThreadProcessId(hwnd, &pid);
// → 进程名通过 OpenProcess + GetModuleFileNameEx 获取
// → 传入 BuildVolcContextJson() 的 context 字段
```

示例效果：在 VS Code 中说话时，窗口标题 "main.cpp - VoxType - Visual Studio Code"
会作为上下文传给 ASR，帮助识别编程术语。

**位置**：新增 `window_context.h`，集成到 `volcengine_asr.h` 的 `OpenSession`

**难度**：低 | **收益**：中（提升特定场景准确率）

---

#### 2.2 nostream 模式分包优化 (100ms)

**问题**：VoxType 当前 200ms 分包。nostream 模式 15s 阈值从第一个包开始计算，
100ms 分包能更快把音频送入服务器，15s 阈值更早触发返回结果。

**AriaType 做法**：`RECOMMENDED_CHUNK_SAMPLES = 1600` (100ms)。

**VoxType 实现**：
```cpp
// nostream 模式用更小的分包
constexpr size_t kChunkBytes = nostreamMode ? 3200 : 6400;
```

**注意**：需要验证 100ms 分包对 async 模式的影响。官方文档说 async 200ms 最优，
但 100ms 可能也 OK。建议先做 nostream 模式的优化。

**位置**：`main.cpp` kChunkBytes 定义处

**难度**：低 | **收益**：低-中（nostream 模式更快出结果）

---

### P3: 体验提升

#### 3.1 LLM Polish 模板系统

**问题**：VoxType 只有一个 LLM system prompt，不支持风格选择。

**AriaType 做法**：4 个内置模板 (filler/formal/concise/agent) + 自定义模板。

**VoxType 实现**：
```
Settings 增加：
  - polish_template 下拉选择（none/filler/formal/concise/agent/custom）
  - custom_polish_prompt 文本编辑框（选择 custom 时显示）
LLM 调用时用对应 prompt 替换当前硬编码的 system prompt
```

内置模板参考 AriaType 的设计，但适配中文场景：
- filler: 去语气词 + 修同音字
- formal: 正式书面风格
- concise: 精简
- agent: 结构化 markdown

**位置**：`settings.cpp` 增加控件 + `main.cpp` 替换 prompt

**难度**：低 | **收益**：中（用户可自定义纠错风格）

---

#### 3.2 录音保存调试

**问题**：排查 ASR 问题时没有录音数据，难以复现。

**AriaType 做法**：录音保存到文件，成功后删除，失败时保留。

**VoxType 实现**：
```
Config 新增 debug.save_recording (bool)
开启时：
  WASAPI 采集的原始 PCM 保存到 recordings/ 目录
  写 WAV 文件头（16kHz, 16bit, mono）
  成功识别后自动清理
  失败时保留 + 日志提示文件路径
```

**位置**：`wasapi_capture.cpp` 或 `main.cpp` 录音线程

**难度**：低 | **收益**：中（调试利器）

---

### P4: 稳定性基建

#### 4.1 设置迁移框架 ✅ 已实现 (v0.8.2)

**问题**：Config 结构体多次迭代新增字段，旧版 config.json 缺少字段时不会主动补充。

**AriaType 做法**：`migrate_cloud_settings()` / `migrate_to_profiles_map()` 迁移函数。

**VoxType 实现**：
```cpp
// globals.h Config 增加 configVersion 字段 (默认 0)
int configVersion = 0;

// engine.cpp
static constexpr int kCurrentConfigVersion = 1;

// LoadConfig 中读取版本号，执行迁移：
g_config.configVersion = ExtractJsonInt(json, "config_version", 0);
if (g_config.configVersion < 1) {
    // v0→v1: 补充火山引擎默认值
    if (g_config.volcMode.empty()) g_config.volcMode = L"bigmodel";
    if (g_config.volcResourceId.empty()) g_config.volcResourceId = L"volc.seedasr.sauc.duration";
    if (g_config.cloudProvider.empty()) g_config.cloudProvider = L"volcengine";
}
// LoadConfig 末尾：版本升级后自动保存
if (g_config.configVersion < kCurrentConfigVersion) {
    g_config.configVersion = kCurrentConfigVersion;
    SaveConfig();
}

// SaveConfig 开头写入版本号：
"config_version": 1,
```

后续新增字段时，只需：
1. `kCurrentConfigVersion++`
2. 在 `LoadConfig` 中加 `if (g_config.configVersion < N)` 迁移块

**位置**：`globals.h` Config + `engine.cpp` LoadConfig/SaveConfig

**难度**：低 | **收益**：高（防止配置损坏/不兼容）

---

#### 4.2 CancelIoEx 替代 WinHttpCloseHandle 中断

**问题**：`WinHttpCloseHandle` 在另一线程 `WinHttpWebSocketReceive` 时调用是 UB。
当前靠"先 join 再 close"避免，但脆弱。

**替代方案**：`CancelIoEx(hWebSocket, NULL)` 中断阻塞的 Receive，不销毁句柄。

**风险**：微软文档未明确 WebSocket 句柄在 CancelIoEx 后的状态。AriaType 用
`tokio-tungstenite` 底层是 `rustls + tokio`，取消机制完全不同，不能直接类比。

**建议**：当前"先 join 再 close"方案已解决死锁，暂缓。等 WinHTTP 社区有
CancelIoEx + WebSocket 的成功案例再跟进。

**难度**：中 | **收益**：中（消除 UB，但风险未知）

---

### P5: 长期重构

#### 5.1 AsrConsumer 统一抽象

**AriaType 做法**：`RecordingConsumer` trait，`BufferingConsumer` + `StreamingConsumer`。

**VoxType 现状**：本地 ASR、百度、火山引擎各有独立调用路径和线程模型。

**建议**：暂缓。当前三个 backend 差异太大（同步/HTTP REST/WebSocket 双向流），
C++ 没有 async/await，抽象层需要用回调或 future 模拟，复杂度高。
等新 backend 需求出现时再重构。

**难度**：高 | **收益**：高（新 backend 接入成本极低）

---

#### 5.2 Atomic state machine 替代分散的 flag

**问题**：`g_recording`、`g_volcStreaming`、`g_volcSession.connected` 等多个分散 flag。

**建议**：暂缓。当前 flag 体系经过多轮 bug 修复后已相对稳定，引入状态机
改动面大、风险高。可以在后续大版本重构时考虑。

**难度**：中 | **收益**：中（减少竞态条件）

---

#### 5.3 RNNoise 降噪

**AriaType 做法**：RNNoise 48kHz 降噪，50% dry/wet 混合，然后重采样到 16kHz。

**VoxType 实现**：需要引入 `nnnoiseless` C 库或类似依赖，增加编译复杂度。
VoxType 的 FireRed VAD 已有基础的语音/静默区分，降噪是更高层的前处理。

**建议**：暂缓。降噪对干净环境收益有限，嘈杂环境收益大但用户可以自己用
系统级降噪。等用户反馈有需求再考虑。

**难度**：中 | **收益**：中-高（嘈杂环境）

---

#### 5.4 OCR 窗口上下文（第二步）

**AriaType 做法**：截图 → OCR → 术语提取 → 置信度分级 → ASR context + LLM context。

**VoxType 实现**：Windows 上可用 `Windows.Media.Ocr` API（UWP/WinRT），
不需要第三方依赖，但需要 C++/WinRT 头文件和权限。

**建议**：等 P2.1 窗口标题上下文验证效果后再决定是否做 OCR。
窗口标题+进程名可能已经足够提升准确率，OCR 是锦上添花。

**难度**：高 | **收益**：高（专业场景准确率大幅提升）

---

#### 5.5 结构化日志

**AriaType 做法**：`tracing` crate 的 `info!(key=value)` 结构化字段 + `#[instrument]`。

**VoxType 现状**：`VolcDebugLog("...%d...", val)` printf 风格。

**建议**：暂缓。当前日志格式已够用，等需要日志分析工具时再改进。

**难度**：低 | **收益**：低-中

---

## 实施路线图

| 阶段 | 内容 | 难度 | 收益 | 状态 |
|------|------|------|------|------|
| **1** | Keepalive 保活 + Stop flush 尾部音频 | 低 | 高 | ✅ v0.8.2 |
| **2** | 窗口标题上下文 + nostream 分包优化 | 低 | 中 | |
| **3** | LLM Polish 模板 + 录音保存调试 | 低 | 中 | |
| **4** | 设置迁移框架 | 低 | 高 | ✅ v0.8.2 |
| **5** | CancelIoEx / State machine / 降噪 | 中-高 | 中 | |
| **6** | AsrConsumer 抽象 / OCR / 结构化日志 | 高 | 中-高 | |

---

## VoxType 的自身优势（不需要改的）

| 方面 | VoxType 优势 | AriaType 劣势 |
|------|-------------|---------------|
| Windows 原生 | WASAPI + WinHTTP，零依赖运行 | Tauri/Electron 打包大，Windows 支持还在 in progress |
| 火山引擎深度集成 | nostream/async 双模式 + VAD 裁剪 + 连接池 + 重连 | 只用 nostream，无连接池 |
| 本地 ASR | sherpa-onnx 直接调用，启动快 | 本地模型加载慢，GGUF 推理延迟高 |
| 微信粘贴适配 | WM_CHAR 逐字符发送 | 无此适配 |
| CapsLock 热键 | 短按切换大小写，长按录音 | 无此设计 |
| 延迟加载 | DLL 延迟加载，纯云端模式空闲 ~12MB | Tauri 基础内存更高 |
| HUD 实时反馈 | 音量条 + 语音检测变色 + partial 显示 | Pill Window 功能类似但实现不同 |

VoxType 不必照着 AriaType 的 Rust 架构重构，但可以在**具体可落地的特性**
（keepalive、flush、窗口标题、模板）上快速跟进，这些 bug fix 之外的体验优化
能明显提升用户感知质量。同时保留 VoxType 在 Windows 原生、火山引擎深度集成、
本地 ASR 性能等方面的优势。
