# AriaType 启发下的 VoxType 优化实施方案

> 状态：实施前重构版
> 日期：2026-06-29
> 研究来源：三轮 AriaType / VoxType 对照研究汇总
> AriaType 快照：`.cache/AriaType`，commit `4549f0546f8ec9aed884c99eb8c11100f668c134`，已确认等于 `origin/master`
> 目标：把研究结论整理成下一步可直接执行的实施路径，不记录重复评审过程。

---

## 1. 最终结论

AriaType 对 VoxType 最有价值的启发不是 Tauri/Rust/React 技术栈，而是它把“语音输入”做成了一条完整工作流：录音、识别、确定性纠错、可选 LLM 润色、历史记录、失败重试、快捷键 profile 和可诊断日志。

VoxType 不应该向 AriaType 重写。当前 Windows 原生 C++ 架构、WASAPI、CapsLock 长按、WeChat `WM_CHAR` 注入、IMM32、云端 ASR provider 深度，以及 `IAsrSession` / `IStreamingAsrSession` 分层都已经很适合项目目标。真正需要补的是识别之后的“可靠交付层”。

最终推荐路线：

```text
先让每次录音有独立身份和完整结果载荷
  -> 加固 LLM 输出，避免误改、误答、陈旧结果错贴
  -> 增加本地 terminology 确定性后处理
  -> 增加基于结果载荷的 history JSONL
  -> 再做 WAV retention / retry / provider probes
  -> 最后做第二热键、自动学习、模型状态和降噪
```

下一步实施应从“阶段 0”开始。不要先做 history、词库 UI 或第二热键，否则会把后续功能接到当前不够稳定的全局状态上。

---

## 2. 必须遵守的项目边界

这些边界来自 VoxType 当前代码和项目约束，实施时优先级高于 AriaType 的设计风格。

- 不重写技术栈。保留 Win32/C++ 单进程和现有 tray / Settings / HUD。
- 不绕过现有 `IAsrSession` / `IStreamingAsrSession` 抽象。
- 不把 LLM 设为默认纠错路径。
- 不让 UI 线程加载模型、等待 ASR 或等待云端 final。
- 不破坏 CapsLock 短按补发、长按录音、录音结束恢复原 Caps Lock 状态。
- 不破坏 WeChat `WM_CHAR` 注入路径。
- Settings 新增配置仍同步三处：`src/app/globals.h`、`src/audio/engine.cpp`、`src/ui/settings.cpp`。
- Settings 布局常量继续使用 `UiStyle`，不要硬编码魔法尺寸。
- 新用户数据优先进入 `%APPDATA%/VoxType`，不要继续扩大 app root 写入面。

---

## 3. 目标架构

### 3.1 目标数据流

```text
Hotkey press/release
  -> audio capture
  -> ASR session
  -> UtteranceContext / result payload
  -> deterministic post-STT pipeline
  -> optional LLM refine with guard
  -> paste / WM_CHAR injection
  -> history / diagnostics
```

关键变化是：ASR、LLM、debug、history 不再依赖一组“最近一次录音”的全局变量拼装结果，而是围绕同一个 `utterance_id` 传递结果载荷。

### 3.2 文本阶段定义

后续所有 debug、history、LLM guard、词库命中统计都使用这四个阶段名。

| 字段 | 含义 | 要求 |
| --- | --- | --- |
| `raw_asr_text` | provider/model 直接输出，尽量在 Normalize、标点和词库前 | 第一版拿不到可为空，但不能伪装 |
| `normalized_text` | `NormalizeAsrText()` 后，包含 no-speech/error 归一化 | 必填 |
| `deterministic_text` | 本地 terminology/correction 后、LLM 前 | 必填 |
| `final_text` | 最终粘贴文本；LLM 接受时为 LLM 输出，否则为 deterministic | 必填 |

当前代码里“raw”已经容易混淆：本地 ASR 在 `postprocess == itn|punct|llm` 时可能已经加标点，batch session 返回前也会 Normalize。因此实施时宁可让 `raw_asr_text` 暂时为空，也不要把已处理文本写成 raw。

