# Debug Mode — 实时性能计时控制台

> 所属路线：`OPTIMIZATION_PLAN.md` → P0 §3 性能日志落地
>
> 状态：implemented（已完成）

## 概述

托盘右键菜单 **Debug Mode**（checkbox）。勾选时打开 CMD 控制台，每次识别实时打印各阶段耗时。

## 输出格式

```
-- 14:30:22  Rec 1.9s(60KB) -- WASAPI 48kHz->16kHz (耳机麦克风)
  Pipeline: VAD(FireRed) 57 | ASR 907 | Punct 2 | LLM 845 | Paste 2 = Total 1.8s
  ASR: "HELLO FOR YOU DOING。"
  LLM: "Hello, how are you doing?"
```

规则：
- `Rec` 在头行，不参与 Total
- `Total` = 各转录阶段之和（**不含录制时长**）
- 头行末尾显示音频后端信息（WASAPI 采样率+设备名 / waveIn）
- `ASR:` = LLM 纠正前原始 ASR 文本（仅 LLM 路径）
- `LLM:` = 经 LLM 纠正后的最终文本（LLM 路径）
- `OK:` = 最终文本（非 LLM 路径）
- 文本均带双引号包裹
- 无 VAD / 无 Punct 时跳过对应字段
- 去掉 `PCM to Float`（始终 0）、`ASR Pipeline`（冗余）、`End-to-End`（含录制）

### 各后端示例

**Local + 无 LLM**
```
-- 14:30:22  Rec 1.9s(60KB) -- WASAPI 48kHz->16kHz (耳机麦克风)
  Pipeline: VAD(FireRed) 57 | ASR 907 | Punct 2 | Paste 2 = Total 968ms
  OK: "今天天气不错"
```

**Local + LLM**
```
-- 14:30:22  Rec 1.9s(60KB) -- WASAPI 48kHz->16kHz (耳机麦克风)
  Pipeline: VAD(FireRed) 57 | ASR 907 | Punct 2 | LLM 845 | Paste 2 = Total 1.8s
  ASR: "HELLO FOR YOU DOING。"
  LLM: "Hello, how are you doing?"
```

**Baidu + LLM**
```
-- 14:30:22  Rec 1.4s(45KB) -- waveIn 16kHz (default)
  Pipeline: Baidu 367 | LLM 601 | Paste 3 = Total 971ms
  ASR: "还是不行啊！"
  LLM: "还是不行啊！"
```

**Volcengine + LLM**
```
-- 14:30:22  Rec 1.4s(~46KB) -- waveIn 16kHz (default)
  Pipeline: Volcengine 2203 | LLM 835 | Paste 3 = Total 3.0s
  ASR: "Come on, come on, baby."
  LLM: "Come on, come on, baby."
```

---

## 架构设计

### 数据流

```
StopRecordingSession → g_recordingMs, g_lastPcmBytes
RecognizeAsync worker thread:
  engine.cpp Recognize() → 赋值 g_vadMs / g_asrDecodeMs / g_punctMs / g_vadModelName
  cloud 后端 → 赋值 g_cloudApiMs
  LLM 路径 → 赋值 g_lastRawAsrText
  PostMessage → 主线程
kAsrResultMessage (wParam=0) 或 kLlmResultMessage:
  → 统一下游一次输出 header + pipeline + text lines
```

旧方案各阶段独立 printf，分散在 3 处代码。新方案全局变量汇聚，**下游单点输出**。

### 全局变量

```cpp
// main.cpp 定义
static double   g_vadMs = 0.0;          // VAD 阶段耗时
static double   g_asrDecodeMs = 0.0;    // ASR 解码耗时
static double   g_punctMs = 0.0;        // 标点耗时
static double   g_cloudApiMs = 0.0;     // 云端 API 耗时
static double   g_llmMs = 0.0;          // LLM 耗时
static std::wstring g_vadModelName;     // "FireRed" / "Silero"
static std::wstring g_lastRawAsrText;   // LLM 路径下的原始 ASR 文本
static size_t   g_lastPcmBytes = 0;     // PCM 字节数

// globals.h extern 声明
extern double g_vadMs;
extern double g_asrDecodeMs;
extern double g_punctMs;
extern double g_cloudApiMs;
extern double g_llmMs;
extern std::wstring g_vadModelName;
```

