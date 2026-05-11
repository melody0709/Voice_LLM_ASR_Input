# 火山引擎流式 ASR 第二波修复方案

> 状态：待审查
> 来源：第 5 轮 Review 发现的 6 个额外问题
> 前置：volc-streaming-fixes.md（P0-1/2/3 + P1-4 已实施）

---

## P0：必须修

### 1. "Too short" 路径 join() 卡死 UI

**原因**：用户快速按松键（<400ms）时，volc 线程可能还在 `OpenSession()` 中。OpenSession 有 2 次重试，WinHTTP 超时可达每次 10~35 秒。`join()` 阻塞 UI 线程同样时长。

**涉及文件**：`src/main.cpp:474`

```cpp
// 变更前
if (pcm.size() < 8000) {
    ShowHud(L"Too short");
    SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
    if (g_volcThread.joinable()) g_volcThread.join();  // ← 删除此行
    return;
}

// 变更后
if (pcm.size() < 8000) {
    ShowHud(L"Too short");
    SetTimer(g_hudWindow, kHudHideTimer, 1200, nullptr);
    return;
}
```

**理由**：
- 正常路径不 join，Too short 也没有理由 join
- 线程看到 `g_volcStreaming = false` 后会自行退出循环，CloseSession，exit
- 下次 `StartRecordingSession` 开头的 `join()`（line 334）会收尾
- 副作用：线程退出时 Post 的 ASR 结果可能短暂覆盖 "Too short" HUD——这是已知 P2 问题（volc-streaming-fixes.md P1-5），本次不修

**注意**：volc 线程在第 334 行已有 join 防护（`if (g_volcThread.joinable()) g_volcThread.join();`），StartRecordingSession 创建新线程前会收尾旧线程。如果用户连续快速按键，join 仍会阻塞——但这只在旧线程仍未退出且新录音已开始时才触发，概率极低。

---

## P1：建议修

### 2. bigmodel_async 模式 WebSocket 双线程接收竞争

**原因**：async 模式有 drain 线程持续 `DrainReceiveBuffer(hWebSocket)` = `ReceiveResult(1)`。同时 `SendAudio(empty, true, asyncMode=true)` 内部也调用 `ReceiveResult(4000)`。同一个 `hWebSocket` 被两个线程同时 `WinHttpWebSocketReceive`——

**涉及文件**：`src/main.cpp:413-425`

```cpp
// 变更前（简化）
if (!chunk.empty()) {
    SendAudio(chunk, false, asyncMode, nostreamMode);  // async 非 last: 不 receive ✅
}
SendAudio(empty, true, asyncMode, nostreamMode);      // async 且 last: 会 receive ❌
if (asyncMode) {
    asyncDrainDone = true;
    if (drainThread.joinable()) drainThread.join();
    // ...
}

// 变更后
if (asyncMode) {
    asyncDrainDone.store(true);
    if (drainThread.joinable()) drainThread.join();   // 先等 drain 线程退出
}
if (!chunk.empty()) {
    volc_asr::SendAudio(g_volcSession, chunk, false, asyncMode, nostreamMode);
}
std::wstring lastResult = volc_asr::SendAudio(g_volcSession, empty, true, asyncMode, nostreamMode);
if (!lastResult.empty()) lastPartial = lastResult;
// 注意：asyncPartial 不再使用——drain 已停，SendAudio(last) 拿到的才是最终结果
```

**涉及变更点**：
1. `main.cpp:417-423`：将 `asyncDrainDone = true; drainThread.join()` 移到 `SendAudio(last)` **之前**
2. 删除原来 drain 退出后 `asyncPartial` 覆盖 `lastPartial` 的逻辑——drain 已停，`asyncPartial` 是过时数据，`SendAudio(last)` 拿到的才是最终结果

---

### 3. SendAudio 失败静默丢音频

**原因**：`SendAudio` 返回空串时无法区分"无 partial 文本"和"WebSocket 断开"。当前循环一直发送直到用户松键，所有音频进黑洞。

**涉及文件**：

**(a)** `src/volcengine_asr.h`：`SendAudio` 增加错误返回机制

```cpp
// SendAudio 签名不变，但返回空串 + hWebSocket 变 null 时表示连接已死
// 当前 SendAudio 在 WinHttpWebSocketSend 返回错误时仅 log 并 return L""
// 修正：发送失败时关闭 WebSocket，使后续调用短路
```

