# 按键录音头部/尾部音丢失优化方案

> 目标：消除 CapsLock 长按录音的头部音丢失（~350–550ms），并通过 150ms 停止延迟 + 停止时排干驱动缓冲消除尾部音丢失；再以设备保活（keep-alive 2.5s）消除短按/连续录音时的麦克风反复开关。
>
> 用户已拍板：**头部用方案 A（按下即录、300ms 确认后才弹 HUD）；尾部用方案 1（150ms 停止延迟）+ 方案 2（排干缓冲）；另加设备保活（停止/丢弃后 2.5s 内不关麦克风，复用免重开）。**

---

## 1. 问题诊断（已对照源码验证）

### 1.1 头部丢失（仅 CapsLock 路径）

三层叠加：

1. **长按判定窗口 300ms**：`kCapsLockLongPressMs = 300`（src/app/globals.h:73）。KEYDOWN 后 300ms 内完全不录音（src/ui/hotkey.cpp:162–197 `StartCapsLockHotkeyPress` / `ActivateCapsLockLongPress` / `FinishCapsLockHotkeyPress`）。
2. **消息队列延迟**：定时器到点后经 `PostHotkeyRecordingCommand` → 主线程消息循环才执行 `StartRecordingSession()`。
3. **设备打开延迟**：`StartAudioCapture()`（src/audio/engine.cpp:959）内 WASAPI Init/Start 还有几十到上百 ms。

合计按下后 ~350–550ms 才采到第一帧，而用户习惯边按边说，头部必丢。

### 1.2 尾部丢失（所有热键路径）

1. **松键即硬停**：`StopRecordingSession()` → `StopAudioCapture()`（engine.cpp:1161）立即停止设备。
   - WASAPI：`WasapiCapture::Stop()`（wasapi_capture.cpp:172）直接 `m_running=false` + join，驱动缓冲里采集线程尚未取走的包全部丢弃（最多几十 ms）。
   - waveIn：`waveInReset` 处理未满缓冲的行为依赖驱动，可能丢弃最多 ~100ms（每缓冲 100ms，engine.cpp:1074）。**注意**：MSDN 称 `waveInReset` 会把 pending 缓冲标记 done 并经回调返回，而 `WaveInProc`（engine.cpp:901）不检查 `g_captureActive` 就直接追加数据，所以 waveIn 路径**可能**已经能收回部分尾部——§4.3 用 dwUser 防重标记使两种驱动行为下都正确，无需实测先行。
2. **无尾部余量**：人的习惯是说完最后一个字的瞬间就松键，尾音落在采集窗口外。
3. **附带问题**：快按快放时 PCM < 8000 字节触发 "Too short" 整段丢弃（main.cpp:1857）。

---

## 2. 方案 A：按下即录，300ms 事后裁决（头部）

### 2.1 核心思路

把 CapsLock 路径从「先判定、后采集」改为「先采集、后判定」：

```
KEYDOWN ──→ 立即开始音频采集（只攒 PCM，不建 ASR 会话、不弹 HUD、不抓 selection）
   │
   ├─ 300ms 定时器到点（确认长按）──→ 弹 HUD + 建 ASR 会话，已录 PCM 灌入会话
   │
   └─ 300ms 内 KEYUP（判定短按）──→ 停止采集、丢弃 PCM、SendCapsLockTap（行为不变）
```

**关键架构依据**：`ActivateStreamingSession()`（main.cpp:1483–1515）已内置「回放存量 `g_audioData` PCM + 安装 session」机制（注释明确说明，且与两条采集回调保持相同的 `g_audioLock → g_streamingSessionCs` 锁顺序）。300ms 确认后建会话时，提前录的 PCM 会自动灌入流式会话；批量后端在 `StopAudioCapture()` 时天然包含全部 PCM。**无需为该方案新增 PCM 传输机制。**

### 2.2 状态机改动

新增一个「仅采集待确认」状态（建议 `g_capturePendingOnly` bool 或三态枚举）：

