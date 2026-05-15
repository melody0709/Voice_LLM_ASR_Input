# 火山引擎 ASR 卡死修复方案

## 问题描述

使用火山引擎 ASR 时，每天约卡住 10 次。现象：说完话后一直停在"识别中"，等十几秒无反应，最终只能结束进程。

## 根因分析

### 核心原因：`WinHttpWebSocketReceive` 超时不可靠

代码中所有 WebSocket 接收操作都依赖 `WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT` 来超时退出，但这个超时在以下网络异常条件下**可能不生效**，导致调用无限阻塞：

1. **网络静默断连** — WiFi 切换、网络闪断、VPN 断开等场景下，TCP 连接被中间设备（NAT、防火墙、代理）静默丢弃，没有发送 TCP RST/FIN 包。客户端不知道连接已断，`Receive` 永远等下去。**这是最可能的日常卡死原因。**
2. **代理服务器保活** — 企业代理维持 TCP 连接存活，但后端 WebSocket 已断开。
3. **SSL 重协商卡住** — TLS 层面的重协商不受 WebSocket 超时控制。

微软官方确认：`WinHttpSetTimeouts` 只控制特定阶段（DNS/连接/发送/接收），不覆盖整个 HTTPS 生命周期。

### 连锁效应

一旦 `WinHttpWebSocketReceive` 卡住 → `g_volcThread` 阻塞 → `WM_DESTROY` 中 `g_volcThread.join()` 也无限等待 → 用户无法正常关闭程序 → 只能杀进程。

### 5 个具体卡住路径

| # | 卡住位置 | 代码行 | 触发条件 | 现有超时 |
|---|---------|--------|---------|---------|
| 1 | `ReceiveResult` 主循环 | volcengine_asr.h:251 | bigmodel 模式发音频后收 partial | 150ms |
| 2 | `ReceiveResult` 结束帧 | volcengine_asr.h:694 | isLast=true 后等最终结果 | 4000ms |
| 3 | `ReceiveResult` nostream drain | volcengine_asr.h:680 | bigmodel_nostream 循环等结果 | 1500ms × N |
| 4 | `ReceiveResult` CloseSession | volcengine_asr.h:713 | 关闭时收集残留结果 | 500ms × 6 |
| 5 | `WebSocketCloseGracefully` | volcengine_asr.h:378 | 等待 close frame | 3000ms |

### 次要问题：Keep-Alive 僵尸连接

`EnsureConnection` 复用 `hSession`/`hConnect`，如果底层 TCP 已半死，下次 `OpenSession` 时 WebSocket 升级可能失败或卡住。但这个问题有 `WinHttpSetTimeouts` 的 10 秒超时保护，不是日常卡死的主因。

---

## 修复方案

### P0-1：全局会话超时 + 强制中断机制

**核心思路**：给整个识别会话设一个硬性超时上限。超时后，从另一个线程调用 `WinHttpCloseHandle(hWebSocket)` 强制中断阻塞的 `ReceiveResult`。

微软官方确认这是线程安全的：
> "Calling `WinHttpCloseHandle` from another thread while the worker thread is in a synchronous `WinHttpWebSocketReceive` is safe. The API will return `ERROR_WINHTTP_OPERATION_CANCELLED`."

**修改文件**：`volcengine_asr.h`, `main.cpp`

**具体改动**：

1. `VolcSession` 增加 `forceAbort` 原子标志：
```cpp
struct VolcSession {
    // ... 现有字段 ...
    std::atomic<bool> forceAbort{false};
};
```

2. `ReceiveResult` 增加 `VolcSession*` 参数，在关键位置检查 `forceAbort`：
```cpp
inline VolcResult ReceiveResult(HINTERNET hWebSocket, DWORD timeoutMs,
                                 VolcSession* sess = nullptr) {
    if (sess && sess->forceAbort.load()) return result;
    // ... 现有 WinHttpWebSocketReceive 调用 ...
    // 如果返回 ERROR_WINHTTP_OPERATION_CANCELLED，直接返回空结果
}
```

3. `SendAudio` 和 `CloseSession` 中所有 `ReceiveResult` 调用传入 `&sess`。

4. `main.cpp` 的 volc 线程中增加 watchdog 逻辑：
```cpp
// 在发送结束帧后启动 watchdog
ULONGLONG sessionDeadline = GetTickCount64() + 15000; // 15 秒硬上限

// 在每个可能卡住的 ReceiveResult 调用前检查
if (GetTickCount64() > sessionDeadline || g_volcSession.forceAbort.load()) {
    g_volcSession.forceAbort = true;
    break;
}
```

