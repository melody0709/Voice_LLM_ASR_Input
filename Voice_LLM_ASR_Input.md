请作为顶级的 Windows C++ 系统级开发专家，帮我设计并实现一个极低消耗、高性能的 Windows 11 桌面语音输入法的前端客户端。

**【核心架构与技术栈要求】**
本项目采用**前后端分离**的极客架构。当前实现是：**纯 C++ Win32 瘦客户端 (UI与系统交互) + 本地 Python TCP JSON line 守护进程 (AI 推理引擎)**。WebSocket/真实流式是后续演进方向。
本需求仅要求实现 **C++ 客户端** 部分。

- **语言**：Modern C++ (C++17 或 C++20)，要求严格遵循 RAII 规范。
- **UI & 渲染**：纯 Win32 API 创建窗口，绝不使用 Qt/WPF/MFC。使用 Direct2D 进行高质量抗锯齿渲染。
- **视觉特效**：使用 DWM API 调用 Windows 11 原生亚克力 (Acrylic) 或云母 (Mica) 模糊材质及原生圆角。
- **音频系统**：当前使用 WinMM `waveIn` 采集 16kHz mono PCM；WASAPI Shared Mode 是后续低延迟演进方向。
- **网络通信**：
  - ASR 引擎通信：使用极轻量的 C++ WebSocket 库（如 `ixwebsocket` 或 `Boost.Beast`）与本地 Python 服务进行 PCM 音频流双向传输。
  - LLM 纠错请求：使用原生 WinHTTP 进行 API 请求。
  - JSON 解析：使用单头文件库 `nlohmann/json`。
- **构建系统**：CMake。

**【具体功能模块与实现要求】**

**1. 托盘图标与原生右键菜单 (Shell_NotifyIcon)**
- 应用无任务栏图标，仅存在于系统托盘。
- 托盘右键触发传统 Win32 菜单 (`CreatePopupMenu`)。*(注：参考 ../zencrop 项目的实现方式)*
- 菜单项包含：
  - `Version: v0.1.3` (灰色不可点击状态)
  - `Settings...` (点击打开设置窗口)
  - 分割线
  - `Quit`

**2. 全局自定义快捷键与录音控制 (WH_KEYBOARD_LL)**
- 使用低级键盘钩子全局监听。默认长按 `CapsLock` 300ms 后开始录音，松开停止；短按 `CapsLock` 保留系统大小写切换。
- 必须支持在 Settings 中自定义快捷键（如长按 CapsLock，或长按 Shift+CapsLock 等）。
- 钩子拦截到配置的快捷键时，必须 return 1 抑制事件向系统传递，防止打字干扰或触发系统原有功能；`CapsLock` 短按场景需要补发系统按键事件，长按结束后不得改变原 Caps Lock 状态。

**3. Direct2D 悬浮胶囊波形窗 (The HUD)**
- 录音时在屏幕底部居中弹出无边框悬浮窗 (`WS_POPUP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED`)。
- 当前高度约 56 DIP，圆角半径随高度取半径；使用 Direct2D/DirectWrite 渲染，暂未启用 DWM 亚克力/云母模糊背景。
- **左侧波形**：5根垂直条。必须由录音线程实时计算的 PCM 数据 RMS 电平驱动（绝对不要假动画）。
  - 权重分布：`[0.5, 0.8, 1.0, 0.75, 0.55]`。
  - 当前实现：由 `waveIn` 录音 buffer 计算 PCM RMS，Attack 40%，Release 15%，录音时约 33ms 重绘。
- **右侧文字**：当前显示 Listening/Recognizing/final/error 状态；窗口按 DirectWrite 文本测量结果动态调整宽度，并在高 DPI 下做 DIP 到物理像素转换。
- **动画**：当前已实现音量条实时动画；弹簧入场、宽度平滑过渡、退场缩放仍是后续视觉优化项。

**4. ASR 流式通信与极保守的 LLM 纠错**
- **当前 ASR 通信**：松开快捷键后将录音写入 WAV，通过本地 TCP JSON line 发送 `recognize` 请求到 `127.0.0.1:18088`，等待最终识别结果。
- **后续流式 ASR 通信**：按下快捷键录音时，连接本地 Python 服务 `ws://127.0.0.1:18088/asr`。以 100ms 为 Chunk 将音频流发送给服务端；同时接收服务端的实时文本更新 HUD 窗口。松开按键发送 stop，等待最终识别结果。
- **LLM 纠错**：拿到最终结果后，若启用了 LLM，悬浮窗文字变为灰色并显示 "Refining..."。后台使用 WinHTTP 异步请求 OpenAI 兼容接口。
- LLM System Prompt 要求极其保守：**"只修复明显的中文谐音错误和被错误翻译为中文的技术术语(如Python/JSON/API)。绝对禁止改写、润色、增加或删除任何字词。如果原句看起来没有错误，必须原样输出原句。"**

**5. 剪贴板注入与输入法 (IME) 完美对抗**
- 拿到最终打磨好的文本后，保存当前剪贴板内容。
- 将文本写入剪贴板。
- **关键拦截对抗**：使用 `ImmGetContext` 和 `ImmSetConversionStatus` 临时将当前前台焦点窗口的 CJK 输入法状态关闭（强制切换为英文），绝对防止中文拼音输入法拦截快捷键。
- 使用 `SendInput` 模拟 `Ctrl + V`。
- 恢复原输入法状态，恢复剪贴板原始内容。

**6. 设置窗口 (Settings Dialog) 与配置持久化**
- 提供一个标准的 Win32 Dialog 窗口。
- 包含控件：快捷键录制框 (支持捕获修饰键和主键)、LLM 启用开关、API Base URL 输入框、API Key 输入框 (可清空，密码遮罩)、Model 输入框。
- 配置信息本地序列化保存到 `%APPDATA%\MyAppName\config.json`。




## 适配需求
https://github.com/FireRedTeam/FireRedASR2S 