删除：`g_lastAsrTotalMs`（由 Pipeline 逐个字段展示取代）。

### 辅助函数

```cpp
static void DebugPrintHeader(double recMs, size_t pcmBytes) {
    SYSTEMTIME st; GetLocalTime(&st);
    printf("\n── %02d:%02d:%02d  Rec %.1fs(%zuKB) ────\n",
           st.wHour, st.wMinute, st.wSecond, recMs / 1000.0, pcmBytes / 1024);
}

static void DebugPrintTextLine(const wchar_t* prefix, const std::wstring& text) {
    if (text.empty()) return;
    std::wstring oneLine = text;
    for (auto& c : oneLine) if (c == L'\n' || c == L'\r') c = L' ';
    std::wstring line = std::wstring(L"  ") + prefix + L" \"" + oneLine + L"\"\n";
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD written;
    WriteConsoleW(hOut, line.c_str(), static_cast<DWORD>(line.size()), &written, nullptr);
}
```

---

## 逐文件变更

### 1. `src/globals.h`

Config struct 末尾的 `};` 之后，添加 extern 声明：

```cpp
extern double g_vadMs;
extern double g_asrDecodeMs;
extern double g_punctMs;
extern double g_cloudApiMs;
extern double g_llmMs;
extern std::wstring g_vadModelName;
```

### 2. `src/engine.cpp` — AsrEngine::Recognize()

VAD 分支：`printf(...)` → 赋值全局变量
ASR 分支：`printf(...)` → 赋值
Punct 分支：`printf(...)` → 赋值

```cpp
// VAD
double ms = tVad.ElapsedMs();
if (config.enableDebugMode) {
    g_vadMs = ms;
    g_vadModelName = (config.vadModel == L"firered") ? L"FireRed" : L"Silero";
}

// ASR
double ms = tAsr.ElapsedMs();
if (config.enableDebugMode) g_asrDecodeMs = ms;

// Punct
double ms = tPunct.ElapsedMs();
if (config.enableDebugMode) g_punctMs = ms;
```

### 3. `src/main.cpp` — 全局变量

新增全局变量区块（替换旧的 `g_lastAsrTotalMs`）：

```cpp
static double g_vadMs = 0.0;
static double g_asrDecodeMs = 0.0;
static double g_punctMs = 0.0;
static double g_cloudApiMs = 0.0;
static double g_llmMs = 0.0;
static std::wstring g_vadModelName;
static std::wstring g_lastRawAsrText;
static size_t g_lastPcmBytes = 0;
```

删除 `static double g_lastAsrTotalMs = 0.0;` 和 `static double g_lastLlmMs = 0.0;`（重命名为 `g_llmMs`）。

### 4. `src/main.cpp` — RecognizeAsync 线程

**本地 ASR 分支**：删除 Recording / PCM printf。新增保存 raw text 和 pcmBytes。

```cpp
std::thread([config, pcm]() {
    g_lastPcmBytes = pcm.size();

    HiResTimer tPcm;
    auto samples = PcmToFloat(pcm);

    HiResTimer tAsr;
    std::wstring text = g_asrEngine.Recognize(samples, 16000, config);
    // Recognize 内部已设置 g_vadMs / g_asrDecodeMs / g_punctMs / g_vadModelName

    if (text.empty()) text = L"(empty result)";

    bool needLlm = ...;
    if (needLlm) {
        g_lastRawAsrText = text;
        PostMessageW(..., 1, ...);
        RefineWithLlmAsync(text, config);
    } else {
        PostMessageW(..., 0, ...);
    }
}).detach();
```

**Baidu 分支**：删除 printf，改为赋值 `g_cloudApiMs`：

```cpp
std::thread([config, pcm]() {
    g_lastPcmBytes = pcm.size();
    ...
    std::wstring text = baidu_asr::Recognize(pcm, bcfg);
    g_cloudApiMs = tBaidu.ElapsedMs();
    ...
    if (needLlm) { g_lastRawAsrText = text; ... }
}).detach();
```

**Volcengine 线程**：删除 debug printf，改为赋值 `g_cloudApiMs`：

```cpp
g_volcThread = std::thread([vcfg, config]() {
    // 删除 if(config.enableDebugMode) printf(...) 行
    g_lastPcmBytes = 0; // Volcengine 流式，无精确 PCM 字节数
    ...
    g_cloudApiMs = static_cast<double>(GetTickCount64() - tTotal0);
    ...
}).detach();
```