### 3.3 结果载荷草案

```cpp
struct UtteranceResultPayload {
    uint64_t utteranceId = 0;
    std::wstring backend;

    std::wstring rawAsrText;
    std::wstring normalizedText;
    std::wstring deterministicText;
    std::wstring finalText;

    double recordingMs = 0.0;
    size_t pcmBytes = 0;
    double vadMs = 0.0;
    double asrMs = 0.0;
    double punctMs = 0.0;
    double llmMs = 0.0;
    double pasteMs = 0.0;

    bool needsLlm = false;
    bool llmAccepted = false;
    bool isError = false;
    std::wstring error;
    std::wstring decisionReason;
};
```

第一版仍可用 `PostMessageW(..., reinterpret_cast<LPARAM>(new UtteranceResultPayload(...)))` 传递，先不引入复杂事件系统。

---

## 4. 统一实施路径

只使用“阶段”这一套编号，不再混用旧版的多套优先级表达。每个阶段都以前一阶段为基础。

| 阶段 | 名称 | 目标 | 进入下一阶段的条件 |
| --- | --- | --- | --- |
| 阶段 0 | UtteranceContext + LLM Guard | 先把交付链路变可靠 | 连续录音、LLM 回退、debug 输出验证通过 |
| 阶段 1 | Terminology 本地确定性后处理 | 不靠 LLM 修正常见专名 | 手动词条可命中，误替换规则可验证 |
| 阶段 2 | History JSONL | 每次识别可追踪 | 成功/失败记录可信，不从全局变量拼装 |
| 阶段 3 | WAV Retention + Retry Tool | 失败可复现 | 音频默认不落盘，失败保留受控 |
| 阶段 4 | Provider Contract Probes | 云端协议可诊断 | probe 手动运行并能区分 auth/400/timeout |
| 阶段 5 | LLM 模板 + 第二热键 | 场景化输出入口 | 第一热键不回归，第二热键默认关闭 |
| 阶段 6 | Correction Learning Observer | 自动学习用户修正 | 默认关闭，只存短 pair，输入框读取可控 |
| 阶段 7 | 模型状态 / 降噪等增强 | 体验细化 | 前面核心链路稳定后再做 |

### 阶段 0：UtteranceContext + LLM Guard

目标：

- 每次录音生成递增 `utterance_id`。
- `kAsrResultMessage` / `kLlmResultMessage` 都传 result payload。
- LLM 结果返回时校验 `utterance_id`，旧结果不能粘贴到新焦点。
- LLM refine 输出必须经过 guard，失败时回退 deterministic 文本。

范围：

- 新增 result payload 类型，可以放在 `src/asr/asr_result.h` 或新增小头文件。
- 修改 `DispatchAsrFinalText()`，让它产出 payload，而不是只传 `std::wstring` 和 `lastRawAsrText`。
- 修改 `RefineWithLlmAsync()`，让 detached thread 持有 `utterance_id` 和 deterministic 文本。
- 修改 `kAsrResultMessage` / `kLlmResultMessage` handler，debug 和 paste 都消费 payload。
- 加固 `src/core/llm_refine.h`：endpoint、response parser、extra params、`<think>`、输出校验。

不做：

- 不做 history UI。
- 不做 terminology UI。
- 不做第二热键。
- 不迁移 `config.json`。

验收：

- 连续录音时，前一段延迟返回的 LLM 结果不会覆盖后一段 debug/history，也不会粘贴。
- LLM 返回 `<think>...</think>文本` 时只使用文本。
- LLM 返回不完整 `<think>` 时回退。
- LLM 不能回答用户听写的问题，只能修正文本。
- LLM 不能改坏 URL、路径、版本号、明显数字。
- Debug Mode 能看到 LLM accepted/rejected/reason。
- `.\build.bat` 通过。

### 阶段 1：Terminology 本地确定性后处理

目标：

- 用本地、可解释、可关闭的规则修正常见专名和术语。
- 让无 LLM 模式也能改善识别结果。

数据文件：

