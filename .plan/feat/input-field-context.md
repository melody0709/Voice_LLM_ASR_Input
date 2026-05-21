# 输入框上下文获取方案

> 目标：读取当前光标所在输入框的已有文本，作为 ASR 上下文发送给服务端，
> 提升识别准确率。特别是用户手动修改了识别错误后，下次识别应利用修改后的文本。

---

## 1. 核心需求

1. **读取输入框已有文本** → 作为 ASR 上下文
2. **利用用户手动修改** → 输入框文本作为最高权重上下文，自然覆盖原始识别结果
3. **非侵入性** → 不影响用户操作，不闪烁，不破坏剪贴板
4. **性能** → <100ms，不阻塞录音启动

---

## 2. 当前 VoxType 上下文机制

`main.cpp` 用 `g_volcRecognitionHistory`（历史识别结果）构建
火山引擎 `corpus.context`。上下文来源由两个独立开关控制：

```
BuildVolcContextJson(inputFieldText, includeHistory):
  {"context_type":"dialog_ctx","context_data":[
    {"text":"输入框末尾200字"}   ← inputFieldText 非空时
    或
    {"text":"上次识别结果1"},    ← includeHistory=true 且 inputFieldText 为空时
    {"text":"上次识别结果2"}
  ]}
```

火山引擎 context 限制：800 tokens，20 轮以内。
窗口标题不再作为 context 发送（仅用于 debug 诊断）。

已有 UI 控件：
- `IDC_VOLC_ENABLE_CONTEXT`（"Use history as context" 复选框）
- `IDC_VOLC_CONTEXT_HISTORY`（history 轮数输入框）
- `IDC_VOLC_ENABLE_INPUT_CONTEXT`（"Read input field context" 复选框）

---

## 3. 方案对比

| 方案 | 兼容性 | 性能 | 侵入性 | 实现难度 | 推荐度 |
|------|--------|------|--------|----------|--------|
| **窗口标题/进程名** | 极高 | <1ms | 无 | 极低 | ★★★★★ |
| **UIA Value 属性** | 高 | 10-50ms | 无 | 中 | ★★★★★ |
| **UIA TextPattern v1** | 中高 | 20-50ms | 无 | 中 | ★★★★ |
| **WM_GETTEXT** | 低 | <5ms | 无 | 低 | ★★★ |
| **MSAA (IAccessible)** | 中 | 10-30ms | 无 | 低 | ★★★ |
| **UIA TextPattern2** | 中 | 20-80ms | 无 | 中 | ★★★ |
| TSF | 极低 | N/A | 无 | 极高 | ★ |
| 剪贴板方法 | 高 | 100-300ms | 极高 | 中 | ★ |

> **窗口标题/进程名**：不需要 UIA/COM，纯 Win32 API 即可获取前台窗口标题和进程名。
> 成本极低，但窗口标题不再作为 context 发送（对 ASR 识别帮助极小），
> 仅用于 debug 诊断。作为 Layer 0 优先获取。
>
> **UIA TextPattern v1**：比 TextPattern2 的 `GetCaretRange` 兼容性更好，
> Chrome/Word/VS Code 都支持。实际验证发现 Word 中 `GetVisibleRanges` 是关键路径，
> `RangeFromPoint` 在 Word Document 控件上可能失败。
>
> **MSAA (IAccessible)**：某些老应用/Win32 自定义控件不支持 UIA 但支持 MSAA 的
> `get_accValue`。实现成本低（COM 接口，不需要额外初始化），作为 UIA 失败后的
> 补充路径。

---

## 4. 实际实现：分层 Fallback

