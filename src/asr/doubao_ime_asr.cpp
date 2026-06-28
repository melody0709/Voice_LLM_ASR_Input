#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "doubao_ime_asr.h"

#include "cloud_http_common.h"
#include "utils.h"

#include <bcrypt.h>
#include <objbase.h>
#include <winhttp.h>
#include <opus/opus.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "opus.lib")

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

#ifndef WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT
#define WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT 122
#endif

namespace doubao_ime_asr {
namespace {

constexpr wchar_t kRegisterHost[] = L"log.snssdk.com";
constexpr wchar_t kSettingsHost[] = L"is.snssdk.com";
constexpr wchar_t kWsHost[] = L"frontier-audio-ime-ws.doubao.com";
constexpr wchar_t kWsPath[] = L"/ocean/api/v1/ws";
constexpr int kAid = 401734;
constexpr int kVersionCode = 100102018;
constexpr char kUserAgentA[] =
    "com.bytedance.android.doubaoime/100102018 (Linux; U; Android 16; en_US; Pixel 7 Pro; "
    "Build/BP2A.250605.031.A2; Cronet/TTNetVersion:94cf429a 2025-11-17 QuicVersion:1f89f732 2025-05-08)";
constexpr wchar_t kUserAgentW[] =
    L"com.bytedance.android.doubaoime/100102018 (Linux; U; Android 16; en_US; Pixel 7 Pro; "
    L"Build/BP2A.250605.031.A2; Cronet/TTNetVersion:94cf429a 2025-11-17 QuicVersion:1f89f732 2025-05-08)";
constexpr DWORD kSessionReadyTimeoutMs = 8000;
constexpr DWORD kReceiveSliceMs = 1000;

enum class FrameState {
    First = 1,
    Middle = 3,
    Last = 9,
};

struct DeviceRegistration {
    std::wstring deviceId;
    std::wstring cdid;
};

struct AsrRequest {
    std::string token;
    std::string serviceName;
    std::string methodName;
    std::string payload;
    std::vector<BYTE> audioData;
    std::string requestId;
    int frameState = 0;
};

struct AsrResponse {
    std::string requestId;
    std::string taskId;
    std::string serviceName;
    std::string messageType;
    int statusCode = 0;
    std::string statusMessage;
    std::string resultJson;
};

struct TextCandidate {
    std::wstring text;
    bool isFinal = false;
    bool found = false;
};

struct DoubaoConnection {
    std::atomic<HINTERNET> hSession{nullptr};
    std::atomic<HINTERNET> hConnect{nullptr};
    std::atomic<HINTERNET> hBootstrapSession{nullptr};
    std::atomic<HINTERNET> hBootstrapConnect{nullptr};
    std::atomic<HINTERNET> hBootstrapRequest{nullptr};
    std::atomic<HINTERNET> hRequest{nullptr};
    std::atomic<HINTERNET> hWebSocket{nullptr};

    ~DoubaoConnection() {
        Close();
    }

    HINTERNET Request() const {
        return hRequest.load();
    }

    void SetSession(HINTERNET session) {
        hSession.store(session);
    }

    HINTERNET TakeSession() {
        return hSession.exchange(nullptr);
    }

    void SetConnect(HINTERNET connect) {
        hConnect.store(connect);
    }

    HINTERNET TakeConnect() {
        return hConnect.exchange(nullptr);
    }

    void SetBootstrapSession(HINTERNET session) {
        hBootstrapSession.store(session);
    }

    HINTERNET TakeBootstrapSession() {
        return hBootstrapSession.exchange(nullptr);
    }

    void SetBootstrapConnect(HINTERNET connect) {
        hBootstrapConnect.store(connect);
    }

    HINTERNET TakeBootstrapConnect() {
        return hBootstrapConnect.exchange(nullptr);
    }

    void SetBootstrapRequest(HINTERNET request) {
        hBootstrapRequest.store(request);
    }

    HINTERNET TakeBootstrapRequest() {
        return hBootstrapRequest.exchange(nullptr);
    }

    void SetRequest(HINTERNET request) {
        hRequest.store(request);
    }

    HINTERNET TakeRequest() {
        return hRequest.exchange(nullptr);
    }

    HINTERNET WebSocket() const {
        return hWebSocket.load();
    }

    void SetWebSocket(HINTERNET ws) {
        hWebSocket.store(ws);
    }

    HINTERNET TakeWebSocket() {
        return hWebSocket.exchange(nullptr);
    }

    void CloseBootstrap() {
        HINTERNET req = TakeBootstrapRequest();
        if (req) WinHttpCloseHandle(req);
        HINTERNET connect = TakeBootstrapConnect();
        if (connect) WinHttpCloseHandle(connect);
        HINTERNET session = TakeBootstrapSession();
        if (session) WinHttpCloseHandle(session);
    }

    void CloseRequest() {
        HINTERNET req = TakeRequest();
        if (req) WinHttpCloseHandle(req);
    }

    void CloseTransportHandles() {
        CloseRequest();
        HINTERNET connect = TakeConnect();
        if (connect) WinHttpCloseHandle(connect);
        HINTERNET session = TakeSession();
        if (session) WinHttpCloseHandle(session);
    }

    void Abort() {
        CloseBootstrap();
        HINTERNET ws = TakeWebSocket();
        if (ws) WinHttpCloseHandle(ws);
        CloseTransportHandles();
    }

