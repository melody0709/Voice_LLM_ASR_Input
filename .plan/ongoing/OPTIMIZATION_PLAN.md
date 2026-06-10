# Optimization Plan

本文用于记录优化路线。每完成一项就在对应 checkbox 打勾，并在"完成后推荐"里追加下一步建议。

## 当前基线

- 当前版本：`v0.9.0`
- 已实现功能：
  - 本地 ASR：sherpa-onnx OfflineRecognizer + Punct，支持 FireRed CTC/AED、SenseVoice
  - 云端 ASR：百度云、火山引擎（豆包）WebSocket、Qwen ASR（DashScope `qwen3-asr-flash-realtime`）
  - 双 VAD：Silero（轻量）/ FireRed（高精度），Settings 可切换
  - LLM 纠错：DeepSeek / OpenRouter / SiliconFlow 等多 Provider，可自定义 Prompt 和 Extra Params
  - 录音：WASAPI Shared Mode（48kHz→16kHz 线性重采样）+ waveIn fallback
  - HUD：Direct2D/DirectWrite 胶囊窗，5 根音量条
  - 托盘常驻，CapsLock 长按录音短按切换
  - DLL 延迟加载：纯云端模式空闲 ~12 MB
  - 模型预加载：本地模式启动时后台加载
  - 模型下载器：Settings 内一键下载
  - 剪贴板注入：微信 WM_CHAR、其他应用 IMM32 + Ctrl+V、Force Unicode Input 菜单

## 已完成

### 核心链路
- [x] v0.2.x：LLM 纠错集成（DeepSeek / OpenAI-compatible），CapsLock 热键长按录音短按切换
- [x] v0.3.x：百度云 ASR
- [x] v0.4.x：火山引擎流式 ASR（WebSocket binary protocol）
- [x] v0.5.x：Cloud ASR UI 统一、Tab 合并、volcengine nostream 修复
- [x] v0.6.0：源码模块化（单文件 → 6 个编译单元）
- [x] v0.6.1：模型预加载、DELAYLOAD DLL（纯云端 ~12 MB）
- [x] v0.6.2：UiStyle 常量集中化
- [x] v0.7.0：火山引擎全参数支持、热词/纠错表、对话上下文、模型下载器
- [x] v0.7.1：volcengine nostream 性能优化、WinHTTP 连接复用
- [x] v0.7.2：CODE_REVIEW 修复 13 项（线程安全、资源泄漏、SSL、工具函数去重等）
- [x] v0.7.3：火山引擎流式 ASR 修复（round1: atomic/初始音频丢失/脏音频残留；round2: async 双线程竞争/SendAudio 失败处理/WASAPI Init 脏状态）
- [x] v0.7.4：微信中文输入法粘贴修复（WM_CHAR 绕过 IME）、IMM32 输入法状态切换、Unicode SendInput fallback、Force Unicode Input 菜单
- [x] v0.8.x：流式 VAD、火山引擎连接复用、输入框上下文、空结果 replay retry、自适应 finalize timeout
- [x] v0.9.0：Qwen ASR 接入、边录边发 partial HUD、Manual turn detection 固定策略、统一 ASR session/dispatcher/result 架构

### Audio
- [x] WASAPI Shared Mode 核心捕获（`wasapi_capture.h/cpp`）+ 线性插值重采样
- [x] waveIn 自动 fallback
- [x] `CalculateAudioLevelFloat`（float32 RMS + dB 归一化）
- [x] Config 字段 `audioBackend` / `audioDeviceId`（Load/Save 持久化）
- [x] Debug Console 显示音频后端信息（WASAPI 采样率+设备名 / waveIn）
- [x] WASAPI 生命周期：`Init → Start → Stop → Release`
- [x] WASAPI Init 失败后脏状态修复（Release 清理）

### 可观测性
- [x] Debug Mode：托盘右键 checkbox，CMD 控制台实时打印各阶段耗时
- [x] Debug 输出格式：`-- HH:MM:SS  Rec X.Xs(XXKB) --` + Pipeline + text lines
- [x] 各阶段计时：VAD / ASR / Punct / LLM / Paste，Total 不含录制时长
- [x] Local / Baidu / Volcengine × 有无 LLM 均覆盖

### 工程质量
- [x] 源码模块化（main / engine / hud / hotkey / settings / wasapi_capture）
- [x] `utils.h` 统一工具函数（WideToUtf8 / Utf8ToWide / EscapeJson / Trim）
- [x] `AsrEngine::lock_` 改为 private + Lock/Unlock
- [x] SSL 证书验证恢复
- [x] 线程安全：volc `connected`/`streaming` atomic、Baidu token mutex、`VolcDebugLog` mutex
- [x] 资源泄漏：`g_volcAudioCs` 全部退出路径 Delete、PositionHud region 去重
- [x] 模型下载器异步化（不再阻塞 UI）
- [x] 火山引擎连接复用（hSession+hConnect KeepAlive）

---

