# Volcano Engine bigmodel_nostream 响应慢 - 优化计划

## 问题

`bigmodel_nostream` 模式下，一次 1.6 秒的录音识别总耗时 ~7-8 秒，其中约 4 秒浪费在等待中间空响应上。

### 耗时分析（来自日志）

| 阶段 | 耗时 | 说明 |
|------|------|------|
| TLS 握手 (WinHttpConnect + SendRequest) | ~2.8s | 网络连 openspeech.bytedance.com |
| Init 帧收发 | ~0.6s | 服务端处理 init JSON |
| **8 次中间 SendAudio 等待** | **~4.0s** | **主瓶颈** |
| 最终结果 (isLast=1) | ~0.7s | 服务端处理全部音频 |
| CloseSession | ~0s | |
| **合计** | **~7-8s** | 对应 ~1.6s 音频 |

### 根因

`volcengine_asr.h:653` 中，每个中间 chunk 都同步等待服务端响应：

```cpp
VolcResult vr = ReceiveResult(sess.hWebSocket, isLast ? 4000 : 150);
```

- 超时设为 150ms，但实际每次等待 ~500ms
- WinHTTP 的 `WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT` 不会提前触发，因为服务端确实在 ~500ms 内发送了数据（空文本 `""`）
- `bigmodel_nostream` 模式下，中间响应全部为 `{"text":""}`，没有实际价值
- 同样的模式在 `bigmodel_async`（asyncMode=true）中已正确跳过

### 结果正确性

最终文本实际已被正确提取（日志 wlen=5）。CloseSession 返回空但 main.cpp 用 `lastPartial` 兜底。纯粹是速度问题。

## 修改方案

### 核心修改：`src/volcengine_asr.h`

1. **SendAudio 函数**：`bigmodel_nostream` 模式下，非最后一个 chunk 不调用 `ReceiveResult`（与 asyncMode 行为一致）

```cpp
// 改前
if (asyncMode && !isLast) {
    return L"";
}
VolcResult vr = ReceiveResult(sess.hWebSocket, isLast ? 4000 : 150);

// 改后
bool skipRecv = (asyncMode || nostreamMode) && !isLast;
if (skipRecv) {
    return L"";
}
VolcResult vr = ReceiveResult(sess.hWebSocket, isLast ? 4000 : 150);
```

2. **新增参数**：`SendAudio` 增加 `nostreamMode` 参数，或复用 `asyncMode` 语义

3. **发送 isLast 后循环 drain**：在 `ReceiveResult(isLast=true)` 后，如果返回空文本，继续循环 drain 中间排队的空响应，直到拿到有文本的结果或超时

### 辅助修改：`src/main.cpp`

4. **调用处传参**：将 `bigmodel_nostream` 的 nostream 标志传给 SendAudio

5. **drain 逻辑**：发送 isLast 后，如果 SendAudio 返回空，循环调用 ReceiveResult 直到拿到文本

### 预估效果

- 中间 chunk 等待：4.0s → 0s（跳过 ReceiveResult）
- 总耗时：~7-8s → ~3-4s

### 文件清单

- `src/volcengine_asr.h`：SendAudio 函数修改
- `src/main.cpp`：调用处修改

## 后续优化（不在本次范围）

- WebSocket 连接复用 / 预热，避免每次 2.8s TLS 握手
- 考虑切换到 `bigmodel` 流式模式获得实时中间结果