```
录音开始时，获取输入框上下文（UI 线程 + 独立工作线程）：

Layer 0: 窗口标题（零成本，仅用于 debug 诊断）
  GetForegroundWindow() → GetWindowTextW() → 窗口标题
  性能: <1ms
  适用: 所有应用
  注意: 在 UI 线程同步获取，始终可用
        窗口标题不再作为 context 发送（对 ASR 识别帮助极小）
        但保存在 InputContextResult.windowTitle 中，用于 debug 诊断

Layer 1: WM_GETTEXT（快速路径，仅 Edit 控件）
  GetGUIThreadInfo(fgThreadId, &guiInfo) → guiInfo.hwndFocus
  GetClassNameW(hwndFocus) → 仅对 "Edit" 类控件执行
  SendMessageTimeoutW(hwndFocus, WM_GETTEXT, 4095, ..., 500ms)
  截取最后 200 字符
  性能: <5ms
  适用: 记事本、Win32 Edit 控件
  注意: ⚠️ 只对 "Edit" 类控件使用 WM_GETTEXT
        Word/Excel 等应用的焦点 HWND 是顶层窗口，WM_GETTEXT 返回窗口标题
        会导致误判（§13.10），所以必须检查窗口类名

Layer 2: UIA GetFocusedElement → TryReadFromElement（4 种方法依次尝试）
  ⚠️ 前置步骤：EnsureAccessibilityTree(fgHwnd)
     Chrome/Edge/Electron 默认不构建完整 UIA 树，需先发送
     WM_GETOBJECT(0, 0xFFFFFFFC) 触发懒构建。
     用 std::set<HWND> 记录已触发的窗口避免重复发送。

  pAutomation->GetFocusedElement(&pFocused)

  TryReadFromElement 依次尝试：
    2a. TryGetValueText           → UIA Value 属性
        pElement->GetCurrentPropertyValue(UIA_ValueValuePropertyId)
        → 拿到输入框全文，截取最后 200 字符
        适用: Chrome/Edge 输入框、WPF TextBox、大多数标准控件

    2b. TryTextPatternFromPoint    → TextPattern v1 RangeFromPoint
        pElement->GetCurrentPatternAs(UIA_TextPatternId)
        pTextPattern->RangeFromPoint(caretPoint)
        pRange->Move(TextUnit_Character, -100, &moved)
        pRange->GetText(200, &text)
        适用: Chrome/VS Code（光标位置感知）
        注意: Word Document 控件上 RangeFromPoint 可能失败

    2c. TryTextPatternVisibleRanges → TextPattern v1 GetVisibleRanges（Word 关键路径）
        pTextPattern->GetVisibleRanges(&pRanges)
        对每个 range 调用 GetText(500) 拼接，累积超过 400 字符时提前退出
        截取最后 200 字符
        适用: Word Document 控件（Value 为空，RangeFromPoint 也可能失败）
        注意: 用 GetText(500) 代替 GetText(-1) 避免大视口下读取过多文本

    2d. TryTextPattern2Caret       → TextPattern2 GetCaretRange
        pElement->GetCurrentPatternAs(UIA_TextPattern2Id)
        pTP2->GetCaretRange(&isActive, &pCaretRange)
        pCaretRange->Move(TextUnit_Character, -100, &moved)
        pCaretRange->GetText(200, &text)
        适用: WPF TextBox、RichEdit（兼容性有限）

  如果 TryReadFromElement 全部失败 → TryWalkParentsForText
    用 ControlViewWalker 向上遍历父元素（最多 8 层，到 Window 停止）
    对每个父元素调用 TryReadFromElement
    适用: 焦点落在子元素但文本在父元素上的场景

Layer 2.5: UIA ElementFromPoint → TryReadFromElement（同上 4 种方法 + 向父冒泡）
  当 GetFocusedElement 失败或返回非文本元素时，用坐标定位
  坐标来源：GetGUIThreadInfo.rcCaret（光标位置）
  无光标时：窗口中心偏下 2/3 处
  pAutomation->ElementFromPoint(uiaPt, &pFromPt)
  → 同 Layer 2 的 TryReadFromElement + TryWalkParentsForText

Layer 2.7: MSAA (IAccessible) get_accValue
  AccessibleObjectFromWindow(hwndFocus, OBJID_WINDOW, IID_IAccessible, &pAcc)
  VARIANT self; self.vt = VT_I4; self.lVal = CHILDID_SELF;
  pAcc->get_accValue(self, &value)
  截取最后 200 字符
  性能: 10-30ms
  适用: 不支持 UIA 但支持 MSAA 的老应用/Win32 自定义控件

Layer 4: 放弃
  微信、Java、游戏等不支持 UIA/MSAA 的应用
  → 如果开了 "Use history as context"，用历史识别结果兜底
  → 否则不发送 context
```

### 实际检测流程图

```
GetInputFieldContext()  [UI 线程]
  │
  ├─ GetForegroundWindow()  → fgWnd (UI 线程保存，传入工作线程)
  ├─ GetForegroundWindowTitle()  → windowTitle (Layer 0)
  │
  └─ UIA 工作线程 (200ms 超时, MTA, 接收 fgWnd)
      │
      ├─ GetGUIThreadInfo → hwndFocus, caretScreenPt
      │
      ├─ Layer 1: WM_GETTEXT (仅 Edit 类控件)
      │   → 成功: return
      │
      ├─ EnsureAccessibilityTree(fgWnd)
      │
      ├─ Layer 2: GetFocusedElement
      │   ├─ TryGetValueText           → 成功: return
      │   ├─ TryTextPatternFromPoint    → 成功: return
      │   ├─ TryTextPatternVisibleRanges → 成功: return
      │   ├─ TryTextPattern2Caret       → 成功: return
      │   └─ TryWalkParentsForText      → 成功: return
      │
      ├─ Layer 2.5: ElementFromPoint
      │   ├─ TryGetValueText           → 成功: return
      │   ├─ TryTextPatternFromPoint    → 成功: return
      │   ├─ TryTextPatternVisibleRanges → 成功: return
      │   ├─ TryTextPattern2Caret       → 成功: return
      │   └─ TryWalkParentsForText      → 成功: return
      │
      ├─ Layer 2.7: MSAA get_accValue
      │   → 成功: return
      │
      └─ 全部失败: failReason = "UIA_EMPTY"
```

---

## 5. 各应用兼容性（实测结果）

