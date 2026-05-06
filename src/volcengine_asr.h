#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

#define VOLC_DEBUG_LOG 1

#if VOLC_DEBUG_LOG
inline void VolcDebugLog(const char* fmt, ...) {
    static char logPath[MAX_PATH] = {};
    if (logPath[0] == '\0') {
        GetTempPathA(MAX_PATH, logPath);
        strcat_s(logPath, "volc_asr_debug.log");
    }
    FILE* f = nullptr;
    fopen_s(&f, logPath, "a");
    if (!f) return;
    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d.%03d] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

inline void VolcDebugHex(const char* label, const uint8_t* data, size_t len, size_t maxBytes = 64) {
    std::string hex;
    size_t n = (len < maxBytes) ? len : maxBytes;
    for (size_t i = 0; i < n; i++) {
        char buf[4];
        snprintf(buf, sizeof(buf), "%02X ", data[i]);
        hex += buf;
    }
    if (len > maxBytes) hex += "...";
    VolcDebugLog("%s (%zu bytes): %s", label, len, hex.c_str());
}
#else
#define VolcDebugLog(...) ((void)0)
#define VolcDebugHex(...) ((void)0)
#endif

#define VOLC_WEB_SOCKET_BINARY_MSG 0
#define VOLC_WEB_SOCKET_BINARY_FRAG 1

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif

#ifndef WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT
#define WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT 122
#endif

namespace volc_asr {

constexpr uint8_t MSG_FULL_CLIENT_REQ = 1;
constexpr uint8_t MSG_AUDIO_ONLY = 2;
constexpr uint8_t MSG_FULL_SERVER_RESP = 9;
constexpr uint8_t MSG_ERROR_RESP = 15;

constexpr uint8_t FLAG_NO_SEQ = 0;
constexpr uint8_t FLAG_POS_SEQ = 1;
constexpr uint8_t FLAG_NEG_PACKET = 2;
constexpr uint8_t FLAG_NEG_SEQ = 3;

constexpr uint8_t SER_NONE = 0;
constexpr uint8_t SER_JSON = 1;

constexpr uint8_t COMP_NONE = 0;
constexpr uint8_t COMP_GZIP = 1;

struct VolcConfig {
    std::wstring apiKey;
    std::wstring resourceId = L"volc.seedasr.sauc.duration";
    std::wstring mode = L"bigmodel";
    std::wstring language;
    bool enableItn = true;
    bool enablePunc = true;
};

struct VolcSession {
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hWebSocket = nullptr;
    int sequence = 0;
    std::wstring partialText;
    std::wstring lastError;
    volatile bool connected = false;
};

inline std::string WideToUtf8(const std::wstring& value) {
    if (value.empty()) return {};
    int required = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (required <= 0) return {};
    std::string result(static_cast<size_t>(required - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), required, nullptr, nullptr);
    return result;
}

inline std::wstring Utf8ToWide(const std::string& value) {
    if (value.empty()) return {};
    int required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    return result;
}

inline std::wstring ExtractJsonStr(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return L"";
    pos += search.size();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return L"";
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size()) return L"";
    if (json[pos] != '"') return L"";
    pos++;
    size_t start = pos;
    while (pos < json.size()) {
        if (json[pos] == '"' && (pos == 0 || json[pos - 1] != '\\')) break;
        if (json[pos] == '\\' && pos + 1 < json.size()) pos++;
        pos++;
    }
    VolcDebugLog("ExtractJsonStr: key='%s' raw_bytes=%zu result_wchars=%zu",
                 key.c_str(), pos - start, Utf8ToWide(json.substr(start, pos - start)).size());
    return Utf8ToWide(json.substr(start, pos - start));
}

