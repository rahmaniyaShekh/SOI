#pragma once
//
// WebRTC transport built on libdatachannel.
//
// libdatachannel gives us ICE/DTLS/SRTP and RTP packetization but deliberately
// no bandwidth estimator -- Google's GCC lives in libwebrtc. This class therefore
// implements its own loss-based AIMD controller driven by RTCP Receiver Reports
// and REMB. That is weaker than GCC (it reacts to loss instead of predicting
// congestion from delay gradient) but it prevents the classic failure of
// blasting 8 Mbps into a 3 Mbps link indefinitely.
//
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rtc/rtc.hpp>

namespace soi {

struct StreamerConfig {
    // Multiple STUN servers are gathered in parallel; the first to answer wins,
    // so listing two costs nothing and survives one being down. Empty => no STUN
    // at all (host candidates only, same-LAN, genuinely zero external contact).
    std::vector<std::string> stunUrls;

    // Optional TURN relay. Only used if direct paths fail, and it never sees
    // plaintext: DTLS-SRTP terminates at the peers, so a relay forwards
    // ciphertext. It does learn both IPs and the traffic volume.
    std::string turnUrl;
    std::string turnUser;
    std::string turnPass;

    int width          = 1920;
    int height         = 1080;
    int fps            = 30;
    int maxBitrateKbps = 6000;
    int minBitrateKbps = 600;
};

struct StreamStats {
    uint64_t framesSent    = 0;
    uint64_t bytesSent     = 0;
    uint64_t keyframesSent = 0;
    uint64_t pliCount      = 0;
    uint64_t nackCount     = 0;
    double   lossFraction  = 0.0;    // 0..1, from the most recent RR
    double   rttMs         = 0.0;
    int      targetBitrateKbps = 0;
    bool     rembSeen      = false;
};

class Streamer {
public:
    using KeyframeRequestFn = std::function<void()>;
    using BitrateTargetFn   = std::function<void(int kbps)>;
    using StateFn           = std::function<void(const std::string& state)>;
    // The viewer asking for a different quality level, by name. Invoked on the
    // data-channel thread, so the handler must not block.
    using QualityRequestFn  = std::function<void(const std::string& level)>;
    // The viewer picking a different monitor to watch, by index.
    using MonitorRequestFn  = std::function<void(int index)>;
    // Fired when the control channel opens, so the sender can announce the
    // levels on offer and which one is currently running.
    using ControlReadyFn    = std::function<void()>;

    Streamer() = default;
    ~Streamer();

    Streamer(const Streamer&) = delete;
    Streamer& operator=(const Streamer&) = delete;

    // Creates the peer connection, the send-only H.264 track and the control
    // data channel, then starts ICE gathering.
    bool start(const StreamerConfig& cfg);
    void stop();

    // Blocks until ICE gathering completes. There is no signalling channel to
    // trickle candidates over, so the offer is only useful once it is complete.
    bool waitForGathering(std::chrono::milliseconds timeout);

    std::string localDescriptionSdp() const;
    bool        acceptAnswer(const std::string& sdp, std::string& error);
    bool        waitForConnected(std::chrono::milliseconds timeout);

    bool isConnected() const { return connected_.load(); }
    bool hasFailed()   const { return failed_.load(); }

    // Annex-B access unit. Safe to call from the encoder thread.
    void sendFrame(const uint8_t* annexB, size_t len, bool keyframe, int64_t ptsNs);

    void setKeyframeRequestHandler(KeyframeRequestFn fn) { onKeyframe_ = std::move(fn); }
    void setBitrateTargetHandler(BitrateTargetFn fn)     { onBitrate_  = std::move(fn); }
    void setStateHandler(StateFn fn)                     { onState_    = std::move(fn); }
    void setQualityRequestHandler(QualityRequestFn fn)   { onQuality_  = std::move(fn); }
    void setMonitorRequestHandler(MonitorRequestFn fn)   { onMonitor_  = std::move(fn); }
    void setControlReadyHandler(ControlReadyFn fn)       { onControlReady_ = std::move(fn); }

    bool controlIsOpen() const;

    StreamStats stats() const;
    void        sendControl(const std::string& text);

private:
    void handleRtcp(const std::byte* data, size_t size);
    void handleReceiverReport(const uint8_t* block);
    void handleRemb(uint64_t bitsPerSecond);
    void handleControlMessage(const std::string& text);
    void runAimd(double lossFraction);
    void applyTarget(int kbps);

    std::shared_ptr<rtc::PeerConnection>           pc_;
    std::shared_ptr<rtc::Track>                    track_;
    std::shared_ptr<rtc::DataChannel>              control_;
    std::shared_ptr<rtc::RtpPacketizationConfig>   rtpConfig_;
    std::shared_ptr<rtc::RtcpSrReporter>           srReporter_;

    StreamerConfig cfg_{};

    mutable std::mutex      mtx_;
    std::condition_variable cv_;
    bool                    gatheringComplete_ = false;

    std::atomic<bool> connected_{false};
    std::atomic<bool> failed_{false};
    std::atomic<bool> running_{false};

    int64_t firstPtsNs_ = -1;

    KeyframeRequestFn onKeyframe_;
    BitrateTargetFn   onBitrate_;
    StateFn           onState_;
    QualityRequestFn  onQuality_;
    MonitorRequestFn  onMonitor_;
    ControlReadyFn    onControlReady_;

    // Stats / control state.
    std::atomic<uint64_t> framesSent_{0}, bytesSent_{0}, keyframes_{0};
    std::atomic<uint64_t> pliCount_{0}, nackCount_{0};
    std::atomic<int>      targetKbps_{0};
    std::atomic<bool>     rembSeen_{false};
    std::atomic<double>   lossFraction_{0.0};
    std::atomic<double>   rttMs_{0.0};

    std::chrono::steady_clock::time_point lastAimd_{};
    std::chrono::steady_clock::time_point lastKeyframeRequest_{};
};

} // namespace soi
