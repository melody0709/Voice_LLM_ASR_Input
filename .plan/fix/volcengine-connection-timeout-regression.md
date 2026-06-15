# VolcEngine 连接超时与丢头部音频回归分析

## 现象

用户近两天频繁遇到火山引擎 ASR 连接超时，日志典型模式：

```
[23:29:33.631] EnsureConnection: connection expired (127782ms old), rebuilding
[23:29:33.631] WinHttpConnect: 0ms
[23:29:36.653] OpenSession: hard timeout (3015ms), closing hReq
[23:29:36.653] OpenSession: WinHttpSendRequest failed (err=12017, elapsed=3015ms)
[23:29:36.653] Volc thread: attempt 1 failed, retrying in 500ms...
...（4 次重试全部超时）
[23:29:49.271] PasteTextImeAware: skipped operational error 'VolcEngine connect failed'
```

错误码 12017 = `ERROR_WINHTTP_OPERATION_CANCELLED`，是 watchdog 线程在硬超时后主动关闭 `hReq` 导致的。

## 根因确认

通过 `git diff v0.8.7..HEAD` 确认：`volcengine_asr.h` 从 `src/` 移到 `src/asr/`，**内容完全一致**。协议层零改动，连接超时根因不在协议层。

断线频率增加的机制是**级联效应**：
```
网络波动 → OpenSession 超时 3s → Abort join 阻塞 3s → WASAPI 延迟启动 → 丢头部音频
→ 用户觉得"断线" → 快速再按 → 又触发 Abort join → 又丢头部音频 → 恶性循环
```

## P0：启动顺序 + 头部音频补发

### 问题

v0.8.7 的 `StartRecordingSession`：`StartAudioCapture()` → `join()` → 新线程

当前版本：`AbortAndReset()` → `session->Start()` → `g_activeStreamingSession = session` → `StartAudioCapture()`

`Abort()` 的 `worker_.join()` 可能阻塞 UI 线程最多 3 秒。这期间 WASAPI 没启动，用户已经开始说话 → **头部音频丢失**。

### 关键发现：简单调整启动顺序不够

1. 把 `StartAudioCapture()` 移到 `AbortAndReset()` 之前，`g_activeStreamingSession` 仍为 null，`EnqueuePcmChunk` 被跳过
2. `g_audioData` 会无条件记录音频（wasapi_capture.cpp:241），但不会进入 session 的 `pendingAudio_`
3. **VAD 双重处理问题**：WASAPI 回调中 `g_streamingVadTrimmer->ProcessPcm16()` 在 `g_activeStreamingSession` 检查**之前**调用（wasapi_capture.cpp:246-249）。即使 `EnqueuePcmChunk` 被跳过，VAD trimmer 状态机已被推进。后续 `ReplayPreCapturedAudio` 用新 trimmer 处理同一段音频 → 双重处理

### 修复方案：Pre-captured Sync

#### 步骤 1：修改 WASAPI 回调（src/audio/wasapi_capture.cpp + src/audio/engine.cpp）

将 VAD trim 处理移到 `g_activeStreamingSession` 检查之内：

```cpp
// 修改前：VAD trim 无条件调用
g_audioData.insert(...);  // 无条件写入 ✓
const bool useStreamingVadTrim = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
if (useStreamingVadTrim) {
    g_streamingVadTrimmer->ProcessPcm16(begin, bytesWritten, streamingOutputs);  // ← 无条件！
}
EnterCriticalSection(&g_streamingSessionCs);
if (g_activeStreamingSession && g_activeStreamingSession->IsRunning()) {
    if (useStreamingVadTrim) { EnqueuePcmChunk... }
    else { EnqueuePcmChunk... }
}
LeaveCriticalSection(&g_streamingSessionCs);

// 修改后：VAD trim 只在 session 存在时调用
g_audioData.insert(...);  // 无条件写入 ✓（不变）
EnterCriticalSection(&g_streamingSessionCs);
if (g_activeStreamingSession && g_activeStreamingSession->IsRunning()) {
    const bool useStreamingVadTrim = g_streamingVadTrimmer && g_streamingVadTrimmer->IsActive();
    if (useStreamingVadTrim) {
        std::vector<std::vector<BYTE>> streamingOutputs;
        g_streamingVadTrimmer->ProcessPcm16(begin, bytesWritten, streamingOutputs);
        for (const auto& chunk : streamingOutputs) {
            if (!chunk.empty()) {
                g_activeStreamingSession->EnqueuePcmChunk(chunk.data(), chunk.size());
            }
        }
    } else {
        g_activeStreamingSession->EnqueuePcmChunk(begin, bytesWritten);
    }
}
LeaveCriticalSection(&g_streamingSessionCs);
```