| 应用 | 窗口标题 | WM_GETTEXT | UIA Value | TextPattern | MSAA | 实测成功 Layer | 备注 |
|------|---------|-----------|-----------|-------------|------|---------------|------|
| 记事本 | ✅ | ✅ | ✅ | — | ✅ | L1_WM_GETTEXT | Edit 控件，WM_GETTEXT 快速路径 |
| Word | ✅ | ❌ | ❌ | ✅ (VisibleRanges) | ❌ | L2.5_TEXTPATTERN | Value 为空，RangeFromPoint 也可能失败，GetVisibleRanges 成功 |
| Chrome 输入框 | ✅ | ❌ | ✅ | ✅ | — | L2_UIA_VALUE | 需 EnsureAccessibilityTree 触发 |
| Edge 输入框 | ✅ | ❌ | ✅ | ✅ | — | L2_UIA_VALUE | 同 Chrome |
| VS Code | ✅ | ❌ | ✅ | ✅ | — | L2_UIA_VALUE | Electron，编辑器为 Document |
| Excel | ✅ | ❌ | ✅ | ❌ | ❌ | — | 单元格编辑模式，待测 |
| Firefox | ✅ | ❌ | ✅ | 部分 | ❌ | — | 独立 UIA 实现，待测 |
| 微信 | ✅ | ❌ | ❌ | ❌ | ❌ | L0_TITLE | Qt 自绘控件，UIA/MSAA 不可见 |
| QQ | ✅ | ❌ | ❌ | ❌ | ❌ | L0_TITLE | 同微信 |
| 钉钉 | ✅ | ❌ | 部分 | 部分 | ❌ | — | Electron 壳，待测 |
| 飞书 | ✅ | ❌ | 部分 | 部分 | ❌ | — | Electron 壳，待测 |

**微信/QQ 是最大盲区**——VoxType 已有微信粘贴适配（WM_CHAR 逐字符发送），
但读取微信输入框文本目前无解。AriaType 在 macOS 上用 Accessibility API
可以读取，但 Windows 上同样无法读取微信。如果开了 "Use history as context"，
历史识别结果仍能作为兜底上下文。

---

## 6. UIA 实现要点

### 6.1 COM 初始化

UIA 需要 COM 环境。VoxType 当前 COM 初始化情况：
- `wasapi_capture.cpp`：`CoInitializeEx(NULL, COINIT_MULTITHREADED)` — 在 WASAPI 线程
- UI 线程：未初始化 COM
- Volc 线程 / drainThread：未初始化 COM（只用 WinHTTP，不需要 COM）

**实际方案**：不在 UI 线程初始化 COM（避免 STA 消息泵依赖），而是在 UIA 工作线程中
初始化 `COINIT_MULTITHREADED`。每个 UIA 工作线程独立初始化/反初始化 COM。

```cpp
HWND fgWnd = GetForegroundWindow();  // UI 线程保存
std::wstring wt = result.windowTitle;
std::thread worker([promise, wt, fgWnd]() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    InputContextResult r = ReadInputFieldTextUIA(wt, fgWnd);
    CoUninitialize();
    promise->set_value(std::move(r));
});
worker.detach();
```

**注意**：`GetForegroundWindow()` 在 UI 线程调用，结果传入工作线程，
避免工作线程启动时前台窗口已切换。

### 6.2 延迟初始化

每次 UIA 调用都创建新的 `IUIAutomation` 实例，不缓存：
- UIA 工作线程是 detached 的，生命周期不确定
- `IUIAutomation` 是 COM 对象，需要在线程退出前 Release
- 每次创建的开销很小（<1ms），不值得缓存

### 6.3 超时保护

UIA 跨进程调用可能挂起（目标进程无响应时）。策略：
- 用 `std::thread` + `std::promise` + `std::future` 超时
- 超时后放弃，不影响录音
- **不在 UI 线程直接调用 UIA**，防止 UIA 挂起导致 HUD 不更新

**⚠️ 不能用 `std::async`**：`std::async(std::launch::async, ...)` 返回的 `std::future`
在析构时会阻塞等待线程完成（C++ 标准规定），导致超时保护完全无效。

**⚠️ 不能用 `std::async`**：`std::async(std::launch::async, ...)` 返回的 `std::future`
在析构时会阻塞等待线程完成（C++ 标准规定），导致超时保护完全无效。

**防重入**：用 `std::atomic<bool> s_uiaThreadRunning` 标记，如果上次 UIA 线程还在运行，跳过本次读取。

### 6.4 焦点元素过滤

当前实现**不过滤 ControlType**，对所有焦点元素尝试全部 4 种读取方法。
原因：Word 的 Document 控件 Value 为空但 TextPattern 可用，
如果过滤掉非 Edit/Document 类型，可能错过可读取的元素。

### 6.5 密码框保护

在 `TryGetValueText` 开头检查 `UIA_IsPasswordPropertyId`，
如果是密码框则设置 `result.isPassword = true` 并返回 false。
后续 TextPattern 等方法不检查密码（TextPattern 通常不暴露密码内容）。

### 6.6 Chrome 无障碍树触发（关键前置步骤）

