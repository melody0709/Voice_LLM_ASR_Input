# Qwen Audio 3.0 ASR 集成计划

> 状态：已实施；北京 Workspace 的 HTTP/WSS 正常识别、streaming partial 和静音路径已由真实运行载荷确认。网络中断/设备热插拔仍属于需要现场注入的破坏性 smoke test。
>
> 创建日期：2026-08-10
>
> 目标版本：v0.9.24

## 1. 目标与结论

将阿里云百炼的 Qwen Audio 3.0 ASR 系列集成到 VoxType 的现有 **Qwen ASR（DashScope）** 入口中。

用户界面仍只保留一个顶层 Provider：

```text
Qwen ASR (DashScope)
```

Provider 内部根据 Model 下拉菜单选择不同的协议适配器：

```text
qwen3-asr-flash-realtime
    -> 现有 DashScope Realtime WebSocket 客户端

qwen-audio-3.0-asr-flash
    -> 新增 DashScope Multimodal Generation HTTP Batch 客户端

qwen-audio-3.0-asr-flash-streaming
    -> 本轮新增 DashScope Fun-ASR Realtime WebSocket 客户端
```

本轮不把任意新模型当作现有实时客户端的“换模型名”。三种模型共用 Qwen Provider、API Key、录音和结果管线，但分别使用协议适配器：旧 realtime 继续走原有 OpenAI-style Realtime WebSocket；`qwen-audio-3.0-asr-flash` 走整段音频 HTTP；`qwen-audio-3.0-asr-flash-streaming` 走新的 `run-task` / 二进制 PCM / `finish-task` WebSocket。

新安装默认选中 `qwen-audio-3.0-asr-flash-streaming`，并将它排在 Model 下拉菜单第一位。已有配置文件继续保留用户原来的模型和协议，避免升级后突然改变行为。`qwen3-asr-flash-realtime` 仍保留为兼容选项。

## 2. 官方接口事实

### 2.1 本轮目标模型

目标模型：

```text
qwen-audio-3.0-asr-flash
```

调用类型：整段音频 HTTP 请求。音频输入不是录音期间逐 chunk 上传；`SSE` 只控制提交完整音频后的响应是否分段返回。

请求方法：`POST`。

服务路径：

```text
/api/v1/services/aigc/multimodal-generation/generation
```

请求头（本轮默认使用非 SSE final 响应）：

```text
Authorization: Bearer <DASHSCOPE_API_KEY>
Content-Type: application/json
X-DashScope-SSE: disable
```

请求体的核心结构：

```json
{
  "model": "qwen-audio-3.0-asr-flash",
  "input": {
    "messages": [
      {
        "role": "user",
        "content": [
          {
            "type": "input_audio",
            "input_audio": {
              "data": "<audio URL or supported Base64 Data URI>"
            }
          }
        ]
      }
    ]
  },
  "parameters": {
    "format": "wav",
    "sample_rate": 16000,
    "language_hints": ["zh"]
  }
}
```

官方接口支持音频 URL 或 Base64 Data URI；客户端优先使用 Base64 Data URI，避免为了短句识别引入临时公网文件上传。用户录音在 VoxType 中已经是 16 kHz、16-bit、mono PCM，客户端需要在请求前封装为 WAV。HTTP 模型只发送该 profile 明确支持的 `format`、`sample_rate`、`language_hints`、`vocabulary_id` 和 `vocabulary` 字段；语义标点、句间静音、多阈值、heartbeat、噪声阈值属于 streaming-only，不能发送到 HTTP profile。不能把旧 realtime 的 `language` 字符串字段原样复用。

`X-DashScope-SSE: enable` 可以让 HTTP 响应以 SSE 分段返回，但它仍然是“松开后整体提交、服务端处理期间分段返回”，不是录音期间的输入流。本轮默认 `disable`，先保证 final 解析稳定；后续可增加 HTTP response streaming，把 release 后的中间文本映射到 HUD。

### 2.2 现有模型

当前实现默认配置：

```text
Model:    qwen3-asr-flash-realtime
Base URL: wss://dashscope.aliyuncs.com/api-ws/v1/realtime
```

现有客户端使用：

- WebSocket Upgrade；
- `session.update`；
- `input_audio_buffer.append`；
- `input_audio_buffer.commit`；
- `session.finish`；
- partial/final/session finished 事件；
- 独立 drain thread 和 replay retry。

这些行为必须保持不变，不能为新模型重写或破坏。

### 2.3 新版 Audio 3.0 Streaming 模型

目标模型：

```text
qwen-audio-3.0-asr-flash-streaming
```

官方实时协议与旧 `qwen3-asr-flash-realtime` 不同：

- Endpoint 使用工作空间地域 WSS 的 `/api-ws/v1/inference` 路径；
- 建立 WebSocket 后先发送 `run-task` JSON；
- 音频以二进制 WebSocket message 发送，默认 16 kHz / mono / PCM；
- 录音结束后发送 `finish-task`；
- 接收 `task-started`、`result-generated`、`task-finished` 等任务事件；
- `language_hints` 是数组，最多支持多个语言提示；
- 可选参数包括词汇表/热词、语义标点、句间静音、噪声阈值等，只有本模型 profile 才能发送；
- 文档中的连接复用属于独立生命周期能力，本轮先做单录音 session，确认稳定后再做跨录音 prewarm/reuse。

因此新 streaming 模型需要新的 `qwen_audio_streaming.*` client 和 `IStreamingAsrSession` 外壳，不能直接调用当前 `qwen_asr::RealtimeClient`。

### 2.4 地域和 API Key

