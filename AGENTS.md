# AGENTS.md

本文件给后续继续开发本项目的 Codex/开发者使用。请优先阅读 `README.md`、`ARCHITECTURE.md` 再改代码。

## 项目目标

做一个 Windows 11 本地 CPU 语音输入工具：

- 托盘常驻。
- 按住快捷键录音，松开后识别。
- 本地 ASR，不依赖云端。
- 支持多模型切换和后处理实验。
- 先做稳定可用的工具，不急着做完整 TSF/IME。

## 当前技术栈

- 前端：Win32 C++ 多模块架构，入口在 `src/main.cpp`，后端在 `src/engine.cpp`，UI 在 `src/settings.cpp` / `src/hud.cpp`，热键在 `src/hotkey.cpp`。
- ASR：C++ 直接调用 `sherpa-onnx-cxx-api`（`OfflineRecognizer`、`VoiceActivityDetector`、`OfflinePunctuation`）。
- VAD：Silero VAD（sherpa-onnx 内置）或 FireRed VAD（`src/firered_vad.h`，用 `kaldi_native_fbank` + `onnxruntime`）。
- 构建：`build.bat` 调用 Visual Studio 2022 `cl` 和 `rc`。
- 运行时 DLL：`sherpa-onnx-cxx-api.dll`、`sherpa-onnx-c-api.dll`、`onnxruntime.dll`、`kaldi-native-fbank-core.dll`。
- 模型目录：`models/`，不提交到 git。

## 开发约定

- 手工编辑文件时优先使用 `apply_patch`。
- 不要把 `models/`、`build/`、`__pycache__/` 加入 git。
- 不要把模型文件打进 exe 或资源文件。
- C++ 新增依赖时同步更新：
  - `#pragma comment(lib, "...")`
  - `build.bat`
  - `CMakeLists.txt`
- **更新版本号时必须同步更新以下文件**：
  - `src/resource.h`：APP_VERSION_MAJOR/MINOR/PATCH/BUILD（**唯一需要改的文件**，字符串宏会自动派生）
  - `README.md`：当前版本
  - `CHANGELOG.md`：添加新版本记录
  - `src/main.cpp` 和 `src/resources.rc` 使用 `resource.h` 中的版本字符串宏，无需手动更新
- sherpa-onnx `cxx-api.h` 包含非 ASCII 注释，编译时需要 `/utf-8` 标志。
- UI 修改后必须重新编译，并尽量实际打开 Settings 看是否裁切/重叠。
- HUD 修改后要特别检查高 DPI 缩放：DirectWrite/Direct2D 使用 DIP，Win32 `SetWindowPos` 使用物理像素，二者不能混用。

## 构建

```powershell
.\build.bat
```

如果链接失败并提示不能打开 `build\VoxType.exe`，通常是旧程序还在运行：

```powershell
Get-Process VoxType -ErrorAction SilentlyContinue | Stop-Process -Force
.\build.bat
```

## 当前实现脉络

### 源码模块（v0.6.0 起）

| 文件 | 职责 |
|------|------|
| `src/globals.h` | 共享常量、控件 ID、结构体、extern 全局变量声明 |
| `src/engine.h` / `src/engine.cpp` | 后端：字符串/路径工具、JSON 配置持久化、音频采集、`AsrEngine` 类、`PreloadAsrEngine()` |
| `src/hud.h` / `src/hud.cpp` | HUD 窗口、Direct2D 渲染、托盘图标、UI 资源创建/销毁 |
| `src/hotkey.h` / `src/hotkey.cpp` | 热键配置、CapsLock 长按、`WH_KEYBOARD_LL` Hook、HotkeyEdit 自绘控件 |
| `src/settings.h` / `src/settings.cpp` | Settings 窗口、tab UI、控件创建、加载/保存、Provider 管理、输入对话框 |
| `src/main.cpp` | 入口（`wWinMain`）、主窗口过程、录音会话编排、LLM 纠错 |

全局变量在 `main.cpp` 中定义，其他模块通过 `globals.h` 的 `extern` 声明引用。

### `src/main.cpp`

负责：

- 全局变量定义（所有 `g_` 变量的实际存储）。
- 单实例互斥。
- `WriteLlmLog` / `RefineWithLlmAsync`：LLM 纠错日志和异步调用。
- `RecognizeAsync`：ASR 识别异步调度（本地 / 百度 / 火山引擎）。
- `StartRecordingSession` / `StopRecordingSession`：录音会话编排。
- 托盘菜单。
- `MainWndProc`：主窗口消息处理。
- `RegisterWindowClasses`：注册所有窗口类。
- `wWinMain`：程序入口。

### `AsrEngine`

C++ 类，位于 `src/engine.h` / `src/engine.cpp`，封装 sherpa-onnx API，缓存模型实例：

