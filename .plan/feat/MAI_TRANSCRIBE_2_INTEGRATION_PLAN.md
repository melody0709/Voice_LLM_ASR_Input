# Microsoft MAI-Transcribe 2 集成计划

> 状态：计划中
>
> 创建日期：2026-09-04
>
> 目标：在 VoxType 中新增一个 `Microsoft MAI-Transcribe 2` ASR 后端，Settings 可选择 `OpenRouter` 或 `Azure Speech API`；当前优先交付用户已有凭据可用的 OpenRouter batch 路径。

## 1. 结论先行

采用一个顶层 ASR 后端、两个 API 通道：

```text
Microsoft MAI-Transcribe 2
├─ OpenRouter           -> HTTP batch，用户当前可用
└─ Azure Speech API     -> Fast Transcription HTTP batch，未来有 Azure Key 时可用
```

两条正式接入路径都按 **batch ASR** 实现：录音期间只采集 PCM，松开快捷键后一次上传完整音频，返回 final 文本。

第一版不实现 partial：

- OpenRouter 当前公开的是 `/api/v1/audio/transcriptions` 单请求/单响应接口，没有音频 append/commit WebSocket 协议。
- Azure Fast Transcription 也是完整文件 multipart 请求。
- Azure Voice Live 虽然存在 `conversation.item.input_audio_transcription.delta`，但官方说明转写在 `input_audio_buffer.commit` 后开始；对 VoxType 的 manual hold-to-talk 来说，通常仍是松开后才产生 delta。
- Voice Live 当前公开配置名是 `mai-transcribe`，没有明确证明可固定选择 `MAI-Transcribe-2`。在真实协议探针确认前，不能把它宣传成 v2 的录音中 partial。

因此本计划的 partial 结论是：

```text
OpenRouter                    final only
Azure Fast Transcription      final only
Azure Voice Live              独立研究阶段；不承诺录音期间 partial
```

不要用“每隔几秒重复上传累计 WAV”伪造 partial。这会重复计费、产生重叠文本、增加延迟，并且没有稳定的 session/segment 合并语义。

## 2. 本轮范围

### 2.1 必须完成

- 新增 ASR backend id：`mai`。
- Recognition 的主后端和 fallback 下拉菜单均可选择 `Microsoft MAI Transcribe 2`。
- Cloud ASR Provider 增加 `Microsoft MAI Transcribe 2`。
- MAI Settings 中增加 API 通道选择：
  - `OpenRouter`
  - `Azure Speech API`
- OpenRouter 为新 MAI 配置的默认通道；只配置 OpenRouter Key 就能工作。
- Azure Key/Endpoint 为空不能影响 OpenRouter 保存、测试或识别。
- 两个通道共用录音、WAV、VAD、batch session、超时、取消、诊断、fallback、LLM 和最终文本注入链路。
- OpenRouter 和 Azure 使用各自独立保存的凭据，切换通道不覆盖另一方。
- 两个 API Key 都使用现有 DPAPI 加密保存。
- 支持 `Auto / Chinese / English / Cantonese` 语言选择；Auto 时不发送语言限制。
- 支持连接测试、错误分类、一次 transient retry、录音诊断和 `asr_audio_replay`。
- 增加纯协议回归测试，并用规范运行载荷做 Settings/UI smoke test。

### 2.2 第一版明确不做

- 不把 MAI 设为现有用户或新安装的默认 ASR 后端。
- 不实现 OpenRouter partial、SSE 或重复分片上传。
- 不实现 Azure Voice Live WebSocket。
- 不实现 diarization、word timestamps、speaker labels 的 UI 消费链路。
- 不增加 Model 编辑框：模型固定为 `microsoft/mai-transcribe-2` / `MAI-Transcribe-2`。
- 不增加 OpenRouter Base URL：固定使用官方地址。
- 不把 Azure API version 做成用户配置：使用代码常量，升级时由代码和测试一起更新。
- 不增加新的 HTTP/JSON/音频第三方依赖。
- 不把 provider 逻辑塞进 `src/app/main.cpp`。
- 不自动上传输入框上下文、历史文本或热词。

## 3. 截至 2026-09-04 的官方接口事实

### 3.1 OpenRouter

模型：

```text
microsoft/mai-transcribe-2
```

请求：

