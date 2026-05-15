# 火山引擎 ASR 连接预热方案 v3

## 问题回顾

日志显示卡住的位置是 `OpenSession` 的 HTTPS 请求阶段（`WinHttpSendRequest`/`WinHttpReceiveResponse`），不是 WebSocket 数据接收。TCP 连接本身 0ms 成功，但 HTTPS 请求卡住 10+ 秒。

**根本原因分析**：从日志看，`WinHttpConnect: 0ms`（TCP连接立即成功），但第一次 `SendRequest+ReceiveResponse` 等待了 14 秒才超时重试。这是典型的 **WPAD 代理自动检测超时** —— Windows 默认会自动查找代理服务器（DHCP 查询 + DNS 查询），每步可能等待 5-10 秒，全部超时后才回退到直接连接。

## 现状分析

当前代码的流程：

```
按下热键 → StartRecordingSession()
  ├─ 开始录音（音频缓冲到 g_volcPendingAudio）
  └─ 启动 volc 线程
       └─ OpenSession() ← 卡在这里（WPAD检测 + TCP+TLS+WS升级，超时10s×2次=20+秒）
            ├─ WinHttpOpen() → WPAD 代理检测（可能卡 10-30秒）
            ├─ WinHttpConnect() → TCP连接（正常 <100ms）
            ├─ WinHttpSendRequest/ReceiveResponse() → HTTPS请求（正常 <500ms）
            └─ 成功 → 进入音频发送循环
```

**核心问题**：
1. **WPAD 代理检测超时**：`WINHTTP_ACCESS_TYPE_DEFAULT_PROXY` 触发自动检测，可能卡 10-30 秒
2. **超时时间过长**：每次连接尝试最多等 10 秒，2 次重试 = 20+ 秒
3. **连接失败直接放弃**：音频数据丢失，用户必须重新录音

## 改进方案：禁用 WPAD + 连接重试 + 音频保留

### 核心思路

1. **禁用 WPAD 代理检测**：改用 `WINHTTP_ACCESS_TYPE_NO_PROXY`，跳过代理自动检测，直接连接
2. **连接失败不放弃**：OpenSession 失败后，只要用户还在录音，就持续重试连接
3. **音频不丢失**：录音数据始终缓冲在 `g_volcPendingAudio`，连接成功后一起发送
4. **缩短单次超时**：每次尝试 5 秒超时（递减到 3 秒），快速失败快速重试
5. **连接成功后快速追赶**：如果连接成功时已有大量缓冲音频，快速连续发送

### 新流程

```
按下热键 → StartRecordingSession()
  ├─ 开始录音（音频缓冲到 g_volcPendingAudio）
  └─ 启动 volc 线程
       ├─ WinHttpOpen(NO_PROXY) — 跳过 WPAD，直接连接
       ├─ OpenSession() — 5秒超时
       │   ├─ 成功（~200ms）→ 进入音频发送循环
       │   └─ 失败 → 如果还在录音，显示 "Connecting..."，重试
       │       ├─ 重试 OpenSession() — 5秒超时
       │       │   ├─ 成功 → 进入音频发送循环
       │       │   └─ 失败 → 继续重试（最多 N 次，或录音结束）
       │       └─ ...
       ├─ 音频发送循环（边录边发，partial 实时显示）
       └─ 录音结束 → 发送结束帧 → 收结果

松开热键 → StopRecordingSession()
  ├─ 如果连接已就绪 → 正常发剩余音频+结束帧 → 快速出结果
  └─ 如果连接还没好 → 最后再试一次连接
       ├─ 成功 → 发送全部缓冲音频+结束帧 → 出结果
       └─ 失败 → 显示 "Network unavailable"，音频不丢失但无法识别
```

### 具体改动

#### 1. 禁用 WPAD 代理自动检测（最关键）

当前代码：
```cpp
// volcengine_asr.h:407-409
sess.hSession = WinHttpOpen(L"VoxType/1.0",
    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
    WINHTTP_NO_PROXY_BYPASS, 0);
```

改为：
```cpp
// 禁用 WPAD 代理自动检测，直接连接
sess.hSession = WinHttpOpen(L"VoxType/1.0",
    WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME,
    WINHTTP_NO_PROXY_BYPASS, 0);
```

**效果**：跳过 DHCP/DNS 代理查找，直接连接服务器，节省 10-30 秒等待时间。

**注意**：如果用户确实需要代理才能访问火山引擎，需要手动配置系统代理（IE 代理设置）。

#### 2. volc 线程中 OpenSession 失败后重试循环

当前代码：
```cpp
if (!volc_asr::OpenSession(g_volcSession, vcfg)) {
    // 直接退出，音频丢失
    return;
}
```

