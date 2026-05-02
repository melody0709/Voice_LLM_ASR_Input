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

- 前端：单文件 Win32 C++，主要在 `src/main.cpp`。
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
  - `src/resource.h`：APP_VERSION_MAJOR/MINOR/PATCH/BUILD
  - `src/resources.rc`：FileVersion 和 ProductVersion 字符串
  - `README.md`：当前版本
  - `CHANGELOG.md`：添加新版本记录
- sherpa-onnx `cxx-api.h` 包含非 ASCII 注释，编译时需要 `/utf-8` 标志。
- UI 修改后必须重新编译，并尽量实际打开 Settings 看是否裁切/重叠。
- HUD 修改后要特别检查高 DPI 缩放：DirectWrite/Direct2D 使用 DIP，Win32 `SetWindowPos` 使用物理像素，二者不能混用。

## 构建

```powershell
.\build.bat
```

如果链接失败并提示不能打开 `build\VoiceLLMASRInput.exe`，通常是旧程序还在运行：

```powershell
Get-Process VoiceLLMASRInput -ErrorAction SilentlyContinue | Stop-Process -Force
.\build.bat
```

## 当前实现脉络

### `src/main.cpp`

负责：

- 单实例互斥。
- 托盘菜单。
- Settings 窗口和 tab UI。
- 自绘 hotkey edit 控件。
- 打开 Settings 时暂停全局键盘 hook，关闭后恢复。
- `WH_KEYBOARD_LL` 长按快捷键录音。
- 默认 `CapsLock` 使用 300ms 长按判定：短按交还系统切换大小写，长按录音，结束后恢复按下前 Caps Lock 状态。
- `waveIn` 采集 16kHz mono PCM。
- Direct2D/DirectWrite 绘制底部 HUD，5 根音量条由 `waveIn` buffer 的 RMS 实时驱动。
- `AsrEngine` 直接调用 sherpa-onnx C++ API 完成 VAD、ASR、标点（无需 Python）。
- 收到文本后写剪贴板并发送 `Ctrl+V`。

### `AsrEngine`

C++ 类，封装 sherpa-onnx API，缓存模型实例：

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
  - `Recognition`
  - `Shortcut`
  - `LLM`（供应商配置、API Key、Model、Test Connection、Extra Params）
  - `LLM Prompt`（System Prompt 编辑、Basic Fix / Deep Fix 预设）
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

`AsrEngine` 类位于 `src/main.cpp` 内部，通过 `g_asrEngine` 全局实例访问。模型加载在后台线程完成，不阻塞 UI。

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


  
## 踩坑规则

> AI 在完成重大修改或解决复杂报错后，可追加规则。

- 改 `CapsLock` 热键逻辑时不要吞掉短按的系统大小写切换；短按需要补发 `CapsLock`，长按录音结束后必须保持原来的 Caps Lock 状态。
- 改 HUD 尺寸/文字布局时注意 DPI 单位：DirectWrite 文本测量和 Direct2D 绘制是 DIP，Win32 窗口大小是物理像素；高 DPI 下需要显式换算。
- FireRedVAD 的 fbank 特征提取期望 int16 范围音频值（-32768~32767），不是归一化 float（-1.0~1.0）。传入前必须乘以 32768，否则模型输出概率始终接近 0。
