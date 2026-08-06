#include "qwen_free_proto_asr.h"

#include "asr_runtime_log.h"
#include "globals.h"
#include "qwen_free_asr_json.h"
#include "qwen_free_diagnostics.h"
#include "qwen_free_proto_unet.h"
#include "qwen_free_proto_utdid.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>

#pragma comment(lib, "winhttp.lib")

namespace qwen_free_proto_asr {

namespace {

constexpr size_t kAudioCommitBytes = 0xf00;
constexpr DWORD kReceivePollSliceMs = 50;
constexpr DWORD kStartHandshakeTimeoutMs = 3000;
constexpr size_t kMaxAsrFrameBytes = 4u * 1024u * 1024u;
constexpr size_t kMaxReceivePayloadBytes = 4u * 1024u * 1024u;

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring w(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), len);
    w.resize(static_cast<size_t>(len - 1));
    return w;
}

std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string s(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    s.resize(static_cast<size_t>(len - 1));
    return s;
}

int HexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string JsonEscape(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 8);
    for (const unsigned char c : value) {
        switch (c) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (c < 0x20u) {
                char buffer[7];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                              static_cast<unsigned int>(c));
                escaped += buffer;
            } else {
                escaped.push_back(static_cast<char>(c));
            }
            break;
        }
    }
    return escaped;
}

// unet 的 EncryptWithNumber 返回原始密文的 hex 展开；原版 /ws query 的
// ut 字段使用同一密文的标准 Base64 表示。
std::string HexToBase64(const std::string& hex) {
    if (hex.empty() || (hex.size() & 1u) != 0) return {};
    std::string bytes;
    bytes.reserve(hex.size() / 2);
    for (size_t i = 0; i < hex.size(); i += 2) {
        int hi = HexValue(hex[i]);
        int lo = HexValue(hex[i + 1]);
        if (hi < 0 || lo < 0) return {};
        bytes.push_back(static_cast<char>((hi << 4) | lo));
    }

    static const char kBase64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const unsigned b0 = static_cast<unsigned char>(bytes[i]);
        const unsigned b1 = (i + 1 < bytes.size())
                          ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const unsigned b2 = (i + 2 < bytes.size())
                          ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        out.push_back(kBase64[(b0 >> 2) & 0x3f]);
        out.push_back(kBase64[((b0 & 0x03) << 4) | (b1 >> 4)]);
        out.push_back(i + 1 < bytes.size() ? kBase64[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
        out.push_back(i + 2 < bytes.size() ? kBase64[b2 & 0x3f] : '=');
    }
    return out;
}

void LogErr(const wchar_t* tag, const std::wstring& msg) {
    // asr_runtime_log::Write 只接受 char*，需要把 wide 拼接后转 UTF-8。
    std::wstring line = L"[qwen_free_asr] ";
    line += tag;
    line += L": ";
    line += msg;
    std::string utf8 = WideToUtf8(line);
    asr_runtime_log::Write("%s", utf8.c_str());
}

} // namespace

QwenFreeProtoAsrSession::~QwenFreeProtoAsrSession() {
    Close();
}

bool QwenFreeProtoAsrSession::BeginOperation() {
    std::lock_guard<std::mutex> lock(operationMutex_);
    if (closing_ || closeRequested_.load(std::memory_order_acquire) ||
        cancelRequested_.load(std::memory_order_acquire)) {
        return false;
    }
    ++activeOperations_;
    return true;
}

void QwenFreeProtoAsrSession::EndOperation() {
    std::lock_guard<std::mutex> lock(operationMutex_);
    if (activeOperations_ > 0) --activeOperations_;
    if (activeOperations_ == 0) operationCv_.notify_all();
}

void QwenFreeProtoAsrSession::RequestCancel() {
    cancelRequested_.store(true, std::memory_order_release);
    operationCv_.notify_all();
}

bool QwenFreeProtoAsrSession::ResetCancellation() {
    std::lock_guard<std::mutex> lock(operationMutex_);
    if (closing_ || activeOperations_ != 0 ||
        cancelRequested_.load(std::memory_order_acquire)) {
        return false;
    }
    closeRequested_.store(false, std::memory_order_release);
    return true;
}

