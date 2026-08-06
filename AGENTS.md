# AGENTS.md
详细架构见 `ARCHITECTURE.md`，版本历史见 `CHANGELOG.md`。

## 项目概要

Windows 11  语音输入法工具：托盘常驻，按住快捷键录音松开识别，本地 ASR（sherpa-onnx），可选云端 ASR（百度/火山引擎/Qwen）和 LLM 纠错。

# 参考项目
- C:\Program Files\QianwenIME 只读 需要逆向。
- /reverse-skill 涉及到取证，请采用相关的技能。
- 获取到相关证据，必须更新 /reverse 目录下的相关文档以及证据，以便后面研究。
## 构建

```powershell
.\build.bat
```

`CMakeLists.txt` 与 `CMakePresets.json` 是唯一编译权威；`build.bat` 负责准备 MSVC、调用 CMake/Ninja 并安装运行载荷。

唯一可直接运行的开发程序是 `build\run\x64-release\VoxType.exe`，不能运行 `build\cmake\x64-release\VoxType.exe` 或手工向运行目录复制文件。

## `build/` 生成目录边界

- `build/` 完全是可删除、gitignored 的生成输出；禁止存放源码、手工脚本、截图、输入数据、用户数据或人工备份。
- 顶层白名单仅为 `cmake/`、`run/`、`artifacts/`、`logs/`、`packages/` 和 `README.txt`。新增顶层项必须同步更新 `build.bat`、布局校验脚本和本文档。
- `build/cmake/x64-release/` 只放 CMake/Ninja 编译中间产物；`build/run/x64-release/` 是唯一规范运行载荷；发布包只进入 `build/packages/`。
- 测试、诊断和显式日志分别放入 `build/artifacts/`、`build/artifacts/diagnostics/` 和 `build/logs/`。
- `build.bat --clean` 清理 `cmake`、`run`、`artifacts`、`logs` 和布局说明，但保留已验证的 `packages/`。未知项必须由布局校验报错，不能自动删除或打包。

## 开发约定

- 编辑文件优先用精确替换（SearchReplace / apply_patch），避免全文覆写。
- C++ 新增依赖同步更新：`#pragma comment(lib)` + `CMakeLists.txt`；若新增运行时 DLL 或资源，还要更新 `cmake/VoxTypeRuntime.cmake` 的安装清单。
- **版本号只改 `src/app/resource.h` 的 APP_VERSION_MAJOR/MINOR/PATCH/BUILD**，再同步 `README.md` 版本和 `CHANGELOG.md` 记录。`src/app/main.cpp` / `src/app/resources.rc` 用宏自动派生。
- sherpa-onnx `cxx-api.h` 含非 ASCII 注释，编译需 `/utf-8`。
- UI 修改后必须编译验证，Settings 检查裁切/重叠，HUD 检查高 DPI。
- DPI 单位：DirectWrite/Direct2D 用 DIP，Win32 `SetWindowPos` 用物理像素，不能混用。
- Settings 布局常量在 `src/app/globals.h` 的 `UiStyle` 命名空间，不要硬编码魔法数字。
- 新增配置项同步三处：`src/app/globals.h` Config 字段 + `src/audio/engine.cpp` LoadConfig/SaveConfig + `src/ui/settings.cpp` UI 控件。
- DLL 延迟加载：`onnxruntime.dll`、`sherpa-onnx-cxx-api.dll`、`kaldi-native-fbank-core.dll` 通过 `/DELAYLOAD` 延迟加载，纯云端模式空闲 ~12 MB。`TryLoadAsrDlls()` 用 SEH 安全检测。
- 本地模式启动时 `PreloadAsrEngine()` 后台线程预加载模型，Save 后 Reload + 预加载。

## 新增云端 ASR 接入规则

- 先判断新后端是 batch 还是 streaming，不要直接在 `src/app/main.cpp` 写大段 provider 逻辑。
- Batch 后端优先走 `IAsrSession` / `BatchAsrSessionBase`；streaming 后端优先走 `IStreamingAsrSession` / `StreamingAsrSessionBase`。
- 复用已有公共层：`asr_dispatcher` / `asr_result` / `cloud_http_common` / `cloud_asr_common` / `PendingPcmBuffer` / VAD trimmer。只有 provider 协议差异留在各自 client/session 内。
- 音频回调只做轻量采集、可选 VAD trim、`EnqueuePcmChunk()`；不要在 WASAPI/waveIn 回调里做网络请求或 provider 协议逻辑。
- Streaming 停止录音用 `StopInput()` 通知 session，不要让 `StopRecordingSession()` 同步等待云端 final。
- 新 provider 如果有跨录音 session 的连接预热/复用句柄，先保留清晰生命周期，不要强行收进单次录音 session。
- Settings 新增配置项仍必须同步三处：`src/app/globals.h` Config 字段 + `src/audio/engine.cpp` LoadConfig/SaveConfig + `src/ui/settings.cpp` UI 控件。

