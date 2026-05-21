# IME 状态切换修复方案

## 问题描述

在微信输入框中，中文输入法下无法实现自动上屏。按住快捷键录音后松开，显示一个"v"而不是粘贴识别结果。其他输入框（浏览器等）正常。

## 根因分析

微信使用自定义输入控件，**更积极地遵循IME组合状态**：

1. `SendInput` 生成合成的 `Ctrl+V` 按键
2. Windows 将 `WM_KEYDOWN(VK_CONTROL)` 和 `WM_KEYDOWN('V')` 发送到前台窗口
3. **如果IME处于中文模式**，IME的键盘钩子（TSF/IMM32）拦截 `'V'` 按键
4. IME将 `'V'` 作为拼音输入字符处理（如输入"v"进入拼音模式）
5. `Ctrl+V` 组合被IME组合引擎消费，而不是作为粘贴命令

**为什么其他应用可以：**

| 应用 | IME集成方式 | Ctrl+V处理 |
|------|-------------|-----------|
| 浏览器、记事本 | 标准Windows编辑控件 | 在传递给IME前显式检查Ctrl+V |
| 微信 | 自定义RichEdit-like控件 | 将所有按键处理委托给IME |

## 解决方案：双层策略

### 第一层：`WM_PASTE`（优先）

`WM_PASTE` 是 Windows 标准粘贴消息，直接发送到目标控件，**完全不走键盘输入路径**，IME 无法拦截：

1. `GetFocus()` 获取焦点控件
2. `SendMessage(hwnd, WM_PASTE, 0, 0)` 触发粘贴
3. 微信的 RichEdit-like 控件通常支持此消息

### 第二层：IMM32 切换 + `Ctrl+V`（回退）

当 `GetFocus()` 返回 NULL 或目标窗口有 Admin 权限无法发送消息时，使用 IMM32 API 临时切换输入法到英文模式：

1. 获取前台窗口的 IMM 上下文
2. 保存当前 IME 状态（转换模式、打开状态）
3. 临时切换到英文模式（`IME_CMODE_ALPHANUMERIC`）
4. `Sleep(15)` 等待第三方 IME 异步完成模式切换
5. 发送 `Ctrl+V`
6. 恢复原 IME 状态

## 技术细节

### 所需API

```cpp
#include <imm.h>
#pragma comment(lib, "imm32.lib")
```

| 函数 | 用途 |
|------|------|
| `ImmGetContext(HWND)` | 获取指定窗口的输入上下文 |
| `ImmGetConversionStatus(HIMC, DWORD*, DWORD*)` | 读取当前IME转换模式 |
| `ImmSetConversionStatus(HIMC, DWORD, DWORD)` | 设置IME转换模式 |
| `ImmGetOpenStatus(HIMC)` | 查询IME是否打开 |
| `ImmSetOpenStatus(HIMC, BOOL)` | 打开/关闭IME |
| `ImmReleaseContext(HWND, HIMC)` | 释放输入上下文 |

### 转换模式常量

```
IME_CMODE_ALPHANUMERIC  = 0x0000  // 英文模式
IME_CMODE_NATIVE        = 0x0001  // 本地（CJK）模式
IME_CMODE_FULLSHAPE     = 0x0008  // 全角字符
```

典型中文IME状态：`IME_CMODE_NATIVE | IME_CMODE_FULLSHAPE` = `0x0009`
目标英文模式：`IME_CMODE_ALPHANUMERIC` = `0x0000`

### TSF兼容性

Windows 10/11 提供 IMM32 到 TSF 的兼容层，系统会将 `ImmSetConversionStatus()` 调用转换为等效的 TSF 操作。这意味着：
- IMM32 API 对大多数 IME（包括微软拼音）有效
- 无需额外实现 TSF 接口（`ITfThreadMgr` 等）

### 第三方 IME 注意事项

搜狗/百度等第三方输入法处理模式切换是**异步**的，不加延时 `V` 仍可能被吞：
- `ImmSetConversionStatus` 后必须 `Sleep(15)` 等待异步完成
- 部分第三方 IME 可能完全忽略 IMM32 调用——此时 `WM_PASTE` 作为第一层不受影响，因为不走键盘路径
- 如果两层的失效，用户可通过 Ctrl+Shift 手动切英文模式后粘贴（已在 `ImeStateGuard::Disable` 中跳过）

## 实施计划