| 状态 | 含义 | 进入 | 退出 |
|---|---|---|---|
| Idle | 无采集 | 初始 / 丢弃完成 / 停止完成 | KEYDOWN |
| CapturePending | 采集进行中，未到 300ms，未建会话 | CapsLock KEYDOWN | 定时器到点 → Recording；KEYUP → Idle |
| Recording | 正常录音（现有 `g_recording`） | 300ms 确认 / 普通热键 KEYDOWN | KEYUP（经 §3 延迟）→ Idle |

`g_sessionStartTick` 改为在 **KEYDOWN 开始采集时**记录，使 `g_recordingMs` 与 Too-short 判断把预录段算进去（这对 Too-short 是良性影响）。

### 2.3 新增命令与常量

- globals.h：
  - 新增 `constexpr WPARAM kHotkeyCaptureBegin = 4;`（CapsLock KEYDOWN 即开始采集）
  - 新增 `constexpr WPARAM kHotkeyCaptureDiscard = 5;`（短按，丢弃采集）
  - `kCapsLockLongPressMs = 300` 保持不变（判定/HUD 时机不变）
- hotkey.cpp：
  - `StartCapsLockHotkeyPress()`：追加 `PostHotkeyRecordingCommand(kHotkeyCaptureBegin)`
  - `FinishCapsLockHotkeyPress()`：短按分支改为「先 post `kHotkeyCaptureDiscard`，再 `SendCapsLockTap()`」；长按分支不变
- main.cpp：
  - `kHotkeyRecordingMessage` handler 新增两个分支：`BeginCaptureOnly()` / `DiscardPendingCapture()`
  - `StartRecordingSession()` 拆分：若当前处于 CapturePending（采集已激活），跳过 `StartAudioCapture()` 与 HUD 之前的重复初始化，直接走「弹 HUD → selection capture → BeginAsrAttempt → 建会话」后半段

### 2.4 关键函数职责

- `BeginCaptureOnly()`（新）：调用 `StartAudioCapture()`；成功则置 CapturePending、记录 `g_sessionStartTick`；**不弹 HUD、不建会话、不抓 selection**。失败处理见 §2.5。
- `DiscardPendingCapture()`（新）：`StopAudioCapture()` 取出的 PCM 直接丢弃；清理 CapturePending 状态；**全程无 HUD、无 ASR 请求、无诊断上报**（诊断 capture 已在 `StartAudioCapture` 入口按配置 reset/cancel，短按丢弃时确认诊断侧也一并 cancel）。
- `StartRecordingSession()`（改）：入口增加「CapturePending → 复用已有采集」分支；HUD「Listening...」只在确认后显示（用户确认：按下不弹，300ms 才弹）。

### 2.5 边界情况

1. **采集在 pending 阶段打开失败**：记录错误；300ms 确认时 `StartRecordingSession` 检测到无活跃采集，重试一次 `StartAudioCapture`（会再次失败并走现有错误 HUD 路径）。短按则静默丢弃（用户只是切大小写，不该弹错误）。
2. **pending 阶段收到运行时采集失败消息**（`kAudioCaptureErrorMessage` / `kWaveInCaptureErrorMessage`）：`HandleAudioCaptureFailure` 目前假定 `g_recording`，需加 CapturePending 分支——按「采集终止 + 状态回 Idle + 记录日志」处理，300ms 定时器到点后按 §2.5-1 走重试。
3. **pending 阶段重复 KEYDOWN**（长按自动重复）：`StartCapsLockHotkeyPress` 已有 `g_activeHotkeyKey == VK_CAPITAL` 去重，天然免疫。
4. **pending 阶段按了别的录音热键**：普通热键 KEYDOWN 直接 post `kHotkeyRecordingStart`，`StartRecordingSession` 复用 pending 采集直接确认——行为合理，视为用户意图明确。
5. **应用退出 / 设置重载发生在 pending 阶段**：`ResetCapsLockHotkeyState` 之外，需确保退出路径也清理 CapturePending（`StopAudioCapture` 已在退出路径调用，确认状态标志一并复位）。
6. **VAD trimmer 时机**：`StartStreamingVadTrimmerForCloud` 仍在 300ms 建会话时启动，回放 PCM 会先过 trimmer 再入队（`ActivateStreamingSession` 现有逻辑），行为与现状一致。

