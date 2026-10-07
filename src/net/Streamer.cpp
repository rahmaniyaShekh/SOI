#include "net/Streamer.h"
#include "encode/H264Encoder.h"   // h264LevelForResolution
#include "util/Log.h"

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

constexpr uint32_t kClockRate  = 90000;    // H.264 RTP clock
constexpr rtc::SSRC kVideoSsrc = 0x5301A1;
// uint8_t, not int: that is the type libdatachannel's packetizer config takes,
// and an int here made every build emit three C4244 narrowing warnings from
// deep inside <xutility> that had nothing to do with the code being worked on.
constexpr uint8_t  kPayloadType = 96;
// Keep RTP payloads under a typical 1500-byte path MTU once SRTP, UDP, IP and a
// possible tunnel header are accounted for.
constexpr uint16_t kMaxFragmentSize = 1200;

uint32_t read32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}

// Middle 32 bits of the current NTP timestamp (16.16 fixed point seconds), which
// is the format RTCP uses for the LSR/DLSR round-trip calculation.
uint32_t ntpMiddle32() {
    using namespace std::chrono;
    const auto now = system_clock::now().time_since_epoch();
    const auto secs = duration_cast<seconds>(now);
    const auto frac = duration_cast<microseconds>(now - secs);

    constexpr uint64_t kEpochOffset = 2208988800ULL;   // 1900 -> 1970
    const uint64_t ntpSec  = static_cast<uint64_t>(secs.count()) + kEpochOffset;
    const uint32_t ntpFrac = static_cast<uint32_t>((frac.count() * 65536ULL) / 1000000ULL);

    return (static_cast<uint32_t>(ntpSec & 0xFFFF) << 16) | (ntpFrac & 0xFFFF);
}

// Minimal extractor for the tiny control protocol. A full JSON parser is not
// worth a dependency for two integer fields; anything unrecognised is ignored.
bool extractInt(const std::string& text, std::string_view key, long long& out) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;

    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;

    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '"')) ++pos;

    bool negative = false;
    if (pos < text.size() && text[pos] == '-') { negative = true; ++pos; }

    if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;

    long long value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        value = value * 10 + (text[pos] - '0');
        ++pos;
        if (value > 1'000'000'000LL) break;
    }
    out = negative ? -value : value;
    return true;
}

bool containsKey(const std::string& text, std::string_view key) {
    return text.find(std::string("\"") + std::string(key) + "\"") != std::string::npos;
}

// Pulls a short string field out of the tiny control protocol. Bounded on
// purpose: this parses data straight off the network, and the only legitimate
// values are level names a dozen characters long.
bool extractString(const std::string& text, std::string_view key, std::string& out) {
    const std::string needle = std::string("\"") + std::string(key) + "\"";
    size_t pos = text.find(needle);
    if (pos == std::string::npos) return false;

    pos = text.find(':', pos + needle.size());
    if (pos == std::string::npos) return false;
    ++pos;
    while (pos < text.size() && text[pos] == ' ') ++pos;
    if (pos >= text.size() || text[pos] != '"') return false;
    ++pos;

    out.clear();
    while (pos < text.size() && text[pos] != '"') {
        if (out.size() >= 32) return false;      // no legitimate value is longer
        out.push_back(text[pos++]);
    }
    return pos < text.size();                    // must have found the closing quote
}

// Builds a TURN IceServer from a "turn:host:port?transport=tcp" style URL.
// Parsing the host/port ourselves and using the explicit constructor avoids the
// URL-escaping problems that credentials containing ':' '@' or '/' otherwise hit.
rtc::IceServer makeTurnServer(std::string url, const std::string& user,
                              const std::string& pass) {
    auto relayType = rtc::IceServer::RelayType::TurnUdp;

    if (url.rfind("turns:", 0) == 0) {
        relayType = rtc::IceServer::RelayType::TurnTls;
        url.erase(0, 6);
    } else if (url.rfind("turn:", 0) == 0) {
        url.erase(0, 5);
    }

    if (const size_t q = url.find('?'); q != std::string::npos) {
        const std::string query = url.substr(q + 1);
        url.resize(q);
        if (query.find("transport=tcp") != std::string::npos &&
            relayType != rtc::IceServer::RelayType::TurnTls)
            relayType = rtc::IceServer::RelayType::TurnTcp;
    }

    std::string host = url;
    std::string service = "3478";
    if (const size_t colon = url.rfind(':'); colon != std::string::npos) {
        host    = url.substr(0, colon);
        service = url.substr(colon + 1);
    } else if (relayType == rtc::IceServer::RelayType::TurnTls) {
        service = "5349";
    }

    return rtc::IceServer(host, service, user, pass, relayType);
}

} // namespace

