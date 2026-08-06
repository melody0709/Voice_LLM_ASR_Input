#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "asr_dispatcher.h"
#include "asr_streaming_session.h"

#include <memory>
#include <string>

// 创建千问 IME 免费后端的流式 ASR 会话（A1 纯协议还原）。
//
// 协议层：
//   - qwen_free_proto_utdid: 获取设备指纹（LoadLibrary UTDID.dll 或注册表）
//   - qwen_free_proto_sign:  WSG HMAC-SHA1 签名
//   - qwen_free_proto_asr:   WebSocket ASR（PCM → transcript）
//   - qwen_free_proto_llm:   HTTP LLM 后处理（润色/标点/纠错）
//
// VoxType 负责 WASAPI 采集 PCM，送 EnqueuePcmChunk；
// StopInput 后等待 ASR final，再调用 LLM 做后处理。
//
// 失败（UTDID 无效 / ASR 连接失败 / LLM 失败）会回填 error 并返回 nullptr，
// 由调用方降级到其他 ASR provider。
std::unique_ptr<IStreamingAsrSession> CreateQwenFreeStreamingSession(
    const Config& config,
    HWND targetWindow,
    AsrLlmRefineFn refineFn,
    std::wstring* lastRawAsrText,
    const SelectionContext& selection = {});
