# VoxType 构建、MSI / Portable 与开机自启动重构方案

状态：已实施；发布前升级生命周期实机验证仍需在隔离 Windows VM 执行
日期：2026-07-27
本轮范围：方案已落实到产品代码、构建脚本、Portable/MSI 发布资产和验证脚本；本文保留为升级契约与验收依据。

## 1. 目标与已确定的发布策略

将现有“手工 cl 编译到 build/ + 另行用 package.py 生成 release/”的方式，重构为一个可追溯的 Windows 发布流水线：

1. CMake/Ninja 是唯一的编译权威；build.bat 只负责准备 MSVC 环境、调用 CMake、安装运行载荷和分发打包。
2. build/run/x64-release/ 是唯一允许直接运行的开发运行目录。它由 cmake --install 生成，不从编译目录手工复制文件。
3. Portable .7z 与 MSI 都只能从同一份“规范运行载荷”生成，并在生成后重新解包、逐文件校验。
4. 首个 MSI 起就建立永久升级契约，使后续公开版本能够安全 Major Upgrade，而不是只能覆盖安装一次。
5. 开机自启动采用当前用户的 HKCU Run 注册表项，默认关闭，不要求管理员权限，MSI 与 Portable 共用同一套实现。
6. 安装版的配置、下载模型和日志必须移出安装目录；Portable 保持数据随解压目录携带。

本方案建议将首条 MSI 产品线固定为：

| 项目 | 决策 |
| --- | --- |
| 架构 | x64 |
| 安装上下文 | per-machine |
| 首次默认目录 | ProgramFiles64Folder\\VoxType |
| 用户可写数据 | %LOCALAPPDATA%\\VoxType |
| Portable 格式 | 7z，解压根目录带 portable.flag |
| 启动项 | HKCU\\Software\\Microsoft\\Windows\\CurrentVersion\\Run 的 VoxType 值 |

这是对参考项目 stable|x64|perMachine 设计的有意复用。若要改为 per-user、ARM64 或另一个发布通道，必须在第一个公开 MSI 前整体改为另一条 ProductLine，并生成另一套永久 MSI 身份；不能在首发后混用。

## 2. 研究结论与当前阻塞点

### 2.1 当前 VoxType 的状态

- build.bat 直接调用 rc / cl，在 build/ 同时放入 obj、res、exe 和 DLL。
- CMakeLists.txt 与 build.bat 重复维护源文件、库和延迟加载选项，但当前仅供 IDE 索引使用。
- package.py 从 build/ 临时拼装 7z，并输出到 release/；它会枚举 DLL，未以固定清单或运行载荷清单为输入。
- 没有 MSI 作者文件、稳定产品身份、升级规则、安装后载荷校验或同版本资产保护。
- src/audio/engine.cpp 的 ConfigPath()、默认 ASR / 标点模型目录和 VAD 路径均依赖 AppRootDir()；src/app/main.cpp 的调试日志也写入 AppRootDir()。
- AppRootDir() 还包含“路径以 \\build 结尾时退到仓库根目录”的开发特例。它使当前开发可用，但不能成为安装版的数据目录规则。

因此，若直接把现有 EXE 安装到 Program Files：

- 保存设置会尝试写入受保护安装目录；
- download_models.ps1 会尝试把数百 MB 到数 GB 的模型写进安装目录；
- 本地调试日志也会写入安装目录；
- 升级和卸载无法区分 MSI 自己拥有的静态文件与用户数据。

数据路径重构是 MSI 的前置条件，不能后置。

### 2.2 参考项目中应复用的原则

已审阅的参考实现包括：

- D:\GITHUB_melody0709\zencrop_ocr_pxipin\build.bat
- D:\GITHUB_melody0709\zencrop_ocr_pxipin\cmake\ZenCropRuntime.cmake
- D:\GITHUB_melody0709\zencrop_ocr_pxipin\scripts\package_zencrop.ps1
- D:\GITHUB_melody0709\zencrop_ocr_pxipin\packaging\windows\UPGRADE_CONTRACT.md
- D:\GITHUB_melody0709\zencrop_ocr_pxipin\src\core\StartupRegistration.cpp

需要吸收的不是其全部复杂度，而是以下不可省略的边界：