inline std::wstring GenerateUuidStr() {
    wchar_t buf[48] = {};
    ULONGLONG ticks = GetTickCount64();
    DWORD r0 = (ticks >> 32) ^ (static_cast<DWORD>(ticks) << 2) ^ GetCurrentProcessId();
    DWORD r1 = GetCurrentThreadId() ^ (static_cast<DWORD>(ticks ^ (ticks >> 21)));
    DWORD r2 = static_cast<DWORD>(reinterpret_cast<ULONG_PTR>(&buf));
    swprintf_s(buf, 48, L"%08lx-%04x-%04x-%04x-%08lx%04x",
               r0, static_cast<unsigned int>(r1 & 0xFFFF),
               static_cast<unsigned int>((r1 >> 16) & 0xFFFF),
               static_cast<unsigned int>(r2 & 0xFFFF),
               static_cast<unsigned long>(r2 >> 16),
               static_cast<unsigned int>(ticks & 0xFFFF));
    return std::wstring(buf);
}

inline std::string ToBackslashEscape(const std::string& src) {
    std::string out;
    for (char c : src) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

inline std::vector<BYTE> BuildFrame(uint8_t msgType, uint8_t flags,
                                     uint8_t serialization, uint8_t compression,
                                     uint32_t seq, const std::vector<BYTE>& payload) {
    std::vector<BYTE> frame;
    frame.reserve(12 + payload.size());

    frame.push_back(0x11);

    frame.push_back(((msgType & 0x0F) << 4) | (flags & 0x0F));

    frame.push_back(((serialization & 0x0F) << 4) | (compression & 0x0F));

    frame.push_back(0x00);

    if (flags == FLAG_POS_SEQ || flags == FLAG_NEG_SEQ) {
        frame.push_back(static_cast<BYTE>((seq >> 24) & 0xFF));
        frame.push_back(static_cast<BYTE>((seq >> 16) & 0xFF));
        frame.push_back(static_cast<BYTE>((seq >> 8) & 0xFF));
        frame.push_back(static_cast<BYTE>(seq & 0xFF));
    }

    uint32_t payloadSize = static_cast<uint32_t>(payload.size());
    frame.push_back(static_cast<BYTE>((payloadSize >> 24) & 0xFF));
    frame.push_back(static_cast<BYTE>((payloadSize >> 16) & 0xFF));
    frame.push_back(static_cast<BYTE>((payloadSize >> 8) & 0xFF));
    frame.push_back(static_cast<BYTE>(payloadSize & 0xFF));

    if (!payload.empty()) {
        frame.insert(frame.end(), payload.begin(), payload.end());
    }
    return frame;
}

inline std::wstring TrimWhitespace(const std::wstring& s) {
    size_t start = 0;
    while (start < s.size() && (s[start] == L' ' || s[start] == L'\t' || s[start] == L'\r' || s[start] == L'\n')) start++;
    size_t end = s.size();
    while (end > start && (s[end - 1] == L' ' || s[end - 1] == L'\t' || s[end - 1] == L'\r' || s[end - 1] == L'\n')) end--;
    return s.substr(start, end - start);
}

inline std::wstring ReceiveResult(HINTERNET hWebSocket, DWORD timeoutMs) {
    std::vector<BYTE> recvBuf(65536);
    std::string responseBody;

    VolcDebugLog("ReceiveResult: waiting (timeout=%ums)...", timeoutMs);

    DWORD actualTimeout = timeoutMs;
    WinHttpSetOption(hWebSocket, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &actualTimeout, sizeof(actualTimeout));

    DWORD bytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType = static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG);
    DWORD err = WinHttpWebSocketReceive(hWebSocket, recvBuf.data(),
                                        static_cast<DWORD>(recvBuf.size()),
                                        &bytesRead, &bufType);

    VolcDebugLog("ReceiveResult: err=%u, bytesRead=%u, bufType=%d", err, bytesRead, (int)bufType);

    if (err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_OPERATION_CANCELLED) return L"";
    if (err != ERROR_SUCCESS) return L"";
    if (bytesRead == 0) return L"";

    VolcDebugHex("Recv raw", recvBuf.data(), bytesRead);

    responseBody.append(reinterpret_cast<char*>(recvBuf.data()), bytesRead);

    if (bufType == VOLC_WEB_SOCKET_BINARY_FRAG) {
        for (int i = 0; i < 64; i++) {
            DWORD moreBytes = 0;
            err = WinHttpWebSocketReceive(hWebSocket, recvBuf.data(),
                                          static_cast<DWORD>(recvBuf.size()),
                                          &moreBytes, &bufType);
            if (err != ERROR_SUCCESS) break;
            if (moreBytes == 0) break;
            responseBody.append(reinterpret_cast<char*>(recvBuf.data()), moreBytes);
            if (bufType != VOLC_WEB_SOCKET_BINARY_FRAG) break;
        }
    }

    if (responseBody.size() < 4) return L"";

    std::wstring resultText;
    size_t pos = 0;
    while (pos + 4 <= responseBody.size()) {
        uint8_t b0 = static_cast<uint8_t>(responseBody[pos + 0]);
        uint8_t b1 = static_cast<uint8_t>(responseBody[pos + 1]);
        uint8_t b2 = static_cast<uint8_t>(responseBody[pos + 2]);
        uint8_t headerSize = b0 & 0x0F;
        uint8_t msgType = (b1 >> 4) & 0x0F;
        uint8_t msgFlags = b1 & 0x0F;
        uint8_t ser = (b2 >> 4) & 0x0F;
        uint8_t comp = b2 & 0x0F;
        VolcDebugLog("Frame@%zu: b0=0x%02X b1=0x%02X b2=0x%02X hdrSz=%u msgType=%u flags=%u ser=%u comp=%u",
                     pos, b0, b1, b2, headerSize, msgType, msgFlags, ser, comp);
        pos += 4;

        size_t headerBytes = static_cast<size_t>(headerSize) * 4;
        if (headerBytes > 4 && pos + (headerBytes - 4) <= responseBody.size()) {
            pos += (headerBytes - 4);
        }

        bool hasSeq = (msgFlags == FLAG_POS_SEQ || msgFlags == FLAG_NEG_SEQ);
        if (hasSeq && pos + 4 <= responseBody.size()) {
            pos += 4;
        }

        if (pos + 4 > responseBody.size()) break;
        uint32_t payloadSize = (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos])) << 24) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos + 1])) << 16) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos + 2])) << 8) |
                               (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos + 3])));
        pos += 4;

        if (msgType == MSG_ERROR_RESP) {
            uint32_t errorCode = payloadSize;
            VolcDebugLog("Error frame: code=%u", errorCode);
            if (pos + 4 > responseBody.size()) {
                return L"[VolcEngine error: code=" + std::to_wstring(errorCode) + L"]";
            }
            uint32_t errMsgSize = (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos])) << 24) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos + 1])) << 16) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos + 2])) << 8) |
                                  (static_cast<uint32_t>(static_cast<uint8_t>(responseBody[pos + 3])));
            pos += 4;
            VolcDebugLog("Error msg size=%u", errMsgSize);
            if (errMsgSize > 0 && pos + errMsgSize <= responseBody.size()) {
                std::string errMsg(responseBody.begin() + static_cast<ptrdiff_t>(pos),
                                   responseBody.begin() + static_cast<ptrdiff_t>(pos + errMsgSize));
                VolcDebugLog("Error msg: %s", errMsg.c_str());
                return L"[VolcEngine error: " + Utf8ToWide(errMsg) + L"]";
            }
            return L"[VolcEngine error: code=" + std::to_wstring(errorCode) + L"]";
        }

        if (payloadSize == 0) {
            if (msgType == MSG_FULL_SERVER_RESP) break;
            continue;
        }
        if (pos + payloadSize > responseBody.size()) break;

        if (msgType == MSG_FULL_SERVER_RESP) {
            std::string payloadJson(responseBody.begin() + static_cast<ptrdiff_t>(pos),
                                    responseBody.begin() + static_cast<ptrdiff_t>(pos + payloadSize));
            VolcDebugLog("Server resp payload (%u bytes): %.200s", payloadSize, payloadJson.c_str());
            std::wstring text = ExtractJsonStr(payloadJson, "text");
            if (text.empty()) {
                size_t rp = payloadJson.find("\"result\"");
                if (rp != std::string::npos) {
                    text = ExtractJsonStr(payloadJson.substr(rp), "text");
                }
            }
            VolcDebugLog("Extracted text: wlen=%zu", text.size());
            if (!text.empty() && text.size() > resultText.size()) {
                resultText = text;
            }
        }
        pos += static_cast<size_t>(payloadSize);
    }

    return resultText;
}

