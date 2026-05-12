# 微信文本输入替代方案研究

## 背景

当前 VoxType 对微信采用 `PostMessageW(fg, WM_CHAR, ch, 0)` 逐字符发送，每字符 `Sleep(1)`。原因见 AGENTS.md 踩坑规则：

> 微信（`Weixin.exe`）用自定义 Qt 控件，`GetFocus()` 返回 NULL 且 IME 拦截 Ctrl+V。必须用 `WM_CHAR` 逐字符发送，不能用剪贴板+Ctrl+V。

**问题**：逐字符 WM_CHAR 有明显缺点——长文本输入慢（每字 1ms+），视觉上逐字出现像打字机，且 `PostMessageW` 发到 `fg`（前台顶层窗口）而非实际输入控件，可能在某些微信版本/布局下发错目标。

用户观察到其他语音输入法（讯飞等）使用剪贴板方式也能在微信中正常输入，说明存在替代方案。

---

## 当前实现分析

代码位于 [settings.cpp:126-171](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/settings.cpp#L126-L171)：

```
PasteTextImeAware(text)
├── forceUnicodeInput → SendUnicodeText()  (SendInput KEYEVENTF_UNICODE 逐字符)
├── GetFocus() != NULL → SetClipboardText + SendMessage(WM_PASTE)
├── !isWeChat → SetClipboardText + ImeStateGuard.Disable() + SendCtrlV()
└── isWeChat → PostMessageW(fg, WM_CHAR, ch, 0) × N  ← 当前微信路径
```

---

## 替代方案

### 方案 A：UI Automation ValuePattern（推荐优先验证）

**原理**：Windows UI Automation API 可以遍历微信窗口的控件树，找到输入框 `EditControl`，通过 `ValuePattern.SetValue()` 直接设置文本，或通过 `TextPattern` 插入文本。

**参考实现**：
- Python `uiautomation` 库广泛用于微信自动化，已验证可行
- 微信输入框可通过 `EditControl` + `Name='输入'` 定位
- `pyperclip.copy(text) + auto.SendKeys('{Ctrl}v')` 组合在微信中可用

**C++ 实现路径**：
1. `UIAutomationCreate()` 获取 `IUIAutomation` 接口
2. `ElementFromHandle(fgWindow)` 获取微信窗口 Automation 元素
3. 递归查找 `ControlType.Edit` + `Name` 含 "输入" 的子元素
4. 尝试 `ValuePattern.SetValue(text)` 直接设值
5. 若 ValuePattern 不可用，则 `SetClipboardText` + 聚焦输入框 + `SendCtrlV()`

**优点**：
- 直接操作目标控件，不依赖顶层窗口转发
- SetValue 是原子操作，瞬间完成，无逐字延迟
- 不破坏剪贴板（SetValue 不需要剪贴板）
- 微信 Qt 控件暴露了 UI Automation 接口（Python 生态已验证）

**缺点**：
- 需要链接 `oleaut32.lib`、`oleacc.lib`，增加 COM 初始化开销
- UI Automation 树遍历可能较慢（首次调用 ~50-200ms）
- 微信版本更新可能改变控件树结构
- `ValuePattern.SetValue` 会替换全部内容而非追加，需先获取已有文本再拼接
- 需要在目标线程初始化 COM（`CoInitializeEx`）

**复杂度**：中等。约 150-200 行 C++ 代码。

---

### 方案 B：UI Automation 定位 + 剪贴板 Ctrl+V

**原理**：用 UI Automation 找到微信输入框并聚焦，然后走剪贴板 + Ctrl+V 路径。这实际上就是讯飞等输入法的工作方式——它们作为 TSF 输入法注册，文本通过 TSF 框架提交，而 TSF 框架会自动与 Qt 控件交互。

**实现路径**：
1. 用 UI Automation 找到微信输入框 `EditControl`
2. 调用 `EditControl.SetFocus()` 或 `Click()` 聚焦
3. `SetClipboardText(text)`
4. 临时关闭 IME（`ImeStateGuard`）
5. `SendCtrlV()`

**关键洞察**：当前代码之所以微信 Ctrl+V 失败，是因为 `GetFocus()` 返回 NULL，导致无法获取正确的 IME 上下文。如果能通过 UI Automation 正确聚焦输入框，`GetFocus()` 就能返回有效句柄，IME 关闭 + Ctrl+V 就能正常工作。

**优点**：
- 复用现有 `SetClipboardText` + `ImeStateGuard` + `SendCtrlV` 逻辑
- 剪贴板粘贴是追加模式，不覆盖已有文本
- 实现相对简单

**缺点**：
- 仍需 UI Automation 定位控件（同方案 A 的 COM 依赖）
- 聚焦操作可能引起视觉闪烁
- 仍需处理 IME 状态
- 剪贴板内容被覆盖

**复杂度**：中等偏低。约 100-150 行 C++ 代码。

---

### 方案 C：TSF Text Service 注册（OpenLess 方案）

**原理**：注册一个自定义 TSF Text Input Processor (TIP)，当需要插入文本时，临时激活该 TIP，通过 TSF edit session 将文本 commit 到目标应用的文本流中。这是最"正统"的方式，也是讯飞输入法等 TSF 输入法的工作原理。

**参考实现**：[OpenLess PR #210](https://github.com/appergb/openless/pull/210) 完整实现了此方案。

**架构**：
```
VoxType.exe (主进程)
  ↓ named pipe IPC
VoxTypeIme.dll (TSF TIP, 加载在目标进程中)
  ↓ ITfRange.SetText
目标应用 (微信等)
```

**流程**：
1. 录音结束 → 捕获当前活跃 IME profile
2. 临时切换到 VoxType TIP
3. 通过 named pipe 发送文本到 VoxTypeIme DLL
4. VoxTypeIme 在 TSF edit session 中 `ITfRange::SetText` 提交文本
5. 恢复之前的 IME profile
6. 失败时回退到 SendInput / 剪贴板

**优点**：
- 最正统的 Windows 文本插入方式
- 对所有 TSF-aware 应用通用（包括微信、Word、浏览器等）
- 不破坏剪贴板
- 不需要逐字符发送
- 文本通过 TSF 框架正确触发应用的文本变更事件

**缺点**：
- **实现复杂度极高**：需要写一个完整的 COM DLL（TSF TIP），实现 `ITfTextInputProcessor`、`ITfEditSession` 等多个接口
- 需要注册 COM 组件和 TSF 类别（写注册表），需要管理员权限
- 需要处理 x64 和 Win32 两种架构
- 需要处理 TSF session 生命周期、线程安全、竞态条件
- 安装/卸载需要额外步骤
- 调试困难（DLL 加载在目标进程中）
- OpenLess 的实现用了 46 个 commit，涉及 ~2000+ 行 Rust 代码

**复杂度**：极高。C++ 估计 500-800 行 DLL + 安装注册脚本 + IPC 机制。

---

### 方案 D：SendInput KEYEVENTF_UNICODE 替代 PostMessage WM_CHAR

**原理**：当前 `forceUnicodeInput` 模式已实现此方案。`SendInput` + `KEYEVENTF_UNICODE` 在 OS 级别生成键盘事件，比 `PostMessage WM_CHAR` 更"真实"，且不需要知道目标窗口句柄。

**当前代码**（[settings.cpp:63-76](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/settings.cpp#L63-L76)）：
```cpp
void SendUnicodeText(const std::wstring& text) {
    for (wchar_t ch : text) {
        INPUT inputs[2] = {};
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = ch;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        inputs[1].type = INPUT_KEYBOARD;
        inputs[1].ki.wScan = ch;
        inputs[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, inputs, sizeof(INPUT));
        Sleep(1);
    }
}
```

**优点**：
- 代码已存在，只需将微信路径从 `PostMessageW WM_CHAR` 改为 `SendInput KEYEVENTF_UNICODE`
- 不需要知道目标窗口句柄
- SendInput 是 OS 级事件，所有应用都能接收
- 不需要额外依赖

**缺点**：
- 仍然是逐字符发送，有延迟
- 仍然像打字机效果
- `KEYEVENTF_UNICODE` 事件会被 IME 拦截处理，中文输入法开启时可能产生意外结果
- 需要确保前台窗口确实是微信（SendInput 发给当前焦点窗口）

**复杂度**：极低。改动约 5 行。

---

### 方案 E：子窗口枚举 + WM_PASTE

**原理**：微信 `GetFocus()` 返回 NULL 是因为 Qt 自绘控件不响应 Win32 焦点。但微信窗口内仍有子窗口层级，可以通过 `EnumChildWindows` 找到实际接收文本输入的子窗口，然后对其发送 `WM_PASTE`。

**实现路径**：
1. `EnumChildWindows(fg, callback, ...)` 遍历微信子窗口
2. 查找类名包含 "Edit" 或 "Qt" 的子窗口
3. 对该子窗口 `SetClipboardText` + `SendMessage(childHwnd, WM_PASTE, 0, 0)`

**优点**：
- 不需要 COM / UI Automation 依赖
- WM_PASTE 是原子操作，瞬间完成
- 实现简单

**缺点**：
- 微信 Qt 控件可能没有传统 Win32 子窗口（Qt 自绘），`EnumChildWindows` 可能找不到
- 即使找到子窗口，WM_PASTE 也可能被 Qt 事件过滤器拦截
- 需要实际测试验证微信子窗口结构

**复杂度**：低。约 50-80 行 C++ 代码。

---

## 方案对比

| 维度 | A: UIA ValuePattern | B: UIA+剪贴板 | C: TSF TIP | D: SendInput Unicode | E: 子窗口 WM_PASTE |
|------|---------------------|---------------|------------|----------------------|-------------------|
| 输入速度 | ⚡ 瞬间 | ⚡ 瞬间 | ⚡ 瞬间 | 🐌 逐字 | ⚡ 瞬间 |
| 实现复杂度 | 中 | 中低 | 极高 | 极低 | 低 |
| 额外依赖 | oleaut32/oleacc | oleaut32/oleacc | 独立 DLL + 注册 | 无 | 无 |
| 剪贴板影响 | ✅ 不破坏 | ❌ 覆盖 | ✅ 不破坏 | ✅ 不破坏 | ❌ 覆盖 |
| 通用性 | 好 | 好 | 最好 | 好 | 差 |
| 微信兼容风险 | 中（版本更新） | 中（版本更新） | 低 | 低 | 高（可能找不到子窗口） |
| IME 干扰 | 无 | 需处理 | 无 | 可能被拦截 | 需处理 |
| 追加/替换 | 替换（需拼接） | 追加 | 追加 | 追加 | 追加 |

---

## 推荐实施路径

### Phase 1：快速验证（1-2 天）

1. **验证方案 E**：写一个小工具 `EnumChildWindows` 遍历微信窗口，输出子窗口类名和层级。确认 Qt 控件是否有可用的子窗口句柄。
2. **验证方案 A/B**：用 C++ 写一个最小 POC，通过 UI Automation 查找微信输入框，尝试 `ValuePattern.SetValue` 和 `SetFocus + Ctrl+V`。
3. **验证方案 D**：在微信中测试 `SendInput KEYEVENTF_UNICODE`，确认中文输入法开启时是否正常。

### Phase 2：实施最佳方案（根据验证结果选择）

**最可能的推荐**：方案 B（UIA 定位 + 剪贴板 Ctrl+V）

理由：
- 讯飞等输入法通过 TSF 框架实现，本质是"找到输入控件 → 提交文本"。方案 B 是最接近此原理的轻量实现。
- 当前微信 Ctrl+V 失败的根因是 `GetFocus() == NULL`，导致无法正确操作 IME。UI Automation 可以绕过此限制。
- 实现复杂度适中，不需要独立 DLL 和注册。
- 剪贴板覆盖问题可后续通过剪贴板恢复机制解决（已在 OPTIMIZATION_PLAN.md P0 §1 规划）。

**备选**：如果方案 B 的 IME 问题仍无法解决，则方案 A（ValuePattern）作为不需要 IME 的替代。

**长期**：方案 C（TSF TIP）是终极方案，但建议作为 v2.0 目标，当前版本先不实施。

### Phase 3：回退策略

所有新方案都应保留当前 `WM_CHAR` 逐字符作为最终 fallback：

```
PasteTextImeAware(text)
├── GetFocus() != NULL → WM_PASTE（不变）
├── isWeChat:
│   ├── [新] UIA 找到输入框 → SetFocus → ImeStateGuard + Ctrl+V
│   ├── [新] UIA ValuePattern.SetValue（如果可用）
│   ├── [新] SendInput KEYEVENTF_UNICODE（IME 关闭时）
│   └── [旧] PostMessageW WM_CHAR 逐字符（最终 fallback）
└── !isWeChat → 剪贴板 + IME + Ctrl+V（不变）
```

---

## 技术细节备忘

### UI Automation C++ 最小 POC 要点

```cpp
#include <uiautomation.h>
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "oleacc.lib")
#pragma comment(lib, "uuid.lib")

// 初始化
CoInitializeEx(NULL, COINIT_MULTITHREADED);
IUIAutomation* pAutomation = nullptr;
CoCreateInstance(CLSID_CUIAutomation, nullptr, CLSCTX_INPROC_SERVER,
                 IID_IUIAutomation, (void**)&pAutomation);

// 从窗口句柄获取元素
IUIAutomationElement* pElement = nullptr;
pAutomation->ElementFromHandle(hwndWeChat, &pElement);

// 查找 Edit 控件
IUIAutomationCondition* pCondition = nullptr;
pAutomation->CreatePropertyCondition(UIA_ControlTypePropertyId,
                                      UIA_EditControlTypeId, &pCondition);
IUIAutomationElement* pEdit = nullptr;
pElement->FindFirst(TreeScope_Descendants, pCondition, &pEdit);

// 方案 A: ValuePattern
IValueProvider* pValue = nullptr;
pEdit->GetCurrentPatternAs(UIA_ValuePatternId, IID_IValueProvider, (void**)&pValue);
pValue->SetValue(text);

// 方案 B: 聚焦 + Ctrl+V
pEdit->SetFocus();
// ... SetClipboardText + ImeStateGuard + SendCtrlV
```

### 微信控件树参考（Python uiautomation 验证）

```
WindowControl (ClassName='WeChatMainWndForPC', Name='微信')
  └─ PaneControl
       └─ ...
            └─ EditControl (Name='输入', 深度约 10-15)
                 └─ 支持 ValuePattern / SendKeys
```

### 关键注意

- UI Automation 需要在调用线程初始化 COM（`CoInitializeEx`）
- VoxType 主线程已有消息循环，COM 初始化应在 `WinMain` 早期完成
- UI Automation 操作应在 UI 线程执行，不应在 ASR 回调线程
- `ValuePattern.SetValue` 是替换语义，需先读取已有文本再拼接
- 微信输入框可能不支持 `ValuePattern`（Qt 自定义控件），需实际测试
- `FindFirst(TreeScope_Descendants)` 在微信深层控件树中可能较慢，可缓存结果