Streamer::~Streamer() { stop(); }

bool Streamer::start(const StreamerConfig& cfg) {
    if (running_.load()) return true;
    cfg_ = cfg;
    targetKbps_.store(cfg.maxBitrateKbps);

    try {
        rtc::Configuration config;

        for (const auto& url : cfg.stunUrls)
            if (!url.empty()) config.iceServers.emplace_back(url);

        if (config.iceServers.empty())
            logI("STUN disabled: only host candidates will be gathered "
                 "(same-LAN peers only)");
        else
            logI("STUN: {} server(s)", config.iceServers.size());

        if (!cfg.turnUrl.empty()) {
            config.iceServers.push_back(
                makeTurnServer(cfg.turnUrl, cfg.turnUser, cfg.turnPass));
            logI("TURN relay configured ({}). It is a fallback only -- direct "
                 "paths still win when one exists, and the relay cannot decrypt "
                 "the stream.", cfg.turnUrl);
        }
        // No trickle: with no signalling channel, candidates discovered after the
        // offer is printed could never reach the peer.
        config.disableAutoNegotiation = false;

        pc_ = std::make_shared<rtc::PeerConnection>(config);

        pc_->onLocalCandidate([this](rtc::Candidate c) {
            if (c.type() != rtc::Candidate::Type::ServerReflexive &&
                c.type() != rtc::Candidate::Type::Relayed)
                return;
            {
                std::lock_guard lk(mtx_);
                if (firstPublicCandidate_ == std::chrono::steady_clock::time_point{})
                    firstPublicCandidate_ = std::chrono::steady_clock::now();
            }
            cv_.notify_all();
        });

        pc_->onGatheringStateChange([this](rtc::PeerConnection::GatheringState state) {
            logT("ICE gathering state: {}",
                 state == rtc::PeerConnection::GatheringState::Complete ? "complete"
                 : state == rtc::PeerConnection::GatheringState::InProgress ? "in-progress"
                                                                            : "new");
            if (state == rtc::PeerConnection::GatheringState::Complete) {
                { std::lock_guard lk(mtx_); gatheringComplete_ = true; }
                cv_.notify_all();
            }
        });

        pc_->onStateChange([this](rtc::PeerConnection::State state) {
            std::string name;
            switch (state) {
                case rtc::PeerConnection::State::New:          name = "new"; break;
                case rtc::PeerConnection::State::Connecting:   name = "connecting"; break;
                case rtc::PeerConnection::State::Connected:    name = "connected"; break;
                case rtc::PeerConnection::State::Disconnected: name = "disconnected"; break;
                case rtc::PeerConnection::State::Failed:       name = "failed"; break;
                case rtc::PeerConnection::State::Closed:       name = "closed"; break;
            }
            logI("peer connection: {}", name);

            // Take the lock before notifying: waitForConnected() evaluates its
            // predicate while holding it, so a store+notify that lands in that
            // window would otherwise be missed and the waiter would hang until
            // its timeout.
            {
                std::lock_guard lk(mtx_);
                connected_.store(state == rtc::PeerConnection::State::Connected);
                if (state == rtc::PeerConnection::State::Failed) {
                    failed_.store(true);
                    logE("ICE failed. Without a TURN relay this is expected when "
                         "both peers sit behind symmetric NAT -- see --turn.");
                }
            }
            cv_.notify_all();

            if (onState_) onState_(name);
        });

        // ---- video track -------------------------------------------------
        // The advertised profile-level-id must match what the encoder actually
        // produces (Main profile, level computed from resolution and frame rate).
        const int level = h264LevelForResolution(cfg.width, cfg.height, cfg.fps);
        const std::string fmtp =
            soi::format("profile-level-id=4d00{:02x};packetization-mode=1;"
                        "level-asymmetry-allowed=1",
                        level);

        rtc::Description::Video media("video", rtc::Description::Direction::SendOnly);
        media.addH264Codec(kPayloadType, fmtp);
        media.addSSRC(kVideoSsrc, "soi-video", "soi-stream", "soi-video");
        track_ = pc_->addTrack(media);

        rtpConfig_ = std::make_shared<rtc::RtpPacketizationConfig>(
            kVideoSsrc, "soi", kPayloadType, kClockRate);

        // StartSequence, NOT LongStartSequence. The Media Foundation H.264
        // encoder emits a MIX of 3-byte (00 00 01) and 4-byte (00 00 00 01)
        // Annex-B start codes -- measured at 61 three-byte vs 182 four-byte in a
        // 60-frame run. Configuring the packetizer for 4-byte only splits every
        // subsequent NAL at the wrong offset: RTP flows, the receiver reports
        // bytes and even sends REMB, and not a single frame ever decodes.
        auto packetizer = std::make_shared<rtc::H264RtpPacketizer>(
            rtc::NalUnit::Separator::StartSequence, rtpConfig_, kMaxFragmentSize);

        // Sender reports let the viewer compute RTT and keep its jitter buffer sane.
        srReporter_ = std::make_shared<rtc::RtcpSrReporter>(rtpConfig_);
        packetizer->addToChain(srReporter_);

        // Retransmit on NACK: recovering a single lost packet is far cheaper, and
        // subjectively far better, than waiting for the next IDR.
        packetizer->addToChain(std::make_shared<rtc::RtcpNackResponder>(512));

        track_->setMediaHandler(packetizer);

        track_->onMessage(
            [this](rtc::binary data) { handleRtcp(data.data(), data.size()); },
            [](rtc::string) {});

        track_->onOpen([] { logI("video track open"); });

        // ---- control channel ---------------------------------------------
        control_ = pc_->createDataChannel("soi-control");
        control_->onOpen([this] {
            logI("control channel open");
            // The sender owns the list of quality levels, so it announces them
            // and lets the viewer choose. Doing it this way means the viewer
            // never has to be redeployed to match a change here.
            if (onControlReady_) onControlReady_();
        });
        control_->onMessage(
            [](rtc::binary) {},
            [this](rtc::string text) { handleControlMessage(text); });

        pc_->setLocalDescription();
        running_.store(true);
        return true;

    } catch (const std::exception& e) {
        logE("failed to start WebRTC transport: {}", e.what());
        pc_.reset();
        return false;
    }
}