    void Close() {
        CloseBootstrap();
        CloseRequest();
        HINTERNET ws = TakeWebSocket();
        if (ws) {
            WinHttpWebSocketClose(ws, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(ws);
        }
        HINTERNET connect = TakeConnect();
        if (connect) WinHttpCloseHandle(connect);
        HINTERNET session = TakeSession();
        if (session) WinHttpCloseHandle(session);
    }
};

std::wstring LowerCase(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return value;
}

long long UnixTimeMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::wstring GuidString() {
    GUID guid = {};
    if (FAILED(CoCreateGuid(&guid))) {
        wchar_t fallback[64] = {};
        swprintf_s(fallback, L"%08lx-%04x-%04x-%04x-%012llx",
                   GetTickCount(),
                   static_cast<unsigned>(GetCurrentProcessId() & 0xffff),
                   static_cast<unsigned>(GetCurrentThreadId() & 0xffff),
                   static_cast<unsigned>((GetTickCount64() >> 16) & 0xffff),
                   static_cast<unsigned long long>(GetTickCount64() & 0xffffffffffffULL));
        return LowerCase(fallback);
    }

    wchar_t buf[64] = {};
    StringFromGUID2(guid, buf, 64);
    std::wstring value = buf;
    if (!value.empty() && value.front() == L'{') value.erase(value.begin());
    if (!value.empty() && value.back() == L'}') value.pop_back();
    return LowerCase(value);
}

std::wstring RandomHex16() {
    BYTE bytes[8] = {};
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const ULONGLONG fallback = GetTickCount64() ^
            (static_cast<ULONGLONG>(GetCurrentProcessId()) << 32) ^
            static_cast<ULONGLONG>(GetCurrentThreadId());
        memcpy(bytes, &fallback, sizeof(bytes));
    }
    std::wostringstream oss;
    oss << std::hex << std::setfill(L'0');
    for (BYTE b : bytes) {
        oss << std::setw(2) << static_cast<unsigned>(b);
    }
    return oss.str();
}

std::string ToBody(const std::string& value) {
    return value;
}

std::vector<BYTE> BodyBytes(const std::string& value) {
    return std::vector<BYTE>(value.begin(), value.end());
}

std::wstring WinHttpErrorText(const wchar_t* step, DWORD err) {
    return std::wstring(step) + L" failed (err=" + std::to_wstring(err) + L")";
}

std::wstring HttpFailureText(const CloudHttpResponse& response) {
    if (response.ok) {
        return L"HTTP " + std::to_wstring(response.statusCode);
    }
    std::wstring step = response.failedStep.empty() ? L"HTTP request" : response.failedStep;
    return step + L" failed (err=" + std::to_wstring(response.winhttpError) + L")";
}

CloudHttpResponse SendDoubaoHttpRequest(DoubaoConnection& conn, const CloudHttpRequest& request) {
    CloudHttpResponse response;
    auto cleanup = [&]() {
        conn.CloseBootstrap();
    };

    HINTERNET hSession = WinHttpOpen(
        kUserAgentW,
        request.accessType,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0);
    if (!hSession) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpOpen";
        return response;
    }
    conn.SetBootstrapSession(hSession);
    WinHttpSetTimeouts(hSession, request.timeoutMs, request.timeoutMs,
                       request.timeoutMs, request.timeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, request.host.c_str(), request.port, 0);
    if (!hConnect) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpConnect";
        cleanup();
        return response;
    }
    conn.SetBootstrapConnect(hConnect);

    DWORD flags = request.useSsl ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        request.method.c_str(),
        request.path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags);
    if (!hRequest) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpOpenRequest";
        cleanup();
        return response;
    }
    conn.SetBootstrapRequest(hRequest);
    WinHttpSetTimeouts(hRequest, request.timeoutMs, request.timeoutMs,
                       request.timeoutMs, request.timeoutMs);

    void* bodyData = request.body.empty()
        ? WINHTTP_NO_REQUEST_DATA
        : const_cast<BYTE*>(request.body.data());
    DWORD bodySize = static_cast<DWORD>((std::min)(
        request.body.size(),
        static_cast<size_t>((std::numeric_limits<DWORD>::max)())));

    if (!WinHttpSendRequest(hRequest,
                            request.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : request.headers.c_str(),
                            request.headers.empty() ? 0 : static_cast<DWORD>(-1),
                            bodyData,
                            bodySize,
                            bodySize,
                            0)) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpSendRequest";
        cleanup();
        return response;
    }

    if (!WinHttpReceiveResponse(hRequest, nullptr)) {
        response.winhttpError = GetLastError();
        response.failedStep = L"WinHttpReceiveResponse";
        cleanup();
        return response;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    response.statusCode = statusCode;

    for (;;) {
        DWORD bytesAvailable = 0;
        if (!WinHttpQueryDataAvailable(hRequest, &bytesAvailable)) {
            response.winhttpError = GetLastError();
            response.failedStep = L"WinHttpQueryDataAvailable";
            cleanup();
            return response;
        }
        if (bytesAvailable == 0) break;

        std::string chunk(bytesAvailable, '\0');
        DWORD bytesRead = 0;
        if (!WinHttpReadData(hRequest, chunk.data(), bytesAvailable, &bytesRead)) {
            response.winhttpError = GetLastError();
            response.failedStep = L"WinHttpReadData";
            cleanup();
            return response;
        }
        response.body.append(chunk.data(), bytesRead);
    }

    response.ok = true;
    cleanup();
    return response;
}

std::wstring HexUpper(const BYTE* data, size_t bytes) {
    std::wostringstream oss;
    oss << std::uppercase << std::hex << std::setfill(L'0');
    for (size_t i = 0; i < bytes; ++i) {
        oss << std::setw(2) << static_cast<unsigned>(data[i]);
    }
    return oss.str();
}

std::wstring Md5HexUpper(const std::string& value) {
    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    std::vector<BYTE> hashObject;
    std::vector<BYTE> hash;
    DWORD objectLength = 0;
    DWORD hashLength = 0;
    DWORD cbData = 0;

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_MD5_ALGORITHM, nullptr, 0) != 0) return L"";
    auto cleanup = [&]() {
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    };

    if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength),
                          sizeof(objectLength), &cbData, 0) != 0 ||
        BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                          sizeof(hashLength), &cbData, 0) != 0) {
        cleanup();
        return L"";
    }

    hashObject.resize(objectLength);
    hash.resize(hashLength);
    if (BCryptCreateHash(hAlg, &hHash, hashObject.data(), objectLength, nullptr, 0, 0) != 0 ||
        BCryptHashData(hHash, reinterpret_cast<PUCHAR>(const_cast<char*>(value.data())),
                       static_cast<ULONG>(value.size()), 0) != 0 ||
        BCryptFinishHash(hHash, hash.data(), hashLength, 0) != 0) {
        cleanup();
        return L"";
    }
    cleanup();
    return HexUpper(hash.data(), hash.size());
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ReadHex4(const std::string& value, size_t pos, uint32_t& out) {
    if (pos + 4 > value.size()) return false;
    uint32_t cp = 0;
    for (size_t i = 0; i < 4; ++i) {
        int v = HexValue(value[pos + i]);
        if (v < 0) return false;
        cp = (cp << 4) | static_cast<uint32_t>(v);
    }
    out = cp;
    return true;
}

