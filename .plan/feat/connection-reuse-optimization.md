# 火山引擎连接复用优化方案

> 目标：减少连续快速录音时的 reconnect，将连接建立延迟从 ~1.5-2s 降到 ~0.3-0.5s

---

## 1. 问题分析

### 1.1 WinHTTP 句柄层级（微软官方文档确认）

```
hSession (WinHTTP session)  — 长生命周期，维护 DNS 缓存和连接池
  └─ hConnect (逻辑句柄)    — 仅存储服务器名+端口，不建立 TCP 连接
      └─ hRequest → hWebSocket — 真正的 TCP+TLS 连接在这里建立
```

**关键发现**（微软文档原文）：

> A call to **WinHttpConnect** does not result in an actual connection to the HTTP server
> until a request is made for a specific resource.

即 `WinHttpConnect` **不建立 TCP 连接**，只是创建一个逻辑句柄。真正的 TCP+TLS 连接在 `WinHttpSendRequest` 时才建立。

### 1.2 当前代码的耗时分析

```
OpenSession() 各步骤的实际耗时：

1. EnsureConnection()
   ├─ WinHttpConnect() — DNS 解析 ~1.0-1.4s（首次）或 ~0ms（DNS 缓存命中）
   └   注：WinHttpConnect 不建立 TCP 连接，只做 DNS 解析

2. WinHttpOpenRequest() — ~0ms（只创建请求句柄）

3. WinHttpSendRequest() + WinHttpReceiveResponse()
   ├─ TCP 三次握手 — ~50-100ms
   ├─ TLS 1.3 握手 — ~200-400ms（1-RTT，如果 TLS Session Resumption）
   ├─ HTTP 101 Switching Protocols — ~50ms
   └   总计 ~300-550ms

4. Init frame send + receive — ~200-400ms

总计：首次 ~1.8-2.3s，DNS 缓存命中后 ~0.5-0.9s
```

### 1.3 hConnect 复用的核心问题

**问题 1：服务器空闲超时短**

火山引擎 ASR 服务器在空闲 ~3s 后关闭 TCP 连接（实测数据，官方文档未明确说明）。
如果 hConnect 复用时 WinHTTP 内部连接池中的 TCP 连接已被服务器关闭，`WinHttpSendRequest` 会超时。

**问题 2：只关 hConnect 不关 hSession 会复用死连接**

WinHTTP 的 TCP 连接池在 hSession 级别管理。只关闭 hConnect 不会清空连接池中的空闲 TCP 连接。
新创建的 hConnect 仍然会复用池中已死的 TCP 连接，导致同样的超时问题。
**必须同时关闭 hSession 才能彻底清空连接池。**

### 1.4 火山引擎官方文档确认

- WebSocket 是**每次录音一个连接**，录音结束发负包后服务端关闭连接
- 鉴权在 WebSocket 握手的 HTTP Header 中完成
- **没有限制 TCP 连接复用**——同一个 hConnect 上可以发起多个 WebSocket 升级请求
- 没有并发连接数限制
- **未明确说明服务端空闲连接超时时间**（实测 ~3s）

---

## 2. 网络异常场景分析（核心）

### 2.1 场景矩阵