- 编译树与唯一可运行树分离，运行树由 CMake install 产生；
- Portable 与 MSI 从同一 canonical payload 出发；
- 打包前后都校验布局，禁止把未知构建垃圾带入发布包；
- Portable 写入空的 portable.flag，并解压后与规范载荷逐文件比较；
- MSI 的安装包数据库和 msiexec /a 管理安装解包结果都要与规范载荷比较；
- 第一个 MSI 一次性冻结 UpgradeCode、ProductCode UUID 命名空间和 Component UUID 命名空间；
- 公开的 major.minor.patch 版本一律走 Major Upgrade，禁止同版本覆盖；
- MSI 绝不拥有或递归删除 LocalAppData 用户数据；
- 启动项以注册表为唯一真相，不在 JSON 中复制一份容易漂移的布尔状态。

## 3. 目标 build/ 布局与产物命名

~~~text
build/
├─ cmake/
│  └─ x64-release/                 # CMake/Ninja cache、obj、install manifest、临时 package staging
├─ run/
│  └─ x64-release/                 # 唯一可直接运行的规范运行载荷
│     ├─ VoxType.exe
│     ├─ *.dll
│     ├─ models/                    # 仅随程序发布的 VAD 资产
│     ├─ aria2c.exe
│     ├─ download_models.ps1
│     └─ runtime-manifest.json
├─ packages/
│  ├─ VoxType-v0.9.7-win-x64-portable-unsigned.7z
│  ├─ VoxType-v0.9.7-win-x64-unsigned.msi
│  └─ *.sha256
├─ artifacts/
│  ├─ tests/
│  └─ package-verification/
├─ logs/
└─ README.txt                      # 由 build.bat 写出的布局说明
~~~

正式签名发布去掉 -unsigned 后缀：

~~~text
VoxType-v<major.minor.patch>-win-x64-portable.7z
VoxType-v<major.minor.patch>-win-x64.msi
~~~

Portable 压缩包内只有一个顶级目录：

~~~text
VoxType-v<version>-win-x64-portable[-unsigned]/
└─ portable.flag                   # 空文件；仅用于选择便携数据路径
~~~

build/ 继续完全 gitignored；release/ 不再作为发布输出。package.py 将被移除，或保留为只提示新命令的兼容入口，不能再产生第二条打包路径。

## 4. 运行载荷与用户数据边界

### 4.1 三类路径

新增集中式路径 API，替换以 AppRootDir() 推导所有路径的做法：

| 路径 | 安装版 | Portable |
| --- | --- | --- |
| ExecutableDir / RuntimeAssetDir | MSI 安装目录 | EXE 所在目录 |
| MutableDataDir | %LOCALAPPDATA%\\VoxType | EXE 所在目录 |
| ConfigPath | MutableDataDir\\config.json | EXE 旁 config.json |
| DownloadedModelRoot | MutableDataDir\\models | EXE 旁 models |
| LogDir | MutableDataDir\\log | EXE 旁 log |
| VAD 资产 | RuntimeAssetDir\\models | EXE 旁 models |

这保留 Portable 的现有直觉：配置和模型都跟目录走；而安装版不会向 Program Files 写入任何可变内容。

### 4.2 必须迁移的现有调用点

- src/audio/engine.cpp
  - ConfigPath() 改用 MutableDataDir；
  - DefaultModelDir()、AnyModelDirExists()、标点模型路径改用 DownloadedModelRoot；
  - Silero / FireRed VAD 路径保留在 RuntimeAssetDir\\models；
  - RunModelDownloader() 把目标模型目录和 aria2c 路径显式传给脚本；
  - 删除“目录名是 build 就退回父目录”的数据路径特殊分支。
- src/app/main.cpp
  - 调试日志改写入 LogDir。
- download_models.ps1
  - 增加 Destination 参数，默认保持脚本旁 models 的兼容行为；
  - 增加 Aria2Path 参数，以便安装版仍使用运行载荷中的 aria2c.exe；
  - 不从 MSI、安装器 custom action 或网络安装步骤下载模型。

### 4.3 配置 / 模型迁移策略

1. 非便携运行时，若 LocalAppData 中尚无 config.json，且 EXE 旁有旧 config.json，则仅做一次保守导入；成功后保留源文件，不覆盖已有目标文件。
2. Portable 模式始终优先使用 EXE 旁的数据，以兼容已有解压版。
3. 不扫描磁盘寻找旧版本，不让 MSI custom action 迁移数据；从旧 Portable 切换到 MSI 的任意目录导入应由应用提供明确的导入入口或文档说明。
4. 配置中旧的 model_dir 若为空、失效，或明确指向当前安装目录的旧 models 路径，才回退到新的 DownloadedModelRoot。有效的用户自定义模型目录不得改写。
5. 提升 configVersion，并在 LoadConfig() 中做幂等迁移；SaveConfig() 只写新的数据根目录。