void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp <= 0x7F) {
        out.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::wstring ExtractJsonStringValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return L"";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') return L"";
    ++pos;

    std::string unescaped;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '"') break;
        if (c != '\\') {
            unescaped.push_back(c);
            continue;
        }
        if (pos >= json.size()) break;
        char esc = json[pos++];
        switch (esc) {
        case '"': unescaped.push_back('"'); break;
        case '\\': unescaped.push_back('\\'); break;
        case '/': unescaped.push_back('/'); break;
        case 'b': unescaped.push_back('\b'); break;
        case 'f': unescaped.push_back('\f'); break;
        case 'n': unescaped.push_back('\n'); break;
        case 'r': unescaped.push_back('\r'); break;
        case 't': unescaped.push_back('\t'); break;
        case 'u': {
            uint32_t cp = 0;
            if (!ReadHex4(json, pos, cp)) break;
            pos += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF &&
                pos + 6 <= json.size() && json[pos] == '\\' && json[pos + 1] == 'u') {
                uint32_t low = 0;
                if (ReadHex4(json, pos + 2, low) && low >= 0xDC00 && low <= 0xDFFF) {
                    pos += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                }
            }
            AppendUtf8(unescaped, cp);
            break;
        }
        default:
            unescaped.push_back(esc);
            break;
        }
    }
    return Utf8ToWide(unescaped);
}

std::wstring ExtractJsonNumberLikeString(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return L"";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos < json.size() && json[pos] == '"') {
        return ExtractJsonStringValue(json, key);
    }
    const size_t start = pos;
    while (pos < json.size() && json[pos] != ',' && json[pos] != '}' &&
           json[pos] != '\r' && json[pos] != '\n') {
        ++pos;
    }
    return Trim(Utf8ToWide(json.substr(start, pos - start)));
}

bool ExtractJsonBoolValue(const std::string& json, const std::string& key, bool fallback) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return fallback;
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return fallback;
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (json.compare(pos, 4, "true") == 0) return true;
    if (json.compare(pos, 5, "false") == 0) return false;
    return fallback;
}

bool IsAsciiWordChar(wchar_t c) {
    return (c >= L'a' && c <= L'z') ||
           (c >= L'A' && c <= L'Z') ||
           (c >= L'0' && c <= L'9');
}

bool IsAsciiSentencePunctuation(wchar_t c) {
    return c == L'.' || c == L',' || c == L';' || c == L':' ||
           c == L'!' || c == L'?';
}

void AppendRecognizedSegment(std::wstring& target, const std::wstring& text) {
    if (text.empty()) return;
    if (!target.empty() && !iswspace(target.back()) && !iswspace(text.front())) {
        const wchar_t prev = target.back();
        const wchar_t next = text.front();
        if (IsAsciiWordChar(next) && (IsAsciiWordChar(prev) || IsAsciiSentencePunctuation(prev))) {
            target.push_back(L' ');
        }
    }
    target += text;
}

size_t FindMatching(const std::string& value, size_t openPos, char openChar, char closeChar) {
    int depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = openPos; i < value.size(); ++i) {
        char c = value[i];
        if (inString) {
            if (escape) {
                escape = false;
            } else if (c == '\\') {
                escape = true;
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == openChar) {
            ++depth;
        } else if (c == closeChar) {
            --depth;
            if (depth == 0) return i;
        }
    }
    return std::string::npos;
}

std::string ExtractJsonObjectValue(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return "";
    pos = json.find(':', pos + search.size());
    if (pos == std::string::npos) return "";
    ++pos;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                 json[pos] == '\r' || json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '{') return "";
    const size_t end = FindMatching(json, pos, '{', '}');
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos + 1);
}

TextCandidate ExtractTextCandidate(const std::string& resultJson) {
    TextCandidate candidate;
    const size_t key = resultJson.find("\"results\"");
    if (key == std::string::npos) return candidate;
    const size_t arrayStart = resultJson.find('[', key);
    if (arrayStart == std::string::npos) return candidate;
    const size_t arrayEnd = FindMatching(resultJson, arrayStart, '[', ']');
    if (arrayEnd == std::string::npos) return candidate;

    std::wstring aggregateText;
    bool lastSegmentFinal = false;
    size_t pos = arrayStart + 1;
    while (pos < arrayEnd) {
        const size_t objStart = resultJson.find('{', pos);
        if (objStart == std::string::npos || objStart >= arrayEnd) break;
        const size_t objEnd = FindMatching(resultJson, objStart, '{', '}');
        if (objEnd == std::string::npos || objEnd > arrayEnd) break;

        std::string object = resultJson.substr(objStart, objEnd - objStart + 1);
        std::wstring text = ExtractJsonStringValue(object, "text");
        if (!text.empty()) {
            const bool isInterim = ExtractJsonBoolValue(object, "is_interim", true);
            const bool isVadFinished = ExtractJsonBoolValue(object, "is_vad_finished", false);
            const std::string extra = ExtractJsonObjectValue(object, "extra");
            const bool nonstreamResult = !extra.empty() &&
                ExtractJsonBoolValue(extra, "nonstream_result", false);
            AppendRecognizedSegment(aggregateText, text);
            lastSegmentFinal = nonstreamResult || (!isInterim && isVadFinished);
        }

        pos = objEnd + 1;
    }

    if (!aggregateText.empty()) {
        candidate.text = aggregateText;
        candidate.isFinal = lastSegmentFinal;
        candidate.found = true;
    }
    return candidate;
}

void EncodeVarint(std::vector<BYTE>& out, uint64_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<BYTE>(value | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<BYTE>(value));
}

void EncodeFieldTag(std::vector<BYTE>& out, uint32_t fieldNumber, uint32_t wireType) {
    EncodeVarint(out, (static_cast<uint64_t>(fieldNumber) << 3) | wireType);
}

