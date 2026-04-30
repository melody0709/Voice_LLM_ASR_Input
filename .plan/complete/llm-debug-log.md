# Plan: LLM Debug Checkbox + Refine 对比日志

## 目标

在 Settings LLM tab 添加 Debug 复选框。勾选后每次 LLM refine 会将前后对比写入 `<app_root>/log/` 目录。

---

## 1. Config 扩展

位置：`main.cpp` Config struct (~L92)

```cpp
bool enableLlmDebug = false;
```

---

## 2. 控件 ID

位置：`main.cpp` (~L90)

```cpp
constexpr int IDC_LLM_DEBUG = 2025;
```

---

## 3. Settings UI — LLM Tab 添加 Debug 复选框

位置：`main.cpp` WM_CREATE (~L1817，在 Test Connection 按钮之后)

```
checkbox: [x] Log refine before/after to /log
```

布局：在 Test Connection 按钮右侧或下方

---

## 4. LoadSettingsControls

位置：`main.cpp` (~L1466)

```cpp
Button_SetCheck(GetDlgItem(hwnd, IDC_LLM_DEBUG), g_config.enableLlmDebug ? BST_CHECKED : BST_UNCHECKED);
```

---

## 5. SaveSettingsControls

位置：`main.cpp` (~L1521)

```cpp
g_config.enableLlmDebug = Button_GetCheck(GetDlgItem(hwnd, IDC_LLM_DEBUG)) == BST_CHECKED;
```

---

## 6. LoadConfig / SaveConfig

### LoadConfig (~L446)

```cpp
g_config.enableLlmDebug = ExtractJsonBool(json, "enable_llm_debug", false);
```

### SaveConfig (~L467)

```cpp
<< "  \"enable_llm_debug\": " << (g_config.enableLlmDebug ? "true" : "false") << "\n"
```

注意：最后一行不再有逗号，需要调整 JSON 输出顺序。

---

## 7. 日志写入逻辑

### 7.1 日志目录

```cpp
std::wstring LogDir() {
    return AppRootDir() + L"\\log";
}
```

### 7.2 日志文件名

```
log/llm_refine_20260430.log
```

按日期分文件，每天一个。

### 7.3 日志格式

```
[2026-04-30 14:32:05]
[ASR]  今天天气很好我们去公园散步吧
[LLM]  今天天气很好，我们去公园散步吧
---
```

每条记录包含时间戳、ASR 原文、LLM 结果，用 `---` 分隔。

### 7.4 写入位置

在 `RefineWithLlmAsync` 中，收到 LLM 结果后，如果 `config.enableLlmDebug`，写入日志文件。

位置：`main.cpp` `RefineWithLlmAsync` (~L1058)

```cpp
if (config.enableLlmDebug) {
    WriteLlmLog(asrText, result);
}
```

---

## 8. WriteLlmLog 函数

```cpp
void WriteLlmLog(const std::wstring& asrText, const std::wstring& llmText) {
    // 1. 确保 log 目录存在: CreateDirectoryW
    // 2. 生成文件名: log/llm_refine_YYYYMMDD.log
    // 3. 获取时间戳: GetLocalTime → "[YYYY-MM-DD HH:MM:SS]"
    // 4. 追加写入 (std::ofstream app)
    // 5. 写入: [时间戳]\n[ASR] ...\n[LLM] ...\n---\n
}
```

---

## 9. 修改文件清单

| 文件 | 修改 |
|------|------|
| `main.cpp` | Config + 控件 ID + UI checkbox + LoadConfig/SaveConfig + LoadSettingsControls/SaveSettingsControls + WriteLlmLog + RefineWithLlmAsync |

---

## 10. 实现顺序

1. Config 添加 `enableLlmDebug`
2. 控件 ID `IDC_LLM_DEBUG`
3. LoadConfig / SaveConfig
4. Settings WM_CREATE 添加 checkbox
5. LoadSettingsControls / SaveSettingsControls
6. WriteLlmLog 函数
7. RefineWithLlmAsync 调用 WriteLlmLog
8. 编译测试
