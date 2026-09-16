#pragma once
//
// Local HTTP handover.
//
// The signalling blob has to reach the viewer somehow. Sending a file plus
// pasting a code back works everywhere but is clumsy, and phones in particular
// cannot easily open a local .html file.
//
// So when the peer can reach this machine (same Wi-Fi, or a forwarded port),
// this serves the viewer straight off the sharing PC:
//
//   GET  /         -> viewer.html with the offer already injected
//   POST /answer   -> the viewer posts its answer back automatically
//   GET  /health   -> liveness probe
//
// The connection then needs ZERO manual steps: the friend opens one URL and the
// screen appears.
//
// Scope, stated honestly: this IS a server -- a ~200-line one, running on the
// sharing machine, serving exactly one HTML file and accepting exactly one
// answer. No third party is involved and no data leaves the LAN during
// signalling. Media remains direct peer-to-peer either way. If you want strictly
// zero listening sockets, use --no-http and the send-a-file flow instead.
//
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace soi {

// IPv4 addresses of up-and-running, non-loopback adapters, best candidate first.
std::vector<std::string> localAddresses();

class LocalHandover {
public:
    // Invoked with the answer blob the viewer posted. Return true if accepted.
    using AnswerHandler = std::function<bool(const std::string&)>;

    LocalHandover() = default;
    ~LocalHandover();

    LocalHandover(const LocalHandover&) = delete;
    LocalHandover& operator=(const LocalHandover&) = delete;

    // `viewerHtml` is the full page source; the offer is injected into it.
    // Port 0 asks the OS for a free port. Returns false if the socket could not
    // be bound (most often: another instance, or a firewall policy).
    bool start(int port, const std::string& viewerHtml, const std::string& offerBlob,
               AnswerHandler onAnswer);
    void stop();

    int  port() const { return port_; }
    bool answered() const { return answered_.load(); }

    // Ready-to-share URLs, one per local address.
    std::vector<std::string> urls() const;

private:
    void serve();
    void handleClient(uintptr_t clientSocket);

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> answered_{false};
    uintptr_t         listenSocket_ = ~uintptr_t(0);
    int               port_ = 0;
    std::string       page_;          // viewer with the offer already inside
    AnswerHandler     onAnswer_;
};

} // namespace soi
