# VoxType 代码审查 — 2026-08-29

审查范围：`src/` 全部 34k 行（app / asr / audio / ui / core），重点是四个网络 ASR
后端的连接正确性、线程安全与资源生命周期。

**总判断：四个后端的连接实现都是正确的。** 协议层的帧编解码、端点解析、重试分类、
连接复用未发现错误。问题集中在三处：一处确定的 UI 冻结、跨后端不一致的文本处理与
缓存策略、若干未定义行为与代码整洁项。

> 第三轮交叉审查（2026-08-30，架构与维护性层面）的方向性结论：正确性问题已基本挖净，
> 新增发现全部收在新加的「P3 — 架构 / 维护性」一节，均为 P3 级、不影响正确性。
> 其中 P3-1（启动块去重）与 P2-2 的修复点重合，建议合并为一个改动切片。
>
> 第四轮复核（2026-08-30，独立逐条核验 + 对照官方协议文档）：全部既有条目属实、无夸大；
> 「待核实」的 `MSG_ERROR_RESP` 已定案并降级到「无需改动」；新增 P1-6
> （百度 `expires_in` 提取恒失败）与 P2-1 的 NULL 防御补充；两条低优先观察项并入 P2-11 表。
>
> 第五轮复核（独立 AI 交叉核验 + 微软官方 WinHTTP 并发文档对照）：三处定性修正——
> P1-1 冻结上界漏算 OpenSession 最坏 +12.5 s、P2-3 的"文档化安全"不成立（同步句柄）、
> P2-4 "锁外 Abort" 建议撤回（裸指针指向 worker 栈对象）；新增 P2-12（Settings 测试
> generation）、P3-5（火山请求 JSON 拼接安全）、P3-6（百度/火山离线协议测试）；
> P1-2 / P1-3 / P1-6 按实际影响下调定级；修复顺序按新证据重排。

---

## P1 — 会冻结 UI / 功能性错误

### 1. Volcengine 空结果重放（`RetryRecognitionOnce`）无法被 `Abort()` 取消 → UI 冻结

`RetryRecognitionOnce` 用的是**局部** `retrySess`（`:264`），其 `forceAbort` 全程只被
`.load()` 读取、从未置位；`Abort()` 在 `:162` 置的是全局 `s_volcSession.forceAbort`，
是另一个对象。**三个等待点一个都不检查 `abort_`**：
`:270` 的 OpenSession 重试循环、`:294` 的发送循环、`:333` 的 drain 循环。
`Abort()` 随后在 `:172` `worker_.join()`，调用方线程会一直等到重放跑完。

作为对照，Qwen / Qwen Audio 3 / Qwen Free / Doubao 四个 session 的循环全部检查
`abort_`（连 qwen_free 的重放循环 `:722/:742/:749` 都查），**火山是唯一的例外**。

冻结上界（块数 = `kVolcMaxReplayBytes` 3,840,000 ÷ 6400 = 600）：

| 模式 | 每块 receive | 发送耗时 | drain 上限 | 合计 |
|---|---|---|---|---|
| `bigmodel_nostream`（**默认**） | 否（`:873` 直接返回） | ~1 s | 12 s（fallback 开）/ 30 s（关） | **约 15 s / 33 s** |
| `bigmodel_async` | 否 | ~1 s | 同上 | 约 15 s / 33 s |
| `bigmodel` | 是，150 ms/块（`:877`） | 90 s | 同上 | **约 105 s** |

**上界修正（第五轮复核）**：上表没有计入重放阶段的 OpenSession 重试。最坏情形
（服务器不可达、三次 open 全部失败）：三次硬超时 3 s / 3 s / 5 s
（`kVolcOpenHardTimeouts = {3000, 3000, 5000, 6000}`，`volcengine_streaming_session.cpp:36`）
加两次退避 500 + 1000 ms，open 阶段最坏再 **+12.5 s**。故冻结上界的保守值应为
**约 27.5 s / 45.5 s**（nostream / async）与**约 117.5 s**（bigmodel）。
服务器可达的正常重放场景下 open 亚秒级完成，仍是表内数字。

drain 上限来自 `VolcFinalizeWaitMs`：`ComputeCloudAsrStreamingFinalWaitMs`
`clamp(audioMs×0.25+4500, 6000, 12000)` = 12 s；无 fallback 时
`ComputeCloudAsrLegacyFinalizeTimeoutMs` `clamp(audioMs×0.8+6000, 8000, 30000)` = 30 s。
另加 `CloseVolcSessionHandles` → `WebSocketCloseGracefully` 的 3 s 关闭握手。
默认模式为 `bigmodel_nostream`（`globals.h:365`、`engine.cpp:552/697`），
三档可选（`settings.cpp:1461-1463`）。

**两个受害点**（同一根因）：

1. `main.cpp:1633` 新热键按下 → `AbortAndResetActiveStreamingSession()` → `Abort()` → join
   —— **阻塞 UI 线程 / 消息泵**，这是主要危害。
