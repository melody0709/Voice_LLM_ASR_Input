# Changelog

> 🇬🇧 [English](../CHANGELOG.md)

## v0.6.1 (2026-05-06)

### 变更

- **ASR 模型启动预加载**：当 `asrBackend` 为 `local` 且模型目录存在时，启动时在后台线程预加载模型（ASR + VAD + 标点），消除首次按热键的延迟
- **onnxruntime/sherpa-onnx/kaldi DLL 延迟加载**：`onnxruntime.dll`、`sherpa-onnx-cxx-api.dll`、`kaldi-native-fbank-core.dll` 通过 `/DELAYLOAD` 延迟加载，只在本地 ASR 实际使用时才加载到内存。纯云端模式空闲内存 ~12 MB（从 ~20 MB 降低）
- **Settings Save 后预加载**：切换到本地 ASR 后端或更换模型后，Save 时后台重新预加载新模型
- **预加载完成 HUD 提示**：预加载完成后显示 "ASR ready: \<模型名\>"

### 修复

- **火山引擎 ASR 现在支持 LLM 纠错**：此前火山引擎结果即使 `postprocess` 设为 `Auto punctuate + LLM` 也会跳过 LLM 纠错。现在三个 ASR 后端（本地、百度、火山引擎）统一支持 LLM 纠错
- **`SaveConfig` 现在正确持久化 `llm_endpoint`、`llm_api_key`、`llm_model` 字段**（此前遗漏）
- 移除未使用的 `g_volcFinalText` 全局变量

## v0.6.0 (2026-05-06)

### 变更

- **源码从单文件重构为多模块架构**：`src/main.cpp`（3269 行）拆分为 5 个编译单元，职责清晰分离
  - `src/globals.h` — 共享常量、控件 ID、结构体定义、extern 全局变量声明
  - `src/engine.h` / `src/engine.cpp`（~750 行）— 后端：配置持久化、ASR 引擎、音频采集、工具函数
  - `src/hud.h` / `src/hud.cpp`（~380 行）— HUD 窗口、Direct2D 渲染、托盘图标、UI 资源管理
  - `src/hotkey.h` / `src/hotkey.cpp`（~330 行）— 热键逻辑、CapsLock 长按、键盘 Hook、HotkeyEdit 自绘控件
  - `src/settings.h` / `src/settings.cpp`（~1190 行）— Settings 窗口、控件、加载/保存、Provider 管理、输入对话框
  - `src/main.cpp`（~560 行）— 入口（WinMain）、主窗口过程、录音会话编排、LLM 纠错
- 全局变量在 `main.cpp` 中定义，其他模块通过 `globals.h` 的 `extern` 声明引用
- `build.bat` 更新：`cl` 命令现在编译 5 个源文件
- `CMakeLists.txt` 更新：`add_executable` 包含新 `.cpp` 文件，新增 `winhttp` 和 `crypt32` 链接依赖
- `.clangd` 更新：添加 UTF-8 字符集标志以兼容 sherpa-onnx 头文件

### 修复

- `SaveConfig` 现在正确持久化 `llm_endpoint`、`llm_api_key`、`llm_model` 字段（此前遗漏）
- 移除未使用的 `g_volcFinalText` 全局变量

## v0.5.0 (2026-05-05)

### 新增

- **Cloud ASR UI 重构**：合并 "Baidu ASR" 和 "豆包ASR" 两个 Tab 为一个 "Cloud ASR" Tab，顶部 Provider 下拉框切换
  - Provider ComboBox 切换 "百度智能云" 和 "火山引擎（豆包）"，下方控件动态显示/隐藏
  - 分组标题根据选中的 Provider 动态更新
- **ASR Backend 选择器移至 Recognition Tab**：作为全局设置放在 Recognition Tab 顶部
- **Shortcut 设置合并到 Recognition Tab**：删除独立 Shortcut Tab，快捷键配置移至 Recognition Tab 底部，加分隔线
- **豆包 ASR Mode 排序调整**：File Recognition (nostream) 排第一（推荐默认）
- **豆包 Model Version 清理**：删除 BigASR 1.0 选项，仅保留 Seed-ASR 2.0 (duration/concurrent)
- **Cloud ASR 报告**：新增 `.trae/documents/cloud_asr_report.md`，记录协议细节、调试指南和踩坑记录

