#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

// qwen_free_proto_sign
//
// Generic HMAC-SHA1 helper used only by offline protocol regression tests.
// This module intentionally contains only the generic HMAC-SHA1 primitive
// used by protocol tests. Provider-specific reverse-engineered key material
// must not be embedded in application source or release payloads.

#include <cstdint>
#include <string>
#include <vector>

namespace qwen_free_proto_sign {

// 通用 HMAC-SHA1。
// key/keyLen: 任意长度密钥（>64 字节会先 SHA1 哈希）。
// msg/msgLen: 待签名消息。
// out20: 输出 20 字节摘要。
void HmacSha1(const void* key, size_t keyLen,
              const void* msg, size_t msgLen,
              uint8_t out20[20]);

// HMAC-SHA1 with a caller-supplied test key.
std::string WsgSignWithKey(const std::string& key, const std::string& content);

// 字节数组转小写 hex 字符串。
std::string ToHexLower(const uint8_t* data, size_t len);

} // namespace qwen_free_proto_sign