inline void WebSocketCloseGracefully(HINTERNET hWebSocket) {
    if (!hWebSocket) return;
    DWORD recvTimeout = 3000;
    WinHttpSetOption(hWebSocket, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &recvTimeout, sizeof(recvTimeout));
    WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
    BYTE closeBuf[128];
    DWORD closeBytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE closeBufType = static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG);
    WinHttpWebSocketReceive(hWebSocket, closeBuf, sizeof(closeBuf), &closeBytesRead, &closeBufType);
    WinHttpCloseHandle(hWebSocket);
}

inline bool OpenSession(VolcSession& sess, const VolcConfig& cfg) {
    VolcDebugLog("=== OpenSession START ===");
    std::wstring cleanKey = TrimWhitespace(cfg.apiKey);
    if (cleanKey.empty()) return false;

    sess.hSession = WinHttpOpen(L"VoxType/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess.hSession) return false;
    WinHttpSetTimeouts(sess.hSession, 10000, 10000, 15000, 15000);

    sess.hConnect = WinHttpConnect(sess.hSession, L"openspeech.bytedance.com",
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!sess.hConnect) return false;

    std::wstring path = L"/api/v3/sauc/" + cfg.mode;
    DWORD flags = WINHTTP_FLAG_SECURE;
    HINTERNET hReq = WinHttpOpenRequest(sess.hConnect, L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hReq) return false;

    WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);

    DWORD closeTimeout = 5000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
        &closeTimeout, sizeof(closeTimeout));
    DWORD keepAlive = 15000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
        &keepAlive, sizeof(keepAlive));

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    std::wstring uuid = GenerateUuidStr();
    std::wstring headers =
        L"X-Api-Key: " + cleanKey +
        L"\r\nX-Api-Resource-Id: " + cfg.resourceId +
        L"\r\nX-Api-Request-Id: " + uuid +
        L"\r\nX-Api-Connect-Id: " + uuid +
        L"\r\nX-Api-Sequence: -1\r\n";

    if (!WinHttpAddRequestHeaders(hReq, headers.c_str(),
        static_cast<DWORD>(wcslen(headers.c_str())),
        WINHTTP_ADDREQ_FLAG_ADD)) {
        WinHttpCloseHandle(hReq);
        return false;
    }

    WinHttpSetTimeouts(hReq, 10000, 10000, 10000, 10000);

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        WinHttpCloseHandle(hReq);
        return false;
    }

    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        WinHttpCloseHandle(hReq);
        return false;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
                        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 101) {
        WinHttpCloseHandle(hReq);
        return false;
    }

    sess.hWebSocket = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    WinHttpCloseHandle(hReq);

    if (!sess.hWebSocket) return false;

    DWORD recvTimeout = 2000;
    WinHttpSetOption(sess.hWebSocket, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &recvTimeout, sizeof(recvTimeout));

    sess.sequence = 0;

    std::string uid = ToBackslashEscape(WideToUtf8(cleanKey.substr(0, (std::min)(size_t(12), cleanKey.size()))));

    std::string requestJson = "{"
        "\"user\":{\"uid\":\"" + uid + "\"},"
        "\"audio\":{"
            "\"format\":\"pcm\","
            "\"rate\":16000,"
            "\"bits\":16,"
            "\"channel\":1,"
            "\"codec\":\"raw\"";

    if (!cfg.language.empty()) {
        requestJson += ",\"language\":\"" + WideToUtf8(cfg.language) + "\"";
    }

    requestJson += "},"
        "\"request\":{"
            "\"model_name\":\"bigmodel\","
            "\"enable_itn\":" + std::string(cfg.enableItn ? "true" : "false") + ","
            "\"enable_punc\":" + std::string(cfg.enablePunc ? "true" : "false") + ","
            "\"enable_ddc\":false,"
            "\"show_utterances\":false,"
            "\"result_type\":\"full\""
        "}"
    "}";

    std::vector<BYTE> jsonPayload(requestJson.begin(), requestJson.end());
    std::vector<BYTE> frame = BuildFrame(MSG_FULL_CLIENT_REQ, FLAG_NO_SEQ,
                                         SER_JSON, COMP_NONE, 0, jsonPayload);

    VolcDebugLog("Sending init frame (%zu bytes), json=%zu bytes", frame.size(), jsonPayload.size());
    VolcDebugHex("Init frame", frame.data(), frame.size());
    VolcDebugLog("Init JSON: %.300s", requestJson.c_str());

    WinHttpWebSocketSend(sess.hWebSocket,
                         static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG),
                         frame.data(), static_cast<DWORD>(frame.size()));

    std::wstring initResp = ReceiveResult(sess.hWebSocket, 5000);
    VolcDebugLog("Init response: '%ls'", initResp.c_str());
    if (initResp.find(L"[VolcEngine error:") != std::wstring::npos) {
        VolcDebugLog("OpenSession FAILED - server error: %ls", initResp.c_str());
        sess.lastError = initResp;
        WebSocketCloseGracefully(sess.hWebSocket);
        sess.hWebSocket = nullptr;
        return false;
    }

    VolcDebugLog("=== OpenSession OK ===");
    sess.sequence = 2;
    return true;
}