void EncodeString(std::vector<BYTE>& out, uint32_t fieldNumber, const std::string& value) {
    if (value.empty()) return;
    EncodeFieldTag(out, fieldNumber, 2);
    EncodeVarint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void EncodeBytes(std::vector<BYTE>& out, uint32_t fieldNumber, const std::vector<BYTE>& value) {
    if (value.empty()) return;
    EncodeFieldTag(out, fieldNumber, 2);
    EncodeVarint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

void EncodeVarintField(std::vector<BYTE>& out, uint32_t fieldNumber, uint64_t value) {
    if (value == 0) return;
    EncodeFieldTag(out, fieldNumber, 0);
    EncodeVarint(out, value);
}

std::vector<BYTE> EncodeRequest(const AsrRequest& request) {
    std::vector<BYTE> out;
    out.reserve(256 + request.audioData.size());
    EncodeString(out, 2, request.token);
    EncodeString(out, 3, request.serviceName);
    EncodeString(out, 5, request.methodName);
    EncodeString(out, 6, request.payload);
    EncodeBytes(out, 7, request.audioData);
    EncodeString(out, 8, request.requestId);
    EncodeVarintField(out, 9, static_cast<uint64_t>(request.frameState));
    return out;
}

bool DecodeVarint(const std::vector<BYTE>& data, size_t& cursor, uint64_t& value) {
    value = 0;
    int shift = 0;
    while (cursor < data.size()) {
        BYTE b = data[cursor++];
        value |= static_cast<uint64_t>(b & 0x7F) << shift;
        if ((b & 0x80) == 0) return true;
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

bool DecodeStringField(const std::vector<BYTE>& data, size_t& cursor, std::string& value) {
    uint64_t len = 0;
    if (!DecodeVarint(data, cursor, len)) return false;
    if (len > data.size() || cursor + static_cast<size_t>(len) > data.size()) return false;
    value.assign(reinterpret_cast<const char*>(data.data() + cursor), static_cast<size_t>(len));
    cursor += static_cast<size_t>(len);
    return true;
}

bool SkipField(const std::vector<BYTE>& data, size_t& cursor, uint32_t wireType) {
    if (wireType == 0) {
        uint64_t ignored = 0;
        return DecodeVarint(data, cursor, ignored);
    }
    if (wireType == 2) {
        uint64_t len = 0;
        if (!DecodeVarint(data, cursor, len)) return false;
        if (len > data.size() || cursor + static_cast<size_t>(len) > data.size()) return false;
        cursor += static_cast<size_t>(len);
        return true;
    }
    if (wireType == 5) {
        if (cursor + 4 > data.size()) return false;
        cursor += 4;
        return true;
    }
    if (wireType == 1) {
        if (cursor + 8 > data.size()) return false;
        cursor += 8;
        return true;
    }
    return false;
}

bool DecodeResponse(const std::vector<BYTE>& data, AsrResponse& response, std::wstring& error) {
    response = {};
    size_t cursor = 0;
    while (cursor < data.size()) {
        uint64_t tag = 0;
        if (!DecodeVarint(data, cursor, tag)) {
            error = L"failed to decode response tag";
            return false;
        }
        const uint32_t fieldNumber = static_cast<uint32_t>(tag >> 3);
        const uint32_t wireType = static_cast<uint32_t>(tag & 0x7);

        switch (fieldNumber) {
        case 1:
            if (wireType != 2 || !DecodeStringField(data, cursor, response.requestId)) return false;
            break;
        case 2:
            if (wireType != 2 || !DecodeStringField(data, cursor, response.taskId)) return false;
            break;
        case 3:
            if (wireType != 2 || !DecodeStringField(data, cursor, response.serviceName)) return false;
            break;
        case 4:
            if (wireType != 2 || !DecodeStringField(data, cursor, response.messageType)) return false;
            break;
        case 5: {
            if (wireType != 0) return false;
            uint64_t value = 0;
            if (!DecodeVarint(data, cursor, value)) return false;
            response.statusCode = static_cast<int>(value);
            break;
        }
        case 6:
            if (wireType != 2 || !DecodeStringField(data, cursor, response.statusMessage)) return false;
            break;
        case 7:
            if (wireType != 2 || !DecodeStringField(data, cursor, response.resultJson)) return false;
            break;
        default:
            if (!SkipField(data, cursor, wireType)) return false;
            break;
        }
    }
    return true;
}

std::wstring BuildRegisterQuery(const std::wstring& cdid) {
    std::wostringstream query;
    query << L"/service/2/device_register/?device_platform=android&os=android&ssmix=a"
          << L"&_rticket=" << UnixTimeMs()
          << L"&cdid=" << cdid
          << L"&channel=official&aid=" << kAid
          << L"&app_name=oime&version_code=" << kVersionCode
          << L"&version_name=1.1.2&manifest_version_code=" << kVersionCode
          << L"&update_version_code=" << kVersionCode
          << L"&resolution=1080*2400&dpi=420&device_type=Pixel%207%20Pro"
          << L"&device_brand=google&language=zh&os_api=34&os_version=16&ac=wifi";
    return query.str();
}

std::string BuildRegisterBody(const std::wstring& cdid,
                              const std::wstring& openudid,
                              const std::wstring& clientudid) {
    std::ostringstream body;
    body << "{";
    body << "\"magic_tag\":\"ss_app_log\",";
    body << "\"header\":{";
    body << "\"device_id\":0,\"install_id\":0,\"aid\":" << kAid << ",";
    body << "\"app_name\":\"oime\",\"version_code\":" << kVersionCode << ",";
    body << "\"version_name\":\"1.1.2\",\"manifest_version_code\":" << kVersionCode << ",";
    body << "\"update_version_code\":" << kVersionCode << ",";
    body << "\"channel\":\"official\",\"package\":\"com.bytedance.android.doubaoime\",";
    body << "\"device_platform\":\"android\",\"os\":\"android\",\"os_api\":\"34\",\"os_version\":\"16\",";
    body << "\"device_type\":\"Pixel 7 Pro\",\"device_brand\":\"google\",\"device_model\":\"Pixel 7 Pro\",";
    body << "\"resolution\":\"1080*2400\",\"dpi\":\"420\",\"language\":\"zh\",\"timezone\":8,";
    body << "\"access\":\"wifi\",\"rom\":\"UP1A.231005.007\",\"rom_version\":\"UP1A.231005.007\",";
    body << "\"openudid\":\"" << EscapeJson(openudid) << "\",";
    body << "\"clientudid\":\"" << EscapeJson(clientudid) << "\",";
    body << "\"cdid\":\"" << EscapeJson(cdid) << "\",";
    body << "\"region\":\"CN\",\"tz_name\":\"Asia/Shanghai\",\"tz_offset\":28800,";
    body << "\"sim_region\":\"cn\",\"carrier_region\":\"cn\",\"cpu_abi\":\"arm64-v8a\",";
    body << "\"build_serial\":\"unknown\",\"not_request_sender\":0,";
    body << "\"sig_hash\":\"\",\"google_aid\":\"\",\"mc\":\"\",\"serial_number\":\"\"";
    body << "},";
    body << "\"_gen_time\":" << UnixTimeMs();
    body << "}";
    return body.str();
}

bool RegisterDevice(DoubaoConnection& conn, DoubaoImeConfig& cfg, std::wstring& error) {
    const std::wstring cdid = GuidString();
    const std::wstring openudid = RandomHex16();
    const std::wstring clientudid = GuidString();
    const std::string body = BuildRegisterBody(cdid, openudid, clientudid);

    CloudHttpRequest request;
    request.method = L"POST";
    request.host = kRegisterHost;
    request.path = BuildRegisterQuery(cdid);
    request.timeoutMs = cfg.connectTimeoutMs;
    request.headers =
        L"Content-Type: application/json\r\n"
        L"User-Agent: " + std::wstring(kUserAgentW) + L"\r\n";
    request.body = BodyBytes(body);

    CloudHttpResponse response = SendDoubaoHttpRequest(conn, request);
    if (!response.ok || response.statusCode < 200 || response.statusCode >= 300) {
        error = L"device registration failed: " + HttpFailureText(response);
        if (!response.body.empty()) error += L" " + Utf8ToWide(response.body);
        return false;
    }

    std::wstring deviceId = ExtractJsonNumberLikeString(response.body, "device_id");
    if (deviceId.empty() || deviceId == L"0") {
        error = L"device registration failed: missing device_id";
        return false;
    }
    cfg.deviceId = deviceId;
    cfg.cdid = cdid;
    cfg.token.clear();
    return true;
}

std::wstring BuildSettingsQuery(const std::wstring& deviceId, const std::wstring& cdid) {
    std::wostringstream query;
    query << L"/service/settings/v3/?device_platform=android&os=android&ssmix=a"
          << L"&channel=official&aid=" << kAid
          << L"&app_name=oime&version_code=" << kVersionCode
          << L"&version_name=1.1.2&device_id=" << deviceId
          << L"&cdid=" << cdid;
    return query.str();
}

bool GetAsrToken(DoubaoConnection& conn, DoubaoImeConfig& cfg, std::wstring& error) {
    if (cfg.deviceId.empty() || cfg.cdid.empty()) {
        error = L"missing device credentials";
        return false;
    }

    const std::string body = "body=null";
    std::wstring stub = Md5HexUpper(body);
    if (stub.empty()) {
        error = L"failed to calculate x-ss-stub";
        return false;
    }

    CloudHttpRequest request;
    request.method = L"POST";
    request.host = kSettingsHost;
    request.path = BuildSettingsQuery(cfg.deviceId, cfg.cdid);
    request.timeoutMs = cfg.connectTimeoutMs;
    request.headers =
        L"Content-Type: application/x-www-form-urlencoded\r\n"
        L"User-Agent: " + std::wstring(kUserAgentW) + L"\r\n"
        L"x-ss-stub: " + stub + L"\r\n";
    request.body = BodyBytes(body);

    CloudHttpResponse response = SendDoubaoHttpRequest(conn, request);
    if (!response.ok || response.statusCode < 200 || response.statusCode >= 300) {
        error = L"token request failed: " + HttpFailureText(response);
        if (!response.body.empty()) error += L" " + Utf8ToWide(response.body);
        return false;
    }

    std::wstring token = ExtractJsonStringValue(response.body, "app_key");
    if (token.empty()) {
        error = L"token request failed: missing app_key";
        return false;
    }
    cfg.token = token;
    return true;
}

std::string BuildStartSessionPayload(const DoubaoImeConfig& cfg) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"audio_info\":{";
    oss << "\"channel\":" << cfg.channels << ",";
    oss << "\"format\":\"speech_opus\",";
    oss << "\"sample_rate\":" << cfg.sampleRate;
    oss << "},";
    oss << "\"enable_punctuation\":" << (cfg.enablePunctuation ? "true" : "false") << ",";
    oss << "\"enable_speech_rejection\":false,";
    oss << "\"extra\":{";
    oss << "\"app_name\":\"com.android.chrome\",";
    oss << "\"cell_compress_rate\":8,";
    oss << "\"did\":\"" << EscapeJson(cfg.deviceId) << "\",";
    oss << "\"enable_asr_threepass\":true,";
    oss << "\"enable_asr_twopass\":true,";
    oss << "\"input_mode\":\"tool\"";
    oss << "}";
    oss << "}";
    return oss.str();
}

std::string BuildAudioPayload(long long timestampMs) {
    return "{\"extra\":{},\"timestamp_ms\":" + std::to_string(timestampMs) + "}";
}

bool SendBinaryMessage(HINTERNET ws, const std::vector<BYTE>& data, std::wstring& error) {
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }
    DWORD err = WinHttpWebSocketSend(ws,
                                     WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
                                     const_cast<BYTE*>(data.data()),
                                     static_cast<DWORD>(data.size()));
    if (err != NO_ERROR) {
        error = L"WebSocket send failed (err=" + std::to_wstring(err) + L")";
        return false;
    }
    return true;
}

bool ReceiveBinaryMessage(HINTERNET ws,
                          DWORD timeoutMs,
                          std::vector<BYTE>& data,
                          bool& timedOut,
                          bool& closed,
                          std::wstring& error) {
    data.clear();
    timedOut = false;
    closed = false;
    if (!ws) {
        error = L"WebSocket not connected";
        return false;
    }

    DWORD effectiveTimeout = std::max<DWORD>(timeoutMs, 1);
    WinHttpSetOption(ws, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &effectiveTimeout, sizeof(effectiveTimeout));

    std::vector<BYTE> buffer(64 * 1024);
    while (true) {
        DWORD bytesRead = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufferType = WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        DWORD err = WinHttpWebSocketReceive(ws,
                                            buffer.data(),
                                            static_cast<DWORD>(buffer.size()),
                                            &bytesRead,
                                            &bufferType);
        if (err == ERROR_WINHTTP_TIMEOUT) {
            timedOut = true;
            return true;
        }
        if (err != NO_ERROR) {
            error = L"WebSocket receive failed (err=" + std::to_wstring(err) + L")";
            return false;
        }
        if (bufferType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            closed = true;
            return true;
        }
        if (bytesRead > 0) {
            data.insert(data.end(), buffer.begin(), buffer.begin() + bytesRead);
        }
        if (bufferType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            bufferType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
            return true;
        }
    }
}

std::wstring ResponseFailureText(const AsrResponse& response, const wchar_t* fallback) {
    std::wstring message = Utf8ToWide(response.statusMessage);
    if (message.empty()) message = fallback;
    if (response.statusCode != 0) {
        message += L" (status=" + std::to_wstring(response.statusCode) + L")";
    }
    if (!response.messageType.empty()) {
        message = Utf8ToWide(response.messageType) + L": " + message;
    }
    return message;
}

bool ReadResponse(DoubaoConnection& conn,
                  DWORD timeoutMs,
                  AsrResponse& response,
                  bool& timedOut,
                  bool& closed,
                  std::wstring& error) {
    std::vector<BYTE> message;
    if (!ReceiveBinaryMessage(conn.WebSocket(), timeoutMs, message, timedOut, closed, error)) {
        return false;
    }
    if (timedOut || closed) return true;
    if (message.empty()) {
        timedOut = true;
        return true;
    }
    if (!DecodeResponse(message, response, error)) {
        if (error.empty()) error = L"failed to decode response";
        return false;
    }
    return true;
}

bool WaitForMessage(DoubaoConnection& conn,
                    const char* expectedType,
                    DWORD timeoutMs,
                    std::wstring& error) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        AsrResponse response;
        bool timedOut = false;
        bool closed = false;
        const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(
            kReceiveSliceMs, deadline - GetTickCount64()));
        if (!ReadResponse(conn, std::max<DWORD>(slice, 1), response, timedOut, closed, error)) {
            return false;
        }
        if (timedOut) continue;
        if (closed) {
            error = L"server closed WebSocket while waiting for " + Utf8ToWide(expectedType);
            return false;
        }
        if (response.messageType == expectedType) {
            return true;
        }
        if (response.messageType == "TaskFailed" || response.messageType == "SessionFailed") {
            error = ResponseFailureText(response, L"ASR session failed");
            return false;
        }
        if (!response.statusMessage.empty() && response.statusCode != 0) {
            error = ResponseFailureText(response, L"ASR server error");
            return false;
        }
    }
    error = L"timed out waiting for " + Utf8ToWide(expectedType);
    return false;
}