- `qwenApiKey` 继续作为三个 Qwen 模型的共享 API Key 字段。
- HTTP Base URL、旧 realtime WSS Base URL、新 Audio streaming WSS Base URL 必须分开保存；不能用一个字符串同时表达三种协议/路径。
- 默认实时模型继续使用现有 DashScope 通用 WSS 地址。
- 新 HTTP 模型和新 streaming 模型使用同一北京工作空间的地域域名，但路径不同：HTTP 为 `/api/v1/services/aigc/multimodal-generation/generation`，streaming 为 `/api-ws/v1/inference`。
- 本项目不提供地域切换项，Audio 3 默认固定使用已开通权限的北京 Workspace 域名：`llm-c6rtn7zy4nw0u39k.cn-beijing.maas.aliyuncs.com`；API Key 仍由用户配置，不写入源码。
- 本次按用户确认使用已开通权限的北京 Workspace；默认值固定为该 Workspace 的 HTTP/WSS 两个官方路径。API Key 不写入源码，Endpoint 仍可在 Settings 中覆盖并分别保存。
- 保存和测试连接时，不得把旧通用域名、北京工作空间域名或不同 Workspace ID 交叉使用。
- 用户手动修改 Base URL 后，切换模型不应无条件覆盖自定义地址。

## 3. 用户体验设计

### 3.1 Model 下拉菜单

当前 Model 普通 Edit 控件改为 ComboBox，至少包含：

1. `qwen-audio-3.0-asr-flash-streaming`
2. `qwen-audio-3.0-asr-flash`
3. `qwen3-asr-flash-realtime`

新安装默认选中第 1 项 `qwen-audio-3.0-asr-flash-streaming`；加载已有配置时按保存的 `qwen_model` 恢复，不因升级自动切换。

### 3.2 参数联动规则

| 当前模型 | 传输模式 | Endpoint profile | Language | Chunk ms | Audio 3 参数 |
|---|---|---|---|---|---|
| `qwen3-asr-flash-realtime` | 旧 Realtime WebSocket | 现有 `.../api-ws/v1/realtime` | 单语言，映射旧 `language` | 显示、启用 | 不发送 |
| `qwen-audio-3.0-asr-flash` | HTTP Batch | 工作空间 HTTPS `.../generation` | 单语言转换为 `language_hints` 单元素数组，Auto 省略 | 隐藏或禁用 | 显示 HTTP 专属高级项 |
| `qwen-audio-3.0-asr-flash-streaming` | Audio 3 Realtime WebSocket | 北京工作空间 WSS `.../api-ws/v1/inference` | 逗号/分号分隔，最多 4 个，`fil` 映射 `tl`，Auto 省略 | 显示、启用 | 显示 streaming 专属高级项 |

切换模型时：

- 自动切换内部 transport profile；
- 根据 profile 选择 HTTP、旧 WSS、新 Audio WSS 三种 Endpoint；
- 保留 API Key；
- 对不适用的 `Chunk ms` 隐藏或禁用；HTTP 的 Chunk ms 不参与请求；
- HTTP 模型显示“松开后提交完整音频”；Audio streaming 模型显示“按住期间持续发送 PCM，支持 partial”；
- 现有 Language 下拉先保留单选 UI；对 Audio 3 模型转换为 `language_hints: [code]`，Auto 时不发送。后续若需要多语言提示，再单独增加多选控件，不把逗号字符串塞入旧字段；
- Audio 3 高级项只在对应模型显示，例如 vocabulary ID、热词、语义标点、句间静音等；旧 realtime 模型不发送这些字段；
- 不改变用户已有的语言选择，除非新模型不接受该语言值；
- 保留用户自定义 Endpoint，除非当前 URL 仍等于另一个 profile 的内置默认值；
- 如果用户只配置了一个工作空间域名，切换 HTTP/Audio WSS 时只替换 path，不改变 host、region 和 Workspace ID；
- 如果无法从当前 URL 安全推导另一个 profile 的 Endpoint，清空该 profile 的地址并提示用户填写，不猜测其他地域或工作空间。

### 3.3 配置测试

`Test Connection` 必须按当前 transport profile 工作：

- 旧 Realtime：执行现有 WebSocket 握手和 session-ready 检测；
- Audio Streaming：执行 `/api-ws/v1/inference` 握手，发送合法 `run-task`，等待 `task-started`，随后结束测试任务；
- HTTP Batch：发送最小合法 WAV 测试请求，解析 HTTP 状态和模型错误；
- 不能用 WSS 客户端去测试 HTTP Endpoint；
- 错误消息只显示短摘要，完整响应只进入 Debug Mode 安全日志，不记录 API Key、音频 Base64 或完整 transcript。

## 4. 配置模型与兼容性

### 4.1 `Config` 新增字段

在 `src/app/globals.h` 的 `Config` 中增加：

```cpp
enum class QwenTransportMode {
    LegacyRealtimeWebSocket,
    AudioHttpBatch,
    AudioStreamingWebSocket,
};
```

推荐保存为字符串而不是直接序列化 enum：

```cpp
std::wstring qwenTransport = L"audio_streaming";
std::wstring qwenHttpBaseUrl;
std::wstring qwenAudioStreamingBaseUrl = L"wss://llm-c6rtn7zy4nw0u39k.cn-beijing.maas.aliyuncs.com/api-ws/v1/inference";
std::wstring qwenModel = L"qwen-audio-3.0-asr-flash-streaming";
```

字段建议：

