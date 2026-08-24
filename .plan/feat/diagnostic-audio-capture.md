# 失败录音留样与 ASR 复用实施计划

> 状态：已实施（2026-08-24）  
> 创建日期：2026-08-24  
> 适用范围：WASAPI / waveIn 采集、所有本地与云端 ASR、Streaming 重试与 fallback 诊断

## 1. 结论先行

VoxType 有必要重新引入本地录音保存能力，但用途应从早期的“每次覆盖一个临时 WAV”调整为受控的诊断留样：

1. 默认不保存用户语音。
2. 推荐提供 `Failures only`，仅在异常结果出现时留样。
3. `All recordings` 仅供开发或用户显式排障使用。
4. 保存完整采集 PCM，并对 primary、provider 内部 retry、fallback 各自登记实际使用的 ASR 输入。
5. 保存音频摘要、设备信息、每个识别阶段的终态和输入哈希，使同一份音频可以试听、跨后端识别和云端重放。
6. 文件写入、哈希和清理必须在录音结束后的工作线程执行，不能进入 WASAPI/waveIn 回调或 UI 热路径。

本功能必须是公共 `audio_diagnostics` 服务，不能写进任何单一 provider session。第一版完成标准就是覆盖当前全部 ASR 接口：Local、Baidu、Qwen 的全部 transport、Volcengine、MiMo、Doubao IME Free、Qwen IME Free，以及它们作为 primary、内部 retry 或 fallback 时的路径。不能先做一个仅对 Qwen 生效、其他后端以后再补的半公共实现。

## 2. 问题背景

2026-08-24 11:08:37 的 Qwen Audio 3 Streaming 录音具有以下现象：

- WASAPI 采集持续约 5.7 秒，得到 182720 字节 16 kHz/16-bit/mono PCM；
- primary 请求发送完成但没有文本；
- VoxType 新建 WebSocket，重放同一长度的 PCM；
- retry 仍无文本，最终分类为 `no_speech`；
- 前后相邻录音正常，Windows 事件中没有设备断开、PnP 或 USB 异常。

现有日志能证明录音长度、网络握手和重放流程完成，但不能回答：

- PCM 中是否真的存在清晰语音；
- 麦克风是否音量过低、静音或选错默认设备；
- 多声道降混是否造成抵消；
- primary 与 retry 是否发送了逐字节相同的数据；
- provider 是正常 `task-finished` 空文本，还是明确返回 `ASR_RESPONSE_HAVE_NO_WORDS`；
- VAD、缓冲或重采样是否改变了实际发送内容。

保留失败音频和结构化元数据后，上述问题可以离线复现，不必等待下一次偶发故障。

## 3. UI 入口

### 3.1 主入口位置

放在：

```text
Settings
  └─ General
       ├─ Shortcut
       ├─ Startup
       └─ Diagnostics
```

在 `Startup` 分组下方新增 `Diagnostics` group box。录音留样是采集层和所有 ASR provider 共用的诊断功能，不放在 `Cloud ASR → Qwen` 页面。

建议控件：

```text
Recording diagnostics   [Off | Failures only | All recordings]

[Open recordings folder]  [Delete saved recordings...]

Audio stays on this PC. Old diagnostic recordings are removed automatically.
```

行为约束：

- `Open recordings folder` 是立即执行按钮，不要求先点击 Settings 的 `Save`。
- 目录不存在时先安全创建，再通过 Explorer 打开。
- `Delete saved recordings...` 必须二次确认，只删除 VoxType 自己管理并符合命名规则的文件组，不能清空未知文件。
- `All recordings` 选中时显示明确的隐私提示。
- 首版不增加托盘菜单入口，避免托盘菜单继续膨胀；如后续真实使用频率较高，再考虑仅在 Debug Mode 下显示 `Open diagnostics folder`。

### 3.2 Settings 布局

在 `src/app/globals.h` 的 `UiStyle` 中增加命名常量，例如：

- `GeneralDiagnosticsGroupY`
- `GeneralDiagnosticsGroupH`
- `GeneralDiagnosticsModeY`
- `GeneralDiagnosticsActionsY`
- `GeneralDiagnosticsHintY`

禁止在 `settings.cpp` 内散落新的魔法数字。当前 General 页只有 `Shortcut` 和 `Startup` 两个 group，`Startup` 下方到 footer 之间有足够空间容纳一个约 150 px 的 Diagnostics group；仍需验证 100%、125%、150%、200% DPI 下不裁切、不覆盖 footer。

