# ASR Fallback Backend 方案

> 状态：方案设计  
> 日期：2026-06-28  
> 复审：2026-06-29  
> 目标：在 Recognition tab 增加 Fallback ASR Backend。当默认 ASR 后端发生可恢复/运行类失败时，用同一段录音自动走备用 ASR。

---

## 结论

推荐做法：**串行 fallback，不做并行双 ASR**。

也就是默认 ASR 先在明确的 primary 预算内跑完它自己的连接、重试、replay、final timeout 逻辑。只有默认 ASR 最终确认失败，才用同一段原始 PCM 启动 fallback。这样不会让两个云端 ASR 同时消耗连接和额度，也不会把 partial HUD、LLM、粘贴路径搞成竞态。

第一版建议支持：

```text
Primary ASR:  local / baidu / qwen / mimo / volcengine / doubao_ime
Fallback ASR: disabled / local / baidu / qwen / mimo
```

暂缓把 `volcengine` 和 `doubao_ime` 作为 fallback 目标。原因不是不能做，而是它们现在的录音回放识别逻辑藏在 streaming session 内部，且分别带有火山持久连接、Doubao 凭据写回、partial 聚合等 provider 生命周期。第一版把它们作为 primary 支持即可，失败后 fallback 到 batch-capable 后端。等 v1 稳定后，再抽出 `RecognizeRecordedPcm()` helper 支持它们作为 fallback 目标。

2026-06-29 复审后补充三条实施硬约束：

- streaming session 的 final callback **只允许投递主窗口消息**，不能在 session worker 线程里直接运行 fallback。
- 主窗口收到 primary final/timeout 后，fallback batch 也必须放到后台 worker 跑，不能阻塞 UI/window proc。
- ASR final 和 LLM final 的消息 payload 要携带最终 backend config/usedFallback/primaryError，不能继续完全依赖全局 `g_config` 做 debug 和日志判断。

---

## 当前事实

### 1. 完整原始 PCM 已经可用于 fallback

两条录音路径都会先把 16kHz / s16le / mono PCM 写入 `g_audioData`：

- waveIn: `src/audio/engine.cpp` 的 `WaveInProc()`。
- WASAPI: `src/audio/wasapi_capture.cpp` 的 capture loop。

streaming 模式下，即使 Qwen/Volc 走 `StreamingVadTrimmer` 上传裁剪后的 chunk，原始 PCM 仍先写入 `g_audioData`。`StopAudioCapture()` 返回这段完整 PCM 并清空缓存。因此 fallback 应使用 `StopAudioCapture()` 返回的原始 PCM，而不是复用 primary streaming 已裁剪/已上传的音频。

这一点很重要：

- Doubao IME 当前故意绕过本地 VAD，不能把 streaming VAD 输出当成统一输入。
- Qwen/Volc streaming VAD 只用于 primary 上传优化，fallback 后端应该自己按 batch 规则决定是否 VAD trim。
- `No speech detected` 应保持原有语义，不能因为 primary VAD 没检测到语音就盲目 fallback。

### 2. Batch session 已经适合做 fallback 执行层

`src/asr/asr_session.cpp` 已有 `CreateBatchAsrSession()`，目前覆盖：

- Local
- Baidu batch
- Qwen realtime batch path
- MiMo batch

它返回 `AsrSessionResult`，包含 text、backend、cloudApiMs、VAD trim stats 等。fallback 执行可以直接复用这个接口，避免在 `main.cpp` 写 provider 协议逻辑。

需要注意：当前 Qwen batch session 没有像 Baidu/MiMo 一样接 `BatchVadTrimmer`。如果把 Qwen 作为 fallback target，建议顺手补齐 batch VAD trim，使 batch cloud 后端语义一致。

### 3. Streaming session 现在自己 dispatch final，fallback 插不进去

Qwen、Volcengine、Doubao IME 的 streaming session 都继承 `StreamingAsrSessionBase`，最终通过：

```cpp
DispatchFinal(text)
```

直接调用 `DispatchAsrFinalText()`，然后发 `kAsrResultMessage`。这条路径到 UI 时只剩 text，没有 PCM，也没有 primary/fallback 上下文。

因此 fallback 不能放在 `kAsrResultMessage` handler 里。应该在 streaming final 完成点前接管：

```text
Streaming session final
-> main/orchestrator receives final text + attempt id
-> classify
-> maybe run fallback with stored raw PCM
-> only selected final result enters DispatchAsrFinalText()
```

### 4. 现有失败判断是字符串前缀

`src/asr/asr_result.cpp` 的 `IsOperationalAsrError()` 目前用字符串前缀识别错误，例如：

- `ASR failed:`
- `Baidu ASR error:`
- `Qwen ASR error:`
- `MiMo ASR error:`
- `Doubao IME ASR error:`
- `VolcEngine connect failed`
- `[VolcEngine error:`

这足够防止错误文本被粘贴，但不足以做 fallback 策略。fallback 需要区分：

