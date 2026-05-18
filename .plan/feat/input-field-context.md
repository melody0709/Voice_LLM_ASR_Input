# 输入框上下文获取方案研究

> 目标：读取当前光标所在输入框的已有文本，作为 ASR 上下文发送给服务端，
> 提升识别准确率。特别是用户手动修改了识别错误后，下次识别应利用修改后的文本。

---

## 1. 核心需求

1. **读取输入框已有文本** → 作为 ASR 上下文
2. **捕获用户手动修改** → 下次识别时用修改后的文本替代原始识别结果
3. **非侵入性** → 不影响用户操作，不闪烁，不破坏剪贴板
4. **性能** → <100ms，不阻塞录音启动

---

## 2. 当前 VoxType 上下文机制

`main.cpp:162-181` 用 `g_volcRecognitionHistory`（历史识别结果）构建
火山引擎 `corpus.context`，没有读取输入框已有文本。

```
BuildVolcContextJson():
  {"context_type":"dialog_ctx","context_data":[
    {"text":"上次识别结果1"},
    {"text":"上次识别结果2"}
  ]}
```

火山引擎 context 限制：800 tokens，20 轮以内。

---

## 3. 方案对比

| 方案 | 兼容性 | 性能 | 侵入性 | 实现难度 | 推荐度 |
|------|--------|------|--------|----------|--------|
| **UIA Value 属性** | 高 | 10-50ms | 无 | 中 | ★★★★★ |
| **UIA TextPattern2** | 中 | 20-80ms | 无 | 中 | ★★★★ |
| **WM_GETTEXT** | 低 | <5ms | 无 | 低 | ★★★ |
| IAccessible/MSAA | 中 | 10-30ms | 无 | 低 | ★★ |
| TSF | 极低 | N/A | 无 | 极高 | ★ |
| 剪贴板方法 | 高 | 100-300ms | 极高 | 中 | ★ |

---

## 4. 推荐方案：分层 Fallback

```
录音开始时，在工作线程获取输入框上下文：

Layer 1: WM_GETTEXT + EM_GETSEL（快速路径，仅 Win32 控件）
  GetFocus() → AttachThreadInput → SendMessage(WM_GETTEXT)
  SendMessage(EM_GETSEL) → 获取光标位置
  截取光标前最后 200 字符
  性能: <5ms
  适用: 记事本、Win32 Edit 控件

Layer 2: UIA Value 属性（主力方案，覆盖 90% 场景）
  pAutomation->GetFocusedElement(&pFocused)
  检查 ControlType == Edit/Document
  pFocused->GetCurrentPropertyValue(UIA_ValueValuePropertyId, &var)
  → 拿到输入框全文
  → 截取最后 200 字符（假设光标在末尾，语音输入场景通常如此）
  性能: 10-50ms
  适用: Win32/WPF/Electron/Chrome/Edge

Layer 3: UIA TextPattern2（精确光标位置，增强功能）
  pTextPattern2->GetCaretRange(&pCaretRange)
  pCaretRange->Move(TextUnit_Character, -200)
  pCaretRange->GetText(-1, &bstrText)
  性能: 20-80ms
  适用: WPF TextBox、RichEdit、部分浏览器

Layer 4: 放弃
  微信、Java、游戏等不支持 UIA 的应用
  → 仅使用历史识别结果
```

---

## 5. 各应用兼容性

| 应用 | WM_GETTEXT | UIA Value | TextPattern2 | 备注 |
|------|-----------|-----------|-------------|------|
| 记事本 | ✅ | ✅ | ❌ | Edit 控件 |
| Word | ❌ | ✅ | ✅ | Office UIA 完善 |
| Excel | ❌ | ✅ | ❌ | 单元格编辑模式 |
| VS Code | ❌ | ✅ | ✅ | Electron，编辑器为 Document |
| Chrome 网页输入框 | ❌ | ✅ | 部分 | Chromium 内置 UIA |
| Edge 网页输入框 | ❌ | ✅ | 部分 | 同 Chrome |
| Firefox 网页输入框 | ❌ | ✅ | 部分 | 独立 UIA 实现 |
| 微信 | ❌ | ❌ | ❌ | Qt 自绘控件，UIA 不可见 |
| QQ | ❌ | ❌ | ❌ | 同微信 |
| 钉钉 | ❌ | 部分 | ❌ | Electron 壳 + 部分 UIA |
| 飞书 | ❌ | 部分 | ❌ | Electron 壳 |
| Telegram | ❌ | 部分 | ❌ | 部分控件可见 |
| Discord | ❌ | 部分 | ❌ | Electron |