建议新增控件 ID：

```cpp
IDC_DIAGNOSTIC_AUDIO_MODE
IDC_DIAGNOSTIC_AUDIO_OPEN_FOLDER
IDC_DIAGNOSTIC_AUDIO_DELETE
```

## 4. 保存目录与文件结构

### 4.1 规范目录

Installed build：

```text
%LOCALAPPDATA%\VoxType\diagnostics\audio\
```

Portable build：

```text
<portable-root>\diagnostics\audio\
```

实现上由 `MutableDataDir()` 派生 `DiagnosticAudioDir()`。禁止把录音、JSON sidecar 或人工样本写入 `build/`。

### 4.2 文件组

文件名不包含转写文本、窗口标题或输入框内容：

```text
20260824-110837.788-attempt13-no_speech-capture.wav
20260824-110837.788-attempt13-no_speech-input01.wav
20260824-110837.788-attempt13-no_speech-input02.wav
20260824-110837.788-attempt13-no_speech.json
```

- `capture.wav`：录音结束时保存的完整 16 kHz、16-bit、mono PCM，即当前 `g_audioData`/active attempt 持有的规范采集音频。
- `inputNN.wav`：primary、内部 retry 或 fallback 实际使用的 PCM。按 SHA-256 去重；多个阶段使用相同输入时只生成一个文件，并由 JSON stage 引用同一 artifact。
- `.json`：结构化诊断元数据、所有唯一音频 artifact 的哈希，以及各 ASR stage 对 artifact 的引用。

如果某个阶段的输入与 `capture.wav` 完全一致，则该 stage 直接引用 capture artifact，不重复生成 WAV。Streaming provider 如果进行了有损编码，例如 PCM 转 Opus，首版仍保存编码前的规范 provider 输入 WAV，同时在 JSON 中记录 transport 编码和网络发送字节数。不要把 Opus 网络帧伪装成 WAV。

## 5. 保存策略

配置字段建议使用字符串而非多个布尔值：

```json
"diagnostic_audio_mode": "off"
```

允许值：

| 值 | 行为 |
| --- | --- |
| `off` | 不保存录音，只写普通日志 |
| `failures` | 保存符合异常策略的录音，推荐排障模式 |
| `all` | 保存每次录音，仅供显式开发/诊断 |

新增配置项必须同步：

- `src/app/globals.h` 的 `Config`；
- `src/audio/engine.cpp` 的 LoadConfig/SaveConfig；
- `src/ui/settings.cpp` 的控件加载、保存和校验。

### 5.1 `Failures only` 触发条件

首版建议保存：

- WASAPI/waveIn 在首个 PCM 前全部启动失败：只保存 JSON manifest，记录尝试后端、终止后端、phase、错误码和已知设备/格式信息，不生成空 WAV；
- 最终为 `NoSpeech`，且录音/实际 ASR 输入达到 3 秒；
- empty final 触发 replay 后仍为空或收到 provider `NO_WORDS`；
- 已采集有效长度 PCM，但最终发生 transport/provider timeout、peer close、send/receive failure；
- WASAPI/waveIn runtime failure，保存故障前已获得的 partial PCM；
- primary 与 retry/fallback 对同一音频给出矛盾结果时，可记录为诊断样本。

不保存：

- `Too short`；
- 用户主动 abort/cancel；
- stale attempt；
- Settings 打开导致的预期取消；
- 没有 PCM 的鉴权或配置错误（采集启动失败是上一条明确列出的 JSON-only 例外）。

其中“3 秒”应复用公共常量或按 PCM 字节数计算，不在多个 provider 中分别硬编码。

### 5.2 留存上限

首版采用固定安全上限，暂不增加更多 Settings 控件：

- 最多 20 个完整文件组；
- 总大小最多 100 MiB；
- 最长保留 7 天；
- 任一条件超限时按时间删除最旧的完整文件组。

清理器只识别本模块生成的 manifest/命名模式；目录中的未知文件必须保留并写日志提示，不能顺手删除。

## 6. 音频与设备诊断指标

### 6.1 采集摘要

每次录音维护 `AudioCaptureDiagnostics`，至少包含：