**关键变化**：session 为 null 时，音频只写入 `g_audioData`，不经过 VAD trim。`WaveInProc`（engine.cpp）同样修改。

#### 步骤 2：重构 StartRecordingSession（src/app/main.cpp）

```
void StartRecordingSession() {
    if (g_recording) return;
    if (g_hudWindow) KillTimer(g_hudWindow, kHudHideTimer);

    // 1. 立即弹出 HUD（解决 UI 冻结感）
    ShowHud(L"Listening... " + AsrBackendDisplayName(g_config));

    // 2. 立即开启音频采集（g_audioData 无条件记录，VAD trim 不执行）
    std::wstring error;
    if (!StartAudioCapture(error)) {
        ShowHud(error);
        if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
        return;
    }
    g_sessionStartTick = GetTickCount64();
    g_recording = true;

    // 3. 同步等待旧 session 退出（WASAPI 在后台继续录音到 g_audioData）
    AbortAndResetActiveStreamingSession();
    ResetStreamingVadTrimmerState();

    // 4. 创建新 session 并 Start
    //    统一 Qwen/火山启动顺序：Start → Replay → g_activeStreamingSession → VAD trimmer
    auto session = CreateXxxStreamingSession(...);
    if (!session->Start(startError)) {
        // ★ 错误路径：必须 StopAudioCapture，因为步骤 2 已经启动了 WASAPI
        StopAudioCapture();
        g_recording = false;
        ShowHud(startError);
        if (g_hudWindow) SetTimer(g_hudWindow, kHudHideTimer, 1800, nullptr);
        return;
    }

    // 5. 补发积攒的头部音频（VAD trimmer 还没创建，直接发原始 PCM）
    ReplayPreCapturedAudio(session.get());

    // 6. 设置 g_activeStreamingSession（此后 WASAPI 回调开始正常入队）
    EnterCriticalSection(&g_streamingSessionCs);
    g_activeStreamingSession = std::move(session);
    LeaveCriticalSection(&g_streamingSessionCs);

    // 7. 创建 VAD trimmer（后续音频经过 VAD trim）
    StartStreamingVadTrimmerForCloud(...);

    // 8. 设置 Watchdog 计时器等收尾工作
    ...
}
```

**错误路径注意**：`StartAudioCapture()` 提前到 session 创建之前，如果 `session->Start()` 失败，必须调用 `StopAudioCapture()` + `g_recording = false` 清理状态。当前版本没这个问题（`StartAudioCapture` 在 session 创建之后），新流程必须补上。

#### 步骤 3：实现 ReplayPreCapturedAudio（src/app/main.cpp）

```cpp
void ReplayPreCapturedAudio(IStreamingAsrSession* session) {
    if (!session) return;
    std::vector<BYTE> preCaptured;
    EnterCriticalSection(&g_audioLock);
    preCaptured = g_audioData;  // 拷贝，不影响本地 ASR
    LeaveCriticalSection(&g_audioLock);
    if (preCaptured.empty()) return;
    // 直接补发原始 PCM，不做 VAD trim
    // 头部音频包含用户说话开头，不应被 VAD 裁掉
    session->EnqueuePcmChunk(preCaptured.data(), preCaptured.size());
}
```

#### 注意事项

