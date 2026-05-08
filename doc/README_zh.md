<p align="center">
  <img src="../src/app.ico" width="64" alt="VoxType icon" />
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
  当前版本：<code>v0.7.0</code> &nbsp;|&nbsp; 🇬🇧 <a href="../README.md">English</a>
</p>

https://github.com/user-attachments/assets/36243dc2-cfc8-41fb-b0cf-6e558f02cd5e

---

## Highlights

- **按住即说** — 默认 CapsLock 长按录音，松开自动粘贴到当前窗口；短按照常切换大小写
- **100% 本地** — C++ 直接调用 sherpa-onnx，无需 Python、无需云端 API，所有数据不出本机
- **实时 HUD** — 录音时底部显示悬浮胶囊窗，5 根音量条随声音跳动
- **双 VAD 可选** — Silero VAD（轻量）/ FireRed VAD（高精度 F1 97.57），智能跳过静音
- **LLM 纠错（可选）** — 支持 DeepSeek / OpenRouter / SiliconFlow 等多供应商，一键配置
- **Cloud ASR（可选）** — 支持火山引擎（豆包）和百度智能云流式 ASR 作为替代后端

## Quick Start

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

### 3. 运行

```powershell
.\build\VoxType.exe
```

右键托盘图标打开 Settings，按住快捷键开始录音，松开后识别并粘贴。

---

<details open>
<summary><strong>⚙️ Settings 说明</strong></summary>

**Recognition tab**
- `ASR Backend` — 选择 `Local (sherpa-onnx)` / `Volcano Engine` / `Baidu Cloud`
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
- `Provider` — 选择 `Volcano Engine (Doubao)` 或 `Baidu Cloud`，下方控件动态切换
- **Baidu Cloud**：`API Key` / `Secret Key`（DPAPI 加密）+ `Language Model`（普通话/英语/粤语/四川话）+ `Test Connection`
- **Volcano Engine (Doubao)**：`API Key`（DPAPI 加密）+ `ASR Mode` + `Model Version` + `Language` + `Test Connection`
  - ASR Mode：`File Recognition (nostream)`（推荐，准确率最高）/ `Streaming (bigmodel)`（实时部分结果）/ `Streaming Optimized (async)`（最佳延迟）
  - Model Version：`Seed-ASR 2.0 (duration)`（按时长计费）/ `Seed-ASR 2.0 (concurrent)`（按并发计费）
  - Hotwords ID/Name、Correct ID/Name — 引用自学习平台热词词表和替换词词表
  - Use history as context — 将最近识别结果作为对话上下文发送，提升准确率
- 云端 ASR 自带标点；使用云端后端时 VAD 和本地标点模型不生效

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
  main.cpp          — 入口、录音会话编排、LLM 纠错调度
  engine.h / .cpp   — 配置持久化、ASR 引擎、音频采集、工具函数
  hud.h / .cpp      — HUD 窗口、Direct2D 渲染、托盘图标、UI 资源
  hotkey.h / .cpp   — 热键逻辑、CapsLock、键盘 Hook、HotkeyEdit 控件
  settings.h / .cpp — Settings 窗口、控件、加载/保存、Provider 管理
  baidu_asr.h       — 百度智能云 ASR 模块（header-only）
  volcengine_asr.h  — 火山引擎（豆包）ASR 模块（header-only, WebSocket）
  firered_vad.h     — FireRed VAD 模块（header-only）
  llm_refine.h      — LLM 纠错模块（header-only）
  globals.h         — 共享常量、控件 ID、extern 全局变量声明
  resources.rc / resource.h / app.ico / app.manifest
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

- 录音后才识别，尚未实现真正流式 partial
- 文本注入以剪贴板 + Ctrl+V 为主，管理员权限窗口可能拦截
- 模型文件较大（约 3GB），首次加载需要几秒
- LLM 纠错尚未完全实现（Punctuation 的 +L 选项当前按本地标点处理）

</details>

<details>
<summary><strong>📋 更新日志</strong></summary>

详见 [CHANGELOG.md](CHANGELOG.md)

**最近更新：**
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