```http
POST https://openrouter.ai/api/v1/audio/transcriptions
Authorization: Bearer <OPENROUTER_API_KEY>
Content-Type: application/json
```

首版采用 JSON + Base64 WAV：

```json
{
  "model": "microsoft/mai-transcribe-2",
  "input_audio": {
    "data": "<base64 wav>",
    "format": "wav"
  },
  "language": "zh"
}
```

`language` 可省略；省略代表自动识别。首版只请求普通 JSON final：

```json
{
  "text": "识别结果",
  "usage": {
    "type": "transcription"
  }
}
```

OpenRouter 文档同时接受 multipart，但 VoxType 已经有 WAV/Base64 路径和 WinHTTP JSON 请求模式，JSON 是更小的接入改动。若后续实际测试表明 Base64 内存或请求体限制成为问题，再切 multipart；第一版不同时维护两种 OpenRouter body。

当前模型页只列出 Azure 一个上游 endpoint，因此 OpenRouter 的 provider routing 不能替代 VoxType 自己的 fallback。

### 3.2 Azure Fast Transcription

请求：

```http
POST <AZURE_RESOURCE_ENDPOINT>/speechtotext/transcriptions:transcribe?api-version=2025-10-15
Ocp-Apim-Subscription-Key: <AZURE_SPEECH_KEY>
Content-Type: multipart/form-data; boundary=<boundary>
```

multipart 至少包含两个 part：

```text
audio       audio/wav 文件内容
definition  application/json 配置
```

`definition`：

```json
{
  "locales": ["zh-CN"],
  "enhancedMode": {
    "enabled": true,
    "model": "MAI-Transcribe-2",
    "modelOptions": {
      "transcribeStyle": "clean"
    }
  }
}
```

Auto 时省略 `locales`。VoxType 是语音输入法，Azure 首版固定 `clean`，不增加 style 控件；需要严格逐字稿时再增加 `Clean / Verbatim`。

Azure 成功结果从 `combinedPhrases[].text` 聚合；VoxType 输入固定 16 kHz、16-bit、mono，因此通常只有一个 combined phrase。解析器仍需正确处理空数组和多项。

Azure Endpoint 只保存用户资源根地址，不保存完整 REST path。运行时使用 `WinHttpCrackUrl` 解析并追加固定 path/query：

```text
https://<resource>.cognitiveservices.azure.com
```

校验只要求 HTTPS、非空 host、无 fragment；不要硬编码域名后缀，以兼容 Azure 私有端点或未来的官方 host 形式。

### 3.3 Azure Voice Live 与 partial 的边界

Voice Live 客户端确实可以收到：

```text
conversation.item.input_audio_transcription.delta
conversation.item.input_audio_transcription.completed
```

但官方 reference 同时说明，输入音频转写在 buffer 被 commit 后开始。VoxType 当前 manual hold-to-talk 是松开时 `StopInput()`/commit，因此这个 delta 很可能只是“松开后的增量输出”，不是用户说话时的实时 partial。

Azure Voice Live 只有在以下探针全部通过后才进入实施：

1. 能显式固定使用 `MAI-Transcribe-2`，而不是不透明的 `mai-transcribe` 别名。
2. 在持续 `input_audio_buffer.append`、尚未最终 commit 时收到可用 delta；或者官方支持不破坏录音边界的 segment commit。
3. 多次 commit 的 item 能稳定合并，不在停顿处丢字或重复。
4. 不需要额外启动无关的 LLM response。
5. 成本、endpoint、地区和 public preview 风险可接受。

任一条件失败，MAI 继续保持 batch-only；partial 使用现有 Qwen/火山 streaming 后端。

## 4. Settings 设计

### 4.1 Recognition 页面

在现有 `kBackendOptions` 增加：

```text
Microsoft MAI Transcribe 2
```

内部 id：

```text
mai
```

能力：

```text
primarySupported = true
fallbackSupported = true
```

### 4.2 Cloud ASR 页面

Provider 下拉新增：

```text
Microsoft MAI Transcribe 2
```

MAI 子页第一版布局：

```text
API                 [OpenRouter ▼]
OpenRouter API Key  [••••••••••••] [Show]
Language            [Auto ▼]
                     OpenRouter sends the complete recording after key release.
                     This API returns final text only.
                                            [Test Connection]
```

切到 Azure：

