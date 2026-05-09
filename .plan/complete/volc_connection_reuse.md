# Volcano Engine 复用 WinHTTP 连接

## 目标

跨录音会话复用 `hSession` + `hConnect`（TCP/TLS 连接），每次只新建 WebSocket 会话。
预估省掉 ~1.4s TLS 握手（第二次录音起）。

## 原理

```
第一次录音：
  WinHttpOpen → WinHttpConnect → TLS握手(1.4s) → WebSocket升级 → 发音频 → 收结果 → 关WebSocket
  [hSession/hConnect 保留]

第二次录音：
  [hSession/hConnect 已存在，跳过] → WebSocket升级 → 发音频 → 收结果 → 关WebSocket
  ↑ 省掉 1.4s
```

- WebSocket 会话每次新建/关闭，不影响服务端
- 底层 TCP/TLS 连接保持，服务端有 keep-alive 超时（通常 60-300s）
- 长时间不录音：服务端关闭连接，下次检测失败后自动重建

## 修改文件

### 1. `src/volcengine_asr.h`

**新增 `EnsureConnection()` 函数：**
- 检查 `sess.hSession` + `sess.hConnect` 是否有效
- 有效时尝试 `WinHttpOpenRequest` 验证连接存活
- 无效或验证失败时重建连接
- 返回 `bool`

**修改 `OpenSession()`：**
- 调用 `EnsureConnection()` 代替直接创建连接
- 移除 `WinHttpOpen` / `WinHttpConnect` / `WinHttpSetTimeouts` 代码
- 失败时清理连接并重试

**修改 `CloseSession()`：**
- 检查 `g_volcKeepAlive`
- 为 true 时只关 `hWebSocket`，保留 `hSession` / `hConnect`
- 为 false 时行为不变（完全清理）

**新增 `ClosePersistentConnection()`：**
- 关闭 `hSession` / `hConnect`
- 程序退出时调用

### 2. `src/globals.h`

- 新增 `extern bool g_volcKeepAlive;` 声明

### 3. `src/main.cpp`

- 定义 `bool g_volcKeepAlive = false;`
- 录音线程中设置 `g_volcKeepAlive = true`（OpenSession 成功后）
- `wWinMain` 退出前调用 `volc_asr::ClosePersistentConnection(g_volcSession)`

## 容错

| 场景 | 处理 |
|------|------|
| 连接被服务端超时关闭 | `WinHttpOpenRequest` 失败 → 重建连接 |
| TLS 错误 | 重建连接 |
| WebSocket 升级失败 | `OpenSession` 返回 false，清理连接 |
| 程序退出 | `ClosePersistentConnection` 释放资源 |
| Settings Save 切换模式 | 连接仍然有效，下次录音自动复用 |
