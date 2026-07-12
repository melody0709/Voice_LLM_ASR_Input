#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "asr_runtime_log.h"
#include "utils.h"

#pragma comment(lib, "winhttp.lib")

#define VOLC_DEBUG_LOG 1

#if VOLC_DEBUG_LOG
inline void VolcDebugLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    asr_runtime_log::WriteNamedV(L"volc_asr_debug.log", fmt, args);
    va_end(args);
}
#else
#define VolcDebugLog(...) ((void)0)
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

extern std::atomic<bool> g_volcKeepAlive;

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
    bool enableNonstream = false;
    int endWindowSize = 800;
    bool enableDdc = false;
    bool enableMusicFc = false;
    bool enablePoiFc = false;
    int forceToSpeechTime = 0;
    std::wstring extraParams;
    std::wstring contextJson;
    std::wstring hotwordsId;
    std::wstring hotwordsName;
    std::wstring correctTableId;
    std::wstring correctTableName;
};

struct VolcSession {
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hWebSocket = nullptr;
    std::atomic<HINTERNET> activeReq{nullptr};  // tracks hReq during OpenSessionImpl for fast Abort
    int sequence = 0;
    std::wstring partialText;
    std::wstring lastError;
    std::atomic<bool> connected{false};
    std::atomic<bool> forceAbort{false};
    ULONGLONG lastUsedTick = 0;
};

struct VolcResult {
    std::wstring text;
    bool definite = false;
};

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

inline bool ExtractJsonBool(const std::string& json, const std::string& key) {
    std::string search = "\"" + key + "\"";
    size_t pos = json.find(search);
    if (pos == std::string::npos) return false;
    pos += search.size();
    pos = json.find(':', pos);
    if (pos == std::string::npos) return false;
    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) pos++;
    if (pos >= json.size()) return false;
    return (json[pos] == 't' || json[pos] == '1');
}

inline VolcResult ReceiveResult(HINTERNET hWebSocket, DWORD timeoutMs, VolcSession* sess = nullptr) {
    VolcResult result;
    if (sess && sess->forceAbort.load()) {
        VolcDebugLog("ReceiveResult: aborted (forceAbort)");
        return result;
    }
    std::vector<BYTE> recvBuf(65536);
    std::string responseBody;

    if (timeoutMs > 1500) {
        VolcDebugLog("ReceiveResult: waiting (timeout=%ums)...", timeoutMs);
    }

    DWORD actualTimeout = timeoutMs;
    WinHttpSetOption(hWebSocket, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &actualTimeout, sizeof(actualTimeout));

    DWORD bytesRead = 0;
    WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType = static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG);
    DWORD err = WinHttpWebSocketReceive(hWebSocket, recvBuf.data(),
                                        static_cast<DWORD>(recvBuf.size()),
                                        &bytesRead, &bufType);

    if (err == ERROR_WINHTTP_TIMEOUT || err == ERROR_WINHTTP_OPERATION_CANCELLED) {
        VolcDebugLog("ReceiveResult: timeout/cancelled (err=%u)", err);
        return result;
    }
    if (err != ERROR_SUCCESS) {
        static std::atomic<DWORD> s_lastRecvError{0};
        if (err != s_lastRecvError.load()) {
            VolcDebugLog("ReceiveResult: error %u (suppressing repeats)", err);
            s_lastRecvError.store(err);
        }
        if (sess) sess->connected = false;
        return result;
    }
    if (bytesRead == 0) {
        VolcDebugLog("ReceiveResult: 0 bytes (close frame)");
        if (sess) sess->connected = false;
        return result;
    }

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

    if (responseBody.size() < 4) return result;

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
                result.text = L"[VolcEngine error: code=" + std::to_wstring(errorCode) + L"]";
                return result;
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
                result.text = L"[VolcEngine error: " + Utf8ToWide(errMsg) + L"]";
                return result;
            }
            result.text = L"[VolcEngine error: code=" + std::to_wstring(errorCode) + L"]";
            return result;
        }

        if (payloadSize == 0) {
            if (msgType == MSG_FULL_SERVER_RESP) break;
            continue;
        }
        if (pos + payloadSize > responseBody.size()) break;

        if (msgType == MSG_FULL_SERVER_RESP) {
            std::string payloadJson(responseBody.begin() + static_cast<ptrdiff_t>(pos),
                                    responseBody.begin() + static_cast<ptrdiff_t>(pos + payloadSize));
            std::wstring text = ExtractJsonStr(payloadJson, "text");
            if (!text.empty()) {
                VolcDebugLog("ExtractJsonStr: key='text' result_wchars=%zu", text.size());
            }
            if (text.empty()) {
                size_t rp = payloadJson.find("\"result\"");
                if (rp != std::string::npos) {
                    text = ExtractJsonStr(payloadJson.substr(rp), "text");
                    if (!text.empty()) {
                        VolcDebugLog("ExtractJsonStr (result): key='text' result_wchars=%zu", text.size());
                    }
                }
            }
            bool isDefinite = ExtractJsonBool(payloadJson, "definite");
            if (!text.empty()) {
                VolcDebugLog("Server resp: text_wlen=%zu definite=%d payload_bytes=%zu",
                             text.size(), isDefinite, payloadJson.size());
                if (isDefinite) {
                    result.text = text;
                    result.definite = true;
                } else if (text.size() > result.text.size() && !result.definite) {
                    result.text = text;
                }
            }
        }
        pos += static_cast<size_t>(payloadSize);
    }

    return result;
}