改为：
```cpp
while (!volc_asr::OpenSession(g_volcSession, vcfg)) {
    if (g_volcSession.forceAbort.load()) break;
    if (!g_volcStreaming.load()) {
        // 用户已松开热键，最后再试一次
        // （录音数据还在 g_volcPendingAudio 里）
        break;
    }
    // 还在录音，显示 "Connecting..." 并重试
    VolcDebugLog("OpenSession failed, retrying while recording...");
    ShowHud(L"Connecting... Volcano Engine");
    Sleep(500);  // 等 500ms 再重试，避免密集请求
}
```

#### 3. 缩短 OpenSession 超时

当前：`WinHttpSetTimeouts(hReq, 10000, 10000, 10000, 10000)` — 每次最多等 10 秒

改为：
```cpp
// hSession 级别
WinHttpSetTimeouts(sess.hSession, 5000, 5000, 5000, 5000);

// hReq 级别（OpenSession 内部）- 递减超时
int timeoutMs = (attempt == 0) ? 5000 : 3000;
WinHttpSetTimeouts(hReq, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
```

#### 4. OpenSession 内部重试保留 2 次

保留 OpenSession 内部的 2 次重试，但使用递减超时：
```cpp
for (int attempt = 0; attempt < 2; attempt++) {
    int timeoutMs = (attempt == 0) ? 5000 : 3000;
    // ... 设置超时并尝试连接
}
```

#### 5. 连接成功后快速追赶缓冲音频

```cpp
// 连接成功后，先快速发送已缓冲的音频
EnterCriticalSection(&g_volcAudioCs);
std::vector<BYTE> backlog = std::move(g_volcPendingAudio);
g_volcPendingAudio.clear();
LeaveCriticalSection(&g_volcAudioCs);

// 快速发送积压的音频（不等 partial）
while (backlog.size() >= kChunkBytes) {
    std::vector<BYTE> chunk(backlog.begin(), backlog.begin() + kChunkBytes);
    backlog.erase(backlog.begin(), backlog.begin() + kChunkBytes);
    volc_asr::SendAudio(g_volcSession, chunk, false, asyncMode, nostreamMode);
    if (!g_volcSession.hWebSocket) break;
}
// 剩余不足一个 chunk 的数据放回 g_volcPendingAudio
if (!backlog.empty()) {
    EnterCriticalSection(&g_volcAudioCs);
    g_volcPendingAudio.insert(g_volcPendingAudio.begin(), backlog.begin(), backlog.end());
    LeaveCriticalSection(&g_volcAudioCs);
}
```

#### 6. OpenSession 失败后清理 hSession

```cpp
// OpenSession 失败后，清理所有连接句柄
WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
```

#### 7. 松开热键后如果连接还没好，最后尝试一次

```cpp
// 用户已松开热键
if (!g_volcSession.connected.load()) {
    // 最后一次尝试连接
    VolcDebugLog("Recording stopped but not connected, final connect attempt");
    if (!volc_asr::OpenSession(g_volcSession, vcfg)) {
        // 真的连不上，显示错误
        ShowHud(L"VolcEngine connect failed");
        return;
    }
    g_volcSession.connected = true;
    volc_asr::g_volcKeepAlive = true;
}
```

### 预期效果

| 场景 | 当前 | 改进后 |
|------|------|--------|
| 网络正常 | 连接 ~200ms，体验好 | 不变（跳过 WPAD，可能更快） |
| WPAD 代理检测 | 卡 10-30 秒 | **直接跳过，无延迟** |
| 网络闪断（1-3秒恢复） | 卡 20+ 秒，杀进程 | 自动重试，用户无感知 |
| 网络持续断开 | 卡 20+ 秒，杀进程 | 5秒超时快速重试，录音中持续尝试 |
| 录音中连接失败 | 白录，必须重来 | 持续重试，连上后发送缓冲音频 |
| 松开热键时网络刚恢复 | 白录 | 最后一次连接尝试成功，正常识别 |

### 实施步骤

1. **禁用 WPAD**：修改 `WinHttpOpen` 参数为 `WINHTTP_ACCESS_TYPE_NO_PROXY`
2. **缩短超时**：修改 `WinHttpSetTimeouts` 为 5秒/3秒 递减
3. **添加重试循环**：volc 线程中 OpenSession 失败后持续重试（录音期间）
4. **连接成功后快速追赶**：发送缓冲的音频数据
5. **松开热键后最后尝试**：确保录音数据不丢失
6. **OpenSession 失败后清理 hSession**：避免复用坏连接
7. 编译验证

### 风险评估

| 风险 | 描述 | 缓解措施 |
|------|------|----------|
| 需要代理的用户无法连接 | 禁用 WPAD 后，使用代理的用户需要手动配置 | 在设置中添加代理配置选项 |
| 连接重试过于频繁 | 重试间隔 500ms，可能导致服务器限流 | 添加重试间隔递增策略（500ms → 1s → 2s） |
| 音频缓冲过大 | 长时间重试可能导致音频数据过多 | 设置最大缓冲时间（如 30 秒） |