---

## 3. 方案 1：松键延迟 150ms 停止（尾部，所有热键生效）

### 3.1 机制

- KEYUP 不再立即 `StopRecordingSession()`，改为启动 **150ms 停止定时器**，期间采集继续，尾音落进缓冲区。
- 定时器到点才真正停止。
- **150ms 内重新按下**：取消停止定时器，录音无缝继续（`g_recording` 仍为 true）。附带收益：「松一下手又接着说」不再断成两段。

### 3.2 改动点

- globals.h：
  - 新增 `constexpr UINT kRecordingStopDelayMs = 150;`（用户明确不要 300ms，150ms 即可）
  - 新增 `constexpr UINT_PTR kRecordingStopDelayTimer = 5;`
- main.cpp `kHotkeyRecordingMessage` handler：
  - `kHotkeyRecordingStop` / `kHotkeyCapsLockRecordingStop` 分支：不再直接调 `StopRecordingSession()`，改为记录停止类型（`g_stopDelayRestoreCapsLock = (wParam == kHotkeyCapsLockRecordingStop)`）并 `SetTimer(g_mainWindow, kRecordingStopDelayTimer, kRecordingStopDelayMs, nullptr)`；HUD 维持录音态（采集本来就没停）
  - `WM_TIMER` 新增 `kRecordingStopDelayTimer` 分支：KillTimer → `StopRecordingSession()` → 若 `g_stopDelayRestoreCapsLock` 则 `RestoreCapsLockState()` 并清标志
  - `StartRecordingSession()` 入口：**先 `KillTimer(kRecordingStopDelayTimer)` 并清标志**，再执行 `if (g_recording) return;`——保证延迟窗口内重新按下能续录
- hotkey.cpp：无需改动（停止命令仍由现有 keyup 路径 post）

### 3.3 边界情况

1. **延迟窗口内 CapsLock 重新按下**：走 §2 的 capture-begin → 已激活采集下 `BeginCaptureOnly()` 应为 no-op（用 `g_captureActive` / `g_recording` 判定）；300ms 确认后 `StartRecordingSession` 入口杀掉停止定时器并 early-return（`g_recording` 为 true）。注意 `ActivateCapsLockLongPress` 仍会置 `g_capsLockLongPressActive = true`，保证后续 KEYUP 走 CapsLock 停止分支。**旧的 `g_stopDelayRestoreCapsLock` 必须在新按下时被清除**，避免误恢复大小写状态。
2. **定时器到点时 `g_recording` 已为 false**（如采集失败路径已先停）：`StopRecordingSession` 现有 early-return 兜底，no-op。
3. **Too-short**：多收 150ms 降低了误触概率；判定逻辑不变。
4. **流式后端 FinishInput 时序**：停止流程整体后移 150ms，内部链路不变。
5. **连续触发**：150ms 内反复松/按，定时器反复 Kill/Set，不会叠加多个停止。

---

## 4. 方案 2：压缩停止瞬间的驱动内残留（尾部，与方案 1 叠加）

> **实施期设计修正**：原方案是「停止时排干驱动缓冲」（WASAPI drain + waveIn dwUser 回收）。实施中发现与设备保活（§5）冲突：保活要求暂停时设备**不停流**，驱动缓冲正被活跃写入，主线程抢读存在竞态；且 WASAPI/waveIn 的采集线程在保活期间仍在运行，会自行取走残留包。因此改为更稳妥的等价方案——**把停止瞬间的驱动内残留压缩到可忽略量级**，使 150ms 停止延迟（§3）足以覆盖尾部。