void Streamer::stop() {
    if (!running_.exchange(false)) return;
    relay_.reset();   // closes the relay socket and joins its threads
    try {
        if (control_) control_->close();
        if (track_)   track_->close();
        if (pc_)      pc_->close();
    } catch (const std::exception& e) {
        logT("error during shutdown: {}", e.what());
    }
    control_.reset();
    track_.reset();
    srReporter_.reset();
    rtpConfig_.reset();
    pc_.reset();
    connected_.store(false);
}

bool Streamer::waitForGathering(std::chrono::milliseconds timeout,
                                const std::function<bool()>& cancel) {
    using namespace std::chrono_literals;
    const auto t0 = std::chrono::steady_clock::now();
    std::unique_lock lk(mtx_);
    while (!gatheringComplete_) {
        if (cancel && cancel()) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now - t0 >= timeout) break;
        if (firstPublicCandidate_ != std::chrono::steady_clock::time_point{} &&
            now - firstPublicCandidate_ >= 1s)
            break;
        cv_.wait_for(lk, 100ms);
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
    if (gatheringComplete_) logT("ICE gathering complete in {} ms", ms);
    else logT("ICE gathering {} after {} ms; offering what we have",
              firstPublicCandidate_ != std::chrono::steady_clock::time_point{}
                  ? "still running" : "found no public address", ms);
    return gatheringComplete_;
}

