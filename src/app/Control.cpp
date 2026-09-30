#include "app/Control.h"
#include "app/Service.h"
#include "util/Log.h"
#include "util/Win.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

namespace soi {
namespace {

constexpr char   kInstanceFile[]  = "instance.txt";
constexpr size_t kMaxRequestBytes = 64 * 1024;

std::atomic<SOCKET>                   g_listener{INVALID_SOCKET};
std::thread                           g_server;
std::string                           g_secret;
std::string                           g_version;
std::chrono::steady_clock::time_point g_started;
long long                             g_startedUnix = 0;

bool ensureWinsock() {
    static const bool ok = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

std::string randomHex(size_t bytes) {
    std::string raw(bytes, '\0');
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(raw.data()),
                        static_cast<ULONG>(raw.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        return {};
    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : raw) { out += kHex[c >> 4]; out += kHex[c & 15]; }
    return out;
}

// Constant-time: the secret is compared against whatever a local process sends.
bool sameSecret(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i)
        diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

void sendAll(SOCKET s, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

std::map<std::string, std::string> statusReply() {
    std::map<std::string, std::string> kv;
    for (const auto& [k, v] : liveSnapshot()) kv[k] = v;
    kv["pid"]     = std::to_string(GetCurrentProcessId());
    kv["version"] = g_version;
    kv["exe"]     = currentExePath();
    kv["started"] = std::to_string(g_startedUnix);
    kv["uptime"]  = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::steady_clock::now() - g_started).count());
    return kv;
}

void handleClient(SOCKET client) {
    const DWORD timeout = 2000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);

    std::string line;
    char buf[4096];
    while (line.find('\n') == std::string::npos && line.size() < kMaxRequestBytes) {
        const int n = recv(client, buf, sizeof buf, 0);
        if (n <= 0) break;
        line.append(buf, static_cast<size_t>(n));
    }
    if (const size_t nl = line.find('\n'); nl != std::string::npos) line.resize(nl);
    if (!line.empty() && line.back() == '\r') line.pop_back();

    // <secret> <command> [argument]
    const size_t sp1 = line.find(' ');
    const std::string secret  = line.substr(0, sp1);
    std::string       rest    = sp1 == std::string::npos ? "" : line.substr(sp1 + 1);
    const size_t sp2 = rest.find(' ');
    const std::string command = rest.substr(0, sp2);
    const std::string arg     = sp2 == std::string::npos ? "" : rest.substr(sp2 + 1);

    std::map<std::string, std::string> reply;
    if (!sameSecret(secret, g_secret)) {
        reply["error"] = "denied";
    } else if (command == "ping") {
        reply["ok"]  = "1";
        reply["pid"] = std::to_string(GetCurrentProcessId());
    } else if (command == "status") {
        reply = statusReply();
        reply["ok"] = "1";
    } else if (command == "stop") {
        logI("stop requested over the control channel");
        requestStop();
        reply["ok"] = "1";
    } else if (command == "answer") {
        if (arg.empty()) {
            reply["error"] = "empty answer";
        } else {
            pushAnswer(arg);
            reply["ok"] = "1";
        }
    } else {
        reply["error"] = "unknown command";
    }

    sendAll(client, serializeKeyValues(reply));
    shutdown(client, SD_SEND);
    closesocket(client);
}

void serveLoop() {
    for (;;) {
        const SOCKET listener = g_listener.load();
        if (listener == INVALID_SOCKET) return;
        const SOCKET client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            // closesocket() on the listener is how stopControlServer wakes us.
            if (g_listener.load() == INVALID_SOCKET) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }
        handleClient(client);
    }
}

bool sameImage(std::string a, std::string b) {
    // A running exe that `update` renamed aside keeps running as *.old.
    auto strip = [](std::string& s) {
        if (s.size() > 4 && _stricmp(s.c_str() + s.size() - 4, ".old") == 0) s.resize(s.size() - 4);
    };
    strip(a);
    strip(b);
    const std::wstring wa = toUtf16(a), wb = toUtf16(b);
    return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()),
                                wb.c_str(), static_cast<int>(wb.size()), TRUE) == CSTR_EQUAL;
}

} // namespace

// ---------------------------------------------------------------------------

std::string serializeKeyValues(const std::map<std::string, std::string>& kv) {
    std::string out;
    for (const auto& [k, v] : kv) {
        std::string value = v;
        for (char& c : value)
            if (c == '\n' || c == '\r') c = ' ';
        out += k + "=" + value + "\n";
    }
    return out;
}

std::map<std::string, std::string> parseKeyValues(const std::string& text) {
    std::map<std::string, std::string> kv;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Split on the FIRST '=': values (an offer blob, a URL) may contain more.
        if (const size_t eq = line.find('='); eq != std::string::npos && eq > 0)
            kv[line.substr(0, eq)] = line.substr(eq + 1);
        start = end + 1;
    }
    return kv;
}

