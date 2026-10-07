#include "net/RelayLink.h"
#include "util/Json.h"
#include "util/Log.h"

#include <algorithm>

namespace soi {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// About half a second of video at the relay frame rate. More than this queued
// means the path is not keeping up, and waiting longer only adds delay.
constexpr size_t kMaxQueuedFrames = 7;
// Cloudflare accepts WebSocket messages up to 1 MiB; leave room for the seal.
constexpr size_t kMaxFrameBytes = 1000 * 1000;
// The viewer reports its decode backlog every second; this long without a word
// means it is gone (a closed laptop does not always close its socket).
constexpr auto kViewerSilence = 20s;
// Text the rendezvous answers by itself, so it keeps proxies and NATs from
// timing the socket out without waking the Durable Object.
constexpr auto kKeepalive = 20s;

int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               Clock::now().time_since_epoch()).count();
}

} // namespace

RelayLink::RelayLink(std::unique_ptr<RelaySocket> socket, RelayKey key, Callbacks callbacks,
                     int startKbps, int minKbps, int maxKbps)
    : socket_(std::move(socket)), key_(std::move(key)), cb_(std::move(callbacks)),
      minKbps_(minKbps), maxKbps_(std::max(minKbps, maxKbps)) {
    kbps_.store(std::clamp(startKbps, minKbps_, maxKbps_));
    cleanSince_ = lastAdapt_ = Clock::now();
    lastHeardMs_.store(nowMs());
    receiver_ = std::thread([this] { receiveLoop(); });
    sender_   = std::thread([this] { sendLoop(); });
}

RelayLink::~RelayLink() {
    close();
    if (receiver_.joinable()) receiver_.join();
    if (sender_.joinable())   sender_.join();
}

