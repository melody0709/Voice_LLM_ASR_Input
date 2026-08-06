# Qwen Free 脱离千问输入法安装的独立运行方案

> 状态：研究已归档，暂不实施
>
> 日期：2026-08-06
>
> 目标：让已经使用过 Qwen Free 的用户在卸载千问输入法后，仍能在 VoxType 中继续使用 Qwen Free ASR 和 VoiceInputWrite 后处理。

---

## 结论

当前 Qwen Free 不要求千问输入法进程正在运行，也不要求它仍注册为 Windows 输入法，但仍依赖千问安装目录中的设备身份和签名组件。

现阶段直接卸载千问输入法并删除其安装目录，VoxType 重启后大概率无法继续使用 Qwen Free。

对已经安装并使用过千问输入法的个人用户，技术上可实现“迁移后卸载”：

1. 卸载前取得当前有效 UTDID。
2. 使用 Windows DPAPI 把 UTDID 加密保存到 VoxType 用户数据。
3. 保留与当前实现兼容的 unet.dll，并放在用户私有运行时目录。
4. 让 Qwen Free 从私有运行时目录加载 unet.dll，不再探测千问安装目录。
5. 在完全屏蔽原千问安装目录的条件下完成 ASR、润色和选区改写验证。
6. 验证通过后再卸载千问输入法。

从未安装过千问输入法的新用户不能直接使用上述迁移方式，因为他们同时缺少已初始化的 UTDID 和兼容的 unet.dll。该场景应继续使用官方 DashScope Qwen 后端，或另立项目研究完整的设备初始化与纯协议实现。

---

## 当前实现依赖

### 1. UTDID 设备身份

当前获取顺序见 [qwen_free_proto_utdid.cpp](../../src/asr/qwen_free_proto_utdid.cpp)：

1. qwenFreeUtdidOverride 配置覆盖值。
2. 从指定目录或千问安装目录加载 UTDID.dll，并调用 getUTDID。
3. 扫描已知 UTDID 注册表位置。

录音 session 在建立协议前必须先取得有效 UTDID，失败会终止 Qwen Free session，见 [qwen_free_streaming_session.cpp](../../src/asr/qwen_free_streaming_session.cpp)。

本机研究结果：

- 已检查的常见 UTDID 注册表位置均不存在。
- 当前有效 UTDID 能通过 UTDID.dll 的缓存读取路径取得。
- 在千问本地日志中能找到该设备标识的使用记录，但尚未确认真正的持久化缓存文件位置。
- 因此不能依赖“卸载后缓存自然保留”，必须在卸载前由 VoxType 主动取得并保存设备身份。
- 研究记录和计划文档不得写入真实 UTDID。

### 2. unet.dll 的 WSG 签名和加密

当前实现通过 [qwen_free_proto_unet.cpp](../../src/asr/qwen_free_proto_unet.cpp) 加载 unet.dll，执行：

- ASR 和 LLM 请求的 WSG 签名。
- ASR encrypted UTDID 生成。
- VoiceInputWrite 的加密 KPS/VEKP 生成。

原有纯 HMAC fallback 已从运行路径和源码中的 provider 专用固定材料中移除。当前实现缺少兼容 `unet.dll` 或正确设备密文时会在联网前失败，并交给统一备用 ASR 策略。

已有静态依赖和探针研究表明：

- unet.dll 可独立 LoadLibrary，不需要千问主程序或 shell_ffi.dll 才能注册 native FFI。
- 其直接依赖主要是 Windows 系统组件；dbghelp.dll 为延迟依赖。
- 取得并持久化 UTDID 后，正常运行阶段理论上不再需要 UTDID.dll。

但当前实现直接使用两个版本相关的固定 RVA：

- core sign RVA
- core encrypt RVA

所以私有运行时必须保存经过验证的同版本 unet.dll。未来加载器还应校验文件指纹和 PE 布局，不能对任意新版 unet.dll 盲目调用固定地址。

### 3. 当前配置和 Settings 状态

配置结构见 [globals.h](../../src/app/globals.h)，读写见 [engine.cpp](../../src/audio/engine.cpp)，界面见 [settings.cpp](../../src/ui/settings.cpp)。

当前已有：

- qwenFreeShellPath：Settings 中可见，用于覆盖 DLL 目录。
- qwenFreeUtdidOverride：已有配置读写，但 Settings 没有可见输入控件。

当前不足：

