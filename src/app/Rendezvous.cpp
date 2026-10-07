#include "app/Rendezvous.h"
#include "util/Crypto.h"
#include "util/Log.h"

#if defined(_WIN32)
  #include "util/Win.h"
  #include <windows.h>
  #include <winhttp.h>
#else
  #include "util/HttpCurl.h"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

namespace soi {
namespace {

// No O/0, I/1 or L: these are read aloud and typed on phones.
constexpr char kAlphabet[] = "23456789ABCDEFGHJKMNPQRSTUVWXYZ";
constexpr size_t kAlphabetSize = sizeof(kAlphabet) - 1;   // 31
constexpr size_t kCodeLength   = 6;

struct HttpResponse {
    bool        transportOk = false;
    int         status = 0;
    std::string body;
    std::string error;
    unsigned long winError = 0;   // the WinHTTP error behind a transport failure
};

#if defined(_WIN32)
// WinHTTP handle wrapper: these leak silently and the failure mode is a hung
// process rather than an obvious error.
class HInternet {
public:
    HInternet() = default;
    explicit HInternet(HINTERNET h) : h_(h) {}
    ~HInternet() { if (h_) WinHttpCloseHandle(h_); }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
    HInternet(HInternet&& o) noexcept : h_(std::exchange(o.h_, nullptr)) {}