bool EnsureCredentials(DoubaoConnection& conn, DoubaoImeConfig& cfg, bool& changed, std::wstring& error) {
    changed = false;
    if (cfg.deviceId.empty() || cfg.cdid.empty()) {
        if (!RegisterDevice(conn, cfg, error)) return false;
        changed = true;
    }
    if (cfg.token.empty()) {
        if (!GetAsrToken(conn, cfg, error)) return false;
        changed = true;
    }
    return true;
}

bool OpenWebSocket(DoubaoConnection& conn, const DoubaoImeConfig& cfg, std::wstring& error) {
    HINTERNET hSession = WinHttpOpen(kUserAgentW,
                                     WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME,
                                     WINHTTP_NO_PROXY_BYPASS,
                                     0);
    if (!hSession) {
        error = WinHttpErrorText(L"WinHttpOpen", GetLastError());
        return false;
    }
    conn.SetSession(hSession);
    WinHttpSetTimeouts(hSession,
                       cfg.connectTimeoutMs,
                       cfg.connectTimeoutMs,
                       cfg.connectTimeoutMs,
                       cfg.connectTimeoutMs);

    HINTERNET hConnect = WinHttpConnect(hSession, kWsHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        error = WinHttpErrorText(L"WinHttpConnect", GetLastError());
        return false;
    }
    conn.SetConnect(hConnect);

    std::wstring path = std::wstring(kWsPath) + L"?aid=" + std::to_wstring(kAid) +
        L"&device_id=" + cfg.deviceId;
    HINTERNET hReq = WinHttpOpenRequest(hConnect,
                                        L"GET",
                                        path.c_str(),
                                        nullptr,
                                        WINHTTP_NO_REFERER,
                                        WINHTTP_DEFAULT_ACCEPT_TYPES,
                                        WINHTTP_FLAG_SECURE);
    if (!hReq) {
        error = WinHttpErrorText(L"WinHttpOpenRequest", GetLastError());
        return false;
    }
    conn.SetRequest(hReq);

    WinHttpSetTimeouts(hReq,
                       cfg.connectTimeoutMs,
                       cfg.connectTimeoutMs,
                       cfg.connectTimeoutMs,
                       cfg.connectTimeoutMs);
    WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

    DWORD closeTimeout = 3000;
    DWORD keepAlive = 30000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &closeTimeout, sizeof(closeTimeout));
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof(keepAlive));

    std::wstring headers =
        L"User-Agent: " + std::wstring(kUserAgentW) + L"\r\n"
        L"proto-version: v2\r\n"
        L"x-custom-keepalive: true\r\n";
    if (!WinHttpAddRequestHeaders(hReq, headers.c_str(), static_cast<DWORD>(-1),
                                  WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE)) {
        error = WinHttpErrorText(L"WinHttpAddRequestHeaders", GetLastError());
        conn.CloseRequest();
        return false;
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        error = WinHttpErrorText(L"WinHttpSendRequest", GetLastError());
        conn.CloseRequest();
        return false;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        error = WinHttpErrorText(L"WinHttpReceiveResponse", GetLastError());
        conn.CloseRequest();
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 101) {
        error = L"WebSocket upgrade failed (HTTP " + std::to_wstring(statusCode) + L")";
        conn.CloseRequest();
        return false;
    }

    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    conn.CloseRequest();
    if (!ws) {
        error = WinHttpErrorText(L"WinHttpWebSocketCompleteUpgrade", GetLastError());
        return false;
    }
    conn.SetWebSocket(ws);
    return true;
}

} // namespace