- 可用文本
- 用户/音频类无结果：Too short、No speech detected
- 运行类错误：网络、超时、鉴权、模型加载、provider error
- 主动取消：aborted、被新 session 取代、程序退出

### 5. 现有 timeout 已经分层

当前不是没有 timeout，而是分布在 provider 内部：

| 层 | 当前行为 |
| --- | --- |
| 公共 cloud final | 当前为 `ComputeCloudAsrFinalizeTimeoutMs(audioMs * 0.8 + 6000, clamp 8000..30000)`；fallback 方案中应缩短并拆分语义 |
| Qwen connect | 8s hard connect timeout + 5s session.updated timeout |
| Qwen streaming final | adaptive final timeout，失败后 replay retry |
| Volc connect | open hard timeout: 3000, 3000, 5000, 6000ms |
| Volc opening while finalizing | 当前 final watchdog 至少 10000ms；fallback 方案中建议降到 8000ms |
| Volc final | adaptive final timeout + empty final replay retry |
| Doubao connect | WinHTTP connectTimeoutMs 默认 10000ms + session ready 8000ms |
| Doubao final | adaptive final timeout + replay retry |
| Baidu | token request 5000ms，ASR request 8000ms，token/transient retry |
| MiMo | adaptive final timeout + 12000ms，clamp 15000..60000，transient retry |

fallback 不应抢在这些 provider 内部 retry 前启动。否则会出现 primary 其实能恢复，但 fallback 已经粘贴了另一份文本的问题。

复审结论：现有 `8s..30s` 的 streaming final 等待对 fallback 场景偏长，尤其 20 秒录音后还等 22 秒、30 秒以上还等 30 秒，体感上会像卡死。实现 fallback 时应把“松开后等 primary final 的 UI 上限”缩短到约 `6s..12s`，同时保留 batch/replay 请求自己的较长网络预算，避免一个函数同时承担两种语义。

---

## Fallback 触发规则

### 触发 fallback

默认 ASR 最终结果归类为 `OperationalError` 时触发：

- 网络错误，例如 WinHTTP send/receive/connect failed。
- provider HTTP 5xx、429、408 等经过内部 retry 后仍失败。
- WebSocket connect/session ready/final transcript timeout。
- 火山 `OpenSession` 失败、final timeout。
- Doubao token/bootstrap/WebSocket transient 失败且内部 retry 用尽。
- 鉴权或配置错误，例如 missing API key、401、token/auth failed。
- local model load error，例如 `ASR failed: model load error`。
- provider 返回明确 error frame，例如 `[VolcEngine error: ...]`。

### 不触发 fallback

这些不是默认 ASR “失败”，不应 fallback：

- `Too short`
- `No speech detected`
- 空文本经 `NormalizeAsrText()` 变成的 `No speech detected`
- 用户主动取消、程序退出、被新录音 session abort 的结果
- primary 已返回可用文本，即使识别质量主观上不好
- fallback backend 为空或与 primary backend 相同
- fallback 已经执行过一次，禁止循环 fallback

需要特别注意：`aborted` 只有在用户退出、开始新录音、watchdog 主动终止旧 session 之后才应归为 `Cancelled`。如果 provider 内部把正常网络中断包装成 `... aborted`，分类前要确认它不是由 VoxType 自己发起的 abort。否则可能误把真实失败跳过 fallback。

### 建议新增分类

在 `src/asr/asr_result.h/.cpp` 新增结构化分类，保留旧函数兼容：

```cpp
enum class AsrResultKind {
    UsableText,
    NoSpeech,
    TooShort,
    OperationalError,
    Cancelled,
};

enum class AsrFailureReason {
    None,
    Timeout,
    Network,
    AuthOrConfig,
    ModelLoad,
    ProviderError,
    BufferOverflow,
    Unknown,
};

struct AsrResultClassification {
    AsrResultKind kind = AsrResultKind::UsableText;
    AsrFailureReason reason = AsrFailureReason::None;
};

AsrResultClassification ClassifyAsrResult(const std::wstring& text);
bool ShouldFallbackAsrResult(const Config& config,
                             const std::wstring& primaryBackend,
                             const std::wstring& text);
```

`IsOperationalAsrError()` 可以继续调用 `ClassifyAsrResult()`，避免改动面太大。

建议把 fallback eligibility 设计成显式函数，而不是散落在 `RecognizeAsync()`、watchdog、streaming callback 里：

```cpp
bool IsSupportedFallbackBackend(const std::wstring& backend);
Config BuildFallbackConfig(const Config& primary);
bool ShouldRunFallback(const Config& primary,
                       const std::wstring& text,
                       bool fallbackAlreadyAttempted,
                       bool selfAbortOrStaleAttempt);
```

这样 Settings 保存校验、runtime 校验、测试都能复用同一套规则。

---

## 超时策略

### 基本原则

fallback 启动时机是：

```text
primary 内部 retry/replay 完成
AND primary 最终给出 OperationalError
AND fallback backend 可用
```

不做：