```text
%APPDATA%/VoxType/terminology.txt
```

格式：

```text
# wrong => corrected
火山阴晴 => 火山引擎
open ai api => OpenAI API
```

规则：

- UTF-8 with BOM / no BOM 都接受。
- 空行和 `#` 注释跳过。
- 每行只按第一个 `=>` 切分。
- wrong/corrected trim 后不能为空。
- exact replacement。
- ASCII wrong 需要 word boundary。
- 单字 CJK 不自动替换。
- 同一个 wrong 对应多个 corrected 时跳过。
- 每次最多应用 50 条，按 wrong 长度降序。
- 命中数和跳过原因进入 Debug Mode。

落点：

- 新增 `src/core/post_stt_pipeline.h/.cpp` 或 `src/asr/post_stt_pipeline.h/.cpp`。
- 在 `NormalizeAsrText()` 之后、`ShouldRunLlmRefine()` 之前调用。
- Settings 第一版只增加 `Open Terminology File` 和 `Reload Terminology`。

不做：

- 不做复杂 JSON 数组。
- 不做表格 UI。
- 不做拼音近音和模糊匹配。
- 不做自动学习。
- 不自动下发给火山 `corpus` / Qwen hints。

验收：

- 手动 mapping 可修正 ASR 输出。
- LLM 开启时，terminology 在 LLM 前生效。
- 单字 CJK、冲突 mapping、ASCII 子串误替换都能被拦住。
- Settings 高 DPI 不裁切。

### 阶段 2：History JSONL

目标：

- 每次识别可追踪、可对比、可复现问题。
- 为后续 retry 和 correction learning 提供可信数据基础。

数据文件：

```text
%APPDATA%/VoxType/history/history_YYYYMMDD.jsonl
```

字段草案：

```json
{
  "timestamp": "2026-06-29T12:00:00+08:00",
  "utterance_id": 12345,
  "backend": "doubao_ime",
  "status": "success",
  "raw_asr_text": "",
  "normalized_text": "火山阴晴很好用",
  "deterministic_text": "火山引擎很好用",
  "final_text": "火山引擎很好用",
  "recording_ms": 3200,
  "pcm_bytes": 102400,
  "vad_ms": 0,
  "asr_ms": 780,
  "punct_ms": 0,
  "llm_ms": 0,
  "paste_ms": 20,
  "llm_accepted": false,
  "decision_reason": "",
  "error": "",
  "audio_path": ""
}
```

范围：

- 成功和失败都写 metadata。
- history 消费阶段 0 payload，不从 `g_lastRawAsrText` / `g_recordingMs` 等全局变量事后拼装。
- 第一版不保存音频。
- Debug Mode 输出 history 写入路径和错误。

隐私策略：

- 可跟随 Debug Mode，或新增 `enable_history_log` 默认关闭。
- 如果默认开启，必须提供 `Open History Folder` 和 `Clear History`。

验收：

- 本地、百度、Qwen、火山、MiMo、Doubao IME 成功和失败路径都有合理记录。
- no speech、too short、timeout、auth error 能区分。
- 不影响粘贴和 HUD。

### 阶段 3：WAV Retention + Retry Tool

目标：

- 让失败录音可复现。
- 保留隐私边界，不默认保存所有音频。

配置：

```text
audio_retention_mode = off | failed_only | debug_all
```

范围：

- 新增 `WritePcm16Mono16kWav(path, pcm)`。
- 失败音频写入 `%APPDATA%/VoxType/recordings/failed/*.wav`。
- 自动清理：最大保存天数、最大目录大小。
- retry 工具优先支持 local / batch provider。

不做：

- 不在 `off` 模式落音频。
- 不第一版支持 streaming provider 的 WAV 实时重放。
- 不把 retry 直接塞进 Settings 大 UI。

验收：

- `off` 模式没有音频文件。
- `failed_only` 只保存失败音频。
- retry 能对同一个 failed wav 重跑 local / batch 路径。

### 阶段 4：Provider Contract Probes

目标：

