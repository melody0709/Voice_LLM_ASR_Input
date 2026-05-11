# 代码审查报告

> 日期: 2026-05-09 | 审查范围: src/ 全 17 个源文件 + build.bat
> 修复日期: 2026-05-09 | 已修复: 13 项 | 跳过: 1 项 (D4)

### 状态图例

| 图标 | 含义 |
|------|------|
| ✅ | 已修复 |
| ⏳ | 待讨论/后续推进 |
| ➖ | 暂不处理（当前无影响） |

---

## 🔴 BUG — 需要修复

### ✅ B1. 火山引擎线程在退出时不会被停止

**严重程度**: 中 | **文件**: [src/main.cpp](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp)

**问题描述**:

当用户在火山引擎录音过程中退出程序时：

1. `WM_DESTROY` (L513-519) 设置了 `g_captureActive = false`，但**没有设置 `g_volcStreaming = false`**
2. 火山引擎线程的 while 循环 (L302-326) 检查 `g_volcStreaming`（仍是 `true`），永远不会退出
3. 音频回调已停止，`g_volcPendingAudio` 始终为空 → 线程进入 `Sleep(20)` 死循环
4. `PostQuitMessage(0)` 后消息循环退出，但线程仍在运行
5. 进程退出时由 OS 强制终止线程（不干净，但不会崩溃）

**涉及代码**:

- [main.cpp:513-519](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L513-L519) — `WM_DESTROY` 处理
- [main.cpp:302-326](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L302-L326) — volcano 线程 while 循环

**修复建议**:

```cpp
// WM_DESTROY 中插入 (在 UninstallKeyboardHook 之前):
g_volcStreaming = false;
if (g_volcThread.joinable()) g_volcThread.join();
```

---

### ✅ B2. 百度 ASR Token 缓存存在多线程数据竞争

**严重程度**: 高 | **文件**: [src/baidu_asr.h](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/baidu_asr.h)

**问题描述**:

`GetAccessToken` (L148-189) 使用 `static` 变量缓存 token，没有任何同步：

```cpp
static std::wstring s_cachedToken;      // L149
static ULONGLONG     s_tokenExpiresAt = 0; // L150
```

调用方：`Recognize` (后台线程) 和 `TestConnection` (后台线程，从 settings UI 触发) 都可能并发调用 `GetAccessToken`。

存在对 `s_cachedToken`（`std::wstring`）的并发读写 —— 这是未定义行为，可能导致崩溃或 token 损坏。

**涉及代码**:

