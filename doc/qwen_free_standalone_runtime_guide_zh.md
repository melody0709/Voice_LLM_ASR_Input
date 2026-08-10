# Qwen IME (Free) 独立运行时指南

> 适用于 VoxType 的 `Qwen IME (Free)` 后端。本文说明如何先安装并初始化千问输入法，再把 VoxType 需要的运行时迁移到私有目录，最后卸载千问输入法。

## 重要结论

- 每台电脑都必须使用自己的 UTDID（设备级身份标识）。不要复制其他电脑的 `config.json` 或 UTDID。
- `unet.dll` 和 `UTDID.dll` 可以来自同一版本的千问安装包，但 DLL 相同不代表设备身份相同。
- 仅复制两个 DLL 尚未证明能在从未初始化过千问的全新电脑上工作；推荐先安装并使用一次千问输入法。
- 当前 `Polish (auto)` 的 `VoiceInputWrite` 后处理仍可能失败；普通 ASR 不受影响。
- `Rewrite selection` 当前是实验路径，已被 VoxType 禁用，不属于独立 ASR 迁移的必要步骤。

## 路径和依赖关系

`Shell Path` 必须指向一个同时包含以下文件的目录：

```text
unet.dll
UTDID.dll
```

首次使用时通常指向千问安装目录：

```text
C:\Program Files\QianwenIME
```

迁移后应改为用户自己的私有运行时目录。普通安装版推荐使用：

```text
%LOCALAPPDATA%\VoxType\qwen_free_runtime
```

当前开发电脑使用的是：

```text
D:\GITHUB_melody0709\VoxType\third_party\qwen_free_runtime
```

这个开发路径不能写死后分发给其他用户。每台电脑都必须保存自己的实际路径。

运行链路如下：

```text
本机 UTDID
  + unet.dll 生成签名和加密参数
  + Windows 网络连接
  → Qwen Free ASR 服务
```

`UTDID.dll` 主要用于读取千问已经初始化的本机 UTDID；VoxType 迁移脚本会把该值使用当前 Windows 用户的 DPAPI 加密保存到配置中。

## 首次初始化

1. 安装千问输入法。
2. 启动千问输入法，并实际完成一次语音输入，让它初始化本机 UTDID。
3. 至少启动一次 VoxType，让它创建本机配置文件。
4. 进入 `Settings → Cloud ASR`。
5. 选择 `Qwen IME (Free)`。
6. 将 `Shell Path` 指向千问安装目录，例如：

   ```text
   C:\Program Files\QianwenIME
   ```

7. 暂时关闭 `Polish (auto)`，避免尚未打通的 LLM 后处理影响 ASR 验证。
8. 点击 `Test Connection`，确认至少显示：

   ```text
   UTDID OK
   ASR WebSocket handshake OK
   ```

9. 使用真实麦克风完成一次语音识别，确认识别文本能够正常上屏。

如果显示 `UTDID unavailable`，先确认千问输入法确实已经启动和使用过一次，再重新测试。

## 迁移到私有目录

迁移前请完全退出 VoxType、千问输入法及其后台进程。

推荐运行仓库中的迁移脚本，而不是只手工复制 DLL：

[migrate_qwen_free_runtime.ps1](../reverse/tools/powershell/migrate_qwen_free_runtime.ps1)

普通安装版示例：

```powershell
powershell -ExecutionPolicy Bypass -File `
  .\reverse\tools\powershell\migrate_qwen_free_runtime.ps1 `
  -InstallDir "C:\Program Files\QianwenIME" `
  -RuntimeDir "$env:LOCALAPPDATA\VoxType\qwen_free_runtime" `
  -BackupDir "$env:LOCALAPPDATA\VoxType\migration-backup"
```

如果使用 Portable 版，应额外通过 `-ConfigPath` 指向 Portable 目录中的 `config.json`。

脚本会：

- 从千问安装目录复制 `unet.dll` 和 `UTDID.dll`；
- 读取本机 UTDID；
- 使用当前 Windows 用户的 DPAPI 加密保存 UTDID；
- 将 `qwen_free_shell_path` 指向私有运行时目录；
- 创建配置备份，但不会输出真实 UTDID。

迁移后重新启动 VoxType，并在设置中确认 `Shell Path` 已指向包含以下文件的目录：

```text
unet.dll
UTDID.dll
```

仅手工复制两个 DLL 不等于迁移完成，因为还需要把本机 UTDID安全地保存到本机配置中。

## 卸载前验证

1. 确认 `Shell Path` 已经指向私有运行时目录，然后保存设置。
2. 完全退出 VoxType 和千问相关进程。
3. 临时将原目录改名，例如：

   ```text
   C:\Program Files\QianwenIME
   →
   C:\Program Files\QianwenIME.disabled
   ```

   修改 `Program Files` 下的目录通常需要管理员权限。