### 5. `src/main.cpp` — kAsrResultMessage

**wParam=1（LLM 预重构）**：控制台不输出，仅 HUD。删除原 Raw ASR + ASR Pipeline 输出。

**wParam=0（非 LLM，最终结果）**：输出 header + pipeline + OK

```cpp
} else {
    g_hudIsRefining = false;
    ShowHud(text.empty() ? L"(empty result)" : text);
    if (!text.empty() && text.rfind(L"ASR failed:", 0) != 0) {
        HiResTimer tPaste;
        SetClipboardText(text);
        SendCtrlV();
        double pasteMs = tPaste.ElapsedMs();

        if (g_config.enableDebugMode) {
            DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

            if (g_config.asrBackend == L"local") {
                printf("  Pipeline: ");
                if (g_vadMs > 0) printf("VAD(%ls) %.0f | ", g_vadModelName.c_str(), g_vadMs);
                printf("ASR %.0f", g_asrDecodeMs);
                if (g_punctMs > 0) printf(" | Punct %.0f", g_punctMs);
                printf(" | Paste %.0f = Total %.0fms\n", pasteMs,
                       g_vadMs + g_asrDecodeMs + g_punctMs + pasteMs);
            } else {
                const char* backend = (g_config.asrBackend == L"baidu") ? "Baidu" : "Volcengine";
                printf("  Pipeline: %s %.0f | Paste %.0f = Total %.0fms\n",
                       backend, g_cloudApiMs, pasteMs, g_cloudApiMs + pasteMs);
            }

            DebugPrintTextLine(L"OK", text);
        }
    }
    ...
}
```

### 6. `src/main.cpp` — kLlmResultMessage

输出 header + pipeline（含 LLM）+ raw text + final text：

```cpp
case kLlmResultMessage: {
    ...
    if (!text.empty() && text.rfind(L"LLM failed:", 0) != 0) {
        HiResTimer tPaste;
        SetClipboardText(text);
        SendCtrlV();
        double pasteMs = tPaste.ElapsedMs();

        if (g_config.enableDebugMode) {
            DebugPrintHeader(g_recordingMs, g_lastPcmBytes);

            if (g_config.asrBackend == L"local") {
                printf("  Pipeline: ");
                if (g_vadMs > 0) printf("VAD(%ls) %.0f | ", g_vadModelName.c_str(), g_vadMs);
                printf("ASR %.0f", g_asrDecodeMs);
                if (g_punctMs > 0) printf(" | Punct %.0f", g_punctMs);
                printf(" | LLM %.0f | Paste %.0f = Total %.0fms\n",
                       g_llmMs, pasteMs,
                       g_vadMs + g_asrDecodeMs + g_punctMs + g_llmMs + pasteMs);
            } else {
                const char* backend = (g_config.asrBackend == L"baidu") ? "Baidu" : "Volcengine";
                printf("  Pipeline: %s %.0f | LLM %.0f | Paste %.0f = Total %.0fms\n",
                       backend, g_cloudApiMs, g_llmMs, pasteMs,
                       g_cloudApiMs + g_llmMs + pasteMs);
            }

            DebugPrintTextLine(L"ASR", g_lastRawAsrText);
            DebugPrintTextLine(L"LLM", text);
        }
    }
    ...
}
```

### 7. `src/main.cpp` — RefineWithLlmAsync

`g_lastLlmMs` → `g_llmMs`：

```cpp
g_llmMs = tLlm.ElapsedMs();
```

---

## 验收标准

- [ ] 输出符合新格式（`-- header --` + Pipeline + text lines）
- [ ] Total 不含录制时长
- [ ] `ASR:` 显示 raw ASR，`LLM:`/`OK:` 显示 final
- [ ] 无 `PCM to Float`、`ASR Pipeline`、`End-to-End` 行
- [ ] Local / Baidu / Volcengine 各 permutation 输出正确
- [ ] 无 VAD / 无 Punct 时对应字段省略
- [ ] 构建通过

## 风险

- `g_lastRawAsrText` 竞态：同 session 内不存在并发，极低概率
- VAD 关闭时 `g_vadMs` 需清零（Recognize 中 VAD 段被跳过时不赋值，依赖旧值会残留）
- cloud 后端无细分阶段，Pipeline 仅显示单一时长
