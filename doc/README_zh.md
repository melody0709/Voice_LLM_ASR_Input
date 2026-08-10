<p align="center">
  <img src="../src/app/app.ico" width="64" alt="VoxType icon" />
</p>

<h1 align="center">VoxType</h1>

<p align="center">
  <strong>Local voice input for Windows. Press, speak, paste.</strong><br/>
  Windows 11 本地语音输入工具 — 按住说话，松开粘贴，无需云端
</p>

<p align="center">
  <img src="https://img.shields.io/badge/Platform-Windows%2011-blue?logo=windows" alt="Platform" />
  <img src="https://img.shields.io/github/license/melody0709/VoxType" alt="License" />
  <img src="https://img.shields.io/github/v/release/melody0709/VoxType" alt="Release" />
  <img src="https://img.shields.io/badge/CPU-only-green" alt="CPU Only" />
</p>

<p align="center">
  当前版本：<code>v0.9.24</code> &nbsp;|&nbsp; 🇬🇧 <a href="../README.md">English</a>
</p>

https://github.com/user-attachments/assets/36243dc2-cfc8-41fb-b0cf-6e558f02cd5e

---

## Highlights

- **按住即说** — 默认 CapsLock 长按录音，松开自动粘贴到当前窗口；短按照常切换大小写
- **本地优先** — C++ 直接调用 sherpa-onnx，无需 Python；云端 ASR / LLM 供应商均为显式可选
- **实时 HUD** — 录音时底部显示悬浮胶囊窗，5 根音量条随声音跳动
- **双 VAD 可选** — Silero VAD（轻量）/ FireRed VAD（高精度 F1 97.57），智能跳过静音
- **LLM 纠错（可选）** — 支持 DeepSeek / OpenRouter / SiliconFlow 等多供应商，一键配置
- **Cloud ASR（可选）** — 支持火山引擎（豆包）、百度智能云、Qwen ASR（旧版 `qwen3-asr-flash-realtime`、Audio 3 HTTP `qwen-audio-3.0-asr-flash`、Audio 3 streaming `qwen-audio-3.0-asr-flash-streaming`）、小米 MiMo ASR（`mimo-v2.5-asr`）、实验性豆包输入法 ASR，以及逆向还原的千问输入法（`qwen_free`，需本机已安装千问 IME）作为替代后端，并支持可选 fallback ASR

## Quick Start

### Qwen IME (Free) 独立运行时指南

需要先安装千问输入法初始化本机 UTDID，再迁移私有运行时并卸载千问时，请参阅：

[Qwen IME (Free) 独立运行时指南](qwen_free_standalone_runtime_guide_zh.md)

### 1. 下载模型

```powershell
.\download_models.ps1
```

交互式菜单选择模型，自动下载并解压到 `models/` 目录（约 3GB 磁盘空间）。

> Silero VAD 和 FireRed VAD 已内置，无需下载。

### 2. 构建

```powershell
.\build.bat
```

需要 Visual Studio 2022（C++ 桌面开发工作负载）。

运行离线 Qwen 协议回归测试（JSON 解析、HMAC 签名和 ASR 二段帧，不需要网络或麦克风）：

```powershell
.\build.bat --test
```

### 3. 运行

```powershell
.\build\run\x64-release\VoxType.exe
```

右键托盘图标打开 Settings，按住快捷键开始录音，松开后识别并粘贴。

---

<details open>
<summary><strong>⚙️ Settings 说明</strong></summary>

**Recognition tab**
- `ASR Backend` — 选择 `Local (sherpa-onnx)` / `Volcano Engine` / `Baidu Cloud` / `Qwen ASR` / `MiMo ASR` / `Doubao IME (Free)` / `Qwen IME (Free)`
- `Fallback` — 可选备用 ASR 后端：`Local` / `Baidu Cloud` / `Qwen ASR` / `MiMo ASR` / `Doubao IME (Free)` / `Qwen IME (Free)`。默认 ASR 后端出现 timeout、网络错误、鉴权/配置错误或模型加载错误等运行类失败时，会用同一段原始 PCM 自动重试 fallback；`Too short` 和 `No speech detected` 不触发 fallback。
- `ASR model` — 语音识别模型（仅 Local 后端）：
  - `FireRedASR2 CTC` — 速度快，适合日常输入
  - `FireRedASR2 AED` — 质量更好，长句更准
  - `SenseVoiceSmall` — 轻量模型，适合低资源机器
