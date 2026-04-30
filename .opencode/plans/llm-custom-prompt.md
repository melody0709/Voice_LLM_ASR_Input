# Plan: Add Custom System Prompt to LLM Settings Tab

## Problem

LLM refine output is identical to ASR input — the current conservative system prompt
likely causes the model to always return verbatim. Need to expose the prompt in Settings
so the user can experiment and debug.

## Root Cause

`BuildRequestBody` in `llm_refine.h:133` hardcodes `kSystemPrompt`. No way to
customize it from the UI.

## Changes

### 1. `llm_refine.h` — Pass custom prompt through `RequestConfig`

- Add `std::wstring systemPrompt;` to `RequestConfig` (line ~128)
- In `BuildRequestBody`: use `cfg.systemPrompt` if non-empty, else fall back to `kSystemPrompt`

### 2. `main.cpp` — Config & Constants

- Add `constexpr int IDC_LLM_PROMPT = 2026;`
- Add `std::wstring llmPrompt;` to `Config` struct

### 3. `main.cpp` — LLM Tab UI

- Add label "System Prompt" + multiline edit (`ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | ES_WANTRETURN`, IDC_LLM_PROMPT) between Model and Test Connection
- Prompt field: y=230, h=100; shift Test Connection + Debug checkbox down ~80px

### 4. `main.cpp` — Load/Save

- `LoadConfig`: `g_config.llmPrompt = ExtractJsonString(json, "llm_prompt", L"");`
- `SaveConfig`: serialize `llm_prompt`
- `LoadSettingsControls`: populate prompt edit
- `SaveSettingsControls`: read prompt edit into `g_config.llmPrompt`

### 5. `main.cpp` — Pass prompt to LLM

- `RefineWithLlmAsync`: set `cfg.systemPrompt = config.llmPrompt;`

## Files to Edit

- `llm_refine.h`: RequestConfig, BuildRequestBody
- `main.cpp`: Config struct, constants, UI, Load/Save, RefineWithLlmAsync