    HINTERNET get() const { return h_; }
    HINTERNET release() { return std::exchange(h_, nullptr); }
    explicit operator bool() const { return h_ != nullptr; }
private:
    HINTERNET h_ = nullptr;
};

struct ParsedUrl {
    std::wstring host;
    std::wstring path;
    INTERNET_PORT port = 443;
    bool          https = true;
    bool          valid = false;
};

ParsedUrl parseUrl(const std::string& url, const std::string& extraPath) {
    ParsedUrl out;
    std::string u = url;

    if (u.rfind("https://", 0) == 0)      { u.erase(0, 8); out.https = true;  out.port = 443; }
    else if (u.rfind("http://", 0) == 0)  { u.erase(0, 7); out.https = false; out.port = 80;  }
    else return out;

    std::string hostPart = u;
    std::string basePath;
    if (const size_t slash = u.find('/'); slash != std::string::npos) {
        hostPart = u.substr(0, slash);
        basePath = u.substr(slash);
    }
    while (!basePath.empty() && basePath.back() == '/') basePath.pop_back();

    if (const size_t colon = hostPart.find(':'); colon != std::string::npos) {
        out.port = static_cast<INTERNET_PORT>(std::strtoul(hostPart.c_str() + colon + 1, nullptr, 10));
        hostPart = hostPart.substr(0, colon);
    }
    if (hostPart.empty()) return out;

    out.host  = toUtf16(hostPart);
    out.path  = toUtf16(basePath + extraPath);
    out.valid = true;
    return out;
}

// Two long-lived sessions, and which one goes first.
//
// WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY runs WPAD discovery (DNS for wpad.<domain>,
// a DHCP INFORM) before its first request. On a network with no WPAD server that
// stalls until WinHTTP gives up: measured, the first publish of every start timed
// out after 8 s and only the retry got through, so the code took 10-20 s to
// appear. Most machines have no proxy at all, so requests go DIRECT first -- or
// through the proxy the user explicitly configured -- and discovery is only the
// fallback when that route cannot connect. Sessions are kept for the process so
// discovery, if it is ever needed, runs once and is cached.
HINTERNET openSession(DWORD access, const wchar_t* proxy = WINHTTP_NO_PROXY_NAME,
                      const wchar_t* bypass = WINHTTP_NO_PROXY_BYPASS) {
    return WinHttpOpen(L"soi-share/1.0", access, proxy, bypass, 0);
}

struct Sessions {
    HINTERNET primary  = nullptr;   // direct, or the user's explicit proxy
    HINTERNET fallback = nullptr;   // automatic proxy discovery
};

const Sessions& sessions() {
    static const Sessions s = [] {
        Sessions out;
        WINHTTP_CURRENT_USER_IE_PROXY_CONFIG ie{};
        if (WinHttpGetIEProxyConfigForCurrentUser(&ie)) {
            if (ie.lpszProxy)
                out.primary = openSession(WINHTTP_ACCESS_TYPE_NAMED_PROXY, ie.lpszProxy,
                                          ie.lpszProxyBypass ? ie.lpszProxyBypass
                                                             : WINHTTP_NO_PROXY_BYPASS);
            for (LPWSTR p : {ie.lpszProxy, ie.lpszProxyBypass, ie.lpszAutoConfigUrl})
                if (p) GlobalFree(p);
        }
        if (!out.primary) out.primary = openSession(WINHTTP_ACCESS_TYPE_NO_PROXY);
        out.fallback = openSession(WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY);
        if (!out.fallback) out.fallback = openSession(WINHTTP_ACCESS_TYPE_DEFAULT_PROXY);
        return out;
    }();
    return s;
}

// Set once the fallback has worked where the primary did not, so later requests
// stop paying for a route that is known not to connect.
std::atomic<bool> g_preferFallback{false};

// Errors that mean "no connection was made", where another route or a fresh
// connection may well succeed. Anything else is an answer worth reporting.
bool connectFailure(DWORD err) {
    return err == ERROR_WINHTTP_CANNOT_CONNECT || err == ERROR_WINHTTP_TIMEOUT ||
           err == ERROR_WINHTTP_NAME_NOT_RESOLVED || err == ERROR_WINHTTP_CONNECTION_ERROR;
}

// The connect step is capped at 3.5 s. On a network that intermittently drops
// SYNs to one of Cloudflare's addresses, an uncapped connect waits out TCP's
// 21 s retry: measured, every other start took 17-25 s before the code was
// published. A fresh connection usually lands on the other address instead.
constexpr int kConnectTimeoutMs = 3500;

HttpResponse httpRequestOn(HINTERNET session, const std::string& baseUrl,
                           const std::string& path, const char* method,
                           const std::string& body, int timeoutMs) {
    HttpResponse out;

    const ParsedUrl url = parseUrl(baseUrl, path);
    if (!url.valid) { out.error = "malformed rendezvous URL: " + baseUrl; return out; }
    if (!session) { out.error = "WinHttpOpen failed"; return out; }

    HInternet connect(WinHttpConnect(session, url.host.c_str(), url.port, 0));
    if (!connect) { out.error = "cannot connect to the rendezvous host"; return out; }

    HInternet request(WinHttpOpenRequest(
        connect.get(), toUtf16(method).c_str(), url.path.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        url.https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) { out.error = "WinHttpOpenRequest failed"; return out; }

    // Per-request, so a name-resolution or connect stall gives up quickly and
    // the caller's retry gets a fresh go, while still allowing a slow-but-alive
    // link the full budget to send and receive.
    WinHttpSetTimeouts(request.get(), std::min(timeoutMs, 5000),
                       std::min(timeoutMs, kConnectTimeoutMs), timeoutMs, timeoutMs);

    const std::wstring headers = L"Content-Type: application/json\r\n";

    if (!WinHttpSendRequest(request.get(),
                            body.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
                            body.empty() ? 0 : static_cast<DWORD>(-1),
                            body.empty() ? WINHTTP_NO_REQUEST_DATA
                                         : const_cast<char*>(body.data()),
                            static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0)) {
        out.winError = GetLastError();
        out.error = "request failed (" + std::to_string(out.winError) + ")";
        return out;
    }
    if (!WinHttpReceiveResponse(request.get(), nullptr)) {
        out.winError = GetLastError();
        out.error = "no response (" + std::to_string(out.winError) + ")";
        return out;
    }

    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(),
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    out.status = static_cast<int>(status);

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available) || available == 0) break;
        std::vector<char> chunk(available);
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &read) || read == 0) break;
        out.body.append(chunk.data(), read);
        if (out.body.size() > 256 * 1024) break;   // a blob is ~1 KB
    }

    out.transportOk = true;
    return out;
}