- 用手动诊断工具检查云端 provider 请求格式是否仍符合契约。

范围：

```text
tools/diagnostics/probe_qwen_contract.bat
tools/diagnostics/probe_volcengine_contract.bat
tools/diagnostics/probe_baidu_contract.bat
```

原则：

- 使用 mock credentials。
- 预期 401/403 表示 endpoint/header/body 大体通。
- 400/404 通常表示请求结构或 URL 错。
- connect timeout 表示网络/代理/服务不可达。
- 不加入默认 `build.bat`。

### 阶段 5：LLM 模板 + 第二热键

目标：

- 把“原样听写”和“润色/模板输出”做成两个入口，而不是让用户频繁打开 Settings 切换。

前置条件：

- 阶段 0 LLM Guard 已稳定。
- 阶段 1 terminology 已稳定。

最小设计：

- 第一热键保持 `Config.hotkey`，默认 `CapsLock`。
- 第二热键新增 `secondary_hotkey_enabled=false`。
- 第二热键第一版只支持 hold。
- 第二热键只覆盖 `postprocess/template`，不覆盖 ASR backend/provider。
- 不默认使用 CapsLock 组合键。

验收：

- 第一热键行为完全不变。
- CapsLock 短按/长按不回归。
- Settings 打开时仍不拦截录音快捷键。
- 第二热键可走指定 LLM 模板。

### 阶段 6：Correction Learning Observer

目标：

- 用户修正过的短词条，在明确允许后成为后续本地 correction。

前置条件：

- history 已能记录文本阶段差异。
- input context 读取有更可靠的取消/并发模型。
- 隐私开关和清理入口明确。

范围：

- 默认关闭。
- paste 后短时 observer。
- 只提取短 pair。
- frequency 达阈值后才自动应用。
- 不存完整输入框内容。

先不要做的原因：

- `GetInputFieldContext()` 当前是 200ms UIA worker + detached thread，读不到或超时很常见。
- 微信、浏览器、IDE、Qt 自定义控件兼容性不稳定。
- 自动学习涉及隐私，不能早于手动 terminology。

### 阶段 7：模型状态、降噪和其他增强

模型状态：

- VoxType 已有 `Download Local Model` 和 `RunModelDownloader()`。
- 后续只补模型完整性检查、缺文件提示、`Open Model Folder`。

降噪：

- RNNoise 或其他降噪放到后期。
- 默认 off。
- 只作为显式选项，不影响已稳定的 VAD / provider 路径。

OCR 上下文：

- 暂不做。
- 先把现有 `GetInputFieldContext()` 用于 terminology ranking 和 LLM hint。

---

## 5. 阶段 0 详细设计

阶段 0 是下一步真正要动手的部分，本节比后续阶段更具体。

### 5.1 当前问题

现有代码中，ASR 和 LLM 结果之间通过全局变量和裸文本消息衔接：

- `src/app/main.cpp` 中有 `g_lastRawAsrText`、`g_recordingMs`、`g_lastPcmBytes`、`g_llmMs`。
- `RefineWithLlmAsync()` 启动 detached thread，完成后只投递 `std::wstring`。
- `kLlmResultMessage` handler 收到结果后直接粘贴，没有校验这是否还是当前录音。

这在单次录音时能工作，但连续录音时存在陈旧 LLM 结果错贴、debug 错配、history 错配风险。

### 5.2 建议改动顺序

1. 新增 `utterance_id` 生成器。
2. 新增 result payload 类型。
3. 修改 ASR final dispatch，让 `kAsrResultMessage` 传 payload。
4. 修改 LLM async，让 `kLlmResultMessage` 传同一个 `utterance_id` 的 payload 或 decision payload。
5. 在 LLM result handler 中丢弃 stale result。
6. 再接 LLM Guard 输出校验。
7. 最后整理 Debug Mode 输出。

### 5.3 LLM Guard 详细要求

当前 `src/core/llm_refine.h` 需要一起修正：