### 变更

- Settings Tab 从 6 个减为 4 个：`Recognition` / `LLM` / `LLM Prompt` / `Cloud ASR`
- 豆包默认模式改为 `bigmodel_nostream`（录音文件识别）

### 修复

- **豆包 nostream 空结果**：`SendAudio(isLast=true)` 返回值被丢弃，现已保存到 `lastPartial` 作为回退
- **ReceiveResult 超时未生效**：`timeoutMs` 参数被忽略，始终使用 2000ms；现通过 `WinHttpSetOption` 动态设置
- **豆包 async 模式超时**：`bigmodel_async` 模式因 `ReceiveResult` 阻塞音频发送导致 8 秒超时；修复为发送/接收线程分离
  - 发送线程：只发音频包，不阻塞在接收上，保证 200ms 间隔
  - 接收线程（drainThread）：持续排空 WebSocket 接收缓冲区，有部分结果就更新 HUD
- **百度 App ID 移除**：确认百度 REST API 不使用 App ID，从 BaiduConfig、UI、config.json 中移除

### 移除

- 移除 `IDC_BAIDU_APP_ID` 控件和 `baiduAppId` 配置字段
- 移除 "Shortcut" Tab（合并到 Recognition）
- 移除 BigASR 1.0 模型版本选项

## v0.2.2 (2026-05-02)

### Added

- **GitHub 发布准备**：目录结构重组，源代码移入 `src/` 目录
  - 运行时 DLL 移入 `dll/` 目录并打包到 git（约 20MB）
  - 新增 `third_party/sherpa-onnx/` 头文件和导入库
  - 新增 `download_models.ps1` 模型下载脚本
- **模型下载优化**：集成 aria2c 多连接下载（4 连接并行）
  - 支持交互式菜单选择下载模型
  - 支持命令行参数 `-Models 1,3` 或 `-Models all`
  - 显示下载进度和速度
- **新用户引导**：首次启动检测模型目录，提示下载
  - 检查是否有任意 ASR 模型目录存在
  - Settings Recognition tab 新增 "Download" 按钮
- **内置 VAD 模型**：Silero VAD 和 FireRed VAD 打包到 git（约 2.5MB）

### Changed

- `.gitignore` 更新：允许 `models/silero_vad.int8.onnx` 和 `models/fireredvad_stream_vad_with_cache.onnx` 提交
- `build.bat` 更新：从 `dll/` 目录复制 DLL，从 `third_party/sherpa-onnx/` 获取头文件
- `CMakeLists.txt` 更新：源文件路径和 include 目录
- `.clangd` 更新：添加 `-Isrc` 和 `-Ithird_party/sherpa-onnx/include`

## v0.2.1 (2026-04-30)

### Added

- **多供应商预设系统**：Settings LLM tab 新增 Provider 下拉框，内置 DeepSeek / OpenRouter / SiliconFlow 三个预设
  - 选择预设自动填充 API Base URL、Model、Extra Params（关闭思考模式参数）
  - 每个供应商独立保存 API Key（DPAPI 加密），切换时自动恢复
  - 支持添加/删除自定义供应商（[+] / [−] 按钮）
- **LLM Prompt 独立 Tab**：System Prompt 从 LLM tab 拆出到独立的 "LLM Prompt" tab
  - System Prompt 多行编辑器高度增加到 340px
  - Basic Fix / Deep Fix 预设按钮保留在 Prompt tab 顶部
- **Extra Params 字段**：LLM tab 新增 Extra Params 单行编辑框
  - 用户可输入 JSON 片段，合并到 LLM API 请求体
  - 预设供应商自动注入关闭思考模式参数
  - 下方提示文字说明用途和示例格式
- **统一关闭思考模式**：`BuildRequestBody` 动态合并 `extraParams`，不再硬编码
  - DeepSeek: `"thinking":{"type":"disabled"}`
  - OpenRouter: `"reasoning":{"effort":"none"}`
  - SiliconFlow/Qwen3.6: `"chat_template_kwargs":{"enable_thinking":false}`

### Changed