static bool StartsWithText(const std::wstring& value, const std::wstring& prefix) {
    return value.size() >= prefix.size() &&
           std::equal(prefix.begin(), prefix.end(), value.begin());
}

static size_t SuffixPrefixOverlap(const std::wstring& base, const std::wstring& incoming) {
    const size_t maxOverlap = (std::min)(base.size(), incoming.size());
    for (size_t len = maxOverlap; len > 1; --len) {
        if (std::equal(base.end() - static_cast<ptrdiff_t>(len),
                       base.end(),
                       incoming.begin())) {
            return len;
        }
    }
    return 0;
}

std::wstring MergeRecognizedText(std::wstring base, const std::wstring& incomingRaw) {
    std::wstring incoming = Trim(incomingRaw);
    base = Trim(base);
    if (incoming.empty()) return base;
    if (base.empty()) return incoming;
    if (base == incoming) return base;
    if (StartsWithText(incoming, base)) return incoming;
    if (StartsWithText(base, incoming)) return base;

    const size_t overlap = SuffixPrefixOverlap(base, incoming);
    if (overlap > 0) {
        base += incoming.substr(overlap);
        return base;
    }

    AppendRecognizedSegment(base, incoming);
    return base;
}

struct RealtimeClient::Impl {
    DoubaoImeConfig cfg;
    DoubaoConnection conn;
    std::string requestId;
    long long timestampBaseMs = 0;
    uint64_t frameIndex = 0;
    bool credentialsChanged = false;
    std::atomic<bool> connected{false};
    OpusEncoder* encoder = nullptr;

    explicit Impl(DoubaoImeConfig c) : cfg(std::move(c)) {}

    ~Impl() {
        if (encoder) {
            opus_encoder_destroy(encoder);
            encoder = nullptr;
        }
    }
};

RealtimeClient::RealtimeClient(DoubaoImeConfig cfg)
    : impl_(std::make_unique<Impl>(std::move(cfg))) {}

RealtimeClient::~RealtimeClient() = default;