inline std::wstring SendAudio(VolcSession& sess, const std::vector<BYTE>& pcmChunk, bool isLast, bool asyncMode = false) {
    if (!sess.hWebSocket || !sess.connected) return L"";

    uint8_t flags = isLast ? FLAG_NEG_PACKET : FLAG_POS_SEQ;
    uint32_t seq = isLast ? 0 : static_cast<uint32_t>(sess.sequence);
    std::vector<BYTE> frame = BuildFrame(MSG_AUDIO_ONLY, flags,
                                         SER_NONE, COMP_NONE, seq, pcmChunk);

    VolcDebugLog("SendAudio: %zu bytes, isLast=%d, seq=%u, frame=%zu bytes, async=%d",
                 pcmChunk.size(), isLast, seq, frame.size(), asyncMode);
    if (!pcmChunk.empty()) {
        VolcDebugHex("Audio frame hdr", frame.data(), (std::min)(frame.size(), size_t(16)));
    }

    DWORD err = WinHttpWebSocketSend(sess.hWebSocket,
                                     static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG),
                                     frame.data(), static_cast<DWORD>(frame.size()));

    if (err != ERROR_SUCCESS) {
        VolcDebugLog("SendAudio FAILED: err=%u", err);
        return L"";
    }

    if (!isLast) {
        sess.sequence++;
    }

    if (asyncMode && !isLast) {
        return L"";
    }

    std::wstring result = ReceiveResult(sess.hWebSocket, isLast ? 4000 : 150);
    VolcDebugLog("SendAudio result: '%ls'", result.c_str());
    return result;
}

