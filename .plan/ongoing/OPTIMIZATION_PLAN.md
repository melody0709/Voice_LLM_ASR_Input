# Optimization Plan

本文用于记录优化路线。每完成一项就在对应 checkbox 打勾，并在”完成后推荐”里继续追加下一步建议。

## 当前基线

- 当前版本：`v0.1.5`
- 前端：单文件 Win32 C++，核心在 `main.cpp`
- 录音：`waveIn`，16kHz mono PCM，约 100ms buffer
- ASR：C++ 直接调用 sherpa-onnx（`OfflineRecognizer`、`OfflinePunctuation`）
- VAD：Silero VAD（sherpa-onnx 内置）或 FireRed VAD（`firered_vad.h`，Settings 可切换）
- 识别：录音结束后直接调用 C++ API，无中间进程和网络开销
- HUD：Direct2D/DirectWrite 胶囊窗，PCM RMS 驱动 5 根音量条
- 主要限制：尚无真实 partial，剪贴板未恢复，缺少完整性能日志和可视化日志

## 已完成

- [x] v0.1.3 版本化：README、CHANGELOG、资源版本、manifest、托盘菜单同步到 `v0.1.3`
- [x] HUD 从 GDI 固定绘制升级到 Direct2D/DirectWrite
- [x] HUD 文字尺寸使用 DirectWrite DIP 测量，并按当前 DPI 转 Win32 物理像素
- [x] HUD 5 根音量条由实时 PCM RMS 驱动
- [x] 修复高 DPI 下 HUD 默认文本裁切、文字不居中、圆角黑边问题
- [x] 文档补充 HUD/DPI 踩坑规则，避免后续再次混用 DIP 和物理像素
- [x] v0.1.4 移除 Python worker，C++ 直接调用 sherpa-onnx
- [x] v0.1.5 接入 FireRedVAD，Settings 可切换 Silero/FireRed

## P0：先稳住日常可用性

### 1. HUD 视觉验收与微调

- [ ] 实机确认 100% / 125% / 150% DPI 下默认文本完整显示
- [ ] 实机确认多显示器下 HUD 出现在当前光标所在显示器底部
- [ ] 实机确认录音音量条随真实麦克风输入变化，不在静音时乱跳
- [ ] 调整 HUD 停留时间：最终文本不要一闪而过，错误文本要足够读完

验收标准：
- 默认 `Listening... FireRedASR2 ...` 不裁切
- 圆角无黑边
- 字体垂直居中
- 音量条运动明显但不刺眼

完成后推荐：
- 继续做“最终文本展示策略”：短文本单行显示，长文本最多 2-3 行后自动收起。

### 2. 剪贴板注入安全化

- [ ] 粘贴前保存原剪贴板文本
- [ ] `Ctrl+V` 后延迟恢复原剪贴板
- [ ] 恢复失败时不影响最终文本上屏
- [ ] 增加 fallback：剪贴板失败时用 Unicode `SendInput` 逐字输入
- [ ] 检测高权限窗口，给出 HUD 提示而不是静默失败

验收标准：
- 普通编辑器、浏览器输入框、微信/QQ/Obsidian/VS Code 至少各测一次
- 原剪贴板内容能恢复
- 目标应用粘贴慢时不被过早恢复打断

完成后推荐：
- 研究 IMM 输入法状态临时切换，减少中文输入法拦截粘贴快捷键的概率。

### 3. 性能日志落地

- [ ] C++ 记录录音时长、VAD 耗时、ASR 耗时、标点耗时、粘贴耗时
- [ ] 写入 `%APPDATA%\VoxType\logs\app.log`
- [ ] 日志滚动：最多保留最近 5 个文件，每个文件最多 1-2 MB
- [ ] Settings 或托盘菜单增加 `Open Logs`

验收标准：
- 每次识别都能看到一行完整时序
- 首次加载和模型复用能明显区分
- 失败原因能定位到录音、worker、ASR、标点或注入阶段

完成后推荐：
- 加一个简单 benchmark 命令，批量跑固定 WAV 文件对比模型性能。

## P1：提高识别体验

### 4. ASR 引擎可观测性

- [ ] C++ 记录模型加载耗时、VAD 耗时、ASR 耗时、标点耗时
- [ ] 模型文件缺失或加载失败时 HUD 显示明确错误
- [ ] FireRedVAD / Silero VAD 切换后自动重新加载模型
- [ ] 增加结构化日志：模型 ID、耗时、异常信息

验收标准：
- 模型路径错、加载失败时都能显示可读错误
- 每次识别都能看到完整时序

完成后推荐：
- 加一个简单 benchmark 命令，批量跑固定 WAV 文件对比模型性能。

### 5. 模型默认值和质量压测

