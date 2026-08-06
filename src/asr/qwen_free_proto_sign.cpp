#include "qwen_free_proto_sign.h"

#include <cstring>

namespace qwen_free_proto_sign {

namespace {

// === SHA-1 实现（FIPS 180-4）===
// 仅用于 HMAC 内层/外层哈希，无需外部直接调用。

struct Sha1Ctx {
    uint32_t h[5];        // H0-H4 状态
    uint64_t bitLen;      // 已处理消息位数
    uint8_t buf[64];      // 当前块缓冲
    size_t bufLen;        // buf 中已写入字节数
};

constexpr uint32_t kSha1Iv[5] = {
    0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u
};

inline uint32_t Rotl(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

void Sha1Init(Sha1Ctx* ctx) {
    std::memcpy(ctx->h, kSha1Iv, sizeof(kSha1Iv));
    ctx->bitLen = 0;
    ctx->bufLen = 0;
}

void Sha1ProcessBlock(Sha1Ctx* ctx, const uint8_t* block) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(block[i * 4]) << 24) |
               (uint32_t(block[i * 4 + 1]) << 16) |
               (uint32_t(block[i * 4 + 2]) << 8) |
               (uint32_t(block[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = Rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = ctx->h[0], b = ctx->h[1], c = ctx->h[2], d = ctx->h[3], e = ctx->h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);              k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                          k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);        k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                          k = 0xCA62C1D6u; }
        uint32_t t = Rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = Rotl(b, 30); b = a; a = t;
    }
    ctx->h[0] += a; ctx->h[1] += b; ctx->h[2] += c; ctx->h[3] += d; ctx->h[4] += e;
}

void Sha1Update(Sha1Ctx* ctx, const uint8_t* data, size_t len) {
    ctx->bitLen += uint64_t(len) * 8;
    while (len > 0) {
        size_t copy = 64 - ctx->bufLen;
        if (copy > len) copy = len;
        std::memcpy(ctx->buf + ctx->bufLen, data, copy);
        ctx->bufLen += copy;
        data += copy;
        len -= copy;
        if (ctx->bufLen == 64) {
            Sha1ProcessBlock(ctx, ctx->buf);
            ctx->bufLen = 0;
        }
    }
}

void Sha1Final(Sha1Ctx* ctx, uint8_t out20[20]) {
    // 追加 0x80
    ctx->buf[ctx->bufLen++] = 0x80;
    // 若剩余不足 8 字节放 bitLen，再开一块
    if (ctx->bufLen > 56) {
        while (ctx->bufLen < 64) ctx->buf[ctx->bufLen++] = 0;
        Sha1ProcessBlock(ctx, ctx->buf);
        ctx->bufLen = 0;
    }
    while (ctx->bufLen < 56) ctx->buf[ctx->bufLen++] = 0;
    // 末尾 8 字节大端 bitLen
    uint64_t bl = ctx->bitLen;
    for (int i = 7; i >= 0; --i) {
        ctx->buf[56 + i] = uint8_t(bl & 0xff);
        bl >>= 8;
    }
    Sha1ProcessBlock(ctx, ctx->buf);

    for (int i = 0; i < 5; ++i) {
        out20[i * 4]     = uint8_t(ctx->h[i] >> 24);
        out20[i * 4 + 1] = uint8_t(ctx->h[i] >> 16);
        out20[i * 4 + 2] = uint8_t(ctx->h[i] >> 8);
        out20[i * 4 + 3] = uint8_t(ctx->h[i]);
    }
    // 清理敏感状态（best-effort，不保证编译器不优化掉）
    std::memset(ctx, 0, sizeof(*ctx));
}

} // namespace

void HmacSha1(const void* key, size_t keyLen,
              const void* msg, size_t msgLen,
              uint8_t out20[20]) {
    // 1. key > 64 字节先 SHA1 哈希；否则直接用，右侧补 0 到 64 字节
    uint8_t k0[64] = {0};
    if (keyLen > 64) {
        Sha1Ctx kc;
        Sha1Init(&kc);
        Sha1Update(&kc, static_cast<const uint8_t*>(key), keyLen);
        Sha1Final(&kc, k0);
    } else {
        std::memcpy(k0, key, keyLen);
    }

    // 2. ipad / opad
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = k0[i] ^ 0x36;
        opad[i] = k0[i] ^ 0x5c;
    }

    // 3. inner = SHA1(ipad || msg)
    Sha1Ctx ic;
    Sha1Init(&ic);
    Sha1Update(&ic, ipad, 64);
    Sha1Update(&ic, static_cast<const uint8_t*>(msg), msgLen);
    uint8_t inner[20];
    Sha1Final(&ic, inner);

    // 4. output = SHA1(opad || inner)
    Sha1Ctx oc;
    Sha1Init(&oc);
    Sha1Update(&oc, opad, 64);
    Sha1Update(&oc, inner, 20);
    Sha1Final(&oc, out20);
}

std::string ToHexLower(const uint8_t* data, size_t len) {
    static const char hex[] = "0123456789abcdef";
    std::string out;
    out.resize(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = hex[data[i] >> 4];
        out[i * 2 + 1] = hex[data[i] & 0x0f];
    }
    return out;
}

std::string WsgSignWithKey(const std::string& key, const std::string& content) {
    uint8_t digest[20];
    HmacSha1(key.data(), key.size(), content.data(), content.size(), digest);
    return ToHexLower(digest, 20);
}

} // namespace qwen_free_proto_sign
