# Plan: 多供应商预设 + Prompt Tab 拆分

## 目标

1. LLM tab 支持多供应商切换（预设 + 自定义），选择后自动填充 URL/Model
2. 每个供应商独立保存 API Key
3. System Prompt 拆到独立 "Prompt" tab，释放 LLM tab 空间
4. 统一关闭思考模式：预设自动注入 `extraParams`，用户可在 Extra Params 文本框自定义

## 调研结论：各供应商关闭思考模式方式

| 供应商 | 模型 | 关闭参数 |
|--------|------|---------|
| DeepSeek | deepseek-v4-flash | `"thinking":{"type":"disabled"}` |
| OpenRouter | 任意模型 | `"reasoning":{"effort":"none"}` |
| SiliconFlow | Qwen3.6 | `"chat_template_kwargs":{"enable_thinking":false}` |
| SiliconFlow | Qwen3 非推理模型 | 不需要，模型本身不推理 |
| SiliconFlow | DeepSeek-R1 | `thinking_budget` 设小或 0 |

同一供应商不同模型，关闭方式不同。用 `extraParams` 文本框让用户手动补充。

---

## UI 设计：4 个 Tab

### 现有 Tab（不变）

- Tab 1: Recognition
- Tab 2: Shortcut

### Tab 3: LLM（连接配置）

```
┌──────────────────────────────────────────────┐
│ [Recognition] [Shortcut] [LLM] [Prompt]      │
│                                               │
│ y=76   Provider       [Dropdown ▼]            │
│                                               │
│ y=140  API Base URL   [____________________]  │
│ y=186  API Key        [______________] [Show] │
│ y=232  Model          [____________________]  │
│                                               │
│ y=280  [Test Connection]   ☑ Debug log        │
│                                               │
│ y=330  Extra Params   [____________________]  │
│                                               │
│ (余量 ~116px)                                  │
│                                               │
│ Status / Save / Close                         │
└──────────────────────────────────────────────┘
```

**控件清单：**

| 控件 | ID | 位置 | 说明 |
|------|----|------|------|
| Provider label | - | (54, 82) | "Provider" |
| Provider dropdown | `IDC_LLM_PROVIDER` | (200, 76, 580, 32) | `CBS_DROPDOWNLIST`，预设 + 已保存的自定义供应商 |
| API Base URL label | - | (54, 146) | "API Base URL" |
| API Base URL edit | `IDC_LLM_ENDPOINT` | (200, 140, 580, 32) | |
| API Key label | - | (54, 192) | "API Key" |
| API Key edit | `IDC_LLM_KEY` | (200, 186, 480, 32) | `ES_PASSWORD` |
| Show/Hide button | `IDC_LLM_SHOW_KEY` | (694, 185, 92, 34) | |
| Model label | - | (54, 238) | "Model" |
| Model edit | `IDC_LLM_MODEL` | (200, 232, 580, 32) | |
| Test Connection button | `IDC_LLM_TEST` | (200, 280, 140, 36) | |
| Debug checkbox | `IDC_LLM_DEBUG` | (360, 286, 220, 26) | "Log refine before/after" |
| Extra Params label | - | (54, 336) | "Extra Params" |
| Extra Params edit | `IDC_LLM_EXTRA` | (200, 330, 580, 32) | 单行 edit，用户填 JSON 片段 |

**Provider dropdown 内容：**
- `DeepSeek`（预设）
- `OpenRouter`（预设）
- `SiliconFlow`（预设）
- `Custom`（用户自己填所有字段）
- 已保存的自定义供应商名称（如有）

**切换 Provider 时的行为：**
- 选择预设 → 自动填充 URL、Model、Extra Params
- 恢复该供应商的 API Key（从 `llm_providers` 读取）
- 选择 Custom → 清空所有字段

### Tab 4: Prompt（提示词）

```
┌──────────────────────────────────────────────┐
│ [Recognition] [Shortcut] [LLM] [Prompt]      │
│                                               │
│ y=76   [Basic Fix]   [Deep Fix]               │
│                                               │
│ y=120  System Prompt                          │
│        ┌────────────────────────────────────┐ │
│        │                                    │ │
│        │                                    │ │
│        │  multiline edit (h=300+)           │ │
│        │                                    │ │
│        │                                    │ │
│        │                                    │ │
│        └────────────────────────────────────┘ │
│                                               │
│ Status / Save / Close                         │
└──────────────────────────────────────────────┘
```

**控件清单：**

| 控件 | ID | 位置 | 说明 |
|------|----|------|------|
| Basic Fix button | `IDC_LLM_PRESET1` | (200, 76, 120, 32) | 填入保守 prompt |
| Deep Fix button | `IDC_LLM_PRESET2` | (340, 76, 120, 32) | 填入激进 prompt |
| System Prompt label | - | (54, 126) | "System Prompt" |
| System Prompt edit | `IDC_LLM_PROMPT` | (200, 120, 580, 340) | 多行 edit，h=340 |

---

## 预设供应商定义