- `qwenTransport`：`legacy_realtime`、`audio_http` 或 `audio_streaming`；
- `qwenBaseUrl`：保留现有旧版 WSS 地址字段，兼容旧配置；
- `qwenHttpBaseUrl`：新 HTTP 工作空间地址；
- `qwenAudioStreamingBaseUrl`：北京 Workspace 的新 Audio 3 streaming WSS 地址；
- `qwenModel`：继续保存当前下拉选择；
- `qwenApiKey`、`qwenLanguage`、`qwenChunkMs`：继续复用。
- `qwenLanguage`：旧模型继续保存单语言字符串；新模型请求时转换成 `language_hints` 数组；
- 语言码兼容映射必须显式维护：现有 UI 的 `fil` 在 Audio 3 请求中映射为官方 `tl`；`Auto` 为空时省略 `language_hints`；
- Audio 3 可调参数增加以下字段，且只在对应 profile 序列化和发送：
  - `qwenLanguageHints`：逗号分隔的语言代码，最多 4 个；为空时自动识别；旧模型继续只使用 `qwenLanguage`；
  - `qwenVocabularyId`：预编译热词列表 ID；
  - `qwenVocabulary`：Audio 3 HTTP 与 streaming 都支持的即时热词 JSON object，例如 `{"通义千问":5}`；客户端先做严格 JSON 校验再放入 `parameters.vocabulary`；
  - `qwenSemanticPunctuation`：语义断句开关，默认关闭；
  - `qwenMaxSentenceSilenceMs`：VAD 断句静音阈值，范围 200–6000 ms，默认 1300；
  - `qwenMultiThresholdMode`：多阈值模式，默认关闭，仅 `semantic_punctuation=false` 时生效；
  - `qwenHeartbeat`：心跳包，默认关闭，仅 Audio streaming 使用；
  - `qwenSpeechNoiseThreshold` + `qwenSpeechNoiseThresholdEnabled`：高级 VAD 灵敏度，范围 -1.0 到 1.0，默认关闭。
- `special_word_filter` 和 `context` 先不做普通设置项：前者涉及敏感词处理语义，后者会携带历史文本并有长度/顺序约束，第一版不应自动上传用户历史内容。后续如要支持，必须单独增加显式开关、预览和隐私提示。
- `format`、`sample_rate` 固定由 VoxType 音频管线提供（HTTP 封装 WAV；Audio streaming 发送 16 kHz PCM），不在 Settings 暴露自由编辑，避免用户把采样率改成与实际音频不一致。

不要仅通过模型字符串判断协议作为唯一逻辑。模型名可以被用户手动编辑、未来会新增模型，显式 transport profile 更安全。UI 可隐藏 transport 字段，但运行时必须有明确值。

### 4.2 旧配置迁移

`LoadConfig()`：

1. 新安装（不存在 `config.json`）默认使用 `qwen-audio-3.0-asr-flash-streaming` + `audio_streaming`；
2. 没有 `qwen_http_base_url` 时使用空值，不影响旧用户；
3. 没有 `qwen_audio_streaming_base_url` 时使用空值；
4. 已存在 `config.json` 但没有 `qwen_model`/`qwen_transport` 时，按旧版本兼容策略迁移为 `qwen3-asr-flash-realtime` + `legacy_realtime`，避免老用户升级后突然切换服务；
5. 若旧配置模型是 `qwen3-asr-flash-realtime`，强制 transport 为 `legacy_realtime`；
6. 若配置模型是 `qwen-audio-3.0-asr-flash` 且 transport 缺失，迁移为 `audio_http`；
7. 若配置模型是 `qwen-audio-3.0-asr-flash-streaming` 且 transport 缺失，迁移为 `audio_streaming`；
8. 对未知 model/transport 使用安全回退并在 Settings 状态栏提示，而不是启动时崩溃。

`SaveConfig()`：

- 保存新字段；
- 保留旧 `qwen_base_url`、`qwen_model` 字段格式，保证现有配置文件兼容；
- API Key 仍用现有 DPAPI 加密流程；
- 不把音频数据、请求体或响应写入配置。

## 5. 代码实施分层

### 阶段 A：Qwen profile 与公共配置

目标：先让 UI 和配置能够表达三种 Qwen 协议，但不改变旧实时行为。

涉及文件：

- `src/app/globals.h`
- `src/audio/engine.cpp`
- `src/asr/qwen_asr.h`
- `src/ui/settings.cpp`

工作项：

- 定义 Qwen transport/profile helper；
- 添加三种模型选项常量、显示名、能力标志和 Endpoint profile；
- 将 Model Edit 改为 ComboBox；
- 实现模型切换联动；
- 实现配置加载、保存和旧配置迁移；
- 保证新安装默认值为 `qwen-audio-3.0-asr-flash-streaming`，并保证已有配置值不被覆盖；
- 保证现有实时模型的 Test Connection、Language、Chunk ms 和 fallback 行为不变；
- 为新 Audio 3 streaming 模型预留 profile 和设置控件，不允许 UI 选择未实现的 transport。

验收：旧配置直接启动，实时 Qwen 识别与保存前一致；三个模型切换时 URL、说明文字、Chunk ms 和 Audio 3 参数显示正确。

### 阶段 B：HTTP 音频格式与 JSON 工具

目标：提供独立、可测试的请求构造和响应解析，不把协议逻辑塞进 `main.cpp` 或 Settings。

新增文件：

- `src/asr/qwen_audio_http.h`
- `src/asr/qwen_audio_http.cpp`

建议命名空间：`qwen_audio_http`。

建议接口：

```cpp
struct QwenAudioHttpConfig {
    std::wstring apiKey;
    std::wstring baseUrl;
    std::wstring model = L"qwen-audio-3.0-asr-flash";
    std::wstring language;
    std::wstring vocabularyId;
    std::wstring vocabulary;
    bool semanticPunctuation = false;
    int maxSentenceSilenceMs = 0;
    int sampleRate = 16000;
    bool enableSse = false;
};

struct RecognitionResult {
    bool ok = false;
    std::wstring text;
    std::wstring error;
    DWORD httpStatus = 0;
    double elapsedMs = 0.0;
};

RecognitionResult RecognizeWav(const QwenAudioHttpConfig& config,
                               const std::vector<BYTE>& pcm,
                               DWORD timeoutMs);

TestResult TestConnection(const QwenAudioHttpConfig& config);
```