```text
API                 [Azure Speech API ▼]
Azure Endpoint      [https://<resource>.cognitiveservices.azure.com]
Azure API Key       [••••••••••••] [Show]
Language            [Auto ▼]
                     Azure Fast Transcription sends the complete WAV after key release.
                     This API returns final text only.
                                            [Test Connection]
```

语言映射：

| Settings | OpenRouter | Azure |
|---|---|---|
| Auto | 省略 `language` | 省略 `locales` |
| Chinese | `zh` | `zh-CN` |
| English | `en` | `en-US` |
| Cantonese | `yue` | `yue-CN`，实施前由 live probe 确认 Azure 接受的 locale |

UI 行为：

- 默认选中 OpenRouter。
- API 通道切换时只改变显示的 key/endpoint 控件，不清空任何已输入值。
- 只有当前通道参与保存前必填校验。
- OpenRouter 模式只要求 OpenRouter Key。
- Azure 模式要求 Azure Endpoint + Azure Key。
- Base URL、model、API version 不暴露给用户。
- 动态说明必须明确写 `Final only`，避免用户把模型低延迟误解成 partial。
- 使用 `UiStyle` 行/列常量；不增加固定 Settings 窗口高度。
- 新增 `g_maiControls`、`g_maiOpenRouterControls`、`g_maiAzureControls`，沿用当前 Cloud Provider 子页显示模式。
- Test Connection 沿用 `g_sharedTestGeneration` + worker thread + `PostSharedTestResult()`，关闭 Settings 后的迟到结果必须被 generation 丢弃。

### 4.3 Test Connection

不引入测试音频资源。运行时生成一个很短的 16 kHz mono silence WAV：

- HTTP 2xx + 空文本视为 endpoint/auth/model 可用。
- 已确认的 provider no-speech 响应也可视为连接成功。
- 401/403 显示凭据错误。
- OpenRouter 402 显示额度不足。
- 404 显示模型或 Azure endpoint/path 不可用。
- 413 显示音频/请求体过大。
- 429/5xx/网络超时显示临时服务错误。

提示用户连接测试会发出一次极小的真实 API 请求；不在日志里写 key、Base64 或完整 response body。

## 5. Config 与迁移

在 `src/app/globals.h::Config` 增加：

```cpp
std::wstring maiApiProvider = L"openrouter";
std::wstring maiOpenRouterApiKey;
std::wstring maiAzureEndpoint;
std::wstring maiAzureApiKey;
std::wstring maiLanguage = L"auto";
```

配置文件字段：

```json
{
  "mai_api_provider": "openrouter",
  "mai_openrouter_api_key": "<DPAPI ciphertext>",
  "mai_azure_endpoint": "",
  "mai_azure_api_key": "<DPAPI ciphertext>",
  "mai_language": "auto"
}
```

加载规则：

- 缺少 `mai_api_provider` 时默认 `openrouter`。
- 未知通道归一化为 `openrouter`，但 Settings 状态栏提示配置已修正。
- `mai_language` 只允许 `auto/zh/en/yue`，未知值归一化为 `auto`。
- 两个 key 分别调用现有 `llm::DecryptString()`。
- Azure Endpoint trim 尾部 `/`；不在 LoadConfig 时拼 REST path。
- runtime-only attempt/diagnostic 字段继续由现有 `Config` snapshot 传递，不新增持久字段。
- 不改变现有 `asr_backend`、`fallback_asr_backend` 或 `cloud_provider` 默认值。

保存规则：

- 两个 key 分别调用现有 `llm::EncryptString()`。
- 即使当前选择 OpenRouter，也保留 Azure Endpoint/Key；反之亦然。
- 不保存完整请求 URL、请求体、音频或 transcript。

## 6. 代码分层

### 6.1 新增协议客户端

新增：

```text
src/asr/mai_transcribe.h
src/asr/mai_transcribe.cpp
```

不为两个通道建立接口/工厂层；一个小型 client 按 `apiProvider` 分支即可。

建议接口：