**问题**：Chrome/Edge/Electron 出于性能考虑，默认不构建完整 UIA 树。
只有收到 `WM_GETOBJECT(0, OBJID_CLIENT)` 或检测到屏幕阅读器时才懒构建。

**实现**：
```cpp
static std::set<HWND> s_triggeredWindows;

inline void EnsureAccessibilityTree(HWND hwnd) {
    if (!hwnd) return;
    if (s_triggeredWindows.count(hwnd)) return;
    SendMessageW(hwnd, WM_GETOBJECT, 0, (LPARAM)0xFFFFFFFC);
    s_triggeredWindows.insert(hwnd);
}
```

**注意**：`s_triggeredWindows` 用 `inline` 声明（C++17），避免 header-only 中多 TU 问题。
只增不减，HWND 可能被系统复用。当前不清理，因为影响极小。

### 6.7 TextPattern v1 的两种读取方式

#### RangeFromPoint（光标位置感知）

从光标位置向前 100 字符 + 向后 200 字符，适合光标在文本中间的场景。
需要屏幕坐标（从 `GetGUIThreadInfo.rcCaret` 获取）。

```cpp
pTextPattern->RangeFromPoint(caretPoint, &pRange);
pRange->Move(TextUnit_Character, -100, &moved);  // 3 个参数！
pRange->GetText(200, &text);
```

**踩坑**：`Move` 需要 3 个参数（unit, count, &moved），不是 2 个。

#### GetVisibleRanges（Word 关键路径）

读取当前视口可见的所有文本范围。不需要光标坐标。
在 Word 中，Document 控件的 Value 为空、RangeFromPoint 也可能失败，
但 `GetVisibleRanges` 能成功返回文本。

```cpp
pTextPattern->GetVisibleRanges(&pRanges);
for each range: pRange->GetText(500, &text);  // 限制单次读取量
allText += text;
if (allText.size() >= 400) break;  // 提前退出
result.inputFieldText = TakeLastN(allText, 200);  // 截取最后 200 字符
```

**注意**：用 `GetText(500)` 代替 `GetText(-1)`，并在累积超过 400 字符时提前退出，
避免大视口（如 Word 缩放 25%）下读取过多文本。

### 6.8 MSAA (IAccessible) get_accValue

当 UIA 全部失败时，尝试 MSAA 的 `get_accValue`。
只尝试 `CHILDID_SELF`（元素自身），不递归遍历子元素。

```cpp
AccessibleObjectFromWindow(hwndFocus, OBJID_WINDOW, IID_IAccessible, &pAcc);
VARIANT varSelf; varSelf.vt = VT_I4; varSelf.lVal = CHILDID_SELF;
pAcc->get_accValue(varSelf, &value);
```

需要链接 `oleacc.lib` 和 `oleaut32.lib`（通过 `#pragma comment(lib)` 声明）。

### 6.9 向父元素冒泡 (TryWalkParentsForText)

当焦点元素的 4 种方法都失败时，用 `ControlViewWalker` 向上遍历父元素
（最多 8 层，遇到 `Window` 类型停止），对每个父元素尝试 `TryReadFromElement`。

适用场景：焦点落在子元素（如 Word 中的行内元素），但文本在父 Document 控件上。

---

## 7. 上下文合并策略

### 核心思路

第一版**不做修改检测**，直接把输入框末尾文本作为 context 的唯一来源（优先），
history 作为兜底（输入框文本不可用时）。窗口标题不再作为 context 发送。

### 上下文来源与优先级

两个开关独立控制，输入框文本优先、历史记录兜底：

| 开关 | 控制的上下文 | 说明 |
|------|------------|------|
| Read input field context | 输入框文本 | 读取当前输入框末尾 200 字符 |
| Use history as context | 历史识别结果 | 发送历史识别结果（轮数可配置），仅在输入框文本不可用时发送 |

**组合效果**：

| Read input field context | Use history as context | 发送的 context_data | 说明 |
|:---:|:---:|------|------|
| ✅ | ✅ | 输入框文本（优先）或历史识别结果（兜底） | 输入框有文本时只发输入框，避免重复；输入框为空时用历史兜底 |
| ✅ | ❌ | 输入框文本 | 只用输入框上下文 |
| ❌ | ✅ | 历史识别结果 | 只用历史兜底 |
| ❌ | ❌ | 不发送 context | 完全关闭 |

**为什么输入框文本和历史记录不同时发送**：
1. 输入框文本已包含历史识别内容（且可能被用户修改更准确），再发历史记录是重复
2. 火山引擎 context 限制 800 tokens，避免浪费在重复内容上
3. 输入框文本是用户确认过的最新版本，比历史记录更可靠

**为什么窗口标题不再作为 context 发送**：
- 火山引擎的 context 是对话上下文，期望的是用户说过的话，不是窗口标题
- "项目讨论 - 微信" 这类标题对 ASR 识别帮助极小
- 窗口标题仍保存在 `InputContextResult.windowTitle` 中，用于 debug 诊断

### BuildVolcContextJson 实现