## 不要轻易做的事

- 不要把 LLM 接成默认纠错，容易乱改用户意思。
- 不要让 UI 线程加载模型或等待 ASR。
- 不要在 Settings 打开时继续拦截录音快捷键。
- 不要依赖固定窗口高度放底部按钮。
- 不要为了美观牺牲控件可读性，高 DPI 优先留空间。
- 不要混淆 `src/asr/volcengine_asr.h` 的 `ExtractJsonStr`（无转义）和 `src/audio/engine.cpp` 的 `ExtractJsonString`（支持转义）。
- 不要在火山引擎 `bigmodel_nostream` 模式下对中间 chunk 调用 `ReceiveResult`——服务器返回空文本，浪费时间。
- 不要在 Extra Params 和代码生成的 `corpus` 中同时写 `corpus`——代码已跳过 Extra Params 中的 `corpus` key，手动编辑 config 需注意。
- 不要新增云端 ASR 时绕过 `IAsrSession` / `IStreamingAsrSession`，否则 `main.cpp` 会重新膨胀。
- 不要把 Qwen / 火山 / 未来 streaming provider 的 send loop、drain thread、retry 强行做成一个复杂模板；公共层负责生命周期和通用策略，协议差异留在各自 session/client 内。

## 踩坑规则

> AI 在完成重大修改或解决复杂报错后，可追加规则。

- CapsLock 热键：短按必须补发 `CapsLock` 保持系统切换，长按录音结束后必须恢复原 Caps Lock 状态。
- HUD 尺寸：DirectWrite 测量 DIP，`SetWindowPos` 用物理像素，高 DPI 需显式换算。
- FireRedVAD：fbank 期望 int16 范围（-32768~32767），不是归一化 float，传入前必须乘 32768。
- 公共 VAD trim 只裁剪头尾静音，不能裁掉中间停顿；`VadTrimCore::ProcessChunk()` 是追加输出语义，调用方要自己清空/使用局部 `outputs`。
- Streaming VAD 的 no-speech 判断用 `StreamingVadTrimmer::DetectedSpeech()`，不要重新引入 provider 专属 `g_xxxVadState`。
- `ExtractJsonStr` 处理 `\\"` 时不能只看前一个字符，必须统计连续反斜杠数量——偶数个后的 `"` 是结束符。
- 火山引擎 WebSocket `connected` 必须用 `std::atomic<bool>`，不能用 `volatile bool`。
- 火山引擎 `context` 字段值必须是 JSON 字符串（内部引号转义），不能是原始 JSON 对象。
- 火山引擎 `volcengine_asr.h` 协议层可后续机械拆薄，但不要在无测试覆盖时重写 frame 编解码、三模式 send/drain、`PrewarmConnection()` / `CloseSession()` 行为。
- 火山引擎 `g_volcSession` 仍保留全局是为了 `hSession/hConnect` 跨录音 session 复用；不要轻易改成单次录音 session 成员，否则 Settings 保存后的预热连接会失效。
- 百度 ASR transient 失败应重发同一段 PCM；token/auth 错误应清 token 后刷新重试，避免用户必须重新说第二遍。
- WASAPI 生命周期必须是 `Init → Start → Stop → Release`，`Stop()` 只停线程不清资源，没有 `Release()` 第二次录音会卡死。`Init()` 成功但 `Start()` 失败时也必须 `Release()`。
- WASAPI 重采样相位更新必须用 `m_resamplePhase -= written / m_resampleRatio`（实际消耗的源样本数），不能用 `m_resamplePhase -= numFrames`（输入帧数），否则非整数采样率比会越界。
- **微信粘贴**：微信（`Weixin.exe`）用自定义 Qt 控件，`GetFocus()` 返回 NULL 且 IME 拦截 Ctrl+V。必须用 `WM_CHAR` 逐字符发送，不能用剪贴板+Ctrl+V。其他应用用剪贴板+Ctrl+V+IMM32 切换。