inline void WebSocketCloseGracefully(HINTERNET hWebSocket, VolcSession* sess = nullptr) {
    if (!hWebSocket) return;
    if (sess && sess->forceAbort.load()) {
        VolcDebugLog("WebSocketCloseGracefully: forceAbort, skipping close handshake");
        WinHttpCloseHandle(hWebSocket);
        return;
    }
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

inline bool EnsureConnection(VolcSession& sess) {
    if (sess.hSession && sess.hConnect) {
        if (sess.lastUsedTick > 0 && GetTickCount64() - sess.lastUsedTick > 300000) {
            VolcDebugLog("EnsureConnection: connection expired (%llums old), rebuilding",
                         GetTickCount64() - sess.lastUsedTick);
            WinHttpCloseHandle(sess.hConnect);
            sess.hConnect = nullptr;
            WinHttpCloseHandle(sess.hSession);
            sess.hSession = nullptr;
        } else {
            VolcDebugLog("EnsureConnection: reusing existing connection (%llums old)",
                         GetTickCount64() - sess.lastUsedTick);
            sess.lastUsedTick = GetTickCount64();
            return true;
        }
    }

    if (!sess.hSession) {
        sess.hSession = WinHttpOpen(L"VoxType/1.0",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS, 0);
        if (!sess.hSession) return false;
        WinHttpSetTimeouts(sess.hSession, 3000, 3000, 5000, 5000);
    }

    ULONGLONG t1 = GetTickCount64();
    sess.hConnect = WinHttpConnect(sess.hSession, L"openspeech.bytedance.com",
                                   INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!sess.hConnect) return false;
    ULONGLONG t2 = GetTickCount64();
    VolcDebugLog("WinHttpConnect: %llums", t2 - t1);
    sess.lastUsedTick = GetTickCount64();
    return true;
}

inline bool RebuildConnection(VolcSession& sess) {
    VolcDebugLog("RebuildConnection: rebuilding hConnect+hSession");
    if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
    if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
    return EnsureConnection(sess);
}

struct VolcWinHttpTraceContext {
    int id = 0;
    ULONGLONG startTick = 0;
    DWORD hardTimeoutMs = 0;
    std::atomic<DWORD> lastStatus{0};
    std::atomic<ULONGLONG> lastStatusTick{0};
    std::atomic<DWORD> lastActiveStatus{0};
    std::atomic<ULONGLONG> lastActiveStatusTick{0};
};

inline const char* VolcWinHttpStatusName(DWORD status) {
    switch (status) {
    case WINHTTP_CALLBACK_STATUS_RESOLVING_NAME: return "RESOLVING_NAME";
    case WINHTTP_CALLBACK_STATUS_NAME_RESOLVED: return "NAME_RESOLVED";
    case WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER: return "CONNECTING_TO_SERVER";
    case WINHTTP_CALLBACK_STATUS_CONNECTED_TO_SERVER: return "CONNECTED_TO_SERVER";
    case WINHTTP_CALLBACK_STATUS_SENDING_REQUEST: return "SENDING_REQUEST";
    case WINHTTP_CALLBACK_STATUS_REQUEST_SENT: return "REQUEST_SENT";
    case WINHTTP_CALLBACK_STATUS_RECEIVING_RESPONSE: return "RECEIVING_RESPONSE";
    case WINHTTP_CALLBACK_STATUS_RESPONSE_RECEIVED: return "RESPONSE_RECEIVED";
    case WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION: return "CLOSING_CONNECTION";
    case WINHTTP_CALLBACK_STATUS_CONNECTION_CLOSED: return "CONNECTION_CLOSED";
    case WINHTTP_CALLBACK_STATUS_HANDLE_CREATED: return "HANDLE_CREATED";
    case WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING: return "HANDLE_CLOSING";
    case WINHTTP_CALLBACK_STATUS_DETECTING_PROXY: return "DETECTING_PROXY";
    case WINHTTP_CALLBACK_STATUS_REDIRECT: return "REDIRECT";
    case WINHTTP_CALLBACK_STATUS_INTERMEDIATE_RESPONSE: return "INTERMEDIATE_RESPONSE";
    case WINHTTP_CALLBACK_STATUS_SECURE_FAILURE: return "SECURE_FAILURE";
    case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE: return "HEADERS_AVAILABLE";
    case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: return "DATA_AVAILABLE";
    case WINHTTP_CALLBACK_STATUS_READ_COMPLETE: return "READ_COMPLETE";
    case WINHTTP_CALLBACK_STATUS_WRITE_COMPLETE: return "WRITE_COMPLETE";
    case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: return "REQUEST_ERROR";
    case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE: return "SENDREQUEST_COMPLETE";
    default: return "UNKNOWN";
    }
}

inline void CALLBACK VolcWinHttpStatusCallback(HINTERNET,
                                               DWORD_PTR context,
                                               DWORD status,
                                               LPVOID statusInfo,
                                               DWORD statusInfoLen) {
    auto* trace = reinterpret_cast<VolcWinHttpTraceContext*>(context);
    if (!trace) return;
    const ULONGLONG now = GetTickCount64();
    trace->lastStatus.store(status);
    trace->lastStatusTick.store(now);
    if (status != WINHTTP_CALLBACK_STATUS_HANDLE_CREATED &&
        status != WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING &&
        status != WINHTTP_CALLBACK_STATUS_CLOSING_CONNECTION &&
        status != WINHTTP_CALLBACK_STATUS_CONNECTION_CLOSED) {
        trace->lastActiveStatus.store(status);
        trace->lastActiveStatusTick.store(now);
    }

    const ULONGLONG elapsed = trace->startTick > 0 ? now - trace->startTick : 0;
    if (status == WINHTTP_CALLBACK_STATUS_REQUEST_ERROR && statusInfo &&
        statusInfoLen >= sizeof(WINHTTP_ASYNC_RESULT)) {
        const auto* asyncResult = static_cast<const WINHTTP_ASYNC_RESULT*>(statusInfo);
        VolcDebugLog("OpenSession WinHTTP trace #%d: %s elapsed=%llums api=%p err=%u",
                     trace->id, VolcWinHttpStatusName(status), elapsed,
                     asyncResult->dwResult, asyncResult->dwError);
        return;
    }
    if (status == WINHTTP_CALLBACK_STATUS_SECURE_FAILURE && statusInfo &&
        statusInfoLen >= sizeof(DWORD)) {
        const DWORD flags = *static_cast<const DWORD*>(statusInfo);
        VolcDebugLog("OpenSession WinHTTP trace #%d: %s elapsed=%llums flags=0x%08X",
                     trace->id, VolcWinHttpStatusName(status), elapsed, flags);
        return;
    }

    VolcDebugLog("OpenSession WinHTTP trace #%d: %s elapsed=%llums",
                 trace->id, VolcWinHttpStatusName(status), elapsed);
}

inline int NextVolcWinHttpTraceId() {
    static std::atomic<int> s_nextTraceId{1};
    return s_nextTraceId.fetch_add(1);
}

void VolcMaybeLogConnectDiagnosticsAsync(int triggerTraceId, const char* reason, DWORD cooldownMs = 15000);

inline bool OpenSessionImpl(VolcSession& sess, const VolcConfig& cfg, bool isRetry, DWORD hardTimeoutMs) {
    if (hardTimeoutMs < 1000) hardTimeoutMs = 1000;
    ULONGLONG t0 = GetTickCount64();
    VolcDebugLog("=== OpenSession START ===");
    std::wstring cleanKey = TrimWhitespace(cfg.apiKey);
    if (cleanKey.empty()) return false;

    std::string uid = ToBackslashEscape(WideToUtf8(cleanKey.substr(0, (std::min)(size_t(12), cleanKey.size()))));

    std::string requestJson = "{"
        "\"user\":{\"uid\":\"" + uid + "\"},"
        "\"audio\":{"
            "\"format\":\"pcm\","
            "\"rate\":16000,"
            "\"bits\":16,"
            "\"channel\":1,"
            "\"codec\":\"raw\"";

    if (!cfg.language.empty() && cfg.mode == L"bigmodel_nostream") {
        requestJson += ",\"language\":\"" + WideToUtf8(cfg.language) + "\"";
    }

    requestJson += "},"
        "\"request\":{"
            "\"model_name\":\"bigmodel\","
            "\"enable_itn\":" + std::string(cfg.enableItn ? "true" : "false") + ","
            "\"enable_punc\":" + std::string(cfg.enablePunc ? "true" : "false") + ","
            "\"enable_ddc\":" + std::string(cfg.enableDdc ? "true" : "false") + ","
            "\"show_utterances\":false,"
            "\"result_type\":\"full\"";
    if (cfg.enableMusicFc && (cfg.mode == L"bigmodel_nostream" ||
        (cfg.mode == L"bigmodel_async" && cfg.enableNonstream))) {
        requestJson += ",\"enable_music_fc\":true";
    }
    if (cfg.enablePoiFc && (cfg.mode == L"bigmodel_nostream" ||
        (cfg.mode == L"bigmodel_async" && cfg.enableNonstream))) {
        requestJson += ",\"enable_poi_fc\":true";
    }
    if (cfg.enableNonstream && cfg.mode == L"bigmodel_async") {
        requestJson += ",\"enable_nonstream\":true";
    }
    if (cfg.endWindowSize > 0 && cfg.endWindowSize != 800) {
        requestJson += ",\"end_window_size\":" + std::to_string(cfg.endWindowSize);
    }
    if (cfg.forceToSpeechTime > 0) {
        requestJson += ",\"force_to_speech_time\":" + std::to_string(cfg.forceToSpeechTime);
    }
    if (!cfg.extraParams.empty()) {
        std::string extra = WideToUtf8(cfg.extraParams);
        size_t i = 0;
        while (i < extra.size()) {
            while (i < extra.size() && (extra[i] == ' ' || extra[i] == '\t' || extra[i] == '\n' || extra[i] == '\r')) i++;
            if (i >= extra.size() || extra[i] != '"') break;
            size_t kStart = i;
            while (i < extra.size() && extra[i] != ':') i++;
            if (i >= extra.size()) break;
            std::string key = extra.substr(kStart, i - kStart);
            if (key == "\"corpus\"") {
                i++;
                while (i < extra.size() && (extra[i] == ' ' || extra[i] == '\t')) i++;
                size_t vStart = i;
                int depth = 0;
                while (i < extra.size()) {
                    if (extra[i] == '{' || extra[i] == '[') depth++;
                    else if (extra[i] == '}' || extra[i] == ']') depth--;
                    else if (extra[i] == ',' && depth == 0) break;
                    i++;
                }
                if (i < extra.size() && extra[i] == ',') i++;
                continue;
            }
            i++;
            while (i < extra.size() && (extra[i] == ' ' || extra[i] == '\t')) i++;
            size_t vStart = i;
            int depth = 0;
            while (i < extra.size()) {
                if (extra[i] == '{' || extra[i] == '[') depth++;
                else if (extra[i] == '}' || extra[i] == ']') depth--;
                else if (extra[i] == ',' && depth == 0) break;
                i++;
            }
            std::string val = extra.substr(vStart, i - vStart);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            requestJson += "," + key + ":" + val;
            if (i < extra.size() && extra[i] == ',') i++;
        }
    }
    {
        std::string corpusParts;
        if (!cfg.hotwordsId.empty()) {
            corpusParts += ",\"boosting_table_id\":\"" + WideToUtf8(cfg.hotwordsId) + "\"";
        }
        if (!cfg.hotwordsName.empty()) {
            corpusParts += ",\"boosting_table_name\":\"" + WideToUtf8(cfg.hotwordsName) + "\"";
        }
        if (!cfg.correctTableId.empty()) {
            corpusParts += ",\"correct_table_id\":\"" + WideToUtf8(cfg.correctTableId) + "\"";
        }
        if (!cfg.correctTableName.empty()) {
            corpusParts += ",\"correct_table_name\":\"" + WideToUtf8(cfg.correctTableName) + "\"";
        }
        if (!cfg.contextJson.empty()) {
            std::string ctxRaw = WideToUtf8(cfg.contextJson);
            std::string ctxEscaped;
            for (char c : ctxRaw) {
                if (c == '\\') ctxEscaped += "\\\\";
                else if (c == '"') ctxEscaped += "\\\"";
                else if (c == '\n') ctxEscaped += "\\n";
                else if (c == '\r') ctxEscaped += "\\r";
                else if (c == '\t') ctxEscaped += "\\t";
                else ctxEscaped += c;
            }
            corpusParts += ",\"context\":\"" + ctxEscaped + "\"";
        }
        if (!corpusParts.empty()) {
            requestJson += ",\"corpus\":{" + corpusParts.substr(1) + "}";
        }
    }
    requestJson += "}"
    "}";

    std::vector<BYTE> jsonPayload(requestJson.begin(), requestJson.end());
    std::vector<BYTE> frame = BuildFrame(MSG_FULL_CLIENT_REQ, FLAG_NO_SEQ,
                                         SER_JSON, COMP_NONE, 0, jsonPayload);

    if (sess.forceAbort.load()) {
        VolcDebugLog("OpenSession: aborted (forceAbort)");
        return false;
    }

    if (!EnsureConnection(sess)) {
        VolcDebugLog("OpenSession: EnsureConnection failed");
        return false;
    }

    std::wstring path = L"/api/v3/sauc/" + cfg.mode;
    HINTERNET hReq = WinHttpOpenRequest(sess.hConnect, L"GET",
        path.c_str(), nullptr, WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!hReq) {
        VolcDebugLog("OpenSession: WinHttpOpenRequest failed (err=%u, elapsed=%llums)",
                     GetLastError(), GetTickCount64() - t0);
        WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
        WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
        if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess)) return OpenSessionImpl(sess, cfg, true, hardTimeoutMs);
        return false;
    }
    sess.activeReq.store(hReq);

    WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    DWORD closeTimeout = 5000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT, &closeTimeout, sizeof(closeTimeout));
    DWORD keepAlive = 15000;
    WinHttpSetOption(hReq, WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL, &keepAlive, sizeof(keepAlive));

    std::wstring uuid = GenerateUuidStr();
    std::wstring headers =
        L"X-Api-Key: " + cleanKey +
        L"\r\nX-Api-Resource-Id: " + cfg.resourceId +
        L"\r\nX-Api-Connect-Id: " + uuid +
        L"\r\n";

    if (!WinHttpAddRequestHeaders(hReq, headers.c_str(),
        static_cast<DWORD>(wcslen(headers.c_str())), WINHTTP_ADDREQ_FLAG_ADD)) {
        VolcDebugLog("OpenSession: WinHttpAddRequestHeaders failed (err=%u, elapsed=%llums)",
                     GetLastError(), GetTickCount64() - t0);
        { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }
        WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
        WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
        if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess)) return OpenSessionImpl(sess, cfg, true, hardTimeoutMs);
        return false;
    }

    const int requestTimeoutMs = static_cast<int>(hardTimeoutMs);
    WinHttpSetTimeouts(hReq, requestTimeoutMs, requestTimeoutMs, requestTimeoutMs, requestTimeoutMs);
    VolcDebugLog("OpenSession: request hard timeout budget=%ums", hardTimeoutMs);

    if (sess.forceAbort.load()) {
        VolcDebugLog("OpenSession: aborted before SendRequest");
        { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }
        return false;
    }

    ULONGLONG t3 = GetTickCount64();
    VolcWinHttpTraceContext traceContext;
    traceContext.id = NextVolcWinHttpTraceId();
    traceContext.startTick = t3;
    traceContext.hardTimeoutMs = hardTimeoutMs;
    WINHTTP_STATUS_CALLBACK callbackResult = WinHttpSetStatusCallback(
        hReq, VolcWinHttpStatusCallback, WINHTTP_CALLBACK_FLAG_ALL_NOTIFICATIONS, 0);
    if (callbackResult == WINHTTP_INVALID_STATUS_CALLBACK) {
        VolcDebugLog("OpenSession WinHTTP trace #%d: SetStatusCallback failed (err=%u)",
                     traceContext.id, GetLastError());
    } else {
        VolcDebugLog("OpenSession WinHTTP trace #%d: START hardTimeout=%ums path=/api/v3/sauc/%ls",
                     traceContext.id, hardTimeoutMs, cfg.mode.c_str());
    }

    std::atomic<bool> requestDone{false};
    std::atomic<bool> hardTimedOut{false};
    std::thread watchdogThread([&sess, &requestDone, &hardTimedOut, &traceContext, t3, hardTimeoutMs]() {
        while (!requestDone.load()) {
            Sleep(100);
            if (requestDone.load()) return;
            if (GetTickCount64() - t3 >= hardTimeoutMs) {
                hardTimedOut.store(true);
                const DWORD lastStatus = traceContext.lastStatus.load();
                const ULONGLONG lastStatusTick = traceContext.lastStatusTick.load();
                const ULONGLONG lastStatusAge = lastStatusTick > 0 ? GetTickCount64() - lastStatusTick : 0;
                const DWORD activeStatus = traceContext.lastActiveStatus.load();
                const ULONGLONG activeStatusTick = traceContext.lastActiveStatusTick.load();
                const ULONGLONG activeStatusAge = activeStatusTick > 0 ? GetTickCount64() - activeStatusTick : 0;
                VolcDebugLog("OpenSession: hard timeout (%llums >= %ums), closing activeReq (trace=%d lastStatus=%s age=%llums activeStatus=%s activeAge=%llums)",
                             GetTickCount64() - t3, hardTimeoutMs, traceContext.id,
                             VolcWinHttpStatusName(lastStatus), lastStatusAge,
                             VolcWinHttpStatusName(activeStatus), activeStatusAge);
                HINTERNET taken = sess.activeReq.exchange(nullptr);
                if (taken) WinHttpCloseHandle(taken);
                return;
            }
        }
    });

    BOOL sendResult = WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                         WINHTTP_NO_REQUEST_DATA, 0, 0,
                                         reinterpret_cast<DWORD_PTR>(&traceContext));
    DWORD sendErr = sendResult ? ERROR_SUCCESS : GetLastError();
    BOOL recvResult = FALSE;
    DWORD recvErr = ERROR_SUCCESS;
    if (sendResult) {
        recvResult = WinHttpReceiveResponse(hReq, nullptr);
        if (!recvResult) recvErr = GetLastError();
    }
    bool sendOk = (sendResult != FALSE);
    bool recvOk = (recvResult != FALSE);
    requestDone.store(true);
    watchdogThread.join();
    {
        const DWORD lastStatus = traceContext.lastStatus.load();
        const ULONGLONG lastStatusTick = traceContext.lastStatusTick.load();
        const ULONGLONG lastStatusAge = lastStatusTick > 0 ? GetTickCount64() - lastStatusTick : 0;
        const DWORD activeStatus = traceContext.lastActiveStatus.load();
        const ULONGLONG activeStatusTick = traceContext.lastActiveStatusTick.load();
        const ULONGLONG activeStatusAge = activeStatusTick > 0 ? GetTickCount64() - activeStatusTick : 0;
        VolcDebugLog("OpenSession WinHTTP trace #%d: END sendOk=%d recvOk=%d hardTimedOut=%d forceAbort=%d lastStatus=%s age=%llums activeStatus=%s activeAge=%llums elapsed=%llums",
                     traceContext.id, sendOk ? 1 : 0, recvOk ? 1 : 0,
                     hardTimedOut.load() ? 1 : 0, sess.forceAbort.load() ? 1 : 0,
                     VolcWinHttpStatusName(lastStatus), lastStatusAge,
                     VolcWinHttpStatusName(activeStatus), activeStatusAge, GetTickCount64() - t3);
    }

    if (!sendOk) {
        { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }
        VolcDebugLog("OpenSession: WinHttpSendRequest failed (err=%u, elapsed=%llums, hardTimedOut=%d, forceAbort=%d)",
                     sendErr, GetTickCount64() - t0, hardTimedOut.load() ? 1 : 0, sess.forceAbort.load() ? 1 : 0);
        if (traceContext.lastActiveStatus.load() == WINHTTP_CALLBACK_STATUS_CONNECTING_TO_SERVER) {
            VolcMaybeLogConnectDiagnosticsAsync(
                traceContext.id,
                hardTimedOut.load() ? "send_failed_after_connect_hard_timeout" : "send_failed_while_connecting");
        }
        if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
        if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
        if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess)) return OpenSessionImpl(sess, cfg, true, hardTimeoutMs);
        return false;
    }

    if (!recvOk) {
        { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }
        VolcDebugLog("OpenSession: WinHttpReceiveResponse failed (err=%u, elapsed=%llums, hardTimedOut=%d, forceAbort=%d)",
                     recvErr, GetTickCount64() - t0, hardTimedOut.load() ? 1 : 0, sess.forceAbort.load() ? 1 : 0);
        if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
        if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
        if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess)) return OpenSessionImpl(sess, cfg, true, hardTimeoutMs);
        return false;
    }

    if (hardTimedOut.load()) {
        { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }
        VolcDebugLog("OpenSession: request completed after hard timeout race (elapsed=%llums), treating as failed",
                     GetTickCount64() - t0);
        if (sess.hConnect) { WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr; }
        if (sess.hSession) { WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr; }
        return false;
    }

    ULONGLONG t4 = GetTickCount64();
    VolcDebugLog("SendRequest+ReceiveResponse: %llums", t4 - t3);

    DWORD statusCode = 0;
    DWORD statusCodeSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusCodeSize,
                        WINHTTP_NO_HEADER_INDEX);

    if (statusCode != 101) {
        VolcDebugLog("OpenSession: HTTP status %u (expected 101, elapsed=%llums)",
                     statusCode, GetTickCount64() - t0);
        { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }
        WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
        WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
        if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess)) return OpenSessionImpl(sess, cfg, true, hardTimeoutMs);
        return false;
    }

    sess.hWebSocket = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    { HINTERNET taken = sess.activeReq.exchange(nullptr); if (taken) WinHttpCloseHandle(taken); }

    if (!sess.hWebSocket) {
        VolcDebugLog("OpenSession: CompleteUpgrade failed (err=%u, elapsed=%llums)",
                     GetLastError(), GetTickCount64() - t0);
        WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
        WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
        if (!isRetry && GetTickCount64() - t0 < 2000 && RebuildConnection(sess)) return OpenSessionImpl(sess, cfg, true, hardTimeoutMs);
        return false;
    }

    DWORD recvTimeout = 2000;
    WinHttpSetOption(sess.hWebSocket, WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT,
                     &recvTimeout, sizeof(recvTimeout));

    sess.sequence = 0;

    VolcDebugLog("Sending init frame (%zu bytes), json=%zu bytes", frame.size(), jsonPayload.size());

    WinHttpWebSocketSend(sess.hWebSocket,
                         static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG),
                         frame.data(), static_cast<DWORD>(frame.size()));

    ULONGLONG t5 = GetTickCount64();
    VolcResult initResp = ReceiveResult(sess.hWebSocket, 5000, &sess);
    ULONGLONG t6 = GetTickCount64();
    VolcDebugLog("Init frame receive: %llums", t6 - t5);
    VolcDebugLog("Init response: text_wlen=%zu error=%d",
                 initResp.text.size(),
                 initResp.text.find(L"[VolcEngine error:") != std::wstring::npos ? 1 : 0);
    if (initResp.text.find(L"[VolcEngine error:") != std::wstring::npos) {
        VolcDebugLog("OpenSession FAILED - server error (text_wlen=%zu)", initResp.text.size());
        sess.lastError = initResp.text;
        WebSocketCloseGracefully(sess.hWebSocket, &sess);
        sess.hWebSocket = nullptr;
        WinHttpCloseHandle(sess.hConnect); sess.hConnect = nullptr;
        WinHttpCloseHandle(sess.hSession); sess.hSession = nullptr;
        return false;
    }

    VolcDebugLog("=== OpenSession OK (total: %llums) ===", GetTickCount64() - t0);
    sess.sequence = 2;
    return true;
}