实现要求：

- 将 16 kHz mono s16le PCM 封装成合法 RIFF/WAVE；
- Base64 编码 WAV，并组成 `data:audio/wav;base64,...`；
- 使用 WinHTTP，复用 `cloud_http_common` 中可复用的超时/错误处理原则；
- 请求头使用 Bearer API Key；
- 默认禁用 SSE，先解析单个 final JSON；
- 为 `enableSse` 预留 SSE 解析器，但只有 HTTP 模型收到完整音频后才启用，不能把它当作录音期间的 streaming input；
- 按 HTTP profile 发送 `format`、`sample_rate`、`language_hints`、Vocabulary ID 和即时 `vocabulary`；不发送 streaming-only 的语义标点、句间静音、多阈值、heartbeat、噪声阈值；
- 对 HTTP 4xx/5xx 提取安全的 `code`/`message`；
- 对 401/403、429、5xx、超时、DNS、连接失败做可分类错误；
- 限制请求体大小，避免长录音无界 Base64 内存增长；
- 不在音频回调中执行 HTTP 请求；
- 不记录 API Key、完整 WAV Base64、完整请求体和完整 transcript。

JSON 解析必须覆盖官方返回可能出现的文本路径，并通过小型单元测试固定：

- 成功返回文本；
- 空文本/无语音；
- 错误 envelope；
- Unicode 转义；
- 字段缺失；
- 非 JSON 错误页面。

### 阶段 C：Batch session 接入

目标：让 `qwen-audio-3.0-asr-flash` 走现有 Batch ASR 生命周期。

涉及文件：

- `src/asr/asr_session.h`
- `src/asr/asr_session.cpp`
- `src/asr/asr_result.h/.cpp`
- `src/asr/cloud_asr_common.*`

工作项：

- 新增 `AsrSessionBackend::QwenAudioBatch`；
- 新增 `QwenAudioAsrSession : BatchAsrSessionBase`；
- `Finish()` 中检查 abort、API Key、HTTP Base URL；
- 按现有 Batch provider 规则执行 `BatchVadTrimmer`；
- 对 no-speech/too-short 保持现有不触发错误粘贴的语义；
- 计算 `cloudApiMs`；
- 将 `qwenTransport == audio_http` 路由到新 session；
- 保持 `qwen3-asr-flash-realtime` 的现有 batch fallback 语义不变；
- 不在 session 中直接操作 UI、剪贴板或 `g_config`。

建议路由：

```cpp
if (config.asrBackend == L"qwen") {
    if (IsQwenAudioHttp(config)) {
        return std::make_unique<QwenAudioAsrSession>(...);
    }
    return std::make_unique<QwenAsrSession>(...);
}
```

`QwenAsrSession` 现有实现调用的是实时 WebSocket 客户端，不能把它改名后直接复用给 HTTP。

### 阶段 D：Audio 3 Streaming WebSocket 客户端与 session

目标：让 `qwen-audio-3.0-asr-flash-streaming` 真正支持 VoxType 的按住说话、实时 partial 和松开 final 体验。

新增文件：

- `src/asr/qwen_audio_streaming.h`
- `src/asr/qwen_audio_streaming.cpp`
- `src/asr/qwen_audio_streaming_session.h`
- `src/asr/qwen_audio_streaming_session.cpp`

协议客户端职责：

- 解析工作空间 WSS URL；
- Authorization Bearer 握手；
- 发送 `run-task`，携带 `task_id`、`model`、`parameters`；
- 发送二进制 PCM message，不在音频回调里做 JSON/网络工作；
- 读取并解析 `task-started`、`result-generated`、`task-finished`、`task-failed`；
- 发送 `finish-task`；
- 支持 Abort/Close 和有界接收超时；
- 保持 task_id/session 生命周期清晰。

Streaming session 职责：

- 继承现有 `StreamingAsrSessionBase`；
- 复用 `PendingPcmBuffer`；
- 复用 `StreamingVadTrimmer` 的头尾裁剪语义；
- 音频回调只调用 `EnqueuePcmChunk()`；
- drain thread 只做接收、partial/final 解析和状态投递；
- 停止录音用 `StopInput()`，不让 `main.cpp` 同步等待云端 final；
- 保留 replay buffer、adaptive final timeout 和一次有界 replay retry；
- 不把新 Audio 3 协议的 send/drain/retry 强行抽成现有 Qwen realtime 的复杂模板。

第一版不做跨录音连接复用和 prewarm。官方文档虽支持连接复用，但应等单录音 session 的握手、finish、abort、异常恢复稳定后再单独增加连接 owner；不能因为两个模型都使用 WSS 就复用现有 `qwen_asr::RealtimeClient` 的连接句柄。

### 阶段 E：主录音流程按 profile 路由

目标：让新模型真正可作为主 ASR 使用，同时保留实时模型的 partial HUD。

涉及文件：

- `src/app/main.cpp`
- `src/asr/asr_dispatcher.*`
- `src/asr/asr_result.*`

当前行为：

- `asrBackend == qwen` 会直接创建 `CreateQwenStreamingSession()`；
- batch 路径使用 `CreateBatchAsrSession()`。

需要改为：

```text
qwen + legacy_realtime
    -> CreateQwenStreamingSession()

qwen + audio_http
    -> 录音期间只收集 raw PCM
    -> Stop 后 CreateBatchAsrSession()
    -> HTTP POST
    -> DispatchAsrFinalText()

qwen + audio_streaming
    -> CreateQwenAudioStreamingSession()
    -> 录音期间发送二进制 PCM
    -> StopInput()
    -> finish-task / final drain
    -> DispatchAsrFinalText()
```

