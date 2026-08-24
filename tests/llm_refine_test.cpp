#include "llm_refine.h"

#include <iostream>
#include <string>

namespace {

int g_failures = 0;

void Expect(bool condition, const char* description) {
    if (condition) return;
    std::cerr << "FAIL: " << description << '\n';
    ++g_failures;
}

void ExpectEndpoint(const std::wstring& endpoint,
                    const std::wstring& expectedHost,
                    const std::wstring& expectedPath,
                    bool expectedSsl,
                    INTERNET_PORT expectedPort,
                    const char* description) {
    std::wstring host;
    std::wstring path;
    bool useSsl = false;
    INTERNET_PORT port = 0;
    const bool parsed = llm::ParseEndpoint(endpoint, host, path, useSsl, port);
    Expect(parsed && host == expectedHost && path == expectedPath &&
               useSsl == expectedSsl && port == expectedPort,
           description);
}

} // namespace

int main() {
    Expect(std::wstring(llm::kProviderPresets[0].defaultModel) == L"deepseek-v4-flash",
           "DeepSeek preset uses the current V4 Flash model");
    Expect(std::wstring(llm::kProviderPresets[1].defaultModel) == L"qwen/qwen3.5-9b",
           "OpenRouter preset no longer uses the retired Qwen3 4B model");
    Expect(std::wstring(llm::kProviderPresets[2].defaultModel) == L"Qwen/Qwen3.6-35B-A3B",
           "SiliconFlow preset keeps the current documented Qwen3.6 model");
    Expect(std::wstring(llm::kProviderPresets[2].extraParams) == L"\"enable_thinking\":false",
           "SiliconFlow preset uses the documented top-level thinking switch");

    llm::RequestConfig request;
    request.model = L"test-model";
    request.extraParams = L"\"thinking\":{\"type\":\"disabled\"}";
    const std::string requestBody = llm::BuildRequestBody(L"test", request);
    const std::string testBody = llm::BuildTestBody(request);
    Expect(llm::IsValidJson(requestBody), "normal refine request is valid JSON");
    Expect(llm::IsValidJson(testBody), "connection-test request is valid JSON");
    Expect(testBody.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos,
           "connection test includes provider Extra Params");
    Expect(testBody.find("\"max_tokens\":8") != std::string::npos,
           "connection test keeps its small completion budget");

    request.extraParams = L"{ \"enable_thinking\": false }";
    const std::string objectExtraBody = llm::BuildRequestBody(L"test", request);
    Expect(llm::IsValidJson(objectExtraBody), "outer braces around Extra Params are accepted");
    Expect(objectExtraBody.find("\"temperature\":0.1,{") == std::string::npos &&
               objectExtraBody.find("\"enable_thinking\": false") != std::string::npos,
           "outer Extra Params braces are merged instead of nested");

    request.extraParams = L"\"broken\":";
    const std::string invalidBody = llm::BuildRequestBody(L"test", request);
    Expect(!llm::IsValidJson(invalidBody), "invalid Extra Params are detected before network I/O");
    const llm::RequestResult invalidRequest = llm::SendRequestRaw(request, invalidBody);
    Expect(!invalidRequest.success && invalidRequest.error.find(L"Invalid request JSON") != std::wstring::npos,
           "invalid Extra Params produce a local diagnostic");

    request.endpoint = L"   ";
    request.apiKey = L"key";
    request.model = L"test-model";
    request.extraParams.clear();
    const llm::RequestResult blankEndpoint = llm::SendRequestRaw(
        request, llm::BuildRequestBody(L"test", request));
    Expect(!blankEndpoint.success &&
               blankEndpoint.error.find(L"required") != std::wstring::npos,
           "whitespace-only required fields are rejected before network I/O");

    ExpectEndpoint(L"https://api.deepseek.com", L"api.deepseek.com", L"/chat/completions",
                   true, INTERNET_DEFAULT_HTTPS_PORT,
                   "host-only DeepSeek base URL produces one path separator");
    ExpectEndpoint(L"https://api.deepseek.com/", L"api.deepseek.com", L"/chat/completions",
                   true, INTERNET_DEFAULT_HTTPS_PORT,
                   "trailing slash is normalized");
    ExpectEndpoint(L"https://openrouter.ai/api/v1", L"openrouter.ai", L"/api/v1/chat/completions",
                   true, INTERNET_DEFAULT_HTTPS_PORT,
                   "versioned provider base path is preserved");
    ExpectEndpoint(L"https://example.com/v1/chat/completions/", L"example.com", L"/v1/chat/completions",
                   true, INTERNET_DEFAULT_HTTPS_PORT,
                   "a full Chat Completions URL is not appended twice");
    ExpectEndpoint(L"localhost:8080/v1", L"localhost", L"/v1/chat/completions",
                   true, 8080,
                   "scheme-less custom endpoint defaults to HTTPS and preserves its port");

    {
        std::wstring host;
        std::wstring path;
        bool useSsl = false;
        INTERNET_PORT port = 0;
        Expect(!llm::ParseEndpoint(L"ftp://example.com/v1", host, path, useSsl, port),
               "non-HTTP endpoint schemes are rejected");
        Expect(!llm::ParseEndpoint(L"https://example.com/v1?key=value", host, path, useSsl, port),
               "query strings are rejected in API Base URLs");
    }

    const std::string escapedResponse =
        "{\"content\":\"wrong\",\"choices\":[{\"message\":{"
        "\"reasoning_content\":\"hidden\","
        "\"content\":\"  \\u4F60\\u597D\\nA\\\"B\\\\C\\/D \\uD83D\\uDE00  \"}}]}";
    Expect(llm::WideToUtf8(llm::ParseResponse(escapedResponse)) == u8"你好\nA\"B\\C/D 😀",
           "response parser follows choices[0].message.content and decodes JSON escapes");

    const std::string arrayResponse =
        "{\"choices\":[{\"message\":{\"content\":["
        "{\"type\":\"text\",\"text\":\"one\"},"
        "{\"type\":\"text\",\"text\":\"two\"}]}}]}";
    Expect(llm::ParseResponse(arrayResponse) == L"onetwo",
           "response parser accepts text content arrays from compatible providers");
    Expect(llm::ParseResponse("{\"choices\":[{\"message\":{\"content\":null}}]}").empty(),
           "null assistant content is rejected");
    Expect(llm::ParseResponse("{\"choices\":[{\"message\":{\"content\":\"\\uD800\"}}]}").empty(),
           "invalid Unicode surrogate escapes are rejected");
    Expect(llm::ParseResponse(
               "{\"choices\":[{\"message\":{\"content\":\"ok\"}}]} trailing").empty(),
           "response parser rejects trailing non-JSON data");

    {
        std::wstring model = L"deepseek-chat";
        std::wstring extra = L"\"thinking\":{\"type\":\"enabled\"}";
        Expect(llm::MigrateLegacyProviderConfig(
                   L"DeepSeek", L"https://api.deepseek.com", model, extra) &&
                   model == L"deepseek-v4-flash" &&
                   extra == L"\"thinking\":{\"type\":\"disabled\"}",
               "retired DeepSeek chat alias migrates while preserving non-thinking behavior");
    }
    {
        std::wstring model = L"deepseek-reasoner";
        std::wstring extra = L"\"thinking\":{\"type\":\"disabled\"}";
        Expect(llm::MigrateLegacyProviderConfig(
                   L"DeepSeek", L"https://api.deepseek.com/", model, extra) &&
                   model == L"deepseek-v4-flash" &&
                   extra == L"\"thinking\":{\"type\":\"enabled\"}",
               "retired DeepSeek reasoner alias migrates while preserving thinking behavior");
    }
    {
        std::wstring model = L"deepseek-chat";
        std::wstring extra;
        Expect(llm::MigrateLegacyProviderConfig(
                   L"DeepSeek", L"https://api.deepseek.com/v1/chat/completions", model, extra) &&
                   model == L"deepseek-v4-flash" &&
                   extra == L"\"thinking\":{\"type\":\"disabled\"}",
               "DeepSeek documented /v1 full endpoint is included in conservative migration");
    }
    {
        std::wstring model = L"deepseek-chat";
        std::wstring extra;
        Expect(llm::MigrateLegacyProviderConfig(
                   L"DeepSeek", L"api.deepseek.com", model, extra) &&
                   model == L"deepseek-v4-flash",
               "scheme-less official DeepSeek endpoint is included in migration");
    }
    {
        std::wstring model = L"qwen/qwen3-4b";
        std::wstring extra = L"\"reasoning\":{\"effort\":\"none\"}";
        Expect(llm::MigrateLegacyProviderConfig(
                   L"OpenRouter", L"https://openrouter.ai/api/v1", model, extra) &&
                   model == L"qwen/qwen3.5-9b",
               "retired OpenRouter preset model migrates to Qwen3.5 9B");
    }
    {
        std::wstring model = L"Qwen/Qwen3.6-35B-A3B";
        std::wstring extra = L"\"chat_template_kwargs\":{\"enable_thinking\":false}";
        Expect(llm::MigrateLegacyProviderConfig(
                   L"SiliconFlow", L"https://api.siliconflow.cn/v1", model, extra) &&
                   model == L"Qwen/Qwen3.6-35B-A3B" &&
                   extra == L"\"enable_thinking\":false",
               "SiliconFlow legacy preset migrates only the obsolete thinking wrapper");
    }
    {
        std::wstring model = L"Qwen/Qwen3.6-35B-A3B";
        std::wstring extra = L"\"chat_template_kwargs\":{\"enable_thinking\":false}";
        Expect(llm::MigrateLegacyProviderConfig(
                   L"SiliconFlow", L"https://api.siliconflow.com/v1/chat/completions", model, extra) &&
                   extra == L"\"enable_thinking\":false",
               "SiliconFlow global official endpoint is included in migration");
    }
    {
        std::wstring model = L"custom/model";
        std::wstring extra = L"\"chat_template_kwargs\":{\"enable_thinking\":false}";
        Expect(!llm::MigrateLegacyProviderConfig(
                   L"SiliconFlow", L"https://api.siliconflow.cn/v1", model, extra) &&
                   extra == L"\"chat_template_kwargs\":{\"enable_thinking\":false}",
               "SiliconFlow migration preserves custom model parameter choices");
    }
    {
        std::wstring model = L"deepseek-chat";
        std::wstring extra = L"\"thinking\":{\"type\":\"disabled\"}";
        Expect(!llm::MigrateLegacyProviderConfig(
                   L"DeepSeek", L"https://proxy.example/v1", model, extra) &&
                   model == L"deepseek-chat",
               "legacy model migration does not rewrite a custom endpoint");
    }

    {
        std::string providers =
            "{\"Alpha\":{\"endpoint\":\"https://alpha.example/v1\","
            "\"extra_params\":\"{ unmatched brace in a string\"},"
            "\"Beta\":{\"endpoint\":\"https://beta.example/v1\"}}";
        std::vector<std::string> names;
        Expect(llm::GetJsonObjectMemberNames(providers, names) &&
                   names.size() == 2 && names[0] == "Alpha" && names[1] == "Beta",
               "provider-store scanning ignores braces inside JSON strings");
        std::string beta;
        Expect(llm::GetJsonObjectMemberRaw(providers, "Beta", beta) &&
                   beta.find("beta.example") != std::string::npos &&
                   beta.find("alpha.example") == std::string::npos,
               "provider-store lookup is scoped to the exact top-level provider");
        const std::string gamma =
            "{\"endpoint\":\"https://gamma.example/v1\","
            "\"extra_params\":\"} unmatched closing brace\"}";
        Expect(llm::SetJsonObjectMemberRaw(providers, "Gamma", gamma) &&
                   llm::GetJsonObjectMemberRaw(providers, "Gamma", beta),
               "provider-store update safely accepts brace characters in string fields");
        Expect(llm::RemoveJsonObjectMember(providers, "Alpha") &&
                   !llm::GetJsonObjectMemberRaw(providers, "Alpha", beta) &&
                   llm::GetJsonObjectMemberRaw(providers, "Beta", beta) &&
                   llm::IsValidJson(providers),
               "provider-store deletion removes only the requested member");
    }
    {
        std::string malformed = "{\"Alpha\":";
        const std::string original = malformed;
        Expect(!llm::SetJsonObjectMemberRaw(malformed, "Beta", "{}") &&
                   malformed == original,
               "provider-store update preserves malformed input byte-for-byte");
    }

    Expect(llm::kRefineTimeouts.receiveMs == 15000 &&
               llm::kRefineTimeouts.connectMs == 5000,
           "LLM receive timeout is tolerant but still bounded");

    if (g_failures == 0) {
        std::cout << "LLM refine regression tests passed\n";
    }
    return g_failures == 0 ? 0 : 1;
}