| # | 场景 | hConnect 状态 | WinHttpSendRequest 行为 | 处理方式 |
|---|------|--------------|------------------------|---------|
| 1 | 本地网络开关关闭 | 仍然有效（逻辑句柄） | 失败（ERROR_WINHTTP_CANNOT_CONNECT / ERROR_WINHTTP_TIMEOUT） | 关闭 hConnect+hSession → RebuildConnection → 重试 |
| 2 | 网络关闭后恢复 | 仍然有效 | 成功（DNS 缓存可能过期，但 hConnect 仍可用） | ✅ 直接复用 |
| 3 | 网络波动（短暂丢包） | 仍然有效 | 可能失败（TCP 超时） | 关闭 hConnect+hSession → RebuildConnection → 重试 |
| 4 | 路由器静默丢弃 TCP 连接 | 仍然有效 | **关键**：WinHTTP 内部连接池中的空闲 TCP 连接已被路由器丢弃 | 关闭 hConnect+hSession → RebuildConnection → 重试 |
| 5 | DNS 服务器不可达 | 仍然有效 | WinHttpConnect 可能失败（DNS 解析超时） | EnsureConnection 失败 → OpenSession 返回 false |
| 6 | WiFi 切换到有线 | 仍然有效 | 可能失败（源 IP 变化，TCP 连接失效） | 关闭 hConnect+hSession → RebuildConnection → 重试 |
| 7 | VPN 连接/断开 | 仍然有效 | 可能失败（路由表变化） | 关闭 hConnect+hSession → RebuildConnection → 重试 |
| 8 | 服务端关闭空闲 TCP（~3s） | 仍然有效 | WinHTTP 检测到 RST → 自动重建 TCP 连接 | ✅ WinHTTP 内部处理 |
| 9 | 服务端重启 | 仍然有效 | WinHTTP 自动重建 TCP 连接 | ✅ WinHTTP 内部处理 |
| 10 | hConnect 被其他线程关闭 | 无效 | 访问已关闭句柄 → crash | ❌ 已有 data race（watchdog/WM_DESTROY） |

### 2.2 关键场景详解

**场景 4：路由器静默丢弃 TCP 连接（最常见的问题）**

```
时间线：
T0: 录音1结束，hConnect+hSession 保留
T1: 路由器/NAT 设备因为空闲超时丢弃 TCP 连接（通常 5-15 分钟）
    客户端不知道连接已断开（没有收到 RST/FIN）
T2: 用户开始录音2，OpenSession → EnsureConnection → hConnect 存在 → 复用
T3: WinHttpOpenRequest → 成功（只创建请求句柄）
T4: WinHttpSendRequest → 失败！
    - 如果 WinHTTP 内部连接池有该 TCP 连接，发送数据时收到 RST
    - WinHttpSendRequest 返回 FALSE，GetLastError() = ERROR_WINHTTP_CONNECTION_ERROR
T5: OpenSessionImpl 检测到失败 → 关闭 hConnect+hSession → RebuildConnection → 重试
T6: 新的 WinHttpSendRequest → 建立新的 TCP+TLS → 成功
```

**场景 8：服务端关闭空闲 TCP 连接（~3s 超时）**

```
T0: 录音1结束，hConnect+hSession 保留
T1: 服务端因为空闲超时关闭 TCP 连接（发送 FIN/RST）
T2: WinHTTP 收到 FIN/RST，从内部连接池移除该 TCP 连接
T3: 用户开始录音2，OpenSession → EnsureConnection → hConnect 存在 → 复用
T4: WinHttpSendRequest → WinHTTP 发现连接池中没有空闲连接 → 建立新的 TCP+TLS → 成功
```

**这是最理想的场景**——WinHTTP 自动处理了服务端关闭连接的情况，应用层无需干预。
但前提是 WinHTTP 确实收到了 FIN/RST。如果中间有 NAT 设备丢弃了 FIN，
WinHTTP 不知道连接已死，就会走场景 4 的路径。

### 2.3 hConnect 复用是否会导致异常？

**不会**。原因：

1. **hConnect 是逻辑句柄**：它只存储服务器名和端口，不维护 TCP 连接状态
2. **WinHTTP 内部管理连接池**：TCP 连接的创建/复用/关闭由 WinHTTP 内部处理
3. **WinHTTP 会自动检测失效连接**：如果连接池中的 TCP 连接已失效（收到 RST），WinHTTP 会自动移除并重建
4. **WinHttpSendRequest 是幂等的**：在同一个 hConnect 上多次调用是安全的

**唯一的例外**：hConnect 句柄被关闭后再次使用 → crash。但这不是"复用"的问题，而是线程安全问题。

### 2.4 hConnect 复用失败时的完整处理链