```cpp
namespace mai_transcribe {

enum class ApiProvider {
    OpenRouter,
    AzureSpeech,
};

struct Config {
    ApiProvider apiProvider = ApiProvider::OpenRouter;
    std::wstring apiKey;
    std::wstring azureEndpoint;
    std::wstring language;
    uint64_t attemptId = 0;
    audio_diagnostics::StageKind diagnosticStageKind =
        audio_diagnostics::StageKind::Primary;
    unsigned diagnosticStageIndex = 0;
};

struct Result {
    bool ok = false;
    bool retryable = false;
    std::wstring text;
    std::wstring error;
    DWORD statusCode = 0;
    DWORD winhttpError = 0;
    std::string providerCode;
    double elapsedMs = 0.0;
    size_t networkBytes = 0;
};

Result Recognize(const std::vector<BYTE>& pcm,
                 const Config& config,
                 DWORD timeoutMs,
                 CloudHttpCancellation* cancellation);

TestResult TestConnection(const Config& config);

// Pure protocol helpers exposed only for the existing protocol test target.
std::string BuildOpenRouterJsonForTest(...);
std::vector<BYTE> BuildAzureMultipartForTest(...);
std::wstring ParseOpenRouterTextForTest(...);
std::wstring ParseAzureTextForTest(...);

} // namespace mai_transcribe
```

客户端职责：

- 验证选中通道所需凭据。
- 把 PCM 封装为 WAV。
- 构造 OpenRouter JSON 或 Azure multipart。
- 使用 `CloudHttpRequest` 发请求。
- 解析 provider response/error。
- 返回 retryable、status、providerCode、elapsed 和 networkBytes。
- 支持 `CloudHttpCancellation` 中断 WinHTTP。

客户端不负责：

- 加载/执行 VAD。
- 操作 HUD、窗口、剪贴板或 `g_config`。
- 决定 fallback。
- 执行 LLM。
- 保存诊断文件。

### 6.2 Batch session

在 `src/asr/asr_session.h` 增加：

```cpp
AsrSessionBackend::MaiBatch
```

在 `src/asr/asr_session.cpp` 增加：

```cpp
class MaiAsrSession final : public BatchAsrSessionBase
```

`Finish()` 按现有 Baidu/MiMo/Qwen Audio batch 模式执行：

1. 检查 abort。
2. 校验当前通道凭据。
3. `uploadPcm = pcm_`。
4. 复用 `TrimBatchPcm16WithVad()`。
5. VAD no-speech 返回空文本，走现有 no-speech 分类，不启动 fallback。
6. 复用 `asr_diagnostics::MakeStageMetadata()` 和 `RegisterInput()`。
7. 使用 `ComputeCloudAsrRecordedRequestTimeoutMs()` 计算 timeout。
8. 调用 `mai_transcribe::Recognize()`。
9. transient transport/429/5xx 最多重发同一段 PCM 一次。
10. 每次请求分别完成 primary/retry diagnostic stage。
11. 成功文本调用 `NormalizeAsrText()`。
12. 返回 `cloudApiMs`、`vadMs`、`vadModelName`、`vadTrimmedSamples`。

Factory：

```cpp
if (config.asrBackend == L"mai") {
    return std::make_unique<MaiAsrSession>(config, localEngine);
}
```

不要新增 `IStreamingAsrSession`，也不要在 `main.cpp` 新建 MAI 专属 worker。

### 6.3 主流程

MAI 不加入 `IsStreamingCloudBackend()`，自然走现有 batch 流程：

```text
BeginAsrAttempt
  -> capture PCM
  -> RecognizeAsync
  -> RunBatchAsrOnce
  -> CreateBatchAsrSession
  -> MaiAsrSession::Finish
  -> fallback policy
  -> DispatchAsrFinalText
  -> optional LLM
  -> paste/input
```

`RecognizeAsync()` 当前用排除列表决定是否搬移 `g_streamingVadSamples`。该缓存实际上只被 `LocalAsrSession` 消费；实施时先验证并把条件收窄为 `config.asrBackend == L"local"`，不要再追加一个 `!= L"mai"` 特例。

除这项小型根因修正外，`src/app/main.cpp` 不应出现 MAI 请求构造、response parse 或 provider retry。

## 7. 本该直接复用的现有链路

