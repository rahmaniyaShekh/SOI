#pragma once
//
// The host's end of the rendezvous relay: the session's video and control
// messages over one WebSocket, end-to-end encrypted (net/RelayFrame.h), for
// when the two networks cannot reach each other directly.
//
// The path is TCP, so nothing is ever lost and there is nothing to repair --
// no NACK, no FEC, no PLI. What matters is not putting more into the pipe than
// it carries:
//
//   * frames are queued and sent from a thread of their own, so a full TCP
//     path can never stall the capture thread;
//   * if the queue backs up, queued delta frames are dropped and the encoder is
//     asked for a keyframe, because the viewer's decoder cannot resume from a
//     gap any other way;
//   * the bitrate steps down by a quarter when sends start to block or the
//     viewer reports a decode backlog, and creeps up after ten clean seconds.
//
// The free Workers plan counts every message the host sends as 1/20 of a
// request, so the session runs at no more than kRelayMaxFps frames a second.
//
#include "app/Rendezvous.h"
#include "net/RelayFrame.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace soi {

// One message per frame keeps a relayed viewer under ~20 messages a second.
constexpr int kRelayMaxFps = 15;

class RelayLink {
public:
    struct Callbacks {
        std::function<void()>                   onLive;        // first word from the viewer
        std::function<void()>                   onClosed;      // the relay is gone
        std::function<void()>                   onKeyframe;    // the viewer needs a keyframe
        std::function<void(int kbps)>           onBitrate;     // what the relay will carry
        std::function<void(const std::string&)> onControl;     // the viewer's control JSON
    };

    struct Stats {
        uint64_t framesSent   = 0;
        uint64_t bytesSent    = 0;
        uint64_t framesDropped = 0;
        int      kbps         = 0;
        double   sendMs       = 0;   // smoothed time one send spends in the socket
        int      viewerQueue  = 0;   // the viewer's decode backlog, in frames
    };

    RelayLink(std::unique_ptr<RelaySocket> socket, RelayKey key, Callbacks callbacks,
              int startKbps, int minKbps, int maxKbps);
    ~RelayLink();

    RelayLink(const RelayLink&) = delete;
    RelayLink& operator=(const RelayLink&) = delete;

    // Safe from any thread; never blocks on the network.
    void sendFrame(const uint8_t* annexB, size_t len, bool keyframe, int64_t ptsNs);
    void sendControl(const std::string& json);

    bool  live()   const { return live_.load(); }
    bool  closed() const { return closed_.load(); }
    Stats stats()  const;

    void close();

private:
    struct Outgoing {
        uint8_t              type = 0;
        bool                 video = false;
        bool                 keyframe = false;
        std::vector<uint8_t> payload;
    };

    void receiveLoop();
    void sendLoop();
    void adapt(std::chrono::steady_clock::time_point now);
    void congested(const char* why);
    void requestKeyframe();

    std::unique_ptr<RelaySocket> socket_;
    const RelayKey               key_;
    Callbacks                    cb_;
    const int                    minKbps_, maxKbps_;

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    std::deque<Outgoing>    queue_;
    bool                    stopping_ = false;
    bool                    waitingForKey_ = true;   // never start a viewer on a delta
    int64_t                 firstPtsNs_ = -1;

    std::atomic<bool>   live_{false};
    std::atomic<bool>   closed_{false};
    std::atomic<int>    kbps_{0};
    std::atomic<int>    viewerQueue_{0};
    std::atomic<int>    viewerDropped_{0};
    std::atomic<bool>   congested_{false};
    std::atomic<double> sendMs_{0};
    std::atomic<uint64_t> framesSent_{0}, bytesSent_{0}, framesDropped_{0};
    std::atomic<int64_t>  lastHeardMs_{0};

    std::chrono::steady_clock::time_point lastKeyRequest_{};
    std::chrono::steady_clock::time_point cleanSince_{};
    std::chrono::steady_clock::time_point lastAdapt_{};
    int                                   seenViewerDropped_ = 0;

    std::thread receiver_, sender_;
};

} // namespace soi