```cpp
static std::wstring BuildVolcContextJson(const std::wstring& inputFieldText = L"",
                                          bool includeHistory = true) {
    std::wstring json = L"{\"context_type\":\"dialog_ctx\",\"context_data\":[";
    int idx = 0;

    if (!inputFieldText.empty()) {
        json += L"{\"text\":\"" + JsonEscape(inputFieldText) + L"\"}";
        idx++;
    }

    if (includeHistory) {
        for (size_t i = 0; i < g_volcRecognitionHistory.size(); ++i) {
            if (idx > 0) json += L",";
            json += L"{\"text\":\"" + JsonEscape(g_volcRecognitionHistory[i]) + L"\"}";
            idx++;
        }
    }
    json += L"]}";
    return json;
}
```

`includeHistory` 参数：输入框文本可用时传 `false`（避免重复），不可用时传 `true`（历史兜底）。

### JsonEscape（完整版）

Word 文档包含 `\f`（form feed）、`\v`（vertical tab）等控制字符，
如果不转义会导致火山引擎 JSON 解析失败。

```cpp
static std::wstring JsonEscape(const std::wstring& s) {
    std::wstring out;
    out.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'\\') out += L"\\\\";
        else if (c == L'"') out += L"\\\"";
        else if (c == L'\n') out += L"\\n";
        else if (c == L'\r') out += L"\\r";
        else if (c == L'\t') out += L"\\t";
        else if (c == L'\b') out += L"\\b";
        else if (c == L'\f') out += L"\\f";
        else if (c < 0x20) {
            wchar_t buf[8];
            swprintf_s(buf, L"\\u%04x", (unsigned)c);
            out += buf;
        }
        else out += c;
    }
    return out;
}
```

---

## 8. 与火山引擎 API 的集成

### context 字段格式

```json
{
  "request": {
    "corpus": {
      "context": "{\"context_type\":\"dialog_ctx\",\"context_data\":[{\"text\":\"修改后的上下文\"}]}"
    }
  }
}
```

`context` 字段值必须是 JSON 字符串（内部引号转义），不能是原始 JSON 对象。
VoxType 已有此处理逻辑（volcengine_asr.h）。

### 上下文长度

所有路径最终都经过 `TakeLastN(text, 200)`，发送给火山引擎的输入框上下文
**最多 200 个 wchar（约 200 个中文字/英文字符）**。

| Layer | 方法 | 取文字量 |
|-------|------|---------|
| L1 WM_GETTEXT | `SendMessageTimeoutW(WM_GETTEXT, 4095)` | 最多 4095 wchar → 截取最后 200 |
| L2 UIA Value | `GetCurrentPropertyValue(UIA_ValueValuePropertyId)` | 控件返回全部 → 截取最后 200 |
| L2.5 TextPattern FromPoint | `Move(-100) + GetText(200)` | 光标前 100 + 后 200 = 最多 300 → 再截取最后 200 |
| L2.5 TextPattern VisibleRanges | `GetVisibleRanges + GetText(500)` | 每个 range 最多 500，累积上限 400 → 截取最后 200 |
| L2.7 MSAA | `get_accValue` | 控件返回全部 → 截取最后 200 |

---

## 9. 隐私考虑

- UIA 读取不触发任何系统提示（Windows 11 没有 macOS 那样的无障碍权限弹窗）
- 应在 Settings 中说明此功能，并提供开关
- 不读取密码框（`UIA_IsPasswordPropertyId` 检查）
- 只取最后 200 字符，不读取完整文档

---

## 9.5 Debug 诊断方案

### 诊断信息结构

```cpp
struct InputContextResult {
    int successLayer = -1;       // -1=全部失败, 1=WM_GETTEXT,
                                 // 2=UIA Value, 3=TextPattern v1, 4=MSAA, 5=TextPattern2
    std::wstring windowTitle;    // Layer 0 结果
    std::wstring inputFieldText; // Layer 1-5 结果（截断后）
    double elapsedMs = 0.0;     // 总耗时
    bool timedOut = false;      // 是否超时
    std::string failReason;     // 失败原因（HRESULT、错误码等）
    int textLength = 0;         // 输入框原文长度（截断前）
    std::string controlType;    // UIA ControlType 名称（Edit/Document/Text）
    std::string focusWindowClass; // 焦点窗口类名（UTF-8）
    bool isPassword = false;    // 是否密码框
};
```

### Console 输出格式

在 `DebugPrintHeader` 之后、Pipeline 之前输出，顺序：Header → Context → Pipeline → OK

```
-- 20:36:45  Rec 1.6s(49KB) -- WASAPI 48kHz->16kHz (CABLE Output)
  Context: L2.5_TEXTPATTERN 10ms class=_WwG uia=Document len=49
  ContextText: "Do you love me? It's the best friend. Yes, OK."
  Pipeline: Volcengine 298 | Paste 6 = Total 304ms
  VAD trim: 1.6s/49KB -> 1.4s/43KB (87%)
  OK: "你喜不喜欢我？"
```

**格式说明**：