- capture backend：`wasapi` / `wavein`；
- 实际设备 friendly name；
- 设备 ID 的 SHA-256，不把完整稳定标识直接写日志；
- 是否使用系统默认设备；
- 原生 sample rate、channels、bits、float/PCM；
- 输出格式固定为 16000 Hz、mono、PCM16；
- 原生帧数、输出采样数、最终 PCM 字节数；
- RMS dBFS、peak dBFS、零采样比例、接近静音的帧比例、削波比例；
- WASAPI `AUDCLNT_BUFFERFLAGS_SILENT` 包/帧数；
- `AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY` 次数；
- 最大 callback 间隔、首个非静音包延迟；
- 每个原生声道的 RMS，以及降混后 RMS。

每声道 RMS 很重要：若各声道有明显信号，而平均为 mono 后能量突然下降，可以定位多声道相位抵消，而不是误判为麦克风完全无声。

统计只做 O(n) 累加，不在回调中计算 SHA-256、写文件、格式化 JSON 或获取设备属性。

### 6.2 所有 ASR 阶段摘要

至少记录：

- stage kind：`primary`、`internal_retry` 或 `fallback`；
- backend、model、transport、chunk size；
- VAD 是否启用、模型、是否检测到 speech、trim 前后字节数；
- 每个 stage 的输入 artifact、输入字节数和 SHA-256；
- retry/fallback 原因、stage index、重放字节数和 SHA-256；
- 每个 stage 的精确终态；
- `task-finished`、`task-finished-empty`、`provider-no-words`、timeout、peer-close、transport-error 必须分开；
- text chars、committed text chars，只记录长度，不保存实际文本；
- provider error code 和经过清洗/截断的错误类别。

如果某个 provider 声称 retry 重放同一段 PCM，而 primary 与 retry 的输入哈希不同，必须写高优先级诊断事件。fallback 可以因自身 VAD/前处理策略使用不同输入，但差异必须在 stage 元数据中可见。

## 7. JSON sidecar 建议格式

```json
{
  "schema_version": 1,
  "capture_id": "20260824-110837.788-attempt13",
  "timestamp_utc": "2026-08-24T03:08:37.788Z",
  "app_version": "0.x.x.x",
  "attempt_id": 13,
  "backend": "qwen",
  "model": "qwen-audio-3.0-asr-flash-streaming",
  "transport": "audio_streaming",
  "result_kind": "no_speech",
  "capture": {
    "backend": "wasapi",
    "device_name": "...",
    "device_id_sha256": "...",
    "used_default_device": true,
    "native_rate": 48000,
    "native_channels": 2,
    "native_bits": 32,
    "native_float": true,
    "recording_ms": 5703,
    "pcm_bytes": 182720,
    "rms_dbfs": -18.4,
    "peak_dbfs": -3.2,
    "zero_ratio": 0.01,
    "silent_frames": 0,
    "discontinuities": 0,
    "sha256": "..."
  },
  "audio_artifacts": [
    {
      "id": "capture",
      "file": "...-capture.wav",
      "pcm_bytes": 182720,
      "sha256": "..."
    }
  ],
  "stages": [
    {
      "kind": "primary",
      "index": 0,
      "backend": "qwen",
      "input_artifact": "capture",
      "vad_enabled": false,
      "terminal": "task_finished_empty",
      "sent_bytes": 182720,
      "text_chars": 0
    },
    {
      "kind": "internal_retry",
      "index": 1,
      "backend": "qwen",
      "reason": "empty_final",
      "input_artifact": "capture",
      "terminal": "provider_no_words",
      "sent_bytes": 182720,
      "text_chars": 0
    }
  ]
}
```

禁止写入：

- API Key、token、cookie、UTDID；
- 输入框 context、选区文本、窗口正文；
- 完整 transcript；
- 包含用户内容的原始 provider JSON。

## 8. 模块与生命周期设计

### 8.1 公共模块

建议新增：

```text
src/audio/audio_diagnostics.h
src/audio/audio_diagnostics.cpp
```

职责：

- 音频统计累加与快照；
- PCM WAV 封装；
- SHA-256；
- sidecar JSON；
- 保存策略判断；
- 异步原子写入；
- retention 清理；
- `DiagnosticAudioDir()` 和打开目录 helper。

不要让公共模块依赖某个 Qwen/Volc/Doubao client。Provider 通过通用结构提交终态和输入摘要。

建议公共接口围绕一次物理录音建立共享 attempt，而不是让各 provider 各自保存文件：