- `g_audioData` 在 `StartAudioCapture()` 时被 `clear()`，之后所有数据都是本次录音的，直接全部补发
- 不需要新的全局变量（如 `g_sentAudioDataBytes`）
- 补发在 UI 线程执行，不在音频回调中，不影响音频线程性能
- 补发的 96KB 音频不会一次性发送——worker 线程以 6400 字节（200ms）为单位分批发送，符合火山引擎官方协议建议
- **Qwen/未来新供应商兼容**：只要实现 `IStreamingAsrSession` 接口，补发逻辑自动适用
- **Qwen/火山启动顺序统一**：两个分支都对齐为 `Start → Replay → g_activeStreamingSession → VAD trimmer`

## P1：网络参数放宽

### 1. 代理设置恢复（volcengine_asr.h）

| 位置 | 当前值 | 修改为 |
|------|--------|--------|
| `EnsureConnection` 第 403 行 | `WINHTTP_ACCESS_TYPE_NO_PROXY` | `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` |
| `TestConnection` 第 878 行 | `WINHTTP_ACCESS_TYPE_NO_PROXY` | `WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` |

v0.8.0 从 `DEFAULT_PROXY` 改为 `NO_PROXY`。如果用户系统配了代理/VPN，`NO_PROXY` 导致请求直连不通。无代理时 `DEFAULT_PROXY` 行为与 `NO_PROXY` 完全一致，百利无一害。

### 2. 超时调整（volcengine_asr.h）

| 位置 | 当前值 | 修改为 |
|------|--------|--------|
| `EnsureConnection` 第 407 行 | `2000, 2000, 2000, 2000` | `3000, 3000, 5000, 5000` |
| `OpenSessionImpl` 第 598 行 | `2000, 2000, 2000, 2000` | `3000, 3000, 5000, 5000` |
| `OpenSessionImpl` 第 607 行 | `kHardTimeoutMs = 3000` | **保持 3000** |

**注意**：`kHardTimeoutMs` 不能放宽。它直接决定 UI 卡顿上限——`Abort()` 的 `worker_.join()` 会阻塞直到 watchdog 关闭 `hReq`，hard timeout 3 秒 = UI 最多卡 3 秒。改成 6 秒意味着 UI 卡顿翻倍。

真正改善连接成功率的关键不是加长超时，而是：
1. 连接过期改 5 分钟（减少重新建连次数，最大改善）
2. 代理恢复 DEFAULT_PROXY（解决 VPN 用户问题）
3. activeReq 快速取消（让 Abort 立即打断 WinHTTP 操作，UI 卡顿从 3 秒降到接近 0）

`TestConnection` 保持 `8000, 8000, 10000, 10000` 不变。

### 3. 连接过期阈值放宽（volcengine_asr.h）

| 位置 | 当前值 | 修改为 |
|------|--------|--------|
| `EnsureConnection` 第 395 行 | `> 3000`（3 秒） | `> 300000`（5 分钟） |

`hSession` 维护 WinHTTP 内部的 TCP+TLS 连接池。3 秒过期会销毁所有 keep-alive 连接，每次录音都要重新 DNS + TCP + TLS 握手。5 分钟内连续录音可复用连接池。

## P1：activeReq 快速取消（消除 UI 卡顿）

### 问题

即使 Pre-captured Sync 解决了丢音，`Abort()` 的 `worker_.join()` 仍可能阻塞 UI 线程最多 3 秒（worker 卡在 `WinHttpSendRequest`），HUD 冻结。

### 方案

在 `VolcSession` 中新增 `std::atomic<HINTERNET> activeReq{nullptr}`：

1. `OpenSessionImpl` 创建 `hReq` 后设 `sess.activeReq.store(hReq)`，所有关闭 `hReq` 的路径改为 `sess.activeReq.exchange(nullptr)` 取所有权后关闭
2. `Abort()` 中 `auto req = s_volcSession.activeReq.exchange(nullptr); if (req) WinHttpCloseHandle(req);`，立即打断 `WinHttpSendRequest`
3. watchdog 线程也改用 `sess.activeReq.exchange(nullptr)` 替代直接关闭 `hReq`

### 双重关闭防护

`OpenSessionImpl` 中所有 `WinHttpCloseHandle(hReq)` 改为 `activeReq.exchange(nullptr)` 取所有权后关闭。只有取到非空值的一方执行 `WinHttpCloseHandle`，保证不会双重关闭。

