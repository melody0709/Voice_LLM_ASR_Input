// 离线协议回归测试：公共 JSON 解码、百度响应解析与火山请求参数构造。
// 无网络、无 WinHTTP 实际调用，只验证纯协议函数。

#include <windows.h>
#include <climits>
#include <cstdio>
#include <string>

#include "utils.h"
#include "baidu_asr.h"       // ExtractJsonInt / ExtractBaiduResultText
#include "volcengine_asr.h"  // BuildExtraParamsJson

// 离线测试桩：不链接真实日志实现（其依赖 globals.h → sherpa-onnx 等重头文件），
// 本测试只验证协议解析行为，日志调用直接吞掉。
namespace asr_runtime_log {
void Write(const char* format, ...) {
    (void)format;
}
} // namespace asr_runtime_log

static int g_failures = 0;

#define CHECK(cond, name)                                     \
    do {                                                      \
        if (!(cond)) {                                        \
            ++g_failures;                                     \
            wprintf(L"FAIL: %hs\n", name);                    \
        } else {                                              \
            wprintf(L"ok:   %hs\n", name);                    \
        }                                                     \
    } while (0)

int wmain() {
    // 1) 基础反转义：\" \\ \n \t
    {
        const std::string json = "{\"text\":\"a\\\"b\\\\c\\nd\\te\"}";
        std::wstring t = ExtractJsonStringDecoded(json, "text");
        CHECK(t == L"a\"b\\c\nd\te", "decoded basic escapes");
    }
    // 2) Unicode 代理对：\ud83d\ude00 -> U+1F600（UTF-16 代理对）
    {
        const std::string json = "{\"text\":\"\\ud83d\\ude00\"}";
        std::wstring t = ExtractJsonStringDecoded(json, "text");
        CHECK(t.size() == 2 && t[0] == 0xD83D && t[1] == 0xDE00,
              "surrogate pair UTF-16 preserved");
    }
    // 3) 汉字原样透传
    {
        const std::string json = "{\"text\":\"\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C\"}";
        CHECK(ExtractJsonStringDecoded(json, "text") == L"\u4F60\u597D\u4E16\u754C",
              "utf8 passthrough");
    }
    // 4) 字符串提取器拒绝数字；数字走 ExtractJsonInt
    {
        const std::string json = "{\"expires_in\":2592000}";
        CHECK(ExtractJsonStringDecoded(json, "expires_in") == L"",
              "string extractor rejects numbers");
        CHECK(baidu_asr::ExtractJsonInt(json, "expires_in", -1) == 2592000,
              "numeric expires_in decoded");
    }
    // 5) 百度 result[0]：文本内转义引号（旧裸 find 会在此截断）
    {
        const std::string body = "{\"err_no\":0,\"result\":[\"\xE4\xBB\x96\xE8\xAF\xB4\\\"\xE4\xBD\xA0\xE5\xA5\xBD\\\"\xE5\x95\x8A\"]}";
        CHECK(baidu_asr::ExtractBaiduResultText(body) == L"\u4ED6\u8BF4\"\u4F60\u597D\"\u554A",
              "baidu result[0] escaped quotes");
    }
    // 6) 百度 result[0]：结果含反斜杠
    {
        const std::string body = "{\"err_no\":0,\"result\":[\"C:\\\\temp\"]}";
        CHECK(baidu_asr::ExtractBaiduResultText(body) == L"C:\\temp",
              "baidu result[0] backslash");
    }
    // 7) 火山 Extra Params：字符串内逗号/括号不得截断
    {
        const std::wstring params = L"\"utterance_eos\":1,\"punctuation_text\":\"a,b{c}\",\"nested\":{\"k\": [1,2]}";
        const std::string out = volc_asr::BuildExtraParamsJson(params);
        const std::string expect = ",\"utterance_eos\":1,\"punctuation_text\":\"a,b{c}\",\"nested\":{\"k\": [1,2]}";
        CHECK(out == expect, "extra params string-aware");
    }
    // 8) 火山 Extra Params：corpus 整体跳过，其余保留
    {
        const std::wstring params = L"\"corpus\":{\"x\":\"a,b\"},\"keep\":1";
        const std::string out = volc_asr::BuildExtraParamsJson(params);
        CHECK(out == ",\"keep\":1", "extra params corpus skipped");
    }
    // 9) 火山 Extra Params：空输入返回空
    {
        CHECK(volc_asr::BuildExtraParamsJson(L"").empty(), "extra params empty input");
        CHECK(volc_asr::BuildExtraParamsJson(L" \t\r\n ").empty(),
              "extra params whitespace-only input");
        std::string out;
        CHECK(!volc_asr::TryBuildExtraParamsJson(L"\"bad\":\"unterminated", out),
              "extra params malformed string rejected");
        CHECK(!volc_asr::TryBuildExtraParamsJson(L"\"bad\":[1,2", out),
              "extra params malformed nesting rejected");
    }
    // 10) 畸形 JSON 字符串必须整体失败，不能返回部分文本
    {
        CHECK(ExtractJsonStringDecoded("{\"text\":\"partial", "text").empty(),
              "unterminated string rejected");
        CHECK(ExtractJsonStringDecoded("{\"text\":\"bad\\q\"}", "text").empty(),
              "unknown escape rejected");
        CHECK(ExtractJsonStringDecoded("{\"text\":\"bad\\u12xz\"}", "text").empty(),
              "invalid unicode escape rejected");
        CHECK(ExtractJsonStringDecoded("{\"text\":\"ok\",\"tail\":", "text").empty(),
              "malformed trailing document rejected");
    }
    // 11) UTF-16 代理项必须成对出现
    {
        CHECK(ExtractJsonStringDecoded("{\"text\":\"\\ud83d\"}", "text").empty(),
              "lone high surrogate rejected");
        CHECK(ExtractJsonStringDecoded("{\"text\":\"\\ude00\"}", "text").empty(),
              "lone low surrogate rejected");
    }
    // 12) 数组跳过嵌套值时必须保持结构，不得被嵌套逗号截断
    {
        const std::string body = "{\"result\":[{\"nested\":[1,2]},\"ok\"]}";
        CHECK(ExtractJsonArrayFirstStringDecoded(body, "result") == L"ok",
              "array nested value skipped structurally");
        CHECK(ExtractJsonArrayFirstStringDecoded("{\"result\":[{\"x\":1},]}", "result").empty(),
              "malformed array rejected");
    }
    // 13) 百度短 token 有效期不会因提前刷新减法而下溢
    {
        CHECK(baidu_asr::TokenCacheLifetimeMs(30) == 27000ULL,
              "short token lifetime bounded");
        CHECK(baidu_asr::TokenCacheLifetimeMs(600) == 540000ULL,
              "long token refresh margin capped");
    }
    // 14) 火山字符串字段转义覆盖引号、反斜杠和控制字符
    {
        CHECK(volc_asr::ToBackslashEscape("a\"b\\c\nd\t") == "a\\\"b\\\\c\\nd\\t",
              "volc json string escaping");
        CHECK(EscapeJson(std::wstring(L"a\b\f\n\t") + wchar_t(1)) ==
                  "a\\b\\f\\n\\t\\u0001",
              "common json control escaping");
    }
    // 15) 百度整数解析不抛异常，也不把小数/溢出值截断成有效整数
    {
        CHECK(baidu_asr::ExtractJsonInt("{\"n\":-2147483648}", "n", 7) == INT_MIN,
              "baidu int minimum decoded");
        CHECK(baidu_asr::ExtractJsonInt("{\"n\":2147483648}", "n", 7) == 7,
              "baidu int overflow rejected");
        CHECK(baidu_asr::ExtractJsonInt("{\"n\":12.5}", "n", 7) == 7,
              "baidu fractional number rejected");
        CHECK(baidu_asr::ExtractJsonInt("{\"n\":--1}", "n", 7) == 7,
              "baidu malformed number rejected");
    }

    if (g_failures == 0) {
        wprintf(L"ALL PASS\n");
        return 0;
    }
    wprintf(L"%d FAIL\n", g_failures);
    return 1;
}