注意事项：

- HTTP 模型默认不显示 partial HUD；录音时显示音量和 `Listening...`，松开后显示 `Recognizing... Qwen Audio`。如果启用 HTTP SSE，partial 只能发生在松开之后；
- Audio 3 streaming 模型显示实时 partial，松开后等待 `task-finished` final；
- HTTP 请求必须放后台 worker，不能阻塞 UI 线程；
- Audio streaming 的发送和 drain 也必须在 session worker/专用 drain 线程执行，不能阻塞 UI 线程；
- 使用当前 attempt id/stale guard，避免旧 HTTP 请求覆盖下一次录音；
- fallback 继续使用原始 16 kHz PCM；
- Qwen Audio HTTP 或 Audio streaming 请求失败时，按现有 operational error 分类进入 fallback；
- 成功结果才进入 LLM refine 和文本注入。

### 阶段 F：Settings UI 完成

涉及文件：

- `src/app/globals.h`
- `src/ui/settings.cpp`

工作项：

- 新增 `IDC_QWEN_MODEL_COMBO` 或复用原 Model 控件 ID；
- 保持旧配置可加载；
- 模型下拉选择变更时更新 profile；
- Base URL 根据 profile 在旧 WSS、HTTP generation、新 Audio WSS 三个保存值之间切换；
- 允许用户手动覆盖 Base URL；
- `Chunk ms` 对 HTTP 模型禁用；
- `Chunk ms` 对两个 streaming 模型启用，其中新 Audio streaming 发送二进制 PCM；
- 增加说明文字：旧 realtime 与 Audio streaming 支持录音期间 partial，HTTP 模型在松开后整段识别；
- Audio 3 模型显示“Audio 3 高级参数”区域，至少包含：
  - Language hints（逗号分隔，最多 4 个，Auto 为空）；
  - Vocabulary ID；
  - Immediate vocabulary/Hot words JSON object 多行编辑（HTTP 与 streaming 均支持，例如 `{"通义千问":5}`）；
  - Semantic punctuation；
  - Max sentence silence（200–6000 ms）；
  - Multi-threshold mode（Semantic punctuation 打开时禁用）；
  - Heartbeat（仅 Audio streaming）；
  - Speech noise threshold（高级项，默认关闭）；
- 旧 realtime 隐藏 Audio 3 高级控件；HTTP 隐藏 streaming-only 控件；
- Test Connection 按 profile 调用对应客户端；
- 保存时校验：
  - API Key 非空；
  - Model 非空且在支持列表内；
  - 两种 Realtime URL 为 `ws://`/`wss://`；
  - Audio streaming Endpoint 必须指向 `/api-ws/v1/inference`；
  - HTTP URL 为 `http://`/`https://`；
  - HTTP Endpoint 路径完整；
  - `language_hints` 代码合法且不超过 4 个；
  - 热词总数不超过 2000 个，权重只允许 1–5 或 50，权重 50 不超过 50 个；
  - `max_sentence_silence` 在 200–6000 ms；
  - `speech_noise_threshold` 在 -1.0 到 1.0；
- `semantic_punctuation_enabled` 与 `multi_threshold_mode_enabled` 不能同时启用；
- 150%/200% DPI 下检查 ComboBox、Base URL、按钮和底部状态栏不裁切/重叠。

不建议本轮保留一个允许任意模型名的普通文本框作为主要入口。若保留“自定义模型”能力，应放到高级选项，并在未知模型时要求用户显式选择 transport。

## 6. 现有路径复用边界

### 6.1 必须复用

以下是三个 Qwen model profile 的公共 VoxType 管线，不得重复实现：

- WASAPI/waveIn 16 kHz、mono、s16le 采集路径；
- `IAsrSession` / `BatchAsrSessionBase`：HTTP 模型；
- `IStreamingAsrSession` / `StreamingAsrSessionBase`：两个 streaming 模型；
- `PendingPcmBuffer`：新 Audio streaming 的轻量入队；
- `BatchVadTrimmer`：HTTP 模型的头尾静音裁剪；
- `StreamingVadTrimmer`：新 Audio streaming 的头尾静音裁剪；
- `cloud_asr_common`：replay buffer、adaptive timeout、recorded request timeout；
- `asr_result`：错误分类、显示名、日志名；
- `asr_dispatcher`：final、LLM gate、raw ASR text；
- 主窗口 attempt id、stale result guard 和 fallback orchestration；
- Settings 的 Qwen API Key、Show/Hide、DPAPI 保存路径；
- HUD partial/final 和文本注入路径。

### 6.2 只复用外壳，不复用协议

现有 `src/asr/qwen_streaming_session.cpp` 的生命周期模式可以参考并复用公共组件，但不能直接把 client 类型或消息名替换成新模型：

- 旧客户端发送 Base64 JSON audio；新 Audio streaming 发送二进制 PCM；
- 旧协议使用 `session.update` / `input_audio_buffer.append`；新协议使用 `run-task` / `finish-task`；
- 旧 final 事件是 conversation/session 事件；新 final 是 task 事件；
- 两者的连接复用、task 生命周期和错误 envelope 不同。

因此推荐保留现有 `qwen_asr.*` 和 `qwen_streaming_session.*` 不动，新增 Audio 3 专属 client/session 文件。仅共享 provider-neutral 的 buffer、VAD、timeout、dispatch 和 result policy。

### 6.3 不应复用

- 不复用 `qwen_asr::BuildSessionUpdateMessage()` 给 Audio 3；
- 不复用旧 `input_audio_buffer.append` 发送器给二进制 PCM；
- 不复用一个 Base URL 字段在三条路径间覆盖；
- 不把 HTTP 请求塞进现有 realtime retry loop；
- 不把新 Audio streaming 的 task events 强行翻译成旧 conversation events 后再解析；
- 不把 API Key 硬编码进源码；本次用户明确要求的北京 Workspace Endpoint 作为默认值，旧配置和 Settings 自定义 Endpoint 仍优先保留。