// One request/response. Deliberately synchronous: this runs during setup, not
// in the frame path. Every call made through here is idempotent (publish
// overwrites, polls read, delete deletes), so retrying on the other route is safe.
HttpResponse httpRequest(const std::string& baseUrl, const std::string& path,
                         const char* method, const std::string& body,
                         int timeoutMs) {
    const Sessions& s = sessions();
    const bool viaFallback = g_preferFallback.load();

    HttpResponse first = httpRequestOn(viaFallback ? s.fallback : s.primary,
                                       baseUrl, path, method, body, timeoutMs);
    if (first.transportOk) return first;

    // Once more on the same route with a fresh connection before trying the
    // other one: a connect that timed out has usually just picked an address
    // the network is dropping packets to.
    if (connectFailure(first.winError)) {
        HttpResponse again = httpRequestOn(viaFallback ? s.fallback : s.primary,
                                           baseUrl, path, method, body, timeoutMs);
        if (again.transportOk) return again;
    }

    HINTERNET other = viaFallback ? s.primary : s.fallback;
    if (!other) return first;
    HttpResponse second = httpRequestOn(other, baseUrl, path, method, body, timeoutMs);
    if (!second.transportOk) return first;

    g_preferFallback.store(!viaFallback);
    logI("rendezvous reachable {}; using that from now on",
         viaFallback ? "directly" : "only through the system proxy");
    return second;
}

// The relay socket: a WinHTTP WebSocket, so it uses Schannel and the same proxy
// routes as every other rendezvous call.
class WinRelaySocket final : public RelaySocket {
public:
    WinRelaySocket(HINTERNET connect, HINTERNET ws) : connect_(connect), ws_(ws) {}
    ~WinRelaySocket() override {
        close();
        if (connect_) WinHttpCloseHandle(connect_);
    }

    bool sendBinary(const uint8_t* data, size_t len) override {
        return send(WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, data, len);
    }
    bool sendText(const std::string& text) override {
        return send(WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE, text.data(), text.size());
    }

    bool receive(std::string& message, bool& binary) override {
        message.clear();
        std::vector<char> buf(64 * 1024);
        for (;;) {
            const HINTERNET h = handle();
            if (!h) return false;
            DWORD got = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE type{};
            const DWORD err = WinHttpWebSocketReceive(h, buf.data(), static_cast<DWORD>(buf.size()),
                                                      &got, &type);
            if (err != NO_ERROR || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                closed_.store(true);
                return false;
            }
            message.append(buf.data(), got);
            if (message.size() > 2 * 1024 * 1024) { closed_.store(true); return false; }
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE ||
                type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                binary = type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE;
                return true;
            }
        }
    }

    // Abortive on purpose: a graceful close handshake can block for as long as
    // the far side takes to answer, and this runs on the shutdown path. Closing
    // the handle unblocks a send or receive pending on another thread. The
    // handle is taken under the lock and closed outside it, so a thread that
    // needs the lock to finish its own call can never deadlock against this.
    void close() override {
        closed_.store(true);
        HINTERNET h = nullptr;
        {
            std::lock_guard lk(mtx_);
            h = std::exchange(ws_, nullptr);
        }
        if (h) WinHttpCloseHandle(h);
    }

private:
    HINTERNET handle() {
        std::lock_guard lk(mtx_);
        return closed_.load() ? nullptr : ws_;
    }

    bool send(WINHTTP_WEB_SOCKET_BUFFER_TYPE type, const void* data, size_t len) {
        const HINTERNET h = handle();
        if (!h) return false;
        std::lock_guard lk(sendMtx_);   // one message at a time on the wire
        if (WinHttpWebSocketSend(h, type, const_cast<void*>(data), static_cast<DWORD>(len)) !=
            NO_ERROR) {
            closed_.store(true);
            return false;
        }
        return true;
    }

    HINTERNET         connect_ = nullptr;
    HINTERNET         ws_      = nullptr;
    std::mutex        mtx_;       // guards ws_ only, never held across a call
    std::mutex        sendMtx_;
    std::atomic<bool> closed_{false};
};