2. `main.cpp:2314` 看门狗超时 → 同上 —— **同样阻塞 UI 线程**。
3. `main.cpp:495` `RunStreamingAsrOnce()` 在 cv 等待超时后对 fallback session 调
   `Abort()` —— 阻塞的是 fallback 工作线程而非 UI，效果是"fallback 超时"结果要再晚
   约 15~33 s 才报到。危害较低，但同根因，验证修复时应一并覆盖。

**修法**：把 `abort_`（或共享 `std::atomic<bool>*`）传进 `RetryRecognitionOnce`，
在 `:270`、`:294`、`:333` 三个等待点都检查，并让 `Abort()` 能关到重放会话的句柄，
使 close-to-cancel 对重放同样生效（惯例见 P2-3）。

> ⚠️ **第五轮复核补充**：让 `Abort()` 关到重放句柄，等于继续沿用现状"跨线程关闭同步
> WinHTTP 句柄"的务实做法——实测有效，但**不是微软文档化的取消语义**（见 P2-3 修正）。
> 短期"abort_ 三检查点 + 独立句柄槽"是有效缓解；长期方向是异步 WinHTTP 或
> worker 独占句柄 + UI 线程不同步 join 的生命周期设计。二者不要混为一谈。

> ⚠️ **实现约束**：**不要把 `retrySess` 的句柄登记进 `s_volcSession`。**
> AGENTS.md 明确 `g_volcSession` 全局是留给 `hSession/hConnect` 跨录音预热复用的，
> 往里塞单次重放的句柄会破坏 Settings 保存后的预热连接。
> 正确做法是另设一个**独立的原子句柄槽**，照 `AtomicTakeWebSocket()`
> （`:95-100`，`InterlockedExchangePointer` 惯例）实现；
> `Abort()` 里同时关主会话槽与重放槽两套句柄，互不干扰。

### 2. `utils.h::ExtractJsonStr` 不反转义 → Volcengine / 百度 识别文本被污染 —— 定级由 P1 调整为 P2（第五轮复核：低命中频率的正确性问题，见 P3-6 测试切片）

`utils.h:48-71` 正确统计连续反斜杠来定位结束引号（符合 AGENTS.md 踩坑记录），
但 `return Utf8ToWide(json.substr(start, pos - start))` 返回**未解码**的原始串：
`\"`、`\\`、`\n`、`\uXXXX` 全部原样保留。

受影响：
- `volcengine_asr.h:304` `ExtractJsonStr(payloadJson, "text")`
- `baidu_asr.h:190` `ExtractJsonStr(body, "result")` —— 百度 `result` 是数组，
  `:57` 要求冒号后是 `"` 否则返回空，因此**必然**落到 `:192-205` 的手工数组解析，
  那段用裸 `find('"')` 定位，**遇到文本内的转义引号会截断**

对比：Qwen（`qwen_asr.cpp:228`）、Doubao（`doubao_ime_asr.cpp:496`）、
MiMo（`mimo_asr.cpp:189 ExtractJsonStringDecoded`，含 `\uXXXX` 代理对）都有完整实现。
同一份文本在不同后端表现不一致 —— 这是最该统一的点。中文口播里引号频率低，
属低频正确性问题，但命中即错字。

**修法（两步，缺一不可）**：

1. 把 `ExtractJsonStringDecoded` 提到 `utils.h`，替换 `ExtractJsonStr` 的所有调用点
   —— 这一步修好**火山**（`:304`）。
2. **百度不能只靠第一步**：`result` 是数组，换成 `ExtractJsonStringDecoded` 后
   仍会因冒号后不是 `"` 而返回空，仍然落回 `:192-205` 的手工数组解析。
   那段必须一并改成**转义感知**的扫描（至少统计连续反斜杠数量再定位结束引号，
   可复用 `utils.h:60-69` 的判定逻辑），否则截断 bug 原样保留。
   更彻底的做法是给百度写一个 `ExtractJsonArrayFirstStringDecoded()`。

### 3. Volcengine `TestConnection` 只验证 HTTP 101，不验证 ASR 会话 —— 定级由 P1 调整为 P2（第五轮复核：影响配置诊断，不直接影响识别结果）

`volcengine_asr.h:990-1163` 完成 WebSocket 升级即返回 `Connection OK`（`:1131-1135`），
**从不发送 full client request**。而错误的 `resource_id` / `mode` 是服务端在升级
**之后**通过 error frame（`MSG_ERROR_RESP`）返回的 → 用户看到"连接正常"，识别必然失败。

**修法（第五轮复核修正）**：不要照抄现有握手流程再拼一份 init——官方协议里 full client
request 会收到 full server response。直接构造一个**本地临时 `VolcSession`**、复用生产的
`OpenSession()`（`volcengine_asr.h:829`），用其结果（init 响应 / 错误帧）判定配置正确性。
测试路径与生产路径共享同一份握手 + init 实现，避免维护两份协议代码。

> 💡 **验证方式**：修复后可**故意填一个错误的 `resource_id`**，确认 Test Connection
> 能把服务端的 error frame 读出来并提示配置错误，而不是显示"连接正常"。
> （错误帧的解析布局已经对照官方文档定案——见「无需改动」——不再承担协议定案任务。）

### 4. 百度 access token 缓存不随凭据变更失效