- `Model folder` — 模型文件存放目录
- `Threads` — 推理线程数，`auto` 自动使用 CPU 核心数（上限 8）
- `Enable VAD` — 开启人声检测，录音前先判断是否有语音，无人声时跳过 ASR 以节省时间
- `VAD model` — 人声检测模型（需先开启 Enable VAD）：
  - `Silero VAD` — 轻量快速，准确率 F1 95.95
  - `FireRed VAD` — 高精度（F1 97.57，误报率 2.69%），模型仅 2.2MB
- `Punctuation` — 识别后处理：
  - `Disabled` — 不处理，输出 ASR 原文
  - `Auto punctuate` — 本地 CT-Transformer 自动补标点（无需网络）
  - `Auto punctuate + LLM` — 本地标点 + 云端 LLM 纠错（需在 LLM tab 配置供应商和 API Key，详见下方 LLM 纠错区块）
- `Hold hotkey` — 点击输入框后按快捷键录入
  - `Esc` 取消本次录入，`Backspace/Delete` 清空快捷键
  - 默认 CapsLock：短按切换大小写，长按 300ms 触发语音输入

**LLM tab**
- `Provider` — 供应商下拉框，内置 DeepSeek / OpenRouter / SiliconFlow 预设，选择后自动填充下方字段
- `API Base URL` — 供应商 API 地址（预设自动填入）
- `API Key` — API 密钥，使用 DPAPI 加密存储到本地配置
- `Model` — 模型名称（如 `deepseek-v4-flash`）
- `Test Connection` — 测试 API 连接是否正常，结果在下方 Status 区域显示
- `Extra Params` — 附加 JSON 参数，合并到请求体中（格式：`"key":"value","key2":"value2"`）。预设供应商会自动注入关闭思考模式参数，用户可在此追加自定义字段
- `[+]` / `[−]` — 添加/删除自定义供应商（预设不可删除）

**LLM Prompt tab**
- `System Prompt` — 多行编辑器，自定义 LLM 纠错的系统提示词（留空使用内置默认）
- `Basic Fix` / `Deep Fix` — 预设按钮，一键填入不同纠错力度的 System Prompt

**Cloud ASR tab**
- `Provider` — 选择 `Volcano Engine (Doubao)` / `Baidu Cloud` / `Qwen ASR (DashScope)` / `MiMo ASR (Xiaomi)` / `Doubao IME (Free)` / `Qwen IME (Free)`，下方控件动态切换
- **Baidu Cloud**：`API Key` / `Secret Key`（DPAPI 加密）+ `Language Model`（普通话/英语/粤语/四川话）+ `Test Connection`
- **Volcano Engine (Doubao)**：`API Key`（DPAPI 加密）+ `ASR Mode` + `Model Version` + `Language` + `Test Connection`
  - ASR Mode：`bigmodel_nostream`（推荐，准确率最高）/ `bigmodel_async`（最佳延迟）/ `bigmodel`（实时部分结果）
  - Model Version：`Seed-ASR 2.0 (duration)` / `Seed-ASR 2.0 (concurrent)` / `BigASR 1.0 (duration)` / `BigASR 1.0 (concurrent)`
  - Hotwords ID/Name、Correct ID/Name — 引用自学习平台热词词表和替换词词表
  - Use history as context — 将最近识别结果作为对话上下文发送，提升准确率
  - Read input field context — 读取当前输入框文本作为 ASR 上下文（UIA/MSAA/WM_GETTEXT 分层 Fallback，输入框优先、历史兜底）
- **Qwen ASR (DashScope)**：`API Key`（DPAPI 加密）+ `Base URL` + `Model` + `Language` + `Chunk ms` + `Test Connection`
  - 新安装默认模型：`qwen-audio-3.0-asr-flash-streaming`；已有配置保持原模型
  - Audio 3 默认使用已配置的北京 Workspace 域名，不提供地域选择项
  - `qwen-audio-3.0-asr-flash-streaming` 在录音期间显示 partial；`qwen-audio-3.0-asr-flash` 松开后提交完整 WAV，只返回 final
  - Turn detection 固定为 Manual，匹配按住说话/松开上屏的输入法场景；Server VAD 设置已隐藏
- **MiMo ASR (Xiaomi)**：`API Key`（DPAPI 加密）+ `Base URL` + `Model` + `Language` + `Test Connection`
  - 默认 Base URL：`https://token-plan-ams.xiaomimimo.com/v1`
  - 默认模型：`mimo-v2.5-asr`；音频会封装为 WAV 后通过 `/chat/completions` 上传