- [ ] 准备固定测试语料：短句、长句、中英混说、技术词、噪声、方言
- [ ] 对 FireRed CTC / FireRed AED / SenseVoice 分别记录 RTF 和主观错误
- [ ] 比较 `postprocess = none / itn` 的标点质量和耗时
- [ ] 确认默认模型是否继续使用 FireRed CTC
- [ ] 写入 `BENCHMARK.md`

验收标准：
- 至少 20 条真实使用语音样本
- 每个模型有平均耗时、最慢耗时、明显错误案例

完成后推荐：
- 根据数据决定是否把 AED 作为质量模式，SenseVoice 作为低配模式。

### 6. 术语替换和用户词库

- [ ] 增加 `%APPDATA%\VoxType\terms.json`
- [ ] 支持简单替换：错误词 -> 正确词
- [ ] 支持大小写敏感选项
- [ ] 支持中文技术词：派森 -> Python，杰森 -> JSON，温三十二 -> Win32
- [ ] Settings 增加打开词库文件入口

验收标准：
- 替换只作用于最终结果
- 不破坏数字、路径、URL、代码片段

完成后推荐：
- 再考虑保守 LLM 纠错，默认必须关闭。

## P2：接近参考项目的流式体验

### 7. 模拟 partial

- [ ] 录音时每隔 800-1200ms 取当前 PCM 快照
- [ ] 后台临时识别快照，不阻塞主录音
- [ ] HUD 显示不稳定 partial，最终仍以松开后的完整识别为准
- [ ] 如果模型太慢或文本抖动明显，可以自动关闭模拟 partial

验收标准：
- 不影响最终识别结果
- 不明显增加 CPU 卡顿
- HUD 能优雅处理 partial 回退和重写

完成后推荐：
- 如果模拟 partial 体验差，直接跳到 streaming 模型实验，不要在 offline 模型上硬磨。

### 8. 真实 streaming ASR 实验

- [ ] 下载并单独验证 sherpa-onnx online/streaming 中文或中英模型
- [ ] 写独立 `streaming_worker_experiment.py`
- [ ] 验证 `OnlineRecognizer` 的 partial 延迟、CPU 占用、准确率
- [ ] 决定协议：WebSocket 还是长连接 TCP 二进制帧
- [ ] 只在实验稳定后接入主程序

验收标准：
- 100ms PCM chunk 能持续送入 worker
- 300-800ms 内能看到可用 partial
- 松开后 final 稳定，不比当前 offline 明显差

完成后推荐：
- 把 `enable_partial` 从 UI 预留变成真实功能开关。

### 9. 录音链路升级到 WASAPI

- [ ] 新增 WASAPI Shared Mode 捕获模块
- [ ] 转换系统默认采样率到 16kHz mono s16le
- [ ] 保持 waveIn fallback
- [ ] 比较 waveIn 和 WASAPI 的启动延迟、稳定性、权限问题

验收标准：
- 默认麦克风切换后能自动跟随或给出提示
- 不比 waveIn 更容易失败

完成后推荐：
- streaming ASR 接入前优先完成 WASAPI，避免后面重写音频输入层。

## P3：产品化和维护

### 10. Settings 完整化

- [ ] Settings 支持打开日志目录
- [ ] Settings 支持打开配置文件
- [ ] Settings 支持打开模型目录
- [ ] Settings 校验模型目录并提示缺失文件
- [ ] Settings 显示当前 worker 状态和最近一次耗时

完成后推荐：
- 增加导入/导出配置，方便跨机器迁移。

### 11. 安装和发布

- [ ] 明确模型不随 exe 打包
- [ ] 写 `INSTALL.md`
- [ ] 写常见问题：模型目录、管理员窗口注入失败、FireRedVAD 模型缺失
- [ ] 评估是否做 zip 绿色包

完成后推荐：
- 再考虑模型下载器或首次启动向导。

## 每轮优化后的固定动作

- [ ] 更新 `CHANGELOG.md`
- [ ] 更新 `README.md` 当前能力和限制
- [ ] 必要时更新 `ARCHITECTURE.md`
- [ ] 必要时更新 `AGENTS.md` 踩坑规则
- [ ] 运行 `.\build.bat`
- [ ] 做 1 次启动冒烟测试

## 下一步推荐顺序

1. 先做 P0-2：剪贴板保存/恢复。这是最容易影响真实日常使用的点。
2. 再做 P0-3：性能日志。没有日志，后面模型和流式优化会靠感觉。
3. 然后做 P1-4：ASR 引擎可观测性。先让失败可解释。
4. 再做 P1-5：模型质量压测。决定默认模型和线程策略。
5. 最后再进入 P2：partial/streaming。流式体验很诱人，但要在稳定输入链路和日志之后做。