`baidu_asr.h:128` `GetAccessToken()` 用函数静态变量缓存，有效期取 `expires_in`
（默认 30 天）。`ClearCachedToken()` 全项目仅 2 处出现（定义 `:122`、
唯一调用 `:349`，位于 token 出错分支），Settings 改 API Key / Secret 后不失效。

实际表现：换 Key 后仍用旧 token。若旧 key 仍有效，识别照常工作 ——
**新凭据被静默忽略**（计费/归属仍是旧 key）；旧 key 作废时走一次 token 错误
→ `:349` 清缓存 → 换新 key 重试，能自愈。

**修法（第五轮复核修正，覆盖测试按钮路径）**：仅在 `SaveConfig()` 清缓存不够——
Settings 的百度测试按钮（`settings.cpp:3628-3643`）直接从**未保存的输入框**读
Key/Secret 调 `TestConnection` → `GetAccessToken`，不经过保存路径。
正确做法是让缓存绑定凭据身份：缓存同时保存 `token + expiresAt + apiKey/secretKey 指纹`
（哈希或原文比对），`GetAccessToken` 开头比对指纹，不一致即视为未命中重新获取。
识别与测试连接就都不再依赖保存动作。

### 5. 百度请求超时固定 8 秒，与音频长度无关

`baidu_asr.h:242` `req.timeoutMs = 8000` 被 `SendCloudHttpRequest`
同时用作 resolve / connect / **send** / receive 四段超时
（`cloud_http_common.cpp:97`、`:126`）。WinHTTP 的 send 超时覆盖整个请求体发送，
60 秒录音 ≈ 1.9 MB，需要 ≥240 KB/s 的实际上行速率，慢链路必然超时。
MiMo 有自适应（`mimo_asr.cpp:334-337`），百度没有。

**修法**：不需要新写公式 —— 直接复用公共层现成的
`ComputeCloudAsrRecordedRequestTimeoutMs`（`cloud_asr_common.cpp:45`，
`audioMs×0.8+6000`，clamp 8~30 s），与 MiMo 行为对齐：

```cpp
req.timeoutMs = ComputeCloudAsrRecordedRequestTimeoutMs(0.0, pcm.size());
```

注意 `baidu_asr.h:17-20` 目前只 include 了 `cloud_http_common.h`，
需补 `#include "cloud_asr_common.h"`。
（若要与 MiMo 完全一致的余量，可照 `mimo_asr.cpp:335` 再 `+12000` 并 clamp 到 15~60 s。）

### 6. 百度 token 响应的 `expires_in` 永远提取失败 —— 定级调整为 P3（第五轮复核：默认值恰好与官方 30 天一致，属防未来变化）

`baidu_asr.h:154` 用 `ExtractJsonStr(respUtf8, "expires_in")` 提取有效期，
但百度 token 响应里 `expires_in` 是**数字**（`"expires_in":2592000`，无引号），
而 `utils.h:57` 要求冒号后是 `"` 否则返回空 → `expiresInStr` 恒为空 →
`expiresIn` 永远走默认值 2592000（30 天，`baidu_asr.h:162-166`）。

当前恰好无害：百度实际有效期就是 30 天。但一旦百度调整有效期，
缓存窗口就与实际不符，新旧 token 的切换只能靠 token 错误分支自愈（见 P1-4）。

**修法**：改用同文件已有的 `ExtractJsonInt`（`baidu_asr.h:36`，`:260/:420` 提取
`err_no` 已在用）：

```cpp
int expiresIn = ExtractJsonInt(respUtf8, "expires_in", 2592000);
if (expiresIn <= 0) expiresIn = 2592000;
```

一行替换，不需要新 include。与 P1-4 / P1-5 合并为同一个热身切片。

---

## P2 — 健壮性 / 一致性

### 1. 单实例互斥量创建过晚

`main.cpp:2458` 才 `CreateMutexW(L"Local\\VoxType.SingleInstance")`，而在此之前已经
detach 了两个线程：`:2445` 模型预加载（`PreloadAsrEngine`，加载完整 ONNX 模型）
和 `:2453` Volcengine 预热（`VolcenginePrewarmConnection`，建 TCP/TLS 连接）。

第二个实例会先加载几百 MB 模型、并对 `openspeech.bytedance.com` 建连，
**之后**才发现自己是重复实例并退出；两个 detached 线程也没被 join。
（第二个实例在 `:2465` 就 return 了，永远走不到 `:2476` 的 `g_mainWindow` 赋值，
因此其预加载线程若跑到 `:2447` 的 `PostMessageW(g_mainWindow, ...)`，
看到的必然是 nullptr；但这是一处未同步竞态 —— 通常第二个实例在那行之前就已退出。
无论哪种情况都无害：返回值 FALSE，不崩，只是模型白加载了。）

**修法**：把 `CreateMutexW` 提到 `wWinMain` 最前面，`LoadConfig` 与预加载之前。
另加一行 `if (!mutex)` 防御：当前若创建失败（返回 NULL 且非 ALREADY_EXISTS），
进程继续运行、完全失去单实例保护，退出路径的 `if (mutex)` 只是静默跳过关闭。
防御与本次移动同切片。

### 2. 流式后端开头可能丢失回放+安装窗口内的全部回调块（多至数十 ms 级音频，非单个周期）