DPAPI 加密字段仍在同一 Windows 用户下读取，因此从旧配置导入到 LocalAppData 不会改变其加密边界。

## 5. 构建系统重构

### 5.1 CMake 成为唯一编译权威

新增 CMakePresets.json，至少提供 x64-release 预设，二进制目录为 build/cmake/x64-release。CMakeLists.txt 将完整承接现有 build.bat 的实际编译语义：

- 所有 C++ / RC 源文件仅在 CMake 中维护；
- C++17、UNICODE、_UNICODE、/O2、/EHsc、/MT、/utf-8；
- 现有 include、静态库和系统库；
- onnxruntime.dll、sherpa-onnx-cxx-api.dll、kaldi-native-fbank-core.dll 的 /DELAYLOAD；
- src/app/resources.rc、manifest 和版本资源；
- 新增源文件时只更新 CMake；不再同步第二份 cl 文件清单。

版本只从 src/app/resource.h 的 APP_VERSION_MAJOR / MINOR / PATCH 读取。APP_VERSION_BUILD 不进入 MSI ProductVersion；若载荷或安装器语义变更，必须提升公开三段版本，不能仅增加 BUILD 后重发同版本 MSI。

### 5.2 CMake install 生成规范运行载荷

新增 cmake/VoxTypeRuntime.cmake 和 runtime-manifest 生成模板，定义 Runtime install component：

- VoxType.exe；
- sherpa-onnx-cxx-api.dll、sherpa-onnx-c-api.dll、onnxruntime.dll、kaldi-native-fbank-core.dll；
- aria2c.exe；
- 两个内置 VAD 模型；
- download_models.ps1；
- README / 必要的云端 ASR 使用文档；
- runtime-manifest.json（相对路径、大小、SHA-256、产品版本）。

不安装 config.json、日志、已下载的 ASR / 标点模型、obj、pdb、res 或临时文件。安装刷新只能删除明确列出的旧运行时文件，不能递归删除未知内容。

### 5.3 build.bat 的职责

目标命令：

~~~text
build.bat
build.bat --rebuild
build.bat --clean
build.bat --package
build.bat --package-portable
build.bat --package-msi
build.bat --require-signing --package
~~~

流程为：

1. 使用 vswhere 定位 VS 2022，进入 x64 开发环境，并定位 VS 自带 CMake / Ninja；
2. 仅终止绝对路径等于 build/run/x64-release/VoxType.exe 的进程，绝不按进程名终止系统中其他 VoxType；
3. CMake configure、build、cmake --install 到 build/run/x64-release；
4. 校验 build/ 允许的顶层目录与 runtime-manifest；
5. 仅在 package 参数存在时调用打包脚本。

--clean 默认清理 cmake、run、artifacts、logs，但保留 packages，避免无意删除已验证的版本资产；删除 packages 必须是明确、单独的本地开发操作。

## 6. Portable 与 MSI 打包实现

### 6.1 共同的规范载荷

新增 scripts/package_voxtype.ps1。它接收运行目录、CMake install manifest 和三段产品版本，并执行：

1. 校验输入路径都位于本仓库的 build/ 范围内；
2. 拒绝 reparse point、绝对路径、..、大小写碰撞和未知运行载荷文件；
3. 将 build/run/x64-release 复制到 build/cmake/x64-release/package-staging/canonical；
4. 对规范载荷建立排序的 SHA-256 清单；
5. 由该规范载荷分别生产 Portable 与 MSI，禁止任何分支回读编译目录或源码目录。

### 6.2 Portable

- 在 staging 副本中写入空 portable.flag；
- 使用 7-Zip 生成 solid LZMA2 .7z；
- 执行 7z t，解压到独立验证目录；
- 比较解压结果和规范载荷，唯一允许的差异是 portable.flag；
- 生成 SHA-256 sidecar；
- 已有同名版本资产时只验证，绝不覆盖。

### 6.3 MSI

新增以下提交到源码的作者文件：

~~~text
packaging/windows/VoxType.Installer.wixproj
packaging/windows/Package.wxs
packaging/windows/InstallerUi.wxs
packaging/windows/ProductIdentity.wxi
packaging/windows/UPGRADE_CONTRACT.md
scripts/generate_wix_runtime_fragment.ps1
scripts/generate_wix_product_instance.ps1
scripts/test_msi_lifecycle.ps1
~~~

WiX 项目使用 SDK 风格的 WixToolset.Sdk NuGet 包，避免依赖机器级 WiX 安装。打包脚本生成 RuntimeFiles.generated.wxs，MSI 中每个规范运行时文件只属于一个 Component。

MSI 构建后至少执行：