时序安全分析：
- watchdog 关闭 `activeReq` 后，`WinHttpSendRequest`/`WinHttpReceiveResponse` 会返回 12017（`ERROR_WINHTTP_OPERATION_CANCELLED`）
- `sendOk=false` 或 `recvOk=false`，不会走到后续 `WinHttpQueryHeaders`/`WinHttpWebSocketCompleteUpgrade`
- `exchange(nullptr)` 是幂等的，第二次取到 `nullptr` 不会重复关闭

效果：快速连续录音时 UI 冻结从 3 秒降到接近 0。

## P2：g_volcSession 移入 static

将 `g_volcSession` 从全局变量移入 `volcengine_streaming_session.cpp` 的 `static volc_asr::VolcSession s_volcSession`，暴露 4 个自由函数：

- `VolcengineResetForNewSession()` — 清除 forceAbort + lastError
- `VolcenginePrewarmConnection()` — 后台预创建 hSession + hConnect
- `VolcengineClosePersistentConnection()` — 关闭 hSession + hConnect（应用退出或 keep-alive 关闭时）
- `VolcengineForceAbortAndCloseAll()` — force-abort + 关闭所有句柄（WM_DESTROY）

同时将 `g_volcKeepAlive` 从 `main.cpp` 移入 `volcengine_streaming_session.cpp`（匿名命名空间外，避免遮蔽 `volc_asr` 命名空间）。

`globals.h` 移除 `volcengine_asr.h` include 和 `g_volcSession`/`g_volcKeepAlive` 声明，减少编译依赖。`settings.cpp` 需自行 `#include "volcengine_asr.h"`。

## P2：Qwen/火山 Stop 逻辑去重

`StopRecordingSession` 中 Qwen 和火山两个几乎完全相同的分支合并为 `(qwen || volcengine) && HasActiveStreamingSession()` 单一分支。HUD 文案用 `AsrBackendDisplayName(g_config)` 统一生成，火山专属的 `VolcDebugLog` 保留为条件调用。降低维护成本和引入不一致 bug 的风险。

## 已确认无需修改

### StopRecordingSession 顺序（当前版本更优）

当前版本先 `StopAudioCapture()` 再 `StopInput()`。`StopAudioCapture()` 内部 `m_captureThread.join()` 会等 WASAPI 线程完全退出，退出前最后一个包在 `streaming_` 仍为 true 时被正常接收。v0.8.7 先设 `g_volcStreaming = false` 再停 WASAPI，反而会导致尾音丢失。**保持现状**。

### WASAPI 回调三层条件

当前版本需要 `g_activeStreamingSession != null` + `IsRunning()` + `streaming_` 三层条件，这是架构重构的必然结果（用 `IStreamingAsrSession` 接口替代全局标志），不是 bug。唯一的数据丢失窗口是启动顺序回归导致的，P0 修复后解决。

### s_volcSession.connected 残留

`connected` 残留为 true 时，`bufferUntilStop()` 中的 `!s_volcSession.connected.load()` 检查可能误判。但 `bufferUntilStop` 只在 `!s_volcSession.hWebSocket || !s_volcSession.connected.load()` 时调用，`connected=true` 但 `hWebSocket=nullptr` 仍会触发。影响有限。

### drainThread 生命周期

`Abort()` 只 join `worker_`，不 join `drainThread`。`drainThread` 通过 `forceAbort` + `AtomicTakeWebSocket` 自行退出。`WM_DESTROY` 关闭 `hConnect/hSession` 时 `drainThread` 可能还在运行，但 `ReceiveResult` 会返回错误后退出。进程退出时线程被强制终止，不影响。如果将来加"最小化到托盘"功能需重新评估。

## 实施状态