```cpp
enum class AsrDiagnosticStageKind {
    Primary,
    InternalRetry,
    Fallback,
};

struct AsrDiagnosticInput {
    AsrDiagnosticStageKind kind;
    unsigned stageIndex;
    std::wstring backend;
    std::wstring model;
    std::wstring transport;
    std::shared_ptr<const std::vector<BYTE>> pcm;
    // VAD / encoding / byte-count metadata...
};

struct AsrDiagnosticTerminal {
    AsrDiagnosticStageKind kind;
    unsigned stageIndex;
    std::wstring backend;
    std::string terminal;
    std::string reason;
    size_t textChars;
    // timing / provider-code metadata...
};
```

实际类型名可调整，但必须满足：

- 一次物理录音只有一个 `capture_id` 和一份 capture PCM；
- 同一次录音可以登记任意数量的 primary/internal-retry/fallback stages；
- 音频 artifact 按内容哈希去重；
- provider 只上报数据，不决定路径、不写 WAV、不执行 retention；
- 最终分类层统一决定是否持久化整个 attempt；
- 新增 provider 接入 `IAsrSession` / `IStreamingAsrSession` 时，也必须自然进入同一公共诊断服务。

新增 `.cpp` 必须加入 `CMakeLists.txt`。如使用 Windows CNG 计算 SHA-256，需要同步：

- `#pragma comment(lib, "bcrypt.lib")`；
- `CMakeLists.txt` 链接 `bcrypt`。

### 8.2 全后端接入边界

第一版必须覆盖：

| ASR 路径 | 需要登记的输入 |
| --- | --- |
| Local | 完整 capture 或本地 VAD 后 PCM |
| Baidu batch | 实际上传 PCM |
| Qwen Realtime / Audio HTTP / Audio Streaming | 实际发送 PCM 及每次内部 replay |
| Volcengine streaming / nostream | 实际发送 PCM 及每次内部 replay |
| MiMo batch | 实际上传 PCM |
| Doubao IME Free | Opus 编码前规范 PCM、编码及网络字节元数据 |
| Qwen IME Free | 实际提交 PCM 及每次内部 replay |
| Configured fallback | fallback 后端实际使用的 PCM，stage kind=`fallback` |

如果同一 backend 同时作为 primary 和 fallback 出现，必须依靠 stage kind/index 区分，不能只用 backend 名覆盖记录。

### 8.3 生命周期

```text
StartAudioCapture
  └─ reset AudioCaptureDiagnostics

WASAPI / waveIn callback
  ├─ append existing g_audioData
  ├─ update cheap signal counters
  └─ enqueue streaming PCM

StopAudioCapture
  └─ freeze capture PCM + diagnostics snapshot

ASR primary / retry / fallback
  └─ register stage + report exact input and terminal metadata

Final classification
  └─ DecideDiagnosticAudioSave(...)
       └─ background writer: WAV + JSON + retention
```

不能在回调里执行：

- 磁盘 I/O；
- Explorer/Shell API；
- SHA-256；
- JSON 序列化；
- retention 扫描；
- provider 逻辑。

### 8.4 文件写入完整性

- 先写同目录 `.tmp` 文件，flush/close 成功后原子 rename 为最终文件。
- WAV 和 JSON 全部完成后才把文件组视为有效。
- 进程退出或磁盘满导致残留 `.tmp` 时，下次启动只清理由本模块命名且超过安全时间的临时文件。
- 写入失败不能改变 ASR final、HUD 或粘贴行为，只写 `diagnostic_audio_save_failed`。

## 9. 日志增强

普通 runtime log 增加：

```text
event=capture_summary attempt=13 source=wasapi device_hash=... native_rate=48000 channels=2 pcm_bytes=182720 rms_dbfs=-18.4 peak_dbfs=-3.2 zero_ratio=0.01 silent_frames=0 discontinuities=0
event=retry_start attempt=13 backend=qwen reason=empty_final replay_bytes=182720 replay_sha256=...
event=retry_final attempt=13 backend=qwen terminal=provider_no_words text_chars=0 elapsed_ms=...
event=diagnostic_audio_decision attempt=13 mode=failures save=1 reason=no_speech_after_retry
event=diagnostic_audio_saved attempt=13 capture_file=... sidecar_file=... bytes=...
```

字段命名统一：

- 顶层识别编号使用 `attempt` 或 `asr_attempt`，全项目选定一种；
- 连接重试使用 `connect_attempt`；
- PCM replay 使用 `retry_index`；
- 不能再让三个不同含义都叫 `attempt`。

## 10. 复用方式

### 10.1 人工判断