- Settings tab 从 2 个扩展到 4 个：`Recognition` / `Shortcut` / `LLM` / `LLM Prompt`
- `config.json` 结构升级：新增 `llm_provider`、`llm_providers_json`，移除旧的 `llm_endpoint`/`llm_api_key`/`llm_model`
- 向后兼容：首次启动自动将旧配置迁移为 "Custom" 供应商

## v0.2.0 (2026-04-30)

### Added

- **FireRedVAD 接入**：Settings 新增 VAD 模型下拉框，可在 Silero VAD 和 FireRed VAD 之间切换
  - 新增 `src/firered_vad.h` header-only 模块：使用 `kaldi_native_fbank` 提取 80 维 fbank 特征 + `onnxruntime` 加载 DFSMN 流式模型
  - FireRedVAD 准确率显著优于 Silero VAD（F1 97.57 vs 95.95，误报率 2.69% vs 9.41%），模型仅 2.2MB
  - 新增 `third_party/kaldi_native_fbank/` 和 `third_party/onnxruntime/` 依赖
  - `build.bat` 和 `CMakeLists.txt` 同步更新链接配置
  - 运行时新增 DLL：`kaldi-native-fbank-core.dll`

### Fixed

- 修复 FireRedVAD 无法检测语音的问题：音频需要 int16 范围（-32768~32767），而非归一化 float（-1.0~1.0），fbank 特征提取前需乘以 32768

## v0.1.4 (2026-04-30)

### Changed

- **用 C++ 直接调用 sherpa-onnx 替换 Python ASR worker**
  - 移除 `asr_worker.py` 进程和 TCP JSON line 通信
  - 移除 Winsock 依赖（`ws2_32.lib`）
  - 新增 `AsrEngine` 类，直接调用 `sherpa-onnx-cxx-api` 的 `OfflineRecognizer`、`VoiceActivityDetector`、`OfflinePunctuation`
  - 识别流程改为：录音 PCM → C++ 直接调用模型 → 返回文本，无中间进程和网络开销
  - Reload 改为清除模型缓存，下次识别时自动重新加载
- `build.bat` 添加 sherpa-onnx include/lib 路径，自动复制 DLL 到 build 目录
- `CMakeLists.txt` 同步更新链接配置
- 运行时只需 3 个 DLL：`sherpa-onnx-cxx-api.dll`、`sherpa-onnx-c-api.dll`、`onnxruntime.dll`
- 不再需要 Python 环境和 `runtime/` 目录中的 Python 解释器

### Removed

- 移除 Python worker 相关代码：`StartWorkerProcess`、`StopWorkerProcess`、`SendWorkerJson`、`PingWorker` 等
- 移除 `WriteWavFile`（不再需要写临时 WAV 文件）
- 移除 `FindPythonExe`、`QuoteArg` 等辅助函数

## v0.1.3 (2026-04-29)

### Changed

- 版本号更新为 `v0.1.3`
- HUD 从 GDI 固定绘制升级为 Direct2D/DirectWrite 渲染
- HUD 尺寸改为 DPI-aware 的 DIP 计算，按实际文本宽度动态调整
- 5 根录音音量条改为由实时 PCM RMS 驱动，并调大可视尺寸
- 构建链接同步加入 `d2d1.lib` / `dwrite.lib`

### Fixed

- 修复高 DPI 下 HUD 文本被裁切的问题
- 修复 HUD 文本垂直居中不稳定的问题
- 修复 layered window 圆角边缘可能出现黑边的问题

## v0.1.2 (2026-04-29)

### Changed

- 托盘菜单版本号更新为 `v0.1.2`
- 默认 `CapsLock` 快捷键改为 300ms 长按触发语音输入
- 短按 `CapsLock` 交还系统处理，用于正常切换大小写

### Fixed

- 长按 `CapsLock` 语音输入结束后恢复按下前的大小写状态，避免误切换 Caps Lock
- 补发短按 `CapsLock` 时放行注入事件，避免被全局键盘 hook 再次拦截

## v0.1.1 (2026-04-29)

### Changed

- 线程上限从 4 提升至 8，auto 策略改为 `min(8, cpu_count)`
- Settings 线程选项从 1/2/3/4/auto 扩展为 1..8/auto，auto 项显示实际线程数

### Fixed

- Settings 窗口打开时固定在屏幕中央，不再出现在左上角

## v0.1.0 (2026-04-28)

- Initial release