std::unique_ptr<RelaySocket> openWebSocketOn(HINTERNET session, const std::string& baseUrl,
                                             const std::string& path, std::string& error,
                                             DWORD& winError) {
    winError = 0;
    const ParsedUrl url = parseUrl(baseUrl, path);
    if (!url.valid) { error = "malformed rendezvous URL: " + baseUrl; return nullptr; }
    if (!session) { error = "WinHttpOpen failed"; return nullptr; }

    HInternet connect(WinHttpConnect(session, url.host.c_str(), url.port, 0));
    if (!connect) { error = "cannot connect to the rendezvous host"; return nullptr; }
    HInternet request(WinHttpOpenRequest(connect.get(), L"GET", url.path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         url.https ? WINHTTP_FLAG_SECURE : 0));
    if (!request) { error = "WinHttpOpenRequest failed"; return nullptr; }

    // The same connect cap as every other call; receive waits forever, because
    // a relay that is quiet is not a relay that has failed.
    WinHttpSetTimeouts(request.get(), 5000, kConnectTimeoutMs, 10000, 0);
    WinHttpSetOption(request.get(), WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0);
    if (!WinHttpSendRequest(request.get(), WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) {
        winError = GetLastError();
        error = "relay connect failed (" + std::to_string(winError) + ")";
        return nullptr;
    }
    DWORD status = 0, statusSize = sizeof(status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                        WINHTTP_NO_HEADER_INDEX);
    if (status != 101) {
        error = "the rendezvous refused the relay (HTTP " + std::to_string(status) + ")";
        return nullptr;
    }
    HINTERNET ws = WinHttpWebSocketCompleteUpgrade(request.get(), 0);
    if (!ws) {
        error = "relay upgrade failed (" + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    // The request handle is finished with once the upgrade completes; the
    // connection handle must outlive the WebSocket.
    return std::make_unique<WinRelaySocket>(connect.release(), ws);
}

#else
// One request/response through libcurl. Deliberately synchronous: this runs
// during setup, not in the frame path. Proxy handling (environment, then the
// System Settings proxy) lives in util/HttpCurl.cpp.
HttpResponse httpRequest(const std::string& baseUrl, const std::string& path,
                         const char* method, const std::string& body,
                         int timeoutMs) {
    HttpResponse out;
    if (baseUrl.rfind("https://", 0) != 0 && baseUrl.rfind("http://", 0) != 0) {
        out.error = "malformed rendezvous URL: " + baseUrl;
        return out;
    }
    std::string base = baseUrl;
    // Matches the WinHTTP path: a trailing slash on the base is not doubled.
    const size_t schemeEnd = base.find("://") + 3;
    while (base.size() > schemeEnd && base.back() == '/') base.pop_back();

    CurlRequest req;
    req.url              = base + path;
    req.method           = method;
    req.body             = body;
    req.connectTimeoutMs = 3500;
    req.totalTimeoutMs   = timeoutMs + 5000;
    req.userAgent        = "soi-share/1.0";
    if (!body.empty()) req.headers.push_back("Content-Type: application/json");

    const CurlResponse r = curlPerform(req);
    out.transportOk = r.transportOk;
    out.status      = r.status;
    out.body        = r.body;
    out.error       = r.error;
    return out;
}

#endif

// Pulls one string field out of a small, known-shape JSON response. A full
// parser is not worth a dependency for two fields.
std::string jsonString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) return {};
    pos = json.find('"', pos);
    if (pos == std::string::npos) return {};
    ++pos;

    std::string out;
    while (pos < json.size() && json[pos] != '"') {
        if (json[pos] == '\\' && pos + 1 < json.size()) ++pos;   // no escapes expected
        out.push_back(json[pos++]);
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (static_cast<unsigned char>(c) < 0x20) { /* not expected in a blob */ }
        else out.push_back(c);
    }
    return out;
}

} // namespace