- 不和 primary 并行抢跑。
- 不在录音期间启动 fallback。
- 不因为 primary partial 长时间不更新就 fallback。
- 不给 local decode 强行加线程杀死式 timeout。

### 推荐调整：缩短松开后的 final 等待

现有公式：

```cpp
audioMs * 0.8 + 6000, clamp 8000..30000
```

在 fallback 场景偏保守。用户已经配置备用 ASR 后，primary 再等 20 到 30 秒才失败，体验上像程序卡住，也削弱 fallback 的意义。第一版建议采用双策略：

- fallback 未启用时：暂时保留现有 `8s..30s` 逻辑，降低对老行为的回归风险。
- fallback 启用时：松开后 primary streaming final 最多等待 `6s..12s`，到点即走统一 timeout completion handler，然后启动 fallback。

建议新增一个专门用于 post-stop streaming final 的函数，不要继续让同一个 `ComputeCloudAsrFinalizeTimeoutMs()` 同时服务 UI watchdog、batch 全量请求和 replay 请求：

```cpp
DWORD ComputeCloudAsrStreamingFinalWaitMs(double recordingMs, size_t pcmBytes) {
    const double audioMs = pcmBytes > 0
        ? static_cast<double>(pcmBytes) / kPcm16k16MonoBytesPerMs
        : recordingMs;
    const DWORD adaptive = static_cast<DWORD>(audioMs * 0.25 + 4500.0);
    return std::clamp<DWORD>(adaptive, 6000, 12000);
}
```

对应体感：

| 录音长度 | 当前等待 | fallback 启用后建议等待 |
| --- | ---: | ---: |
| 1 秒 | 8 秒 | 6 秒 |
| 5 秒 | 10 秒 | 6 秒 |
| 10 秒 | 14 秒 | 7 秒 |
| 20 秒 | 22 秒 | 约 10 秒 |
| 30 秒以上 | 30 秒 | 最多 12 秒 |

Volcengine 的 `kVolcOpeningFinalizeWatchdogMs` 建议从 `10000` 降到 `8000`。原因是 `OpenSession` 自己已有 `3000/3000/5000/6000ms` hard timeout，fallback 启用后不应再因为“松开时还在 opening”强制把短录音等待抬到 10 秒。Volc opening 场景下的最终等待约为：

| 录音长度 | fallback 启用后，Volc 仍在 opening |
| --- | ---: |
| 1 秒 | 8 秒 |
| 5 秒 | 8 秒 |
| 10 秒 | 8 秒 |
| 20 秒 | 约 10 秒 |
| 30 秒以上 | 最多 12 秒 |

实现时要明确区分三类 timeout：

```cpp
DWORD ComputeCloudAsrLegacyFinalizeTimeoutMs(double recordingMs, size_t pcmBytes);
DWORD ComputeCloudAsrStreamingFinalWaitMs(double recordingMs, size_t pcmBytes);
DWORD ComputeCloudAsrRecordedRequestTimeoutMs(double recordingMs, size_t pcmBytes);
```

- `StopRecordingSession()` 设置 `kStreamingWatchdogTimer`：fallback 启用时用 `ComputeCloudAsrStreamingFinalWaitMs()`；未启用时可继续用 legacy。
- Qwen/Doubao streaming `SendFinish` 后等 final：用同一个 post-stop final deadline，不能比主窗口 watchdog 更长。
- Volc `CurrentWatchdogMs()`：fallback 启用时用短 final wait，opening guard 降为 8000ms。
- Qwen batch、MiMo batch、recorded replay：不要被短 UI watchdog 误伤，继续使用 recorded request timeout 或 provider 自己的 request timeout。

重要约束：streaming provider 内部 retry/replay 不能在每次 retry 时重新获得一个完整 `12s` 窗口。主窗口 post-stop watchdog 是 primary 的总上限；retry 只能在剩余时间内完成。到点后 abort primary，生成 timeout result，再由 fallback handler 决定是否启动 fallback。

### Batch primary

batch primary 由 `RecognizeAsync()` worker 执行：

```text
CreateBatchAsrSession(primary)
-> Start()
-> EnqueuePcmChunk(raw pcm)
-> Finish()
-> classify primary result
-> maybe CreateBatchAsrSession(fallback)
-> Dispatch selected final
```

触发点就是 `Finish()` 返回后。Baidu/MiMo/Qwen batch 内部已经有自己的 request timeout 和 retry，fallback 不应插入其中。

### Streaming primary

streaming primary 有两种失败出口：

1. session worker 自己产出 final error。
2. `kStreamingWatchdogTimer` 在 StopInput 后到期，main abort session 并生成 timeout error。

两者都应走同一个 completion handler：

```text
HandleAsrAttemptFinal(attemptId, primaryConfig, text, metrics)
```

如果 result 可 fallback，则异步启动 batch fallback。fallback 期间 HUD 显示：

```text
Fallback... Local ASR
Fallback... Baidu Cloud
Fallback... Qwen ASR
Fallback... MiMo ASR
```