- [baidu_asr.h:148-189](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/baidu_asr.h#L148-L189) — `GetAccessToken`

**修复建议**:

添加全局 `std::mutex` 保护 token 缓存的读写段，或使用 `std::call_once` + `std::atomic`:

```cpp
static std::mutex s_tokenMutex;
// ... lock_guard<std::mutex> lk(s_tokenMutex); 包裹缓存读写
```

---

### ✅ B3. `VolcDebugLog` 日志文件多线程不安全

**严重程度**: 低 | **文件**: [src/volcengine_asr.h](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/volcengine_asr.h)

**问题描述**:

1. `static char logPath[MAX_PATH] = {}` 初始化存在竞争（多个线程可能同时检测到 `logPath[0] == '\0'` 并写入）
2. 多线程同时 `fopen_s`/`fprintf`/`fclose` 同一文件，日志行会交错

当前仅在 `VOLC_DEBUG_LOG == 1` 时编译（L5 默认定义），理论上每个 release build 也会编译进去。

**涉及代码**:

- [volcengine_asr.h:21-39](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/volcengine_asr.h#L21-L39) — `VolcDebugLog`

**修复建议**:

加 `static CRITICAL_SECTION` 或 `std::mutex` 保护整个函数体。

---

## 🟡 设计/代码质量问题

### ✅ D1. `g_volcAudioCs` 未在退出时删除（资源泄漏）

**严重程度**: 低 | **文件**: [src/main.cpp](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp)

`InitializeCriticalSection(&g_volcAudioCs)` 在 [L565](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L565) 调用，但 `DeleteCriticalSection(&g_volcAudioCs)` 从未被调用。对比 `g_audioLock` 在 [L586](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L586) 初始化、[L636](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L636) 删除 —— `g_volcAudioCs` 遗漏了。

**修复**: 在 `wWinMain` 返回前（L636）和单实例早退路径（L591）两处都添加 `DeleteCriticalSection(&g_volcAudioCs)`。

---

### ✅ D2. SSL 证书验证被全局禁用（安全隐患）

**严重程度**: 中 | **文件**: [src/llm_refine.h](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/llm_refine.h) / [src/baidu_asr.h](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/baidu_asr.h)

LLM 纠错和百度 ASR 的 HTTPS 连接设置了（共 4 个 flag）：

```cpp
SECURITY_FLAG_IGNORE_UNKNOWN_CA |
SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE
```

完全关闭证书验证，连接易受中间人攻击。火山引擎模块反而没有这个问题（使用系统默认验证 `WINHTTP_FLAG_SECURE`）。

**涉及代码**:

- [llm_refine.h:298-303](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/llm_refine.h#L298-L303)
- [baidu_asr.h:105-110](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/baidu_asr.h#L105-L110)

**修复建议**: 至少对正式 API 服务保留系统默认证书验证。若确实需要（如内网自签名），做成可配置选项。

---

### ✅ D3. `WideToUtf8` / `Utf8ToWide` / `EscapeJson` / `Trim` 重复定义

**严重程度**: 低 | **影响**: 维护成本

4 个文件中各自实现了一份相同的工具函数：

| 文件 | 函数 |
|------|------|
| [engine.cpp:22-66](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/engine.cpp#L22-L66) | `WideToUtf8`, `Utf8ToWide`, `EscapeJson`, `Trim` |
| [llm_refine.h:70-108](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/llm_refine.h#L70-L108) | `WideToUtf8`, `Utf8ToWide`, `EscapeJson`, `Trim` |
| [baidu_asr.h:24-38](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/baidu_asr.h#L24-L38) | `WideToUtf8`, `Utf8ToWide` |
| [volcengine_asr.h:126-142](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/volcengine_asr.h#L126-L142) | `WideToUtf8`, `Utf8ToWide` |

共 4 份实现，修改一处需要同步多处。

**修复建议**: 统一到 `engine.h` 或新建 `src/utils.h`，其他文件 include。

---

### ➖ D4. `g_volcKeepAlive` 设置后永不重置

**严重程度**: 低 | **文件**: [src/main.cpp:269](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L269)

首次火山引擎连接成功后 `g_volcKeepAlive = true`，之后永不回 `false`。

- `CloseSession` 依据此标志决定是否关闭句柄
- `ClosePersistentConnection`（退出时调用）直接关闭句柄，绕过此标志
- 当前行为正确（连接复用 + 退出时清理）

但如果未来想支持用户动态关闭 KeepAlive，需要能重置它。

**修复建议**: 当前无功能影响，暂可忽略。未来若要支持动态开关，需添加重置逻辑。

---

### ✅ D5. `AsrEngine::lock` 是 public 成员（封装不当）

**严重程度**: 低 | **文件**: [src/engine.h:57](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/engine.h#L57)

`std::mutex lock` 声明为 public，`PreloadAsrEngine` 在 [engine.cpp:765](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/engine.cpp#L765) 直接 `g_asrEngine.lock.lock()`。

**修复建议**: 改为 private + `Lock()`/`Unlock()` 方法，或将 `PreloadAsrEngine` 声明为 friend。

---

## 🟢 小优化建议

### ✅ O1. 托盘菜单冗余 flag

**文件**: [src/main.cpp:408](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp#L408)

```cpp
AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, ID_TRAY_VERSION, APP_VERSION_WSTR);
```

`MF_GRAYED` 已隐含 disabled 状态，`MF_DISABLED` 是冗余的。

---

### ✅ O2. `HotkeyEditWndProc::WM_PAINT` 每次创建/销毁 brush

**文件**: [src/hotkey.cpp:289](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/hotkey.cpp#L289)

```cpp
HBRUSH bg = CreateSolidBrush(RGB(255, 255, 255));
FillRect(hdc, &rc, bg);
DeleteObject(bg);
```

每次 `WM_PAINT` 都创建和销毁 brush，建议用静态 `HBRUSH`（`COLOR_WINDOW` 对齐系统主题即可）。

---

### ✅ O3. PositionHud 每次创建新 region

**文件**: [src/hud.cpp:105](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/hud.cpp#L105)

`CreateRoundRectRgn` 失败时未释放 region；且每次 `PositionHud` 都创建新 region，旧 region 被 `SetWindowRgn` 接管（由系统管理，不会泄漏）。但若频繁调用可能有性能损耗。当前 `PositionHud` 调用频率低（窗口创建、DPI/文字变化），实际影响可忽略。

**修复**: 使用静态变量缓存窗口尺寸，仅在尺寸变化时重新创建 region 并调用 `SetWindowRgn`（系统接管所有权），避免重复创建和 GDI 句柄泄漏。注意不缓存 region 句柄本身——`SetWindowRgn` 成功后系统会接管该句柄，后续不得复用。

---

### ✅ O4. `RunModelDownloader` 用 `WaitForSingleObject(INFINITE)` 阻塞 UI 线程

**文件**: [src/engine.cpp:162](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/engine.cpp#L162)

```cpp
WaitForSingleObject(pi.hProcess, INFINITE);
```

调用时机：`wWinMain`启动后、消息循环前。在此期间窗口未显示，所以**当前无明显不良影响**。但若未来提前启动下载器，则会冻 UI。

**修复建议**: 改为异步调用，通过 `WM_USER` 消息回传结果。

---

## 📊 问题汇总

| # | 状态 | 标题 | 严重程度 | 文件 |
|---|------|------|----------|------|
| B1 | ✅ | 退出时火山线程未被停止 | 中 | main.cpp |
| B2 | ✅ | 百度 ASR Token 缓存数据竞争 | 高 | baidu_asr.h |
| B3 | ✅ | VolcDebugLog 多线程不安全 | 低 | volcengine_asr.h |
| D1 | ✅ | g_volcAudioCs 未删除 | 低 | main.cpp |
| D2 | ✅ | SSL 证书验证被禁用 | 中 | llm_refine.h / baidu_asr.h |
| D3 | ✅ | 工具函数重复定义 | 低 | 新建 utils.h |
| D4 | ➖ | g_volcKeepAlive 永不重置 | 低 | main.cpp / volcengine_asr.h |
| D5 | ✅ | AsrEngine::lock public | 低 | engine.h |
| O1 | ✅ | 托盘菜单冗余 flag | - | main.cpp:408 |
| O2 | ✅ | HotkeyEdit PAINT 频繁创建 brush | - | hotkey.cpp:289 |
| O3 | ✅ | PositionHud region | - | hud.cpp:105 |
| O4 | ✅ | RunModelDownloader 阻塞 UI | - | engine.cpp:162 |
| N1 | ✅ | WM_APP+20 handler modelId 不一致 | 低 | settings.cpp / engine.cpp |
| N2 | ✅ | firered_vad.h 路径转换非 UTF-8 | 低 | firered_vad.h |

### 已修复 (13项)

B1, B2, B3, D1, D2, D3, D5, O1, O2, O3, O4, N1, N2

### 跳过 (1项)

- **➖ D4** — g_volcKeepAlive 永不重置：当前行为正确，无需改动

---

## 🔴 第二次审查 — 新发现问题

> 日期: 2026-05-09 | 修复状态: ✅ 已修复

### ✅ N1. WM_APP + 20 handler 读 `g_config.modelId` 而非线程捕获的值

**严重程度**: 低 | **文件**: [src/settings.cpp](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/settings.cpp)

**问题描述**:

[engine.cpp:122](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/engine.cpp#L122) 下载线程捕获了启动时的 `modelId`，但 [settings.cpp:1635](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/settings.cpp#L1635) handler 读的是 `g_config.modelId`：

```cpp
std::wstring newDir = DefaultModelDir(g_config.modelId);  // 可能不等于下载时的 modelId
```

如果用户在下载期间改了模型并通过 Save 按钮保存（按钮未禁用），handler 会用新的 modelId 生成目录路径，与实际下载的目录不一致。

**修复**: 下载线程直接计算 `modelDir` 并通过 `lParam` 传递到 handler，handler 用 `unique_ptr` 接管。

---

### ✅ N2. firered_vad.h 路径转换非 UTF-8 安全

**严重程度**: 低 | **文件**: [src/firered_vad.h](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/firered_vad.h)

**问题描述**:

[firered_vad.h:115](file:///d:/%23GITHUB_melody0709/Voice_LLM_ASR_Input/src/firered_vad.h#L115) 将 `std::string` 转 `std::wstring` 使用逐字节拷贝：

```cpp
std::wstring wpath(cfg.modelPath.begin(), cfg.modelPath.end());
```

这仅对纯 ASCII 路径安全。若模型路径含中文（如 `C:\用户\models\...`），每个 UTF-8 多字节字符会被错误地拆成多个 wchar_t，产生乱码。

**修复**: 改为 `Utf8ToWide(cfg.modelPath)`，使用 `utils.h` 中已统一的函数。

---

## 🛡️ 不在审查范围内（已知/已讨论）

- `AsrEngine` 模型加载机制和缓存逻辑 —— 已在 `ARCHITECTURE.md` 中充分说明
- HUD 高 DPI 换算策略 —— 已在 `AGENTS.md` 中有详细踩坑规则
- CapsLock 短按补发逻辑 —— 已有明确踩坑规则
- sherpa-onnx API 稳定性 —— 第三方库，非本项目范围
- 模型文件管理 —— 不在 git 中
- 火山引擎 token 过期重试 —— 已有重试机制 (`RunOnVolcThread`)
- build.bat VS 2022 路径硬编码 —— 已知限制，非本次重点