- `OfflineRecognizer`：ASR 识别（FireRedASR2 CTC/AED、SenseVoice）。
- `VoiceActivityDetector`：Silero VAD。
- `firered_vad::FireRedVad`：FireRed VAD（`src/firered_vad.h`）。
- `OfflinePunctuation`：CT-Transformer 标点。
- 同一模型不重复加载；切换模型或 Reload 时清除缓存。
- 启用 VAD 时保守裁剪头尾静音，中间停顿保留。无人声则直接返回空文本。
- `postprocess` 为 `itn`/`punct`/`llm` 时启用标点模型。

## 已知 UI 注意点

- Settings 不要再放页面大标题，标题栏已有窗口名。
- 当前 Settings 使用 4 个 tab：
  - `Recognition`（含快捷键配置）
  - `LLM`（供应商配置、API Key、Model、Test Connection、Extra Params）
  - `LLM Prompt`（System Prompt 编辑、Basic Fix / Deep Fix 预设）
  - `Cloud ASR`（百度智能云 / 火山引擎豆包）
- 底部 `Status / Save / Close` 通过 `LayoutSettingsWindow()` 按客户区底部动态定位。
- 高 DPI 下 Win32 控件容易裁字；控件高度宁可留大一点。
- tab 字体已故意设小，避免占用空间。
- HUD 尺寸先按 DirectWrite 测量 DIP，再按当前窗口 DPI 转物理像素；不要直接把 DIP 当作 `SetWindowPos` 的宽高。

## LLM 纠错

v0.2.0 起新增 LLM 文本纠错模块 `src/llm_refine.h`（header-only，`llm::` 命名空间）。

v0.2.1 起新增多供应商预设系统：

- `kProviderPresets[]` 定义内置供应商（DeepSeek、OpenRouter、SiliconFlow），每个预设包含 URL、默认模型、Extra Params
- `Provider` 下拉框切换时自动填充所有字段，API Key 从 `llmProvidersJson` 恢复
- `[+]` 按钮弹输入窗口创建自定义供应商，`[−]` 按钮删除自定义供应商（预设不可删）
- `Extra Params` 是单行编辑框，用户填入 JSON 片段，`BuildRequestBody` 动态合并到请求体
- 预设供应商自动注入关闭思考模式参数（DeepSeek 用 `thinking:disabled`，OpenRouter 用 `reasoning:none`，SiliconFlow 用 `chat_template_kwargs:enable_thinking=false`）
- `config.json` 用 `llm_provider` + `llm_providers_json` 存储所有供应商配置
- 向后兼容旧的 `llm_endpoint`/`llm_api_key`/`llm_model` 字段，自动迁移为 "Custom" 供应商

## ASR 引擎

v0.1.4 起移除了 Python worker，改为 C++ 直接调用 sherpa-onnx。

`AsrEngine` 类位于 `src/engine.h` / `src/engine.cpp`，通过 `g_asrEngine` 全局实例访问。模型加载在后台线程完成，不阻塞 UI。

## 后续优先级建议

1. 做稳定的 Settings UI 和快捷键体验。
2. 加性能日志：load/decode/punct/total latency。
3. 记录 VAD/ASR/标点耗时到可查看日志，方便性能压测。
4. 研究 partial：先模拟流式，再评估是否换 streaming 模型。
5. 做用户词库/术语替换。
6. 再接保守 LLM 纠错，必须默认关闭并带超时/回退。

## 不要轻易做的事

- 不要直接把 LLM 接成默认纠错，容易乱改用户意思。
- 不要让 UI 线程加载模型或等待 ASR。
- 不要在 Settings 打开时继续拦截录音快捷键。
- 不要依赖固定窗口高度放底部按钮。
- 不要为了美观牺牲控件可读性，高 DPI 下要优先留空间。


  
## 延迟加载 & 预加载

- `onnxruntime.dll`、`sherpa-onnx-cxx-api.dll`、`kaldi-native-fbank-core.dll` 通过 MSVC `/DELAYLOAD` 延迟加载，只在本地 ASR 实际使用时才加载到内存。纯云端模式空闲内存 ~12 MB。
- `engine.cpp` 中 `TryLoadAsrDlls()` 安全检查 DLL 是否可用，缺失时优雅返回 false。
- 本地模式启动时，`PreloadAsrEngine()` 在后台线程预加载当前模型（ASR + VAD + 标点），消除首次按热键延迟。
- Settings Save 后如果 `asrBackend == "local"`，会 Reload + 后台预加载新模型。
- 预加载完成后发送 `kPreloadDoneMessage`，HUD 显示 "ASR ready: xxx"。

## 踩坑规则

> AI 在完成重大修改或解决复杂报错后，可追加规则。

- 改 `CapsLock` 热键逻辑时不要吞掉短按的系统大小写切换；短按需要补发 `CapsLock`，长按录音结束后必须保持原来的 Caps Lock 状态。
- 改 HUD 尺寸/文字布局时注意 DPI 单位：DirectWrite 文本测量和 Direct2D 绘制是 DIP，Win32 窗口大小是物理像素；高 DPI 下需要显式换算。
- FireRedVAD 的 fbank 特征提取期望 int16 范围音频值（-32768~32767），不是归一化 float（-1.0~1.0）。传入前必须乘以 32768，否则模型输出概率始终接近 0。
