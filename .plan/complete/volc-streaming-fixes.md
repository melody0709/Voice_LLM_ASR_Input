# 火山引擎流式 ASR 修复方案

> 状态：待审查
> 目标：消除 data race (UB)、修初始音频丢失、修脏音频残留、整理冗余代码

---

## P0: 必须修

### 1. `g_volcStreaming` → `std::atomic<bool>`

**原因**：`bool g_volcStreaming` 被 UI 线程写、audio 回调线程和 volc 线程读，无同步 → C++ 标准下的 data race / UB。编译器可能 hoist 读取导致回调节点看不到更新。

**涉及文件**：

| 文件 | 位置 | 操作 |
|------|------|------|
| `src/globals.h:283` | `extern bool g_volcStreaming;` | 改为 `extern std::atomic<bool> g_volcStreaming;` |
| `src/main.cpp:80` | `bool g_volcStreaming = false;` | 改为 `std::atomic<bool> g_volcStreaming{false};` |
| `src/main.cpp:331` | `g_volcStreaming = true;` | 改为 `g_volcStreaming.store(true);` |
| `src/main.cpp:391` | `bool streaming = g_volcStreaming;` | 改为 `bool streaming = g_volcStreaming.load();` |
| `src/main.cpp:468` | `if (g_config.asrBackend == L"volcengine" && g_volcStreaming)` | 改为 `... && g_volcStreaming.load()` |
| `src/main.cpp:469` | `g_volcStreaming = false;` | 改为 `g_volcStreaming.store(false);` |
| `src/main.cpp:660` | `g_volcStreaming = false;` | 改为 `g_volcStreaming.store(false);` |
| `src/engine.cpp:458` | `if (g_volcStreaming && g_volcSession.connected)` | 改为 `if (g_volcStreaming.load() && g_volcSession.connected)` |
| `src/wasapi_capture.cpp:241` | `if (g_volcStreaming && g_volcSession.connected)` | 改为 `if (g_volcStreaming.load() && g_volcSession.connected)` |

**注意**：`g_volcSession.connected` 已经是 `std::atomic<bool>`（`volcengine_asr.h:123`），不动。

---

### 2. 修复初始音频丢失

**原因**：waveIn/WASAPI 回调在 `g_volcSession.connected` 为 true 之前，只推 `g_audioData` 不推 `g_volcPendingAudio`。OpenSession 耗时 100~500ms 期间的音频丢失（volc 线程 consume 不到）。

对 `bigmodel_nostream` 模式，用户若立刻开口将丢开头几个字。

**修复方案**：去掉回调里的 `&& g_volcSession.connected` 判断，只保留 `g_volcStreaming` 判断。volc 线程本就要等到 connected=true 才进 while 循环发送，不会提前发。

**涉及文件**：

| 文件 | 变更前 | 变更后 |
|------|--------|--------|
| `src/engine.cpp:458` | `if (g_volcStreaming.load() && g_volcSession.connected)` | `if (g_volcStreaming.load())` |
| `src/wasapi_capture.cpp:241` | `if (g_volcStreaming.load() && g_volcSession.connected)` | `if (g_volcStreaming.load())` |

**为什么安全**：
- `SendAudio()` 内部有 `if (!sess.hWebSocket || !sess.connected) return L"";` 守卫 (`volcengine_asr.h:646`)
- volc 线程的 while 循环在 `g_volcSession.connected = true` 之后才进入 (`main.cpp:348 → 382`)
- 所以即使回调推了 pending，volc 线程也不会在 connected 之前发送

**为什么比"connected 后补发"好**：
- 无重复发送风险（connected=true 后、flush 前的回调已在 pending 推了一份，flush 又推一份）
- 代码改动更小
- 逻辑更直观：streaming==true 就收集音频

---

### 3. 修复 `g_volcPendingAudio` 残留脏音频

**原因**：如果 OpenSession 失败（volc 线程直接 return），`g_volcPendingAudio` 里的音频无人消费。下一次 StartRecordingSession join 掉旧线程但不清空 pending，新线程会把这些脏音频发给服务器。

**⚠️ 注意与 P0-2 的配合**：
- 不能在 volc 线程**开头**清空 → 会和 P0-2 冲突（删掉 OpenSession 期间积攒的音频）
- 不能在 volc 线程**末尾**清空 → T3（线程清空）到 T5（`g_volcStreaming = false`）之间回调还在推，残留窗口关不掉
- 不能在 StopRecordingSession **正常路径** join 后清空 → 阻塞 UI
- 不能在 StopRecordingSession **正常路径**不 join 的清空 → 可能与仍在跑的 volc 线程 race，wipe 未消费的有效音频
- 修正案 StopRecordingSession StopAudioCapture 后清空 → 正常路径有 race 风险（虽概率低）

**最终方案**：在 **StartRecordingSession** 中、`g_volcStreaming = true` **之前**清空。此时 streaming=false，无回调推数据，无线程竞争，对所有路径有效。