- WAV 清晰：采集链路基本正常，继续检查 provider/协议。
- WAV 静音或极弱：检查默认输入设备、系统静音、麦克风权限和硬件。
- 原始声道 RMS 正常、mono RMS 很低：检查 downmix。
- `capture.wav` 正常、某个 `inputNN.wav` 异常：检查该 stage 的 VAD/trim/缓冲或编码前处理。

### 10.2 自动离线对照

后续增加一个开发工具，优先复用现有 ASR client/session，而不是另写协议：

```text
tools/asr_audio_replay.bat
tools/asr_audio_replay.cpp
```

建议能力：

- 读取 16 kHz/PCM16/mono WAV；
- 对同一文件运行 Local ASR；
- 选择当前任一 ASR backend 重放；支持 streaming 的后端按真实 cadence 发送，batch 后端按录音请求发送；
- 支持一次运行多个已配置 backend，形成横向对照；
- 输出 provider terminal、识别文本长度、耗时和 PCM SHA-256；
- 默认不把 transcript 写入持久日志。

判断矩阵：

| 留样结果 | Local ASR | 原 backend replay | 结论倾向 |
| --- | --- | --- | --- |
| 清晰语音 | 成功 | 再次 no words | 原 provider/参数/协议问题 |
| 清晰语音 | 成功 | 成功 | 云端偶发漏检 |
| 静音/极低 | no speech | no speech | 设备、输入源或录音环境 |
| capture 正常、ASR input 异常 | 不适用 | no speech | VAD/trim/发送前处理 |
| primary/retry 哈希不同 | 不适用 | 不确定 | replay 实现问题 |

## 11. 隐私与安全

- 默认 `off`，用户必须显式开启。
- Settings 明确提示音频会以可播放 WAV 保存在本机。
- 不自动上传诊断文件，不把录音附加到普通日志。
- 文件名和 JSON 不包含用户说话内容。
- 使用用户级 `%LOCALAPPDATA%`/Portable 数据目录，不写公共目录。
- 提供明确删除入口和自动留存上限。
- 首版为了可试听和重放使用普通 WAV，不使用 DPAPI 加密；如果未来需要默认开启，则必须重新评估加密、播放和导出流程。

## 12. 分阶段实施

### Phase 1：最小可用失败留样

- 新增 `audio_diagnostics.*`、WAV writer、目录与 retention。
- Config/UI 增加 `Off / Failures only / All recordings`。
- General 页增加 Diagnostics group 和打开/删除按钮。
- 保存完整 `capture.wav` 和最小 JSON。
- 建立一次物理录音一个 capture、任意数量 ASR stages 的公共服务和 artifact 去重机制。
- 当前全部 backend 接入公共 stage/input/terminal 接口：Local、Baidu、Qwen 全部 transport、Volcengine、MiMo、Doubao IME Free、Qwen IME Free。
- primary、provider 内部 retry 和 configured fallback 全部登记实际输入及 SHA-256。
- Qwen 的 `empty final → retry → no speech` 作为首个真实故障回归案例，但不是专属实现边界。

### Phase 2：精准采集指标和 ASR input

- WASAPI/waveIn 增加 RMS、peak、zero、silent、discontinuity、callback gap、per-channel RMS。
- 所有 VAD 路径提供实际 `inputNN.wav` 和 trim 元数据。
- 为每个 backend 补齐其协议特有但不含隐私的终态、编码和网络字节元数据。

### Phase 3：开发复用工具

- 增加 WAV replay 工具。
- 支持 Local 与任一已配置 backend 对照，并可选择多 backend 横向重放。
- 可选生成不含 transcript 的诊断摘要。

## 13. 测试与验收

### 单元测试

- WAV header、data size、奇偶字节和空 PCM。
- 静音、正弦波、削波音频的 RMS/peak/zero 统计。
- primary/retry 相同 PCM 得到相同 SHA-256。
- `off/failures/all` 保存决策矩阵。
- retention 按文件组删除，未知文件不删除。
- 磁盘写入失败不影响 ASR final。
- JSON 不包含 context、transcript、key/token 字段。

### 集成测试

- Qwen empty final 后 retry 仍 no speech，生成完整文件组。
- Local、Baidu、Qwen 三种 transport、Volcengine、MiMo、Doubao IME Free、Qwen IME Free 各至少覆盖一次成功和一次可留样失败。
- 每个 provider 的内部 retry 都生成独立 stage；输入相同时复用同一个 audio artifact。
- configured fallback 生成 `fallback` stage，并保留 primary stage，不覆盖前者。
- replay 成功时不在 `Failures only` 模式保存。
- capture runtime failure 保存 partial PCM 并标记失败码。
- VAD disabled 时不重复写与 capture 相同的 `inputNN.wav`。
- VAD enabled 且输入变化时同时生成 capture/input artifact。
- 连续快速录音不会让旧 attempt 覆盖新 attempt 文件。
- 退出程序时不死锁、不阻塞音频回调。

