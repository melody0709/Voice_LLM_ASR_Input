# 火山引擎 ASR (volcengine\_asr.h) 优化计划

基于 [volcengine\_asr.h](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/volcengine_asr.h) 与[官方大模型流式语音识别 API 文档](https://www.volcengine.com/docs/6561/1354869)的对比分析。

## 已完成

- [x] **修正模式命名**：ASR Mode 下拉框标签改为官方名字 `bigmodel_nostream` / `bigmodel_async` / `bigmodel`，排序和三处 idx→mode 映射全部同步更新（settings.cpp）
- [x] **修复 language 字段限制**：仅在 `bigmodel_nostream` 模式下发送 `language` 字段（volcengine\_asr.h）；Settings UI 中根据模式动态启用/禁用语言下拉框（settings.cpp）
- [x] **修复 SSL 证书验证**：移除 `OpenSession` 和 `TestConnection` 中的 `SECURITY_FLAG_IGNORE_*` 标志，恢复默认证书验证（volcengine\_asr.h）
- [x] **移除未使用的 BigASR 1.0 映射条目**：从 `kVolcResources[]` 中移除 `volc.bigasr.sauc.duration` 和 `volc.bigasr.sauc.concurrent`（globals.h）
- [x] **优化 Cloud ASR UI 布局**：删除分隔线标签行；Test Connection 移到 Provider 右侧；行号上移一行；提示文本改为单行移至 tab 底部

## P1：增加官方支持的关键参数 —— 已全部完成

### 参数分类

经过研究，6 个参数分为三类：

**A. 适合独立 UI 控件（用户可理解、经常调整）：**

| 参数                 | 类型   | 默认值   | 说明                                   | UI 控件    |
| ------------------ | ---- | ----- | ------------------------------------ | -------- |
| `enable_nonstream` | bool | false | 二遍识别（流式+非流式），仅 `bigmodel_async` 支持开启 | Checkbox |
| `enable_ddc`       | bool | false | 语义顺滑（去停顿词/语气词）                       | Checkbox |
| `end_window_size`  | int  | 800   | 强制判停时间（ms），静音超过此值直接判停                | Edit（数字） |

**B. 适合 JSON 输入框（高级用户才需要，和 LLM Extra Params 风格一致）：**

| 参数                         | 说明        | 理由                       |
| -------------------------- | --------- | ------------------------ |
| `corpus.boosting_table_id` | 热词词表 ID   | 需要去火山控制台获取，普通用户不知道       |
| `corpus.context`           | 热词直传 JSON | 本身就是 JSON，放 Edit 里容易写错格式 |
| `force_to_speech_time`     | 强制语音时间    | 极少数场景需要，大多数用户不理解         |
| 其他未来新增参数                   | —         | 不需要等 UI 更新就能使用           |

**方案**：A 类用独立控件，B 类用 Extra Params JSON 输入框（和 LLM Extra Params 风格一致）。

### 空间计算

窗口 850×680，客户区约 850×650。Tab 内容区到 y≈568。

```
RowInputY(row) = 76 + row × 52

Row 0: y=76   Provider + Test Connection
Row 1: y=128  API Key + Show
Row 2: y=180  ASR Mode
Row 3: y=232  Model Version
Row 4: y=284  Language
Row 5: y=336  ← 可用
Row 6: y=388  ← 可用
Row 7: y=440  ← 可用
Row 8: y=492  ← 可用（492+32=524 < 568）
Row 9: y=544  ← 不可用（544+32=576 > 568）
```

**可用空间**：Row 5-8，共 4 行。需要放 5 项（分隔线 + 2 Checkbox + 1 数字框 + 1 JSON 输入框）。

### UI 布局方案（已实现）

两个 Checkbox 放同一行，节省 1 行：

```
Row 0: Provider                [dropdown ▼] [Test Connection]
Row 1: API Key (X-Api-Key)     [________________] [Show]
Row 2: ASR Mode                [bigmodel_nostream ▼]
Row 3: Model Version           [Seed-ASR 2.0 (duration) ▼]
Row 4: Language (nostream)     [Auto ▼]
Row 5: [☑] enable_nonstream    [☐] enable_ddc
Row 6: end_window_size         [800___]
Row 7: Extra Params            [Edit Params]
  y500: Cloud ASR sends audio to remote servers. Keys are encrypted with DPAPI locally.
```

实际实现中未加 "Advanced" 分隔线，直接用 Checkbox 节省空间。

### 实施记录

#### 1. globals.h — 增加控件 ID 和 Config 字段 ✅

```cpp
constexpr int IDC_VOLC_ENABLE_NONSTREAM = 2050;
constexpr int IDC_VOLC_END_WINDOW_SIZE = 2051;
constexpr int IDC_VOLC_ENABLE_DDC = 2052;
constexpr int IDC_VOLC_EXTRA_PARAMS = 2053;
// IDC_VOLC_EXTRA_RESET 未使用（对话框内用 IDOK/IDCANCEL）

// Config 结构体新增字段
bool volcEnableNonstream = false;
int volcEndWindowSize = 800;
bool volcEnableDdc = false;
std::wstring volcExtraParams;
```

**状态**：已合入 [globals.h](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/globals.h)

#### 2. volcengine\_asr.h — VolcConfig 增加字段 ✅

```cpp
struct VolcConfig {
    // ... 现有字段 ...
    bool enableNonstream = false;
    int endWindowSize = 800;
    bool enableDdc = false;
    std::wstring extraParams;  // JSON string, merged last (overrides above)
};
```

**状态**：已合入 [volcengine\_asr.h](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/volcengine_asr.h)

#### 3. volcengine\_asr.h — OpenSession 初始化 JSON 增加参数 ✅

在 `request` 对象中追加（在 `result_type` 之后、`}` 之前）：

```cpp
if (cfg.enableNonstream && cfg.mode == L"bigmodel_async") {
    requestJson += ",\"enable_nonstream\":true";
}
if (cfg.endWindowSize > 0 && cfg.endWindowSize != 800) {
    requestJson += ",\"end_window_size\":" + std::to_string(cfg.endWindowSize);
}
requestJson += ",\"enable_ddc\":" + std::string(cfg.enableDdc ? "true" : "false");
```

Extra Params 合并逻辑：在构建完基础 JSON 后，解析 `cfg.extraParams`，将其中的 key-value 对合并到 request 对象中（Extra Params 中的 key 覆盖基础设置的同名 key）。

**状态**：已合入 [volcengine\_asr.h](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/volcengine_asr.h)

#### 4. engine.cpp — LoadConfig / SaveConfig 增加字段 ✅

```cpp
// LoadConfig
g_config.volcEnableNonstream = ExtractJsonBool(json, "volc_enable_nonstream", false);
g_config.volcEndWindowSize = _wtoi(Utf8ToWide(ExtractJsonString(json, "volc_end_window_size", "800")).c_str());
if (g_config.volcEndWindowSize <= 0) g_config.volcEndWindowSize = 800;
g_config.volcEnableDdc = ExtractJsonBool(json, "volc_enable_ddc", false);
g_config.volcExtraParams = Utf8ToWide(ExtractJsonString(json, "volc_extra_params", ""));

// SaveConfig
<< "  \"volc_enable_nonstream\": " << (g_config.volcEnableNonstream ? "1" : "0") << ",\n"
<< "  \"volc_end_window_size\": " << g_config.volcEndWindowSize << ",\n"
<< "  \"volc_enable_ddc\": " << (g_config.volcEnableDdc ? "1" : "0") << ",\n"
<< "  \"volc_extra_params\": \"" << EscapeJson(g_config.volcExtraParams) << "\"\n"
```

**注意**：原实现中 `LoadConfig` 使用 `ExtractJsonString(..., "0") == "1"` 来读取 number 类型的字段，但 `ExtractJsonString` 只支持 string 类型（带引号），导致这两个字段永远加载失败、回退到默认值。已修复：

- `ExtractJsonBool` 增加对 `"1"` / `"0"` 的支持
- `LoadConfig` 改用 `ExtractJsonBool` 读取这两个字段

**状态**：已合入 [engine.cpp](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/engine.cpp)

#### 5. settings.cpp — 创建 UI 控件 ✅

在 Language 下拉框（Row 4）之后创建：

```cpp
// Row 5: enable_nonstream + enable_ddc 两个 Checkbox 同行
HWND volcNonstream = CreateWindowW(L"BUTTON", L"enable_nonstream",
    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
    UiStyle::ContentLeft, UiStyle::RowInputY(5) + 6, 220, UiStyle::CheckH,
    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_NONSTREAM)),
    g_instance, nullptr);

HWND volcDdc = CreateWindowW(L"BUTTON", L"enable_ddc",
    WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
    340, UiStyle::RowInputY(5) + 6, 200, UiStyle::CheckH,
    hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_VOLC_ENABLE_DDC)),
    g_instance, nullptr);

// Row 6: end_window_size
CreateLabel(..., L"end_window_size", ...);
CreateWindowExW(..., ES_NUMBER, ..., IDC_VOLC_END_WINDOW_SIZE, ...);

// Row 7: Extra Params
CreateLabel(..., L"Extra Params", ...);
CreateButton(..., IDC_VOLC_EXTRA_PARAMS, ..., L"Edit Params");
```

**状态**：已合入 [settings.cpp](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/settings.cpp)

#### 6. settings.cpp — 加载/保存/事件处理 ✅

- **加载** (`LoadSettingsControls`)：
  ```cpp
  Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM),
      g_config.volcEnableNonstream ? BST_CHECKED : BST_UNCHECKED);
  EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM),
      g_config.volcMode == L"bigmodel_async");
  Button_SetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_DDC),
      g_config.volcEnableDdc ? BST_CHECKED : BST_UNCHECKED);
  SetWindowTextW(GetDlgItem(hwnd, IDC_VOLC_END_WINDOW_SIZE), std::to_wstring(g_config.volcEndWindowSize).c_str());
  ```
- **保存** (`SaveSettingsControls`)：
  ```cpp
  g_config.volcEnableNonstream = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM)) == BST_CHECKED;
  g_config.volcEnableDdc = Button_GetCheck(GetDlgItem(hwnd, IDC_VOLC_ENABLE_DDC)) == BST_CHECKED;
  g_config.volcEndWindowSize = _wtoi(ew);  // 来自 IDC_VOLC_END_WINDOW_SIZE
  if (g_config.volcEndWindowSize <= 0) g_config.volcEndWindowSize = 800;
  ```
- **事件** (`IDC_VOLC_MODE` CBN\_SELCHANGE)：
  ```cpp
  EnableWindow(GetDlgItem(hwnd, IDC_VOLC_ENABLE_NONSTREAM), modeIdx == 1);  // 仅 bigmodel_async 启用
  EnableWindow(langCombo, modeIdx == 0);  // 仅 bigmodel_nostream 启用 language
  ```
- **Extra Params 对话框** (`IDC_VOLC_EXTRA_PARAMS`)：
  弹出 `VoxTypeVolcExtraDlg` 窗口，支持多行 JSON 编辑，提供 Hotwords / Context 预设模板，OK 后保存到 `g_config.volcExtraParams`。

**状态**：已合入 [settings.cpp](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/settings.cpp)

#### 7. main.cpp — 构建 VolcConfig 时读取新字段 ✅

```cpp
volc_asr::VolcConfig vcfg;
vcfg.apiKey = config.volcApiKey;
vcfg.resourceId = config.volcResourceId;
vcfg.mode = config.volcMode;
if (!config.volcLanguage.empty()) vcfg.language = config.volcLanguage;
vcfg.enableNonstream = config.volcEnableNonstream;
vcfg.endWindowSize = config.volcEndWindowSize;
vcfg.enableDdc = config.volcEnableDdc;
vcfg.extraParams = config.volcExtraParams;
```

**状态**：已合入 [main.cpp](file:///d:/#GITHUB_melody0709/Voice_LLM_ASR_Input/src/main.cpp)

## P2：小修复与优化

- [ ] 修正 sequence 起始值：`sess.sequence = 2` → `sess.sequence = 1`（volcengine\_asr.h）
- [ ] 优化音频分包大小：按官方建议 200ms（6400 字节）分包
- [ ] 移除文档未提及的额外头：`X-Api-Request-Id` 和 `X-Api-Sequence: -1`（volcengine\_asr.h）

## P3：长期优化

- [ ] JSON 解析升级：当前 `ExtractJsonStr` 使用手工字符串查找，如需解析 utterances/additions 等复杂结构，建议引入轻量 JSON 解析库
- [ ] bigmodel\_async 模式下默认开启 `enable_nonstream` 二遍识别，兼顾实时性和准确率
- [ ] 记录 `X-Tt-Logid` 响应头到调试日志，方便排错

## 参考文档

- [大模型流式语音识别 API](https://www.volcengine.com/docs/6561/1354869) — 官方 WebSocket 协议文档
- [控制台使用 FAQ](https://www.volcengine.com/docs/6561/196768) — 鉴权参数获取方式