Qwen（`main.cpp:1655→1658`）、Volcengine（`:1761→1764`）、Doubao（`:1687→1690`）
的"回放"与"安装"不是原子的：

```
1654  StartStreamingVadTrimmerForCloud(...)
1655  ReplayPreCapturedAudio(session.get())   // :1454 拷贝 g_audioData
1657  EnterCriticalSection(&g_streamingSessionCs);
1658  g_activeStreamingSession = std::move(session);   // 安装
```

回调先 `insert` 进 `g_audioData`（持 `g_audioLock`），再 `EnqueuePcmChunk`
（持 `g_streamingSessionCs`，`wasapi_capture.cpp:338-352`）。若某次回调的
insert+enqueue 整体落在拷贝与安装之间，该块进了 `g_audioData`
（fallback / 诊断仍有）但**从未送给提供者**。

若块在拷贝前已 insert，它会进回放，而其 enqueue 发生在安装前（session 为 null）
被跳过 —— 只发一次，不会重复；重复仅在回调于 insert 与 enqueue 之间被抢占
超过整个回放窗口（>1 ms）时才可能，概率极低。

窗口大小 = `ProcessPcm16` 跑完预采集缓冲的时间。预采集缓冲的量取决于
1585→1655 的耗时；其中 `:1632 BeginAsrAttempt` 在
`qwenEnableInputContext && Audio3 模型` 时会做 UIA 取词，按 `main.cpp:1613-1615`
的注释**可达数百毫秒**。故窗口是毫秒级（数百 ms 级），期间**所有**落入窗口的回调块
（可多至数个 WASAPI 周期）都会进 `g_audioData` 而不送 provider（第五轮复核修正措辞：
不是"约一个周期"，是"窗口内全部块"），命中时表现为"开头第一个字/词偶尔被吃掉"。

Qwen Free 的 `ActivateQwenFreeStreamingSession`（`:1475-1495`）已正确同时持有
`g_audioLock` + `g_streamingSessionCs`，没有这个窗口。

**修法范围要覆盖两条采集路径**：除了上面引用的 WASAPI 回调
（`wasapi_capture.cpp:332-352`），**waveIn 回调（`engine.cpp:883-902`）有一模一样的
insert / enqueue 双临界区结构和同样的窗口**。好在回放拷贝点（`main.cpp:1454`）与
安装点（`:1658/:1690/:1764`）是两条路径共享的，双锁修法天然覆盖两者 ——
但实现时别只改 WASAPI 那侧。

**修法**：把 `ActivateQwenFreeStreamingSession` 的双锁写法套用到另外三个后端。

### 3. Volcengine `Abort()` 关闭句柄早于 join —— 现状是务实方案，但"文档化安全"不成立（第五轮复核修正）

`volcengine_streaming_session.cpp:170` 的 `AtomicTakeWebSocket()` + `WinHttpCloseHandle`
早于 `:172` 的 `worker_.join()`；`drainThread` 要等 worker 走到 `:711` 才被 join。
故确实存在窗口：句柄被关，而 drain 线程正阻塞在 `WinHttpWebSocketReceive`（`:539`）。

原结论"现状即设计、不建议改动"需要修正定性。第五轮复核对照微软官方
「Concurrency in WinHTTP」的 Handle close 规则：**同步请求句柄只允许在
"没有任何线程正在阻塞调用它"时关闭**；"关闭句柄取消进行中的操作"是**异步**请求
才有的文档化语义。而本仓库火山会话是同步的：`volcengine_asr.h:371` `WinHttpOpen`
无 `WINHTTP_FLAG_ASYNC`；`:667` 注册的只是 `WINHTTP_STATUS_CALLBACK` 状态追踪回调，
不改变同步性质；公共 `CloudHttpCancellation`（`cloud_http_common.h:14-26`）也是同类模式；
仓库内 `winhttp_websocket_transport.cpp:242-243` 注释自己写的也是
"documented cancellation mechanism for outstanding **asynchronous** I/O"。

因此修正后的三点结论：

1. **保留现状**：实测有效（消除 3 s UI 冻结；`ReceiveResult` 对关闭句柄的错误消化，
   `:207`/`:211-218`），在同步 WinHTTP 前提下它是可工作的务实方案，不应在无替代设计时推倒。
2. **但不得标为"已证明安全 / 文档化"**；P1-1 的句柄槽修法同理——沿用同类务实语义，
   不自称文档背书。
3. 一个精确边界：`WinHttpWebSocketClose`（优雅关闭）导致的
   `ERROR_WINHTTP_OPERATION_CANCELLED` 是 `WinHttpWebSocketReceive` 文档列明的返回码；
   `Abort()` 走的 `WinHttpCloseHandle` 硬关闭不在其中。

**长期方向**：异步 WinHTTP，或 worker 独占句柄 + UI 线程不同步 `join()` 的生命周期设计。
在长期方案落地前，短期不要改成"停止事件 + join + 再关句柄"——那会让 `Abort()` 干等
receive 超时（isLast 最长 4 s），把"立即取消"倒退成"等待超时"。