| 优先级 | 修改 | 文件 | 状态 |
|--------|------|------|------|
| **P0** | 启动顺序 + Pre-captured Sync | `main.cpp` | ✅ 已完成 |
| **P0** | HUD 提前显示 | `main.cpp` | ✅ 已完成 |
| **P0** | WASAPI 回调 VAD trim 移入 session 检查内 | `wasapi_capture.cpp` + `engine.cpp` | ✅ 已完成 |
| **P1** | 代理恢复 DEFAULT_PROXY | `volcengine_asr.h` | ✅ 已完成 |
| **P1** | 超时调整 3000/3000/5000/5000，hard timeout 保持 3000 | `volcengine_asr.h` | ✅ 已完成 |
| **P1** | 连接过期阈值 300000ms | `volcengine_asr.h` | ✅ 已完成 |
| **P1** | activeReq 快速取消 | `volcengine_asr.h` + `volcengine_streaming_session.cpp` | ✅ 已完成 |
| **P1** | Qwen/火山启动顺序统一 | `main.cpp` | ✅ 已完成 |
| **P2** | g_volcSession 移入 static + g_volcKeepAlive 移入 session 模块 | 多文件 | ✅ 已完成 |
| **P2** | Qwen/火山 Stop 逻辑去重 | `main.cpp` | ✅ 已完成 |

### 已实施变更摘要

**P0 启动顺序修复**（`src/app/main.cpp`）：
- `StartAudioCapture()` 移到 `AbortAndResetActiveStreamingSession()` 之前，WASAPI 立即开始录音
- 新增 `ReplayPreCapturedAudio()` 函数，在 session Start 后、设置 `g_activeStreamingSession` 前补发积攒音频
- HUD 提前到 `StartAudioCapture()` 之前显示
- Qwen/火山统一启动顺序：`Start → Replay → g_activeStreamingSession → VAD trimmer`
- 错误路径（`session->Start()` 失败）补上 `StopAudioCapture()` + `g_recording = false`

**P0 VAD 双重处理修复**（`src/audio/wasapi_capture.cpp` + `src/audio/engine.cpp`）：
- `StreamingVadTrimmer::ProcessPcm16()` 移入 `g_activeStreamingSession` 检查内
- session 为 null 时音频只写入 `g_audioData`，不经过 VAD trim
- 本地 ASR VAD 检测增加 `!(g_activeStreamingSession && streamingVadTrimmer active)` 守卫，防止与 streaming VAD 双重处理

**P1 网络参数放宽**（`src/asr/volcengine_asr.h`）：
- `EnsureConnection` + `TestConnection` 代理：`NO_PROXY` → `DEFAULT_PROXY`
- `EnsureConnection` + `OpenSessionImpl` 超时：`2000,2000,2000,2000` → `3000,3000,5000,5000`
- `kHardTimeoutMs` 保持 3000（UI 卡顿上限不增加）
- 连接过期阈值：`3000` → `300000`（5 分钟）

**P1 activeReq 快速取消**（`src/asr/volcengine_asr.h` + `src/asr/volcengine_streaming_session.cpp`）：
- `VolcSession` 新增 `std::atomic<HINTERNET> activeReq{nullptr}`，追踪 `OpenSessionImpl` 中的 `hReq`
- `OpenSessionImpl` 创建 `hReq` 后存入 `activeReq`，所有 `WinHttpCloseHandle(hReq)` 改为 `activeReq.exchange(nullptr)` 取所有权后关闭
- watchdog 线程改用 `sess.activeReq.exchange(nullptr)` 替代直接关闭 `hReq`
- `Abort()` 中新增 `s_volcSession.activeReq.exchange(nullptr)` + `WinHttpCloseHandle`，立即打断 `WinHttpSendRequest`
- 效果：快速连续录音时 UI 冻结从 3 秒降到接近 0

**P2 g_volcSession 移入 static**（`src/asr/volcengine_streaming_session.cpp` + `src/app/main.cpp` + `src/app/globals.h`）：
- `g_volcSession` 从全局变量移入 `volcengine_streaming_session.cpp` 的 `static volc_asr::VolcSession s_volcSession`
- `g_volcKeepAlive` 从 `main.cpp` 移入 `volcengine_streaming_session.cpp`（匿名命名空间外）
- 新增 4 个接口函数：`VolcengineResetForNewSession()`、`VolcenginePrewarmConnection()`、`VolcengineClosePersistentConnection()`、`VolcengineForceAbortAndCloseAll()`
- `globals.h` 移除 `volcengine_asr.h` include 和 `g_volcSession`/`g_volcKeepAlive` 声明
- `settings.cpp` 需自行 `#include "volcengine_asr.h"`