### 4.1 残留分析（修正后）

- 停止延迟期间采集持续进行，真正的尾音全部落进 `g_audioData`。
- 暂停瞬间唯一的损失是「已进驱动缓冲、采集线程还没来得及取」的残留，且这部分落在 150ms 尾部余量内，不是语音主体：
  - **WASAPI**：事件驱动采集线程按包取数（典型周期 ~10ms），残留 ≤ 1–2 个包周期。
  - **waveIn**：残留 ≤ 当前正在填充的缓冲。原缓冲为 100ms×4，最坏残留 ~100ms，会吃掉大半余量，必须压缩。

### 4.2 waveIn 缓冲缩小（engine.cpp，已实施）

`g_waveBuffers` 从 **100ms × 4** 改为 **20ms × 8**（`format.nAvgBytesPerSec / 50`，`g_waveHeaders[4]`→`[8]`）：

- 残留从最坏 ~100ms 降到 ≤20ms，与 WASAPI 同量级。
- 队列总深度 160ms（原 400ms），对 50 次/秒的回调节奏仍充裕。
- 回调频率升到 50 次/秒，CPU 开销可忽略。

### 4.3 暂停时的原子交接（防脏数据，已实施）

保活暂停（`StopAudioCapture`）与采集回调的交接必须无竞态，否则暂停瞬间在途的 PCM 会落进已清空的 `g_audioData`，污染下一段录音：

- `StopAudioCapture` 在 `g_audioLock` 内置 `g_captureSuppressed = true` 并原子地取出/清空 `g_audioData`。
- 两条采集回调（`WaveInProc`、WASAPI `CaptureThread`）在锁外快速检查抑制标志，并在 `g_audioLock` 内追加前**复查**——通过外层检查后被暂停截断的包不会进入已清空的缓冲。

---

## 5. 设备保活（keep-alive 2.5s，新增）

### 5.1 动机

方案 A 使短按切大小写也会开/关一次麦克风；连续两次录音之间也要重新 Init/Start（50–100ms，回潮头部延迟）。保活把「关设备」延迟 2.5s：期间任何新采集直接复用已打开的设备，双向受益——短按不反复开关，连续录音第二次按下零设备延迟。

### 5.2 设计

- 新增常量：`kMicKeepAliveMs = 2500`、`kMicKeepAliveTimer = 6`。
- 新增状态 `g_captureSuppressed`：采集管道仍在运行，但 PCM 被丢弃（不追加 `g_audioData`、不入队流式会话、不更新音频电平）。
- `StopAudioCapture()`（改）：在 `g_audioLock` 内置 `g_captureSuppressed = true` 并原子取出/清空 `g_audioData` 返回 → **设备保持打开** → `SetTimer(kMicKeepAliveTimer, 2500)`。不再 bump `g_audioCaptureGeneration`（设备未关，失败消息依然有效）。
- `CloseAudioCapture()`（新）：原 `StopAudioCapture` 的完整拆除逻辑（WASAPI Stop/Release 或 waveInStop/Reset/Close）+ bump `g_audioCaptureGeneration`。调用方：保活定时器、应用退出、采集失败路径、设置变更（音频后端/设备切换）。
- `StartAudioCapture()`（改）：若设备仍打开（保活中）→ `KillTimer(kMicKeepAliveTimer)` → 清 `g_captureSuppressed` → 清 `g_audioData` → **直接返回 true**，跳过 Init/Start 与 generation bump（设备会话延续）。
- 回调侧：
  - `WaveInProc`：`g_captureSuppressed` 时跳过追加/电平/诊断，但仍 re-queue 缓冲（re-queue 条件 `g_captureActive` 在保活期保持 true，管道保持热）。
  - WASAPI `CaptureThread`：同一标志，跳过追加，继续取包。