bool QwenFreeProtoAsrSession::ResetCancellationForNewSession() {
    std::lock_guard<std::mutex> lock(operationMutex_);
    if (closing_ || activeOperations_ != 0) return false;
    cancelRequested_.store(false, std::memory_order_release);
    closeRequested_.store(false, std::memory_order_release);
    operationCv_.notify_all();
    return true;
}

TestResult TestConnection(const AsrConfig& cfg) {
    TestResult result;
    const ULONGLONG started = GetTickCount64();
    QwenFreeProtoAsrSession session;
    std::wstring error;
    if (!session.Connect(cfg, error)) {
        result.message = error.empty() ? L"ASR WebSocket connection failed." : error;
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        return result;
    }
    if (!session.SendStart(error)) {
        result.message = error.empty() ? L"ASR session handshake failed." : error;
        session.Close();
        result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
        return result;
    }
    session.Close();
    result.ok = true;
    result.message = L"ASR WebSocket handshake OK.";
    result.elapsedMs = static_cast<DWORD>(GetTickCount64() - started);
    return result;
}

std::wstring QwenFreeProtoAsrSession::BuildSignedUrl(
    const AsrConfig& cfg,
    std::string& signContent,
    std::string& signWg,
    std::string& encryptedUtdid,
    std::string& vcode) {

    // 原版 shell_ffi 的真实请求格式（右 Alt 触发链路抓包）：
    // /ws/v1/asr?biz_id=qwen_command&chid=<16hex time><16hex seq>&fr=win
    //   &from=qianwen_pc_voice&mt=<plain utdid>&nt=99&nw=0&pr=qwen
    //   &tm=<milliseconds>&uc_param_str=prfrutvemtntnwkpst
    //   &ut=<raw Base64(WSG(utdid))>&ve=0.1.0&version=2&sign=<44hex>
    // sign 的输入是同一顺序去掉 sign 字段后的 query。
    static std::atomic<uint64_t> channelSequence{1};
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const uint64_t timestamp = static_cast<uint64_t>(now);
    const uint64_t sequence = channelSequence.fetch_add(1);

    char vcodeBuf[32];
    std::snprintf(vcodeBuf, sizeof(vcodeBuf), "%llu",
                  static_cast<unsigned long long>(timestamp));
    vcode = vcodeBuf;

    char channelBuf[40];
    std::snprintf(channelBuf, sizeof(channelBuf), "%016llx%016llx",
                  static_cast<unsigned long long>(timestamp),
                  static_cast<unsigned long long>(sequence));
    const std::string channelId = channelBuf;
    channelId_ = channelId;

    encryptedUtdid.clear();
    if (qwen_free_proto_unet::IsReady()) {
        auto r = qwen_free_proto_unet::EncryptWithNumber(
            qwen_free_proto_unet::kWsgNumberAsr, cfg.utdid);
        if (r.ok) {
            encryptedUtdid = HexToBase64(r.sign);
        } else {
            LogErr(L"kps", L"unet FFI failed: " + r.error);
        }
    }

    AsrQueryFields queryFields;
    queryFields.appkey = cfg.appkey;
    queryFields.channelId = channelId;
    queryFields.utdid = cfg.utdid;
    queryFields.vcode = vcode;
    queryFields.encryptedUtdid = encryptedUtdid;
    queryFields.ve = cfg.ve;
    queryFields.version = cfg.version;
    signContent = BuildAsrSignContent(queryFields);

    // Both the encrypted device identity and the native numbered signature
    // are required. The former generic-HMAC fallback was known to be rejected
    // by the service, so do not send a guaranteed HTTP 401 request.
    if (qwen_free_proto_unet::IsReady() && !encryptedUtdid.empty()) {
        auto r = qwen_free_proto_unet::SignWithNumber(
            qwen_free_proto_unet::kWsgNumberAsr, signContent);
        if (r.ok) {
            signWg = r.sign;
        } else {
            LogErr(L"sign", L"unet FFI failed: " + r.error);
        }
    }

    // shell_ffi 保留 Base64 原始字符，并把 sign 放在 query 末尾。服务端对
    // percent-encoded ut 或把 sign 插入中间的组合不会升级 WebSocket。
    std::string hostUtf8 = WideToUtf8(cfg.host);
    std::string pathUtf8 = WideToUtf8(cfg.path);
    std::ostringstream url;
    queryFields = {};
    queryFields.appkey = cfg.appkey;
    queryFields.channelId = channelId;
    queryFields.utdid = cfg.utdid;
    queryFields.vcode = vcode;
    queryFields.encryptedUtdid = encryptedUtdid;
    queryFields.ve = cfg.ve;
    queryFields.version = cfg.version;
    url << "wss://" << hostUtf8 << pathUtf8
        << "?" << BuildAsrQuery(queryFields, signWg);
    return Utf8ToWide(url.str());
}

