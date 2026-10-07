#include "app/LocalHandover.h"
#include "util/Log.h"
#include "util/Socket.h"

#if defined(_WIN32)
  #include "util/Win.h"
  #include <iphlpapi.h>
#else
  #include <ifaddrs.h>
  #include <net/if.h>
#endif

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

constexpr size_t kMaxRequest = 256 * 1024;   // an answer blob is ~1 KB

void ensureWinsock() { socketsReady(); }

std::string httpResponse(const char* status, const char* contentType,
                         const std::string& body) {
    return std::string("HTTP/1.1 ") + status + "\r\n" +
           "Content-Type: " + contentType + "\r\n" +
           "Content-Length: " + std::to_string(body.size()) + "\r\n" +
           "Cache-Control: no-store\r\n"
           // The viewer is same-origin, but be explicit rather than relying on it.
           "Access-Control-Allow-Origin: *\r\n"
           "Access-Control-Allow-Headers: content-type\r\n"
           "Connection: close\r\n\r\n" + body;
}

bool sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent,
                           static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// Injects the offer into the viewer page so it needs no URL fragment and no
// paste.
//
// This MUST land before the page's own <script>, not after it: the viewer reads
// window.__SOI_OFFER__ during its initial autoload pass, so a tag appended at
// the end of <body> would be evaluated too late and the page would sit there
// reporting "waiting for an offer".
std::string injectOffer(const std::string& html, const std::string& offer) {
    const std::string tag =
        "<script>window.__SOI_OFFER__=\"" + offer + "\";"
        "window.__SOI_POST_ANSWER__=true;</script>\n";

    if (const size_t head = html.find("</head>"); head != std::string::npos)
        return html.substr(0, head) + tag + html.substr(head);

    // No </head>: fall back to just before the first <script>.
    if (const size_t script = html.find("<script"); script != std::string::npos)
        return html.substr(0, script) + tag + html.substr(script);

    return tag + html;
}

} // namespace

std::vector<std::string> localAddresses() {
    ensureWinsock();
    std::vector<std::string> out;

#if !defined(_WIN32)
    // Prefer the real Wi-Fi/Ethernet interfaces (en0, en1, ...) over virtual
    // ones -- VPN tunnels (utun), bridges for VMs (bridge, vmnet), AirDrop
    // (awdl) -- which are almost never the address the friend can reach.
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    std::vector<std::string> preferred, other;
    for (ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET) continue;
        if (!(a->ifa_flags & IFF_UP) || !(a->ifa_flags & IFF_RUNNING)) continue;
        if (a->ifa_flags & IFF_LOOPBACK) continue;

        auto* sin = reinterpret_cast<sockaddr_in*>(a->ifa_addr);
        char text[INET_ADDRSTRLEN] = {};
        if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) continue;
        if (std::strncmp(text, "169.254.", 8) == 0) continue;   // link-local

        const bool physical = a->ifa_name && (std::strncmp(a->ifa_name, "en", 2) == 0 ||
                                              std::strncmp(a->ifa_name, "eth", 3) == 0 ||
                                              std::strncmp(a->ifa_name, "wl", 2) == 0);
        (physical ? preferred : other).emplace_back(text);
    }
    freeifaddrs(list);
    out = std::move(preferred);
    out.insert(out.end(), other.begin(), other.end());
    return out;
#else

    ULONG size = 16 * 1024;
    std::vector<uint8_t> buffer(size);
    auto* addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());

    ULONG rc = GetAdaptersAddresses(AF_INET,
                                    GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                        GAA_FLAG_SKIP_DNS_SERVER,
                                    nullptr, addresses, &size);
    if (rc == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(size);
        addresses = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data());
        rc = GetAdaptersAddresses(AF_INET,
                                  GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                      GAA_FLAG_SKIP_DNS_SERVER,
                                  nullptr, addresses, &size);
    }
    if (rc != NO_ERROR) return out;

    // Prefer real Wi-Fi/Ethernet over virtual adapters (VirtualBox, WSL, VPN
    // tunnels): those are almost never the address the friend can reach.
    std::vector<std::string> preferred, other;

    for (auto* a = addresses; a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp) continue;
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;

        const bool physical = (a->IfType == IF_TYPE_IEEE80211 ||
                               a->IfType == IF_TYPE_ETHERNET_CSMACD);

        for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto* sin = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);

            char text[INET_ADDRSTRLEN] = {};
            if (!inet_ntop(AF_INET, &sin->sin_addr, text, sizeof text)) continue;
            if (std::strncmp(text, "169.254.", 8) == 0) continue;   // link-local

            (physical ? preferred : other).emplace_back(text);
        }
    }

    out = std::move(preferred);
    out.insert(out.end(), other.begin(), other.end());
    return out;
#endif
}

LocalHandover::~LocalHandover() { stop(); }

bool LocalHandover::start(int port, const std::string& viewerHtml,
                          const std::string& offerBlob, AnswerHandler onAnswer) {
    ensureWinsock();

    page_     = injectOffer(viewerHtml, offerBlob);
    onAnswer_ = std::move(onAnswer);

    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        logE("handover: socket() failed ({})", lastSocketError());
        return false;
    }

    // Windows' SO_REUSEADDR lets a second process steal a bound port, so it is
    // only the POSIX meaning -- rebind past TIME_WAIT -- that is wanted here.