**微信/QQ 是最大盲区**——VoxType 已有微信粘贴适配（WM_CHAR 逐字符发送），
但读取微信输入框文本目前无解。AriaType 在 macOS 上用 Accessibility API
可以读取，但 Windows 上同样无法读取微信。

---

## 6. UIA 实现要点

### 6.1 延迟初始化

```cpp
// 首次调用时初始化 COM，避免启动时 50-200ms 开销
static IUIAutomation* s_pAutomation = nullptr;

IUIAutomation* GetUIAutomation() {
    if (!s_pAutomation) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CoCreateInstance(CLSID_CUIAutomation, nullptr,
            CLSCTX_INPROC_SERVER, IID_IUIAutomation, (void**)&s_pAutomation);
    }
    return s_pAutomation;
}
```

### 6.2 超时保护

UIA 跨进程调用可能挂起（目标进程无响应时）。策略：
- 在工作线程执行，不阻塞 UI 线程
- 用 `std::async` + `wait_for(200ms)` 超时
- 超时后放弃，不影响录音

```cpp
auto future = std::async(std::launch::async, [&] {
    return ReadInputFieldTextUIA();
});
if (future.wait_for(std::chrono::milliseconds(200)) == std::future_status::timeout) {
    return L""; // 超时放弃
}
return future.get();
```

### 6.3 焦点元素过滤

不是所有焦点元素都是文本控件，需要过滤：

```cpp
CONTROLTYPEID controlType;
pFocused->GetCurrentControlType(&controlType);

// 接受的类型
if (controlType != UIA_EditControlTypeId &&
    controlType != UIA_DocumentControlTypeId &&
    controlType != UIA_TextControlTypeId) {
    return L""; // 不是文本控件
}
```

### 6.4 浏览器特殊处理

浏览器中焦点可能在文档级别，需要向下遍历找到 Edit 控件：

```cpp
// 如果焦点是 Document 类型，尝试找子 Edit
if (controlType == UIA_DocumentControlTypeId) {
    IUIAutomationCondition* editCond = nullptr;
    pAutomation->CreatePropertyCondition(
        UIA_ControlTypePropertyId,
        CComVariant(UIA_EditControlTypeId), &editCond);
    IUIAutomationElement* pEdit = nullptr;
    pFocused->FindFirst(TreeScope_Children, editCond, &pEdit);
    if (pEdit) pFocused = pEdit; // 用 Edit 替代 Document
}
```

### 6.5 密码框保护

```cpp
BOOL isPassword = FALSE;
pFocused->GetCurrentPropertyValue(UIA_IsPasswordPropertyId, &var);
if (var.boolVal) return L""; // 不读取密码框
```

---

## 7. 用户修改检测

### 核心逻辑

```
每次 ASR 识别结果插入后：
  1. 记录插入的文本 → g_volcRecognitionHistory（已有）
  2. 下次录音开始时，读取输入框当前文本
  3. 在输入框文本中搜索 history 中的原始文本
  4. 如果发现差异 → 用户手动修改了
  5. 用修改后的文本替换 history 中的条目
  6. 修改后的文本作为上下文发送给 ASR
```

### 示例

```
第1次识别: "这个分词错误可能是由于标点引起的"  → 插入输入框
用户手动改为: "这个分析错误可能是由于标点引起的"  ← 修改了"分词"→"分析"

第2次录音开始时:
  读取输入框文本: "...这个分析错误可能是由于标点引起的"
  对比 history: "这个分词错误可能是由于标点引起的"
  发现差异 → 更新 history 为修改后的文本
  发送给 ASR 的 context: "这个分析错误可能是由于标点引起的"
  → 下次识别"分析"时，ASR 更可能输出正确的"分析"而不是"分词"
```

### 修改检测算法