- 保活期收到运行时采集失败（如拔麦克风）：`HandleAudioCaptureFailure` 加分支——`!g_recording && 保活中` → 静默 `CloseAudioCapture()` + 日志，不弹 HUD、不走失败诊断流程。

### 5.3 与短按丢弃的关系

`DiscardPendingCapture()` 同样走 `StopAudioCapture()`（保活版）→ 短按后设备也保活 2.5s，连续切大小写不再反复开关麦克风。

### 5.4 边界情况

1. **保活期开始新采集**（任意热键）：复用设备，`g_sessionStartTick` 重置，头部延迟归零。
2. **保活期设备被拔**：静默关闭（见 §5.2），下次按键走正常打开流程。
3. **保活期应用退出 / 设置重载**：退出路径与 `kReloadMessage` 改调 `CloseAudioCapture()` 并 KillTimer。
4. **采集失败路径**：`FinishAudioCaptureFailure` 等失败收尾必须调 `CloseAudioCapture()` 而非保活版——设备可能处于异常状态，不适合保留。
5. **隐私**：松手后麦克风指示灯多亮 ≤2.5s，用户已知悉接受。

---

## 6. 实施顺序与验证

### 6.1 实施顺序

1. **Step 1（方案 A）**：状态机拆分 + 两个新命令 + HUD 时机后移。独立可测。
2. **Step 2（方案 1）**：150ms 停止延迟定时器。独立可测。
3. **Step 3（方案 2）**：WASAPI 排干 + waveIn 排干（dwUser 防重）。
4. **Step 4（保活）**：StopAudioCapture 拆分 + 抑制标志 + 恢复路径 + 失败分支。

### 6.2 验证清单

- [ ] 短按 CapsLock（<300ms）：大小写正常切换、无 HUD、无 ASR 请求、无错误日志
- [ ] 长按 CapsLock：HUD 约 300ms 出现；诊断录音对比说话起点，头部完整
- [ ] 边按边说「短句快收」：尾部完整，识别结果不丢末字
- [ ] 按住说话 → 松手 → 100ms 内再按住：录音不中断，停止定时器被取消
- [ ] 快按快放（总时长 <400ms）：不再频繁触发 "Too short"
- [ ] WASAPI 后端：诊断音频尾部完整（残留 ≤ 1–2 包周期）
- [ ] waveIn 后端：诊断音频尾部完整（20ms 缓冲生效，残留 ≤20ms）
- [ ] 保活：停止后 2.5s 内再次按下，日志出现 `event=capture_keepalive_resume`；2.5s 后设备关闭
- [ ] 保活：连续短按切大小写 5 次，设备只开/关一次
- [ ] 保活期拔麦克风：日志出现 `event=capture_idle_failed`，静默关闭，下次按键正常重开
- [ ] 采集失败注入（拔麦克风）分别发生在 pending / recording / 停止延迟 / 保活四个阶段，HUD 与状态恢复正确
- [ ] 五个后端（local / volcengine / doubao_ime / qwen / qwen_free）各跑一遍长短按冒烟

---

## 7. 风险汇总

| 风险 | 等级 | 缓解 |
|---|---|---|
| 短按也会瞬时打开麦克风（方案 A 固有） | 低 | 保活（§5）使连续短按只开关一次；指示灯短暂亮起可接受 |
| 状态机拆分引入竞态（pending 与 recording 转换） | 中 | 全部状态转换收在主线程消息循环内；采集回调只读状态快照 |
| 保活暂停与回调的交接竞态（脏 PCM 进入下一段录音） | 中 | §4.3：抑制标志在 `g_audioLock` 内翻转，回调锁内复查 |
| 保活期设备异常被保留 | 低 | 失败路径一律 CloseAudioCapture，不走保活 |
| 识别结果出现时间后移 ~150ms | 低 | 用户已接受；常量后续可调 |
| 松手后麦克风指示灯多亮 ≤2.5s | 低 | 用户已知悉接受 |

---

## 8. 实施记录（2026-09-10，已完成，编译通过）