bool QwenFreeProtoAsrSession::Connect(const AsrConfig& cfg, std::wstring& out_error) {
    // A replay normally closes first. Close() is lifetime-safe and waits for
    // any previous protocol operation before releasing the old handle set.
    Close();
    if (!ResetCancellation()) {
        out_error = L"Qwen WebSocket connect cancelled";
        return false;
    }
    cfg_ = cfg;
    // The production signer accepts only the initialized 24-character
    // base62 device identity.  Keep debug URL overrides usable with synthetic
    // test identities, but never send a malformed UTDID to the real service.
    if (cfg_.debugUrlOverride.empty() &&
        !qwen_free_proto_utdid::IsValidUtdid(cfg_.utdid)) {
        out_error = L"invalid utdid (expect 24 initialized base62 chars; received " +
                    std::to_wstring(cfg_.utdid.size()) + L" chars)";
        return false;
    }

    OperationScope operation(*this);
    if (!operation) {
        out_error = L"Qwen WebSocket connect cancelled";
        return false;
    }

    // 1. 构造签名 URL。
    std::string signContent, signWg, encryptedUtdid, vcode;
    std::wstring url = cfg.debugUrlOverride.empty()
                       ? BuildSignedUrl(cfg, signContent, signWg, encryptedUtdid, vcode)
                       : cfg.debugUrlOverride;
    if (cfg.debugUrlOverride.empty() &&
        (encryptedUtdid.empty() || signWg.empty())) {
        out_error = L"native Qwen signer unavailable";
        const std::wstring initError = qwen_free_proto_unet::GetInitError();
        if (!initError.empty()) out_error += L": " + initError;
        return false;
    }
    AsrWebSocketEndpoint endpoint;
    if (!ParseAsrWebSocketUrl(url, endpoint)) {
        out_error = L"invalid Qwen ASR WebSocket URL";
        return false;
    }
    // The URL and signed query contain the device fingerprint, encrypted
    // UTDID and WSG signature.  Keep only lengths in diagnostics; the exact
    // values are neither needed for normal troubleshooting nor safe to show
    // in HUD/MessageBox output or debug logs.
    LogErr(L"connect", L"request prepared host=" + endpoint.host +
           L" path=" + (cfg.debugUrlOverride.empty() ? cfg.path : L"<debug override>") +
           L" unet=" + std::to_wstring(qwen_free_proto_unet::IsReady() ? 1 : 0) +
           L" sign_content_len=" + std::to_wstring(signContent.size()) +
           L" sign_wg_len=" + std::to_wstring(signWg.size()) +
           L" encrypted_utdid_len=" + std::to_wstring(encryptedUtdid.size()));

    if (IsCancellationRequested()) {
        out_error = L"Qwen WebSocket connect cancelled";
        return false;
    }

    // 2. WinHTTP session. Use the system proxy configuration so enterprise
    // deployments do not fail solely because the network requires a proxy.
    HINTERNET session = WinHttpOpen(L"VoxType/0.9 QwenFreeASR/1.0",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    hSession_.store(session);
    if (!session) {
        out_error = L"WinHttpOpen failed: " + std::to_wstring(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(session,
                       static_cast<int>(cfg_.connectTimeoutMs),
                       static_cast<int>(cfg_.connectTimeoutMs),
                       static_cast<int>(cfg_.recvTimeoutMs),
                       static_cast<int>(cfg_.recvTimeoutMs));

    // 3. WinHTTP connect（host:port）。
    HINTERNET connection = WinHttpConnect(
        session, endpoint.host.c_str(), endpoint.port, 0);
    hConnect_.store(connection);
    if (!connection) {
        out_error = L"WinHttpConnect failed: " + std::to_wstring(GetLastError());
        return false;
    }

    // 4. 创建 WebSocket 升级请求。
    //    WinHttpOpenRequest 使用 GET，路径+query 不含 host。
    //    url 是完整 wss://host/path?query，提取 path?query 部分。
    HINTERNET hReq = WinHttpOpenRequest(
        connection, L"GET", endpoint.pathAndQuery.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        endpoint.secure ? WINHTTP_FLAG_SECURE : 0);
    if (!hReq) {
        out_error = L"WinHttpOpenRequest failed: " + std::to_wstring(GetLastError());
        return false;
    }

    // 5. 升级到 WebSocket。
    BOOL status = WinHttpSetOption(hReq, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET,
                                    nullptr, 0);
    if (!status) {
        out_error = L"WinHttpSetOption(UPGRADE_TO_WEB_SOCKET) failed: " +
                    std::to_wstring(GetLastError());
        WinHttpCloseHandle(hReq);
        return false;
    }

    // 6. 发送请求并等待 WebSocket 升级。
    // 原版 shell_ffi 的真实握手头：固定浏览器 UA、Origin，以及与 query 的
    // tm 相同的 x-u-vcode。不要发送旧版 x-audid/X-U-SIGN 头。
    std::wstring requestHeaders;
    requestHeaders += L"User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
                      L"AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Safari/605.1.15 "
                      L"TONGYI_DESKTOP/0.1.0 QuarkPC/standalone\r\n";
    requestHeaders += L"Origin: https://www.qianwen.com\r\n";
    requestHeaders += L"x-u-vcode: " + Utf8ToWide(vcode) + L"\r\n";
    BOOL ok = WinHttpSendRequest(hReq, requestHeaders.c_str(),
                                  static_cast<DWORD>(requestHeaders.size()),
                                  WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) {
        out_error = L"WinHttpSendRequest failed: " + std::to_wstring(GetLastError());
        WinHttpCloseHandle(hReq);
        return false;
    }
    if (!WinHttpReceiveResponse(hReq, nullptr)) {
        out_error = L"WinHttpReceiveResponse failed: " + std::to_wstring(GetLastError());
        WinHttpCloseHandle(hReq);
        return false;
    }

    // 7. 检查 HTTP 状态码：WebSocket 升级期望 101。
    //    非 101 时服务器会返回 4xx/5xx 错误体（如签名失败、UTDID 非法等），
    //    此时直接调用 WinHttpWebSocketCompleteUpgrade 会得到无意义的 4317
    //    (ERROR_WINHTTP_OPERATION_CANCELLED)，需先读出真实错误。
    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(hReq,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX,
                        &statusCode,
                        &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (statusCode != 101) {
        std::wstring err = L"WebSocket upgrade failed (HTTP " +
                           std::to_wstring(statusCode) + L")";
        // 读取响应体片段以便诊断（最大 1KB）。
        DWORD avail = 0;
        if (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
            DWORD toRead = (avail > 1024) ? 1024 : avail;
            std::string body(toRead, '\0');
            DWORD read = 0;
            if (WinHttpReadData(hReq, body.data(), toRead, &read) && read > 0) {
                body.resize(read);
                err += L": " + Utf8ToWide(
                    qwen_free_diagnostics::SummarizeHttpBody(body));
            }
        }
        // 附加非敏感诊断信息（不依赖 debug mode，直接显示在 HUD）。
        err += L" | unet=" + std::to_wstring(qwen_free_proto_unet::IsReady() ? 1 : 0);
        if (!qwen_free_proto_unet::IsReady()) {
            std::wstring unetErr = qwen_free_proto_unet::GetInitError();
            if (!unetErr.empty()) {
                err += L" | unet_err=" + unetErr;
            }
        }
        err += L" | sign_content_len=" + std::to_wstring(signContent.size());
        err += L" | sign_wg_len=" + std::to_wstring(signWg.size());
        err += L" | encrypted_utdid_len=" + std::to_wstring(encryptedUtdid.size());
        err += L" | x_u_vcode=" + Utf8ToWide(vcode);
        LogErr(L"connect", err);
        out_error = err;
        WinHttpCloseHandle(hReq);
        return false;
    }

    // 8. 完成 WebSocket 升级。
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(hReq, 0);
    DWORD lastErr = GetLastError();
    WinHttpCloseHandle(hReq);  // hReq 在升级后不再需要
    if (!ws) {
        out_error = L"WinHttpWebSocketCompleteUpgrade failed: " + std::to_wstring(lastErr);
        return false;
    }
    if (IsCancellationRequested()) {
        WinHttpCloseHandle(ws);
        out_error = L"Qwen WebSocket connect cancelled";
        return false;
    }
    hWebSocket_.store(ws);

    // RecvFrame adjusts the session receive timeout for each pump interval.
    LogErr(L"connect", L"connected, ws=" + std::to_wstring(reinterpret_cast<uintptr_t>(ws)));
    return true;
}

bool QwenFreeProtoAsrSession::SendStart(std::wstring& out_error) {
    if (IsCancellationRequested() || !hWebSocket_.load()) {
        out_error = L"not connected";
        return false;
    }
    const std::string json =
        "{\"data\":{\"bitDepth\":16,\"channel\":\"mono\","
        "\"ctcHotwordType\":2,\"format\":\"pcm\",\"modelType\":4,"
        "\"promptType\":2,\"sampleRate\":16000,"
        "\"type\":\"manualStreamStop\",\"vadStrategy\":2},"
        "\"eventId\":\"" + JsonEscape(channelId_) +
        "_0\",\"eventType\":\"user.session.start\"}";
    {
        std::lock_guard<std::mutex> lock(identifiersMutex_);
        sessionId_.clear();
        roundId_.clear();
    }
    eventSequence_ = 1;
    pendingPcm_.clear();
    receivePayload_.clear();
    if (!SendFrame(nullptr, 0, json, out_error)) return false;

    // The server allocates these identifiers in the start response. Some
    // gateways emit a partial/control frame first, so keep pumping until both
    // IDs arrive instead of treating the first non-error object as a Partial
    // and waiting for a later audio commit that can never be valid.
    const ULONGLONG deadline =
        GetTickCount64() + static_cast<ULONGLONG>(kStartHandshakeTimeoutMs);
    for (;;) {
        if (IsCancellationRequested()) {
            out_error = L"Qwen ASR session start cancelled";
            return false;
        }

        std::string sessionId;
        std::string roundId;
        {
            std::lock_guard<std::mutex> lock(identifiersMutex_);
            sessionId = sessionId_;
            roundId = roundId_;
        }
        if (!sessionId.empty() && !roundId.empty()) return true;

        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            out_error = L"timed out waiting for user.session.start response";
            return false;
        }
        const DWORD remaining = static_cast<DWORD>(
            (std::min<ULONGLONG>)(deadline - now,
                                   static_cast<ULONGLONG>(kStartHandshakeTimeoutMs)));
        const AsrFrame received = RecvFrame(remaining);
        if (received.type == FrameType::Error) {
            out_error = received.errorMsg.empty()
                ? L"server returned an ASR start error"
                : received.errorMsg;
            // RecvFrame sanitizes structured codes before exposing them. Keep
            // the code in the start error so retry policy can distinguish
            // auth/signature failures (for example D30112) from transport
            // failures.
            if (!received.errorCode.empty()) {
                out_error += L" (code=" + received.errorCode + L")";
            }
            return false;
        }
        if (received.type == FrameType::Closed) {
            out_error = L"server closed before user.session.start response";
            return false;
        }
        if (received.type == FrameType::Timeout && GetTickCount64() >= deadline) {
            out_error = L"timed out waiting for user.session.start response";
            return false;
        }
    }
}

bool QwenFreeProtoAsrSession::SendPcm(const void* pcm, size_t bytes,
                                       std::wstring& out_error) {
    if (IsCancellationRequested() || !hWebSocket_.load()) {
        out_error = L"not connected";
        return false;
    }
    if (bytes == 0) return true;

    if (!pcm || bytes > static_cast<size_t>(UINT32_MAX) ||
        bytes > kMaxAsrFrameBytes) {
        out_error = L"invalid PCM buffer";
        return false;
    }
    const auto* input = static_cast<const BYTE*>(pcm);
    pendingPcm_.insert(pendingPcm_.end(), input, input + bytes);
    while (pendingPcm_.size() >= kAudioCommitBytes) {
        if (!SendCommit(pendingPcm_.data(), kAudioCommitBytes, out_error)) return false;
        pendingPcm_.erase(pendingPcm_.begin(),
                          pendingPcm_.begin() + static_cast<ptrdiff_t>(kAudioCommitBytes));
    }
    return true;
}

bool QwenFreeProtoAsrSession::SendStop(std::wstring& out_error) {
    if (IsCancellationRequested() || !hWebSocket_.load()) {
        out_error = L"not connected";
        return false;
    }
    if (!pendingPcm_.empty()) {
        if (!SendCommit(pendingPcm_.data(), pendingPcm_.size(), out_error)) return false;
        pendingPcm_.clear();
    }
    std::string sessionId;
    std::string roundId;
    {
        std::lock_guard<std::mutex> lock(identifiersMutex_);
        sessionId = sessionId_;
        roundId = roundId_;
    }
    if (sessionId.empty() || roundId.empty()) {
        out_error = L"cannot stop without ASR session identifiers";
        return false;
    }
    const std::string json =
        "{\"data\":{\"roundId\":\"" + JsonEscape(roundId) +
        "\",\"sessionId\":\"" + JsonEscape(sessionId) +
        "\",\"type\":\"manualStreamStop\"},\"eventId\":\"" +
        JsonEscape(channelId_) + "_" + std::to_string(eventSequence_++) +
        "\",\"eventType\":\"user.audio.stop\"}";
    return SendFrame(nullptr, 0, json, out_error);
}

bool QwenFreeProtoAsrSession::SendFrame(const void* first,
                                        size_t firstBytes,
                                        const std::string& second,
                                        std::wstring& out_error) {
    if (firstBytes > static_cast<size_t>(UINT32_MAX) ||
        second.size() > static_cast<size_t>(UINT32_MAX) ||
        firstBytes > kMaxAsrFrameBytes ||
        second.size() > kMaxAsrFrameBytes ||
        second.size() > kMaxAsrFrameBytes - 8u ||
        firstBytes > kMaxAsrFrameBytes - 8u - second.size()) {
        out_error = L"Qwen frame segment too large";
        return false;
    }
    if (firstBytes > 0 && !first) {
        out_error = L"Qwen frame has null PCM segment";
        return false;
    }

    std::vector<BYTE> message;
    if (!BuildAsrBinaryFrame(first, firstBytes, second, message)) {
        // The explicit checks above provide the more useful error text for
        // oversize/null inputs; this is only a defensive guard for the shared
        // helper's contract.
        out_error = L"Qwen frame construction failed";
        return false;
    }

    OperationScope operation(*this);
    if (!operation) {
        out_error = L"Qwen WebSocket operation cancelled";
        return false;
    }
    HINTERNET ws = hWebSocket_.load();
    if (!ws) {
        out_error = L"Qwen WebSocket is closed";
        return false;
    }
    DWORD rc = WinHttpWebSocketSend(
        ws, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE,
        message.data(), static_cast<DWORD>(message.size()));
    if (rc != ERROR_SUCCESS) {
        out_error = L"Qwen frame send failed: " + std::to_wstring(rc);
        return false;
    }
    return true;
}

bool QwenFreeProtoAsrSession::SendCommit(const void* pcm,
                                         size_t bytes,
                                         std::wstring& out_error) {
    std::string sessionId;
    std::string roundId;
    {
        std::lock_guard<std::mutex> lock(identifiersMutex_);
        sessionId = sessionId_;
        roundId = roundId_;
    }
    if (sessionId.empty() || roundId.empty()) {
        out_error = L"cannot send audio before user.session.start response";
        return false;
    }
    const std::string json =
        "{\"data\":{\"bitDepth\":16,\"channel\":\"mono\","
        "\"format\":\"pcm\",\"roundId\":\"" + JsonEscape(roundId) +
        "\",\"sampleRate\":16000,\"sessionId\":\"" + JsonEscape(sessionId) +
        "\",\"type\":\"manualStreamStop\"},\"eventId\":\"" +
        JsonEscape(channelId_) +
        "_" + std::to_string(eventSequence_++) +
        "\",\"eventType\":\"user.audio.commit\"}";
    return SendFrame(pcm, bytes, json, out_error);
}

AsrFrame QwenFreeProtoAsrSession::RecvFrame(DWORD timeoutMs) {
    AsrFrame frame;
    OperationScope operation(*this);
    if (!operation) {
        frame.type = FrameType::Closed;
        return frame;
    }

    HINTERNET ws = hWebSocket_.load();
    if (!ws) {
        frame.type = FrameType::Closed;
        return frame;
    }

    // Worker 需要在收包间隙持续发送 PCM。WinHTTP 没有
    // WINHTTP_OPTION_WEB_SOCKET_RECEIVE_TIMEOUT；122 是接收缓冲区大小，
    // 不能拿来设置超时。把 session 的真实 receive timeout 临时设为本次
    // pump 的轮询间隔，避免空读阻塞数秒后 watchdog 先关闭 WebSocket。
    const ULONGLONG started = GetTickCount64();
    const ULONGLONG deadline = timeoutMs == 0
        ? started
        : started + static_cast<ULONGLONG>(timeoutMs);
    HINTERNET session = hSession_.load();
    const auto restoreReceiveTimeout = [this, session]() {
        if (session) {
            WinHttpSetTimeouts(
                session,
                static_cast<int>(cfg_.connectTimeoutMs),
                static_cast<int>(cfg_.connectTimeoutMs),
                static_cast<int>(cfg_.recvTimeoutMs),
                static_cast<int>(cfg_.recvTimeoutMs));
        }
    };

    std::string json;
    BYTE buf[8192];
    for (;;) {
        if (IsCancellationRequested()) {
            restoreReceiveTimeout();
            frame.type = FrameType::Closed;
            return frame;
        }

        const ULONGLONG now = GetTickCount64();
        if (timeoutMs != 0 && now >= deadline) {
            restoreReceiveTimeout();
            frame.type = FrameType::Timeout;
            return frame;
        }
        const DWORD remaining = timeoutMs == 0
            ? 1u
            : static_cast<DWORD>((std::min<ULONGLONG>)
                (deadline - now, static_cast<ULONGLONG>(kReceivePollSliceMs)));
        if (session) {
            // Keep every receive operation short so Close/Abort never has to
            // race an arbitrarily long WinHTTP receive. The outer deadline
            // still preserves the caller's total wait (e.g. 3s for start).
            WinHttpSetTimeouts(
                session,
                static_cast<int>(cfg_.connectTimeoutMs),
                static_cast<int>(cfg_.connectTimeoutMs),
                static_cast<int>(cfg_.recvTimeoutMs),
                static_cast<int>((std::max<DWORD>)(1u, remaining)));
        }

        DWORD readLen = 0;
        WINHTTP_WEB_SOCKET_BUFFER_TYPE bufType =
            WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE;
        DWORD rc = WinHttpWebSocketReceive(ws, buf, sizeof(buf),
                                           &readLen, &bufType);
        if (rc != ERROR_SUCCESS) {
            if (rc == ERROR_WINHTTP_TIMEOUT) {
                if (timeoutMs == 0 || GetTickCount64() >= deadline) {
                    restoreReceiveTimeout();
                    frame.type = FrameType::Timeout;
                    return frame;
                }
                continue;
            } else if (rc == ERROR_WINHTTP_OPERATION_CANCELLED) {
                // Abort/Close intentionally cancels an outstanding receive.
                // Do not turn expected WinHTTP 12017 into a visible ASR
                // failure or dispatch a bogus transcript.
                frame.type = FrameType::Closed;
            } else {
                frame.type = FrameType::Error;
                frame.errorCode = std::to_wstring(rc);
                frame.errorMsg = L"WinHttpWebSocketReceive failed: " + std::to_wstring(rc);
                LogErr(L"recv", frame.errorMsg);
            }
            restoreReceiveTimeout();
            return frame;
        }

        if (bufType == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
            restoreReceiveTimeout();
            frame.type = FrameType::Closed;
            return frame;
        }

        const bool messagePart =
            bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_BINARY_FRAGMENT_BUFFER_TYPE;
        if (!messagePart) {
            restoreReceiveTimeout();
            frame.type = FrameType::Timeout;
            return frame;
        }
        if (readLen > kMaxReceivePayloadBytes -
                       (std::min)(receivePayload_.size(), kMaxReceivePayloadBytes)) {
            receivePayload_.clear();
            restoreReceiveTimeout();
            frame.type = FrameType::Error;
            frame.errorCode = L"payload_too_large";
            frame.errorMsg = L"Qwen ASR response payload too large";
            LogErr(L"recv", frame.errorMsg);
            return frame;
        }
        receivePayload_.append(reinterpret_cast<const char*>(buf), readLen);

        const bool complete =
            bufType == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE ||
            bufType == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
        if (complete) {
            json.swap(receivePayload_);
            break;
        }
    }

    if (session) {
        WinHttpSetTimeouts(
            session,
            static_cast<int>(cfg_.connectTimeoutMs),
            static_cast<int>(cfg_.connectTimeoutMs),
            static_cast<int>(cfg_.recvTimeoutMs),
            static_cast<int>(cfg_.recvTimeoutMs));
    }

    frame = ParseAsrResponseJson(json);
    LogErr(L"recv_json", L"action=" +
           (frame.action.empty() ? std::wstring(L"<empty>") : frame.action) +
           L" bytes=" + std::to_wstring(json.size()) +
           L" text_bytes=" + std::to_wstring(frame.text.size()) +
           L" final=" + std::to_wstring(
               frame.type == FrameType::Final ? 1 : 0) +
           L" session_id=" + std::to_wstring(frame.sessionId.empty() ? 0 : 1) +
           L" round_id=" + std::to_wstring(frame.roundId.empty() ? 0 : 1));
    if (frame.type == FrameType::Error) {
        LogErr(L"recv_error", L"code=" +
               (frame.errorCode.empty() ? L"<empty>" : frame.errorCode) +
               L" message_wlen=" + std::to_wstring(frame.errorMsg.size()) +
               L" payload_bytes=" + std::to_wstring(json.size()));
    }
    if (!frame.sessionId.empty() || !frame.roundId.empty()) {
        std::lock_guard<std::mutex> lock(identifiersMutex_);
        if (!frame.sessionId.empty()) {
            sessionId_ = WideToUtf8(frame.sessionId);
        }
        if (!frame.roundId.empty()) {
            roundId_ = WideToUtf8(frame.roundId);
        }
    }
    return frame;
}

void QwenFreeProtoAsrSession::Close() {
    // Wake a receive/send operation without erasing an external Abort
    // request. Connect() may reset this internal flag for a replay, but a
    // concurrent RequestCancel() remains sticky until the session is
    // discarded.
    closeRequested_.store(true, std::memory_order_release);
    operationCv_.notify_all();

    HINTERNET ws = nullptr;
    HINTERNET connection = nullptr;
    HINTERNET session = nullptr;
    {
        std::unique_lock<std::mutex> lock(operationMutex_);
        // Close() is idempotent, but serialize two concurrent callers so one
        // cannot publish a new operation while the other is releasing handles.
        operationCv_.wait(lock, [this]() { return !closing_; });
        closing_ = true;
        operationCv_.wait(lock, [this]() { return activeOperations_ == 0; });
        ws = hWebSocket_.exchange(nullptr);
        connection = hConnect_.exchange(nullptr);
        session = hSession_.exchange(nullptr);
    }

    if (ws) WinHttpCloseHandle(ws);
    if (connection) WinHttpCloseHandle(connection);
    if (session) WinHttpCloseHandle(session);

    {
        std::lock_guard<std::mutex> lock(operationMutex_);
        closeRequested_.store(false, std::memory_order_release);
        closing_ = false;
    }
    operationCv_.notify_all();

    // pendingPcm_/receivePayload_/identifiers are worker-owned state. They are
    // deliberately not touched here; SendStart resets them after the receiver
    // has been joined for the next transport round.
}

} // namespace qwen_free_proto_asr