| 能力 | 现有实现 | MAI 动作 |
|---|---|---|
| WASAPI/waveIn 录音 | `src/audio/engine.cpp`、`wasapi_capture.*` | 原样复用 |
| 48 kHz 等输入转 16 kHz/PCM16/mono | 现有 capture 层 | 原样复用 |
| 短录音判断 | `StopRecordingSession()` 的 8000-byte 边界 | 原样复用 |
| Batch PCM 收集 | `BatchAsrSessionBase::EnqueuePcmChunk()` | 原样复用 |
| Batch VAD | `TrimBatchPcm16WithVad()` | 原样复用 |
| WAV 构造 | `audio_diagnostics::BuildPcm16MonoWav()` | 直接复用，不复制第三份 RIFF builder |
| JSON escape/Unicode decode | `src/core/utils.h` | 复用 `EscapeJson`、`ExtractJsonStringDecoded` |
| HTTP 生命周期 | `cloud_http_common.*` | 复用 WinHTTP、timeout、cancel、响应上限 |
| 超时计算 | `ComputeCloudAsrRecordedRequestTimeoutMs()` | 原样复用 |
| retry 状态判断 | `IsTransientCloudHttpError()`、`IsRetryableCloudHttpStatus()` | 原样复用；provider 特殊状态只补小表 |
| 结果 normalize/classify | `asr_result.*` | 增加 MAI 名称/前缀后复用 |
| fallback | `BuildFallbackConfig()`、`ShouldRunFallback()`、`RunConfiguredAsrOnce()` | 原样复用 |
| Final 分发 | `asr_dispatcher.*` | 原样复用 |
| LLM gate | `ShouldRunLlmRefine()` | 原样复用，不默认开启 |
| 录音诊断 | `asr_diagnostics.*`、`audio_diagnostics.*` | 增加 model/transport 名称后复用 |
| Settings provider 子页 | `g_*Controls` + `ShowCloudSubPage()` | 按现有模式增加 MAI 三组控件 |
| 异步连接测试 | `g_sharedTestGeneration`、`PostSharedTestResult()` | 原样复用 |
| 调试日志 | `asr_runtime_log.*` | 写 `mai_asr_debug.log`，沿用旋转/隐私规则 |
| 开发者音频复现 | `tools/asr_audio_replay.*` | 增加 `mai` backend |
| 构建与运行布局 | `CMakeLists.txt`、`build.bat` | 原样复用，无新增 runtime DLL |

### 7.1 不应该错误复用的东西

- 不直接调用 `qwen_audio_http::BuildWavForPcm()`：它属于 Qwen provider namespace；使用现有通用 WAV builder。
- 不直接复用 Qwen/MiMo response parser：MAI 两个 API 的 envelope 不同。
- 不把 OpenRouter 当作现有 LLM Provider：ASR 使用 `/audio/transcriptions`，不是 `/chat/completions`。
- 不复用 `IStreamingAsrSession` 或 partial HUD callback：两个正式通道没有录音中 partial。
- 不复用 Qwen 的连接池/prewarm：MAI 第一版是一次 HTTP 请求，没有长期连接 owner。
- 不复用 Azure Voice Live 协议来实现 Azure Fast REST；两者生命周期完全不同。

## 8. 请求构造与安全边界

### 8.1 WAV

- 输入必须是 16 kHz、16-bit、mono PCM。
- 奇数 PCM byte count 先报错，不静默上传损坏音频。
- 使用 `audio_diagnostics::BuildPcm16MonoWav()`。
- 检查 `size_t -> DWORD/uint32_t` 溢出。

### 8.2 OpenRouter Base64 JSON

- 使用 Windows CryptoAPI `CryptBinaryToStringA(..., CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF)`。
- `crypt32` 已是 VoxType 的现有依赖，不新增库。
- 在分配 Base64 和 JSON 前做上界计算。
- 第一版设置一个编译期请求体安全上限，不增加 Settings 项；live 413 数据证明需要调整时再改常量。
- 不记录 JSON body 或音频 Base64。

### 8.3 Azure multipart

- multipart body 使用 `std::vector<BYTE>` 构造，不引入 multipart 库。
- boundary 每次请求唯一；测试 helper 允许注入固定 boundary，保证 deterministic assertion。
- 严格使用 CRLF。
- `definition` JSON 和 `audio` part 分开，音频字节不能经过字符串 API。
- Content-Disposition filename 使用固定 ASCII `recording.wav`，不放用户路径。
- 不把 Azure Key 放 query string。

### 8.4 Endpoint 校验

OpenRouter：

- host/path 固定，不接受用户覆盖。

Azure：

- 仅 HTTPS。
- host 必须非空。
- 用户输入保存为 resource root。
- 拒绝 username/password、fragment。
- query 由客户端生成，避免 API version 被用户输入覆盖。
- 不禁用证书验证。