```
OpenSession 失败
  ├─ EnsureConnection 失败（DNS 不可达）
  │   └─ OpenSession 返回 false → 外部 reconnect 重试
  │
  ├─ WinHttpOpenRequest 失败（hConnect 无效）
  │   ├─ 关闭 hConnect+hSession（清空连接池）
  │   ├─ 快速失败（<2s）→ RebuildConnection → 重试 OpenSessionImpl（isRetry=true）
  │   └─ 超时失败（≥2s）→ OpenSession 返回 false → 外部 reconnect 重试
  │
  ├─ WinHttpSendRequest 失败（TCP 连接失效 / 网络断开）
  │   ├─ 关闭 hConnect+hSession（清空连接池）
  │   ├─ 快速失败（<2s）→ RebuildConnection → 重试 OpenSessionImpl（isRetry=true）
  │   └─ 超时失败（≥2s）→ OpenSession 返回 false → 外部 reconnect 重试
  │
  ├─ WinHttpReceiveResponse 失败（服务端拒绝 / 超时）
  │   ├─ 关闭 hConnect+hSession（清空连接池）
  │   ├─ 快速失败（<2s）→ RebuildConnection → 重试 OpenSessionImpl（isRetry=true）
  │   └─ 超时失败（≥2s）→ OpenSession 返回 false → 外部 reconnect 重试
  │
  ├─ HTTP 状态码 != 101（服务端不支持 WebSocket）
  │   ├─ 关闭 hConnect+hSession（清空连接池）
  │   ├─ 快速失败（<2s）→ RebuildConnection → 重试 OpenSessionImpl（isRetry=true）
  │   └─ 超时失败（≥2s）→ OpenSession 返回 false → 外部 reconnect 重试
  │
  ├─ CompleteUpgrade 失败
  │   ├─ 关闭 hConnect+hSession（清空连接池）
  │   ├─ 快速失败（<2s）→ RebuildConnection → 重试 OpenSessionImpl（isRetry=true）
  │   └─ 超时失败（≥2s）→ OpenSession 返回 false → 外部 reconnect 重试
  │
  └─ Init frame 错误（鉴权失败 / 参数错误）
      └─ 关闭 hConnect+hSession（清空连接池，不重建——可能是 API Key 问题）
          → OpenSession 返回 false → 外部 reconnect 重试
```

**内部重试 vs 外部重试的关键区分**：

- **快速失败**（耗时 <2s）：网络瞬断、连接池死连接等，内部 RebuildConnection 后大概率成功
- **超时失败**（耗时 ≥2s）：网络完全断开、DNS 不可达等，内部重试浪费时间，直接返回让外部循环处理

**外部 reconnect 重试链**：

```
OpenSession 失败 → RebuildConnection → Sleep(500ms) → 重试
→ 失败 → RebuildConnection → Sleep(1000ms) → 重试
→ 失败 → RebuildConnection → Sleep(2000ms) → 重试
→ 全部失败 → 显示错误信息
```

每次外部重试前都强制 `RebuildConnection`，确保不会复用之前失败的连接。

---

## 3. 线程安全分析

### 3.1 当前 g_volcSession 的访问模式

`VolcSession` 结构体（volcengine_asr.h L117-127）：
```cpp
struct VolcSession {
    HINTERNET hSession = nullptr;     // 无保护
    HINTERNET hConnect = nullptr;     // 无保护
    HINTERNET hWebSocket = nullptr;   // 无保护
    int sequence = 0;                 // 无保护
    std::wstring partialText;         // 无保护
    std::wstring lastError;           // 无保护
    std::atomic<bool> connected{false};
    std::atomic<bool> forceAbort{false};
    ULONGLONG lastUsedTick = 0;       // 无保护
};
```

### 3.2 谁在访问 g_volcSession

