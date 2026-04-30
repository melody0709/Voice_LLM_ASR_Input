# Changelog

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
  - 新增 `firered_vad.h` header-only 模块：使用 `kaldi_native_fbank` 提取 80 维 fbank 特征 + `onnxruntime` 加载 DFSMN 流式模型
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