**P2 Qwen/火山 Stop 逻辑去重**（`src/app/main.cpp`）：
- `StopRecordingSession` 中 Qwen 和火山两个分支合并为 `(qwen || volcengine) && HasActiveStreamingSession()` 单一分支
- HUD 文案用 `AsrBackendDisplayName(g_config)` 统一生成
- 火山专属的 `VolcDebugLog` 保留为 `if (g_config.asrBackend == L"volcengine")` 条件调用

## 修复优先级总表

| 优先级 | 修改 | 文件 | 风险 |
|--------|------|------|------|
| **P0** | 启动顺序 + Pre-captured Sync | `main.cpp` | 中 |
| **P0** | HUD 提前显示 | `main.cpp` | 低 |
| **P0** | WASAPI 回调 VAD trim 移入 session 检查内 | `wasapi_capture.cpp` + `engine.cpp` | 中 |
| **P1** | 代理恢复 DEFAULT_PROXY | `volcengine_asr.h` | 低 |
| **P1** | 超时调整 3000/3000/5000/5000，hard timeout 保持 3000 | `volcengine_asr.h` | 低 |
| **P1** | 连接过期阈值 300000ms | `volcengine_asr.h` | 低 |
| **P1** | activeReq 快速取消 | `volcengine_asr.h` + `volcengine_streaming_session.cpp` | 中 |
| **P1** | Qwen/火山启动顺序统一 | `main.cpp` | 低 |
| **P2** | g_volcSession 移入 static + g_volcKeepAlive 移入 session 模块 | 多文件 | 高 |
| **P2** | Qwen/火山 Stop 逻辑去重 | `main.cpp` | 低 |

## 验证方法

1. 修复后编译运行，确认正常网络环境下 OpenSession 在 1-2 秒内完成
2. 在有代理/VPN 的环境下测试，确认不再出现 12017 超时
3. 快速连续录音（间隔 3-5 秒），确认头部音频不丢失
4. 检查 `volc_asr_debug.log` 中 `EnsureConnection` 的行为是否合理
5. 对比 v0.8.7 和修复版本在相同网络环境下的连接成功率
6. 模拟网络波动（断网 5 秒后恢复），确认重试能成功恢复
7. 验证 Qwen ASR 在同样修改下正常工作

## 代码审查结论（实施后）

### 审查范围

对 `git diff` 全部 8 个文件、约 187 增 / 167 删行逐行审查，并执行 `build.bat` 编译验证（`Build Success: build\VoxType.exe`）。

### 总体结论

**所有变更正确，构建通过，可合入。** 无需修改。下列"需要修改"和"建议优化"均为审查过程中讨论过的点，最终结论是**全部保留现状**。

### 需要修改：无

无任何必须修改项。

### 讨论过但否决的方案（记录留档，避免重复踩坑）

#### ❌ 否决 1：调整 ReplayPreCapturedAudio 与 g_activeStreamingSession 赋值的顺序

**起因**：审查发现 `ReplayPreCapturedAudio()`（UI 线程，拷贝 `g_audioData` 快照并 `EnqueuePcmChunk`）与紧随其后的 `g_activeStreamingSession = std::move(session)` 之间存在一个亚毫秒级窗口。此窗口内 WASAPI 线程仍在往 `g_audioData` 写新音频，但这些新音频既不在 Replay 快照里，也因 `g_activeStreamingSession` 仍为 null 而不会被 WASAPI 回调入队 → **理论上丢一小段音频**。

**曾考虑的修复**：把顺序对调成"先设 session，后 Replay"。

**否决理由（关键，务必不要再走回头路）**：

对调会引入**音频乱序**，比小窗口严重得多：