- **Doubao IME (Free)**：无 API Key 输入框。实验 provider 会注册豆包输入法风格设备，保存 device id/cdid 和 DPAPI 加密 token，将 PCM 编码为 Opus，并使用非官方 `frontier-audio-ime-ws.doubao.com` WebSocket 协议；可用性、额度和服务条款不做保证。
  - Doubao IME 绕过本地 VAD，依赖输入法服务自己的分段；一次热键按住期间的 partial HUD 和 final 文本会跨云端分段累计，长录音松手后按合并后的整段结果上屏。作为 Fallback 使用时，Doubao IME 会用同一段原始 PCM 发起 recorded request，并把刷新后的凭据写回配置。
  - Qwen、千问 IME Free、火山引擎和 Doubao IME 的 streaming partial HUD 统一为仅影响显示的清屏模式：三行正文内实时显示，超过后清空前文并从当前最后一句重新开始；新页继续累积到再次超过三行，最终上屏文本仍保持完整。
  - 诊断 probe：运行 `.\tools\doubao_ime_probe.bat` 可编译独立控制台探针，默认复用保存的豆包输入法凭据，执行 live protocol 检查；仓库中存在测试 WAV 时会额外做真实语音识别检查。添加 `--streaming` 可按实时节奏发送 WAV 帧并用 drain 线程验证 partial/final 流式路径；添加 `--fresh` 可强制临时重新注册。
- **Qwen IME (Free)**：无 API Key 输入框。VoxType 本地采集 WASAPI 音频并回放逆向还原的千问协议；鉴权只使用经过 SHA-256 指纹验证的兼容 `unet.dll` 原生签名器。未知 DLL 会被拒绝，不再盲目调用版本相关 RVA，也不再发送已知无效的 HMAC fallback。UTDID 从本机千问缓存或注册表取得，配置中的调试 override 使用 Windows DPAPI 加密保存。当前运行仍需要兼容的千问组件；初始化失败时走统一的备用 ASR 策略。可用性、额度和服务条款不做保证。
  - `Shell install path override` — 可选手动覆盖千问 IME 安装目录（默认从 `C:\Program Files\QianwenIME` 自动探测）。
  - UTDID override 仅保留为配置级诊断入口，不作为普通 Settings 控件展示，并在保存时使用 DPAPI 加密。
  - `Test Connection` 会检查 UTDID 获取和 ASR WebSocket 握手；启用 bundled 后处理时，还会发送一次小型 LLM 探测，并单独报告 LLM 失败。
  - `Polish (auto)` 是 bundled `VoiceInputWrite` 后处理的唯一开关。`Punctuation included` 和 `Correction included` 是只读能力提示，因为原版端点在同一个响应中完成标点和纠错，并不是三个独立 HTTP 请求。实验性的 `Rewrite selection` 代码路径仍保留用于协议研究，但当前在 Settings 中禁用，并且会在配置加载/保存时强制关闭。其请求字段仍是基于逆向证据的兼容映射，需拿到原版同场景真实请求/响应对照后才可作为正式能力启用。
  - `Debug log` 只开启本地千问协议诊断日志，不改变识别结果。
- 云端 ASR 由远端完成识别；启用 VAD 时，Qwen 和火山引擎使用本地 streaming VAD trim，批量云端后端使用 batch VAD trim 后再上传，千问 IME Free/Doubao IME 直接上传完整原始 PCM/Opus，不走本地 VAD，而是依赖服务端分段。本地标点模型在云端后端下仍不生效

</details>

<details>
<summary><strong>📦 支持的模型</strong></summary>

| 模型 | 类型 | 特点 |
|---|---|---|
| FireRedASR2 CTC int8 | ASR | 速度快，适合日常输入 |
| FireRedASR2 AED int8 | ASR | 质量更好，长句更准 |
| SenseVoiceSmall int8 | ASR | 轻量，适合低资源机器 |
| CT-Transformer 标点 int8 | 后处理 | 中英文标点自动补全 |