4. 重启 VoxType，确认设置中仍显示私有运行时路径。
5. 保持 `Polish (auto)` 关闭，点击 `Test Connection`。
6. 使用真实麦克风至少完成两次 Qwen Free ASR。
7. 完全退出并再次启动 VoxType，再重复一次 ASR，确认不是旧进程仍在使用原 DLL。
8. 全部通过后，退出 VoxType，并把原千问目录恢复为原名。Windows 卸载程序可能需要原安装路径，不能在目录仍被改名时直接卸载。
9. 打开 `Windows 设置 → 应用 → 已安装的应用`，卸载千问输入法。
10. 重启 VoxType，再测试一次 Qwen Free ASR。

如果隔离原目录后失败，应立即恢复原目录名称，暂缓卸载，并检查 `Shell Path`、UTDID 和 DLL 版本。

如果卸载后失败，可重新安装同版本千问输入法，恢复原安装目录，再重新执行迁移和验证。

## 每台电脑如何分发

分发时可以提供相同版本的两个 DLL，但每台电脑必须单独完成：

```text
安装并使用一次千问
→ 读取本机 UTDID
→ 生成本机 DPAPI 配置
→ 指向本机私有运行时目录
→ 重启并测试
```

不要分发：

- 你的 `%LOCALAPPDATA%\VoxType\config.json`；
- 你的 `qwen_free_utdid_override`；
- 写死的本机绝对路径。

当前 `unet.dll` 还受 VoxType 的 SHA-256 版本白名单保护。已验证版本为：

```text
eb39951e6bcccfcec9b3576a5014ee35a7db9b3f1f35596a795e12f96f1ef5fa
```

## VoxType 软件升级

普通安装版的配置和推荐私有运行时都位于 `%LOCALAPPDATA%\VoxType`，正常升级 VoxType 时不应删除这些用户数据。

升级后应检查：

1. `Shell Path` 是否仍指向原私有运行时目录；
2. `Test Connection` 是否显示 UTDID 和 ASR 握手成功；
3. 真实麦克风 ASR 是否正常；
4. 如果显示 `a different unet.dll is already active`，完全退出并重启 VoxType 后再测试。

不要把私有运行时放进安装程序可能覆盖或删除的临时目录。Portable 版升级时，也不要直接删除包含配置和私有 DLL 的旧目录，除非已经完成备份和迁移。

## 千问升级和 DLL 替换

当前已验证的 DLL 能正常工作时，不需要因为千问输入法发布新版本就立即替换。

确实需要测试新版时：

1. 完全退出 VoxType 和千问相关进程；
2. 新建版本化目录，不要覆盖当前可用目录，例如：

   ```text
   qwen_free_runtime\known-good
   qwen_free_runtime\candidate-2026xxxx
   ```

3. 从新版千问安装目录复制新的 `unet.dll` 和 `UTDID.dll` 到候选目录；
4. 计算并记录两个文件的 SHA-256；
5. 检查 `unet.dll` 的哈希是否被当前 VoxType 支持；
6. 如果哈希变化，必须先完成兼容研究、探针验证并更新 VoxType 白名单，不能直接绕过校验；
7. 把 `Shell Path` 临时指向候选目录，完全重启 VoxType；
8. 依次测试 UTDID、ASR 握手和真实录音；
9. 全部通过后才将候选版本设为正式版本；
10. 保留旧版目录，以便失败时立即切回并重启 VoxType。

不要在 VoxType 正在运行时替换 `unet.dll`。DLL 一旦加载，本次进程会继续使用已加载版本，路径切换必须通过完整重启生效。

## Windows 账户或系统变化

UTDID override 使用当前 Windows 用户的 DPAPI 加密。以下情况可能需要重新初始化和迁移：

- 换用另一个 Windows 用户；
- 重装 Windows；
- 复制配置到另一台电脑；
- 用户配置或 DPAPI 密钥损坏。

不要把旧电脑的 `config.json` 当作新电脑的通用配置。新电脑应安装并使用一次千问输入法，然后生成自己的 UTDID 配置。

## 常见问题

| 提示或现象 | 处理方法 |
|---|---|
| `UTDID unavailable` | 启动并使用一次千问输入法，然后重新执行迁移 |
| `UTDID.dll cache not initialized` | 千问尚未生成有效 UTDID，不能只复制 DLL |
| `unet.dll version is not supported` | 恢复已验证旧版 DLL；新版需要兼容验证和白名单更新 |
| `a different unet.dll is already active` | 完全退出并重启 VoxType |
| `ASR WebSocket handshake` 失败 | 检查网络、UTDID、Shell Path 和 DLL 版本 |
| `LLM response malformed JSON` | 关闭 `Polish (auto)`；普通 ASR 可以继续使用 |

## 当前功能边界

- Qwen Free ASR：已验证可用。
- `Polish (auto)`：调用 `VoiceInputWrite`，当前仍可能出现 `LLM response malformed JSON`。
- `Rewrite selection`：调用 `VoiceInputRewrite`，当前禁用，尚未作为正式功能发布。

## 研究资料

面向开发和取证的详细记录位于：

- [Qwen Free 私有运行时迁移记录](../reverse/docs/QWEN_FREE_PRIVATE_RUNTIME_20260810.md)
- `reverse/work/20260810-qwen-runtime-migration/`