## 7. HTTP 客户端的请求与错误策略

### 7.1 请求流程

```text
StopInput()
-> 获取完整 raw PCM
-> 可选 Batch VAD trim
-> PCM -> WAV
-> WAV -> Base64 Data URI
-> 构造 multimodal-generation JSON
-> WinHTTP POST
-> 解析 output transcript
-> AsrSessionResult
-> 主窗口 Dispatch
```

### 7.2 超时

- 使用录音时长和 WAV 大小计算请求超时；
- 最小超时不低于现有 cloud recorded request 的安全下限；
- 长音频使用上限，避免 UI 无限等待；
- Stop/Abort 后取消 WinHTTP request；
- 不在析构函数里无限等待网络线程。

### 7.3 重试

第一版只做有限、可解释的重试：

- DNS/连接/超时/HTTP 5xx：重发同一份 WAV；
- 429：短延迟后最多一次重试；
- 401/403：不盲目重试，直接报告鉴权错误；
- 4xx 参数/模型错误：不重试；
- 同一请求不能因为空文本无限重发。

每次重试必须使用同一段 PCM/WAV，不重新录音。

## 8. Test Connection 设计

### 旧 Realtime 模型

复用现有 `qwen_asr::TestConnection()`：

- 验证 API Key；
- 验证 WSS Endpoint；
- 验证 session ready；
- 关闭连接。

### Audio 3 Streaming 模型

新增 `qwen_audio_streaming::TestConnection()`：

- 验证工作空间 WSS Endpoint；
- 发送 `run-task`；
- 等待 `task-started`；
- 不发送真实用户音频；
- 发送 `finish-task` 或安全 Abort；
- 将鉴权失败、模型未授权、Endpoint path 错误和 task 参数错误分别报告。

### HTTP 模型

新增 `qwen_audio_http::TestConnection()`：

- 构造极短、合法的静音 WAV 或使用官方允许的最小测试音频；
- 发送一次 HTTP POST；
- 只验证鉴权、Endpoint、模型和响应结构；
- 若服务拒绝静音音频但鉴权成功，显示“连接成功，测试音频无语音”而不是误报连接失败。

## 9. 构建与文件清单

新增源码：

- `src/asr/qwen_audio_http.h`
- `src/asr/qwen_audio_http.cpp`
- `src/asr/qwen_audio_streaming.h`
- `src/asr/qwen_audio_streaming.cpp`
- `src/asr/qwen_audio_streaming_session.h`
- `src/asr/qwen_audio_streaming_session.cpp`

修改源码：

- `src/app/globals.h`
- `src/audio/engine.cpp`
- `src/asr/asr_session.h`
- `src/asr/asr_session.cpp`
- `src/asr/asr_result.h/.cpp`
- `src/app/main.cpp`
- `src/ui/settings.cpp`
- `src/ui/settings.h`（如需新增 UI helper 声明）

构建清单：

- `CMakeLists.txt` 添加新 `.cpp`；
- 不新增第三方库，WinHTTP/WinCrypt 继续使用现有链接配置；
- `cmake/VoxTypeRuntime.cmake` 无需添加运行时 DLL；
- 若新增测试可放入 `tests/`，测试输出放 `build/artifacts/`，不要把输入音频放进 `build/` 顶层。

文档同步：

- `README.md`：Qwen 支持模型和“Realtime/HTTP Batch”说明；
- `ARCHITECTURE.md`：Cloud ASR、Settings、Batch session 路由；
- `CHANGELOG.md`：新增 Qwen Audio 3.0 ASR 支持；
- 版本号只改 `src/app/resource.h` 的版本宏，并同步 README/CHANGELOG。

## 10. 测试计划

### 10.1 单元/协议测试

- WAV header：空 PCM、1 帧、奇数长度、长音频；
- Base64 编码和 Data URI 前缀；
- JSON 转义、Unicode、嵌套 output 路径；
- 成功响应提取 transcript；
- 错误响应提取 code/message；
- 非 JSON/截断响应安全失败；
- HTTP 状态分类；
- URL 解析和 Endpoint path 校验；
- Audio 3 `run-task` JSON 构造；
- 二进制 PCM message 发送边界；
- `task-started`、`result-generated`、`task-finished`、`task-failed` 解析；
- finish/abort/close 生命周期。

### 10.2 Settings 测试

- 旧配置加载后仍默认实时模型；
- 选择 HTTP 模型后 URL/Chunk ms 联动正确；
- 选择旧 realtime 模型后恢复旧 WSS URL/Chunk ms；
- 选择 Audio streaming 模型后切换到 `/api-ws/v1/inference` 并保留 Chunk ms；
- 选择 Audio 3 模型后显示/隐藏 vocabulary、语义标点等高级项；
- Audio 3 高级参数保存/重开后保持一致；
- Language hints 超过 4 个、非法语言码、热词超限、非法权重、静音阈值越界时阻止保存并显示具体原因；
- Semantic punctuation 与 multi-threshold 同时打开时自动关闭冲突项或阻止保存；
- Heartbeat/noise threshold 仅对 Audio streaming 发出；
- 自定义 URL 在切换模型时不会被无条件覆盖；
- 保存、关闭、重开后 model/transport/URL 保持一致；
- API Key DPAPI 加密流程不回归；
- 100%、150%、200% DPI 无裁切和重叠。

### 10.3 运行时测试