## 9. 错误、retry 与 fallback

统一前缀：

```text
MAI ASR error: ...
MAI ASR failed: ...
```

加入 `asr_result_policy::LooksLikeOperationalPrefix()`，确保错误文本不会被粘贴到用户应用。

分类建议：

| 情况 | 分类 | 内部 retry | VoxType fallback |
|---|---|---:|---:|
| missing selected API key | AuthOrConfig | 否 | 是 |
| invalid Azure endpoint | AuthOrConfig | 否 | 是 |
| HTTP 401/403 | AuthOrConfig | 否 | 是 |
| OpenRouter 402 | AuthOrConfig/ProviderError | 否 | 是 |
| HTTP 404 model/path | AuthOrConfig | 否 | 是 |
| HTTP 413 | ProviderError | 否 | 是 |
| HTTP 408/425/429 | Timeout/ProviderError | 一次 | retry 仍失败后是 |
| HTTP 5xx/524/529 | ProviderError | 一次 | retry 仍失败后是 |
| DNS/connect/TLS/timeout | Network/Timeout | 一次 | retry 仍失败后是 |
| 2xx + empty text | NoSpeech | 否 | 否 |
| Azure `combinedPhrases` 空 | NoSpeech | 否 | 否 |
| malformed 2xx JSON | ProviderError | 否 | 是 |
| self abort/stale attempt | Cancelled | 否 | 否 |

内部 retry 必须重发相同的 VAD 输出 PCM，不重新录音、不重新执行 VAD、不改变 attempt id。

## 10. Diagnostics 与日志

`asr_diagnostics` 增加：

```text
backend   = mai
model     = microsoft/mai-transcribe-2      // OpenRouter
model     = MAI-Transcribe-2                // Azure
transport = batch_http_wav_base64_json      // OpenRouter
transport = batch_http_wav_multipart        // Azure
```

每个 stage 记录：

- 原始/VAD 后 PCM bytes。
- HTTP status、WinHTTP error、provider code。
- request elapsed。
- network body bytes，但不保存 body。
- 选择的 API provider。
- retry 原因。

日志允许：

- attempt id。
- provider、model、transport。
- host/path（不含 key/query secret）。
- status、request id、elapsed、byte count。
- transcript 字符数。

日志禁止：

- API Key。
- Authorization/Ocp-Apim header。
- Base64/WAV/multipart body。
- 完整 transcript。
- Azure response 全文；只保留经过长度限制和敏感字段过滤的 error 摘要。

## 11. 文件影响矩阵

### 11.1 新增文件

| 文件 | 用途 |
|---|---|
| `src/asr/mai_transcribe.h` | 两个 API 通道的配置、结果和纯协议 helper 声明 |
| `src/asr/mai_transcribe.cpp` | WAV 请求封装、HTTP、response/error parse、Test Connection |

### 11.2 修改文件

| 文件 | 变更 |
|---|---|
| `src/app/globals.h` | Config 字段、control IDs、MAI control vectors/visible flags 声明 |
| `src/app/main.cpp` | 定义 MAI UI globals；将 local VAD sample 条件收窄为 local-only；不加入 provider 协议逻辑 |
| `src/audio/engine.cpp` | MAI Config Load/Save、DPAPI、归一化 |
| `src/ui/settings.cpp` | backend/provider 选项、MAI 子页、通道切换、校验、Test Connection、DPI 布局 |
| `src/asr/asr_session.h` | `AsrSessionBackend::MaiBatch` |
| `src/asr/asr_session.cpp` | `MaiAsrSession` 和 batch factory 分支 |
| `src/asr/asr_result.cpp` | display/debug/log name、fallback support |
| `src/asr/asr_result_policy.h` | MAI operational error 前缀 |
| `src/asr/asr_diagnostics.cpp` | MAI model/transport 元数据 |
| `tools/asr_audio_replay.cpp` | 显式 `mai` replay/backend 选择和帮助文字 |
| `CMakeLists.txt` | production、replay、protocol test source 清单 |
| `tests/asr_json_protocol_test.cpp` | OpenRouter JSON、Azure multipart、response/error 回归 |
| `ARCHITECTURE.md` | 实施完成后补 MAI 两条 batch 路径和 partial 边界 |
| `CHANGELOG.md` | 发布时记录新 provider；版本号按项目规则另行决定 |

