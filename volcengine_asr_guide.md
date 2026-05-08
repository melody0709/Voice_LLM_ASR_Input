# Volcengine (Doubao) ASR Setup Guide

> 🇨🇳 [中文版](doc/volcengine_asr_guide_zh.md)

This document provides detailed instructions for configuring the Volcengine (Doubao) large model speech recognition in VoxType, including parameter descriptions, API credential setup, and implementation details.

---

## Table of Contents

- [1. Overview](#1-overview)
- [2. Obtaining API Credentials](#2-obtaining-api-credentials)
- [3. Settings Field Reference](#3-settings-field-reference)
  - [3.1 API Key](#31-api-key)
  - [3.2 Model Version](#32-model-version)
  - [3.3 ASR Mode](#33-asr-mode)
  - [3.4 Language](#34-language)
  - [3.5 end_window_size / force_to_speech_time](#35-end_window_size--force_to_speech_time)
  - [3.6 Feature Toggles](#36-feature-toggles)
  - [3.7 First-Token Acceleration](#37-first-token-acceleration)
  - [3.8 Hotwords & Correction Tables](#38-hotwords--correction-tables)
  - [3.9 Extra Params](#39-extra-params)
  - [3.10 Dialog Context](#310-dialog-context)
- [4. Implementation Architecture](#4-implementation-architecture)
  - [4.1 WebSocket Binary Protocol](#41-websocket-binary-protocol)
  - [4.2 Request Flow](#42-request-flow)
  - [4.3 corpus Field Merging Logic](#43-corpus-field-merging-logic)
  - [4.4 Context Serialization](#44-context-serialization)
- [5. Debugging](#5-debugging)
- [6. Reference Documentation](#6-reference-documentation)

---

## 1. Overview

VoxType connects to the Volcengine Doubao large model speech recognition service via the WebSocket protocol, supporting three recognition modes and two billing methods. Audio is fixed at 16kHz 16bit mono PCM, sent in real-time through a binary frame protocol.

**Key features:**
- Hotword tables (`boosting_table`) and correction tables (`correct_table`)
- Dialog context (`context`) using recognition history for improved accuracy
- Semantic smoothing (DDC), two-pass recognition (nonstream), first-token acceleration
- API Key encrypted with DPAPI and stored locally in `config.json`

---

## 2. Obtaining API Credentials

### 2.1 Create an Application

1. Visit the [Volcengine Doubao Speech Console](https://console.volcengine.com/speech/app)
2. Click "Create Application" and fill in the application name
3. Enable the "Large Model Streaming Speech Recognition" service in the application details

### 2.2 Obtain API Key

**New Console** (the method VoxType currently supports):

1. Go to the application details page
2. Find the **App Key** (this is the value for `X-Api-Key`)
3. Copy this value into the `API Key` field in VoxType Settings

> ⚠️ The old console uses `X-Api-App-Key` + `X-Api-Access-Key` dual-key authentication, which VoxType does not currently support.

### 2.3 Obtain Resource ID

The Resource ID corresponds to the billing method, selected when enabling the service in the console:

| Display Name | Resource ID | Description |
|-------------|-------------|-------------|
| Seed-ASR 2.0 (duration) | `volc.seedasr.sauc.duration` | Per-hour billing (recommended) |
| Seed-ASR 2.0 (concurrent) | `volc.seedasr.sauc.concurrent` | Per-connection billing |
| BigASR 1.0 (duration) | `volc.bigasr.sauc.duration` | Legacy model, per-hour billing |
| BigASR 1.0 (concurrent) | `volc.bigasr.sauc.concurrent` | Legacy model, per-connection billing |

### 2.4 Create Hotword Tables

1. Go to [Self-Learning Platform - Hotword Management](https://console.volcengine.com/speech/hotword)
2. Click "Add Hotword File"
3. Enter the table name and hotword content (one hotword per line, optional weight e.g. `火山语音|8`, default weight 4)
4. After creation, copy the **Hotword ID** and paste it into the `Hotwords ID` field in VoxType Settings

**Limitations:**
- Up to 500 tables per application
- Up to 2000 hotwords per table
- Each hotword must be under 10 characters
- Weight range 1–10, default 4
- No punctuation (except newlines and spaces)
- Arabic numerals must be converted to Chinese characters (e.g. `A4L` → `A四L`)
- Only one table takes effect per request

---

## 3. Settings Field Reference

### 3.1 API Key

The **App Key** obtained from the Volcengine console, used as the `X-Api-Key` HTTP header during WebSocket handshake. This value is encrypted with Windows DPAPI and stored in the `volc_api_key` field of `config.json`.

### 3.2 Model Version

Dropdown selection, corresponds to the `X-Api-Resource-Id` HTTP header:

| Option | Resource ID | Description |
|--------|-------------|-------------|
| Seed-ASR 2.0 (duration) | `volc.seedasr.sauc.duration` | Doubao 2.0 model, per-hour billing |
| Seed-ASR 2.0 (concurrent) | `volc.seedasr.sauc.concurrent` | Doubao 2.0 model, per-connection billing |
| BigASR 1.0 (duration) | `volc.bigasr.sauc.duration` | Legacy 1.0 model, per-hour billing |
| BigASR 1.0 (concurrent) | `volc.bigasr.sauc.concurrent` | Legacy 1.0 model, per-connection billing |

### 3.3 ASR Mode

| Mode | WebSocket Path | Characteristics |
|------|---------------|-----------------|
| `bigmodel_nostream` | `/api/v3/sauc/bigmodel_nostream` | Streaming input mode, returns results after input completes, **highest accuracy**, supports language parameter |
| `bigmodel_async` | `/api/v3/sauc/bigmodel_async` | Optimized bidirectional streaming, only returns data when results change, **best latency** |
| `bigmodel` | `/api/v3/sauc/bigmodel` | Legacy bidirectional streaming, each input packet corresponds to one output packet |

**Recommendations:**
- For accuracy → `bigmodel_nostream`
- For real-time performance → `bigmodel_async`
- `bigmodel` is the legacy path; prefer `bigmodel_async`

### 3.4 Language

Only effective in `bigmodel_nostream` mode. The parameter is not sent in other modes.

| Option | Code | Description |
|--------|------|-------------|
| Auto | (empty) | Auto-detect Chinese, English, Shanghainese, Minnan, Sichuanese, Shaanxi, Cantonese |
| English | `en-US` | English |
| Japanese | `ja-JP` | Japanese |
| Korean | `ko-KR` | Korean |
| French | `fr-FR` | French |
| German | `de-DE` | German |
| Spanish | `es-MX` | Spanish |
| Portuguese | `pt-BR` | Portuguese |
| Indonesian | `id-ID` | Indonesian |

### 3.5 end_window_size / force_to_speech_time

**end_window_size** (forced stop time):
- Default 800ms, minimum 200ms
- When silence exceeds this value, recognition is finalized with a `definite` marker
- For scenarios requiring high real-time performance
- When configured, semantic segmentation (`vad_segment_duration`) is disabled

**force_to_speech_time** (forced speech time):
- Default 10000ms (10 seconds), minimum 1ms
- Finalization based on silence only occurs after audio exceeds this duration
- Without configuration, no finalization occurs in the first 10 seconds
- Recommended to use with `end_window_size`; setting to 1000 may affect accuracy

### 3.6 Feature Toggles

| Toggle | API Parameter | Description | Mode Restriction |
|--------|--------------|-------------|-----------------|
| enable_ddc | `enable_ddc` | Semantic smoothing, removes filler words and repetitions | All modes |
| enable_nonstream | `enable_nonstream` | Two-pass recognition: streaming + offline re-recognition | `bigmodel_async` only |
| enable_music_fc | `enable_music_fc` | Music function call | `bigmodel_nostream` or `bigmodel_async` + `enable_nonstream` |
| enable_poi_fc | `enable_poi_fc` | POI function call | `bigmodel_nostream` or `bigmodel_async` + `enable_nonstream` |

**enable_ddc (Semantic Smoothing)**: Recommended to enable. Automatically removes filler words like "um", "ah", "like" and repetitions, improving input text quality.

**enable_nonstream (Two-pass Recognition)**: Only available in `bigmodel_async` mode. When enabled, VAD-segmented utterances are re-recognized with the non-streaming model after VAD sentence boundary detection, combining real-time display (fast) with final accuracy (accurate). Enabling this automatically activates VAD segmentation (800ms default).

### 3.7 First-Token Acceleration

| Field | API Parameter | Description |
|-------|--------------|-------------|
| enable_accelerate | `enable_accelerate_text` | Enable first-token acceleration, trades accuracy for faster initial output |
| score | `accelerate_score` | Acceleration rate, 0–20, higher = faster first token |

### 3.8 Hotwords & Correction Tables

**Hotwords ID / Name**:
- `boosting_table_id`: Self-learning platform hotword table ID
- `boosting_table_name`: Hotword table name
- Either ID or Name is sufficient; if both are filled, both are sent

**Correct ID / Name**:
- `correct_table_id`: Self-learning platform correction table ID
- `correct_table_name`: Correction table name
- Used to automatically replace specific words in recognition results (e.g., domain-specific terminology correction)

**Hotwords and correction tables are supported in all modes.**

### 3.9 Extra Params

Click the `Edit Params` button to open an editing dialog for additional `request`-level JSON parameters.

**Format:** `"key1":"value1","key2":"value2"` (no outer braces needed)

**Preset templates:**
- `Filter`: Fills in `"sensitive_words_filter":"system_reserved_filter"` (sensitive word filtering)
- `Result`: Fills in `"result_type":"single","vad_segment_duration":3000` (incremental results + semantic segmentation silence threshold)

> ⚠️ The `corpus` key is automatically skipped because `corpus` is built uniformly from the hotword/correction/context fields. Do not manually include `corpus` in Extra Params.

### 3.10 Dialog Context

When `Use history as context` is checked, each recognition request sends the last N recognition results as dialog context to the server.

**How it works:**
1. After each recognition completes, the result is stored in an in-memory history queue
2. On the next recording, history results are built into the `corpus.context` field
3. Format: `{"context_type":"dialog_ctx","context_data":[{"text":"..."}]}`
4. This JSON object is serialized as a string (inner quotes escaped) as the value of `context`

**Limitations:**
- Bidirectional streaming modes (`bigmodel` / `bigmodel_async`): 100 tokens
- Streaming input mode (`bigmodel_nostream`): 5000 words
- Context is ordered newest to oldest, automatically truncated beyond 800 tokens or 20 rounds

**History count:** Default 3, range 1–20. Context data is not persisted; the history starts empty after program restart.

---

## 4. Implementation Architecture

### 4.1 WebSocket Binary Protocol

VoxType uses Volcengine's custom binary frame protocol, not WebSocket text protocol. Frame format:

```
| Byte 0        | Byte 1        | Byte 2        | Byte 3   |
| ver | hdrSize | msgType|flags | ser  | comp   | reserved |
| [sequence number - 4 bytes, optional]                          |
| [payload size - 4 bytes, big-endian]                           |
| [payload]                                                      |
```

**Message types:**

| msgType | Meaning |
|---------|---------|
| 0x01 | full client request (initial request with JSON parameters) |
| 0x02 | audio only request (raw audio data) |
| 0x09 | full server response (recognition results) |
| 0x0F | error response (server error) |

**Flags:**

| flags | Meaning |
|-------|---------|
| 0x00 | No sequence number |
| 0x01 | Positive sequence number |
| 0x02 | Last packet (negative packet), no sequence |
| 0x03 | Last packet (negative packet), with sequence |

### 4.2 Request Flow

```
1. WebSocket handshake
   GET /api/v3/sauc/{mode}
   Headers: X-Api-Key, X-Api-Resource-Id, X-Api-Connect-Id

2. Send full client request (msgType=0x01)
   Payload: JSON-formatted audio metadata and request parameters

3. Loop: send audio only request (msgType=0x02)
   Send one PCM packet every ~200ms (6400 bytes = 200ms @ 16kHz 16bit mono)

4. Send last packet (flags=0x02)
   Empty audio body, signals end of recording

5. Receive server response (msgType=0x09)
   Parse text and definite fields from JSON

6. Close WebSocket connection
```

### 4.3 corpus Field Merging Logic

All `corpus` sub-fields are merged into a single JSON object in code, no longer mutually exclusive:

```cpp
// volcengine_asr.h OpenSession()
std::string corpusParts;
if (!cfg.hotwordsId.empty())
    corpusParts += ",\"boosting_table_id\":\"...\"";
if (!cfg.hotwordsName.empty())
    corpusParts += ",\"boosting_table_name\":\"...\"";
if (!cfg.correctTableId.empty())
    corpusParts += ",\"correct_table_id\":\"...\"";
if (!cfg.correctTableName.empty())
    corpusParts += ",\"correct_table_name\":\"...\"";
if (!cfg.contextJson.empty())
    corpusParts += ",\"context\":\"...\"";  // serialized string
if (!corpusParts.empty())
    requestJson += ",\"corpus\":{" + corpusParts.substr(1) + "}";
```

Generated JSON example:
```json
"corpus": {
    "boosting_table_id": "abc123",
    "correct_table_id": "def456",
    "context": "{\"context_type\":\"dialog_ctx\",\"context_data\":[{\"text\":\"hello world\"}]}"
}
```

### 4.4 Context Serialization

The `context` field value must be a **JSON string** (with inner quotes escaped), not a raw JSON object. This is required by the official API:

> context_data fields are ordered from newest to oldest, and must be serialized as a jsonstring (with escaped quotes)

Implementation (`volcengine_asr.h`):
```cpp
std::string ctxRaw = WideToUtf8(cfg.contextJson);
std::string ctxEscaped;
for (char c : ctxRaw) {
    if (c == '\\') ctxEscaped += "\\\\";
    else if (c == '"') ctxEscaped += "\\\"";
    // ... other escapes
    else ctxEscaped += c;
}
corpusParts += ",\"context\":\"" + ctxEscaped + "\"";
```

---

## 5. Debugging

VoxType has built-in debug logging, output to `%TEMP%\volc_asr_debug.log`.

Log contents include:
- WebSocket connection process
- Sent frame headers and data hex dumps
- Server response frame parsing
- Extracted recognition text and definite markers

If you encounter connection issues, check the log for `X-Tt-Logid` (server-side log ID) to provide to Volcengine technical support.

---

## 6. Reference Documentation

| Document | Link |
|----------|------|
| Large Model Streaming ASR API | https://www.volcengine.com/docs/6561/1354869 |
| Console Usage FAQ | https://www.volcengine.com/docs/6561/196768 |
| Hotword Management | https://www.volcengine.com/docs/6561/155739 |
| Correction Table Management | https://www.volcengine.com/docs/6561/1206007 |
| Self-Learning Platform API | https://www.volcengine.com/docs/6561/1742791 |
| Doubao Speech Console | https://console.volcengine.com/speech/app |
| Hotword Management Console | https://console.volcengine.com/speech/hotword |