```
  Context: {Layer名} {耗时}ms class={窗口类名} uia={控件类型} len={原文长度}
  ContextText: "{完整文本}"      ← 仅在有文本时输出，用 WriteConsoleW 支持中文
  ContextFail: {失败原因}        ← 仅在全部失败时输出
```

各 Layer 的标签：

| Layer | 标签 | 说明 |
|-------|------|------|
| -1 | `NONE` | 全部失败，仅窗口标题 |
| 1 | `L1_WM_GETTEXT` | Win32 Edit 控件 |
| 2 | `L2_UIA_VALUE` | UIA Value 属性 |
| 3 | `L2.5_TEXTPATTERN` | TextPattern v1 (RangeFromPoint / VisibleRanges) |
| 4 | `L2.7_MSAA` | IAccessible |
| 5 | `L3_TEXTPATTERN2` | TextPattern2 |

**示例输出**：

```
// 成功：UIA Value 读取到文本（Chrome）
  Context: L2_UIA_VALUE 32ms class=Chrome_RenderWidgetHostHWND uia=Document len=156
  ContextText: "...这个分析错误可能是由于标点引起的"

// 成功：TextPattern VisibleRanges（Word）
  Context: L2.5_TEXTPATTERN 10ms class=_WwG uia=Document len=49
  ContextText: "Do you love me? It's the best friend. Yes, OK."

// 成功：WM_GETTEXT（记事本）
  Context: L1_WM_GETTEXT 2ms class=Edit len=30

// 仅窗口标题（微信场景）
  Context: NONE 5ms class=ChatWnd [项目讨论 - 微信]

// 历史记录兜底（开了 history 但没开 input context）
  Context: HISTORY 3 rounds

// 超时
  Context: NONE 200ms
  ContextFail: TIMEOUT

// 失败
  Context: NONE 45ms class=_WwG uia=Document [New Microsoft Word Document.docx - Word]
  ContextFail: UIA_EMPTY
```

**中文显示**：用 `WriteConsoleW` 输出 `ContextText`，避免 `printf` + `%ls`
在 Windows console 遇到非当前 codepage 的中文字符截断。

### 两个 debug 输出分支

1. **kAsrResultMessage**（无 LLM 纠错）：Header → Context → Pipeline → OK
2. **kLlmResultMessage**（有 LLM 纠错）：Header → Context → Pipeline → ASR → LLM

两个分支都调用 `DebugPrintInputContext()` 输出 Context，保持一致性。
`DebugPrintInputContext()` 内部逻辑：

1. `volcEnableInputContext` 开启时：显示输入框上下文详情（Layer/耗时/类名/控件类型/文本）
2. `volcEnableContext` 开启但 `volcEnableInputContext` 关闭时：显示 `Context: HISTORY N rounds`
3. 两个都关闭时：不输出任何 Context 信息

这样无论用户使用哪种上下文来源，debug 模式下都能看到 context 的发送情况。

### 全局变量

```cpp
// globals.h 中
#include "input_context.h"
extern InputContextResult g_inputContextResult;

// main.cpp 中
InputContextResult g_inputContextResult;
```

### 调用时机

```
StartRecordingSession() 中：
1. if (config.volcEnableInputContext) {
       g_inputContextResult = GetInputFieldContext();
       ctxInputText = g_inputContextResult.inputFieldText;
       hasInputText = !ctxInputText.empty();
   }
2. if (hasInputText) {
       vcfg.contextJson = BuildVolcContextJson(ctxInputText, false);  // 输入框优先，不发历史
   } else if (config.volcEnableContext) {
       vcfg.contextJson = BuildVolcContextJson(L"", true);            // 历史兜底
   }

两个开关独立控制：
- volcEnableInputContext: 控制是否读取输入框文本（优先）
- volcEnableContext: 控制是否用历史识别结果兜底（输入框不可用时）
- 输入框文本和历史记录不会同时发送，避免重复
- 窗口标题不再作为 context 发送（仅用于 debug 诊断）

### 文件日志（未实现）

方案中设计了 `WriteInputContextLog` 写入 `log/input_context_YYYYMMDD.log`，
但当前仅实现了 console 输出，文件日志待后续版本实现。

---

## 10. 实现路径

### 文件结构

```
src/input_context.h — header-only，UIA/WM_GETTEXT/MSAA/窗口标题 实现
  需要链接的库通过 #pragma comment(lib) 声明：
    #pragma comment(lib, "uiautomationcore.lib")
    #pragma comment(lib, "oleacc.lib")
    #pragma comment(lib, "oleaut32.lib")