```
对每条 history 条目:
  1. 在输入框文本中搜索该条目的近似位置
  2. 提取对应位置的当前文本
  3. 计算编辑距离或简单字符串比较
  4. 如果不同 → 更新 history

简化方案（不需要精确位置匹配）:
  - 只检查最后一条 history（最近一次识别结果）
  - 假设它在输入框文本末尾
  - 比较末尾 N 个字符是否与 history 匹配
  - 不匹配 → 用输入框末尾文本替换
```

### 简化方案的优势

- 不需要精确的光标位置
- 不需要复杂的字符串搜索
- 覆盖最常见的场景（用户修改最近一次识别结果）
- 性能好（只比较末尾文本）

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
VoxType 已有此处理逻辑（volcengine_asr.h:522-533）。

### 上下文合并策略

```
BuildVolcContextJson(inputFieldText):
  context_data = []

  // 输入框文本作为第一条上下文（权重最高）
  if (!inputFieldText.empty()):
    context_data.append({"text": inputFieldText[-200:]})

  // 历史识别结果（已可能被修改检测更新）
  for entry in g_volcRecognitionHistory:
    context_data.append({"text": entry})

  // 总量控制在 800 tokens / 20 轮以内
  // 火山引擎会按时间顺序从新到旧截断
```

---

## 9. 隐私考虑

- UIA 读取不触发任何系统提示（Windows 11 没有 macOS 那样的无障碍权限弹窗）
- 应在 Settings 中说明此功能，并提供开关
- 不读取密码框（`UIA_IsPasswordPropertyId` 检查）
- 只取最后 200 字符，不读取完整文档

---

## 10. 实现路径

### 新增文件

```
src/input_context.h   — 接口声明
src/input_context.cpp — UIA/WM_GETTEXT 实现
```

### 修改文件

```
src/main.cpp:
  - StartRecordingSession 中调用 InputContextGetText()
  - BuildVolcContextJson() 合并输入框文本 + history
  - 识别结果插入后，对比输入框文本更新 history

src/globals.h:
  - Config 增加 volcEnableInputContext (bool)
  - g_inputFieldContext (std::wstring) 缓存

src/settings.cpp:
  - 增加"输入框上下文"开关

build.bat:
  - 添加 uiautomationcore.lib
```

### 工作量估计

| 部分 | 难度 | 工作量 |
|------|------|--------|
| UIA Value 属性读取 | 中 | 1-2h |
| WM_GETTEXT fallback | 低 | 30min |
| 光标位置获取 | 中 | 1h |
| 修改检测 + history 更新 | 中 | 1-2h |
| Settings UI | 低 | 30min |
| 编译验证 + 测试 | 中 | 1h |
| **总计** | | **5-7h** |

---

## 11. 参考资源

### AriaType 源码（本地）
- 窗口上下文 OCR：`.plan/ref/AriaType/apps/desktop/src-tauri/src/sensors/window_context.rs`
- 上下文解析：`.plan/ref/AriaType/apps/desktop/src-tauri/src/runtime_context/window.rs`
- STT 上下文 trait：`.plan/ref/AriaType/apps/desktop/src-tauri/src/stt_engine/traits.rs`

### 微软文档
- UI Automation: https://learn.microsoft.com/en-us/windows/win32/winauto/entry-uiauto-win32
- IUIAutomation: https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/
- TextPattern2: https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nn-uiautomationclient-iuiautomationtextpattern2
- GetCaretRange: https://learn.microsoft.com/en-us/windows/win32/api/uiautomationclient/nf-uiautomationclient-iuiautomationtextpattern2-getcaretrange

### 火山引擎文档
- context 字段: https://www.volcengine.com/docs/6561/1354869
- 限制: 800 tokens, 20 轮

---

## 12. 后续优化方向

1. **TextPattern2 精确光标位置**：获取光标前后文本，而非假设光标在末尾
2. **增量修改检测**：不只检查最后一条，检查所有 history 条目
3. **窗口标题上下文**：结合窗口标题 + 进程名构建更丰富的上下文
4. **OCR 上下文**：对不支持 UIA 的应用（微信），截图 + OCR 提取可见文本
5. **上下文缓存**：同一输入框短时间内不重复读取
6. **热词自动提取**：从输入框文本中提取专业术语，作为 hotwords 发送