bool RealtimeClient::Connect(std::wstring& error) {
    if (!impl_) {
        error = L"client not initialized";
        return false;
    }
    if (impl_->connected.load()) return true;

    bool changed = false;
    if (!EnsureCredentials(impl_->conn, impl_->cfg, changed, error)) {
        impl_->credentialsChanged = impl_->credentialsChanged || changed;
        return false;
    }
    impl_->credentialsChanged = impl_->credentialsChanged || changed;

    if (!OpenWebSocket(impl_->conn, impl_->cfg, error)) {
        Close();
        return false;
    }

    impl_->requestId = WideToUtf8(GuidString());
    impl_->timestampBaseMs = UnixTimeMs();
    impl_->frameIndex = 0;

    AsrRequest startTask;
    startTask.token = WideToUtf8(impl_->cfg.token);
    startTask.serviceName = "ASR";
    startTask.methodName = "StartTask";
    startTask.requestId = impl_->requestId;
    if (!SendBinaryMessage(impl_->conn.WebSocket(), EncodeRequest(startTask), error) ||
        !WaitForMessage(impl_->conn, "TaskStarted", kSessionReadyTimeoutMs, error)) {
        Close();
        return false;
    }

    AsrRequest startSession;
    startSession.token = WideToUtf8(impl_->cfg.token);
    startSession.serviceName = "ASR";
    startSession.methodName = "StartSession";
    startSession.requestId = impl_->requestId;
    startSession.payload = BuildStartSessionPayload(impl_->cfg);
    if (!SendBinaryMessage(impl_->conn.WebSocket(), EncodeRequest(startSession), error) ||
        !WaitForMessage(impl_->conn, "SessionStarted", kSessionReadyTimeoutMs, error)) {
        Close();
        return false;
    }

    int opusError = OPUS_OK;
    impl_->encoder = opus_encoder_create(impl_->cfg.sampleRate,
                                         impl_->cfg.channels,
                                         OPUS_APPLICATION_VOIP,
                                         &opusError);
    if (!impl_->encoder || opusError != OPUS_OK) {
        error = L"Opus encoder init failed";
        Close();
        return false;
    }

    impl_->connected.store(true);
    return true;
}

bool RealtimeClient::SendPcmFrame(const BYTE* pcm, size_t bytes, bool isLast, std::wstring& error) {
    if (!impl_ || !impl_->connected.load()) {
        error = L"WebSocket not connected";
        return false;
    }
    if (!pcm || bytes == 0) return true;
    if (!impl_->encoder) {
        error = L"Opus encoder not initialized";
        return false;
    }

    const size_t frameBytes = FrameBytes();
    if (bytes != frameBytes) {
        error = L"invalid PCM frame size";
        return false;
    }
    const int samplesPerFrame = static_cast<int>(frameBytes / sizeof(int16_t));

    std::vector<opus_int16> samples(samplesPerFrame);
    for (int i = 0; i < samplesPerFrame; ++i) {
        samples[i] = static_cast<opus_int16>(
            static_cast<int16_t>(pcm[i * 2] | (static_cast<int16_t>(pcm[i * 2 + 1]) << 8)));
    }

    std::vector<BYTE> opusFrame(4000);
    int encoded = opus_encode(impl_->encoder,
                              samples.data(),
                              samplesPerFrame,
                              opusFrame.data(),
                              static_cast<opus_int32>(opusFrame.size()));
    if (encoded < 0) {
        error = L"Opus encode failed (" + std::to_wstring(encoded) + L")";
        return false;
    }
    opusFrame.resize(static_cast<size_t>(encoded));

    FrameState state = isLast
        ? FrameState::Last
        : (impl_->frameIndex == 0 ? FrameState::First : FrameState::Middle);

    AsrRequest request;
    request.serviceName = "ASR";
    request.methodName = "TaskRequest";
    request.requestId = impl_->requestId;
    request.payload = BuildAudioPayload(
        impl_->timestampBaseMs + static_cast<long long>(impl_->frameIndex) * impl_->cfg.frameMs);
    request.audioData = std::move(opusFrame);
    request.frameState = static_cast<int>(state);

    if (!SendBinaryMessage(impl_->conn.WebSocket(), EncodeRequest(request), error)) {
        return false;
    }
    ++impl_->frameIndex;
    return true;
}

bool RealtimeClient::SendFinishSession(std::wstring& error) {
    if (!impl_ || !impl_->connected.load()) {
        error = L"WebSocket not connected";
        return false;
    }
    AsrRequest finishSession;
    finishSession.token = WideToUtf8(impl_->cfg.token);
    finishSession.serviceName = "ASR";
    finishSession.methodName = "FinishSession";
    finishSession.requestId = impl_->requestId;
    return SendBinaryMessage(impl_->conn.WebSocket(), EncodeRequest(finishSession), error);
}

bool RealtimeClient::PollEvent(DWORD timeoutMs, RealtimeEvent& event, std::wstring& error) {
    event = {};
    if (!impl_) {
        error = L"client not initialized";
        return false;
    }

    AsrResponse response;
    bool timedOut = false;
    bool closed = false;
    if (!ReadResponse(impl_->conn, timeoutMs, response, timedOut, closed, error)) {
        return false;
    }
    if (timedOut) return true;
    if (closed) {
        event.sessionFinished = true;
        return true;
    }

    if (response.messageType == "TaskFailed" || response.messageType == "SessionFailed") {
        error = ResponseFailureText(response, L"ASR session failed");
        return false;
    }
    if (response.messageType == "SessionFinished") {
        event.sessionFinished = true;
    }
    if (!response.resultJson.empty()) {
        TextCandidate candidate = ExtractTextCandidate(response.resultJson);
        if (candidate.found) {
            if (candidate.isFinal) {
                event.finalText = candidate.text;
                event.transcriptionCompleted = true;
            } else {
                event.partialText = candidate.text;
            }
        }
    }
    return true;
}