**涉及文件**：`src/main.cpp:291` (`StartRecordingSession`)

```cpp
// 在 volcengine 分支开头插入清空（在 streaming=true 之前）
if (g_config.asrBackend == L"volcengine") {
    // 清空上一次 session 的残留音频（正常/失败/too short 三路径全覆盖）
    EnterCriticalSection(&g_volcAudioCs);
    g_volcPendingAudio.clear();
    LeaveCriticalSection(&g_volcAudioCs);

    // ... 原有代码：组装 vcfg, g_volcStreaming = true, 线程启动 ...
```

**不修改的路径**：
- volc 线程 lambda 末尾：不清空（由下次 StartRecordingSession 统一清洗）
- StopRecordingSession 正常路径：恢复原样（不 join，不清空）
- StopRecordingSession "Too short" 路径：只 join + 删 CloseSession，不额外清空

**验证各路径**：
| 场景 | 残留来源 | 何时清除 |
|------|---------|---------|
| 正常录音结束 | 无残留（线程已消费完） | 下次 StartRecordingSession |
| OpenSession 失败 | T3（线程 return）到 T5（streaming=false）的回调数据 | 下次 StartRecordingSession |
| Too short | 基本无残留（join 确认线程已清） | 下次 StartRecordingSession |
| 连续正常录音 | 无残留 | 每次 StartRecordingSession 清空（几乎 no-op） |

---

## P1: 建议修

### 4. 去掉 "Too short" 路径冗余的 CloseSession

**涉及文件**：`src/main.cpp` StopRecordingSession "Too short" 分支

```cpp
// 删除此行（线程内部已调过 CloseSession）
volc_asr::CloseSession(g_volcSession);
```

**说明**：`g_volcThread.join()` 后线程已执行完 `CloseSession`，第二次调立即 `if (!sess.hWebSocket) return L"";` 返回空串。完全无害但冗余。StopRecordingSession 代码中直接删除此句即可。

---

### 5. "Too short" 时避免 volc 线程 Post 无效结果

**原因**：StopRecordingSession "too short" 分支 `join()` volc 线程，线程内部 CloseSession 后 `PostMessage(kAsrResultMessage, ...)`。消息进队列，在主线程 DispatchMessage 时处理——此时 HUD 已显示 "Too short"（1.2s 定时），随后被 ASR 结果覆写（200ms 定时），造成短暂闪烁。

**涉及文件**：`src/main.cpp:345-346` 及 volc 线程尾部

**修复方案**：volc 线程发现 streaming 已变为 false 且从未发送过有效音频时，跳过 Post 结果。判断方式——OpenSession 成功但 pending 始终为空（因为 connected 前就按了松键）。

另一种更简单的方案：OpenSession 失败时（`return` 前），不 Post 消息（当前已经没 Post——只在 return 前做了 ShowHud）。问题只出在 OpenSession 成功但数据量极少的情况。

**建议**：暂作为 P2 观察，用户实际体验中极少触发（<400ms 超短按键），不修也无妨。如果修，在 volc 线程发现 finalText 为空且 lastPartial 为空时跳过 Post 即可。

---

## 审查检查清单

```
[ ] globals.h:283 g_volcStreaming 声明改 std::atomic<bool>
[ ] main.cpp:80 g_volcStreaming 定义改 std::atomic<bool>{false}
[ ] main.cpp:331/391/468/469/660 g_volcStreaming 读写加 store/load
[ ] engine.cpp:458 去掉 && g_volcSession.connected
[ ] wasapi_capture.cpp:241 去掉 && g_volcSession.connected
[ ] main.cpp StartRecordingSession: streaming=true 之前 clear pending
[ ] main.cpp StopRecordingSession Too short: 删除冗余 CloseSession
[ ] 编译通过
[ ] 实际测试：快速短按热键 → 不再丢失开头音频
[ ] 实际测试：OpenSession 失败后再次录音 → 无脏音频混入
[ ] 实际测试：正常松键 → UI 不卡顿，结果正常显示
```

---

## 不作修改

以下为 Review 中审查通过、不需修改的点：

- `g_volcSession.connected` 已正确使用 `std::atomic<bool>`（`volcengine_asr.h:123`）
- 音频双缓冲（`g_audioData` + `g_volcPendingAudio`）架构正确
- 临界区使用一致（`g_volcAudioCs` 保护 pending，`g_audioLock` 保护 audioData）
- `SendAudio(chunk, false)` + `SendAudio(empty, true)` last 帧协议正确
- `CloseSession` drain 最终结果逻辑正确
- `SendAudio` 内部 `!sess.connected` 守卫正确（防止 volc 线程提前发送）
- WASAPI 重采样 `m_resamplePhase -= written / m_resampleRatio` 已按 AGENTS.md 踩坑规则正确实现
- WaveInProc 回调重入安全性由 waveIn 框架保证（每个 WAVEHDR 独立）