#if defined(_WIN32)
    BOOL reuse = TRUE;
#else
    int reuse = 1;
#endif
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof reuse);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(static_cast<uint16_t>(port));

    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == SOCKET_ERROR) {
        logE("handover: cannot bind port {} ({}). Another instance, or a "
             "firewall policy, may be holding it.", port, lastSocketError());
        closesocket(listener);
        return false;
    }

    // Port 0 means "any free port"; read back what we actually got.
    SockLen addrLen = sizeof addr;
    if (getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &addrLen) == 0)
        port_ = ntohs(addr.sin_port);
    else
        port_ = port;

    if (listen(listener, 8) == SOCKET_ERROR) {
        logE("handover: listen() failed ({})", lastSocketError());
        closesocket(listener);
        return false;
    }

    listenSocket_ = static_cast<uintptr_t>(listener);
    running_.store(true);
    thread_ = std::thread([this] { serve(); });

    logI("handover listening on port {}", port_);
    return true;
}

void LocalHandover::stop() {
    if (!running_.exchange(false)) return;

    // The serving thread polls running_, so it is joined BEFORE the socket is
    // closed: closing a descriptor another thread is still waiting on lets the
    // number be reused underneath it.
    if (thread_.joinable()) thread_.join();
    if (listenSocket_ != ~uintptr_t(0)) {
        closesocket(static_cast<SOCKET>(listenSocket_));
        listenSocket_ = ~uintptr_t(0);
    }
}

std::vector<std::string> LocalHandover::urls() const {
    std::vector<std::string> out;
    for (const auto& ip : localAddresses())
        out.push_back("http://" + ip + ":" + std::to_string(port_) + "/");
    return out;
}

void LocalHandover::serve() {
    while (running_.load()) {
        // Polled rather than blocked in accept(), so stop() is seen promptly
        // on every platform -- see util/Socket.h.
        const int ready = waitReadable(static_cast<SOCKET>(listenSocket_), 200);
        if (!running_.load()) break;
        if (ready == 0) continue;
        if (ready < 0) {
            logT("handover: listener failed ({})", lastSocketError());
            break;
        }

        sockaddr_in peer{};
        SockLen peerLen = sizeof peer;
        SOCKET client = accept(static_cast<SOCKET>(listenSocket_),
                               reinterpret_cast<sockaddr*>(&peer), &peerLen);
        if (client == INVALID_SOCKET) {
            if (running_.load()) logT("handover: accept failed ({})", lastSocketError());
            break;
        }

        char ip[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof ip);
        logT("handover: connection from {}", ip);

        handleClient(static_cast<uintptr_t>(client));
    }
}

void LocalHandover::handleClient(uintptr_t clientSocket) {
    SOCKET s = static_cast<SOCKET>(clientSocket);

    // Requests here are tiny and trusted-ish (LAN), so a short receive timeout is
    // enough protection against a client that connects and says nothing.
    setSocketTimeouts(s, 5000);

    std::string request;
    char buffer[4096];

    // Read headers, then any declared body.
    size_t headerEnd = std::string::npos;
    while (request.size() < kMaxRequest) {
        const int n = recv(s, buffer, sizeof buffer, 0);
        if (n <= 0) break;
        request.append(buffer, static_cast<size_t>(n));

        headerEnd = request.find("\r\n\r\n");
        if (headerEnd == std::string::npos) continue;

        size_t contentLength = 0;
        const size_t clPos = request.find("Content-Length:");
        if (clPos != std::string::npos && clPos < headerEnd)
            contentLength = std::strtoul(request.c_str() + clPos + 15, nullptr, 10);

        if (request.size() >= headerEnd + 4 + contentLength) break;
    }

    if (headerEnd == std::string::npos) { closesocket(s); return; }

    const std::string head = request.substr(0, headerEnd);
    const std::string body = request.substr(headerEnd + 4);

    if (head.rfind("GET / ", 0) == 0 || head.rfind("GET /index.html", 0) == 0) {
        sendAll(s, httpResponse("200 OK", "text/html; charset=utf-8", page_));

    } else if (head.rfind("GET /health", 0) == 0) {
        sendAll(s, httpResponse("200 OK", "text/plain", "ok"));

    } else if (head.rfind("OPTIONS ", 0) == 0) {
        sendAll(s, httpResponse("204 No Content", "text/plain", ""));

    } else if (head.rfind("POST /answer", 0) == 0) {
        std::string blob = body;
        while (!blob.empty() && (blob.back() == '\n' || blob.back() == '\r' ||
                                 blob.back() == ' '))
            blob.pop_back();

        const bool accepted = !blob.empty() && onAnswer_ && onAnswer_(blob);
        if (accepted) {
            answered_.store(true);
            logI("handover: answer received from the viewer ({} chars)", blob.size());
            sendAll(s, httpResponse("200 OK", "text/plain", "ok"));
        } else {
            sendAll(s, httpResponse("400 Bad Request", "text/plain", "bad answer"));
        }

    } else {
        sendAll(s, httpResponse("404 Not Found", "text/plain", "not found"));
    }

    // Graceful close so the client reliably sees the full body.
    shutdown(s, SD_SEND);
    closesocket(s);
}

} // namespace soi