### 8.1 相对原方案的偏差

1. **§4 排干方案整体替换**：WASAPI drain / waveIn dwUser 回收均未实施，改为「waveIn 缓冲 100ms×4 → 20ms×8 + 暂停时锁内原子交接」。原因见 §4 引言：排干与保活语义冲突且有竞态。
2. `StopAudioCapture()` 语义从「拆除设备」变为「暂停并保活」；拆除逻辑移入新的 `CloseAudioCapture()`。

### 8.2 改动文件清单

- `src/app/globals.h`：`kHotkeyCaptureBegin` / `kHotkeyCaptureDiscard` 命令；`kRecordingStopDelayTimer` / `kMicKeepAliveTimer`；`kRecordingStopDelayMs = 150` / `kMicKeepAliveMs = 2500`；`g_waveHeaders[4]`→`[8]`；`extern g_captureSuppressed`。
- `src/app/main.cpp`：`g_captureSuppressed` 定义；`g_capturePendingOnly` / `g_stopDelayRestoreCapsLock` 静态状态；`BeginCaptureOnly()` / `DiscardPendingCapture()` 新增；`StartRecordingSession()` 拆分（pending 复用 + 杀停止延迟定时器）；`HandleAudioCaptureFailure()` 加 pending/保活静默关闭分支、失败即 `CloseAudioCapture()`；`StopRecordingSession()` 失败分支补 `CloseAudioCapture()`；`kHotkeyRecordingMessage` 两个新命令分支 + 停止命令改 150ms 定时器；`WM_TIMER` 加停止延迟/保活两个分支；`kReloadMessage` 空闲时 `CloseAudioCapture()`；`WM_DESTROY` 改 `CloseAudioCapture()`。
- `src/ui/hotkey.cpp`：`StartCapsLockHotkeyPress()` 按下即 post `kHotkeyCaptureBegin`；短按分支 post `kHotkeyCaptureDiscard` + `SendCapsLockTap()`；`PostHotkeyRecordingCommand` 定义前移。
- `src/audio/engine.h/.cpp`：`StartAudioCapture()` 保活恢复路径（`event=capture_keepalive_resume`）；`StopAudioCapture()` 暂停版；`CloseAudioCapture()` 新增；`WaveInProc` 抑制检查（锁外快查 + 锁内复查）；waveIn 缓冲 20ms×8。
- `src/audio/wasapi_capture.cpp`：`CaptureThread` 抑制早退（GetBuffer 后直接 Release+drain）+ 锁内复查。

### 8.3 编译验证

- CMake preset `x64-release`（Ninja + MSVC 14.44）增量构建：22/22 目标编译链接通过，无错误。
- 注：本机 PowerShell 沙箱无法拉起 cmake/cl 子进程，构建需在 Git Bash 手工注入 vcvars 环境（INCLUDE/LIB/PATH）后调 VS 自带 cmake；或直接用项目 `build.bat`。

### 8.4 自审与二轮修复（2026-09-10）

实施后对全量 diff 做了一轮审查，结论与修复：

**已排除的疑点（确认非 bug）**：

1. pending 阶段 PCM 会不会灌进上一段录音的收尾会话？——不会。五个流式会话（volcengine / qwen / qwen_audio / doubao_ime / qwen_free）的 `EnqueuePcmChunk` 在 `StopInput` 后一律拒绝入队（`streaming_`/`stopped_` 守卫），pending PCM 只留在 `g_audioData`，确认时经 `ActivateStreamingSession` 回放进新会话。
2. 设置里改音频设备后保活复用旧设备？——设置保存会 post `kReloadMessage`（settings.cpp:2062），该消息在空闲时 `CloseAudioCapture()`，下一段录音按新配置打开。
3. 重按时清掉 `g_stopDelayRestoreCapsLock` 不恢复大小写？——安全。`RestoreCapsLockState` 是幂等检查（`IsCapsLockOn() != g_capsLockWasOn` 才补 tap），长按期间按键被钩子吞掉，状态从未漂移。