- HTTP 模型 1–3 秒普通话短句；
- Audio streaming 模型按住说话时 partial，松开后 final；
- Audio streaming 模型中途静音、短句、长句和中英混说；
- Audio streaming 调整 `max_sentence_silence`、semantic punctuation、multi-threshold 后分别验证断句和 final 延迟；
- Audio streaming 发送 2000 条热词、超限热词、非法权重时验证客户端校验和服务端错误处理；
- Audio streaming 开启 heartbeat 后验证长静音期间连接不被误判断开；
- Audio streaming noise threshold 在 -1/0/1 和关闭状态下验证无崩溃、无异常内存增长；
- Audio streaming 连接失败、task failed、finish 超时和 Abort；
- 录音中 WASAPI/waveIn 设备断开或驱动错误：generation guard 丢弃旧消息，当前 attempt 停止且不把不完整 PCM 交给 fallback；
- 中英混说、标点、数字和专有名词；
- 静音、过短录音、长录音；
- 错误 API Key；
- 错误工作空间 URL；
- 模型未授权/不存在；
- DNS 失败、断网、HTTP 5xx、429；
- HTTP 请求期间开始下一次录音；
- 取消/退出时网络线程可终止；
- HTTP 成功文本进入 LLM refine 和微信/普通窗口注入路径；
- HTTP 失败时 fallback 只使用原始 PCM，并且不粘贴错误文本。

### 10.4 回归测试

- `qwen3-asr-flash-realtime` partial HUD；
- Qwen realtime replay retry；
- Qwen realtime final timeout；
- Qwen realtime fallback；
- 三个 Qwen 模型之间切换后，旧模型协议未被新 profile 污染；
- Baidu、MiMo、Volcengine、Qwen IME Free 不受影响；
- Settings 打开时热键拦截暂停；
- CapsLock 短按/长按状态恢复不变。

## 11. 验收标准

### 功能

- Model 下拉菜单提供现有 realtime、Audio 3 HTTP、Audio 3 streaming 三个模型；
- 新安装 Model 下拉菜单第一项且默认选中 `qwen-audio-3.0-asr-flash-streaming`；
- 已有配置加载后保持原 `qwen_model`/transport，不因升级自动切换；
- 新 HTTP 模型可以使用同一个 API Key 完成识别；
- 新 Audio streaming 模型可以使用同一个 API Key 完成实时识别；
- HTTP 模型不再尝试 WebSocket；
- Audio streaming 模型不再发送旧 `session.update` / `input_audio_buffer.append` 协议；
- HTTP 模型松开按键后在后台完成请求；
- Audio streaming 模型录音期间持续发送二进制 PCM、显示 partial，松开后返回 final；
- 成功结果可以正常显示、LLM refine 和注入；
- 错误不会被当作识别文本粘贴；
- 录音设备运行中失败时显示设备错误，活动 provider session 和 Watchdog 均安全终止；
- 现有 realtime 模型行为无回归。

### 配置

- 旧 `config.json` 无需手工修改即可启动；
- 旧 WSS、HTTP、新 Audio WSS Endpoint 分开保存；
- API Key 仍保持 DPAPI 加密；
- Model/transport/URL 保存后重启仍一致。

### 工程

- `.\build.bat` 成功；
- 只运行 `build\run\x64-release\VoxType.exe` 验证；
- `git diff --check` 通过；
- 新增测试输出只进入 `build/artifacts/` 或 `build/logs/`；
- 不在 `main.cpp` 写大段 HTTP/provider 协议逻辑；
- 不在音频回调中进行网络请求；
- 不新增不必要的第三方依赖。

## 12. 提交拆分建议

建议拆成以下提交，便于逐步验证和回滚：

1. `refactor: add qwen transport profiles and model selector`
   - Config 字段、旧配置迁移、Model ComboBox、参数联动。

2. `feat: add qwen audio http client`
   - WAV/Base64、HTTP POST、响应解析、Test Connection、协议单测。

3. `feat: add qwen audio streaming protocol client`
   - run-task、二进制 PCM、task events、finish-task、协议单测。

4. `feat: route qwen audio profiles through asr sessions`
   - `QwenAudioBatchSession`、main 路由、结果分类和超时。

5. `feat: add qwen audio streaming session`
   - `QwenAudioStreamingSession`、partial HUD、replay/final/fallback。

6. `docs: document qwen audio 3 asr integration`
   - README、ARCHITECTURE、CHANGELOG、版本号。

如实施过程中改动规模较小，也可以合并第 2、3 项，但不要把 UI、HTTP 协议和主流程重构混在一个无法独立验证的大提交中。

## 13. 明确不做

- 不把 HTTP 模型伪装成 partial streaming；
- 不在第一版实现 Audio streaming 的跨录音连接复用/prewarm；
- 不把 API Key 明文写入日志或请求诊断；
- 不在 `main.cpp` 直接拼装 provider JSON；
- 不在 WASAPI/waveIn 回调中上传音频；
- 不新增独立的重复 Qwen API Key 配置；
- 不默认启用 LLM 纠错；
- 不改变已有配置用户的 Qwen realtime 模型、Endpoint 和默认行为；新安装默认使用 Audio 3 streaming。

## 14. 实施前需要确认的外部条件

1. **已确认地域与 Workspace：北京**；默认 HTTP/WSS 使用用户已开通权限的 Workspace 两个官方路径；
2. **已确认 API Key 权限已开通**；实现前仍需用该 Key 对两个 Audio 3 Endpoint 各做一次 Test Connection；
3. 真实 HTTP 响应和 streaming task event 样例，用于固定字段解析测试；
4. HTTP 模型允许的最大音频大小和时长，以及 VoxType 是否需要在客户端提前拒绝超限录音；
5. 已确认：新安装默认选中并置顶 `qwen-audio-3.0-asr-flash-streaming`；已有配置不自动迁移到新模型。