void RelayLink::close() {
    {
        std::lock_guard lk(mtx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (socket_) socket_->close();   // unblocks a pending receive or send
}

RelayLink::Stats RelayLink::stats() const {
    Stats s;
    s.framesSent    = framesSent_.load();
    s.bytesSent     = bytesSent_.load();
    s.framesDropped = framesDropped_.load();
    s.kbps          = kbps_.load();
    s.sendMs        = sendMs_.load();
    s.viewerQueue   = viewerQueue_.load();
    return s;
}

void RelayLink::requestKeyframe() {
    {
        std::lock_guard lk(mtx_);
        const auto now = Clock::now();
        // A burst of requests is one request: each keyframe is the most
        // expensive frame there is, and the path is already struggling.
        if (now - lastKeyRequest_ < 500ms) return;
        lastKeyRequest_ = now;
    }
    if (cb_.onKeyframe) cb_.onKeyframe();
}

void RelayLink::congested(const char* why) {
    if (!congested_.exchange(true)) logT("relay: {}", why);
}

void RelayLink::sendFrame(const uint8_t* annexB, size_t len, bool keyframe, int64_t ptsNs) {
    if (!live_.load() || closed_.load() || len == 0) return;

    if (len > kMaxFrameBytes) {
        // Too big for one relay message: skip it, and ask for a keyframe at the
        // lower bitrate this forces.
        framesDropped_.fetch_add(1);
        {
            std::lock_guard lk(mtx_);
            waitingForKey_ = true;
        }
        congested("a frame was too large for the relay");
        requestKeyframe();
        return;
    }

    bool wantKey = false;
    {
        std::lock_guard lk(mtx_);
        if (stopping_) return;

        if (waitingForKey_ && !keyframe) {
            framesDropped_.fetch_add(1);
            wantKey = true;
        } else {
            size_t queuedVideo = 0;
            for (const auto& o : queue_) queuedVideo += o.video;
            if (queuedVideo >= kMaxQueuedFrames) {
                // The path is not keeping up. Everything queued is already
                // late: drop the deltas and resume from a keyframe.
                const size_t before = queue_.size();
                queue_.erase(std::remove_if(queue_.begin(), queue_.end(),
                                            [](const Outgoing& o) { return o.video && !o.keyframe; }),
                             queue_.end());
                framesDropped_.fetch_add(before - queue_.size());
                congested("send queue full; dropping to the next keyframe");
                if (!keyframe) {
                    framesDropped_.fetch_add(1);
                    waitingForKey_ = true;
                    wantKey = true;
                }
            }
            if (!wantKey) {
                if (keyframe) waitingForKey_ = false;
                if (firstPtsNs_ < 0) firstPtsNs_ = ptsNs;   // the viewer's clock starts here
                Outgoing o;
                o.type = kRelayVideo;
                o.video = true;
                o.keyframe = keyframe;
                const auto ms = static_cast<uint32_t>((ptsNs - firstPtsNs_) / 1'000'000);
                relayAppendFrame(o.payload, keyframe, ms, annexB, len);
                queue_.push_back(std::move(o));
            }
        }
    }
    if (wantKey) requestKeyframe();
    else         cv_.notify_one();
}

void RelayLink::sendControl(const std::string& json) {
    if (closed_.load()) return;
    {
        std::lock_guard lk(mtx_);
        if (stopping_) return;
        Outgoing o;
        o.type = kRelayControl;
        o.payload.assign(json.begin(), json.end());
        queue_.push_back(std::move(o));
    }
    cv_.notify_one();
}

void RelayLink::receiveLoop() {
    std::string msg;
    bool binary = false;
    std::vector<uint8_t> payload;
    while (socket_->receive(msg, binary)) {
        if (!binary) continue;   // the rendezvous answering our keepalive
        uint8_t type = 0;
        if (!relayOpen(key_, reinterpret_cast<const uint8_t*>(msg.data()), msg.size(), type,
                       payload))
            continue;            // not sealed with this session's key: ignore it
        lastHeardMs_.store(nowMs());

        if (!live_.exchange(true)) {
            logI("relay: the viewer is connected through the rendezvous");
            if (cb_.onLive) cb_.onLive();
        }
        if (type != kRelayControl) continue;

        const std::string text(payload.begin(), payload.end());
        JsonValue v;
        if (JsonValue::parse(text, v) && v.isObject()) {
            const std::string& t = v["type"].str();
            if (t == "hello") continue;          // only ever meant "I am here"
            if (t == "rx") {                     // the viewer's decode backlog
                viewerQueue_.store(static_cast<int>(v["q"].num()));
                viewerDropped_.store(static_cast<int>(v["drop"].num()));
                continue;
            }
            if (t == "keyframe") {
                requestKeyframe();
                continue;
            }
        }
        if (cb_.onControl) cb_.onControl(text);
    }

    const bool wasOpen = !closed_.exchange(true);
    {
        std::lock_guard lk(mtx_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (wasOpen) logI("relay: closed");
    if (cb_.onClosed) cb_.onClosed();
}

void RelayLink::sendLoop() {
    auto lastKeepalive = Clock::now();
    for (;;) {
        Outgoing o;
        bool have = false;
        {
            std::unique_lock lk(mtx_);
            cv_.wait_for(lk, 200ms, [this] { return stopping_ || !queue_.empty(); });
            if (stopping_) break;
            if (!queue_.empty()) {
                o = std::move(queue_.front());
                queue_.pop_front();
                have = true;
            }
        }

        const auto now = Clock::now();
        if (have) {
            const auto sealed = relaySeal(key_, o.type, o.payload.data(), o.payload.size());
            if (sealed.empty()) continue;
            const auto t0 = Clock::now();
            if (!socket_->sendBinary(sealed.data(), sealed.size())) break;
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            // A blocking send is the TCP path telling us it is full.
            sendMs_.store(sendMs_.load() * 0.8 + ms * 0.2);
            if (o.video) {
                framesSent_.fetch_add(1);
                bytesSent_.fetch_add(sealed.size());
            }
            lastKeepalive = now;
        } else if (now - lastKeepalive >= kKeepalive) {
            if (!socket_->sendText("ping")) break;
            lastKeepalive = now;
        }

        if (live_.load() && nowMs() - lastHeardMs_.load() >
                std::chrono::duration_cast<std::chrono::milliseconds>(kViewerSilence).count()) {
            logI("relay: nothing from the viewer for {} s; closing",
                 std::chrono::duration_cast<std::chrono::seconds>(kViewerSilence).count());
            break;
        }
        if (live_.load()) adapt(now);
    }
    socket_->close();   // the receive loop sees this and reports the close
}

// Called on the sender thread only.
void RelayLink::adapt(Clock::time_point now) {
    if (now - lastAdapt_ < 2s) return;
    lastAdapt_ = now;

    const int dropped = viewerDropped_.load();
    const bool viewerDropping = dropped > seenViewerDropped_;
    seenViewerDropped_ = dropped;

    const bool struggling = congested_.exchange(false) || viewerDropping ||
                            viewerQueue_.load() > 3 || sendMs_.load() > 30.0;
    const int k = kbps_.load();
    int next = k;
    if (struggling) {
        next = std::max(minKbps_, k * 3 / 4);
        cleanSince_ = now;
    } else if (now - cleanSince_ >= 10s) {
        next = std::min(maxKbps_, k + std::max(100, k / 10));
        cleanSince_ = now;
    }
    if (next == k) return;
    kbps_.store(next);
    logT("relay: {} -> {} kbps (send {:.0f} ms, viewer backlog {})", k, next, sendMs_.load(),
         viewerQueue_.load());
    if (cb_.onBitrate) cb_.onBitrate(next);
}

} // namespace soi
