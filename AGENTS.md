# AGENTS.md

开发者指南。详细架构见 `ARCHITECTURE.md`，版本历史见 `CHANGELOG.md`。

## 项目概要

Windows 11 本地 CPU 语音输入工具：托盘常驻，按住快捷键录音松开识别，本地 ASR（sherpa-onnx），可选云端 ASR（百度/火山引擎）和 LLM 纠错。

## 构建

```powershell
.\build.bat
```

链接失败提示不能打开 `build\VoxType.exe` 时：`Get-Process VoxType -ErrorAction SilentlyContinue | Stop-Process -Force; .\build.bat`

`CMakeLists.txt` 仅用于 clangd/IDE 索引，实际构建用 `build.bat`。

## 开发约定

- 编辑文件优先用精确替换（SearchReplace / apply_patch），避免全文覆写。
- 不提交 `models/`（VAD 模型除外）、`build/`、`__pycache__/`。不把模型打进 exe。
- C++ 新增依赖同步更新：`#pragma comment(lib)` + `build.bat` + `CMakeLists.txt`。
- **版本号只改 `src/resource.h` 的 APP_VERSION_MAJOR/MINOR/PATCH/BUILD**，再同步 `README.md` 版本和 `CHANGELOG.md` 记录。`main.cpp`/`resources.rc` 用宏自动派生。
- sherpa-onnx `cxx-api.h` 含非 ASCII 注释，编译需 `/utf-8`。
- UI 修改后必须编译验证，Settings 检查裁切/重叠，HUD 检查高 DPI。
- DPI 单位：DirectWrite/Direct2D 用 DIP，Win32 `SetWindowPos` 用物理像素，不能混用。
- Settings 布局常量在 `globals.h` 的 `UiStyle` 命名空间，不要硬编码魔法数字。
- 新增配置项同步三处：`globals.h` Config 字段 + `engine.cpp` LoadConfig/SaveConfig + `settings.cpp` UI 控件。
- DLL 延迟加载：`onnxruntime.dll`、`sherpa-onnx-cxx-api.dll`、`kaldi-native-fbank-core.dll` 通过 `/DELAYLOAD` 延迟加载，纯云端模式空闲 ~12 MB。`TryLoadAsrDlls()` 用 SEH 安全检测。
- 本地模式启动时 `PreloadAsrEngine()` 后台线程预加载模型，Save 后 Reload + 预加载。

## 不要轻易做的事

- 不要把 LLM 接成默认纠错，容易乱改用户意思。
- 不要让 UI 线程加载模型或等待 ASR。
- 不要在 Settings 打开时继续拦截录音快捷键。
- 不要依赖固定窗口高度放底部按钮。
- 不要为了美观牺牲控件可读性，高 DPI 优先留空间。
- 不要混淆 `volcengine_asr.h` 的 `ExtractJsonStr`（无转义）和 `engine.cpp` 的 `ExtractJsonString`（支持转义）。
- 不要在火山引擎 `bigmodel_nostream` 模式下对中间 chunk 调用 `ReceiveResult`——服务器返回空文本，浪费时间。
- 不要在 Extra Params 和代码生成的 `corpus` 中同时写 `corpus`——代码已跳过 Extra Params 中的 `corpus` key，手动编辑 config 需注意。

## 踩坑规则

> AI 在完成重大修改或解决复杂报错后，可追加规则。

- CapsLock 热键：短按必须补发 `CapsLock` 保持系统切换，长按录音结束后必须恢复原 Caps Lock 状态。
- HUD 尺寸：DirectWrite 测量 DIP，`SetWindowPos` 用物理像素，高 DPI 需显式换算。
- FireRedVAD：fbank 期望 int16 范围（-32768~32767），不是归一化 float，传入前必须乘 32768。
- `ExtractJsonStr` 处理 `\\"` 时不能只看前一个字符，必须统计连续反斜杠数量——偶数个后的 `"` 是结束符。
- 火山引擎 WebSocket `connected` 必须用 `std::atomic<bool>`，不能用 `volatile bool`。
- 火山引擎 `context` 字段值必须是 JSON 字符串（内部引号转义），不能是原始 JSON 对象。
- WASAPI 生命周期必须是 `Init → Start → Stop → Release`，`Stop()` 只停线程不清资源，没有 `Release()` 第二次录音会卡死。`Init()` 成功但 `Start()` 失败时也必须 `Release()`。
- WASAPI 重采样相位更新必须用 `m_resamplePhase -= written / m_resampleRatio`（实际消耗的源样本数），不能用 `m_resamplePhase -= numFrames`（输入帧数），否则非整数采样率比会越界。

## 后续优先级

1. 本地 ASR 性能日志（VAD/decode/punct 分段耗时）
2. 流式 partial（模拟或换 OnlineRecognizer）
3. 用户词库/术语替换
4. 剪贴板恢复
5. Admin 窗口注入失败检测
6. Unicode SendInput 回退