### 录音期间不 fallback

当前 streaming 录音期间 watchdog 是 18s，用于保持 session 状态和重设 timer。只要用户还在按住热键，fallback 不能启动，因为 fallback 需要完整录音 PCM。

如果 primary 在录音期间连接失败，现有 session 会 `WaitForRecordingStop()`，等用户松开后才 dispatch error。这个语义应保留：松开后再 fallback。

### local primary 的 timeout

local ASR 的现实失败主要是模型加载失败。模型 decode 卡死理论上可能，但当前 sherpa-onnx 调用没有安全取消机制。第一版不建议给 local decode 做“超时后另起 fallback 并让 local 线程继续跑”的设计，这会引入：

- 模型锁竞争
- 后续结果乱序
- 无法安全终止 native decode

第一版只处理 local 明确返回的 `ASR failed: model load error`。

---

## 配置和 UI 方案

### Config

`src/app/globals.h` 增加：

```cpp
std::wstring fallbackAsrBackend = L"none";
```

`src/audio/engine.cpp`：

- `kCurrentConfigVersion` 从 6 升到 7。
- `LoadConfig()` 读取 `fallback_asr_backend`，默认 `none`。
- `SaveConfig()` 写出 `fallback_asr_backend`。
- 迁移规则：旧配置没有该字段时为 `none`。

### Recognition tab

新增控件：

```cpp
IDC_ASR_FALLBACK_BACKEND
```

放在 Recognition tab 第一行右侧，而不是新增一整行。原因：

- Recognition 当前已经使用到 `RowInputY(9)`。
- tab/页脚空间依赖 `UiStyle::FooterMinTop`。
- 新增一行会挤压 VAD group 和 Punctuation 行，增加高 DPI 裁切风险。

推荐布局：

```text
ASR Backend    [primary combo              ]   Fallback [fallback combo]
```

fallback combo 选项：

```text
Disabled
Local (sherpa-onnx)
Baidu Cloud
Qwen ASR
MiMo ASR
```

不要在第一版 UI 里提供 `Volcano Engine` / `Doubao IME` 作为 fallback target，避免用户选到尚未抽出 recorded-pcm helper 的 streaming provider。

保存时校验：

- fallback 与 primary 相同：保存为 `none`，状态提示 `Fallback disabled because it matches ASR Backend.`
- fallback 为不支持值：保存为 `none`。
- `cloudProvider` 不随 fallback 变化。Cloud ASR tab 仍只是配置 provider 参数的页面。

布局常量应进入 `UiStyle` 命名空间，不要把新位置硬编码成孤立魔法数字。

建议抽公共 backend 映射：

```cpp
struct BackendOption {
    const wchar_t* id;
    const wchar_t* label;
    bool primarySupported;
    bool fallbackSupported;
};
```

primary combo 和 fallback combo 都从同一张表填充，避免后续新增 provider 时两个下拉框顺序不一致。fallback combo 的第 0 项为 `none/Disabled`，其余只加入 `fallbackSupported == true` 的项。

UI 细节：

- primary combo 现宽 330。如果第一行右侧加 fallback，primary 宽度可能要收窄到约 260，fallback combo 约 210，具体以 150% DPI 实测为准。
- 新 label 不要用太长文本，建议 `Fallback`。
- fallback combo 的下拉高度用 `S(UiStyle::ComboH)`，不要只给控件 32px 高导致下拉列表不可用。

---

## 编排设计

### 1. 新增 attempt 上下文

需要避免旧 session 的 late final 影响新录音。建议在 `main.cpp` 增加单调递增 attempt id：

```cpp
static std::atomic<uint64_t> g_asrAttemptSeq{0};
static uint64_t g_activeAsrAttemptId = 0;
static std::shared_ptr<const std::vector<BYTE>> g_lastAttemptPcm;
```

更严谨可以包成：

```cpp
struct RecognitionAttemptContext {
    uint64_t id = 0;
    Config primaryConfig;
    Config fallbackConfig;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    bool fallbackEnabled = false;
    bool stopped = false;
};
```

访问需要用 mutex 或 critical section，不能让 streaming worker 和 UI timer 同时读写 PCM/context。

### 2. 抽出 batch 识别 helper

建议新增 helper，供 primary batch 和 fallback batch 共用：

```cpp
AsrSessionResult RunBatchAsrOnce(const Config& config,
                                 const std::vector<BYTE>& pcm,
                                 std::vector<float>&& localPreprocessedSamples);
```

fallback 调用时始终传原始 PCM，`localPreprocessedSamples` 为空，让 fallback backend 自己走 batch VAD。

### 3. Batch primary fallback 流程

当前 `RecognizeAsync()` 可以改为：

```text
copy config snapshot
copy fallback config snapshot
worker:
  primary = RunBatchAsrOnce(primaryConfig, pcm, localStreamingVadSamples)
  if ShouldFallback(primary):
      Post HUD "Fallback... <fallback>"
      fallback = RunBatchAsrOnce(fallbackConfig, pcm, {})
      Dispatch selected fallback result
  else:
      Dispatch primary result
```