无需修改：

- `cmake/VoxTypeRuntime.cmake`：没有新增 DLL/资源。
- `src/asr/asr_dispatcher.*`：现有 final 分发足够。
- `src/asr/asr_streaming_session*`：第一版没有 streaming。
- `src/audio/wasapi_capture.*`：输入格式已满足。

## 12. 分阶段实施

### Phase 0：协议锁定与真实 OpenRouter 探针

目标：在写 Settings 之前固定真实请求/响应。

工作项：

1. 使用用户已有 OpenRouter Key 和生成的短 WAV 请求 `microsoft/mai-transcribe-2`。
2. 保存脱敏后的 status、response 字段名、usage 结构、request id。
3. 验证 `language=zh` 和省略 language。
4. 验证 silence/空文本行为。
5. 验证无效 key、额度不足、未知 model、过大 body 的错误 envelope。
6. 确认 OpenRouter 是否返回 provider-specific headers，可用于诊断但不能成为功能依赖。

验收：形成可放入 protocol test 的脱敏 fixture；没有把 key/audio/完整 transcript 写进仓库。

### Phase 1：纯协议 client 与离线测试

目标：完成两个 batch request builder 和 response parser，不接 UI。

工作项：

- 新增 `mai_transcribe.*`。
- OpenRouter JSON builder。
- Azure multipart builder。
- 两种 endpoint/header 构造。
- 两种 success/error parser。
- cancellation、timeout、retryable metadata。
- 在 `asr_json_protocol_test` 增加 fixtures。

验收：无需 key 的测试覆盖所有 builder/parser 分支；malformed input 不崩溃。

### Phase 2：Batch session、诊断和 fallback

目标：让 `Config{asrBackend=mai}` 能通过生产 session 完成一次识别。

工作项：

- `MaiAsrSession`。
- 公共 Batch VAD。
- 一次 transient retry。
- `AsrSessionBackend::MaiBatch`。
- result/error/fallback mapping。
- diagnostics metadata。
- replay tool。

验收：OpenRouter 主后端成功；故意断网/无效 key 能进入现有 fallback；no-speech 不触发 fallback。

### Phase 3：Settings 与配置持久化

目标：用户只用 OpenRouter Key 即可完成配置。

工作项：

- Recognition backend/fallback 选项。
- Cloud Provider 子页。
- API 通道 combo。
- 两组凭据的独立显示、保存、Show/Hide。
- Language。
- 动态 final-only 提示。
- 当前通道校验和 Test Connection。
- DPAPI Load/Save。

验收：OpenRouter 模式不要求 Azure 字段；切换通道再切回时值不丢失；旧 config 行为不变。

### Phase 4：Azure Fast Transcription live 验证

前置：获得可调用 MAI-Transcribe-2 的 Azure Speech resource、Endpoint 和 Key。

工作项：

- 验证 API version、model name 和 endpoint path。
- 验证 `clean`、auto locale、zh-CN/en-US/yue locale。
- 验证 `combinedPhrases`、silence、401/403/404/429/5xx envelope。
- 验证 Azure public preview 的地区可用性。

没有 Azure 凭据时可以完成编译和离线协议测试，但不能把 Azure 通道标记为“真实服务已验证”。

### Phase 5：Azure Voice Live partial 可行性研究（独立，可取消）

不与 Phase 1–4 混合提交。

仅做一个 developer probe，复用现有 `winhttp_websocket::Transport`：

- WebSocket handshake/header。
- `session.update` 配置 input transcription。
- 持续 append PCM。
- 分别测试未 commit、短段 commit、最终 commit。
- 记录 delta/completed 的时间和 item_id，不保存 transcript。

决策：

- 能固定 v2 且录音期间稳定 delta：再规划 `MaiStreamingSession`。
- 只能 commit 后 delta：不做，batch 已覆盖相同用户体验且更简单。
- 只能使用不透明 `mai-transcribe`：不作为 MAI-Transcribe-2 功能发布。

## 13. 自动化测试清单

### OpenRouter builder/parser

- WAV Base64 无 CRLF。
- model 固定且无法被 config 注入覆盖。
- Auto 不产生 `language`。
- zh/en/yue 映射正确。
- JSON 特殊字符和 Unicode。
- `{text: ...}` 成功响应。
- 2xx empty text。
- 400/401/402/403/404/413/429/5xx envelope。
- HTML/non-JSON error body。
- 大小/整数溢出保护。