### 4. Doubao `AbortActiveClient()` 持互斥锁调用会阻塞的 `Abort()` —— 原"锁外 Abort"建议撤回（第五轮复核修正）

`doubao_ime_streaming_session.cpp:209-214`：`lock_guard(activeClientMutex_)` 内调
`activeClient_->Abort()`。`RealtimeClient::Abort()`（`doubao_ime_asr.cpp:1494`）
不 join 线程（只关句柄），所以**不会死锁**，但仍可能在 UI 线程停顿。

原建议"锁内取句柄、锁外关闭"是**危险改动**：`activeClient_` 是裸指针，而重放路径
（`:351-352`）把 **worker 线程栈上的局部 `RealtimeClient`**（`doubao_ime_asr::RealtimeClient
client(retryCfg); SetActiveClient(&client);`）注册进去。若 UI 线程在锁内拷贝指针、
锁外调用 `Abort()`，worker 可能在两次操作之间完成 `ClearActiveClient` 并返回——
栈对象析构 → use-after-free。

**结论**：保持现状（持锁 Abort）。若要缩短持锁时间，须先把 `activeClient_` 改为
`shared_ptr`（或独立生命周期稳定的取消对象）再谈锁外调用，避免裸指针持有栈对象。

### 5. Volcengine 空闲时向音频流补发静音保活

`volcengine_streaming_session.cpp:607-618`：空闲 3.5 s 发 6400 字节（200 ms）全零，
且该块也会计入 sequence。

**这更像是有意的保活，不建议删除。** 关键事实：当流式 VAD trim 开启时
（`wasapi_capture.cpp:340-348`），静音段根本不会进入 `pendingAudio_`
—— trimmer 无输出即不 enqueue。因此**在静音期间，保活块是服务端收到的唯一音频**。
它不是"在真实静音之上再叠一层静音"，而是防止服务端音频空闲超时的心跳。
（VAD 关闭时真实麦克风音频持续流入，`pendingAudio_` 始终有数据，该分支基本不触发。）

建议：保留原样；若想确认，可向服务方核实其音频空闲超时时长是否短于 3.5 s。

### 6. Volcengine 分片重组上限 64 片

`volcengine_asr.h:229` `for (int i = 0; i < 64; i++)`。超过 64 片静默截断。
而且**循环里三个 `break` 中有两个会留下半截消息去解析**：

- `:234` `if (err != ERROR_SUCCESS) break;` —— 收取出错
- `:235` `if (moreBytes == 0) break;` —— 对端提前结束分片

两者 `break` 后，`:243` 起的解析代码照常拿不完整的 `responseBody` 去解帧，
可能解出乱码或静默丢结果。（第三个 `:237` `bufType != FRAG` 是正常收尾，没问题。）

应改为按长度循环，且上述两种提前退出一律**丢弃整条消息**而不是继续解析。
（实际响应是小 JSON，命中概率低。）

### 7. `GenerateUuidStr()` 不是真 UUID

`volcengine_asr.h:105`：由 tick + tid + **栈地址**拼成，用作 `X-Api-Connect-Id`。
同线程同毫秒内两次会话可能撞号。改用 `CoCreateGuid()` / `UuidCreate()`。

> ⚠️ **注意别顺手"统一"掉第二个实现**：`qwen_free_proto_llm.cpp:175-184` 还有一个
> `GenerateUuid()`（32 hex 无分隔），熵源是 `random_device`（无栈地址缺陷），
> 用途是 qwen_free 协议的 `reqId` / `llmSessionId`，与火山的 Connect-Id 无关。
> 本条目只改火山这一个；顺手给两处各加一行注释说明用途差异，防止未来被误合并。

### 8. `wasapi_capture.cpp:355` 在临界区外读共享指针

`LeaveCriticalSection` 在 `:353`，`:355` 又读了一次 `g_activeStreamingSession`
（以及 session 非空时的 `g_streamingVadTrimmer`）。

实际影响：流式路径下 session 与 trimmer 均非空 → 括号内为真 → 取反为假 → 整块被跳过，
只发生一次无锁读、不调用任何方法；batch 路径下 session 恒为 null（batch 后端从不
安装流式 session）→ `&&` 短路，连 trimmer 都不会被读。
属理论 UB、实际不可观测，归为代码整洁问题。

### 9. 跨线程非原子全局

| 变量 | 写方 | 读方 |
|---|---|---|
| `g_captureActive` | `engine.cpp:916/982/1045`（回调+UI）、`main.cpp:2372`（UI） | `engine.cpp:905/908`（回调） |
| `g_inputContextResult` | `volcengine_streaming_session.cpp:406`（worker）、`main.cpp:709`（UI） | `main.cpp:2222` / `DebugPrintInputContext`（UI） |
| `g_cloudApiMs` / `g_vadMs` | 各 session worker | UI 线程打印 |

建议改 `std::atomic` 或加锁。目前未见由此引发的实测故障。

### 10. 只有旧版 Qwen realtime 实现了 `MaxRecordingMs`（55 s）