1. 检查 MSI 的 ProductCode、UpgradeCode、MajorUpgrade、Feature、Component、文件表和安装根目录约束；
2. 使用 msiexec /a 解出管理安装载荷；
3. 将解出的实际文件逐路径、大小和 SHA-256 与 canonical payload 比较；
4. 生成 SHA-256 sidecar，原子发布到 build/packages。

签名尚未配置时，工程产物必须显式使用 -unsigned 后缀。正式发布必须通过 --require-signing，缺证书或验签失败即失败，不允许把未签名包伪装成正式包。

## 7. MSI 升级契约（首个 MSI 前必须冻结）

### 7.1 永久身份

ProductIdentity.wxi 在首个公开 MSI 前一次性生成并提交：

| 常量 | 用途 |
| --- | --- |
| ProductLineKey | stable|x64|perMachine |
| UpgradeCode | 整条产品线永久不变 |
| ProductCode UUID v5 namespace | 以 ProductLineKey + major.minor.patch 派生 ProductCode |
| Component UUID v5 namespace | 以规范载荷的 slash 相对路径派生 Component GUID |

不得让 CI、构建脚本或后续 AI 改写这些 UUID。PackageCode 由 WiX 为每个实际 MSI 生成即可。

### 7.2 安装与升级规则

- Package Scope=perMachine，默认 ProgramFiles64Folder\\VoxType；
- WiX UI 使用标准安装目录选择页；
- 单独的稳定 MSI Component 把用户选定的 INSTALLFOLDER 写入 HKLM\\Software\\VoxType\\InstallFolder；
- AppSearch 必须在 RemoveExistingProducts 前恢复该目录，因此 N 到 N+1 升级不会退回默认目录；
- 使用 MajorUpgrade Schedule=afterInstallInitialize；
- AllowSameVersionUpgrades=false，降级显示明确错误；
- 每个公开 major.minor.patch 都是完整 Major Upgrade；
- 相同三段版本下，若 canonical payload、签名或安装器语义改变，打包必须失败并要求提升版本；
- Start Menu 快捷方式是独立 Component，目标为 [INSTALLFOLDER]VoxType.exe；
- MSI 只管理明确清单中的安装文件；不使用 wildcard、递归删除或“清空安装目录”；
- LocalAppData 数据、用户模型、配置和日志永远不由 MSI 拥有、移动或删除；
- 未来移动 / 删除某个运行时文件时，让旧 MSI 的已登记 Component 在 Major Upgrade 中清理它；只有已证实的历史孤儿文件才可加入带版本范围说明的精确 cleanup。

第一个 MSI 没有历史 MSI 可升级。对旧的手工 Portable 用户，迁移只由应用的数据路径逻辑或明确导入流程承担，安装器不猜测旧目录。

### 7.3 升级的验收矩阵

正式发布前，在隔离 Windows VM 执行以下显式生命周期测试：

| 场景 | 必须验证 |
| --- | --- |
| 首次安装 | 默认与自定义安装目录、开始菜单、运行载荷、无 Program Files 写入 |
| N-1 → N | 旧文件替换 / 删除、所选安装目录保留、LocalAppData 保留、启动项保持有效 |
| 最早支持版 → N | 同上，且不存在历史孤儿文件 |
| 已开启开机启动后升级 | HKCU Run 值仍指向稳定安装路径的 VoxType.exe |
| 降级 | 被拒绝，不破坏已安装版本 |
| 修复 / 卸载 | 仅 MSI 拥有的文件与快捷方式被移除；LocalAppData 保留 |
| 文件占用 / 回滚 | Windows Installer 事务正确回滚，不能留下不完整的 payload |
| 同版本重打包 | 只做哈希确认，不能覆盖已有发布资产 |

正常 build.bat 不隐式安装、升级或卸载 MSI。会改变机器状态的 lifecycle harness 必须要求独立 VM、提升权限和显式确认开关。

## 8. 开机自启动设计

### 8.1 模块边界

新增 src/core/startup_registration.h/.cpp，并将其加入 CMake。接口包含：

- QueryVoxTypeStartupRegistration()：读取当前用户 Run 值，返回读取错误、是否存在、命令行以及是否精确指向当前 EXE；
- SetVoxTypeStartupRegistration(bool enable)：写入、更新或删除本产品自己的值；
- BuildStartupRegistrationCommandLine()：总是对 EXE 绝对路径加引号；
- IsStartupRegistrationCommandLineValid()：执行 Run 键 260 字符限制检查。

键和值固定为：

~~~text
HKCU\Software\Microsoft\Windows\CurrentVersion\Run
VoxType = "<absolute path to VoxType.exe>"
~~~