std::string generateShareCode() {
    uint8_t raw[kCodeLength] = {};
    if (!randomBytes(raw, sizeof raw)) {
        logE("the system random generator failed; refusing to emit a predictable share code");
        return {};
    }

    // 256 is not a multiple of 31, so a plain modulo is very slightly biased
    // toward the first few symbols. Reject the biased tail instead -- this code
    // is the only credential protecting the session.
    constexpr unsigned kLimit =
        256u - (256u % static_cast<unsigned>(kAlphabetSize));   // 248

    std::string code;
    code.reserve(kCodeLength);
    for (size_t i = 0; i < kCodeLength; ++i) {
        unsigned value = raw[i];
        while (value >= kLimit) {
            uint8_t again = 0;
            if (!randomBytes(&again, 1)) return {};
            value = again;
        }
        code.push_back(kAlphabet[value % kAlphabetSize]);
    }
    return code;
}

std::string formatShareCode(const std::string& code) {
    if (code.size() != kCodeLength) return code;
    return code.substr(0, 3) + "-" + code.substr(3);
}

std::string roomIdForCode(const std::string& code) {
    uint8_t digest[32] = {};
    if (!sha256(code.data(), code.size(), digest)) return {};
    return toHex(digest, sizeof digest);
}

std::string generateSessionId() {
    return randomHex(8);
}

RendezvousResult publishOffer(const std::string& baseUrl, const std::string& roomId,
                              const std::string& sessionId,
                              const std::string& sealedOffer) {
    RendezvousResult result;

    // owner and caps are ignored by a rendezvous that predates the relay, and a
    // viewer page that predates it never asks for one, so both stay compatible.
    const std::string body =
        "{\"id\":\"" + jsonEscape(roomId) + "\",\"session\":\"" + jsonEscape(sessionId) +
        "\",\"offer\":\"" + jsonEscape(sealedOffer) + "\",\"owner\":\"" +
        jsonEscape(hostOwnerToken()) + "\",\"caps\":[" + (relaySupported() ? "\"relay\"" : "") +
        "]}";

    // Publishing is the one call the whole short-code route depends on, and a
    // single timeout used to cost the entire session its code: the sender fell
    // back to the manual route and the friend was left staring at "nobody is
    // sharing with that code" while the daemon sat there running. A flaky link
    // is not a reason to give up the code, so try a few times first.
    for (int attempt = 1; attempt <= 3; ++attempt) {
        const HttpResponse res = httpRequest(baseUrl, "/api/room", "POST", body, 15000);

        if (res.transportOk) {
            if (res.status == 201) { result.ok = true; return result; }
            if (res.status == 409) {
                result.error = "that code is already in use; try again";
                return result;      // not a transport problem; retrying is pointless
            }
            result.error =
                "rendezvous rejected the offer (HTTP " + std::to_string(res.status) + ")";
            return result;
        }

        result.error = res.error.empty() ? "could not reach the rendezvous" : res.error;
        if (attempt < 3) {
            logT("publish attempt {} failed ({}), retrying", attempt, result.error);
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
        }
    }
    return result;
}