**修复的问题**：

1. **P1：local+VAD 后端头部 ≤300ms 不进 VAD**（main.cpp）。确认建会话时 VAD 才 Reset/启用，回调只在 `g_streamingVadReady` 后喂 VAD，pending 段会被 VAD 当静音裁掉。修复：确认时在 `g_audioLock → g_asrEngine` 嵌套锁内完成「VAD Reset + 回放 pending PCM 喂 VAD + 置 ready」，与回调的锁序一致、无逆序嵌套，时序无缝。
2. **P2：pending 采集可能成孤儿**（hotkey.cpp）。打开设置窗口会 `UninstallKeyboardHook` → `ResetCapsLockHotkeyState` 杀掉 300ms 定时器，若发生在 pending 阶段，采集将无人裁决、PCM 无限增长。修复：`ResetCapsLockHotkeyState` 发现 `g_capsLockHotkeyPending` 时 post `kHotkeyCaptureDiscard`，由主线程丢弃 pending 采集。

二轮修复后重新编译通过（3/3 增量）。

### 8.5 三轮修复（外部评审意见核实后，2026-09-10）

外部评审（GPT）提出 2 缺陷 + 2 边界风险，逐条对照源码核实后**四条全部属实**，已全部修复：

1. **CapsLock 快速重按无法续录（P1，main.cpp）**：150ms 停止延迟必然先于 300ms 长按确认触发，CapsLock 的「松→快速重按」总会断成两段并丢失间隙 PCM（普通热键无此问题）。修复：`BeginCaptureOnly` 在 `g_recording` 且停止延迟挂起时扣留停止（`g_stopDelayHeldForRepress`）；确认长按经 `StartRecordingSession` 正常续录；判定为短按时由 `DiscardPendingCapture` 重新武装停止定时器。
2. **保活恢复可能丢开头包（轻微，engine.cpp）**：resume 原来先 `exchange(false)` 再在锁内清 `g_audioData`，两动作之间的回调块会被误清（≤1 块，~10–20ms）。修复：在同一 `g_audioLock` 临界区内「清缓冲 → 解除抑制」，回调锁内复查保证两操作不可分割。
3. **VAD 加载阻塞音频回调（P2，main.cpp，系二轮修复引入）**：`EnsureVadForConfig` 可能加载模型（数百 ms），原先放在 `g_audioLock` 内会堵死采集回调。修复：模型加载/Reset 移出音频锁，仅在「喂 pending 前缀 + 置 ready」时持锁（毫秒级）。
4. **活动录音时保存设置会保留旧设备（P2 边界，main.cpp）**：录音中 `kReloadMessage` 跳过关设备是对的，但停止后旧设备会经保活续命 2.5s 被复用。修复：新增 `g_captureConfigStale`，忙时置位，`StopRecordingSession` / `DiscardPendingCapture` 停止时改走 `CloseAudioCapture`。
5. **CapsLock 续录后短按会撤销大小写切换（main.cpp）**：扣留第一次长按的停止定时器时同步清除 `g_stopDelayRestoreCapsLock`；第二次按键若最终是短按，重新武装的停止只结束录音，不再撤销短按应有的 CapsLock 切换。
6. **保活设备故障与恢复竞态（engine.cpp）**：恢复前若已有 capture failure，先完整关闭故障设备再走全新打开；正常恢复不再清除异步故障状态，且 teardown 全程保持 PCM 抑制。

对评审「非 bug」裁定的意见：三条裁定均认同（其中「设置修改后不复用旧设备」确如评审所说仅空闲路径成立，即上述第 4 条）。

三轮修复后重新编译通过（3/3 增量）。

复核补丁后补齐第 5、6 项，并通过项目唯一入口重新验证：`build.bat` 实际重编译/链接 3/3 成功，`build.bat --test` 全部离线回归测试通过。真机音频与按键时序仍按 §6.2 验证。