`MaxRecordingMs` 全项目仅 3 处：声明（`asr_streaming_session.h:35`）、
使用（`main.cpp:2281`）、唯一实现（`qwen_streaming_session.cpp:123-124`）。
Volcengine / Doubao IME / Qwen Free / Qwen Audio 3 streaming / 全部 batch 后端**无上限**。
batch 后端需把 PCM+WAV+base64（≈2.7×）全放内存（10 分钟 ≈ 19 MB PCM / 25 MB base64）。

**护栏不一致**：MiMo 有上传体上限，会在本地提前拒绝
（`mimo_asr.cpp:26` `kMaxBase64Bytes = 10 MiB`，`:328-331` 超限即报
"audio exceeds 10MB base64 limit"）；**百度 batch 没有任何上限**，
超长录音会先把几 MB 流量全部传完再被服务端拒绝 —— 白费流量且用户等待更久。

建议：① 给百度加同类的上传体上限，超限本地提前报错（百度官方短语音接口上限 60 s，
本地限制可直接取 60000 ms）；② 各后端都给录音时长上限
（`MaxRecordingMs`）并在 HUD 提示。

### 11. 其他

| 位置 | 问题 |
|---|---|
| `volcengine_asr.h:1089` | `ReadResponseHeaderStr(hReq, WINHTTP_QUERY_CUSTOM)` 未传头部名，必然失败返回空；下面的 raw-headers fallback 才真正取到 `X-Tt-Logid`。死代码，可删。 |
| `volcengine_asr.h:803` | init frame 的 `WinHttpWebSocketSend` 返回值未检查，要等 5 s 超时才暴露失败。 |
| `baidu_asr.h:208/255/265/275/356`、`wasapi_capture.cpp`（15 处 `[WASAPI]`）、`engine.cpp:985/990`、`asr_session.cpp:96/168/244/322` | GUI 子系统里 `printf` 诊断，未开 Debug Mode 时不可见，也不进日志。**清扫时注意隐私边界（第五轮复核）**：百度打印含完整响应体（可能含识别文本），不能机械改写成持久化日志；应只记录状态码 / 错误码 / 字节数，原始响应仅在显式开启的敏感诊断模式下落盘。 |
| `main.cpp: MainWndProc` | 未处理 `WM_QUERYENDSESSION` / `WM_ENDSESSION`。 |
| `main.cpp:2468-2474` | `RegisterWindowClasses()` 失败路径没有 `CloseHandle(mutex)`（进程随即退出，仅一致性问题）。 |
| `main.cpp:2490-2496` | `!g_mainWindow` 失败路径同样漏了 `CloseHandle(mutex)`，同上。 |
| `volcengine_asr.h:198-199` | `ReceiveResult` 每次调用都 `WinHttpSetOption` 重设 receive timeout——`bigmodel` 模式 150 ms/块 下是高频无谓调用。低优先，知道即可，不急于改。 |
| `volcengine_streaming_session.cpp:311-313` | `RetryRecognitionOnce` 里 `AppendStageInput` 把整段重放 PCM（最大 3.84 MB）再拷贝一份进诊断缓冲，重放期间内存接近 2 倍。低优先，知道即可，不急于改。 |

### 12. Settings 测试结果缺少统一代次控制（第五轮复核新增）

百度 / 火山 / Qwen / MiMo / LLM 的测试结果共用 `WM_APP + 10` 一个消息
（`settings.cpp:1900/3641/3684/3706/3731/3756/3788` 七处 Post，`:3923` 一个 handler），
不带 generation。用户快速连点两个 provider 的测试按钮时，先发起的慢测试会在后发起的
测试完成之后才返回，把状态栏覆盖成上一个 provider 的旧结果（"last writer wins"，
且赢的不是最后发起者）。

`globals.h:56-57` 已写明 "Settings uses WM_APP + 10 locally for test results"。
Doubao / Qwen Free 已有独立消息 ID（`WM_APP+11/12/13`，`settings.cpp:445/457/465`），
可参照推广：每次发起分配递增 generation，handler 只接受当前 generation 的结果。

---

## P3 — 架构 / 维护性（第三轮补充，不影响正确性，可随前述切片穿插）

### 1. main.cpp 四个流式后端启动块是复制粘贴 —— 与 P2-2 修复同点合一

`main.cpp:1638-1777` 的 qwen / doubao_ime / qwen_free / volcengine 四个块，
每块约 35 行，序列完全相同：Create → SetPartialCallback → SetFinalCallback →
Start → Replay → 安装 → ShowHud → watchdog 查询 → SetTimer，只有参数不同。

P2-2（双锁回放）本来就要统一这四块的"回放+安装"段，因此去重与 P2-2 是同一处代码：
把 `ActivateQwenFreeStreamingSession`（`main.cpp:1475-1495`）泛化为共享的
`ActivateStreamingSession(session, watchMsgText)`，一次改动、一次回归。

注意边界：`main.cpp:980-1065` 的 fallback/final 编排（`StreamingFinalCallback`、
`DispatchStreamingFallbackAsync`、共享消息通道）**已是**单一实现，没有被后端复制 ——
只收敛启动块，不要动已经干净的 fallback 通道。

### 2. 五个 streaming session 的诊断三件套逐字重复 —— 只上移纯公共部分