- `ParseEndpoint()` 不应无条件追加 `/chat/completions`。如果用户已经填完整路径，不再追加。
- `ParseResponse()` 不应只找第一个 `"content"`，应读取 OpenAI-compatible 的 `choices[0].message.content`，至少使用可靠 JSON string decoder。
- `BuildRequestBody()` 不应让 `extraParams` 覆盖 `model`、`messages`、`stream`、`tools` 等关键语义。
- `Refine()` 不应只返回字符串，应返回 decision。

Decision 草案：

```cpp
struct LlmRefineDecision {
    bool accepted = false;
    std::wstring output;
    std::wstring fallbackText;
    std::wstring reason;
    DWORD httpStatus = 0;
    double elapsedMs = 0.0;
};
```

输出校验：

- 完整 `<think>...</think>` 可剥离。
- 不完整 `<think>` 直接拒绝。
- 输出为空拒绝。
- 输出长度显著异常拒绝。
- URL、Windows 路径、版本号、明显数字被改坏时拒绝。
- 输入是问句时，LLM 不能回答问题，只能修正听写文本。

---

## 6. 阶段 1 详细设计

### 6.1 为什么不是先做自动学习

AriaType 的 correction memory 很有价值，但 VoxType 当前不适合一开始就做 observer。原因是 Windows 目标控件复杂，`GetInputFieldContext()` 目前只能 best-effort 读取。先做手动 terminology，可以用更小风险解决专名问题。

### 6.2 文件格式

使用 `terminology.txt`，而不是 `terminology.json`：

- 当前项目没有通用 JSON 库。
- 现有手写 JSON 提取器适合配置，不适合用户手写复杂数组。
- 行文本更容易编辑、恢复、定位错误。

后续如果需要 metadata，再迁移到 JSONL，而不是单个大 JSON。

### 6.3 Settings 入口

第一版只做：

- `Open Terminology File`
- `Reload Terminology`

不要把词条表格塞进当前 Settings。现有 Settings 已有五个 tab，Recognition 和 Cloud ASR 内容密度高，高 DPI 裁切风险比表格收益更大。

---

## 7. 风险控制

| 风险 | 影响 | 控制方式 |
| --- | --- | --- |
| 陈旧 LLM 结果错贴 | 文本进入错误窗口 | `utterance_id` 校验，stale result 不粘贴 |
| LLM 改写用户意思 | 语义错误 | guard 校验，失败回退 deterministic 文本 |
| terminology 误替换 | 改坏专名或普通词 | exact replacement、word boundary、单字 CJK 跳过、冲突跳过 |
| history 保存敏感内容 | 本地隐私风险 | 开关、AppData 路径、清理入口、音频默认不保存 |
| WAV retention 占磁盘 | 隐私和空间问题 | 默认 off、failed_only、天数和大小上限 |
| provider probe 触网 | 网络副作用 | 手动运行、mock credentials、不进 build |
| 第二热键破坏 CapsLock | 热键回归 | 默认关闭、hold-only、不改第一热键状态机 |
| 自动学习侵犯隐私 | 用户不希望被观察 | 默认关闭、只存短 pair、不存全文 |
| AGPL 许可证 | 法务风险 | 只借鉴设计，不复制 AriaType 代码或 prompt |

---

## 8. 验证清单

### 阶段 0 验证

- `.\build.bat` 通过。
- 连续两次录音，第一次 LLM 延迟返回不会错贴。
- LLM 失败、空输出、不完整 `<think>` 都回退。
- Debug Mode 输出 ASR / deterministic / LLM / reason。
- 本地 ASR、Qwen、火山、百度、MiMo、Doubao IME 至少各走一次成功或可解释失败路径。

### 阶段 1 验证

- 中文短语 replacement 命中。
- 英文 acronym replacement 命中。
- ASCII word boundary 生效。
- 单字 CJK 不替换。
- 冲突 mapping 跳过。
- LLM 前生效。
- Settings 高 DPI 不裁切。

### 回归验证

- CapsLock 短按仍切换大小写。
- CapsLock 长按仍录音。
- 录音结束恢复原 Caps Lock 状态。
- Settings 打开时不拦截录音快捷键。
- WeChat 仍走 `WM_CHAR` 注入。
- HUD 高 DPI 不重叠。
- WASAPI 再次录音不卡死。