std::string Streamer::localDescriptionSdp() const {
    if (!pc_) return {};
    auto desc = pc_->localDescription();
    return desc ? std::string(*desc) : std::string{};
}

bool Streamer::acceptAnswer(const std::string& sdp, std::string& error) {
    if (!pc_) { error = "transport is not running"; return false; }
    try {
        pc_->setRemoteDescription(rtc::Description(sdp, rtc::Description::Type::Answer));
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}

bool Streamer::waitForConnected(std::chrono::milliseconds timeout,
                                const std::function<bool()>& cancel) {
    using namespace std::chrono_literals;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lk(mtx_);
    while (!connected_.load() && !failed_.load()) {
        if (cancel && cancel()) return false;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        cv_.wait_for(lk, 100ms);
    }
    return connected_.load();
}

void Streamer::startRelay(std::unique_ptr<RelaySocket> socket, RelayKey key) {
    // The peer connection was built for an answer that is never coming. Its
    // callbacks go first, so closing it cannot report a state over the relay's.
    try {
        if (control_) control_->resetCallbacks();
        if (track_)   track_->resetCallbacks();
        if (pc_) {
            pc_->resetCallbacks();
            pc_->close();
        }
    } catch (const std::exception& e) {
        logT("closing the unused peer connection: {}", e.what());
    }
    {
        std::lock_guard lk(mtx_);
        connected_.store(false);
        failed_.store(false);
    }
    relayed_.store(true);

    RelayLink::Callbacks cb;
    cb.onLive = [this] {
        {
            std::lock_guard lk(mtx_);
            connected_.store(true);
        }
        cv_.notify_all();
        if (onState_) onState_("connected");
        // The viewer's decoder cannot start without a keyframe, and the page
        // shows its controls once it knows what is on offer.
        if (onKeyframe_) onKeyframe_();
        if (onControlReady_) onControlReady_();
    };
    cb.onClosed = [this] {
        {
            std::lock_guard lk(mtx_);
            connected_.store(false);
            failed_.store(true);
        }
        cv_.notify_all();
        if (onState_) onState_("closed");
    };
    cb.onKeyframe = [this] {
        pliCount_.fetch_add(1);
        if (onKeyframe_) onKeyframe_();
    };
    cb.onBitrate = [this](int kbps) {
        targetKbps_.store(kbps);
        if (onBitrate_) onBitrate_(kbps);
    };
    cb.onControl = [this](const std::string& text) { handleControlMessage(text); };

    // Start well inside what a TCP path through the rendezvous carries, and let
    // the relay's own control find the rest.
    const int start = std::clamp(1500, cfg_.minBitrateKbps, cfg_.maxBitrateKbps);
    targetKbps_.store(start);
    relay_ = std::make_unique<RelayLink>(std::move(socket), std::move(key), std::move(cb), start,
                                         cfg_.minBitrateKbps,
                                         std::min(cfg_.maxBitrateKbps, 6000));
    if (onBitrate_) onBitrate_(start);
}

void Streamer::sendFrame(const uint8_t* annexB, size_t len, bool keyframe,
                         int64_t ptsNs) {
    if (relayed_.load()) {
        if (relay_) relay_->sendFrame(annexB, len, keyframe, ptsNs);
        return;
    }
    if (!track_ || len == 0) return;
    if (!track_->isOpen()) return;

    if (firstPtsNs_ < 0) firstPtsNs_ = ptsNs;
    const int64_t relNs = ptsNs - firstPtsNs_;

    // 90 kHz RTP timestamp. Computed in 64-bit then truncated, so it wraps
    // correctly at 2^32 exactly as RTP requires.
    const uint32_t offset =
        static_cast<uint32_t>((relNs * static_cast<int64_t>(kClockRate)) / 1'000'000'000LL);
    rtpConfig_->timestamp = rtpConfig_->startTimestamp + offset;

    if (srReporter_) {
        const uint32_t sinceReport =
            rtpConfig_->timestamp - srReporter_->lastReportedTimestamp();
        if (sinceReport > kClockRate / 2) srReporter_->setNeedsToReport();
    }

    try {
        track_->send(reinterpret_cast<const std::byte*>(annexB), len);
        framesSent_.fetch_add(1);
        bytesSent_.fetch_add(len);
        if (keyframe) keyframes_.fetch_add(1);
    } catch (const std::exception& e) {
        logT("track send failed: {}", e.what());
    }
}

bool Streamer::controlIsOpen() const {
    if (relayed_.load()) return relay_ && relay_->live();
    return control_ && control_->isOpen();
}

void Streamer::sendControl(const std::string& text) {
    if (relayed_.load()) {
        if (relay_) relay_->sendControl(text);
        return;
    }
    if (!control_ || !control_->isOpen()) return;
    try {
        control_->send(text);
    } catch (const std::exception& e) {
        logT("control send failed: {}", e.what());
    }
}

// ---------------------------------------------------------------------------
// RTCP
// ---------------------------------------------------------------------------

void Streamer::handleRtcp(const std::byte* data, size_t size) {
    const auto* buf = reinterpret_cast<const uint8_t*>(data);
    size_t off = 0;

    // RTCP arrives as compound packets: walk every sub-packet.
    while (off + 4 <= size) {
        const uint8_t* p = buf + off;

        if ((p[0] >> 6) != 2) break;                    // version must be 2
        const uint8_t fmt = p[0] & 0x1F;                // RC or FMT
        const uint8_t pt  = p[1];
        const size_t  pktLen = ((static_cast<size_t>(p[2]) << 8) | p[3]) * 4 + 4;

        if (pktLen < 4 || off + pktLen > size) break;

        switch (pt) {
            case 206: {   // PSFB - payload-specific feedback
                if (fmt == 1 || fmt == 4) {             // PLI or FIR
                    pliCount_.fetch_add(1);
                    // Rate-limit: a viewer that has lost sync can emit a burst of
                    // PLIs, and answering each one with an IDR makes it worse.
                    const auto now = std::chrono::steady_clock::now();
                    if (now - lastKeyframeRequest_ > std::chrono::milliseconds(500)) {
                        lastKeyframeRequest_ = now;
                        logT("{} received -> forcing IDR", fmt == 1 ? "PLI" : "FIR");
                        if (onKeyframe_) onKeyframe_();
                    }
                } else if (fmt == 15 && pktLen >= 20 &&
                           std::memcmp(p + 12, "REMB", 4) == 0) {
                    const uint8_t  exp      = p[17] >> 2;
                    const uint32_t mantissa = (static_cast<uint32_t>(p[17] & 0x03) << 16) |
                                              (static_cast<uint32_t>(p[18]) << 8) | p[19];
                    handleRemb(static_cast<uint64_t>(mantissa) << exp);
                }
                break;
            }
            case 205:     // RTPFB - transport feedback (generic NACK)
                if (fmt == 1) nackCount_.fetch_add(1);
                break;

            case 200:     // SR - a sender report from the viewer; report blocks follow
            case 201: {   // RR
                const size_t headerLen = (pt == 200) ? 28 : 8;
                for (int i = 0; i < fmt; ++i) {
                    const size_t blockOff = headerLen + static_cast<size_t>(i) * 24;
                    if (blockOff + 24 > pktLen) break;
                    handleReceiverReport(p + blockOff);
                }
                break;
            }
            default:
                break;
        }
        off += pktLen;
    }
}

void Streamer::handleReceiverReport(const uint8_t* block) {
    const double loss = block[4] / 256.0;               // fraction lost, 8.8 fixed point
    const uint32_t lsr  = read32(block + 16);
    const uint32_t dlsr = read32(block + 20);

    lossFraction_.store(loss);

    if (lsr != 0) {
        // RTT = now - LSR - DLSR, all in 1/65536 s. Unsigned wrap is intentional.
        const uint32_t delta = ntpMiddle32() - lsr - dlsr;
        const double   rtt   = (delta / 65536.0) * 1000.0;
        if (rtt >= 0.0 && rtt < 10000.0) rttMs_.store(rtt);
    }

    runAimd(loss);
}

void Streamer::handleRemb(uint64_t bitsPerSecond) {
    rembSeen_.store(true);
    // The receiver's estimate beats ours, but never exceed what the user asked for.
    const int kbps = static_cast<int>(
        std::min<uint64_t>(bitsPerSecond / 1000, static_cast<uint64_t>(cfg_.maxBitrateKbps)));
    applyTarget(std::max(kbps, cfg_.minBitrateKbps));
}

void Streamer::runAimd(double lossFraction) {
    // REMB, when the viewer sends it, is a better signal than our loss heuristic.
    if (rembSeen_.load()) return;

    const auto now = std::chrono::steady_clock::now();
    if (lastAimd_.time_since_epoch().count() != 0 &&
        now - lastAimd_ < std::chrono::seconds(1))
        return;
    lastAimd_ = now;

    int target = targetKbps_.load();

    if (lossFraction > 0.10) {
        target = static_cast<int>(target * 0.85);       // multiplicative decrease
    } else if (lossFraction < 0.02) {
        target = static_cast<int>(target * 1.05);       // gentle increase
    } else {
        return;                                          // hold in the dead band
    }

    applyTarget(std::clamp(target, cfg_.minBitrateKbps, cfg_.maxBitrateKbps));
}

void Streamer::applyTarget(int kbps) {
    kbps = std::clamp(kbps, cfg_.minBitrateKbps, cfg_.maxBitrateKbps);

    const int previous = targetKbps_.exchange(kbps);
    // Ignore sub-10% churn: reconfiguring the encoder is not free and a
    // constantly-moving target hurts quality more than it helps.
    if (std::abs(kbps - previous) * 10 < previous) return;

    logT("bitrate target {} -> {} kbps (loss {:.1f}%, rtt {:.0f}ms)",
         previous, kbps, lossFraction_.load() * 100.0, rttMs_.load());
    if (onBitrate_) onBitrate_(kbps);
}

void Streamer::handleControlMessage(const std::string& text) {
    logT("control <- {}", text);

    // The viewer choosing a quality level. The name is not trusted: the handler
    // looks it up in the level table and ignores anything that is not there.
    if (containsKey(text, "setQuality")) {
        std::string level;
        if (extractString(text, "level", level) && onQuality_) onQuality_(level);
        return;
    }

    // The viewer choosing what to watch: a monitor by index, or every screen at
    // once. "setMonitor" is the older monitor-only spelling and still works, so
    // a viewer.html that predates this does not break.
    //
    // Nothing is decided here. The bound below only keeps a nonsense index out
    // of the enumeration code; the real checks -- does this monitor exist, and
    // is this viewer allowed to change the target at all -- are made on the
    // capture thread against the live state of the machine.
    if (containsKey(text, "setTarget") || containsKey(text, "setMonitor")) {
        std::string kind;
        if (!extractString(text, "kind", kind)) kind = "monitor";

        long long index = -1;
        if (!extractInt(text, "index", index)) index = -1;
        if (index < -1 || index >= 64) return;

        if (onTarget_) onTarget_(kind, static_cast<int>(index));
        return;
    }

    if (containsKey(text, "keyframe")) {
        if (onKeyframe_) onKeyframe_();
        return;
    }

    long long value = 0;
    if (extractInt(text, "bitrate", value) && value > 0) {
        applyTarget(static_cast<int>(value));
    }
}

StreamStats Streamer::stats() const {
    StreamStats s;
    if (relayed_.load() && relay_) {
        const auto r = relay_->stats();
        s.framesSent        = r.framesSent;
        s.bytesSent         = r.bytesSent;
        s.framesDropped     = r.framesDropped;
        s.targetBitrateKbps = r.kbps;
        s.relaySendMs       = r.sendMs;
        s.viewerBacklog     = r.viewerQueue;
        s.pliCount          = pliCount_.load();
        s.relayed           = true;
        return s;
    }
    s.framesSent        = framesSent_.load();
    s.bytesSent         = bytesSent_.load();
    s.keyframesSent     = keyframes_.load();
    s.pliCount          = pliCount_.load();
    s.nackCount         = nackCount_.load();
    s.lossFraction      = lossFraction_.load();
    s.rttMs             = rttMs_.load();
    s.targetBitrateKbps = targetKbps_.load();
    s.rembSeen          = rembSeen_.load();
    return s;
}

} // namespace soi