fallback config 的构造：

```cpp
Config fallback = primary;
fallback.asrBackend = primary.fallbackAsrBackend;
```

注意：

- fallback 复用同一套 provider keys / model settings / VAD settings / LLM settings。
- fallback 不应继承 primary 的 streaming VAD 预处理 samples。
- fallback 成功后 LLM refine 应针对 fallback final text 跑一次。
- primary 失败时不能先 dispatch 给 UI，否则 UI 会显示错误并隐藏 HUD，然后 fallback 再更新，体验抖动。

### 4. Streaming primary fallback 流程

需要给 streaming session 增加 final completion 回调。

最小侵入方案：

```cpp
using AsrFinalCallback = void(*)(std::wstring text, void* userData);

class IStreamingAsrSession {
public:
    virtual void SetFinalCallback(AsrFinalCallback cb, void* userData) = 0;
};
```

`StreamingAsrSessionBase::DispatchFinal()` 改为只转交 final。注意：这个函数通常运行在 provider worker 线程里。

```cpp
if (finalCallback_) {
    finalCallback_(std::move(text), finalUserData_);
    return;
}
DispatchAsrFinalText(...);
```

main 在创建 Qwen/Volc/Doubao session 时设置 final callback，callback 携带 attempt id。session 成功或失败都只通知 orchestrator，不直接 dispatch。

**强约束：final callback 不能直接运行 fallback。**

推荐 callback 只做一件事：`PostMessageW(g_mainWindow, kAsrAttemptFinalMessage, ...)`。然后由主窗口线程判断是否 stale、是否需要 fallback，再把 fallback batch 丢到后台 worker。

新增主窗口消息建议：

```cpp
constexpr UINT kAsrAttemptFinalMessage = WM_APP + 10;
```

Settings 里当前也局部使用 `WM_APP + 10 / +11 / +20`，它们只投递给 Settings window，本身不冲突。但实现时要在注释里写清楚：`kAsrAttemptFinalMessage` 属于 main window，Settings 的局部测试消息属于 Settings window，不要混用。

payload 建议：

```cpp
struct AsrAttemptFinalMessage {
    uint64_t attemptId = 0;
    Config primaryConfig;
    std::wstring text;
    bool fromWatchdog = false;
};
```

`lParam` 用 `new AsrAttemptFinalMessage`，main window handler 用 `unique_ptr` 接管。不要把指向 stack/context 内部字符串的裸指针传给 PostMessage。

Stop 时：

```text
pcm = StopAudioCapture()
store pcm into active attempt context
session->StopInput(recordingMs, pcm.size())
reset finalize watchdog
```

如果 streaming session `Start()` 失败：

```text
StopAudioCapture()
clear/finish attempt context
classify startError
maybe run fallback only if pcm is long enough and fallback enabled
otherwise show/dispatch startError
```

当前 Qwen/Doubao start 基本只启动 worker，不太会同步失败；Volc start 分支现在直接 `DispatchAsrFinalText(startError)`。实现 fallback 时这条路径也应改成统一 completion handler，避免 Volc start failed 绕过 fallback。

如果 `StopRecordingSession()` 判定 `Too short` 或 streaming VAD `No speech detected`：

```text
mark attempt finalHandled = true
abort/reset active streaming session
clear stored pcm
show HUD directly
do not fallback
```

否则旧 session worker 若稍后 callback，可能被当成同一 attempt 的 primary error 触发 fallback。

watchdog timeout 时：

```text
session = TakeActiveStreamingSession()
providerName = session->ProviderName()
session->Abort()
Post kAsrAttemptFinalMessage(attemptId, timeoutText, fromWatchdog=true)
```

注意顺序：`ProviderName()` 必须在 `session->Abort()` 前取出。`Abort()` 可能 join worker 并析构/移动 session；不要 abort 后再读 session 内部状态。

completion handler：

```text
if stale attempt id: ignore
if no pcm and result needs fallback: dispatch primary error
if ShouldFallback(primary text): start fallback batch worker
else DispatchAsrFinalText(primary text)
```

主窗口 handler 只做轻量判断和启动 worker，不能同步跑 `RunBatchAsrOnce()`。

### 5. 防止 duplicate final 和 stale final

streaming fallback 引入两个 final 来源：

- session worker 自己完成并发 final。
- main watchdog 超时后 abort session 并发 timeout final。

必须保证同一个 attempt 只处理一次 final。建议 attempt context 增加：

```cpp
bool finalHandled = false;
bool fallbackStarted = false;
bool selfAbort = false;
```

处理规则：

```text
StartRecordingSession:
  increment attempt id
  invalidate previous attempt
  selfAbort previous active session

StopRecordingSession:
  store raw pcm in context before StopInput

kAsrAttemptFinalMessage:
  lock context
  if attempt id != active id: ignore
  if finalHandled: ignore
  finalHandled = true
  copy pcm/config/error out of context
  unlock
  classify and maybe start fallback worker

watchdog timeout:
  mark selfAbort/timeout state for current attempt only
  take session
  post timeout final through same message path
```