---

## 9. 暂不实施的事项

- 不做 Tauri/Rust 重写。
- 不复制 AriaType 代码或 prompt。
- 不默认启用 LLM。
- 不做流式逐字注入到目标 app。
- 不优先做 OCR 上下文。
- 不优先引入 RNNoise。
- 不把本地 terminology、火山 `corpus`、Qwen hints、LLM glossary 一次性做成复杂统一系统。
- 不第一版做自动 correction learning observer。

---

## 10. 仍需决定的问题

这些问题不阻塞阶段 0，但会影响后续阶段。

1. history 默认跟随 Debug Mode，还是新增 `enable_history_log` 默认关闭？
2. 如果 history 默认开启，Settings 里的 Open/Clear 入口放 General 还是 Diagnostics？
3. failed wav 默认保留几天、目录大小上限是多少？
4. 第二热键推荐默认键位是什么，如何避开中文输入法和常见 IDE 快捷键？
5. LLM Guard 的变化幅度阈值是否按 preset 固定，还是做高级配置？
6. terminology 是否需要 app/process/title scope？如果需要，是否升级到 JSONL？

---

## 11. AriaType 可借鉴点汇总

### 11.1 值得借鉴

- Correction memory：本地确定性纠错优先于 LLM，频次阈值、冲突过滤、word boundary、CJK 单字保护都值得参考。
- History + retry：每次识别都保留 metadata，失败音频可受控重试。
- Multi-shortcut profile：原样听写和润色输出分成两个入口，比设置项切换更适合语音输入。
- Recording lifecycle guard：start 过程失败时要回滚半初始化状态。
- Engine contract probe：云端 provider 可用 mock credentials 验证请求格式。
- task_id / logs：每条录音链路都要能在日志中串起来。

### 11.2 不适合直接迁移

- Tauri/Rust/React 技术栈不适合当前 VoxType。
- AriaType 的 post-delivery observer 依赖更稳定的输入框读取，VoxType 暂时不能直接照搬。
- AriaType 的 retry 建立在已保存音频文件上，VoxType 要先补受控 WAV retention。
- AriaType 的多快捷键 runtime 不能直接套到当前 CapsLock 状态机上。
- RNNoise / OCR 等增强不是当前最大瓶颈。

---

## 12. 删减与保留对照

这次重构删除的是“研究过程、重复评审、已被后续结论修正的旧建议”，不是删除决策依据。下面是原稿重要信息在新版中的位置。

| 原研究内容 | 新版保留位置 | 处理方式 |
| --- | --- | --- |
| AriaType 产品定位、不是技术栈迁移 | `1. 最终结论`、`11. AriaType 可借鉴点汇总` | 压缩为结论 |
| VoxType 与 AriaType 架构差异 | `2. 必须遵守的项目边界`、`3. 目标架构` | 转为实施边界 |
| correction memory 规则 | `阶段 1`、`阶段 6`、`6. 阶段 1 详细设计` | 手动 terminology 先落地，自动学习后置 |
| LLM 不应默认纠错 | `2. 必须遵守的项目边界`、`阶段 0`、`9. 暂不实施的事项` | 保留为硬约束 |
| 多快捷键 profile | `阶段 5`、`11.1 值得借鉴` | 后置，避免影响 CapsLock |
| history + retry | `阶段 2`、`阶段 3` | 拆成 metadata 先行、音频保留后行 |
| provider contract probe | `阶段 4` | 保留为手动诊断工具 |
| recording lifecycle guard | `11.1 值得借鉴` | 暂不作为近期阶段，后续可并入录音状态机整理 |
| RNNoise / 音频前处理 | `阶段 7`、`9. 暂不实施的事项` | 后置，默认 off |
| input context / OCR | `阶段 6`、`阶段 7`、`9. 暂不实施的事项` | OCR 后置，现有 context 先服务 LLM/terminology |
| 模型管理 UI | `阶段 7` | 只补完整性/状态，不重做下载器 |
| AGPL 许可证风险 | `7. 风险控制`、`13.1 AriaType` | 保留为风险约束 |
| 旧版 P/Phase 优先级 | `4. 统一实施路径` | 重写为单一阶段路径 |
| 三轮评审发现的 LLM 陈旧结果风险 | `阶段 0`、`5. 阶段 0 详细设计` | 提升为下一步首要实施项 |
| JSON parser 风险与 terminology 格式 | `阶段 1`、`6.2 文件格式` | 改成 `terminology.txt`，JSONL 作为未来升级 |
| AppData 路径与日志隐私 | `2. 必须遵守的项目边界`、`阶段 2`、`13.3 关键 VoxType 现状` | 保留为路径策略 |

