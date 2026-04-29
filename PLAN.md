# Win11 本地 CPU 语音输入法计划

## 目标
- 做一个 Windows 11 托盘常驻语音输入助手：按住快捷键录音，松开后把识别文本输入到当前光标位置。
- v1 不做真正 TSF/IME 内核，先用全局热键、WASAPI 录音、HUD 显示、剪贴板/SendInput 注入实现稳定可用。
- ASR 后端必须可切换，优先适配三种 CPU 本地模型，方便实际压测后决定默认方案。

## 三模型适配

### 1. FireRedASR2 CTC int8 ONNX，默认推荐
- 后端：`sherpa-onnx FireRedASR2 CTC int8`
- 模型：`sherpa-onnx-fire-red-asr2-ctc-zh_en-int8-2026-02-25`
- 体积：下载包约 496 MiB，解压后 `model.int8.onnx` 约 740 MB。
- 官方样例性能：单线程 RTF 约 0.173，约 5.8 倍实时速度。
- 优点：中文、英文、20+ 中文方言/口音能力强；CPU 速度好；单 ONNX 文件，部署简单。
- 缺点：CTC 本体不等于完整 FireRedASR2S pipeline，标点、规整、最终文本质量可能需要后处理；实时 partial 属于模拟流式。
- 计划定位：默认模型，用来验证主产品体验。

### 2. FireRedASR2 AED int8 ONNX，高质量模式
- 后端：`sherpa-onnx FireRedASR2 AED int8`
- 模型：`sherpa-onnx-fire-red-asr2-zh_en-int8-2026-02-26`
- 体积：下载包约 800 MiB，解压后 `encoder.int8.onnx` 约 779 MB，`decoder.int8.onnx` 约 398 MB，合计约 1.18 GB。
- 官方样例性能：单线程 RTF 约 0.333，约 3 倍实时速度。
- 优点：质量潜力更高，原 FireRedASR2-AED 支持词级时间戳和置信度；更适合长句、方言、复杂中英混说压测。
- 缺点：体积大、内存占用高、延迟比 CTC 高；对输入法短句场景未必划算。
- 计划定位：Settings 中的“高质量模式”，由你实测确认是否值得作为默认。

### 3. SenseVoiceSmall int8 ONNX，轻量/快速模式
- 后端：`sherpa-onnx SenseVoiceSmall int8`
- 模型：`sherpa-onnx-sense-voice-zh-en-ja-ko-yue-int8-2024-07-17` 或更新的 `2025-09-09` 粤语增强版本。
- 体积：解压后 `model.int8.onnx` 约 226-228 MB。
- 官方/项目说明：非自回归模型，低延迟；RK3588 A76 单线程 RTF 约 0.099，A55 单线程约 0.436。
- 优点：小、快、部署轻；支持中英日韩粤；2024 版支持 `use_itn` 输出标点和文本规整。
- 缺点：普通话和多方言质量预期低于 FireRedASR2；2025 粤语增强版不支持标点；复杂中文方言和技术词需实测。
- 计划定位：低资源 fallback，或偏重快速响应的默认候选。

## 架构
- `VoiceInput.exe`：C++ Win32 前端，负责托盘、Settings、全局快捷键、WASAPI 录音、Direct2D HUD、文本注入。
- `asr-worker`：本地 ASR 服务，监听 `127.0.0.1:18088`，只接受本机连接，负责加载模型、VAD、ASR、标点/ITN、后处理。
- v1 前后端通过本机 TCP JSON line 通信；后续需要真实流式 partial 时再升级为 WebSocket/二进制音频流。
- 模型文件不打进主程序安装包，Settings 里配置模型目录；后续再做模型下载器。
- 标点后处理采用 `sherpa-onnx-punct-ct-transformer-zh-en-vocab272727-2024-04-12-int8`，`model.int8.onnx` 约 72 MB，常驻 worker 内缓存，适配 FireRedASR2 AED/CTC 无标点输出。
- VAD 采用 `silero_vad.int8.onnx`，约 208 KB；启用时 worker 先检测并裁剪语音段，无人声则跳过 ASR。

## 托盘与 Settings
- 程序图标要求：
  - `resources.rc` 必须绑定 `app.ico`，保证 exe 文件图标、任务管理器/进程图标、Settings 窗口标题栏图标一致。
  - 托盘图标使用同一个资源图标，通过 `Shell_NotifyIconW` 注册。
  - v1 可先使用占位图标；发布前替换为专属麦克风/语音输入图标。