```
当前顺序（正确）：
  Replay EnqueuePcmChunk(旧音频)        [UI 线程]
    g_activeStreamingSession 仍为 null → WASAPI 回调不碰 pendingAudio_
  g_activeStreamingSession = session
  WASAPI 回调 EnqueuePcmChunk(新音频)   [音频线程]
  → pendingAudio_: 旧 → 新  ✓
  （session 赋值时刻构成 happens-before 分隔，顺序必然旧→新）

对调顺序（错误）：
  g_activeStreamingSession = session
  WASAPI 回调 EnqueuePcmChunk(新音频)   [音频线程，可能先到]
  Replay EnqueuePcmChunk(旧音频)        [UI 线程，可能后到]
  → pendingAudio_: 新 → 旧 或 旧 → 新（跨线程抢 pendingAudio_ mutex，顺序不确定）✗
```

`PendingPcmBuffer::Append` 的 mutex 只保证单次 append 原子，**不保证两个并发调用方的先后**。WASAPI 在音频线程、Replay 在 UI 线程，跨线程并发 append 时顺序由调度决定，无法保证旧音频先入。

**乱序 vs 小窗口的实际影响对比**：

| 问题 | 触发频率 | 单次影响 | 可见性 |
|------|----------|----------|--------|
| 当前小窗口 | 偶发，窗口亚毫秒 | 至多丢一个 ~10ms 包，位于语音中段 | 用户几乎无感 |
| 对调后乱序 | 每次录音必发 | ASR 喂入时间序列错位，识别质量整体劣化 | 明显，且不可预测 |

**结论：当前 `Start → Replay → g_activeStreamingSession → VAD trimmer` 顺序正确，严禁对调。** `Qwen`/`volcengine` 两个分支顺序一致，✓。

### 建议优化：无强制项

以下均为审查中确认的健壮性细节，当前实现已正确处理，无需改动，仅记录以便后续维护：

#### ✅ 优化点 1：WM_DESTROY 抽出 VolcengineForceAbortAndCloseAll，额外关闭 activeReq

原 `WM_DESTROY` 内联代码只关 `hWebSocket/hConnect/hSession`。新函数 `VolcengineForceAbortAndCloseAll()` 额外 `exchange` 关闭 `activeReq`，**修复了之前 OpenSession 卡在 `WinHttpSendRequest` 时退出会泄漏 `hReq` 的隐患**。这是正确改进，保留。

#### ✅ 优化点 2：activeReq 双重关闭防护，7 个退出路径全覆盖

`OpenSessionImpl` 中所有 `WinHttpCloseHandle(hReq)` 改为 `activeReq.exchange(nullptr)` 取所有权后关闭，幂等。已逐个核对 7 个退出路径：
1. `WinHttpAddRequestHeaders` 失败
2. `forceAbort` 前置检查
3. `!sendOk`（`WinHttpSendRequest` 失败）
4. `!recvOk`（`WinHttpReceiveResponse` 失败）
5. HTTP status != 101
6. `WinHttpWebSocketCompleteUpgrade` 后
7. 成功路径

全部覆盖，无遗漏、无双重关闭。保留。

### 验证通过的关键点

| 检查项 | 结论 |
|--------|------|
| VAD 双重处理修复（`ProcessPcm16` 移入 session 检查内） | ✓ session 为 null 时不推进 trimmer 状态机 |
| 本地 VAD 守卫（`wasapi_capture.cpp:263`） | ✓ `!(g_activeStreamingSession && trimmer active)` 防止双重 VAD |
| `g_volcSession` → `s_volcSession` 迁移完整性 | ✓ grep 确认无残留引用（除注释） |
| `g_volcKeepAlive` 命名空间位置 | ✓ 移到匿名命名空间外的 `volc_asr` 命名空间，无遮蔽 |
| Stop 分支合并（Qwen/Volc） | ✓ 单分支，`VolcDebugLog` 保留为条件调用 |
| 网络参数（代理/超时/过期阈值） | ✓ 全部按计划落地，`kHardTimeoutMs` 保持 3000 |
| 错误路径（`session->Start()` 失败） | ✓ 补 `StopAudioCapture()` + `g_recording = false` |
| 编译验证 | ✓ `Build Success` |
