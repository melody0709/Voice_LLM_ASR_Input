#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

// qwen_free_proto_asr
//
// 千问 IME 免费后端协议层 - ASR WebSocket。
//
// 逆向来源：reverse/work/PROTOCOL.md §3 + reverse/work/ghidra/output/shell_ffi/
//   - 端点：wss://speech-asr.qianwen.com/ws/v1/asr
//   - 鉴权：query 使用 biz_id/chid/.../ut/ve；ut 是 UTDID 的 WSG 加密结果
//     Base64，sign 是 unet.dll 生成的 44 字符签名。
//   - 握手头：原版使用固定 User-Agent、Origin 和 x-u-vcode=<tm>。
//   - 协议：握手后发送 binary 双段帧：两个 u32 大端长度，后跟 PCM/JSON
//     两个 segment。启动和停止帧的 PCM segment 为空；音频提交帧包含 PCM
//     和 user.audio.commit JSON。
//
// 服务端回包是 UTF-8/Binary 承载的 JSON。user.session.start 成功回包中的
// data.sessionId/data.roundId 必须回填到后续 commit/stop 帧。
//
// 调用方负责 WASAPI 采集 PCM 并送 SendPcmChunk，本模块只管协议。

namespace qwen_free_proto_asr {

#ifndef WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET
#define WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET 114
#endif
#ifndef WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL
#define WINHTTP_OPTION_WEB_SOCKET_KEEPALIVE_INTERVAL 116
#endif
#ifndef WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT
#define WINHTTP_OPTION_WEB_SOCKET_CLOSE_TIMEOUT 115
#endif

constexpr const wchar_t* kDefaultAsrHost = L"speech-asr.qianwen.com";
constexpr const wchar_t* kDefaultAsrPath = L"/ws/v1/asr";
constexpr const char*    kDefaultAppkey   = "qianwen_pc_voice";
constexpr const char*    kDefaultVe       = "0.1.0";
constexpr int            kDefaultVersion   = 2;
constexpr INTERNET_PORT  kAsrPort         = INTERNET_DEFAULT_HTTPS_PORT;

// A debug endpoint is intentionally a complete ws:// or wss:// URL rather
// than a separately configurable host/path pair.  Parse it once so WinHTTP
// connects to the override host and port (and uses plaintext only for ws://)
// instead of accidentally retaining the production endpoint settings.
struct AsrWebSocketEndpoint {
    std::wstring host;
    std::wstring pathAndQuery = L"/";
    INTERNET_PORT port = INTERNET_DEFAULT_HTTPS_PORT;
    bool secure = true;
};

inline bool ParseAsrWebSocketUrl(const std::wstring& url,
                                 AsrWebSocketEndpoint& endpoint) {
    endpoint = {};
    const std::size_t schemeEnd = url.find(L"://");
    if (schemeEnd == std::wstring::npos || schemeEnd == 0) return false;

    std::wstring scheme = url.substr(0, schemeEnd);
    for (wchar_t& c : scheme) {
        if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c - L'A' + L'a');
    }
    if (scheme == L"wss") {
        endpoint.secure = true;
        endpoint.port = INTERNET_DEFAULT_HTTPS_PORT;
    } else if (scheme == L"ws") {
        endpoint.secure = false;
        endpoint.port = INTERNET_DEFAULT_HTTP_PORT;
    } else {
        return false;
    }

    const std::size_t authorityStart = schemeEnd + 3;
    if (authorityStart >= url.size() ||
        url.find_first_of(L" \t\r\n#", authorityStart) != std::wstring::npos) {
        return false;
    }
    const std::size_t pathStart = url.find_first_of(L"/?", authorityStart);
    const std::wstring authority = pathStart == std::wstring::npos
        ? url.substr(authorityStart)
        : url.substr(authorityStart, pathStart - authorityStart);
    if (authority.empty() || authority.find(L'@') != std::wstring::npos) {
        return false;
    }

    const auto parsePort = [](const std::wstring& text,
                              INTERNET_PORT& port) -> bool {
        if (text.empty()) return false;
        unsigned long value = 0;
        for (const wchar_t c : text) {
            if (c < L'0' || c > L'9') return false;
            const unsigned long digit = static_cast<unsigned long>(c - L'0');
            if (value > (65535ul - digit) / 10ul) return false;
            value = value * 10ul + digit;
        }
        if (value == 0) return false;
        port = static_cast<INTERNET_PORT>(value);
        return true;
    };

    std::wstring portText;
    bool hasExplicitPort = false;
    if (authority.front() == L'[') {
        const std::size_t closing = authority.find(L']');
        if (closing == std::wstring::npos || closing == 1) return false;
        endpoint.host = authority.substr(1, closing - 1);
        const std::wstring suffix = authority.substr(closing + 1);
        if (!suffix.empty()) {
            if (suffix.front() != L':') return false;
            hasExplicitPort = true;
            portText = suffix.substr(1);
        }
    } else {
        const std::size_t colon = authority.rfind(L':');
        if (colon == std::wstring::npos) {
            endpoint.host = authority;
        } else {
            // Unbracketed IPv6 is ambiguous with a port and is rejected.
            if (authority.find(L':') != colon) return false;
            endpoint.host = authority.substr(0, colon);
            hasExplicitPort = true;
            portText = authority.substr(colon + 1);
        }
    }
    if (endpoint.host.empty()) return false;
    if (hasExplicitPort && !parsePort(portText, endpoint.port)) return false;