## P0：日常可靠性（优先修复）

### 1. 剪贴板注入安全化

> 当前是最影响日常使用的遗留项。

- [x] 微信粘贴：通过进程名检测微信（`Weixin.exe`），用 `WM_CHAR` 逐字符发送绕过 IME
- [x] 其他应用：剪贴板 + Ctrl+V + IMM32 切换（临时关闭中文输入法）
- [x] `GetFocus()` 失败时的 fallback 机制

验收标准：
- [x] 微信/QQ/Obsidian/VS Code/Chrome 至少各测一次
- 微信中文输入法下能正常粘贴
- 其他应用粘贴行为不变

完成后推荐：
- 研究 IMM 输入法状态临时切换，减少中文输入法拦截 `Ctrl+V` 的概率。

### 2. HUD 视觉完善

- [ ] 实机确认 100% / 125% / 150% / 175% DPI 下文本完整显示
- [ ] 实机确认多显示器下 HUD 出现在当前光标所在显示器底部
- [ ] 调整 HUD 停留时间：最终文本不闪退，错误文本留足够可读时间
- [ ] 长文本（>80 字）自动换行或截断，避免 HUD 过宽

验收标准：
- 默认提示文案不裁切
- 圆角无黑边，字体垂直居中
- 音量条运动明显但不刺眼

### 3. 错误处理与恢复

- [ ] 本地 ASR 模型加载失败时 HUD 显示明确错误（当前只有 printf）
- [ ] 云端 API 连接失败/超时后自动重试 1 次
- [ ] 连续识别失败时托盘图标给出视觉提示
- [ ] 火山引擎连接断开后自动重建（当前仅首次连接时重试）

完成后推荐：
- 托盘图标状态灯：绿（就绪）/ 黄（录音中）/ 红（错误）/ 灰（空闲）。

### 4. 全局变量线程安全（与刚修的 g_volcStreaming 同类）

> 以下变量被多线程读写但无同步保护，和已修复的 `g_volcStreaming` 是同一类问题。

| 变量 | 写线程 | 读线程 | 当前类型 | 风险 |
|------|--------|--------|---------|------|
| `g_recording` | UI 线程 | hotkey hook | `bool` | 中：hotkey 判断可能过时 |
| `g_captureActive` | UI 线程 | WaveInProc/WASAPI callback | `bool` | 中：回调可能多收一个 buffer |
| `g_hudIsRefining` | UI 线程 | HUD paint | `bool` | 低：颜色闪烁 |
| `g_hudText` | UI 线程 (ShowHud) | HUD paint (DrawHudDirect2D) | `std::wstring` | **高：并发读写可能崩溃** |

**修复**：
- `g_recording` / `g_captureActive` / `g_hudIsRefining` → `std::atomic<bool>`
- `g_hudText` → 加 `CRITICAL_SECTION` 保护，或用 atomic flag + 双缓冲（一份写、一份读）

**涉及文件**：`globals.h`（声明）、`main.cpp`（定义和所有读写点）、`engine.cpp`（WaveInProc）、`wasapi_capture.cpp`（CaptureThread）、`hud.cpp`（DrawHudDirect2D）

验收标准：
- 全部改为 atomic 或加锁
- 编译通过，无功能回归

---

## P1：识别体验提升

### 5. WASAPI Phase 2：设备选择 UI

> Config 字段已就绪、持久化已完成。仅差 Settings 控件。

- [ ] Settings 增加 "Audio" 分组：后端下拉框（WASAPI / waveIn Legacy）
- [ ] 录音设备下拉框（`WasapiCapture::EnumerateDevices()` 填充）
- [ ] 显示当前设备原生采样率（只读提示）
- [ ] 设备切换后 Save → Reload → 预加载（如需要）

验收标准：
- 切换设备后下一次录音立即生效
- 默认设备变更时能自动检测

### 6. 术语替换 / 用户词库

- [ ] 增加 `%APPDATA%\VoxType\terms.json`，简单替换表
- [ ] 支持大小写敏感选项
- [ ] 常见中文技术词替换示例：派森→Python，杰森→JSON
- [ ] Settings 增加打开词库文件入口

验收标准：
- 替换仅作用于最终结果
- 不破坏数字、路径、URL

完成后推荐：
- 再考虑保守 LLM 纠错开关，默认必须关闭。

### 7. 模型质量评估

- [ ] 准备固定测试语料：短句、长句、中英混说、技术词、噪声
- [ ] 对 FireRed CTC / FireRed AED / SenseVoice 分别记录 WER/RTF
- [ ] 比较 postprocess = none / itn 的标点质量
- [ ] 写入 `BENCHMARK.md`

验收标准：
- 至少 20 条真实语音样本
- 每个模型有平均耗时、最慢耗时、明显错误案例

### 8. 音频管线性能优化