模型下载链接见 `download_models.ps1` 脚本，或手动从 [sherpa-onnx releases](https://github.com/k2-fsa/sherpa-onnx/releases) 获取。

</details>

<details>
<summary><strong>🤖 LLM 纠错</strong></summary>

v0.2.0 起新增可选的云端 LLM 文本纠错。默认关闭，需手动启用：

1. Settings → LLM tab → 选择供应商，填入 API Key
2. Settings → Recognition → Punctuation 设为 `Auto punctuate + LLM`

特性：
- 预设供应商自动注入关闭思考模式参数
- Extra Params 支持自定义 JSON 片段合并到请求体
- API Key 使用 DPAPI 加密存储
- 向下兼容旧配置格式

</details>

<details>
<summary><strong>🏗️ 项目结构</strong></summary>

```
src/
  app/              — 程序入口、全局声明、Win32 资源
  asr/              — ASR 客户端、批量/流式 session、ASR 结果分发辅助
  audio/            — 本地 ASR 引擎、音频采集、WASAPI、FireRed VAD、流式 VAD trim
  ui/               — HUD、热键、Settings 窗口
  core/             — 共享工具、LLM 纠错、输入框上下文读取
dll/                — 运行时 DLL（sherpa-onnx、onnxruntime 等）
third_party/        — 头文件和导入库
models/             — 模型文件（不提交 git）
build.bat           — Visual Studio 2022 编译脚本
download_models.ps1 — 模型下载脚本
ARCHITECTURE.md     — 架构详细说明
AGENTS.md           — 开发协作者注意事项
CHANGELOG.md        — 版本变更记录
```

</details>

<details>
<summary><strong>⚠️ 已知限制</strong></summary>

- 本地、百度和 MiMo 在录音结束后输出 final；Qwen、火山引擎和 Doubao IME 可在录音期间显示 partial HUD，最终文本仍在松开后上屏
- 文本注入以剪贴板 + Ctrl+V 为主，管理员权限窗口可能拦截
- 模型文件较大（约 3GB），首次加载需要几秒

</details>

<details>
<summary><strong>📋 更新日志</strong></summary>

详见 [CHANGELOG.md](CHANGELOG.md)

**最近更新：**
- **v0.9.9** — 恢复 Local 及其他 batch ASR 后端的共用录音中 HUD 音量动画；音量条重新亮起并跳动，同时保持 v0.9.3 的云端“先启动采集”顺序不变
- **v0.9.8** — 统一 CMake/Ninja 发布链路，提供已验证的 Portable 与 MSI 包、MSI 升级/目录选择、便携版数据隔离和开机自启动设置
- **v0.9.7** — 新增隐私安全、有界轮转的 ASR 诊断日志和结构化 primary/fallback 生命周期事件；流式 primary 在松手前回报失败时会延后到完整 PCM 保存后再决定 fallback，不再绕过已配置的备用后端
- **v0.9.6** — Doubao IME 现在可作为 fallback ASR 目标，通过 recorded-PCM helper 重放同一段录音，支持凭据刷新/写回、云端耗时统计、Settings 选择，并保持 raw PCM 上传不走本地 VAD trim
- **v0.9.5** — Recognition tab 新增 fallback ASR 后端，batch/streaming 失败串行 fallback，fallback 启用时缩短 streaming final 等待，ASR/LLM result metadata 支持 debug，Local 作为 fallback 时预加载模型
- **v0.9.4** — 豆包输入法实验 `doubao_ime` 流式云端 ASR 后端、静态 Opus 1.6.1、凭据 bootstrap/reset UI、protocol/WAV/streaming 诊断 probe、长录音聚合修复，以及 Qwen/火山/Doubao IME 共用的清屏 streaming partial HUD
- **v0.9.3** — 火山引擎快速连续录音头部音频丢失修复、streaming VAD 双处理修复、连接复用/超时调优、active request 快速取消、Qwen/火山启动和停止流程清理
- **v0.9.2** — HUD DPI 适配渲染、火山引擎/Qwen WebSocket 双关和数据竞争修复、Qwen activeClient UAF 修复、重试路径 abort 检查
- **v0.9.1** — 云端 ASR 架构稳定版、小米 MiMo ASR（`mimo-v2.5-asr`）后端、Qwen/火山引擎 streaming session、共享 streaming/batch VAD trim core、百度同 PCM 重试和 token refresh retry、源码目录分类
- **v0.9.0** — Qwen ASR（`qwen3-asr-flash-realtime`）后端、边录边发和 partial HUD、默认 Manual turn detection、Qwen watchdog/replay 重试、统一 ASR session/dispatcher/result 架构
- **v0.8.7** — 火山引擎重试识别：断连音频缓冲 + 全量 PCM 重试、自适应 finalize 超时、统一 ASR 错误分类、无文本 close 处理
- **v0.8.6** — 火山引擎连接复用优化（连续快速录音延迟从 ~1.8s 降到 ~0.4s）、3s 过期检测 + hSession 连接池清理、内部重试时间判断、外部递增重试策略
- **v0.8.5** — 输入框上下文（UIA/MSAA/WM_GETTEXT 读取输入框文本作为 ASR 上下文）、上下文逻辑重构（输入框优先、历史兜底、去掉窗口标题）、移除加速首字参数、设置界面重组
- **v0.8.4** — 修复火山引擎 no-speech drainThread.join() 阻塞 17+ 秒（先关 WebSocket 再 join）、watchdog 线程句柄泄漏崩溃、SendMessage(WM_PASTE) 阻塞 UI 线程
- **v0.8.2** — 修复火山引擎 nostream/async 结果丢失、长录音截断、WinHttpCloseHandle 死锁、逻辑死锁、短音频误报超时、HUD 定时器竞态
- **v0.8.0.1** — 修复火山引擎 nostream 无语音时卡死（WinHttpWebSocketReceive 1ms 超时不生效）
- **v0.8.0** — 流式 VAD（录音期间实时语音检测，跳过静音）、火山引擎 nostream/async 长录音卡死修复、HUD 音量条语音检测变色、watchdog 录音期间续期、代码质量修复
- **v0.7.5** — 火山引擎 WebSocket 挂死修复（forceAbort）、连接预热、连接过期重建、WinHTTP 代理修复、超时调优、日志精简
- **v0.7.4** — 微信中文输入法粘贴修复（WM_CHAR 绕过 IME）、IMM32 输入法状态切换、Unicode SendInput fallback、Force Unicode Input 菜单
- **v0.7.3** — WASAPI Shared Mode 录音（48kHz→16kHz 重采样 + waveIn 降级）、Debug Mode 控制台每阶段计时、百度 ASR 响应解析修复

- **v0.7.2** — 线程安全修复（火山引擎线程 join、百度 Token mutex、VolcDebugLog mutex）、SSL 证书验证恢复、`g_volcAudioCs` 泄漏修复、工具函数去重到 `utils.h`、模型下载器异步化、`AsrEngine::lock` 封装
- **v0.7.1** — `bigmodel_nostream` 加速（跳过中间接收）、WinHTTP 连接复用、`ExtractJsonStr` 转义修复、线程安全修复
- **v0.7.0** — 火山引擎 ASR 全参数支持、热词/替换词表、对话上下文、`corpus` 合并修复、`context` 格式修复
- **v0.6.2** — Settings UI 样式统一化：`UiStyle` 命名空间、所有 tab 行间距一致
- **v0.6.1** — ASR 模型启动预加载、onnxruntime/sherpa-onnx DLL 延迟加载、火山引擎 LLM 纠错修复
- **v0.6.0** — 源码从单文件重构为多模块架构
- **v0.5.0** — Cloud ASR UI 重构、async 模式修复、Shortcut 合并到 Recognition
- **v0.4.0** — 火山引擎（豆包）流式 ASR 接入（WebSocket）
- **v0.3.0** — 百度智能云 ASR 接入
- **v0.2.1** — 多供应商预设系统、LLM Prompt 独立 Tab、Extra Params
- **v0.2.0** — FireRedVAD 接入、云端 LLM 纠错模块
- **v0.1.4** — C++ 直接调用 sherpa-onnx，移除 Python 依赖

</details>

---

## Acknowledgments

- [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx) — ASR / VAD / 标点推理引擎
- [FireRedASR](https://github.com/FireRedTeam/FireRedASR) — 高精度中文 ASR 模型
- [FireRed VAD](https://github.com/FireRedTeam/FireRedAudio) — 高精度 VAD 模型
- [Silero VAD](https://github.com/snakers4/silero-vad) — 轻量 VAD 模型
- [onnxruntime](https://github.com/microsoft/onnxruntime) — ONNX 推理引擎
- [百度智能云](https://ai.baidu.com/tech/speech/asr) — 百度云端 ASR API
- [火山引擎语音技术](https://www.volcengine.com/docs/6561/1354869) — 火山引擎（豆包）流式 ASR API
- [阿里云百炼 Qwen ASR](https://help.aliyun.com/zh/model-studio/qwen-asr-realtime-interaction-process) — Qwen ASR Realtime API
- [小米 MiMo](https://platform.xiaomimimo.com/docs/zh-CN/usage-guide/Speech-Recognition) — MiMo ASR API