- 托盘右键菜单：
  - `Version: v0.1.2`，灰色不可点击。
  - `Settings...`，打开设置窗口。
  - `Reload ASR Worker`，保存设置后重启/重载后端。
  - 分割线。
  - `Quit`。
- Settings 必须支持更改模型：
  - `ASR Model` 下拉框：`FireRedASR2 CTC`, `FireRedASR2 AED`, `SenseVoiceSmall`。
  - `Model Directory`：模型目录选择框。
  - `Threads`：1..8/Auto。
  - `VAD`：启用/关闭，默认启用。
  - `Partial Result`：启用/关闭，默认启用；若模型模拟流式效果差，可关闭只显示音量和最终文本。
  - `Text Postprocess`：`None`, `ITN/Punctuation`, `Conservative LLM`；v1 的 `ITN/Punctuation` 使用本地 CT-Transformer 标点模型。
  - `Hotkey`：默认长按 `CapsLock`，支持录制组合键；短按 `CapsLock` 保留系统大小写切换。
  - `LLM`：启用开关、API Base URL、API Key、Model。
- 保存路径：`%APPDATA%\VoiceLLMASRInput\config.json`。

## 配置结构
```json
{
  "hotkey": {
    "mode": "hold",
    "key": "CapsLock",
    "modifiers": []
  },
  "asr": {
    "model_id": "firered_ctc",
    "model_dir": "",
    "threads": "auto",
    "enable_vad": true,
    "enable_partial": true,
    "chunk_ms": 100,
    "sample_rate": 16000
  },
  "postprocess": {
    "mode": "itn",
    "enable_llm": false,
    "api_base_url": "",
    "api_key": "",
    "model": ""
  },
  "ui": {
    "hud_monitor": "cursor",
    "theme": "mica"
  }
}
```

## WebSocket 协议
- `start`：
```json
{
  "type": "start",
  "session_id": "uuid",
  "model_id": "firered_ctc",
  "sample_rate": 16000,
  "format": "s16le",
  "chunk_ms": 100,
  "enable_vad": true,
  "enable_partial": true
}
```
- `audio`：二进制 PCM，16kHz、mono、s16le。
- `partial`：
```json
{
  "type": "partial",
  "session_id": "uuid",
  "text": "实时文本",
  "stable": false
}
```
- `final`：
```json
{
  "type": "final",
  "session_id": "uuid",
  "text": "最终文本",
  "model_id": "firered_ctc",
  "rtf": 0.18,
  "audio_ms": 5200,
  "decode_ms": 920
}
```
- `error`：
```json
{
  "type": "error",
  "session_id": "uuid",
  "code": "model_not_found",
  "message": "model.int8.onnx not found"
}
```
- `stop`：
```json
{
  "type": "stop",
  "session_id": "uuid"
}
```

## 前端实现计划
- 单实例启动，无任务栏图标，只保留托盘图标。
- 使用 `WH_KEYBOARD_LL` 实现长按热键录音；默认 `CapsLock` 以 300ms 区分短按/长按，短按切换大小写，长按才录音。
- WASAPI Shared Mode 采集系统默认麦克风，转换为 16kHz mono s16le 后发给 worker。
- HUD 使用 Direct2D 渲染：
  - 底部居中胶囊窗，56px 高，圆角 28px。
  - RMS 驱动 5 根波形条，权重 `[0.5, 0.8, 1.0, 0.75, 0.55]`。
  - 显示 partial/final/refining/error 状态。
- 文本注入：
  - 保存剪贴板，写入最终文本，模拟 `Ctrl+V`，恢复剪贴板。
  - 对可取得 IMM context 的窗口，临时关闭 CJK 输入法转换状态。
  - 如果剪贴板注入失败，降级为 `SendInput` Unicode 文本。

## ASR Worker 实现计划
- v1 可先用 Python worker 快速验证三模型；接口稳定后再考虑 C++/Rust worker。
- worker 启动时不必一次加载三种模型，只加载 Settings 当前选择的模型。
- 切换模型时：
  - 前端保存配置。
  - 发送 reload，或直接重启 worker。
  - worker 返回当前模型加载状态、模型文件校验和、加载耗时、预估内存。
- 每个模型做一个 adapter：
  - `firered_ctc`: 单模型文件 `model.int8.onnx` + `tokens.txt`。
  - `firered_aed`: `encoder.int8.onnx` + `decoder.int8.onnx` + `tokens.txt`。
  - `sensevoice`: `model.int8.onnx` + `tokens.txt`，可传 `language` 和 `use_itn`。
