# Fix: VolcEngine nostream 卡死 + 结果丢失

## 症状

### Bug 1: CloseSession 卡死 8.6s (第一版)

```log
17:31:44.092 === CloseSession ===
17:31:52.703 Watchdog: volc thread still running after 18s, force aborting
```

`CloseSession` 中 `ReceiveResult(500ms)` 的 WinHTTP 超时不生效，实际阻塞远超 500ms。6 次重试 × ~1.4s + 关闭握手 3s ≈ 8.6s。

### Bug 2: "VolcEngine timeout" 无结果返回 (第二版修复后)

```log
18:04:27.364 SendAudio: isLast sent
18:04:27.438 === CloseSession === ... finalText=''
... finalText "VolcEngine timeout"
```

nostream 模式返回空结果。drainThread 没等到服务器就提前被杀了。

### Bug 3: 长语音 (>15s) 只返回前 15s 结果 (第三版修复后)

nostream 模式 15 秒后有 partial 显示，但最终结果只包含这 15 秒，后面的文字丢失。

### Bug 4: WinHttpCloseHandle 死锁 (第四版修复后)

```log
19:04:09.874 Volc thread: wait done, asyncPartial NOT empty, connected=1
... 之后无日志，卡死
```

主线程在 `WinHttpCloseHandle(hWebSocket)` 时，drainThread 正在同一个句柄上
`WinHttpWebSocketReceive` 中阻塞。WinHTTP 不是线程安全的——对同一句柄并发调用
`WinHttpCloseHandle` 和 `WinHttpWebSocketReceive` 导致内部死锁。

### Bug 5: 短音频服务器关闭连接后白等 5 秒 (第四版修复后)

```log
19:04:14.954 ReceiveResult: 0 bytes (close frame)
19:04:19.810 Volc thread: wait done, asyncPartial empty (5078ms)
```

服务器发了 close frame 关闭连接，但主线程 wait 循环只检查 `asyncPartial.empty()`
和 `g_volcSession.connected`，没检查 drainThread 是否完成。服务器关闭后
drainThread 的 `WinHttpWebSocketReceive` 可能还在阻塞（WinHTTP 超时不可靠），
导致 `asyncPartial` 始终为空，主线程白等 5 秒超时。

## 根因

### 架构问题

VoxType 用 `SendAudio` 发音频、drainThread 收结果，共用同一个 `hWebSocket` 句柄。
收发的同步靠原子标志轮询，没有 AriaType 的 `split()` 双工分离。

### 核心时序 Bug

```
主线程:                           drainThread:
sendChunk(isLast) → 跳过接收        DrainReceiveBuffer(1ms)
                                  → 等服务器响应 (100-500ms)
                                  → 1ms 超时太快，空
asyncDrainDone = true
forceAbort = true                 收到 asyncDrainDone
close ws handle                   进入 final drain
join drainThread                  但 forceAbort 已真 → 跳过!
→ asyncPartial 为空                → done
→ "VolcEngine timeout"
```

对于长语音 (>15s)，`asyncPartial` 已有 15s 的结果：
```
主线程: if (asyncPartial.empty() && ...) → false! 跳过等!
forceAbort = true → 杀死 drainThread
asyncPartial = "前15s" ← 丢失后10s!
```

对于 WinHTTP 死锁：
```
主线程:                           drainThread:
WinHttpCloseHandle(hWebSocket)     WinHttpWebSocketReceive(hWebSocket)
→ WinHTTP 内部死锁                  → 同一句柄并发访问
```

### 根本原因

1. **drainThread 1ms 超时 vs 服务器处理时间 100-500ms**：来不及
2. **`forceAbort` 抢在 final drain 前杀死**：drainThread 没机会读
3. **等待条件只检查 asyncPartial 是否有值**：有 partial 就不等了，但 partial 不是完整结果
4. **`WinHttpCloseHandle` 在 `join` 之前调用**：和 drainThread 并发访问同一句柄导致死锁
5. **WinHTTP 超时设置不可靠**：1ms/500ms 超时实际可能阻塞数秒

## 修复

### 文件: `src/main.cpp`

#### Change 1: 新增 `drainFinalDone` 标志 (行 451)

```cpp
std::atomic<bool> drainFinalDone{false};
```

在 `asyncDrainDone` 旁新增，表示 drainThread 的 final drain 已完成。

#### Change 2: drainThread final drain 设标志 (行 478)

```cpp
drainFinalDone = true;
VolcDebugLog("drainThread: done");
```

在 final drain 完全退出后才设，保证 `asyncPartial` 已被最终结果覆盖。

#### Change 3: drainThread final drain 用长超时 (行 464-470)

```cpp
// 改前: 1ms 轮询，几乎立即退出，拿不到服务器 100-500ms 后的返回
while (...) { DrainReceiveBuffer(1ms); }

// 改后: 先一次 3000ms 等待服务器响应，再 quick drain 剩余
{
    ReceiveResult(3000ms)  // ← 给服务器足够时间返回结果
}
while (...) { DrainReceiveBuffer(1ms); }
```