5. `StopRecordingSession` 中增加超时强制中断：
```cpp
// 设置一个 15 秒定时器，如果 volc 线程还没结束就强制关闭
// 在 MainWndProc 的 WM_TIMER 或单独线程中处理
```

### P0-2：WM_DESTROY 安全退出

**问题**：`g_volcThread.join()` 可能无限等待。

**修改文件**：`main.cpp`

**具体改动**：

```cpp
case WM_DESTROY:
    g_volcStreaming.store(false);
    g_volcSession.forceAbort = true;
    // 先尝试强制关闭 WebSocket，让阻塞的 ReceiveResult 返回
    if (g_volcSession.hWebSocket) {
        WinHttpCloseHandle(g_volcSession.hWebSocket);
        g_volcSession.hWebSocket = nullptr;
    }
    // 给线程 3 秒时间自行退出
    if (g_volcThread.joinable()) {
        // 方案 A：用 WaitForSingleObject 限时等待
        // 方案 B：直接 detach（简单但有风险）
        // 推荐方案 A
    }
    // ... 其余清理 ...
```

### P1-1：bigmodel_nostream drain 循环加退出条件

**问题**：[volcengine_asr.h:679-688] 的 `while(true)` 只有 10 秒总超时，但 `ReceiveResult` 的 1500ms 超时可能不生效。

**修改文件**：`volcengine_asr.h`

**具体改动**：

```cpp
if (isLast && nostreamMode) {
    ULONGLONG tDrain0 = GetTickCount64();
    while (true) {
        if (!sess.connected || sess.forceAbort.load()) break;  // 新增
        VolcResult vr = ReceiveResult(sess.hWebSocket, 1500, &sess);
        // ...
    }
}
```

### P1-2：async 模式 drainThread 安全退出

**问题**：[main.cpp:367-383] 的 `drainThread` 可能卡在 `DrainReceiveBuffer`。

**修改文件**：`main.cpp`

**具体改动**：

```cpp
drainThread = std::thread([&]() {
    while (!asyncDrainDone && g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()) {
        std::wstring partial = volc_asr::DrainReceiveBuffer(g_volcSession.hWebSocket);
        // ...
    }
    // 收尾 drain 也加 forceAbort 检查
    while (g_volcSession.hWebSocket && !g_volcSession.forceAbort.load()) {
        // ...
    }
});
```

### P2-1：Keep-Alive 连接过期检查

**问题**：复用的 `hSession`/`hConnect` 可能已半死。

**修改文件**：`volcengine_asr.h`

**具体改动**：

```cpp
struct VolcSession {
    // ... 现有字段 ...
    ULONGLONG lastUsedTick = 0;  // 上次使用时间
};

inline bool EnsureConnection(VolcSession& sess) {
    if (sess.hSession && sess.hConnect) {
        // 超过 30 秒未使用，丢弃旧连接
        if (GetTickCount64() - sess.lastUsedTick > 30000) {
            VolcDebugLog("EnsureConnection: connection expired, rebuilding");
            WinHttpCloseHandle(sess.hConnect);
            sess.hConnect = nullptr;
            // hSession 保留，只重建 hConnect
        } else {
            return true;
        }
    }
    // ... 创建新连接 ...
}

// CloseSession 中记录时间
sess.lastUsedTick = GetTickCount64();
```

---

## 实施步骤

1. **第一步**：`VolcSession` 增加 `forceAbort` 字段
2. **第二步**：`ReceiveResult` 增加 `VolcSession*` 参数，检查 `forceAbort` 和 `ERROR_WINHTTP_OPERATION_CANCELLED`
3. **第三步**：`SendAudio` 和 `CloseSession` 传入 `&sess`
4. **第四步**：volc 线程主循环加全局超时检查
5. **第五步**：`WM_DESTROY` 改为强制关闭 + 限时等待
6. **第六步**：nostream drain 和 async drainThread 加 `forceAbort` 检查
7. **第七步**：Keep-Alive 过期检查
8. **验证**：编译通过，功能测试

## 验证方法

1. 编译通过：`.\build.bat`
2. 正常使用测试：多次录音识别不卡
3. 模拟网络断连：录音中关闭 WiFi/拔网线，确认 15 秒内自动超时退出
4. 模拟长时间录音：超过 15 秒的录音，确认不会误触发全局超时
5. 关闭程序测试：识别中直接关闭程序，确认不卡死
6. 查看日志：`%TEMP%\volc_asr_debug.log` 确认超时和强制中断的日志输出
