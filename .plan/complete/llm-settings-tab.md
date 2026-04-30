# Plan: LLM Refine + Settings Tab + 硅基流动适配

## 目标

1. Settings 新增 LLM tab，配置 OpenAI 兼容 API
2. ASR 识别后调用 LLM 保守纠错，提升中英文混杂场景准确率
3. HUD 显示 "Refining..." 状态，LLM 返回后再注入文本

---

## 整体流程

```
录音停止
  → HUD "Recognizing... FireRedASR2 CTC"
  → ASR + 标点
  → 如果 postprocess=="llm" 且已配置 API：
      → HUD "Refining..." (灰色文字)
      → WinHTTP 异步 POST 到 LLM API
      → 收到响应 → 写剪贴板 + Ctrl+V
  → 否则直接注入
```

---

## 1. Config 扩展

位置：`main.cpp` Config struct (~L85)

```cpp
struct Config {
    // ... 现有字段 ...
    std::wstring llmEndpoint = L"https://api.siliconflow.cn/v1";
    std::wstring llmApiKey;
    std::wstring llmModel = L"Qwen/Qwen3.5-4B";
};
```

---

## 2. Settings UI — LLM Tab

### 2.1 Tab 扩展

当前：`Recognition` | `Shortcut`
新增：`LLM` (index=2)

### 2.2 控件布局

```
┌──────────────────────────────────────────────────────────┐
│                                                          │
│  API Base URL   [https://api.siliconflow.cn/v1         ] │
│                                                          │
│  API Key        [••••••••••••••••••••••••••••••••••••  ] │
│                                                          │
│  Model          [Qwen/Qwen3.5-4B                      ] │
│                                                          │
│                 [Test Connection]                        │
│                                                          │
└──────────────────────────────────────────────────────────┘
```

### 2.3 控件 ID

```cpp
constexpr int IDC_LLM_ENDPOINT = 2020;
constexpr int IDC_LLM_KEY      = 2021;
constexpr int IDC_LLM_MODEL    = 2022;
constexpr int IDC_LLM_TEST     = 2023;
```

### 2.4 API Key 输入框

- `ES_PASSWORD` 样式显示圆点
- Show/Hide 按钮切换 `EM_SETPASSWORDCHAR`
- Backspace/Delete 完全清空

### 2.5 g_llmControls

```cpp
std::vector<HWND> g_llmControls;
// ShowSettingsPage: page==2 时 ShowWindow(SW_SHOW)
```

---

## 3. Config 读写

### LoadConfig (~L419)

```cpp
g_config.llmEndpoint = ExtractJsonString(json, "llm_endpoint", g_config.llmEndpoint);
g_config.llmApiKey = DecryptString(ExtractJsonString(json, "llm_api_key", L""));
g_config.llmModel = ExtractJsonString(json, "llm_model", g_config.llmModel);
```

### SaveConfig (~L438)

```cpp
<< "  \"llm_endpoint\": \"" << EscapeJson(g_config.llmEndpoint) << "\",\n"
<< "  \"llm_api_key\": \"" << EscapeJson(EncryptString(g_config.llmApiKey)) << "\",\n"
<< "  \"llm_model\": \"" << EscapeJson(g_config.llmModel) << "\"\n"
```

### DPAPI 加密

```cpp
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")

std::wstring EncryptString(const std::wstring& plain);  // DPAPI + Base64
std::wstring DecryptString(const std::wstring& enc);    // Base64 + DPAPI
```

---

## 4. LLM API 调用

### 4.1 System Prompt

```
你是一个严格的语音识别纠错助手。你的唯一任务是修复语音识别（ASR）产生的明显错误。

规则：
1. 只修复明显的中文谐音错误（如"新情"→"心情"，"高心"→"高兴"）
2. 只修复英文技术术语被错误转为中文的情况（如"配森"→"Python"，"杰森"→"JSON"，"塞昆"→"Sequel"）
3. 绝对不要改写、润色、增加或删除任何看起来正确的字词
4. 绝对不要改变句子的语气、风格或表达方式
5. 如果输入文本看起来没有错误，必须原样输出，一个字都不改
6. 只输出纠正后的文本，不要输出任何解释、标注或额外内容
```

### 4.2 Request JSON

```json
{
  "model": "Qwen/Qwen3.5-4B",
  "messages": [
    {"role": "system", "content": "<system prompt above>"},
    {"role": "user", "content": "<ASR识别结果>"}
  ],
  "max_tokens": 1024,
  "temperature": 0.1
}
```

### 4.3 WinHTTP 异步请求

```cpp
void RefineWithLlm(const std::wstring& asrText) {
    // 1. WinHttpOpen(L"VoiceLLMASRInput", WINHTTP_ACCESS_TYPE_DEFAULT, ...)
    // 2. WinHttpConnect + WinHttpOpenRequest
    // 3. 构造 JSON body
    // 4. WinHttpSendRequest with WINHTTP_FLAG_ASYNC
    // 5. WinHttpSetStatusCallback 监听响应
    // 6. 回调中读取 response，解析 choices[0].message.content
    // 7. PostMessageW(g_mainWindow, kLlmResultMessage, ...)
}
```

### 4.4 超时

- WinHTTP 异步回调中设置 5 秒超时
- 超时后 fallback 到 ASR 原文

---

## 5. HUD Refining 状态

### 5.1 新增全局状态

```cpp
bool g_hudIsRefining = false;
```

### 5.2 ShowHud 调用点