## 15. 实施前安全与性能门槛

### 15.1 安全与数据边界

- 新 Audio 3 HTTP 只允许 HTTPS；新 Audio 3 streaming 只允许 WSS。旧配置若使用明文 HTTP/WS，只保留兼容读取能力，不向新 profile 自动复制。
- API Key 永不硬编码或写日志；默认 Endpoint 使用用户明确提供且已开通权限的北京 Workspace，用户自定义覆盖值只保存在本地配置。
- 每个 session 在创建时复制一份不可变 `Config` snapshot；worker 不读取会被 Settings Save 修改的全局 `g_config`。
- HTTP 请求体、WAV/Base64、streaming 二进制 PCM、API Key、context、vocabulary 内容都不能写入运行日志；Debug Mode 只记录状态码、错误类别、耗时和字节数。
- `context` 默认关闭。除非用户显式输入并开启，否则不得把上一条 transcript、当前窗口文本或 LLM 内容自动发送给 Audio 3。
- endpoint 切换必须做 scheme/path 校验；不能因为模型切换把 API Key 发送到另一个 profile 的旧 URL 或不匹配的地域。

### 15.2 线程与生命周期

- HTTP 请求由 batch worker 执行，UI 线程只接收结果消息；
- Audio streaming 的发送、接收、finish、abort 均由 session worker/drain thread 执行；
- `Abort()` 必须关闭 active WinHTTP request/WebSocket，使 `join()` 有界返回；
- `StopInput()` 只停止输入，不同步等待云端 final；
- final、retry、fallback、LLM 和 paste 都必须通过现有 attempt claim/stale guard，保证一次录音最多产生一个最终副作用；
- Settings Save、下一次录音、程序退出期间，旧 worker 的结果不得覆盖新 config、HUD 或剪贴板。

### 15.3 内存与性能

- HTTP 端在构造 WAV/Base64 前先检查录音大小和官方模型限制；超限时本地失败，不创建无界 JSON。
- HTTP 请求应尽量复用已有 PCM buffer，避免同时保留多份 WAV、Base64 和 JSON 副本；必要时设置明确的请求体上限。
- Audio streaming 只保留 `PendingPcmBuffer`、有界 replay buffer 和现有 raw PCM；不能为每个发送 chunk 无界累积副本。
- heartbeat 默认关闭；只在用户选择时启用，避免短按键输入产生无意义的额外网络包。
- `semantic_punctuation_enabled`、`max_sentence_silence`、`multi_threshold_mode_enabled` 的默认值首先采用官方默认值，先测量再调整，不为了降低延迟盲目修改模型 VAD。
- 不在音频回调中做 Base64、JSON、日志、WinHTTP、WebSocket 协议解析或锁等待；
- 记录三个时延指标：连接/任务启动耗时、录音结束到 final 耗时、总 cloud API 耗时；同时记录 HTTP 请求体大小或 streaming PCM 字节数。

### 15.4 通过标准

以下条件全部满足后才允许把 Audio 3 streaming 放入用户可选列表：

1. 旧 Qwen realtime 回归测试通过；
2. HTTP 与 Audio streaming 的协议单测、配置迁移、Settings DPI 检查通过；
3. 正常识别、静音、长静音、断网、错误 Key、错误 Endpoint、task failed、Abort、重复录音测试通过；
4. 连续至少 100 次短句录音没有崩溃、重复粘贴、旧结果污染或线程泄漏；
5. 1 分钟录音和连续长静音场景下内存保持有界；
6. `build.bat`、`git diff --check` 和规范运行载荷验证通过；
7. 真实百炼北京 Workspace 的 HTTP/WSS 端点和 API Key 均已验证。
## 16. Implementation verification addendum (2026-08-10)

The two Audio 3 models were verified against the confirmed Beijing Workspace endpoint:

- `qwen-audio-3.0-asr-flash`: HTTP batch recognition succeeds after release and intentionally produces final only.
- `qwen-audio-3.0-asr-flash-streaming`: WSS recognition succeeds, emits `result-generated` partials during recording, and waits for `finish-task` / `task-finished` final after release.

Recovery boundaries are now explicit: common session/base, pending PCM, bounded replay, adaptive final timeout, main watchdog, fallback, and stale-attempt guards are reused; Audio 3 keeps its own `run-task`, binary PCM, `finish-task`, event parser, and drain loop. Server `task-failed` is non-replayable, transport loss permits one bounded replay, replay-budget exhaustion waits for key release, and pending PCM overflow enters an explicit error path.

Latest offline evidence: `build.bat --test` passed with protocol regression tests, Qwen free protocol regression, runtime layout validation, and Settings 96/144/192/288 DPI startup validation. The user confirmed both real Audio 3 models and streaming partial behavior against the live endpoint.

The same regression target now also asserts the exact replay-buffer limit/disable behavior and pending-PCM overflow/drain/reset behavior used by the Audio 3 session.

No-speech handling is also covered: HTTP `ASR_RESPONSE_HAVE_NO_WORDS` maps to an empty successful recognition and therefore the existing `No speech detected` HUD path; the streaming parser recognizes the same provider marker without treating it as an operational failure.

The user subsequently confirmed the rebuilt canonical executable shows `No speech detected` for a held-but-silent recording. The shared capture layer now also reports runtime WASAPI/`waveIn` failures with a capture-generation guard; the main thread stops/releases capture, invalidates the attempt, aborts the provider session, cancels the watchdog, and refuses to fallback or paste incomplete PCM.

Offline verification after that capture-error change: `build.bat --test` passed, `git diff --check` passed, runtime layout validation passed, and Settings startup layout validation passed at 96/144/192/288 DPI. Actual network interruption and microphone hot-unplug injection remain manual environmental tests rather than protocol/unit-test claims.