inline std::wstring DrainReceiveBuffer(HINTERNET hWebSocket) {
    return ReceiveResult(hWebSocket, 1);
}

inline std::wstring CloseSession(VolcSession& sess) {
    VolcDebugLog("=== CloseSession ===");
    if (!sess.hWebSocket) return L"";

    std::wstring finalText;

    for (int retry = 0; retry < 6; retry++) {
        std::wstring chunk = ReceiveResult(sess.hWebSocket, 500);
        if (!chunk.empty()) {
            finalText = chunk;
        } else {
            break;
        }
    }

    WebSocketCloseGracefully(sess.hWebSocket);
    sess.hWebSocket = nullptr;

    if (sess.hConnect) {
        WinHttpCloseHandle(sess.hConnect);
        sess.hConnect = nullptr;
    }
    if (sess.hSession) {
        WinHttpCloseHandle(sess.hSession);
        sess.hSession = nullptr;
    }

    sess.connected = false;
    VolcDebugLog("=== CloseSession DONE, finalText='%ls' ===", finalText.c_str());
    return finalText;
}

struct TestResult {
    bool ok = false;
    std::wstring message;
};

inline std::string ReadResponseBody(HINTERNET hReq) {
    std::string body;
    DWORD bytesAvail = 0;
    BYTE buf[4096];
    DWORD bytesRead = 0;
    while (WinHttpQueryDataAvailable(hReq, &bytesAvail) && bytesAvail > 0) {
        DWORD toRead = bytesAvail > sizeof(buf) ? sizeof(buf) : bytesAvail;
        if (WinHttpReadData(hReq, buf, toRead, &bytesRead)) {
            body.append(reinterpret_cast<char*>(buf), bytesRead);
        } else {
            break;
        }
    }
    return body;
}