- VAD：
  - v1 使用 sherpa-onnx 的 Silero VAD int8。
  - `CapsLock` 短按少于 300ms 时不送 ASR，补发系统 `CapsLock` 用于大小写切换。
  - 输入法场景先采用保守 VAD：只裁头尾静音，不删除中间停顿；长音频切段留到流式方案中处理。

## 性能测试计划
- 每次测试记录：
  - 模型 ID、线程数、CPU 型号、内存占用、加载耗时。
  - 音频时长、解码耗时、RTF、松开按键到文本上屏耗时。
  - 是否启用 VAD、partial、ITN/标点、LLM。
- 测试音频：
  - 普通话短句：2-5 秒。
  - 普通话长句：15-60 秒。
  - 中英混说：Python、JSON、API、WebSocket、CMake、Win32。
  - 方言/口音：四川话、粤语、河南话、天津话等。
  - 噪声：键盘声、风扇声、远场麦克风。
- 目标线：
  - FireRedASR2 CTC：松开后 1 秒内上屏为理想，1.5 秒内可接受。
  - FireRedASR2 AED：质量模式允许 2 秒左右，但不能卡 UI。
  - SenseVoiceSmall：低配模式目标小于 1 秒。
  - Worker 常驻内存：SenseVoice 小于 1GB；FireRed CTC 尽量小于 2GB；FireRed AED 尽量小于 3GB。

## 需要特别注意
- 不要把 FireRedASR2 CTC 当作完整 FireRedASR2S：ONNX CTC 只转换了 CTC 分支，不包含完整 VAD/LID/Punc pipeline。
- SenseVoice 的 `use_itn` 很有用，但不同版本能力不同：2024 版支持标点/规整，2025 粤语增强版文档注明不支持标点。
- partial 体验可能是最大变量：这些模型多为 offline 或 simulated streaming，实时文字可能会抖、回退、延迟。HUD 必须能优雅处理“只有最终结果”的模式。
- 模型热切换必须避免前端卡死：加载模型在 worker 进程完成，前端只显示 loading/error。
- 管理员权限窗口可能阻止普通权限进程注入输入；v1 记录失败并提示，不强行提权。
- 剪贴板恢复要延迟一点执行，避免目标应用还没 paste 完就被恢复。
- 多显示器和 DPI 缩放会影响 HUD 位置，必须用当前光标/前台窗口所在显示器计算。
- CapsLock 热键要处理键盘状态恢复，避免系统 Caps 状态被误切换；短按必须保留大小写切换，长按录音结束后保持原状态。
- LLM 纠错必须默认关闭；开启时必须超时短、可取消、失败直接使用 ASR 原文。
- 模型许可证和分发方式要单独确认，尤其如果未来把模型打包进安装器。

## 当前建议默认值
- 默认模型：`FireRedASR2 CTC int8`
- 默认线程：`auto`，worker 内部先映射为 `min(8, cpu_count)`，后续由测试数据调整。
- 默认 VAD：启用。
- 默认 partial：启用，但 Settings 允许关闭。
- 默认后处理：`ITN/Punctuation`，LLM 关闭。
- 默认热键：长按 `CapsLock`，300ms 后触发语音输入；短按切换大小写。

## 待确认问题
- 你更看重“松开后最快上屏”，还是“宁可慢一点也要更准”？这个会决定 CTC 和 SenseVoice 谁更适合作为最终默认。
- 你常用的输入语言比例是多少：普通话、方言、粤语、英文、日文/韩文？
- 是否需要“说完自动停止”，还是坚持“按住说话、松开结束”？自动停止会显著增加 VAD 和状态机复杂度。
- 是否接受首次启动下载模型，还是必须手动放置模型目录？
- 是否要保留 Qwen3-ASR 作为第四个实验后端？当前计划先不做，因为纯 CPU 和流式体验不如这三种路线确定。

## Sources
- FireRedASR2S: https://github.com/FireRedTeam/FireRedASR2S
- sherpa-onnx FireRedASR2: https://k2-fsa.github.io/sherpa/onnx/FireRedAsr/pretrained.html
- sherpa-onnx SenseVoice: https://k2-fsa.github.io/sherpa/onnx/sense-voice/pretrained.html
- SenseVoice: https://github.com/FunAudioLLM/SenseVoice