- UTDID override 已在当前工作区改为 DPAPI 加密保存，并兼容迁移旧明文；正式独立运行时仍建议使用单独的设备身份 blob 和迁移元数据。
- 没有迁移、重置或验证设备身份的 UI。
- 当前加载器已对已验证 `unet.dll` 使用 SHA-256 白名单；独立运行时仍需设计可扩展的迁移版本和多指纹记录。
- 没有“忽略千问安装目录，只测试私有运行时”的测试模式。
- README 已明确 UTDID override 是配置级诊断入口，不是普通 Settings 控件。

---

## 推荐的个人迁移架构

建议的最小运行状态：

    VoxType 用户数据
    ├─ DPAPI 加密的 Qwen Free 设备身份
    ├─ 迁移版本和 unet.dll 文件指纹
    └─ Qwen Free 私有运行时路径
       └─ unet.dll

关键原则：

- UTDID 不以明文写入 config.json、日志、HUD、MessageBox 或计划文档。
- 不把 unet.dll 提交到 Git、构建包或公开发布包。
- VoxType 发布包不自动附带千问组件。
- 私有运行时必须由用户从自己机器上的现有安装中明确迁移或选择。
- 迁移完成后的真实验证必须让原千问安装目录不可访问，不能只关闭千问进程。

---

## 未来实施方案

### Phase 0：隔离验证，不改正式配置

目标：先证明“有效 UTDID + 单独 unet.dll”足以覆盖完整 ASR/LLM 流程。

任务：

1. 从当前 GetUtdid 路径读取 UTDID，但不输出原值。
2. 准备一个不位于千问安装目录的临时私有运行时目录。
3. 让测试探针仅从该目录加载 unet.dll。
4. 禁止测试代码回退探测 C:/Program Files/QianwenIME。
5. 依次验证：
   - ASR WebSocket upgrade。
   - user.session.start。
   - 实际语音 final。
   - Polish/VoiceInputWrite HTTP 请求。
   - 选区改写请求。
6. 重启测试进程后再次验证，排除“DLL 已经在进程中加载”的假成功。

通过条件：

- 原千问目录完全不可见时，ASR 和已启用的 LLM 后处理都成功。
- 进程重启后仍成功。
- Process Explorer/模块列表中不加载 UTDID.dll、shell_ffi.dll 或千问主程序 DLL。

### Phase 1：设备身份安全持久化

建议新增专用的 Qwen Free 身份存储，不复用明文 qwenFreeUtdidOverride。

可选字段：

- qwen_free_device_identity_blob
- qwen_free_device_identity_version
- qwen_free_identity_source
- qwen_free_identity_migrated_at

要求：

- 使用 CryptProtectData，绑定当前 Windows 用户。
- 解密后再次验证必须是 24 字符 base62，且不能是 24 个 f。
- 加密/解密失败时清楚提示重新迁移，不回显原值。
- SaveConfig 和 debug log 都不得打印解密后的 UTDID。
- 保留 qwenFreeUtdidOverride 仅供本地调试，正式 UI 不直接暴露。

根据项目配置规则，实施时必须同步：

- src/app/globals.h 的 Config 字段。
- src/audio/engine.cpp 的 LoadConfig/SaveConfig。
- src/ui/settings.cpp 的 Settings 控件和状态。

### Phase 2：私有运行时加载器

建议把 qwenFreeShellPath 的语义从“千问安装目录覆盖”明确为“Qwen Free runtime directory”。

加载顺序建议：

1. 用户配置的私有运行时目录。
2. 迁移记录中的私有运行时目录。
3. 兼容旧配置时，才探测千问安装目录。

加载前检查：

- unet.dll 文件存在。
- PE 架构为 x64。
- unet_native_bind 导出存在。
- 当前固定 RVA 位于可执行映像页。
- 文件 SHA-256 与迁移时记录一致。
- 可选：记录文件大小、时间戳和产品版本，帮助诊断版本漂移。

失败策略：

- 缺少 unet.dll：提示 Qwen Free private runtime is missing。
- 文件指纹变化：拒绝调用固定 RVA，提示 runtime version is incompatible。
- native bind 或 signer 初始化失败：直接报告鉴权运行时不可用。
- 不再把 HMAC fallback 表现为可能正常工作的运行路径。

### Phase 3：Settings 迁移流程

建议在 Qwen Free 区域增加：

- Migrate Runtime
- Test Standalone
- Reset Identity
- Runtime Status

Migrate Runtime 流程：

1. 检测当前千问安装目录。
2. 调用 GetUtdid 并验证设备身份。
3. 使用 DPAPI 保存设备身份。
4. 让用户选择或确认私有运行时目录。
5. 迁移/选择兼容的 unet.dll。
6. 保存文件指纹和运行时路径。
7. 自动执行 standalone ASR handshake。
8. 启用 Polish 时继续测试 VoiceInputWrite。
9. 只有所有必需测试通过才显示 Ready to uninstall Qianwen IME。