```
StopRecordingSession():
  ShowHud("Recognizing... " + model)  // 已有

kAsrResultMessage 处理:
  if (postprocess=="llm" && api已配置 && !text.empty() && !isError):
    g_hudIsRefining = true
    ShowHud("Refining...")            // 新增
    RefineWithLlm(text)
  else:
    直接注入

kLlmResultMessage 处理:
  g_hudIsRefining = false
  ShowHud(result)
  注入文本
```

### 5.3 DrawHudDirect2D 修改

位置：`main.cpp` (~L1542)

```cpp
// 文字颜色：Refining 时灰色，否则白色
g_hudBrush->SetColor(g_hudIsRefining
    ? D2D1::ColorF(0.6f, 0.6f, 0.6f, 0.96f)   // 灰色
    : D2D1::ColorF(0.965f, 0.975f, 0.99f, 0.96f));  // 白色
```

---

## 6. 消息机制

### 6.1 新增消息

```cpp
constexpr UINT kLlmResultMessage = WM_APP + 4;
```

### 6.2 RecognizeAsync 修改

```cpp
void RecognizeAsync(const std::vector<BYTE>& pcm) {
    const Config config = g_config;
    std::thread([config, pcm]() {
        auto samples = PcmToFloat(pcm);
        std::wstring text = g_asrEngine.Recognize(samples, 16000, config);
        if (text.empty()) {
            text = L"(empty result)";
        }

        // 判断是否需要 LLM refine
        bool needLlm = (config.postprocess == L"llm")
                     && !config.llmEndpoint.empty()
                     && !config.llmApiKey.empty()
                     && !text.empty()
                     && text.rfind(L"ASR failed:", 0) != 0;

        if (needLlm) {
            // 先发 ASR 结果用于 HUD 显示
            PostMessageW(g_mainWindow, kAsrResultMessage, 1,  // wParam=1 表示"等待LLM"
                         reinterpret_cast<LPARAM>(new std::wstring(text)));
            // 异步调用 LLM
            RefineWithLlmAsync(text, config);
        } else {
            PostMessageW(g_mainWindow, kAsrResultMessage, 0,
                         reinterpret_cast<LPARAM>(new std::wstring(text)));
        }
    }).detach();
}
```

### 6.3 MainWndProc 处理

```cpp
case kAsrResultMessage: {
    auto result = unique_ptr<wstring>(reinterpret_cast<wstring*>(lParam));
    if (wParam == 1) {
        // ASR 完成，等待 LLM
        g_hudIsRefining = true;
        ShowHud(L"Refining...");
    } else {
        // 最终结果（无 LLM 或 LLM 完成）
        g_hudIsRefining = false;
        ShowHud(*result);
        SetClipboardText(*result);
        SendCtrlV();
        SetTimer(g_hudWindow, kHudHideTimer, 200, nullptr);
    }
    return 0;
}

case kLlmResultMessage: {
    auto result = unique_ptr<wstring>(reinterpret_cast<wstring*>(lParam));
    g_hudIsRefining = false;
    ShowHud(*result);
    SetClipboardText(*result);
    SendCtrlV();
    SetTimer(g_hudWindow, kHudHideTimer, 200, nullptr);
    return 0;
}
```

---

## 7. Test Connection

### 7.1 按钮处理

```cpp
case IDC_LLM_TEST:
    TestLlmConnection(hwnd);
    return 0;
```

### 7.2 TestLlmConnection

```cpp
void TestLlmConnection(HWND hwnd) {
    // 同步 WinHTTP，超时 5 秒
    // 发送: {"model":"...","messages":[{"role":"user","content":"ping"}],"max_tokens":5}
    // 成功: SetStatus "Connection OK (235 ms)"
    // 失败: SetStatus "Connection failed: timeout / 401 / ..."
}
```

---

## 8. 依赖

### main.cpp

```cpp
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
```

### build.bat

cl 命令添加 `winhttp.lib crypt32.lib`

---

## 9. 修改文件清单

| 文件 | 修改 |
|------|------|
| `main.cpp` | Config、LoadConfig、SaveConfig、Settings UI (LLM tab)、ShowSettingsPage、Load/SaveSettingsControls、TestLlmConnection、RefineWithLlmAsync、RecognizeAsync、MainWndProc (kAsrResultMessage/kLlmResultMessage)、DrawHudDirect2D (Refining 颜色)、DPAPI 加密 |
| `build.bat` | 添加 winhttp.lib crypt32.lib |

---

## 10. 实现顺序

1. 依赖：`#include` + `#pragma comment` + build.bat
2. Config 结构 + DPAPI 加密/解密
3. LoadConfig / SaveConfig 扩展
4. 控件 ID 常量
5. Settings WM_CREATE 创建 LLM tab 控件
6. g_llmControls + ShowSettingsPage
7. LoadSettingsControls / SaveSettingsControls
8. TestLlmConnection
9. RefineWithLlmAsync (WinHTTP 异步)
10. RecognizeAsync 修改（判断是否调 LLM）
11. MainWndProc 处理 kAsrResultMessage / kLlmResultMessage
12. DrawHudDirect2D Refining 颜色
13. 编译测试

---

## 11. 注意事项

- API Key 输入框必须能完全清空
- DPAPI 加密绑定当前 Windows 用户
- LLM 超时 5 秒，失败 fallback 到 ASR 原文
- System prompt 极其保守：只修谐音错误和英文术语中文化
- temperature 设 0.1 保证输出稳定
- Refining 时 HUD 文字变灰，区分识别中和纠错中