RendezvousResult waitForAnswer(const std::string& baseUrl, const std::string& roomId,
                               const std::string& sessionId, int timeoutSeconds,
                               std::string& sealedAnswer,
                               const std::function<bool()>& shouldStop) {
    RendezvousResult result;
    sealedAnswer.clear();

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(timeoutSeconds);
    const std::string path = "/api/room/" + roomId + "/answer?session=" + sessionId;

    int consecutiveErrors = 0;

    // Sleep in short steps so a caller that wants to stop is not left waiting
    // out a full poll interval.
    const auto nap = [&shouldStop](int seconds) {
        for (int i = 0; i < seconds * 10; ++i) {
            if (shouldStop && shouldStop()) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    };

    while (std::chrono::steady_clock::now() < deadline) {
        if (shouldStop && shouldStop()) return result;   // operator asked us to stop

        const HttpResponse res = httpRequest(baseUrl, path, "GET", "", 15000);

        if (!res.transportOk) {
            // A flaky link should not abort a session someone is about to join;
            // give up only if it stays broken.
            if (++consecutiveErrors >= 10) {
                result.error = res.error.empty() ? "rendezvous unreachable" : res.error;
                return result;
            }
            logT("rendezvous poll failed ({}), retrying", res.error);
            nap(2);
            continue;
        }
        consecutiveErrors = 0;

        if (res.status == 200) {
            sealedAnswer = jsonString(res.body, "answer");
            if (sealedAnswer.empty()) {
                result.error = "rendezvous returned a malformed answer";
                return result;
            }
            result.ok = true;
            return result;
        }
        if (res.status == 404) {
            result.error = "the room expired before anyone joined";
            return result;
        }
        // 204 => nobody has joined yet.
        nap(2);
    }
    return result;   // ok=false, no error: simply nobody joined
}

RendezvousResult pollAnswerOnce(const std::string& baseUrl, const std::string& roomId,
                                const std::string& sessionId, std::string& sealedAnswer) {
    RendezvousResult result;
    sealedAnswer.clear();
    const HttpResponse res = httpRequest(
        baseUrl, "/api/room/" + roomId + "/answer?session=" + sessionId, "GET", "", 5000);
    if (!res.transportOk) {
        result.error = res.error.empty() ? "rendezvous unreachable" : res.error;
        return result;
    }
    if (res.status == 200) sealedAnswer = jsonString(res.body, "answer");
    result.ok = res.status == 200 || res.status == 204;
    if (!result.ok) result.error = "rendezvous returned HTTP " + std::to_string(res.status);
    return result;
}

const std::string& hostOwnerToken() {
    static const std::string token = randomHex(16);
    return token;
}

#if defined(_WIN32)
bool relaySupported() { return !hostOwnerToken().empty(); }

std::unique_ptr<RelaySocket> openRelaySocket(const std::string& baseUrl,
                                             const std::string& roomId,
                                             const std::string& sessionId,
                                             std::string& error) {
    const std::string path = "/api/room/" + roomId + "/relay?session=" + sessionId +
                             "&role=host&owner=" + hostOwnerToken();
    // The route that last worked, a fresh connection on it, then the other one:
    // the same order as every other call, for the same reasons.
    const Sessions& s = sessions();
    const bool viaFallback = g_preferFallback.load();
    const HINTERNET order[] = {viaFallback ? s.fallback : s.primary,
                               viaFallback ? s.fallback : s.primary,
                               viaFallback ? s.primary : s.fallback};
    for (int i = 0; i < 3; ++i) {
        DWORD winError = 0;
        if (auto ws = openWebSocketOn(order[i], baseUrl, path, error, winError)) return ws;
        if (!connectFailure(winError)) return nullptr;   // refused, not unreachable
    }
    return nullptr;
}
#else
// The relay is Windows-only for now; macOS hosts do not advertise it, so their
// viewers keep the direct path exactly as before.
bool relaySupported() { return false; }

std::unique_ptr<RelaySocket> openRelaySocket(const std::string&, const std::string&,
                                             const std::string&, std::string& error) {
    error = "the relay is not available on this platform";
    return nullptr;
}
#endif

RendezvousResult closeRoom(const std::string& baseUrl, const std::string& roomId) {
    RendezvousResult result;
    // Short timeout: this runs on the shutdown path and must never be the
    // reason the daemon is slow to exit.
    const HttpResponse res =
        httpRequest(baseUrl, "/api/room/" + roomId, "DELETE", "", 2500);
    if (!res.transportOk) {
        result.error = res.error.empty() ? "could not reach the rendezvous" : res.error;
        return result;
    }
    result.ok = (res.status == 204 || res.status == 404);
    if (!result.ok)
        result.error = "rendezvous returned HTTP " + std::to_string(res.status);
    return result;
}

} // namespace soi