Reset Identity：

- 删除 VoxType 保存的 DPAPI blob 和迁移元数据。
- 不删除用户指定的外部目录或千问安装目录。
- 重置后 Qwen Free 回到未迁移状态。

### Phase 4：卸载前模拟测试

不能用“关闭千问输入法进程”代替该测试。

建议测试条件：

1. 完全退出 VoxType 和千问相关进程。
2. 暂时让千问安装目录不可被探测，例如在测试环境中改名或隔离。
3. 启动规范运行载荷 build/run/x64-release/VoxType.exe。
4. 确认 Qwen Free 状态显示来自 private runtime 和 protected identity。
5. 执行真实麦克风测试。
6. 重新启动 VoxType，再测试一次。
7. 验证 ASR、Polish、选区改写和微信输入。
8. 恢复网络后验证断网重连和下一次录音恢复。

只有该阶段全部通过，用户才可以安全卸载千问输入法。

---

## 验收清单

### 正常路径

- 未安装或无法探测千问输入法时，Qwen Free ASR 正常工作。
- VoxType 重启后仍正常工作。
- Polish 正常工作。
- 选区改写正常工作。
- 普通粘贴和微信 WM_CHAR 路径不受影响。
- Test Connection 明确显示身份来源和运行时来源，但不显示敏感值。

### 缺失和损坏

- DPAPI blob 缺失：提示需要迁移设备身份。
- DPAPI 解密失败：不崩溃、不回显密文或 UTDID。
- unet.dll 缺失：立即报告私有运行时缺失。
- unet.dll 被替换或升级：文件指纹检查失败，禁止调用固定 RVA。
- 配置路径无效：允许重新选择目录。
- UTDID 格式无效：禁止发起网络请求。

### 网络和服务失败

- 断网时保持现有可恢复错误分类和有界重连。
- HTTP 400/401/403/404 和 D30112 不做无意义重试。
- WebSocket 失败后下一次录音能够重新创建 session。
- LLM 后处理失败时保留 ASR 原文，不丢失识别结果。
- 所有用户可见错误继续脱敏。

### UI

- 96/144/192/288 DPI 下 Settings 不裁切、不重叠。
- 迁移和测试在后台线程执行，不阻塞 Settings UI。
- Settings 打开时继续不拦截录音快捷键。

---

## 三类用户的支持边界

| 用户场景 | 可行性 | 推荐路线 |
| --- | --- | --- |
| 已安装并使用过千问，准备卸载 | 高 | 迁移 UTDID + 私有 unet.dll runtime |
| 已卸载，但仍保留有效 UTDID 和兼容 unet.dll | 中 | 手动导入身份和 runtime，再做完整验证 |
| 从未安装过千问 | 当前不可直接支持 | 使用官方 DashScope Qwen，或另立纯实现研究 |
| VoxType 公开发布包 | 不应附带千问 DLL | 默认使用官方接口，Qwen Free 只接受用户私有 runtime |

---

## 完全纯实现的独立研究方向

如果未来目标变成“新用户从未安装千问也能使用 Qwen Free”，需要另开计划，至少解决：

- 每个 WSG number 对应的完整签名参数和密钥逻辑。
- EncryptWithNumber 的完整加密算法。
- ASR encrypted UTDID 生成。
- LLM KPS/VEKP 生成。
- 新设备 UTDID/AUDID 初始化和服务端注册。
- 设备生命周期、失效和重新注册策略。
- 上游协议变化后的兼容维护。

现有 Ghidra 资料可从以下目录继续：

- reverse/projects/ghidra/output/unet/
- reverse/projects/ghidra/output/UTDID/
- reverse/docs/findings/GHIDRA_FINDINGS.md
- reverse/docs/protocol/PROTOCOL.md

该方向工作量明显高于个人迁移模式，而且公开发布还需单独评估许可、服务条款和长期维护风险，不应与第一阶段迁移功能混在同一次修改中。

---

## 不实施的内容

本计划当前只归档研究，不进行以下操作：

- 不复制或移动千问安装目录中的 DLL。
- 不保存当前真实 UTDID。
- 不修改 VoxType 配置格式。
- 不增加 Settings 控件。
- 不改变 Qwen Free 加载或 fallback 行为。
- 不卸载、禁用或改名千问输入法。
- 不修改版本号、README、CHANGELOG 或运行代码。

未来正式实施前，应先重新检查上游 DLL 版本、当前 Qwen Free 协议状态和本计划中的假设。