| 线程 | 访问的成员 | 频率 |
|------|-----------|------|
| **volc 线程** | hSession, hConnect, hWebSocket, connected, forceAbort, lastUsedTick, sequence | 每次录音 |
| **drainThread** | hWebSocket, connected, forceAbort | 录音中持续 |
| **UI 线程**（watchdog, WM_DESTROY, 录音开始前 join） | hWebSocket, hConnect, hSession, forceAbort | 偶尔 |
| **PrewarmConnection detach 线程** | hSession, hConnect, lastUsedTick | 启动时一次 |

### 3.3 当前为什么没有 data race

**关键保护**：录音开始前 `g_volcThread.join()` 确保 volc 线程已结束。
同一时刻只有一个线程操作 `g_volcSession` 的非 atomic 成员。

**已有的 data race**（方案不引入新问题）：
1. **watchdog** 在 UI 线程中关闭句柄——但只在 volc 线程超时（18s）时触发
2. **WM_DESTROY** 在窗口销毁时强制关闭句柄
3. 两者概率极低，且是已有问题

### 3.4 不需要增加锁

hConnect 的保留/复用只发生在 volc 线程内部。watchdog/WM_DESTROY 路径保持关闭全部句柄的行为不变。

---

## 4. 优化方案（已实施）

### 4.1 核心思路

录音结束后只关闭 hWebSocket，保留 hConnect+hSession。下次录音直接在已有 hConnect 上发起 WinHttpOpenRequest + WinHttpSendRequest。

**关键约束**：服务器空闲 ~3s 后关闭 TCP 连接，因此：
- 间隔 ≤3s：复用 hConnect+hSession，WinHTTP 连接池中 TCP 连接仍有效 → 节省最多
- 间隔 >3s：检测过期，关闭 hConnect+hSession（清空连接池），重建全新连接

### 4.2 实际改动点

#### A1. EnsureConnection — 过期检测 + 同时关闭 hSession

```cpp
inline bool EnsureConnection(VolcSession& sess) {
    if (sess.hSession && sess.hConnect) {
        if (sess.lastUsedTick > 0 && GetTickCount64() - sess.lastUsedTick > 3000) {
            // 过期：同时关闭 hConnect+hSession，清空连接池
            WinHttpCloseHandle(sess.hConnect);
            sess.hConnect = nullptr;
            WinHttpCloseHandle(sess.hSession);
            sess.hSession = nullptr;
        } else {
            // 复用：更新 lastUsedTick
            sess.lastUsedTick = GetTickCount64();
            return true;
        }
    }
    // 重建 hSession + hConnect ...
}
```

**过期阈值 3s 的依据**：
- 实测 ≤2.5s 复用成功，≥3.5s 复用失败（服务器关闭 TCP）
- 火山引擎官方文档未明确说明空闲超时时间
- 3s 是基于实测数据的安全阈值

**为什么必须同时关 hSession**：
WinHTTP 的 TCP 连接池在 hSession 级别管理。只关 hConnect 不会清空池中已死的 TCP 连接，
新 hConnect 仍会复用池中死连接，导致同样的超时问题。

#### A2. CloseSession — 条件保留 hConnect+hSession

```cpp
if (g_volcKeepAlive) {
    VolcDebugLog("CloseSession: keeping hSession+hConnect alive");
    sess.lastUsedTick = GetTickCount64();
} else {
    if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
    if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
}
```

#### A3. main.cpp 中手动关闭句柄的地方改为条件关闭

3 处代码（no-speech 路径、async/nostream 结束路径）改为：
- `g_volcKeepAlive=true` 时保留 hConnect+hSession，只关闭 hWebSocket
- `g_volcKeepAlive=false` 时关闭全部

watchdog 和 WM_DESTROY 路径保持不变——异常路径关闭全部句柄。

#### A4. OpenSessionImpl — 6 个失败点统一关闭 hConnect+hSession

所有失败点（WinHttpOpenRequest / WinHttpAddRequestHeaders / WinHttpSendRequest / WinHttpReceiveResponse / 状态码 != 101 / CompleteUpgrade）统一：