- [ ] `g_volcPendingAudio` 改 `std::deque<BYTE>`（当前 `erase(begin)` 做队列是 O(n) 每次）
- [ ] `RecognizeAsync` PCM 传递用 `std::move`（当前按值捕贝两份）
- [ ] `StopAudioCapture` 返回值用 `std::move(data)`（当前 `data = g_audioData` 全量拷贝）
- [ ] `g_audioData` 预分配（16kHz×2bytes = 32KB/s，可用 `reserve` 预估录音时长）
- [ ] HUD dirty flag：文本未变时跳过 `InvalidateRect`（当前 33ms 定时器全量重绘）

涉及文件：`engine.cpp`（StopAudioCapture、WaveInProc）、`main.cpp`（RecognizeAsync、volc 线程循环）、`hud.cpp`（ShowHud、DrawHudDirect2D）

---

## P2：流式体验 + 工程质量

### 9. 模拟 partial（可选）

> 在 offline 模型上分段快照识别，模拟流式反馈。如果效果差则直接跳到 §10。

- [ ] 录音时每隔 ~1s 截取当前 PCM 快照
- [ ] 后台临时识别快照，不阻塞主录音
- [ ] HUD 显示不稳定 partial，最终仍以完整识别为准
- [ ] 若文本抖动明显或 CPU 过高，自动关闭此功能

验收标准：
- 不影响最终识别结果
- 不明显增加 CPU 卡顿
- HUD 能优雅处理 partial 回退

### 10. Streaming ASR 实验

> WASAPI 已完成，这是 streaming 的前置条件。

- [ ] 下载 sherpa-onnx online/streaming 中文模型
- [ ] 写独立验证程序，接入 WASAPI 捕获循环
- [ ] 验证 `OnlineRecognizer` 的 partial 延迟、CPU 占用、准确率
- [ ] 决定接入方式：直接 C++ 调用 vs 独立进程
- [ ] 只在实验稳定后接入主程序

验收标准：
- 100ms PCM chunk 持续送入
- 300-800ms 内看到可用 partial
- Final 稳定，不比当前 offline 明显差

### 11. 代码质量清理

- [ ] `RecognizeAsync` / `RefineWithLlmAsync` 的 `detach()` 改为可控生命周期（保存 thread handle，退出时 join 或设 flag 让 PostMessage 跳过）
- [ ] `ExtractJsonString` 去重：`engine.cpp`（支持转义）和 `volcengine_asr.h`（不支持转义）各有一份，统一到 `utils.h`
- [ ] `LoadConfig` 中 `_wtoi` 替换为 `_wtoi_s` 或 `std::stoi`（已废弃）
- [ ] 单例 mutex 检查移到 `PreloadAsrEngine` 之前（当前第二实例会白加载模型）
- [ ] LLM `Refine` 超时从 5s 调大到 10-15s（慢网络下容易误报失败）
- [ ] `PreloadAsrEngine` 裸 Lock/Unlock 改 `lock_guard`（需暴露 mutex 或提供 guard 方法）

---

## P3：产品化

### 12. Settings 完整化

- [ ] Settings 增加打开日志目录按钮
- [ ] Settings 增加打开配置文件按钮
- [ ] Settings 校验模型目录完整性（提示缺失文件）
- [ ] 增加导入/导出配置（zip 或单文件 JSON）
- [ ] LLM Provider 列表支持拖拽排序

### 13. 安装和发布

- [ ] 写 `INSTALL.md`（含常见问题）
- [ ] 评估 zip 绿色包发布
- [ ] 明确模型不随 exe 打包的策略
- [ ] GitHub Actions 自动构建（可选）

---

## 每轮优化后的固定动作

- [ ] 更新 `CHANGELOG.md`
- [ ] 更新 `README.md` 当前能力和限制
- [ ] 必要时更新 `ARCHITECTURE.md`
- [ ] 必要时更新 `AGENTS.md` 踩坑规则
- [ ] 运行 `.\build.bat`
- [ ] 做 1 次启动冒烟测试

## 下一步推荐顺序

1. **P0 §4 线程安全** — 与刚修的 g_volcStreaming 同类，g_hudText 并发读写可能崩溃，投入小风险高
2. **P0 §1 剪贴板恢复** — 最影响日常使用，每次注入都会覆盖剪贴板
3. **P0 §2 HUD 视觉** — 实机 DPI 验收 + 停留时间调整，投入小收益大
4. **P1 §5 设备选择 UI** — Config 已就绪，仅差 Settings 控件，工时小
5. **P1 §8 音频管线性能** — g_volcPendingAudio erase O(n)、PCM 双拷贝等，长录音时有感
6. **P0 §3 错误处理** — 让失败可解释，减少用户困惑
7. **P1 §6 术语替换** — 低风险、可逐步积累
8. **P2 §11 代码质量** — detach threads、ExtractJson 去重等，降低维护风险
9. **P2 §10 Streaming ASR** — 体验提升最大，但需要实验验证
10. **P1 §7 模型评估** — 决定默认模型策略
11. **P3 §12-13** — 最后做，稳定后再发布