`PrimaryStage / RetryStage / CompletePrimary` 在五个文件里各有一份近乎相同的实现：
`qwen_streaming_session.cpp:132-143`、`qwen_audio_streaming_session.cpp:142-153`、
`qwen_free_streaming_session.cpp:205-221`、`doubao_ime_streaming_session.cpp:174-191`、
`volcengine_streaming_session.cpp:196-211`。纯样板、零协议逻辑，可上移到
`asr_streaming_session_base.h`。`retryDelays` 之类的常数（火山内部还定义了两套：
重放 `{500,1000}` / 主循环 `{500,1000,2000,3000}`）建议集中到 `cloud_asr_common.h`。

**边界（遵守 AGENTS.md）**：只提纯公共部分（诊断样板 / 常数 / 日志）；
send loop、drain、retry 这些含协议差异的骨架**不抽成模板**，继续留在各自 session 内。

### 3. `llm_refine.h:25-28` 命名空间转导出（一行级清理）

`namespace llm` 里 `using ::WideToUtf8;` 等 4 条把 utils.h 的函数引入 llm 命名空间，
掩盖真实依赖关系，未来若在 llm 内新增同名函数会静默冲突。
可选：保留但加注释说明 re-export 意图，或改为限定调用。

### 4. 大文件拆分 —— 明确暂缓

`engine.cpp` 混合采集、86 键配置读写、引擎生命周期、预加载；`utils.h` 是 2400+ 行
单头堆放；`globals.h` 体量也大。拆分属纯维护性重构、有回归风险，与功能改动错开排期。
**P1-2 只做 `ExtractJsonStr → ExtractJsonStringDecoded` 的替换，不要顺手拆 `utils.h`**，
避免放大 diff 干扰审查。

### 5. 火山请求构造的 JSON 拼接不安全（第五轮复核新增）

- **Extra Params 手写拆分**（`volcengine_asr.h:526-564`）：逐字符扫 key/value、
  括号计数，完全不感知字符串字面量——value 内含 `,`、`{`、`}`、转义符时会被截断
  或拼出无效 JSON。