```cpp
WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess))
    return OpenSessionImpl(sess, cfg, true);
return false;
```

init frame 错误路径：同样关闭 hConnect+hSession，但不重建（可能是 API Key 问题）。

**内部重试时间判断**（`GetTickCount64() - t0 < 2000`）：
- 快速失败（<2s）：网络瞬断、连接池死连接等，内部 RebuildConnection 后大概率成功
- 超时失败（≥2s）：网络完全断开，内部重试浪费时间，直接返回让外部 reconnect 处理

#### A5. hReq 超时缩短

```cpp
WinHttpSetTimeouts(hReq, 2000, 2000, 2000, 2000);  // 原来是 3000ms
```

即使有漏网之鱼（<3s 间隔但 TCP 已死），死连接也会在 ~2-4s 内失败而非 ~4-6s，
给外部 reconnect 循环留出更多时间。

#### A6. RebuildConnection — 关闭 hConnect+hSession 并重建

```cpp
inline bool RebuildConnection(VolcSession& sess) {
    VolcDebugLog("RebuildConnection: rebuilding hConnect+hSession");
    if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
    if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
    return EnsureConnection(sess);
}
```

#### A7. 外部 reconnect 重试策略

```cpp
if (!volc_asr::OpenSession(g_volcSession, vcfg)) {
    int retryDelays[] = {500, 1000, 2000};
    for (int i = 0; i < 3; i++) {
        if (g_volcSession.forceAbort.load() || !g_volcStreaming.load()) break;
        PostMessageW(g_mainWindow, kHudUpdateMessage, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(
                         L"Reconnecting... (" + std::to_wstring(i + 1) + L"/3)")));
        Sleep(retryDelays[i]);
        volc_asr::RebuildConnection(g_volcSession);  // 每次重试前强制重建
        if (volc_asr::OpenSession(g_volcSession, vcfg)) goto openSessionOk;
    }
    // ... 错误处理 ...
}
```

### 4.3 实测效果

| 场景 | 优化前延迟 | 优化后延迟 | 说明 |
|------|-----------|-----------|------|
| 连续快速录音（<3s） | ~1.8s | ~0.4s | 复用 hConnect+hSession |
| 间隔 3-5s | ~1.8s | ~0.4-0.5s | 过期重建，全新 TCP+TLS |
| 间隔 >5s | ~1.8s | ~0.4-0.5s | 过期重建，全新 TCP+TLS |
| hConnect 复用但 TCP 已死 | ~1.8s + 4.6s 超时 + 4s 内部重试 | ~0.4-0.5s | 3s 阈值拦截，直接重建 |
| 网络断开 | ~1.8s + 3s 重试 | 同左 | RebuildConnection 也失败 |

### 4.4 风险与缓解

| 风险 | 概率 | 影响 | 缓解措施 |
|------|------|------|----------|
| 路由器静默丢弃 TCP 连接 | 中（5-15 分钟空闲后） | WinHttpSendRequest 失败 | 关闭 hConnect+hSession + RebuildConnection + 重试 |
| DNS 缓存过期（TTL 30-300s） | 高 | WinHttpConnect 需要重新解析 | 保留 hConnect 跳过 WinHttpConnect |
| hConnect 句柄泄漏 | 低 | 内存泄漏 | EnsureConnection 过期检查 + ClosePersistentConnection |
| 网络切换导致连接失效 | 低 | WinHttpSendRequest 失败 | 关闭 hConnect+hSession + RebuildConnection + 重试 |
| WinHTTP 内部状态不一致 | 极低 | 不可预测 | RebuildConnection 重建 hSession+hConnect |
| 3s 阈值不够精确 | 低 | 偶尔复用死连接 | hReq 超时 2s 兜底 + 内部重试 |

---

## 5. 边界条件清单