**(b)** `src/main.cpp`：volc 线程循环中检测连接丢失

```cpp
// 在 while 循环的 SendAudio 之后增加检测
if (hasData && chunk.size() >= kChunkBytes) {
    std::wstring partial = volc_asr::SendAudio(g_volcSession, chunk, false, asyncMode, nostreamMode);
    chunk.clear();
    if (!g_volcSession.hWebSocket) break;  // ← 新增：WebSocket 已死，跳出循环
    if (!asyncMode && !partial.empty() && partial != lastPartial) { ... }
}
```

**注意**：`SendAudio` 内部 `if (!sess.hWebSocket || !sess.connected) return L"";` 已经处理了主动 close 后的情况。需要补充的是**发送过程中网络断开**的场景——此时 `WinHttpWebSocketSend` 返回错误，但 `sess.hWebSocket` 仍非 null。

**方案**：在 `SendAudio` 的失败分支中关闭 WebSocket：

```cpp
// volcengine_asr.h:661-663
if (err != ERROR_SUCCESS) {
    VolcDebugLog("SendAudio FAILED: err=%u", err);
    WebSocketCloseGracefully(sess.hWebSocket);
    sess.hWebSocket = nullptr;
    sess.connected = false;
    return L"";
}
```

这样发送失败后 `hWebSocket` 变 null，调用方（volc 线程和 drain 线程）下次进入 `SendAudio`/`DrainReceiveBuffer` 时会直接短路返回。

---

## P2：建议修

### 4. WM_DESTROY 停止顺序与 StopRecordingSession 一致

**涉及文件**：`src/main.cpp:660-662`

```cpp
// 变更前
    g_captureActive = false;
    StopAudioCapture();
    g_volcStreaming.store(false);

// 变更后（与 StopRecordingSession 顺序一致）
    g_volcStreaming.store(false);
    g_captureActive = false;
    StopAudioCapture();
```

**理由**：StopRecordingSession 先设 `streaming=false`（让回调停止推 pending），再停录音。WM_DESTROY 之前顺序相反。实际影响极小（关窗口时不 care 结果），但统一顺序减少维护者困惑。

**注意**：`g_captureActive = false` 在 `StopAudioCapture()` 内部已经会设（`wasapi_capture.cpp:536`、`engine.cpp:536`），此处保留不影响功能。

---

### 5. Keep-alive 连接服务端超时重试

**现象**：`g_volcKeepAlive` 始终为 true，`hSession`/`hConnect` 复用跨录音。如果服务端空闲超时关闭底层连接，下一次 `OpenSession` 第一次尝试会失败，触发 retry 逻辑恢复连接。多一次 ~5-10s 的失败延迟。

**建议**：不做代码修改，记录观察即可。如需优化：
- 在 `EnsureConnection` 中增加连接健康检查（ping）
- 或在长时间无录音后主动 `ClosePersistentConnection` + 重建
- 目前的重试机制已兜底，用户体验可控

---

### 6. `g_cloudApiMs` 计时偏差

**现象**：`g_cloudApiMs = (GetTickCount64() - tTotal0) - g_recordingMs`。`tTotal0` 是 volc 线程启动时间，`g_recordingMs` 是从 `g_sessionStartTick` 到松键的时间。两者起点不同——`tTotal0` 晚于 `g_sessionStartTick`（差线程创建开销 ~1ms）。不影响功能，仅 debug 输出有毫秒级偏差。

**建议**：不做修改。记录观察即可。

---

## 审查检查清单

```
[ ] P0-1: main.cpp Too short 路径删除 join()
[ ] P1-2: async 模式 drainThread.join() 移到 SendAudio(last) 之前，删 asyncPartial 覆盖逻辑
[ ] P1-3: volcengine_asr.h SendAudio 失败时 WebSocketCloseGracefully + 置 null
[ ] P1-3: main.cpp 循环内 SendAudio 后检测 !hWebSocket 并 break
[ ] P2-4: main.cpp WM_DESTROY 交换 streaming=false 与 StopAudioCapture 顺序
[ ] 编译通过
[ ] 实际测试：快速按松键 → UI 不卡死
[ ] 实际测试：async 模式 → 无崩溃
[ ] 实际测试：录音中拔网线 → 合理报错不丢剩余音频
```