    if (pathStart == std::wstring::npos) {
        endpoint.pathAndQuery = L"/";
    } else if (url[pathStart] == L'?') {
        endpoint.pathAndQuery = L"/" + url.substr(pathStart);
    } else {
        endpoint.pathAndQuery = url.substr(pathStart);
    }
    return !endpoint.pathAndQuery.empty();
}

// The ASR service is sensitive to both query order and the placement of the
// final sign field.  Keep the unsigned query construction in one small,
// dependency-free helper so the production path and offline regression test
// exercise exactly the same wire shape.
struct AsrQueryFields {
    std::string appkey;
    std::string channelId;
    std::string utdid;
    std::string vcode;
    std::string encryptedUtdid;
    std::string ve = kDefaultVe;
    int version = kDefaultVersion;
};

inline std::string BuildAsrSignContent(const AsrQueryFields& fields) {
    std::ostringstream out;
    out << "biz_id=qwen_command"
        << "&chid=" << fields.channelId
        << "&fr=win"
        << "&from=" << fields.appkey
        << "&mt=" << fields.utdid
        << "&nt=99"
        << "&nw=0"
        << "&pr=qwen"
        << "&tm=" << fields.vcode
        << "&uc_param_str=prfrutvemtntnwkpst"
        << "&ut=" << fields.encryptedUtdid
        << "&ve=" << fields.ve
        << "&version=" << fields.version;
    return out.str();
}

inline std::string BuildAsrQuery(const AsrQueryFields& fields,
                                 const std::string& sign) {
    return BuildAsrSignContent(fields) + "&sign=" + sign;
}

// The WebSocket binary payload is two length-prefixed segments, not two
// lengths followed by both payloads:
//   [BE32 pcm_length][pcm][BE32 json_length][json]
// Keeping this layout in a shared helper prevents the stop/tail path from
// drifting away from the regular audio commit path.
inline bool BuildAsrBinaryFrame(const void* first,
                                size_t firstBytes,
                                const std::string& second,
                                std::vector<BYTE>& message) {
    if (firstBytes > static_cast<size_t>(UINT32_MAX) ||
        second.size() > static_cast<size_t>(UINT32_MAX) ||
        firstBytes > (std::numeric_limits<size_t>::max)() - 8u ||
        second.size() > (std::numeric_limits<size_t>::max)() - 8u - firstBytes ||
        (firstBytes > 0 && !first)) {
        message.clear();
        return false;
    }

    const auto appendBe32 = [&message](size_t value) {
        const uint32_t v = static_cast<uint32_t>(value);
        message.push_back(static_cast<BYTE>((v >> 24) & 0xffu));
        message.push_back(static_cast<BYTE>((v >> 16) & 0xffu));
        message.push_back(static_cast<BYTE>((v >> 8) & 0xffu));
        message.push_back(static_cast<BYTE>(v & 0xffu));
    };

    message.clear();
    message.reserve(8 + firstBytes + second.size());
    appendBe32(firstBytes);
    if (firstBytes > 0) {
        const auto* data = static_cast<const BYTE*>(first);
        message.insert(message.end(), data, data + firstBytes);
    }
    appendBe32(second.size());
    message.insert(message.end(), second.begin(), second.end());
    return true;
}

struct AsrConfig {
    std::string utdid;                                   // 设备指纹（必填）
    std::string appkey = kDefaultAppkey;                 // ASR appkey
    std::wstring host = kDefaultAsrHost;                 // WebSocket 主机
    std::wstring path = kDefaultAsrPath;                  // WebSocket 路径
    std::string ve = kDefaultVe;                          // ve 参数
    int version = kDefaultVersion;                        // version 参数
    int sampleRate = 16000;                                // PCM 采样率
    int channels = 1;                                     // PCM 声道数
    DWORD connectTimeoutMs = 6000;                       // 连接超时
    DWORD recvTimeoutMs = 8000;                           // 单帧接收超时
    // 调试：覆盖 URL（用于本地代理抓包）。
    std::wstring debugUrlOverride;
};

// ASR 帧类型。
enum class FrameType {
    Started,      // session 开始
    Partial,      // 中间结果（is_final=false）
    Final,        // 最终结果（is_final=true）
    Error,        // 错误
    Closed,       // 服务端关闭
    Timeout,      // 接收超时
};

struct AsrFrame {
    FrameType type = FrameType::Closed;
    std::wstring action;      // normalized control/event name, when present
    std::wstring text;        // transcript text（Partial/Final 时）
    std::wstring sessionId;   // session_id（Started 时）
    std::wstring roundId;     // roundId（Started/commit ack 时）
    std::wstring errorCode;   // code（Error 时）
    std::wstring errorMsg;    // message（Error 时）
};