实现使用动态扩容的 GetModuleFileNameW，不能假设 MAX_PATH。开启时，若已有值存在但仍指向旧的 Portable 目录，必须更新而不是仅因“值存在”而跳过；关闭时只删除 VoxType 这一值。

### 8.2 UI 与状态真相

- Settings 的 General 页面增加“登录 Windows 时启动 VoxType”复选框，使用新的 IDC 和 UiStyle 布局常量，不硬编码坐标；
- 每次打开 Settings 都查询注册表，而不是读取缓存；
- 保存时先执行注册表变更；失败则显示 Win32 错误、保留当前页面与复选状态，且不提交其余设置；
- 成功后才提交普通配置和 ASR reload；
- 默认关闭，MSI 安装器不自动开启启动项。

启动项是操作系统注册状态而不是 VoxType Config 字段。因此不在 Config / LoadConfig / SaveConfig 中存重复 bool；注册表是唯一真相。这是对“新增普通配置要同步三处”约定的有意例外，可避免 JSON 与系统实际注册状态漂移。

### 8.3 与安装 / 升级的关系

- MSI 升级保持 INSTALLFOLDER，因此已登记的 Run 命令在 N → N+1 后仍指向正确 EXE；
- Portable 被用户移动后，Settings 会识别旧路径不匹配；用户再次启用即可更新；
- MSI 不拥有、也不自动创建该 HKCU 值，避免 per-machine 安装器替不同 Windows 用户擅自启用登录启动；
- 用户在 Settings 关闭时始终会清理自己的值；
- MSI 卸载不能可靠代表所有曾使用该机器的 HKCU profile，因此不做跨用户注册表扫描或高权限批量清理。卸载时对当前用户的可选清理方式需要单独设计为显式用户上下文流程，不能以破坏升级事务的 custom action 偷做。

## 9. 实施顺序

1. 路径与数据基础
   - 实现 RuntimeAssetDir / MutableDataDir / portable.flag 检测；
   - 迁移 config、模型、标点、VAD、日志和下载脚本；
   - 验证安装目录只读时，云端与本地模型下载路径都可工作。
2. 单一 CMake 构建与运行时安装
   - 加 CMakePresets、Runtime install component、runtime manifest；
   - 将 build.bat 改为 CMake 编译入口；
   - 删除 build/ 混放运行文件的行为，并验证高 DPI Settings UI。
3. 规范 Portable 打包
   - 实现布局验证、规范载荷、7z 生成 / 解压校验 / SHA-256；
   - 迁移或停用 package.py。
4. MSI 作者与升级契约
   - 在首发前人工确认并冻结永久身份；
   - 生成 WiX runtime fragment，构建 MSI，并实现管理安装载荷比较；
   - 文档化版本、升级、安装根目录和签名规则。
5. 开机自启动
   - 实现注册表模块、Settings 控件、错误处理与单元 / 手工验证；
   - 覆盖安装版、Portable、路径含空格、路径过长、启用 / 更新 / 禁用。
6. 发布验证与文档
   - 更新 README、ARCHITECTURE、CHANGELOG 与构建说明；
   - 在隔离 VM 执行升级矩阵，保留历史 MSI 和校验和。

## 10. 验收标准

- 只需 build.bat 即可得到 build/run/x64-release/VoxType.exe；编译目录自身不是支持的运行目录。
- build.bat --package 在 build/packages 同时留下已验证的 .7z、.msi 与 SHA-256。
- 两种包解出的所有规范运行时文件与 canonical payload 完全一致。
- MSI 升级不会丢失用户的配置、DPAPI 凭据、下载模型、日志或用户选择的安装目录。
- Program Files 安装后，保存设置、下载模型、写调试日志均不因权限失败。
- 同版本资产不会被覆盖；版本来源只有 resource.h 的三段公开版本。
- 开机自启动可在 Settings 正确启用、禁用和修复移动后的 Portable 路径，且不会要求管理员权限。
- 不引入安装时模型下载、全局按名称杀进程、递归删除未知文件、随机重生 MSI 身份或同版本升级。

## 11. 实施前仍需由发布负责人确认的外部条件

- 正式签名证书、时间戳服务和 CI 中的安全注入方式；
- 首个公开 MSI 的 UpgradeCode / UUID namespaces 的一次性生成与保管；
- 首个支持升级的基线版本，以及需要在 VM 保存多久的历史 MSI；
- 是否为 MSI UI 增加“安装完成后启动”选项（不影响、也不替代默认关闭的登录启动选项）。