### Azure multipart/parser

- boundary 与 header 一致。
- CRLF、closing boundary 正确。
- audio part byte-for-byte 等于 WAV。
- definition part model 固定为 `MAI-Transcribe-2`。
- Auto 省略 locales。
- clean style 固定。
- `combinedPhrases` 单项、多项、空数组、缺失字段。
- Unicode escape 和 malformed JSON。
- endpoint root/path/query 拼接。
- 拒绝 HTTP、userinfo、fragment。

### Session/result

- abort 中断 active request。
- transient 只 retry 一次。
- retry 使用相同 PCM/VAD 输出。
- no-speech 不 fallback。
- operational error 进入 fallback。
- stale attempt 不粘贴迟到文本。
- primary/fallback diagnostic stage 不冲突。

### Config/Settings

- 无 MAI 字段的旧 config。
- 只有 OpenRouter Key。
- 两套 key 同时存在并来回切换。
- 未知 `mai_api_provider`/language 归一化。
- DPAPI round trip。
- 100%/125%/150%/200% DPI 无裁切和重叠。
- Settings 打开时快捷键仍按现有规则暂停。

## 14. 手工 smoke test

OpenRouter：

1. 中文短句。
2. 中英混说。
3. 粤语。
4. 专有名词和数字/日期。
5. 1 秒、10 秒、60 秒录音。
6. 头尾静音、纯静音、背景噪声。
7. 连续快速录音。
8. 断网、代理、超时、额度不足、无效 key。
9. MAI primary + Local fallback。
10. Streaming provider primary + MAI fallback。

Azure 获得凭据后重复同一组，额外验证 Endpoint、区域和 locale。

性能记录：

- recording duration。
- WAV/request bytes。
- release-to-final latency P50/P95。
- provider API elapsed。
- retry/fallback 次数。
- CER/人工错误数。

## 15. 构建与发布验收

使用唯一权威流程：

```text
build.bat --test
```

只能运行：

```text
build/run/x64-release/VoxType.exe
```

完成定义：

- 全量编译、现有测试和新增协议测试通过。
- build layout 校验通过。
- Settings DPI 检查通过。
- OpenRouter 使用真实 Key 完成至少一组 smoke test。
- Azure 没有真实凭据时，在计划/CHANGELOG 中明确写“离线实现，未 live verified”；不能宣称已验证。
- API Key、音频、完整 transcript 不出现在 git diff、日志 fixture 或诊断 JSON。
- `ARCHITECTURE.md` 更新两条 batch 路径。
- 用户可见发布时再按项目规则更新 `src/app/resource.h`、README 版本和 CHANGELOG；计划阶段不改版本号。

## 16. 回滚策略

- MAI 是新增 backend；回滚时从 backend/provider 下拉隐藏即可，不影响既有 provider。
- OpenRouter/Azure credential 字段为追加配置，旧版本会忽略。
- provider client/session 独立，回滚不触碰 capture、dispatcher、fallback 核心实现。
- 若 Azure live probe 失败，只禁用 Azure 选项，OpenRouter 继续可用。
- 若 OpenRouter 模型下架或价格变化，错误进入现有 fallback；不自动改用其他 OpenRouter 模型。

## 17. 官方资料

检索日期：2026-09-04。

- OpenRouter model：`https://openrouter.ai/microsoft/mai-transcribe-2`
- OpenRouter STT guide：`https://openrouter.ai/docs/guides/overview/multimodal/stt`
- OpenRouter transcription API：`https://openrouter.ai/docs/api/api-reference/stt/create-transcription`
- Microsoft MAI-Transcribe：`https://learn.microsoft.com/en-us/azure/ai-services/speech-service/mai-transcribe`
- Azure Fast Transcription API：`https://learn.microsoft.com/en-us/azure/ai-services/speech-service/fast-transcription-create`
- Azure Speech REST transcribe：`https://learn.microsoft.com/en-us/rest/api/speechtotext/transcriptions/transcribe`
- Azure Voice Live how-to：`https://learn.microsoft.com/en-us/azure/ai-services/speech-service/voice-live-how-to`
- Azure Voice Live API reference：`https://learn.microsoft.com/en-us/azure/ai-services/speech-service/voice-live-api-reference-2026-04-10`