inline std::wstring ReadResponseHeaderStr(HINTERNET hReq, DWORD dwHeader) {
    DWORD size = 0;
    WinHttpQueryHeaders(hReq, dwHeader, WINHTTP_HEADER_NAME_BY_INDEX,
                        WINHTTP_NO_OUTPUT_BUFFER, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) return L"";
    std::wstring result(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(hReq, dwHeader, WINHTTP_HEADER_NAME_BY_INDEX,
                             result.data(), &size, WINHTTP_NO_HEADER_INDEX)) return L"";
    result.resize(size / sizeof(wchar_t));
    return result;
}

inline TestResult TestConnection(const VolcConfig& cfg) {
    TestResult res;
    std::wstring cleanKey = TrimWhitespace(cfg.apiKey);
    if (cleanKey.empty()) {
        res.message = L"Please fill in API Key (X-Api-Key).\n\n"
            L"Get key at: https://console.volcengine.com/speech/app";
        return res;
    }

    HINTERNET hSession = WinHttpOpen(L"VoxType/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) {
        res.message = L"WinHttpOpen failed.";
        return res;
    }
    WinHttpSetTimeouts(hSession, 8000, 8000, 10000, 10000);

    HINTERNET hConnect = WinHttpConnect(hSession, L"openspeech.bytedance.com",
                                        INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpConnect failed.";
        return res;
    }

    std::wstring path = L"/api/v3/sauc/" + cfg.mode;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpOpenRequest failed.";
        return res;
    }

    if (!WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed (err=" + std::to_wstring(err) +
            L").\n\nThis Windows version may not support WinHTTP WebSocket.\n"
            L"Requires Windows 8+.";
        return res;
    }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
    WinHttpSetTimeouts(hReq, 8000, 8000, 8000, 8000);

    DWORD closeTimeout = 5000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT,
        &closeTimeout, sizeof(closeTimeout));
    DWORD keepAlive = 15000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL,
        &keepAlive, sizeof(keepAlive));

    std::wstring uuid = GenerateUuidStr();
    std::wstring headers =
        L"X-Api-Key: " + cleanKey +
        L"\r\nX-Api-Resource-Id: " + cfg.resourceId +
        L"\r\nX-Api-Request-Id: " + uuid +
        L"\r\nX-Api-Connect-Id: " + uuid +
        L"\r\nX-Api-Sequence: -1\r\n";

    DWORD headerLen = static_cast<DWORD>(wcslen(headers.c_str()));
    if (!WinHttpAddRequestHeaders(hReq, headers.c_str(), headerLen, WINHTTP_ADDREQ_FLAG_ADD)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpAddRequestHeaders failed (err=" + std::to_wstring(err) + L").";
        return res;
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpSendRequest failed (err=" + std::to_wstring(err) + L").\n"
            L"Check network connectivity to openspeech.bytedance.com.";
        return res;
    }

    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        DWORD err = GetLastError();
        WinHttpCloseHandle(hReq);
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        res.message = L"WinHttpReceiveResponse failed (err=" + std::to_wstring(err) + L").";
        return res;
    }

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
                        WINHTTP_NO_HEADER_INDEX);

    std::wstring ttLogId = ReadResponseHeaderStr(hReq, WINHTTP_QUERY_CUSTOM);
    if (ttLogId.empty()) {
        std::wstring allHeaders;
        DWORD allSize = 0;
        WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                            WINHTTP_HEADER_NAME_BY_INDEX,
                            WINHTTP_NO_OUTPUT_BUFFER, &allSize, WINHTTP_NO_HEADER_INDEX);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && allSize > 0) {
            allHeaders.resize(allSize / sizeof(wchar_t));
            WinHttpQueryHeaders(hReq, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                                WINHTTP_HEADER_NAME_BY_INDEX,
                                allHeaders.data(), &allSize, WINHTTP_NO_HEADER_INDEX);
            allHeaders.resize(allSize / sizeof(wchar_t));
            if (allHeaders.find(L"X-Tt-Logid:") != std::wstring::npos) {
                size_t p = allHeaders.find(L"X-Tt-Logid:");
                size_t e = allHeaders.find(L"\r\n", p);
                if (e != std::wstring::npos) ttLogId = allHeaders.substr(p, e - p);
            }
        }
    }

    HINTERNET hWebSocket = nullptr;
    if (statusCode == 101) {
        hWebSocket = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    }

    std::string respBody;
    std::wstring respBodyW;
    if (statusCode != 101 || !hWebSocket) {
        respBody = ReadResponseBody(hReq);
        if (!respBody.empty()) respBodyW = Utf8ToWide(respBody);
    }
    WinHttpCloseHandle(hReq);

    std::wstring debugInfo;
    debugInfo += L"URL: wss://openspeech.bytedance.com/api/v3/sauc/" + cfg.mode + L"\n";
    debugInfo += L"API Key len=" + std::to_wstring(cleanKey.size()) +
        L" preview=" + (cleanKey.size() > 8 ? cleanKey.substr(0, 8) + L"..." : cleanKey) + L"\n";
    debugInfo += L"Resource: " + cfg.resourceId + L"\n";
    debugInfo += L"Client UUID: " + uuid + L"\n";
    if (!ttLogId.empty()) debugInfo += L"Server: " + ttLogId + L"\n";

    if (hWebSocket) {
        WinHttpWebSocketClose(hWebSocket, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
        WinHttpCloseHandle(hWebSocket);
        res.ok = true;
        res.message = L"Connection OK. WebSocket upgrade successful.\n\n" + debugInfo;
    } else if (statusCode == 401 || statusCode == 403) {
        res.message = L"Authentication failed (HTTP " + std::to_wstring(statusCode) +
            L").\n\n" + debugInfo +
            L"\nServer response: " + (respBodyW.empty() ? L"(empty)" : respBodyW) +
            L"\n\nNew console: use X-Api-Key (App Key from console).\n"
            L"Old console: need both X-Api-App-Key and X-Api-Access-Key.\n"
            L"Get key at: https://console.volcengine.com/speech/app";
    } else if (statusCode == 429) {
        res.message = L"Rate limited (HTTP 429). Try again later.\n\n" + debugInfo;
    } else if (statusCode == 400) {
        res.message = L"Bad Request (HTTP 400).\n\n" + debugInfo +
            L"\nServer response: " + (respBodyW.empty() ? L"(empty)" : respBodyW) +
            L"\n\nCommon causes:\n"
            L"  - Key contains whitespace (auto-trimmed, len=" + std::to_wstring(cleanKey.size()) + L")\n"
            L"  - Wrong Resource ID for your plan\n"
            L"  - API Key not activated for ASR service\n"
            L"  - Key from wrong region/console";
    } else {
        res.message = L"WebSocket upgrade failed (HTTP " +
            (statusCode > 0 ? std::to_wstring(statusCode) : L"unknown") +
            L").\n\n" + debugInfo +
            L"\nServer response: " + (respBodyW.empty() ? L"(empty)" : respBodyW);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return res;
}

} // namespace volc_asr