| # | 场景 | 行为 | 特殊处理 |
|---|------|------|---------|
| 1 | 连续快速录音（<3s） | 复用 hConnect+hSession | ✅ 核心优化 |
| 2 | 间隔 3-60s | 过期重建 hConnect+hSession | ✅ EnsureConnection 过期检测 |
| 3 | 间隔 >60s | 同上 | 无 |
| 4 | 路由器静默丢弃 TCP | OpenSession 失败 → 关闭 hConnect+hSession → RebuildConnection → 重试 | ✅ A4 |
| 5 | 本地网络关闭 | OpenSession 失败 → reconnect | 无 |
| 6 | 网络关闭后恢复 | 重试成功 | 无 |
| 7 | 网络波动（丢包） | OpenSession 可能失败 → RebuildConnection → 重试 | ✅ A4 |
| 8 | WiFi→有线切换 | hConnect 失效 → 关闭 hConnect+hSession → RebuildConnection → 重试 | ✅ A4 |
| 9 | VPN 连接/断开 | 可能失败 → RebuildConnection → 重试 | ✅ A4 |
| 10 | 服务端关闭空闲 TCP（~3s） | WinHTTP 自动处理（收到 FIN/RST） | ✅ WinHTTP 内部 |
| 11 | 服务端重启 | WinHTTP 自动重建 | ✅ WinHTTP 内部 |
| 12 | DNS 服务器不可达 | EnsureConnection 失败 | 无 |
| 13 | Esc 取消录音 | forceAbort → join → 保留 hConnect+hSession | ✅ A3 |
| 14 | watchdog 超时 | join → 关闭全部（异常路径不保留） | ⚠️ 保持不变 |
| 15 | WM_DESTROY | 关闭全部（程序退出不保留） | 无 |
| 16 | Settings 切换后端 | ClosePersistentConnection | 无 |
| 17 | PrewarmConnection 启动 | 创建 hConnect+hSession | 无 |
| 18 | init frame 错误 | 关闭 hConnect+hSession（不重建） | ⚠️ 不重建 |
| 19 | API Key 无效 | 服务端返回错误 → 关闭 hConnect+hSession（不重建） | ⚠️ 不重建 |
| 20 | RebuildConnection 也失败 | OpenSession 返回 false → 外部 reconnect | 无 |

---

## 6. 实施细节（与实际代码一致）

### 6.1 volcengine_asr.h — EnsureConnection

```cpp
inline bool EnsureConnection(VolcSession& sess) {
    if (sess.hSession && sess.hConnect) {
        if (sess.lastUsedTick > 0 && GetTickCount64() - sess.lastUsedTick > 3000) {
            VolcDebugLog("EnsureConnection: connection expired (%llums old), rebuilding",
                         GetTickCount64() - sess.lastUsedTick);
            WinHttpCloseHandle(sess.hConnect);
            sess.hConnect = nullptr;
            WinHttpCloseHandle(sess.hSession);
            sess.hSession = nullptr;
        } else {
            VolcDebugLog("EnsureConnection: reusing existing connection (%llums old)",
                         GetTickCount64() - sess.lastUsedTick);
            sess.lastUsedTick = GetTickCount64();
            return true;
        }
    }

    if (!sess.hSession) {
        sess.hSession = WinHttpOpen(L"VoxType/1.0",
            WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sess.hSession) return false;
        WinHttpSetTimeouts(sess.hSession, 5000, 5000, 5000, 5000);
    }

    ULONGLONG t1 = GetTickCount64();
    sess.hConnect = WinHttpConnect(sess.hSession, L"openspeech.bytedance.com",
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!sess.hConnect) return false;
    ULONGLONG t2 = GetTickCount64();
    VolcDebugLog("WinHttpConnect: %llums", t2 - t1);
    sess.lastUsedTick = GetTickCount64();
    return true;
}
```

### 6.2 volcengine_asr.h — RebuildConnection

```cpp
inline bool RebuildConnection(VolcSession& sess) {
    VolcDebugLog("RebuildConnection: rebuilding hConnect+hSession");
    if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
    if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
    return EnsureConnection(sess);
}
```