#### Change 4: 主线程等待条件从 asyncPartial 改为 drainFinalDone (行 557-572)

```cpp
// 改前: 等 asyncPartial 有值（partial 结果导致提前退出）
while (asyncPartial.empty() && ...)

// 改后: 等 drainThread final drain 完成（保证完整结果）
while (!drainFinalDone && ...)
```

#### Change 5: 修复 WinHttpCloseHandle 死锁 — 先 join 再 close (行 563-570)

```cpp
// 改前: 先 close 句柄再 join（并发访问死锁）
WinHttpCloseHandle(hWebSocket);   // drainThread 可能还在 Receive
drainThread.join();

// 改后: 先 join 等 drainThread 退出，再 close 句柄
drainThread.join();               // 等 drainThread 退出（drainFinalDone 保证已完成）
WinHttpCloseHandle(hWebSocket);   // 安全，无并发访问
```

#### Change 6: no-speech 路径同样先 join 再 close (行 527-530)

no-speech 路径设 `forceAbort=true` 后，drainThread 会在主循环检查到
`forceAbort` 后退出，final drain 也因 `forceAbort` 跳过。所以 join 不会
阻塞太久，且避免了并发访问死锁。

### 不修改的文件

- `src/volcengine_asr.h`：`SendAudio`、`ReceiveResult`、`CloseSession` 不变
- `src/main.cpp` 重连逻辑（行 408-432）：在 drainThread 创建之前，不受影响

## 修复后时序

### 正常短语音

```
主线程:                             drainThread:
sendChunk(isLast) → 跳过接收
                                    主循环退出
                                    进 final drain:
                                      ReceiveResult(3000ms)
                                      → 服务器 100-500ms 返回 ✅
                                      → asyncPartial = "完整文本"
                                     drainFinalDone = true
等 drainFinalDone (最多 5s)
  → 发现 true，退出 wait loop
asyncDrainDone = true
join drainThread (已退出)
close ws handle (安全，无并发)
finalText = asyncPartial ✅
```

### 长语音 (>15s)

```
录音中 asyncPartial = "前15s文字"
sendChunk(isLast) → 跳过接收
                                    主循环退出
                                    进 final drain:
                                      ReceiveResult(3000ms)
                                      → 服务器返回完整结果
                                      → asyncPartial = "完整文本" ✅
                                     drainFinalDone = true
等 drainFinalDone
  → 发现 true，退出 wait loop
join drainThread
close ws handle
finalText = asyncPartial ✅
```

### No speech (VAD)

```
VAD 状态为 0 → 走 no-speech 路径
asyncDrainDone = true, forceAbort = true
join drainThread
  → drainThread: forceAbort → 主循环退出
  → final drain: 检查 forceAbort → 跳过
  → drainFinalDone = true
close ws handle (安全)
return "No speech detected" ✅
```

### 超时 (服务器无响应)

```
asyncDrainDone = true
drainThread final drain: ReceiveResult(3000ms) → 超时空
drainFinalDone = true
主线程: drainFinalDone = true → 退出 wait loop
asyncPartial 为空 → "No speech detected" → "VolcEngine timeout" ✅
```

## Review 确认

| 场景 | 预期 | 结果 |
|------|------|------|
| 短语音 (~3s) | 返回完整文本 | ✅ |
| 长语音 (>15s) | 返回完整文本，不丢失 | ✅ 修复 Bug 3 |
| VAD 无语音 | "No speech detected" | ✅ |
| 服务器超时 | "VolcEngine timeout" | ✅ |
| 重连 | OpenSession 失败 → 重试 3 次 | ✅ 不受影响 |
| 用户取消 | forceAbort → 走 no-speech 路径 | ✅ |
| 并发 write/read | 先 join 再 close | ✅ 修复 Bug 4 |
| 短音频服务器关闭 | drainFinalDone 保证不等 | ✅ 修复 Bug 5 |

## 与 AriaType 对比

| 方面 | VoxType (修复后) | AriaType |
|------|-----------------|----------|
| 等结果方式 | `drainFinalDone` 原子标志 + Sleep 轮询 | `oneshot::channel` 阻塞等待 |
| 收发的线程安全 | 先 join 再 close，避免并发 | `split()` 读写分离 |
| final drain 超时 | `ReceiveResult(3000ms)` | `tokio::time::timeout(10s, rx)` |
| 分包的合理性 | 6400 bytes (200ms) | 1600 samples (100ms) |
| 协议解析 | 手写字节流 | `serde_json` + 多偏移启发式 |
| 关键差异 | 重连 + VAD + 连接池 | 并发模型 + 超时可靠性 |

## 后续建议

1. **考虑用 `CancelIoEx` 替代 `WinHttpCloseHandle` 来中断阻塞的 Receive**：避免 close handle 时的 race
2. **分包改为 100ms**：更细粒度，更接近官方建议
3. **`ReceiveResult` 中检查 `payload_msg.is_last_package` JSON 字段**：AriaType 检查两处 isLast（协议头 + JSON），更可靠
4. **长期考虑用 tokio-tungstenite 重写或加 Rust 层**：解决 WinHTTP 超时不靠谱的根因