如果新录音开始时调用 `AbortAndResetActiveStreamingSession()`，这属于 self-abort/stale cleanup，不应触发 fallback。旧 session 若随后产生 final callback，attempt id 已过期，直接忽略。

fallback worker 完成后也要检查 attempt id 仍然有效再 dispatch，避免用户在 fallback 识别期间又开始了新录音。

### 6. Debug/metrics

当前 UI result message 只带字符串，debug 输出用全局 `g_config.asrBackend`。fallback 后容易误报 backend。

第一版至少需要增加一条 fallback debug summary：

```text
Primary: Qwen ASR error: timed out waiting for final transcript
Fallback: Local 186ms | Paste 32ms = Total ...
```

实施前复审建议：不要再把 payload 保持为裸 `std::wstring`。fallback 后最终 backend 可能不是 `g_config.asrBackend`，LLM 结果回来时也需要知道它 refine 的是哪一个 backend。建议把 `kAsrResultMessage` 的 payload 从 `std::wstring` 升级为：

```cpp
struct AsrFinalMessage {
    std::wstring text;
    Config resultConfig;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
    std::wstring fallbackBackend;
};
```

这能让 debug、HUD、LLM、后续日志都知道最终文本来自哪个 backend。改动略大，但比继续依赖全局 `g_config` 更稳。

同时把 LLM 结果 payload 也升级，至少携带 refine 时使用的 config 和 fallback 元信息：

```cpp
struct LlmFinalMessage {
    std::wstring text;
    Config resultConfig;
    bool usedFallback = false;
    std::wstring primaryBackend;
    std::wstring primaryError;
};
```

否则 `kLlmResultMessage` handler 仍会用当前全局 `g_config` 打 pipeline，用户在 LLM 请求期间改 Settings 或新录音时，debug 行可能归属错误。

兼容落地顺序：

1. 先新增 `PostAsrFinalMessage()` helper，统一封装 `DispatchAsrFinalText()` 内的 PostMessage。
2. `DispatchAsrFinalText()` 接收可选 metadata，默认 metadata 为空，旧调用路径仍可编译。
3. `RefineWithLlmAsync()` 捕获并回传 `AsrFinalMessage` 中的 `resultConfig` / fallback metadata。

---

## 预加载和预热

启动和 Settings 保存后，当前只在 `asrBackend == local` 时预加载本地模型。fallback 支持 local 后要改成：

```text
if primary is local OR fallback is local:
    PreloadAsrEngine(configWithLocalBackend)
```

否则默认云端失败后第一次 fallback local 会遇到冷启动模型加载延迟。

`configWithLocalBackend` 不要直接传云端 primary config。应复制一份 config 并设置：

```cpp
Config localPreloadConfig = g_config;
localPreloadConfig.asrBackend = L"local";
```

这样 debug/display 语义清楚，也避免后续 `PreloadAsrEngine()` 如果读取 backend 时行为不确定。

`kReloadMessage` 里同样要按 primary/fallback 检查 local 预加载。Settings Save 后如果 primary 是云端、fallback 是 local，也应触发 preload。

Volcengine 作为 fallback target 暂缓，所以第一版不需要为 fallback Volc 做 prewarm。如果第二版支持 `volcengine` fallback target，再把：

```text
primary is volcengine OR fallback is volcengine
```

纳入 `VolcenginePrewarmConnection()` 条件。

---

## 实施步骤

### Phase 1：基础配置和 UI

改动文件：

- `src/app/globals.h`
- `src/audio/engine.cpp`
- `src/ui/settings.cpp`

任务：

- 新增 `fallbackAsrBackend`。
- 配置读写 `fallback_asr_backend`。
- Recognition tab 增加 fallback combo。
- 增加 backend combo 映射 helper，避免 primary/fallback 各写一套 if/else。
- 保存时校验 same-backend 和 unsupported 值。

验证：

- Settings 打开不裁切。
- fallback 选择能保存/重开恢复。
- 旧 config 没有字段时默认为 Disabled。

### Phase 2：结果分类和 batch fallback

改动文件：

- `src/asr/asr_result.h`
- `src/asr/asr_result.cpp`
- `src/asr/asr_session.cpp`
- `src/asr/asr_dispatcher.h`
- `src/asr/asr_dispatcher.cpp`
- `src/app/main.cpp`

任务：

- 新增结构化 result classification。
- 保留 `IsOperationalAsrError()` 兼容旧调用。
- 升级 ASR/LLM final message payload，确保 debug 不依赖全局 `g_config`。
- 抽 `RunBatchAsrOnce()`。
- `RecognizeAsync()` 支持 primary batch 失败后 fallback batch。
- 给 Qwen batch path 补 `BatchVadTrimmer`，保持 cloud batch 行为一致。