### 1. 修改 `src/settings.cpp`

添加 `ImeStateGuard` 结构体和 `SendCtrlVImeAware()` 函数：

```cpp
#include <imm.h>
#pragma comment(lib, "imm32.lib")

struct ImeStateGuard {
    HWND  targetWnd = nullptr;
    HIMC  hIMC      = nullptr;
    DWORD savedConv = 0;
    DWORD savedSent = 0;
    BOOL  savedOpen = FALSE;
    bool  active    = false;

    bool Disable() {
        targetWnd = GetForegroundWindow();
        if (!targetWnd) return false;

        HWND focused = GetFocus();
        if (focused && IsChild(targetWnd, focused)) {
            targetWnd = focused;
        }

        hIMC = ImmGetContext(targetWnd);
        if (!hIMC) return false;

        ImmGetConversionStatus(hIMC, &savedConv, &savedSent);
        savedOpen = ImmGetOpenStatus(hIMC);

        if (savedConv == IME_CMODE_ALPHANUMERIC && !savedOpen) {
            ImmReleaseContext(targetWnd, hIMC);
            hIMC = nullptr;
            return false;
        }

        ImmSetOpenStatus(hIMC, FALSE);
        ImmSetConversionStatus(hIMC, IME_CMODE_ALPHANUMERIC, 0);
        Sleep(15);  // 等待第三方IME异步完成模式切换
        active = true;
        return true;
    }

    void Restore() {
        if (!active || !hIMC) return;
        ImmSetConversionStatus(hIMC, savedConv, savedSent);
        ImmSetOpenStatus(hIMC, savedOpen);
        if (targetWnd) ImmReleaseContext(targetWnd, hIMC);
        hIMC = nullptr;
        active = false;
    }

    ~ImeStateGuard() { Restore(); }
};

void SendCtrlVImeAware() {
    // 第一层：WM_PASTE 直接发送到焦点控件，不走键盘路径，IME 无法拦截
    HWND focus = GetFocus();
    if (focus) {
        SendMessage(focus, WM_PASTE, 0, 0);
        return;
    }

    // 第二层：回退到 IMM32 切换 + SendInput Ctrl+V
    ImeStateGuard guard;
    guard.Disable();
    SendCtrlV();
}
```

### 2. 修改 `src/settings.h`

添加函数声明：

```cpp
void SendCtrlVImeAware();
```

### 3. 修改 `src/main.cpp`

第561行和第598行，将 `SendCtrlV()` 替换为 `SendCtrlVImeAware()`：

```cpp
// 第561行（kAsrResultMessage处理）
SetClipboardText(text);
SendCtrlVImeAware();  // 原 SendCtrlV()

// 第598行（kLlmResultMessage处理）
SetClipboardText(text);
SendCtrlVImeAware();  // 原 SendCtrlV()
```

### 4. 修改 `build.bat`

第24行添加 `imm32.lib`：

```bat
cl /nologo /O2 /EHsc /MT /std:c++17 /utf-8 ... user32.lib ... imm32.lib ...
```

### 5. 修改 `CMakeLists.txt`

第41-58行添加 `imm32`：

```cmake
target_link_libraries(VoxType
    ...
    imm32
)
```

## 边界情况处理

| 情况 | 处理方式 |
|------|---------|
| 未安装IME（英文Windows） | `ImmGetContext()` 返回NULL，跳过处理 |
| 窗口不支持IMM（控制台应用等） | `ImmGetContext()` 返回NULL，跳过处理 |
| 录音期间窗口被关闭 | 使用当前前台窗口（正确行为） |
| Admin权限窗口 | `ImmGetContext()` 可能失败，静默处理 |
| 多显示器/虚拟桌面 | `GetForegroundWindow()` 返回当前前台窗口 |
| GetFocus()返回NULL | 回退到`GetForegroundWindow()` |

## 验收标准

1. 微信输入框中，中文输入法下能正常粘贴识别结果
2. 粘贴后输入法状态恢复（仍为中文模式）
3. 其他应用（浏览器、记事本等）行为不变
4. 英文Windows下无异常
5. 编译通过，无功能回归

## 参考

- 原设计文档 `Voice_LLM_ASR_Input.md` 第48-53行已规划此方案
- `OPTIMIZATION_PLAN.md` 第80行列为后续任务
- `ARCHITECTURE.md` 第200-213行记录了未来改进方向