inline bool OpenSession(VolcSession& sess, const VolcConfig& cfg, DWORD hardTimeoutMs = 3000) {
    return OpenSessionImpl(sess, cfg, false, hardTimeoutMs);
}

inline std::wstring SendAudio(VolcSession& sess, const std::vector<BYTE>& pcmChunk, bool isLast, bool asyncMode = false, bool nostreamMode = false) {
    if (!sess.hWebSocket || !sess.connected) return L"";
    if (sess.forceAbort.load()) {
        VolcDebugLog("SendAudio: aborted (forceAbort)");
        return L"";
    }

    uint8_t flags = isLast ? FLAG_NEG_PACKET : FLAG_POS_SEQ;
    uint32_t seq = isLast ? 0 : static_cast<uint32_t>(sess.sequence);
    std::vector<BYTE> frame = BuildFrame(MSG_AUDIO_ONLY, flags,
                                         SER_NONE, COMP_NONE, seq, pcmChunk);

    if (isLast || sess.sequence == 2) {
        VolcDebugLog("SendAudio: %zu bytes, isLast=%d, seq=%u, frame=%zu bytes, async=%d nostream=%d",
                     pcmChunk.size(), isLast, seq, frame.size(), asyncMode, nostreamMode);
    }

    ULONGLONG tSend0 = GetTickCount64();
    DWORD err = WinHttpWebSocketSend(sess.hWebSocket,
                                     static_cast<WINHTTP_WEB_SOCKET_BUFFER_TYPE>(VOLC_WEB_SOCKET_BINARY_MSG),
                                     frame.data(), static_cast<DWORD>(frame.size()));

    if (err != ERROR_SUCCESS) {
        VolcDebugLog("SendAudio FAILED: err=%u", err);
        WebSocketCloseGracefully(sess.hWebSocket, &sess);
        sess.hWebSocket = nullptr;
        sess.connected = false;
        return L"";
    }

    if (!isLast) {
        sess.sequence++;
    }

    if ((asyncMode || nostreamMode) && isLast) {
        VolcDebugLog("SendAudio: isLast sent (%s, skipping receive; drainThread will read result)",
                     asyncMode ? "async" : "nostream");
        return L"";
    }

    if ((asyncMode || nostreamMode) && !isLast) {
        return L"";
    }

    VolcResult vr = ReceiveResult(sess.hWebSocket, isLast ? 4000 : 150, &sess);
    ULONGLONG tSend1 = GetTickCount64();
    VolcDebugLog("SendAudio recv: %llums (isLast=%d), text_wlen=%zu definite=%d",
                 tSend1 - tSend0, isLast, vr.text.size(), vr.definite);
    return vr.text;
}