明确删掉的内容：

- 三轮评审的时间顺序叙述。
- 已被后续结论推翻的旧优先级。
- 同一观点的多次重复论证。
- 过早的 UI 细节草案，例如词库表格和大文本框。
- 不准备实施的 AriaType 技术栈细节。

如果后续需要追溯原始研究证据，应看本节映射和最后的信息来源，而不是把旧评审过程重新放回正文。

---

## 13. 信息来源

### 13.1 AriaType

- GitHub：`https://github.com/joe223/AriaType`
- 本地缓存：`.cache/AriaType`
- commit：`4549f0546f8ec9aed884c99eb8c11100f668c134`
- 许可证注意：AriaType 为 AGPL-3.0-only。本方案只借鉴产品和架构思想，不复制实现代码。

重点参考：

- `README.md`
- `context/architecture/data-flow.md`
- `context/spec/engine-api-contract.md`
- `context/spec/hotkey.md`
- `context/feat/correction-learning/0.1.0/prd/erd.md`
- `apps/desktop/src-tauri/src/stt_engine/traits.rs`
- `apps/desktop/src-tauri/src/stt_engine/unified_manager.rs`
- `apps/desktop/src-tauri/src/audio/stream_processor.rs`
- `apps/desktop/src-tauri/src/services/recording_lifecycle.rs`
- `apps/desktop/src-tauri/src/correction_learning/diff.rs`
- `apps/desktop/src-tauri/src/correction_learning/storage.rs`
- `apps/desktop/src-tauri/src/history/models.rs`
- `apps/desktop/src-tauri/src/services/retry_transcription.rs`
- `apps/desktop/src-tauri/src/shortcut/profile_types.rs`
- `apps/desktop/src-tauri/src/polish_engine/templates.rs`

### 13.2 VoxType 对照文件

- `src/app/main.cpp`
- `src/app/globals.h`
- `src/asr/asr_result.cpp`
- `src/asr/asr_dispatcher.cpp`
- `src/asr/asr_session.cpp`
- `src/asr/asr_session.h`
- `src/asr/asr_streaming_session.h`
- `src/asr/asr_streaming_session_base.h`
- `src/audio/engine.cpp`
- `src/core/llm_refine.h`
- `src/core/input_context.h`
- `src/ui/hotkey.cpp`
- `src/ui/settings.cpp`
- `ARCHITECTURE.md`
- `CHANGELOG.md`

### 13.3 关键 VoxType 现状

- `RefineWithLlmAsync()` 当前 detached thread 只回传 `std::wstring`。
- `kAsrResultMessage` / `kLlmResultMessage` 当前没有携带录音身份。
- Debug 和粘贴路径依赖 `g_lastRawAsrText`、`g_recordingMs`、`g_lastPcmBytes`、`g_llmMs`。
- `ParseEndpoint()` 当前有重复追加 `/chat/completions` 的风险。
- `ParseResponse()` 当前不是结构化 JSON 解析，且不完整支持 Unicode escape。
- `WriteLlmLog()` 当前写入 app root 的 `log` 目录；新用户数据应改走 `AppDataDir()`。
- Settings 当前已有 General / Recognition / Cloud ASR / LLM / LLM Prompt 五个 tab，第一版不适合再塞复杂词库表格。