bool RealtimeClient::Finish(DWORD finalTimeoutMs, std::wstring& finalText, std::wstring& error) {
    finalText.clear();
    if (!SendFinishSession(error)) return false;

    std::wstring finalAccumulator;
    std::wstring partialFallback;
    std::wstring lastFinalSegment;
    const ULONGLONG deadline = GetTickCount64() + std::max<DWORD>(finalTimeoutMs, 1000);
    while (GetTickCount64() < deadline) {
        RealtimeEvent ev;
        const DWORD slice = static_cast<DWORD>(std::min<ULONGLONG>(
            kReceiveSliceMs, deadline - GetTickCount64()));
        if (!PollEvent(std::max<DWORD>(slice, 1), ev, error)) {
            return false;
        }
        if (!ev.partialText.empty()) {
            partialFallback = ev.partialText;
            if (finalAccumulator.empty()) finalText = partialFallback;
        }
        if (ev.transcriptionCompleted && !ev.finalText.empty()) {
            if (ev.finalText != lastFinalSegment) {
                finalAccumulator = MergeRecognizedText(finalAccumulator, ev.finalText);
                lastFinalSegment = ev.finalText;
            }
            finalText = finalAccumulator;
        }
        if (ev.sessionFinished) return true;
    }
    if (!finalText.empty()) return true;
    error = L"timed out waiting for final transcript";
    return false;
}

void RealtimeClient::Abort() {
    if (!impl_) return;
    impl_->conn.Abort();
    impl_->connected.store(false);
}

void RealtimeClient::Close() {
    if (!impl_) return;
    impl_->conn.Close();
    impl_->connected.store(false);
    if (impl_->encoder) {
        opus_encoder_destroy(impl_->encoder);
        impl_->encoder = nullptr;
    }
}

bool RealtimeClient::CredentialsChanged() const {
    return impl_ && impl_->credentialsChanged;
}

Credentials RealtimeClient::CurrentCredentials() const {
    Credentials creds;
    if (!impl_) return creds;
    creds.deviceId = impl_->cfg.deviceId;
    creds.cdid = impl_->cfg.cdid;
    creds.token = impl_->cfg.token;
    return creds;
}

size_t RealtimeClient::FrameBytes() const {
    return impl_ ? FrameBytesForConfig(impl_->cfg) : 0;
}

bool RealtimeClient::HasSentAudio() const {
    return impl_ && impl_->frameIndex > 0;
}

bool IsAuthFailure(const std::wstring& error) {
    std::wstring lower = LowerCase(error);
    return lower.find(L"token") != std::wstring::npos ||
           lower.find(L"auth") != std::wstring::npos ||
           lower.find(L"401") != std::wstring::npos ||
           lower.find(L"403") != std::wstring::npos ||
           lower.find(L"unauthorized") != std::wstring::npos ||
           lower.find(L"forbidden") != std::wstring::npos ||
           lower.find(L"app_key") != std::wstring::npos ||
           lower.find(L"appkey") != std::wstring::npos ||
           lower.find(L"credential") != std::wstring::npos;
}

bool IsTransientFailure(const std::wstring& error) {
    std::wstring lower = LowerCase(error);
    return lower.find(L"timeout") != std::wstring::npos ||
           lower.find(L"timed out") != std::wstring::npos ||
           lower.find(L"connection") != std::wstring::npos ||
           lower.find(L"cannot connect") != std::wstring::npos ||
           lower.find(L"name not resolved") != std::wstring::npos ||
           lower.find(L"server closed websocket") != std::wstring::npos ||
           lower.find(L"http 408") != std::wstring::npos ||
           lower.find(L"http 409") != std::wstring::npos ||
           lower.find(L"http 425") != std::wstring::npos ||
           lower.find(L"http 429") != std::wstring::npos ||
           lower.find(L"http 500") != std::wstring::npos ||
           lower.find(L"http 502") != std::wstring::npos ||
           lower.find(L"http 503") != std::wstring::npos ||
           lower.find(L"http 504") != std::wstring::npos ||
           lower.find(L"err=12002") != std::wstring::npos ||
           lower.find(L"err=12007") != std::wstring::npos ||
           lower.find(L"err=12029") != std::wstring::npos ||
           lower.find(L"err=12030") != std::wstring::npos ||
           lower.find(L"err=12031") != std::wstring::npos ||
           lower.find(L"err=12032") != std::wstring::npos ||
           lower.find(L"err=12152") != std::wstring::npos;
}

size_t FrameBytesForConfig(const DoubaoImeConfig& cfg) {
    const int sampleRate = cfg.sampleRate > 0 ? cfg.sampleRate : 16000;
    const int channels = cfg.channels > 0 ? cfg.channels : 1;
    const int frameMs = cfg.frameMs > 0 ? cfg.frameMs : 20;
    return static_cast<size_t>(sampleRate) * static_cast<size_t>(channels) *
        static_cast<size_t>(frameMs) * sizeof(int16_t) / 1000u;
}

std::wstring ErrorText(const std::wstring& error) {
    if (error.rfind(L"Doubao IME ASR error:", 0) == 0) return error;
    return L"Doubao IME ASR error: " + (error.empty() ? L"unknown error" : error);
}

TestResult TestConnection(const DoubaoImeConfig& cfg) {
    TestResult result;
    DoubaoImeConfig attemptCfg = cfg;
    std::wstring lastError;
    bool refreshedCredentials = false;

    for (int attempt = 0; attempt < 3; ++attempt) {
        RealtimeClient client(attemptCfg);
        std::wstring error;
        bool ok = false;

        if (client.Connect(error)) {
            const size_t frameBytes = client.FrameBytes();
            if (frameBytes == 0) {
                error = L"invalid frame size";
            } else {
                std::vector<BYTE> silence(frameBytes, 0);
                std::wstring finalText;
                if (client.SendPcmFrame(silence.data(), silence.size(), true, error) &&
                    client.Finish(8000, finalText, error)) {
                    ok = true;
                }
            }
        }

        const bool authFailure = IsAuthFailure(error);
        if (client.CredentialsChanged() && !authFailure) {
            Credentials creds = client.CurrentCredentials();
            attemptCfg.deviceId = creds.deviceId;
            attemptCfg.cdid = creds.cdid;
            attemptCfg.token = creds.token;
            result.credentials = creds;
            result.credentialsChanged = true;
        }

        if (ok) {
            result.ok = true;
            result.message = L"Connection OK. Doubao IME realtime protocol is ready.";
            result.credentials = client.CurrentCredentials();
            result.credentialsChanged = result.credentialsChanged || client.CredentialsChanged();
            client.Close();
            return result;
        }

        client.Close();
        lastError = error;

        if (!refreshedCredentials && authFailure) {
            attemptCfg.deviceId.clear();
            attemptCfg.cdid.clear();
            attemptCfg.token.clear();
            result.credentials = {};
            result.credentialsChanged = true;
            refreshedCredentials = true;
            Sleep(300);
            continue;
        }

        if (attempt + 1 >= 3 || !IsTransientFailure(error)) {
            break;
        }
        Sleep(attempt == 0 ? 500 : 1000);
    }

    result.message = ErrorText(lastError.empty() ? L"connection failed" : lastError);
    return result;
}

} // namespace doubao_ime_asr