```cpp
// llm_refine.h
struct ProviderPreset {
    const wchar_t* name;        // 显示名
    const wchar_t* url;         // 默认 API Base URL
    const wchar_t* defaultModel;// 默认模型
    const wchar_t* extraParams; // 默认 extra JSON 片段
};

constexpr ProviderPreset kProviderPresets[] = {
    {
        L"DeepSeek",
        L"https://api.deepseek.com",
        L"deepseek-v4-flash",
        L"\"thinking\":{\"type\":\"disabled\"}"
    },
    {
        L"OpenRouter",
        L"https://openrouter.ai/api/v1",
        L"qwen/qwen3-4b",
        L"\"reasoning\":{\"effort\":\"none\"}"
    },
    {
        L"SiliconFlow",
        L"https://api.siliconflow.cn/v1",
        L"Qwen/Qwen3.6-35B-A3B",
        L"\"chat_template_kwargs\":{\"enable_thinking\":false}"
    },
};
```

---

## config.json 结构

### 现有结构

```json
{
  "llm_endpoint": "https://api.siliconflow.cn/v1",
  "llm_api_key": "<encrypted>",
  "llm_model": "Qwen/Qwen3.5-4B",
  "llm_prompt": "",
  "enable_llm_debug": false
}
```

### 新结构

```json
{
  "llm_provider": "DeepSeek",
  "llm_providers": {
    "DeepSeek": {
      "endpoint": "https://api.deepseek.com",
      "api_key": "<encrypted>",
      "model": "deepseek-v4-flash"
    },
    "OpenRouter": {
      "endpoint": "https://openrouter.ai/api/v1",
      "api_key": "<encrypted>",
      "model": "qwen/qwen3-4b"
    }
  },
  "llm_prompt": "",
  "enable_llm_debug": false
}
```

- `llm_provider`：当前选中的供应商名称
- `llm_providers`：每个供应商独立存 endpoint、api_key（DPAPI 加密）、model
- `llm_prompt`：System Prompt（共享，不分供应商）
- `enable_llm_debug`：Debug 开关（共享）
- `llm_extra_params` **不持久化**，切换供应商时从预设自动填；用户手动改的仅当次生效

---

## 数据流

### 切换 Provider Dropdown

```
OnChange(Provider dropdown)
  ↓
  查找 kProviderPresets[] 或 llm_providers[]
  ↓
  填充 Endpoint、Model、Extra Params
  恢复 API Key（如有保存记录）
```

### Save

```
SaveSettingsControls()
  ↓
  读取当前 Provider 名称、Endpoint、API Key、Model
  ↓
  g_config.llm_providers[providerName] = {endpoint, api_key, model}
  g_config.llm_provider = providerName
  ↓
  SaveConfig() → config.json
```

### LLM Refine

```
RefineWithLlmAsync()
  ↓
  llm::RequestConfig cfg
  cfg.endpoint = config.endpoint
  cfg.apiKey = config.apiKey
  cfg.model = config.model
  cfg.systemPrompt = config.llmPrompt
  cfg.extraParams = 从 IDC_LLM_EXTRA 读取当前值
  ↓
  llm::Refine() → BuildRequestBody() 合并 extraParams
```

---

## 改动文件清单

### `llm_refine.h`

| 改动 | 说明 |
|------|------|
| 新增 `ProviderPreset` 结构体 | 定义预设供应商 |
| 新增 `kProviderPresets[]` | 3 个预设 |
| `RequestConfig` 加 `extraParams` | `std::wstring extraParams;` |
| `BuildRequestBody` 合并 extraParams | 非空时拼接到 JSON body |

### `main.cpp`

| 改动 | 说明 |
|------|------|
| `Config` 结构体重构 | `llmEndpoint/llmApiKey/llmModel` → `llmProvider` + `llmProviders` map |
| 新增 `IDC_LLM_PROVIDER` 常量 | 2029 |
| 新增 `IDC_LLM_EXTRA` 常量 | 2030 |
| Tab 从 2 个改为 4 个 | `Recognition / Shortcut / LLM / Prompt` |
| LLM tab 重构 | Provider dropdown + URL/Key/Model + Test/Debug + Extra Params |
| Prompt tab 新建 | 预设按钮 + System Prompt multiline edit |
| `LoadConfig` 重构 | 读取 `llm_provider` + `llm_providers` map |
| `SaveConfig` 重构 | 写入 `llm_provider` + `llm_providers` map |
| `LoadSettingsControls` | 根据当前 provider 填充所有字段 |
| `SaveSettingsControls` | 保存当前 provider 的配置到 map |
| `WM_COMMAND` 新增 Provider 切换处理 | `CBN_SELCHANGE` 时自动填充 |
| `RefineWithLlmAsync` | 传递 extraParams |
| `LayoutSettingsWindow` | 适配 4 tab（tab 数量变了但布局函数不需要改） |
| 兼容旧 config | 首次启动时将旧 `llm_endpoint`/`llm_api_key`/`llm_model` 迁移到新结构 |

---

## 向后兼容

旧 config.json 有 `llm_endpoint`、`llm_api_key`、`llm_model` 三个字段。

`LoadConfig` 中检测：如果存在旧字段但不存在 `llm_providers`，则：
1. 用旧字段创建一个 "Custom" 供应商
2. 设 `llm_provider = "Custom"`
3. 下次 Save 时自动写入新结构

---

## 不做的事

- 不做智能模型名检测（方案 A，靠 Extra Params 手动填）
- 不做 Extra Params 按供应商持久化（用户手动改的仅当次生效，切换供应商时从预设覆盖）
- 不做供应商删除功能（预设不可删，自定义的也很少需要删）