### 6.3 volcengine_asr.h — OpenSessionImpl 失败点统一模式

6 个 WebSocket 升级失败点（WinHttpOpenRequest / WinHttpAddRequestHeaders / WinHttpSendRequest / WinHttpReceiveResponse / 状态码 != 101 / CompleteUpgrade）：

```cpp
// 统一模式：关闭 hConnect+hSession，快速失败时内部重试
WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess))
    return OpenSessionImpl(sess, cfg, true);
return false;
```

init frame 错误路径：

```cpp
WebSocketCloseGracefully(sess.hWebSocket, &sess);
sess.hWebSocket = nullptr;
WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
return false;  // 不重建——可能是 API Key 问题
```

### 6.4 volcengine_asr.h — CloseSession

```cpp
if (g_volcKeepAlive) {
    VolcDebugLog("CloseSession: keeping hSession+hConnect alive");
    sess.lastUsedTick = GetTickCount64();
} else {
    if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
    if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
}
```

### 6.5 main.cpp — 手动关闭改为条件关闭

```cpp
// no-speech 路径、async/nostream 结束路径
if (volc_asr::g_volcKeepAlive) {
    g_volcSession.lastUsedTick = GetTickCount64();
} else {
    if (g_volcSession.hConnect) { WinHttpCloseHandle(g_volcSession.hConnect); g_volcSession.hConnect = nullptr; }
    if (g_volcSession.hSession) { WinHttpCloseHandle(g_volcSession.hSession); g_volcSession.hSession = nullptr; }
}

// watchdog 和 WM_DESTROY 路径保持不变——关闭全部句柄
```

### 6.6 main.cpp — reconnect 重试

```cpp
if (!volc_asr::OpenSession(g_volcSession, vcfg)) {
    int retryDelays[] = {500, 1000, 2000};
    for (int i = 0; i < 3; i++) {
        if (g_volcSession.forceAbort.load() || !g_volcStreaming.load()) break;
        PostMessageW(g_mainWindow, kHudUpdateMessage, 0,
                     reinterpret_cast<LPARAM>(new std::wstring(
                         L"Reconnecting... (" + std::to_wstring(i + 1) + L"/3)")));
        VolcDebugLog("Volc thread: attempt %d failed, retrying in %dms...", i + 1, retryDelays[i]);
        Sleep(retryDelays[i]);
        volc_asr::RebuildConnection(g_volcSession);
        if (volc_asr::OpenSession(g_volcSession, vcfg)) goto openSessionOk;
    }
    g_volcSession.connected = false;
    // ... 错误处理不变 ...
}
```

---

## 7. 测试要点

1. **连续快速录音**：录音1结束 → 立即开始录音2 → 验证日志 "reusing existing connection"
2. **间隔 3-5s 录音**：验证日志 "connection expired" + "WinHttpConnect" 重建
3. **间隔 >10s 录音**：验证过期重建正常
4. **路由器静默丢弃 TCP**：等待 >5 分钟不录音 → 录音 → 验证 RebuildConnection + 重试成功
5. **拔网线再插回**：拔网线 → 录音 → 验证 reconnect → 插回 → 验证连接恢复
6. **WiFi 切换**：WiFi → 有线 → 验证连接重建
7. **VPN 连接/断开**：验证连接重建
8. **Esc 取消录音**：验证 hConnect+hSession 保留
9. **watchdog 超时**：验证关闭全部句柄（不保留 hConnect）
10. **Settings 切换后端**：验证 ClosePersistentConnection 关闭所有连接
11. **无效 API Key**：验证 init frame 错误后 hConnect+hSession 被关闭（不重建）
12. **Debug 日志**：验证 "reusing existing connection" 和连接年龄
13. **PrewarmConnection**：启动时预建立 → 立即录音 → 验证复用
14. **DNS 服务器不可达**：修改 DNS 为无效地址 → 验证 EnsureConnection 失败 → reconnect