验证：

- primary local 模型目录错误，fallback Baidu/Qwen/MiMo 按预期执行。
- primary Baidu missing key，fallback local 执行。
- primary MiMo missing key，fallback local 执行。
- primary Qwen missing key，fallback local 执行。
- primary 返回 `No speech detected` 不 fallback。
- fallback 失败时不粘贴错误文本。
- fallback 成功后 Debug Mode backend 显示 fallback backend，不显示 primary backend。

### Phase 3：streaming final callback 和 watchdog fallback

改动文件：

- `src/asr/asr_streaming_session.h`
- `src/asr/asr_streaming_session_base.h`
- `src/asr/cloud_asr_common.h`
- `src/asr/cloud_asr_common.cpp`
- `src/asr/qwen_streaming_session.cpp`
- `src/asr/volcengine_streaming_session.cpp`
- `src/asr/doubao_ime_streaming_session.cpp`
- `src/app/main.cpp`

任务：

- 给 streaming session 增加 final callback。
- StartRecordingSession 创建 attempt context。
- StopRecordingSession 存储 raw PCM 到 attempt context。
- 拆分 legacy finalize timeout、streaming final wait、recorded request timeout。
- fallback 启用时 streaming post-stop watchdog 使用 `6s..12s` 短等待。
- Volc opening finalize guard 从 `10000ms` 降到 `8000ms`。
- provider 内部 retry/replay 不能重置 post-stop 总等待窗口。
- session final error 走 fallback handler。
- streaming finalize watchdog timeout 走同一 fallback handler。
- 新录音开始时使旧 attempt 失效，防止 late final。

验证：

- Qwen 断网/错 key/timeout 后 fallback local。
- Volc connect failed/final timeout 后 fallback local。
- Doubao token/bootstrap/WebSocket 失败后 fallback local。
- fallback 启用时，松开后 primary streaming 等待约为：1s/5s 录音最多 6s，10s 录音约 7s，20s 录音约 10s，30s+ 最多 12s。
- Volc 松开时仍在 opening：短录音最多约 8s，不再强制等 10s。
- primary partial HUD 曾显示文本时，fallback final 能正常覆盖并粘贴。
- 新录音 abort 旧 session 后，旧 session late final 不触发 fallback。

### Phase 4：debug 和体验打磨

任务：

- HUD fallback 状态文案。
- Debug Mode 输出 primary error + fallback pipeline。
- fallback 成功时 `g_lastRawAsrText` 应是 fallback final text，LLM 日志也应记录 fallback text。
- 如果 fallback 失败，HUD 显示短错误，详细 primary/fallback error 打 debug log。

建议 HUD 短文案：

```text
Fallback... Local ASR
Fallback failed: Local ASR
```

避免把很长的 provider error 全塞 HUD。

### Phase 5：可选，支持 Volc/Doubao 作为 fallback target

暂缓原因：

- Volc 的 recorded replay helper 目前是 `VolcengineStreamingSession::RetryRecognitionOnce()` 私有逻辑。
- Doubao 的 replay helper 目前是 `DoubaoImeStreamingSession::RetryRecognitionOnce()` 私有逻辑，且涉及凭据刷新和写回。
- 两者都不是简单调用 `CreateBatchAsrSession()`。

后续要支持时，建议新增：

```cpp
std::wstring VolcengineRecognizeRecordedPcm(const Config& config,
                                            const std::vector<BYTE>& pcm,
                                            DWORD finalTimeoutMs);

std::wstring DoubaoImeRecognizeRecordedPcm(const Config& config,
                                           const std::vector<BYTE>& pcm,
                                           HWND targetWindowForCredentialWriteback,
                                           DWORD finalTimeoutMs);
```

抽取时只机械移动已有 retry/replay 逻辑，不重写协议 frame、send/drain、Prewarm/CloseSession 行为。

---

## 关键边界

### 不要在 `kAsrResultMessage` 里触发 fallback

原因：

- 到这里已经没有原始 PCM。
- LLM refine 可能已经开始。
- UI handler 会显示 HUD、启动 hide timer、paste。
- 容易出现先显示错误再 fallback 粘贴的抖动。

fallback 必须在 final dispatch 前完成。

### 不要让音频回调参与 fallback

音频回调继续只做：

- 写 `g_audioData`
- 更新音量
- streaming session `EnqueuePcmChunk()`
- 可选 streaming VAD trim

不能在 WASAPI/waveIn 回调里做 provider 选择、fallback 判断、网络请求。

### fallback 使用原始 PCM

无论 primary 是否使用 streaming VAD，fallback 都使用 raw PCM。这样各 backend 的 VAD 行为保持局部、可解释。

### fallback 只执行一次

不要做链式 fallback：

```text
primary -> fallback1 -> fallback2
```

第一版只允许：

```text
primary -> fallback
```

否则错误处理、HUD、debug、用户预期都会变复杂。

---

## 失败显示策略

建议最终用户可见规则：