struct TestResult {
    bool ok = false;
    std::wstring message;
    DWORD elapsedMs = 0;
};

class QwenFreeProtoAsrSession {
public:
    QwenFreeProtoAsrSession() = default;
    ~QwenFreeProtoAsrSession();

    // 建立签名 URL 并连接 WebSocket。失败时 out_error 含原因。
    bool Connect(const AsrConfig& cfg, std::wstring& out_error);

    // 发送 user.session.start，并等待服务端返回 sessionId/roundId。
    bool SendStart(std::wstring& out_error);

    // 累积 PCM，并按原版 0xf00 字节发送 user.audio.commit。
    bool SendPcm(const void* pcm, size_t bytes, std::wstring& out_error);

    // 发送尾部 commit（如有）和 user.audio.stop。
    bool SendStop(std::wstring& out_error);

    // 接收一帧。timeoutMs=0 表示非阻塞。
    // 返回 FrameType::Timeout 表示暂无数据。
    AsrFrame RecvFrame(DWORD timeoutMs);

    // 主动关闭（不发送 close frame，仅释放句柄）。
    // 可以从调用线程调用；WinHTTP API 与 Close() 之间通过 operation
    // lifetime guard 串行化，避免 load() 后句柄被并发释放。
    void Close();

    // 仅请求取消，不直接触碰 WinHTTP 句柄。用于外部 Abort，避免在
    // worker 正执行同步 WinHTTP API 时跨线程 Close。
    void RequestCancel();

    // 为下一轮 worker-owned 连接清除内部 Close() wakeup 标志。外部
    // RequestCancel() 是 sticky，不能被重连路径清掉。
    bool ResetCancellation();

    // Explicitly start a new logical recording on the same session object.
    // This is only called after the previous worker/receiver has been joined;
    // unlike replay ResetCancellation(), it is allowed to clear the external
    // Abort cancellation that intentionally remains sticky for one session.
    bool ResetCancellationForNewSession();

    bool IsCancellationRequested() const {
        return cancelRequested_.load(std::memory_order_acquire) ||
               closeRequested_.load(std::memory_order_acquire);
    }

    bool IsConnected() const { return hWebSocket_.load() != nullptr; }

private:
    class OperationScope {
    public:
        explicit OperationScope(QwenFreeProtoAsrSession& owner)
            : owner_(owner), entered_(owner.BeginOperation()) {}

        ~OperationScope() {
            if (entered_) owner_.EndOperation();
        }

        OperationScope(const OperationScope&) = delete;
        OperationScope& operator=(const OperationScope&) = delete;

        explicit operator bool() const { return entered_; }

    private:
        QwenFreeProtoAsrSession& owner_;
        bool entered_ = false;
    };

    bool BeginOperation();
    void EndOperation();

    // 构造签名 URL（原版 /ws/v1/asr query）。
    // signContent 输出不含 sign 的待签名串，signWg 输出 44 字符，
    // encryptedUtdid 输出 Base64 的 WSG 加密 UTDID。
    std::wstring BuildSignedUrl(const AsrConfig& cfg,
                                 std::string& signContent,
                                 std::string& signWg,
                                 std::string& encryptedUtdid,
                                 std::string& vcode);

    bool SendFrame(const void* first,
                   size_t firstBytes,
                   const std::string& second,
                   std::wstring& out_error);
    bool SendCommit(const void* pcm,
                    size_t bytes,
                    std::wstring& out_error);

    // Handle values remain atomic for cheap connection-state checks, while
    // operationMutex_/activeOperations_ provide the stronger lifetime rule:
    // Close() waits until no WinHTTP API is using a handle before releasing it.
    std::atomic<HINTERNET> hSession_{nullptr};
    std::atomic<HINTERNET> hConnect_{nullptr};
    std::atomic<HINTERNET> hWebSocket_{nullptr};
    // closeRequested_ is an internal handle-lifetime wakeup. Keep it
    // separate from the external Abort cancellation so a replay's Close()
    // cannot accidentally clear a concurrent user cancellation.
    std::atomic<bool> closeRequested_{false};
    std::atomic<bool> cancelRequested_{false};
    mutable std::mutex operationMutex_;
    std::condition_variable operationCv_;
    size_t activeOperations_ = 0;
    bool closing_ = false;
    // The receiver updates identifiers from server control frames while the
    // worker reads them to build audio/stop frames.
    std::mutex identifiersMutex_;
    AsrConfig cfg_;
    std::string channelId_;
    std::string sessionId_;
    std::string roundId_;
    uint64_t eventSequence_ = 1;
    std::vector<BYTE> pendingPcm_;
    std::string receivePayload_;
};

// Performs the signed WebSocket upgrade and user.session.start handshake
// without sending microphone audio. This verifies UTDID/signing/ASR access.
TestResult TestConnection(const AsrConfig& cfg);

} // namespace qwen_free_proto_asr