- **hotwords / correct-table 字段直接拼接**（`volcengine_asr.h:568-578`）：
  `"boosting_table_id":"` + 原值 + `"`，名称含 `"` 或 `\` 直接破坏请求；
  对照 `volcengine_streaming_session.cpp:54-74` 已有 `JsonEscape` 却未在此复用。
- **凭据最小暴露**：full client request 的 `user.uid` 用 API Key 前 12 字符构造
  （`volcengine_asr.h:486`）；Test Connection 结果还展示 Key 前 8 字符
  （`:1125-1126`）；两者均无必要。配合 P2-7 的真 UUID，uid 应改为真 GUID 或
  持久化随机安装 ID，并去掉结果里的 Key 预览。

建议并入「公共 JSON 层」切片：Extra Params / hotwords 的修正与该重构天然同处。

### 6. 百度 / 火山缺离线协议回归测试（第五轮复核新增）

`tests/` 已有离线测试基建（`audio_diagnostics_test.cpp`、`llm_refine_test.cpp`、
`qwen_audio_json_test.cpp`、`qwen_free_protocol_test.cpp`），百度与火山无对应覆盖。
对照本报告各修法，建议新增（纯离线、无网络）：JSON 转义解码（含代理对）、
百度 `result[0]` 数组解析、数字型 `expires_in`、火山分片重组（64 片上限 + 出错丢弃）、
`MSG_ERROR_RESP` 错误帧、Extra Params 拆分、token 缓存凭据指纹失效。
原则：**先写测试锁定行为、再改实现**，与对应修复同切片完成。

---

## 无需改动（已核实，避免误改）

### `MSG_ERROR_RESP` 解析 —— 已对照官方文档定案（第四轮复核）

`volcengine_asr.h:272-292` 把 payload-size 位置当作错误码。第四轮复核对照了火山官方
协议文档（volcengine.com/docs/3019/1354869，"Error message from server"），错误帧布局为：

> Header | Error message code (4B) | Error message size (4B) | Error message (UTF8)

即 payload-size 位置确实被复用为错误码，`errorCode = payloadSize`（`:273`）与其后 4 字节
`errMsgSize`（`:279-283`）与官方布局完全一致 —— **现解析正确，无需改动**。
P1-3 的 Test Connection 修复仍值得做，但目的简化为"验证错误配置能被前置发现"。

- `main.cpp:1642/1675/1712/1748` 未检查 `CreateXxxStreamingSession()` 的返回值
  —— 四个工厂全部是 `return std::make_unique<...>(...)`，不会返回 nullptr
  （失败会抛异常）。不必加空检查。
- `wasapi_capture.cpp:373` `m_resamplePhase -= written / m_resampleRatio` 正确
  （符合 AGENTS.md 踩坑记录）；`:320` 的 `idx + 1 >= numFrames` 边界正确。
- `g_streamingVadTrimmer` 的生命周期是安全的：回调访问它的前提是
  `g_activeStreamingSession` 非空且整段在 `g_streamingSessionCs` 内
  （`wasapi_capture.cpp:339-351`），而 `main.cpp:1633` 的
  `TakeActiveStreamingSession()` 必须获取同一把锁，故 1634 的 reset 不会与回调重叠；
  且 `WasapiCapture::Stop()`（`:171`）join 了采集线程，UI 侧收尾同样安全。
- `ReplayPreCapturedAudio()` 与采集回调不会并发进入同一 VAD trimmer
  （回放发生在 session 安装之前，回调因 session 为 null 跳过整个 VAD 分支）。
- `s_volcSession.forceAbort` 的生命周期是**闭环**的：
  `VolcengineResetForNewSession()`（`:833-836`，清 `forceAbort` + `lastError`）
  在每次新录音前复位，调用点为 `main.cpp:1745`（volcengine 路径）与
  `main.cpp:429`（one-shot / fallback 路径）。
  且预热路径 `PrewarmConnection()`（`:943-955`）→ `EnsureConnection()`（`:353-386`）
  **不经过** `forceAbort` 检查，因此上一次 `Abort()` 的残留不会污染预热连接。

以下实现质量良好，不要在重构中破坏：

- **`asr_streaming_session_base.h` / `cloud_asr_common.h`**：已把回调、replay buffer、
  超时决策收敛成公共层 —— 就是 AGENTS.md 要求的正确形态；P3-2 只做增量上移，不推倒重来。
- **`hud.cpp`**：D2D 资源是惰性创建 + `EndDraw` 失败时全量重建（`:347` / `:462-469`），
  没有 per-frame 资源泄漏问题。
- **`cloud_http_common.cpp`**：`ScopedWinHttpHandle` RAII、16 MiB 响应体上限、
  `CloudHttpCancellation`、四段超时、重试分类 —— 全局最干净的一层，百度与 MiMo 都复用它。
- **`mimo_asr.cpp`**：完整 JSON 反转义（含代理对）、WAV 封装、base64、
  `WinHttpCrackUrl` 端点解析、自适应超时。是其他后端的模板。
- **`doubao_ime_streaming_session.cpp`**：几乎每个循环都查 `abort_.load()`，取消语义最完整。
- **`ActivateQwenFreeStreamingSession()`**（`main.cpp:1475-1495`）：回放+安装的双锁写法正确。
- **`WasapiCapture::Stop()`**（`:164-173`）：先置 generation、再 join 采集线程、
  最后 `Stop()` 设备，顺序正确。
- **配置三处同步完整**：`globals.h` / `LoadConfig` / `SaveConfig` 共 86 个 key，
  脚本双向比对无遗漏、无拼写错位。版本号 `resource.h` 与 README 一致（v0.9.25）。
- WinHTTP 句柄 open/close 粗查计数配平，未见明显泄漏（是粗查，非证明）。

---

## 建议的修复顺序

按「危害 × 改动面 / 回归风险」综合排序，每项独立可验证。第五轮复核后的最终顺序：

1. **百度四项（P1-4 凭据指纹 + P1-6 数字 expires_in + P1-5 自适应超时 + P2-10 的 60 s 上限）—— 热身切片**：集中在 `baidu_asr.h`（凭据指纹比对可放 `engine.cpp`），
   合计十行内、无网络语义改动，适合先建立改动节奏。
2. **P1-1**（火山重放取消）—— 危害最大（UI 可冻结至 27.5~117.5 s，见上界修正），
   **独立设计时间块**：abort_ 三检查点 + 独立句柄槽沿用现状务实语义，
   不以"文档化"自我背书（见 P2-3 修正）；异步 WinHTTP / 生命周期重设计另立后续议题。
3. **P2-2 + P3-1**（回放+安装原子化 + 启动块去重，合并切片）——
   直接影响识别质量（开口丢音），故提前到 JSON 层之前；回归面仍最大，
   需专门验证"开头第一个字不被吃"，保持切片隔离、独立验证窗口。
4. **公共 JSON 层 + 请求构造安全（P1-2 + P3-5 + P3-6 三合一）**：
   统一 `ExtractJsonStringDecoded`（含代理对）、百度 `result[0]`、火山 `result.text`、
   Extra Params、hotwords 转义；**先补百度/火山离线协议测试锁定行为，再改实现**。
   做完这一步四个后端的文本处理才算真正一致。
5. **数据竞争专项（P2-8 + P2-9 + P2-12）**：`g_captureActive` 原子化、锁外读 session 修正、
   跨线程指标、Settings 测试 generation 统一——同属并发安全主题，集中一次验证。
6. **P1-3 + P2-1 + NULL 防御**（均定级 P2）—— 一行级小改动合并切片：
   Test Connection 复用生产 `OpenSession` 临时会话；单实例互斥量前移 + `if (!mutex)`。
7. **P3-2 诊断三件套上移 + 其余 P3 清理**（UUID 真 GUID、分片重组、日志隐私清扫、P3-3）。

**零成本随手项**：P2-7 注释、P3-3 一行清理。
**明确不做**：P3-4（大文件拆分），等功能修复全部落地后再议。

**明确不动 / 保持现状**：P2-5（保活是有意的）；P2-3 与 P2-4 的现状保留
（务实方案，非文档化安全；长期异步化另议，不随本次排期改动）；
原「待核实」的 `MSG_ERROR_RESP` 已定案为**无需改动**（见「无需改动」节）。