bool parseInstanceRecord(const std::string& text, InstanceRecord& out) {
    const auto kv = parseKeyValues(text);
    auto get = [&](const char* k) {
        const auto it = kv.find(k);
        return it == kv.end() ? std::string() : it->second;
    };
    out = InstanceRecord{};
    out.pid         = std::strtoul(get("pid").c_str(), nullptr, 10);
    out.port        = std::atoi(get("port").c_str());
    out.secret      = get("secret");
    out.exe         = get("exe");
    out.version     = get("version");
    out.startedUnix = std::strtoll(get("started").c_str(), nullptr, 10);
    return out.pid != 0 && out.port > 0 && out.port < 65536 && !out.secret.empty();
}

bool startControlServer(const std::string& version, std::string& error) {
    if (!ensureWinsock()) { error = "Winsock would not start"; return false; }

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) { error = "could not create the control socket"; return false; }

    // Nobody else may bind the same port while we hold it.
    BOOL exclusive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
               reinterpret_cast<const char*>(&exclusive), sizeof exclusive);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // 127.0.0.1 only, never the LAN
    addr.sin_port        = 0;                        // ephemeral
    if (bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 || listen(s, 8) != 0) {
        error = "could not listen on 127.0.0.1";
        closesocket(s);
        return false;
    }
    int len = sizeof addr;
    getsockname(s, reinterpret_cast<sockaddr*>(&addr), &len);

    g_secret      = randomHex(16);
    g_version     = version;
    g_started     = std::chrono::steady_clock::now();
    g_startedUnix = static_cast<long long>(std::time(nullptr));
    if (g_secret.empty()) { error = "could not generate a control secret"; closesocket(s); return false; }

    g_listener.store(s);
    g_server = std::thread(serveLoop);

    // Written only once the socket is listening, so a record that exists always
    // points at a channel that answers -- `start` waits on exactly that.
    const std::string record = serializeKeyValues({
        {"pid",     std::to_string(GetCurrentProcessId())},
        {"port",    std::to_string(ntohs(addr.sin_port))},
        {"secret",  g_secret},
        {"exe",     currentExePath()},
        {"version", version},
        {"started", std::to_string(g_startedUnix)},
    });
    if (!writeStateFile(kInstanceFile, record)) {
        error = "could not write " + stateFilePath(kInstanceFile);
        stopControlServer();
        return false;
    }
    logI("control channel on 127.0.0.1:{}", ntohs(addr.sin_port));
    return true;
}

void stopControlServer() {
    const SOCKET s = g_listener.exchange(INVALID_SOCKET);
    if (s != INVALID_SOCKET) closesocket(s);
    if (g_server.joinable()) g_server.join();

    // Only delete the record if it is still ours: a second instance may have
    // started after this one began shutting down.
    std::string text;
    InstanceRecord rec;
    if (readStateFile(kInstanceFile, text) && parseInstanceRecord(text, rec) &&
        rec.pid == GetCurrentProcessId())
        removeStateFile(kInstanceFile);
}

bool processAlive(unsigned long pid, const std::string& exe) {
    if (pid == 0) return false;
    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return false;

    DWORD code = 0;
    bool alive = GetExitCodeProcess(proc, &code) && code == STILL_ACTIVE;

    // A recycled pid belonging to something else must never count as ours, or
    // `stop --force` would kill an unrelated program.
    if (alive && !exe.empty()) {
        std::wstring buf(32768, L'\0');
        DWORD len = static_cast<DWORD>(buf.size());
        if (QueryFullProcessImageNameW(proc, 0, buf.data(), &len)) {
            buf.resize(len);
            if (!sameImage(toUtf8(buf), exe)) alive = false;
        }
    }
    CloseHandle(proc);
    return alive;
}

FoundInstance findInstance() {
    FoundInstance found;

    std::string text;
    if (readStateFile(kInstanceFile, text) && !text.empty()) {
        InstanceRecord rec;
        if (parseInstanceRecord(text, rec) && processAlive(rec.pid, rec.exe)) {
            found.record = rec;
            std::map<std::string, std::string> reply;
            found.state = controlRequest(rec, "ping", reply, 2000) && reply.count("ok")
                              ? InstanceState::Running
                              : InstanceState::Unresponsive;
            return found;
        }
        // The process that wrote this is gone: it crashed, was killed, or the
        // PC restarted. Its record would otherwise make every later command
        // believe it is still there.
        removeStateFile(kInstanceFile);
        found.cleanedStale = true;
    }

    if (unsigned long pid = 0; legacyDaemonRunning(&pid)) {
        found.state      = InstanceState::Legacy;
        found.record.pid = pid;
    }
    return found;
}

bool controlRequest(const InstanceRecord& rec, const std::string& command,
                    std::map<std::string, std::string>& reply, int timeoutMs) {
    reply.clear();
    if (!ensureWinsock() || rec.port <= 0) return false;

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof timeout);

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = htons(static_cast<u_short>(rec.port));
    if (connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        closesocket(s);
        return false;
    }

    sendAll(s, rec.secret + " " + command + "\n");
    shutdown(s, SD_SEND);

    std::string response;
    char buf[4096];
    for (;;) {
        const int n = recv(s, buf, sizeof buf, 0);
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    closesocket(s);

    reply = parseKeyValues(response);
    return !reply.empty();
}

} // namespace soi
