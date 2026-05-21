# 火山引擎（豆包）语音识别设置指南

> 🇬🇧 [English](../volcengine_asr_guide.md)

本文档详细说明 VoxType 中火山引擎（豆包）大模型语音识别的配置方法、各参数含义、API 获取方式及实现细节。

---

## 目录

- [1. 概述](#1-概述)
- [2. 获取 API 凭证](#2-获取-api-凭证)
- [3. Settings 各字段详解](#3-settings-各字段详解)
  - [3.1 API Key](#31-api-key)
  - [3.2 Model Version（模型版本）](#32-model-version模型版本)
  - [3.3 ASR Mode（识别模式）](#33-asr-mode识别模式)
  - [3.4 Language（语言）](#34-language语言)
  - [3.5 end_window_size / force_to_speech_time](#35-end_window_size--force-to_speech_time)
  - [3.6 功能开关](#36-功能开关)
  - [3.7 输入框上下文](#37-输入框上下文)
  - [3.8 热词与替换词](#38-热词与替换词)
  - [3.9 Extra Params](#39-extra-params)
  - [3.10 对话上下文](#310-对话上下文)
- [4. 实现架构](#4-实现架构)
  - [4.1 WebSocket 二进制协议](#41-websocket-二进制协议)
  - [4.2 请求流程](#42-请求流程)
  - [4.3 corpus 字段合并逻辑](#43-corpus-字段合并逻辑)
  - [4.4 context 序列化](#44-context-序列化)
- [5. 调试](#5-调试)
- [6. 参考文档](#6-参考文档)

---

## 1. 概述

VoxType 通过 WebSocket 协议连接火山引擎豆包大模型语音识别服务，支持三种识别模式和两种计费方式。音频格式固定为 16kHz 16bit 单声道 PCM，通过二进制帧协议实时发送。

**关键特性：**
- 支持热词词表（boosting_table）和替换词词表（correct_table）
- 支持对话上下文（context），利用历史识别结果提升准确率
- 支持语义顺滑（DDC）、二遍识别（nonstream）、输入框上下文等高级功能
- API Key 通过 DPAPI 加密存储在本地 `config.json` 中

---

## 2. 获取 API 凭证

### 2.1 创建应用

1. 访问 [火山引擎豆包语音控制台](https://console.volcengine.com/speech/app)
2. 点击「创建应用」，填写应用名称
3. 在应用详情中开通「大模型流式语音识别」服务

### 2.2 获取 API Key

**新版控制台**（VoxType 当前支持的方式）：

1. 进入应用详情页
2. 找到 **App Key**（即 `X-Api-Key` 的值）
3. 复制该值填入 VoxType Settings 的 `API Key` 字段

> ⚠️ 旧版控制台使用 `X-Api-App-Key` + `X-Api-Access-Key` 双密钥鉴权，VoxType 当前不支持此方式。

### 2.3 获取 Resource ID

Resource ID 对应计费方式，在控制台开通服务时选择：

| 显示名称 | Resource ID | 说明 |
|---------|-------------|------|
| Seed-ASR 2.0 (duration) | `volc.seedasr.sauc.duration` | 按时长计费（推荐） |
| Seed-ASR 2.0 (concurrent) | `volc.seedasr.sauc.concurrent` | 按并发计费 |
| BigASR 1.0 (duration) | `volc.bigasr.sauc.duration` | 旧版模型，按时长计费 |
| BigASR 1.0 (concurrent) | `volc.bigasr.sauc.concurrent` | 旧版模型，按并发计费 |

### 2.4 创建热词词表

1. 进入 [自学习平台 - 热词管理](https://console.volcengine.com/speech/hotword)
2. 点击「添加热词文件」
3. 输入词表名称和热词内容（每行一个热词，可带权重如 `火山语音|8`，默认权重 4）
4. 创建完成后复制**热词 ID**，填入 VoxType Settings 的 `Hotwords ID` 字段

**限制：**
- 每个应用最多 500 个词表
- 每个词表最多 2000 个热词
- 每个热词少于 10 个字
- 权重范围 1–10，默认 4
- 不支持标点符号（换行和空格除外）
- 阿拉伯数字需转为汉字（如 `A4L` → `A四L`）
- 一个请求只支持生效一张词表

---

## 3. Settings 各字段详解

### 3.1 API Key

火山引擎控制台获取的 **App Key**，用于 WebSocket 握手时的 `X-Api-Key` HTTP 头。该值通过 Windows DPAPI 加密后存储在 `config.json` 的 `volc_api_key` 字段中。

### 3.2 Model Version（模型版本）

下拉框选择，对应 `X-Api-Resource-Id` HTTP 头：

| 选项 | Resource ID | 说明 |
|------|-------------|------|
| Seed-ASR 2.0 (duration) | `volc.seedasr.sauc.duration` | 豆包 2.0 模型，按时长计费 |
| Seed-ASR 2.0 (concurrent) | `volc.seedasr.sauc.concurrent` | 豆包 2.0 模型，按并发计费 |
| BigASR 1.0 (duration) | `volc.bigasr.sauc.duration` | 旧版 1.0 模型，按时长计费 |
| BigASR 1.0 (concurrent) | `volc.bigasr.sauc.concurrent` | 旧版 1.0 模型，按并发计费 |

### 3.3 ASR Mode（识别模式）

| 模式 | WebSocket 路径 | 特点 |
|------|---------------|------|
| `bigmodel_nostream` | `/api/v3/sauc/bigmodel_nostream` | 流式输入模式，输入完成后一次性返回结果，**准确率最高**，支持 language 参数 |
| `bigmodel_async` | `/api/v3/sauc/bigmodel_async` | 双向流式优化版，仅在有新结果时返回数据包，**延迟最优** |
| `bigmodel` | `/api/v3/sauc/bigmodel` | 双向流式旧版，每包输入对应一包返回 |

**推荐：**
- 追求准确率 → `bigmodel_nostream`
- 追求实时性 → `bigmodel_async`
- `bigmodel` 为旧版链路，建议使用 `bigmodel_async` 替代

### 3.4 Language（语言）

仅 `bigmodel_nostream` 模式生效。其他模式下该参数不会被发送。

| 选项 | 代码 | 说明 |
|------|------|------|
| Auto | （空） | 自动识别中英文、上海话、闽南语、四川话、陕西话、粤语 |
| English | `en-US` | 英语 |
| Japanese | `ja-JP` | 日语 |
| Korean | `ko-KR` | 韩语 |
| French | `fr-FR` | 法语 |
| German | `de-DE` | 德语 |
| Spanish | `es-MX` | 西班牙语 |
| Portuguese | `pt-BR` | 葡萄牙语 |
| Indonesian | `id-ID` | 印尼语 |

### 3.5 end_window_size / force_to_speech_time

**end_window_size**（强制判停时间）：
- 默认 800ms，最小 200ms
- 静音时长超过该值时直接判停，输出 `definite` 标记
- 用于实时性要求较高的场景
- 配置该值后，语义分句（`vad_segment_duration`）失效

**force_to_speech_time**（强制语音时间）：
- 默认 10000ms（10秒），最小 1ms
- 音频时长超过该值后才根据静音时长判停
- 不配置时，前 10 秒不会判停
- 推荐与 `end_window_size` 配合使用，如设为 1000 可提前获得 definite 句子

### 3.6 功能开关

| 开关 | API 参数 | 说明 | 模式限制 |
|------|---------|------|---------|
| enable_ddc | `enable_ddc` | 语义顺滑，去除语气词和重复词 | 所有模式 |
| enable_nonstream | `enable_nonstream` | 二遍识别：流式 + 非流式重识别 | 仅 `bigmodel_async` |
| enable_music_fc | `enable_music_fc` | 音乐 function call | `bigmodel_nostream` 或 `bigmodel_async` + `enable_nonstream` |
| enable_poi_fc | `enable_poi_fc` | POI function call | `bigmodel_nostream` 或 `bigmodel_async` + `enable_nonstream` |

**enable_ddc（语义顺滑）**：建议开启，可自动去除「嗯」「啊」「就是」等语气词和重复词，提升输入文本质量。

**enable_nonstream（二遍识别）**：仅在 `bigmodel_async` 模式下可用。开启后，VAD 分句判停时使用非流式模型重新识别该分句音频，兼顾实时上屏（快）和最终准确率（准）。开启后默认启用 VAD 分句（800ms 判停）。

### 3.7 输入框上下文

勾选 `Read input field context` 后，录音开始时自动读取当前输入框的已有文本，作为 ASR 上下文发送给服务端，提升识别准确率。

**工作原理：**
1. 录音开始时，通过分层 Fallback 方案读取输入框文本
2. 读取方式按优先级依次尝试：WM_GETTEXT（Edit 控件）→ UIA Value → TextPattern → TextPattern2 → 父元素遍历 → ElementFromPoint → MSAA
3. 读取到的文本截取最后 200 字符，构建为 `corpus.context` 字段
4. 整个过程有 200ms 超时保护，不会阻塞录音启动
5. 自动跳过密码框（`UIA_IsPasswordPropertyId` 检测）

**上下文优先级：**
- 输入框有文本时：只发送输入框文本（`includeHistory=false`），避免重复
- 输入框无文本时：如果 `Use history as context` 也勾选了，用历史记录兜底
- 两个开关独立控制，互不依赖

**兼容性：**
- ✅ 记事本、Word、Chrome/Edge 输入框、VS Code、WPF 应用
- ❌ 微信/QQ（Qt 自绘控件，UIA/MSAA 不可见）
- ❌ Java 应用、游戏

### 3.8 热词与替换词

**Hotwords ID / Name**：
- `boosting_table_id`：自学习平台热词词表 ID
- `boosting_table_name`：热词词表名称
- ID 和 Name 二选一即可，同时填写时两个都会发送

**Correct ID / Name**：
- `correct_table_id`：自学习平台替换词词表 ID
- `correct_table_name`：替换词词表名称
- 用于将识别结果中的特定词自动替换为目标词（如专业术语纠正）

**所有模式均支持热词和替换词。**

### 3.9 Extra Params

点击 `Edit Params` 按钮打开编辑对话框，可输入额外的 `request` 级 JSON 参数。

**格式：** `"key1":"value1","key2":"value2"`（不需要外层花括号）

**预设模板：**
- `Filter`：填入 `"sensitive_words_filter":"system_reserved_filter"`（敏感词过滤）
- `Result`：填入 `"result_type":"single","vad_segment_duration":3000`（增量返回 + 语义切句静音阈值）

> ⚠️ `corpus` 键会被自动跳过，因为 `corpus` 由代码根据热词/替换词/上下文字段统一构建。不要在 Extra Params 中手动填入 `corpus`。

### 3.10 对话上下文

VoxType 提供两种独立的上下文来源，由两个开关分别控制：

| 开关 | 控制的上下文 | 说明 |
|------|------------|------|
| `Use history as context` | 历史识别结果 | 将最近 N 条识别结果作为对话上下文发送 |
| `Read input field context` | 输入框文本 | 读取当前输入框末尾 200 字符作为上下文（详见 §3.7） |

**上下文优先级：**
- 输入框有文本时：只发送输入框文本，不发历史记录（避免重复）
- 输入框无文本时：如果 `Use history as context` 已勾选，用历史记录兜底
- 窗口标题不再作为 context 发送（对 ASR 识别帮助极小）

**历史记录上下文工作原理：**
1. 每次识别完成后，结果被存入内存中的历史队列
2. 下次录音时（输入框无文本时），历史结果被构建为 `corpus.context` 字段
3. 格式为 `{"context_type":"dialog_ctx","context_data":[{"text":"..."}]}`
4. 该 JSON 对象会被序列化为字符串（内层引号转义）后作为 `context` 的值

**限制：**
- 双向流式模式（`bigmodel` / `bigmodel_async`）：100 tokens
- 流式输入模式（`bigmodel_nostream`）：5000 个词
- 上下文按从新到旧排列，超出 800 tokens 或 20 轮时自动截断

**历史条数：** 默认 3，范围 1–20。上下文数据不持久化，程序重启后从空历史开始。

---

## 4. 实现架构

### 4.1 WebSocket 二进制协议

VoxType 使用火山引擎自定义的二进制帧协议，而非 WebSocket 文本协议。帧格式：

```
| Byte 0        | Byte 1        | Byte 2        | Byte 3   |
| ver | hdrSize | msgType|flags | ser  | comp   | reserved |
| [sequence number - 4 bytes, optional]                          |
| [payload size - 4 bytes, big-endian]                           |
| [payload]                                                      |
```

**消息类型：**

| msgType | 含义 |
|---------|------|
| 0x01 | full client request（初始请求，包含 JSON 参数） |
| 0x02 | audio only request（纯音频数据） |
| 0x09 | full server response（服务端识别结果） |
| 0x0F | error response（服务端错误） |

**flags：**

| flags | 含义 |
|-------|------|
| 0x00 | 无 sequence number |
| 0x01 | 正 sequence number |
| 0x02 | 最后一包（负包），无 sequence |
| 0x03 | 最后一包（负包），有 sequence |

### 4.2 请求流程

```
1. WebSocket 握手
   GET /api/v3/sauc/{mode}
   Headers: X-Api-Key, X-Api-Resource-Id, X-Api-Connect-Id

2. 发送 full client request（msgType=0x01）
   Payload: JSON 格式的音频元数据和请求参数

3. 循环发送 audio only request（msgType=0x02）
   每 200ms 发送一包 PCM 数据（6400 bytes = 200ms @ 16kHz 16bit mono）

4. 发送最后一包（flags=0x02）
   空音频体，标记录音结束

5. 接收服务端响应（msgType=0x09）
   解析 JSON 中的 text 和 definite 字段

6. 关闭 WebSocket 连接
```

### 4.3 corpus 字段合并逻辑

所有 `corpus` 子字段在代码中合并为单个 JSON 对象，不再互斥：

```cpp
// volcengine_asr.h OpenSession()
std::string corpusParts;
if (!cfg.hotwordsId.empty())
    corpusParts += ",\"boosting_table_id\":\"...\"";
if (!cfg.hotwordsName.empty())
    corpusParts += ",\"boosting_table_name\":\"...\"";
if (!cfg.correctTableId.empty())
    corpusParts += ",\"correct_table_id\":\"...\"";
if (!cfg.correctTableName.empty())
    corpusParts += ",\"correct_table_name\":\"...\"";
if (!cfg.contextJson.empty())
    corpusParts += ",\"context\":\"...\"";  // 序列化后的字符串
if (!corpusParts.empty())
    requestJson += ",\"corpus\":{" + corpusParts.substr(1) + "}";
```

生成的 JSON 示例：
```json
"corpus": {
    "boosting_table_id": "abc123",
    "correct_table_id": "def456",
    "context": "{\"context_type\":\"dialog_ctx\",\"context_data\":[{\"text\":\"你好世界\"}]}"
}
```

### 4.4 context 序列化

`context` 字段的值必须是 **JSON 字符串**（内层引号转义），而非原始 JSON 对象。这是官方 API 的要求：

> context_data 字段按照从新到旧的顺序排列，传入需要序列化为 jsonstring（转义引号）

代码实现（`volcengine_asr.h`）：
```cpp
std::string ctxRaw = WideToUtf8(cfg.contextJson);
std::string ctxEscaped;
for (char c : ctxRaw) {
    if (c == '\\') ctxEscaped += "\\\\";
    else if (c == '"') ctxEscaped += "\\\"";
    // ... 其他转义
    else ctxEscaped += c;
}
corpusParts += ",\"context\":\"" + ctxEscaped + "\"";
```

---

## 5. 调试

VoxType 内置调试日志，输出到 `%TEMP%\volc_asr_debug.log`。

日志内容包括：
- WebSocket 连接过程
- 发送的帧头和数据 hex dump
- 服务端响应的帧解析
- 提取的识别文本和 definite 标记

如遇连接问题，可检查日志中的 `X-Tt-Logid`（服务端 logid）用于向火山引擎技术支持反馈。

---

## 6. 参考文档

| 文档 | 链接 |
|------|------|
| 大模型流式语音识别 API | https://www.volcengine.com/docs/6561/1354869 |
| 控制台使用 FAQ | https://www.volcengine.com/docs/6561/196768 |
| 热词管理 | https://www.volcengine.com/docs/6561/155739 |
| 替换词管理 | https://www.volcengine.com/docs/6561/1206007 |
| 自学习平台 API | https://www.volcengine.com/docs/6561/1742791 |
| 豆包语音控制台 | https://console.volcengine.com/speech/app |
| 热词管理控制台 | https://console.volcengine.com/speech/hotword |