### UI/DPI

- Settings → General 的 Diagnostics group 在 100%、125%、150%、200% DPI 下不裁切。
- `Open recordings folder` 无目录时能创建并打开。
- 删除按钮有确认提示，取消后无文件变化。
- Settings 打开期间仍保持“不拦截录音快捷键”的现有规则。

### 构建

```powershell
.\build.bat
```

只运行规范载荷：

```text
build\run\x64-release\VoxType.exe
```

## 14. 文档与版本

实施时同步：

- `src/app/resource.h` 版本号；
- `README.md` 的隐私、保存目录和 retention 说明；
- `CHANGELOG.md`；
- `ARCHITECTURE.md` 的 Recording/Configuration 章节；
- 如中文文档仍维护，同步 `doc/README_zh.md`、`doc/ARCHITECTURE_zh.md`、`doc/CHANGELOG_zh.md`。

当前 `ARCHITECTURE.md` 仍写着录音会保存到 `%APPDATA%\VoxType\last_recording.wav`，但 `CHANGELOG.md` 记录该 `WriteWavFile` 已在 v0.1.4 删除。实施本功能时必须修正这处历史文档不一致，不能继续描述不存在的行为。

## 15. 完成标准

一次类似 2026-08-24 的 `no_speech after retry` 再次发生时，不需要复现第二次，仅凭文件组即可回答：

1. 实际使用了哪个设备和格式；
2. 录音是否包含足够强度的语音；
3. 是否有静音包、断续或 callback 卡顿；
4. VAD/前处理是否改变了音频；
5. primary 和 retry 是否重放完全相同的 PCM；
6. provider 两次分别以什么精确终态结束；
7. 同一 WAV 在 Local ASR、原 backend 以及任意选定 backend 上的结果是否一致。

达到以上标准，才算真正解决“有日志但仍无法区分设备问题与 provider 漏检”的诊断缺口。

## 16. 实施结果

- 已新增公共 `audio_diagnostics` 与 `asr_diagnostics`，覆盖当前所有 Local/云端 ASR、provider 内部 retry 和 configured fallback，不属于任何单一千问实现。
- 已在 WASAPI/waveIn 采集层记录设备、原生格式、声道/downmix、RMS/peak/zero/silence/clipping、静音包、discontinuity、callback gap 和首个非静音延迟。
- 已实现 `Off` / `Failures only` / `All recordings`、异步原子 WAV/JSON、SHA-256 artifact 去重、20 组/100 MiB/7 天 retention，以及只删除受管文件组的安全入口。
- Settings → General 已增加 Diagnostics 分组、动态隐私提示、打开目录和二次确认删除；96/144/192/288 DPI 布局校验已覆盖。
- 已新增通用 `tools/asr_audio_replay.bat/.cpp`，复用生产 batch/streaming session，支持 Local、当前 primary/fallback 与显式选择的所有现有 backend；云端上传与 transcript 输出均为显式 opt-in。
- streaming provider 全部使用录音开始时冻结的 attempt config；fallback provider 的内部 retry 继续归属 `fallback` stage（index 1、2…），不会覆盖 primary 的 `internal_retry`。
- replay wrapper 显式绑定规范 runtime，Local/VAD bundled assets 与 Portable config 均按 `build/run/x64-release` 解析；直接运行 exe 也支持 `--runtime-dir`。
- 已新增 `audio_diagnostics_test`，覆盖 WAV、PCM 指标、SHA-256、保存决策、去重、VAD 元数据、JSON 隐私、retention 和未知文件保护。
- 审查加固已覆盖首个 PCM 前采集失败的 JSON-only manifest、Qwen/火山 verbose 日志与火山 DNS/TCP 探测隔离、损坏 LLM provider store 原样保留、writer 退出完整 drain、严格受管 WAV/`.tmp` 识别，以及 replay 路径中的 `!` 字符。
- 验证命令：`.\build.bat --test`、`.\build.bat`；唯一可运行开发载荷仍为 `build\run\x64-release\VoxType.exe`，replay 工具只输出到 `build\artifacts\tools`。
