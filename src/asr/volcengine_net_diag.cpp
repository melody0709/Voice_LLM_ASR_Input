#ifndef NOMINMAX
#define NOMINMAX
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>

#include "volcengine_asr.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace volc_asr {
namespace {

constexpr const wchar_t* kVolcHost = L"openspeech.bytedance.com";
constexpr DWORD kProbeTimeoutMs = 450;
constexpr size_t kMaxProbeIps = 12;

int EnsureWinsockReady() {
    static std::once_flag once;
    static int startupResult = WSANOTINITIALISED;
    std::call_once(once, []() {
        WSADATA data = {};
        startupResult = WSAStartup(MAKEWORD(2, 2), &data);
    });
    return startupResult;
}

std::vector<std::string> ResolveVolcIpv4Records(int& resolveErr) {
    resolveErr = 0;
    std::vector<std::string> ips;
    addrinfoW hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    addrinfoW* results = nullptr;
    const int rc = GetAddrInfoW(kVolcHost, L"443", &hints, &results);
    if (rc != 0) {
        resolveErr = rc;
        return ips;
    }

    for (addrinfoW* it = results; it && ips.size() < kMaxProbeIps; it = it->ai_next) {
        if (it->ai_family != AF_INET || !it->ai_addr) continue;
        const auto* sin = reinterpret_cast<const sockaddr_in*>(it->ai_addr);
        char ip[INET_ADDRSTRLEN] = {};
        if (!InetNtopA(AF_INET, const_cast<IN_ADDR*>(&sin->sin_addr), ip, static_cast<DWORD>(sizeof(ip)))) {
            continue;
        }
        if (std::find(ips.begin(), ips.end(), ip) == ips.end()) {
            ips.emplace_back(ip);
        }
    }

    FreeAddrInfoW(results);
    return ips;
}

int ProbeTcp443(const std::string& ip, DWORD timeoutMs, DWORD& elapsedMs) {
    elapsedMs = 0;
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) return WSAGetLastError();

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(443);
    if (InetPtonA(AF_INET, ip.c_str(), &addr.sin_addr) != 1) {
        const int err = WSAGetLastError();
        closesocket(sock);
        return err ? err : WSAEINVAL;
    }

    u_long nonBlocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonBlocking) != 0) {
        const int err = WSAGetLastError();
        closesocket(sock);
        return err;
    }

    const ULONGLONG start = GetTickCount64();
    int rc = connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    if (rc == 0) {
        elapsedMs = static_cast<DWORD>(GetTickCount64() - start);
        closesocket(sock);
        return 0;
    }

    int err = WSAGetLastError();
    if (err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY || err == WSAEINVAL) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        fd_set exceptSet;
        FD_ZERO(&exceptSet);
        FD_SET(sock, &exceptSet);
        timeval tv = {};
        tv.tv_sec = static_cast<long>(timeoutMs / 1000);
        tv.tv_usec = static_cast<long>((timeoutMs % 1000) * 1000);
        rc = select(0, nullptr, &writeSet, &exceptSet, &tv);
        if (rc > 0) {
            int socketErr = 0;
            int optLen = sizeof(socketErr);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketErr), &optLen) == 0) {
                err = socketErr;
            } else {
                err = WSAGetLastError();
            }
        } else if (rc == 0) {
            err = WSAETIMEDOUT;
        } else {
            err = WSAGetLastError();
        }
    }

    elapsedMs = static_cast<DWORD>(GetTickCount64() - start);
    closesocket(sock);
    return err;
}

void LogProxySnapshot(int triggerTraceId) {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG proxy = {};
    if (!WinHttpGetIEProxyConfigForCurrentUser(&proxy)) {
        VolcDebugLog("Volc connect diag #%d: proxy snapshot failed err=%u", triggerTraceId, GetLastError());
        return;
    }

    VolcDebugLog("Volc connect diag #%d: proxy autoDetect=%d autoConfig='%ls' proxy='%ls' bypass='%ls'",
                 triggerTraceId,
                 proxy.fAutoDetect ? 1 : 0,
                 proxy.lpszAutoConfigUrl ? proxy.lpszAutoConfigUrl : L"",
                 proxy.lpszProxy ? proxy.lpszProxy : L"",
                 proxy.lpszProxyBypass ? proxy.lpszProxyBypass : L"");

    if (proxy.lpszAutoConfigUrl) GlobalFree(proxy.lpszAutoConfigUrl);
    if (proxy.lpszProxy) GlobalFree(proxy.lpszProxy);
    if (proxy.lpszProxyBypass) GlobalFree(proxy.lpszProxyBypass);
}

void LogConnectDiagnostics(int triggerTraceId, std::string reason) {
    VolcDebugLog("Volc connect diag #%d: START reason=%s host=openspeech.bytedance.com probeTimeout=%ums",
                 triggerTraceId, reason.c_str(), kProbeTimeoutMs);
    LogProxySnapshot(triggerTraceId);

    const int wsaStartupErr = EnsureWinsockReady();
    if (wsaStartupErr != 0) {
        VolcDebugLog("Volc connect diag #%d: WSAStartup failed err=%d", triggerTraceId, wsaStartupErr);
        return;
    }

    int resolveErr = 0;
    std::vector<std::string> ips = ResolveVolcIpv4Records(resolveErr);
    if (resolveErr != 0) {
        VolcDebugLog("Volc connect diag #%d: DNS GetAddrInfoW failed err=%d", triggerTraceId, resolveErr);
        return;
    }
    if (ips.empty()) {
        VolcDebugLog("Volc connect diag #%d: DNS returned no IPv4 records", triggerTraceId);
        return;
    }

    std::string joined;
    for (const std::string& ip : ips) {
        if (!joined.empty()) joined += ",";
        joined += ip;
    }
    VolcDebugLog("Volc connect diag #%d: DNS IPv4 count=%zu ips=%s",
                 triggerTraceId, ips.size(), joined.c_str());

    size_t okCount = 0;
    for (const std::string& ip : ips) {
        DWORD elapsed = 0;
        const int err = ProbeTcp443(ip, kProbeTimeoutMs, elapsed);
        if (err == 0) okCount++;
        VolcDebugLog("Volc connect diag #%d: TCP443 ip=%s ok=%d err=%d elapsed=%ums",
                     triggerTraceId, ip.c_str(), err == 0 ? 1 : 0, err, elapsed);
    }
    VolcDebugLog("Volc connect diag #%d: DONE ok=%zu/%zu", triggerTraceId, okCount, ips.size());
}

} // namespace

void VolcMaybeLogConnectDiagnosticsAsync(int triggerTraceId, const char* reason, DWORD cooldownMs) {
    if (!g_enableDebugMode) return;

    const ULONGLONG now = GetTickCount64();
    static std::atomic<ULONGLONG> s_lastStartTick{0};
    ULONGLONG prev = s_lastStartTick.load();
    while (true) {
        if (prev != 0 && now - prev < cooldownMs) {
            VolcDebugLog("Volc connect diag #%d: skipped by cooldown (%llums < %ums)",
                         triggerTraceId, now - prev, cooldownMs);
            return;
        }
        if (s_lastStartTick.compare_exchange_weak(prev, now)) break;
    }

    std::string reasonCopy = reason ? reason : "unknown";
    std::thread([triggerTraceId, reasonCopy]() {
        LogConnectDiagnostics(triggerTraceId, reasonCopy);
    }).detach();
}

} // namespace volc_asr