| 场景 | HUD | 粘贴 |
| --- | --- | --- |
| primary 成功 | final text | 是 |
| primary No speech | No speech detected | 否 |
| primary Too short | Too short | 否 |
| primary 失败，fallback 成功 | fallback final text | 是 |
| primary 失败，fallback No speech | No speech detected | 否 |
| primary 失败，fallback 也失败 | Fallback failed: backend name | 否 |
| primary 失败，fallback disabled | primary short error | 否 |

Debug Mode 输出完整细节：

```text
Primary failed: Qwen ASR error: timed out waiting for final transcript
Fallback: Local ASR
Fallback result: ...
```

复审建议：fallback `No speech detected` 应优先显示 `No speech detected`，不要回显 primary 网络错误。因为这说明备用 ASR 在同一段原始 PCM 上也没有识别出文本，对用户来说比 “Qwen timeout” 更接近当前操作结果。Debug Mode 再保留 primary error。

fallback 也失败时 HUD 用短文案，详细错误走 Debug Mode。否则 API key、HTTP body、长错误消息会挤爆 HUD。

---

## 测试清单

### 构建

- `.\build.bat`
- 如果链接失败提示 `build\VoxType.exe` 被占用，先停进程再构建：

```powershell
Get-Process VoxType -ErrorAction SilentlyContinue | Stop-Process -Force
.\build.bat
```

### 配置迁移

- 删除 `fallback_asr_backend` 字段后启动，确认默认为 Disabled。
- 设置 fallback 后保存，重开 Settings 后值仍在。
- primary 与 fallback 相同时保存为 Disabled 或提示并拒绝保存，二选一，但行为必须明确。

### Batch primary

- Local 模型路径改错，fallback Local disabled 时显示 primary error。
- Local 模型路径改错，fallback Baidu/Qwen/MiMo 时走 fallback。
- Baidu API key 清空，fallback Local。
- MiMo API key 清空，fallback Local。
- Qwen API key 清空，fallback Local。
- 说话太短，显示 Too short，不 fallback。
- 静音，显示 No speech detected，不 fallback。

### Streaming primary

- Qwen 错 key，松开后 fallback Local。
- Qwen 断网，松开后 fallback Local。
- Qwen final timeout，watchdog abort 后 fallback Local。
- fallback 启用时模拟 final 不返回：1s/5s/10s/20s/30s 录音的 post-stop watchdog 分别约为 6s/6s/7s/10s/12s。
- Volc opening 未完成时松开：短录音最多约 8s 后进入 timeout/fallback。
- Volc 错 key或资源错误，fallback Local。
- Volc connect timeout，fallback Local。
- Doubao IME 凭据失败且刷新失败，fallback Local。
- Doubao IME 网络失败，fallback Local。
- streaming primary partial HUD 已显示文本时，fallback final 仍能覆盖并正常粘贴。

### 并发和生命周期

- 一次 streaming 还在 finalize 时，立刻开始下一次录音，旧 final 不应粘贴，也不应 fallback。
- Settings 打开时不拦截快捷键的现有行为不变。
- 退出程序时 abort session 不触发 fallback。

### Debug/LLM

- fallback 成功后 Debug Mode 标出 primary failed + fallback backend。
- fallback final text 才进入 LLM refine。
- LLM debug log 记录的是 fallback ASR text，而不是 primary error。

### UI

- 100%、150%、200% DPI 检查 Recognition tab：
  - ASR Backend/Fallback 不重叠。
  - VAD group 不裁切。
  - Punctuation 行仍可见。
  - Footer Save/Close 不遮挡。

---

## 建议提交边界

一次实现建议拆成 2 个提交：

1. `feat: add fallback asr config and batch fallback`
   - Config/UI/result classification/batch fallback。
   - 已可覆盖 local/Baidu/Qwen/MiMo primary 的 fallback。

2. `feat: route streaming asr failures through fallback`
   - streaming final callback、attempt context、watchdog fallback。
   - 覆盖 Qwen/Volc/Doubao primary。

如果代码量控制得住，也可以一个提交完成，但不要同时抽 Volc/Doubao recorded fallback helper。

---

## 文档和版本

真正实现功能时需要同步：

- `src/app/resource.h` 版本号。
- `README.md` 版本。
- `CHANGELOG.md` 记录。
- 如架构变化明显，同步 `ARCHITECTURE.md` 的 Settings/Cloud ASR/Configuration 章节。

新增 `.cpp` 文件时必须同步：

- `build.bat`
- `CMakeLists.txt`
- 如需新 lib，补 `#pragma comment(lib)`，不过本方案不需要新库。

---

## 暂不做

- 不做并行 primary/fallback race。
- 不做自动质量判断，例如“文本太短所以 fallback”。
- 不做 local decode 强制超时取消。
- 不把 LLM 作为 fallback 的判断依据。
- 不把 Qwen/Volc/Doubao send loop、drain thread、retry 抽成复杂模板。
- 第一版不支持 Volc/Doubao 作为 fallback target。