src/main.cpp — 调用入口、BuildVolcContextJson、JsonEscape、debug 输出
src/globals.h — Config 字段 + InputContextResult 全局变量
src/engine.cpp — LoadConfig/SaveConfig
src/settings.cpp — Settings UI 控件
```

### 实现优先级

| 优先级 | 内容 | 理由 | 状态 |
|--------|------|------|------|
| P0 | UIA Value 属性读取 | 覆盖 90% 场景，最核心 | ✅ 已实现 |
| P0 | 超时保护 + 工作线程 | 防止 UIA 挂起阻塞录音 | ✅ 已实现 |
| P0 | Chrome 无障碍树触发 (WM_GETOBJECT) | 不做这个，Chrome/Edge 场景 UIA 可能拿不到值 | ✅ 已实现 |
| P0 | WM_GETTEXT 窗口标题误判修复 | Word 等应用 WM_GETTEXT 返回窗口标题，只对 Edit 类控件使用 | ✅ 已修复 |
| P0 | TextPattern VisibleRanges | Word Document 控件 Value 为空，RangeFromPoint 也可能失败，这是 Word 的关键路径 | ✅ 已实现 |
| P0 | ElementFromPoint 兜底 | GetFocusedElement 可能返回非文本元素，用坐标定位补充 | ✅ 已实现 |
| P0 | TryWalkParentsForText | 焦点落在子元素但文本在父元素上的场景 | ✅ 已实现 |
| P0 | JsonEscape 完整版 | Word 文档含 `\f` 等控制字符，不转义导致火山引擎 JSON 解析失败 | ✅ 已实现 |
| P1 | 密码框保护 | 安全必须 | ✅ 已实现 |
| P1 | Settings 开关 | 隐私必须 | ✅ 已实现 |
| P1 | 窗口标题上下文 | 零成本获取，仅用于 debug 诊断，不再作为 context 发送 | ✅ 已实现 |
| P1 | TextPattern v1 RangeFromPoint | 比 TextPattern2 兼容性好，能解决光标位置问题 | ✅ 已实现 |
| P1 | MSAA (IAccessible) get_accValue | 补充 UIA 覆盖不到的老应用场景 | ✅ 已实现 |
| P1 | Debug console 输出 | 方便排查问题 | ✅ 已实现 |
| P1 | WriteConsoleW 中文显示 | printf + %ls 遇到非当前 codepage 中文字符会截断 | ✅ 已实现 |
| P2 | WM_GETTEXT 快速路径 | 性能优化，仅 Edit 控件 | ✅ 已实现 |
| P2 | 简化版修改检测 | 提升上下文质量 | ❌ 未实现 |
| P3 | TextPattern2 GetCaretRange | 精确但兼容性有限，后续版本 | ✅ 已实现（作为 TryReadFromElement 的第 4 种方法） |
| P3 | 增量修改检测 | 覆盖更多场景，后续版本 | ❌ 未实现 |
| P2 | Debug 文件日志 (WriteInputContextLog) | 方便用户反馈排查 | ❌ 未实现（console 输出已实现） |

---

## 11. 参考资源

### AriaType 源码（本地）
- 窗口上下文 OCR：`.plan/ref/AriaType/apps/desktop/src-tauri/src/sensors/window_context.rs`
- 上下文解析：`.plan/ref/AriaType/apps/desktop/src-tauri/src/runtime_context/window.rs`

### 微软文档
- UI Automation: https://learn.microsoft.com/en-us/windows/win32/winauto/entry-uiauto-win32
- IUIAutomation: https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/
- TextPattern2: https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomationtextpattern2

### 火山引擎文档
- context 字段: https://www.volcengine.com/docs/6561/1354869
- 限制: 800 tokens, 20 轮

### zencrop_ocr SmartDetector（本地）
- SmartDetector 头文件：`D:\GITHUB_melody0709\zencrop_ocr\src\SmartDetector.h`
- SmartDetector 实现：`D:\GITHUB_melody0709\zencrop_ocr\src\SmartDetector.cpp`

**已借鉴的关键技术**：
1. **EnsureAccessibilityTree**：发送 `WM_GETOBJECT(0, 0xFFFFFFFC)` 触发 Chrome/Electron
   懒构建 UIA 树，用 `std::set<HWND>` 记录已触发的窗口
2. **ControlViewWalker 向上遍历**：`TryWalkParentsForText` 用 ControlViewWalker
   向上遍历父元素，到 Window 停止

---

## 12. 后续优化方向

1. **修改检测 + history 更新**：对比输入框文本与 history，自动更新被修改的条目
2. **进程名深度上下文**：根据进程名（如 WeChat.exe → 聊天场景、devenv.exe → 编程场景）提供更精准的场景提示
3. **OCR 上下文**：对不支持 UIA 的应用（微信），截图 + OCR 提取可见文本（参考 AriaType）
4. **上下文缓存**：同一输入框短时间内不重复读取
5. **热词自动提取**：从输入框文本中提取专业术语，作为 hotwords 发送
6. **持久化 UIA 工作线程**：避免每次录音创建/销毁线程，复用 IUIAutomation 实例
7. **s_triggeredWindows 清理**：定期清理无效 HWND，防止 HWND 复用问题
8. **Debug 文件日志**：实现 `WriteInputContextLog`，方便用户反馈排查

---

## 13. 风险分析与缓解措施

### 13.1 COM 线程模型冲突 — 风险等级：🟢 低

UIA 工作线程用 `COINIT_MULTITHREADED`，WASAPI 线程也用 `COINIT_MULTITHREADED`。
两者没有共享 COM 对象，互不干扰。

**关键决策**：不在 UI 线程初始化 `COINIT_APARTMENTTHREADED`（STA），
因为 UI 线程有 `g_volcThread.join()` 阻塞点，STA 线程在阻塞期间消息泵不运转，
可能导致 COM callback 死锁。

### 13.2 std::async 超时保护失效 — 风险等级：🔴 高（已修正）

`std::async(std::launch::async, ...)` 返回的 `std::future` 在析构时会
阻塞等待线程完成，导致超时保护完全无效。

**修正方案**：用 `std::thread` + `std::promise` + `std::future`，线程 detached。

### 13.3 UIA 线程泄漏 — 风险等级：🟡 中

超时后 detached 线程仍在运行，持有 COM 引用。
用 `std::atomic<bool> s_uiaThreadRunning` 防重入，避免累积。

### 13.4 UIA 调用与现有线程的交互 — 风险等级：🟢 低

UIA 工作线程只读取输入框文本，不修改任何 VoxType 全局状态。
结果通过 `std::promise` 传回 UI 线程。

### 13.5 StartRecordingSession 时序问题 — 风险等级：🟡 中

UIA 调用在 `StartRecordingSession` 中执行，增加录音启动延迟。
最坏情况 200ms 超时。Layer 1 (WM_GETTEXT) 通常 <5ms。

**必须用 `GetGUIThreadInfo`** 代替 `GetFocus()`（跨进程焦点获取）。

### 13.6 UIA 跨进程调用异常 — 风险等级：🟡 中

- 目标进程崩溃：COM 返回错误码，自动清理
- 目标进程挂起：200ms 超时保护兜底
- 高权限进程：UIPI 可能阻止，降级为仅用 history 上下文
- 游戏全屏：UIA 可能无法访问，降级

### 13.7 风险汇总

| 风险 | 等级 | 缓解措施 |
|------|------|----------|
| COM 线程模型冲突 | 🟢 低 | UIA 线程用 MTA，不修改 UI 线程 COM 状态 |
| std::async 超时失效 | 🔴 高（已修正） | 用 std::thread + detach 代替 std::async |
| UIA 线程泄漏 | 🟡 中 | atomic flag 防止累积 |
| 与现有线程交互 | 🟢 低 | UIA 线程无共享状态 |
| StartRecordingSession 延迟 | 🟡 中 | Layer 1 <5ms，Layer 2 最多 200ms |
| UIA 跨进程异常 | 🟡 中 | HRESULT 检查 + 超时保护 + 降级策略 |
| 录音期间窗口切换 | 🟢 低 | 行为正确，上下文与识别结果一致 |
| Chrome 无障碍树未触发 | 🟡 中 | EnsureAccessibilityTree 前置调用 |

### 13.8 录音期间窗口切换 — 风险等级：🟢 低

上下文在录音开始时读取，来自窗口 A。录音结束时粘贴到窗口 B。
上下文和识别结果一致，粘贴目标跟随用户焦点。行为正确。

### 13.9 Chrome 无障碍树未触发 — 风险等级：🟡 中

`EnsureAccessibilityTree` 在 Layer 2 入口处调用。
`s_triggeredWindows` 记录已触发的窗口，后续录音不需要重复触发。

### 13.10 WM_GETTEXT 窗口标题误判 — 风险等级：🔴 高（已触发，已修复）

**根因**：Word 的焦点 HWND 是顶层窗口（类名 `OpusApp`），`WM_GETTEXT` 返回窗口标题。
记事本的焦点 HWND 是 Edit 子控件，`WM_GETTEXT` 返回编辑内容。

**修复**：只对 `Edit` 类控件接受 WM_GETTEXT 结果，其他类名直接跳到 UIA。

### 13.11 UIA Document 控件 Value 为空 — 风险等级：🟡 中（已解决）

**场景**：Word 的 UIA 焦点元素是 `Document` 控件，`UIA_ValueValuePropertyId` 返回空。
`RangeFromPoint` 也可能失败。

**解决**：`TryTextPatternVisibleRanges` 读取 `GetVisibleRanges`，在 Word 中成功。
这是 Word 的关键读取路径。

### 13.12 JSON 控制字符导致火山引擎解析失败 — 风险等级：🔴 高（已触发，已修复）

**场景**：Word 文档包含 `\f`（form feed）和 `\v`（vertical tab）等控制字符，
初始版 `JsonEscape` 只转义了 `\n\r\t`，导致火山引擎返回
`invalid character '\f' in string literal`。

**修复**：`JsonEscape` 补全 `\b`、`\f` 转义，所有 `< 0x20` 控制字符用 `\uXXXX` 转义。

### 13.13 Console 中文显示截断 — 风险等级：🟡 中（已修复）

**场景**：`printf` + `%ls` 在 Windows console 遇到非当前 codepage 的中文字符会截断。

**修复**：`ContextText` 用 `WriteConsoleW` 输出，`OK` 行用 `DebugPrintTextLine`
（内部也用 `WriteConsoleW`）。