inline std::wstring DrainReceiveBuffer(HINTERNET hWebSocket, VolcSession* sess = nullptr) {
    return ReceiveResult(hWebSocket, 1, sess).text;
}

inline std::wstring CloseSession(VolcSession& sess) {
    ULONGLONG tClose0 = GetTickCount64();
    VolcDebugLog("=== CloseSession ===");
    if (!sess.hWebSocket) return L"";

    std::wstring finalText;

    if (!sess.forceAbort.load()) {
        for (int retry = 0; retry < 6; retry++) {
            VolcResult chunk = ReceiveResult(sess.hWebSocket, 500, &sess);
            if (!chunk.text.empty()) {
                finalText = chunk.text;
            } else {
                break;
            }
            if (sess.forceAbort.load()) break;
        }
    }

    WebSocketCloseGracefully(sess.hWebSocket, &sess);
    sess.hWebSocket = nullptr;

    if (g_volcKeepAlive) {
        VolcDebugLog("CloseSession: keeping hSession+hConnect alive");
        sess.lastUsedTick = GetTickCount64();
    } else {
        if (sess.hConnect) {
            WinHttpCloseHandle(sess.hConnect);
            sess.hConnect = nullptr;
        }
        if (sess.hSession) {
            WinHttpCloseHandle(sess.hSession);
            sess.hSession = nullptr;
        }
    }

    sess.connected = false;
    ULONGLONG tClose1 = GetTickCount64();
    VolcDebugLog("=== CloseSession DONE (total: %llums), final_text_wlen=%zu ===",
                 tClose1 - tClose0, finalText.size());
    return finalText;
}

inline void ClosePersistentConnection(VolcSession& sess) {
    VolcDebugLog("=== ClosePersistentConnection ===");
    if (sess.hConnect) {
        WinHttpCloseHandle(sess.hConnect);
        sess.hConnect = nullptr;
    }
    if (sess.hSession) {
        WinHttpCloseHandle(sess.hSession);
        sess.hSession = nullptr;
    }
}

inline void PrewarmConnection(VolcSession& sess) {
    VolcDebugLog("=== PrewarmConnection ===");
    if (sess.hSession && sess.hConnect) {
        VolcDebugLog("PrewarmConnection: already connected");
        return;
    }
    if (!EnsureConnection(sess)) {
        VolcDebugLog("PrewarmConnection: failed");
        return;
    }
    sess.lastUsedTick = GetTickCount64();
    VolcDebugLog("PrewarmConnection: OK (hSession+hConnect handles ready; network handshake deferred)");
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
        L"\r\nX-Api-Connect-Id: " + uuid +
        L"\r\n";

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
