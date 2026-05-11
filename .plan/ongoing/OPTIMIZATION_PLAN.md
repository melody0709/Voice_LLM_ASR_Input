# Optimization Plan

本文用于记录优化路线。每完成一项就在对应 checkbox 打勾，并在"完成后推荐"里追加下一步建议。

## 当前基线

- 当前版本：`v0.7.3`
- 已实现功能：
  - 本地 ASR：sherpa-onnx OfflineRecognizer + Punct，支持 FireRed CTC/AED、SenseVoice
  - 云端 ASR：百度云、火山引擎（豆包）WebSocket 流式，含 nostream/async 多种模式
  - 双 VAD：Silero（轻量）/ FireRed（高精度），Settings 可切换
  - LLM 纠错：DeepSeek / OpenRouter / SiliconFlow 等多 Provider，可自定义 Prompt 和 Extra Params
  - 录音：WASAPI Shared Mode（48kHz→16kHz 线性重采样）+ waveIn fallback
  - HUD：Direct2D/DirectWrite 胶囊窗，5 根音量条
  - 托盘常驻，CapsLock 长按录音短按切换
  - DLL 延迟加载：纯云端模式空闲 ~12 MB
  - 模型预加载：本地模式启动时后台加载
  - 模型下载器：Settings 内一键下载

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

### Audio
- [x] WASAPI Shared Mode 核心捕获（`wasapi_capture.h/cpp`）+ 线性插值重采样
- [x] waveIn 自动 fallback
- [x] `CalculateAudioLevelFloat`（float32 RMS + dB 归一化）
- [x] Config 字段 `audioBackend` / `audioDeviceId`（Load/Save 持久化）
- [x] Debug Console 显示音频后端信息（WASAPI 采样率+设备名 / waveIn）
- [x] WASAPI 生命周期：`Init → Start → Stop → Release`

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
- [x] 线程安全：volc `connected` atomic、Baidu token mutex、`VolcDebugLog` mutex
- [x] 资源泄漏：`g_volcAudioCs` 全部退出路径 Delete、PositionHud region 去重
- [x] 模型下载器异步化（不再阻塞 UI）
- [x] 火山引擎连接复用（hSession+hConnect KeepAlive）

---

## P0：日常可靠性（优先修复）

### 1. 剪贴板注入安全化

> 当前是最影响日常使用的遗留项。

- [ ] 粘贴前保存原剪贴板文本
- [ ] `Ctrl+V` 后延迟恢复（避开目标应用粘贴处理时间）
- [ ] 恢复失败时不影响最终文本上屏
- [ ] fallback：剪贴板不可用时用 Unicode `SendInput` 逐字输入
- [ ] 检测高权限窗口（Admin），给出 HUD 提示而非静默失败

验收标准：
- 微信/QQ/Obsidian/VS Code/Chrome 至少各测一次
- 原剪贴板内容可恢复
- 目标应用粘贴慢时不被过早恢复打断

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

---

## P1：识别体验提升

### 4. WASAPI Phase 2：设备选择 UI

> Config 字段已就绪、持久化已完成。仅差 Settings 控件。

- [ ] Settings 增加 "Audio" 分组：后端下拉框（WASAPI / waveIn Legacy）
- [ ] 录音设备下拉框（`WasapiCapture::EnumerateDevices()` 填充）
- [ ] 显示当前设备原生采样率（只读提示）
- [ ] 设备切换后 Save → Reload → 预加载（如需要）

验收标准：
- 切换设备后下一次录音立即生效
- 默认设备变更时能自动检测

### 5. 术语替换 / 用户词库

- [ ] 增加 `%APPDATA%\VoxType\terms.json`，简单替换表
- [ ] 支持大小写敏感选项
- [ ] 常见中文技术词替换示例：派森→Python，杰森→JSON
- [ ] Settings 增加打开词库文件入口

验收标准：
- 替换仅作用于最终结果
- 不破坏数字、路径、URL

完成后推荐：
- 再考虑保守 LLM 纠错开关，默认必须关闭。

### 6. 模型质量评估

- [ ] 准备固定测试语料：短句、长句、中英混说、技术词、噪声
- [ ] 对 FireRed CTC / FireRed AED / SenseVoice 分别记录 WER/RTF
- [ ] 比较 postprocess = none / itn 的标点质量
- [ ] 写入 `BENCHMARK.md`

验收标准：
- 至少 20 条真实语音样本
- 每个模型有平均耗时、最慢耗时、明显错误案例

---

## P2：流式体验

### 7. 模拟 partial（可选）

> 在 offline 模型上分段快照识别，模拟流式反馈。如果效果差则直接跳到 §8。

- [ ] 录音时每隔 ~1s 截取当前 PCM 快照
- [ ] 后台临时识别快照，不阻塞主录音
- [ ] HUD 显示不稳定 partial，最终仍以完整识别为准
- [ ] 若文本抖动明显或 CPU 过高，自动关闭此功能

验收标准：
- 不影响最终识别结果
- 不明显增加 CPU 卡顿
- HUD 能优雅处理 partial 回退

### 8. Streaming ASR 实验

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

---

## P3：产品化

### 9. Settings 完整化

- [ ] Settings 增加打开日志目录按钮
- [ ] Settings 增加打开配置文件按钮
- [ ] Settings 校验模型目录完整性（提示缺失文件）
- [ ] 增加导入/导出配置（zip 或单文件 JSON）
- [ ] LLM Provider 列表支持拖拽排序

### 10. 安装和发布

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

1. **P0 §1 剪贴板恢复** — 最影响日常使用，每次注入都会覆盖剪贴板
2. **P0 §2 HUD 视觉** — 实机 DPI 验收 + 停留时间调整，投入小收益大
3. **P1 §4 设备选择 UI** — Config 已就绪，仅差 Settings 控件，工时小
4. **P0 §3 错误处理** — 让失败可解释，减少用户困惑
5. **P1 §5 术语替换** — 低风险、可逐步积累
6. **P2 §8 Streaming ASR** — 体验提升最大，但需要实验验证
7. **P1 §6 模型评估** — 决定默认模型策略
8. **P3 §9-10** — 最后做，稳定后再发布
