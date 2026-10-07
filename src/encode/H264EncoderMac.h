#pragma once
//
// VideoToolbox H.264 encoder -- the macOS H264Encoder. Included by
// encode/H264Encoder.h, which defines EncoderConfig; the public interface is
// the Windows one, so the session code drives both the same way.
//
// What it produces matches the Windows encoder exactly where the far end can
// tell: Main profile (the SDP advertises profile-level-id 4d00xx), no B-frames
// (WebRTC cannot reorder), a long GOP with IDRs on demand, BT.709 video range,
// and an Annex-B byte stream with SPS/PPS in front of every IDR.
//
// Hardware encoders (every Apple silicon Mac, and Intel Macs with Quick Sync
// or a T2) are preferred; Apple's software encoder is the fallback.
//
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace soi {

class H264Encoder {
public:
    // Annex-B byte stream (start-code separated NALs). Invoked on a
    // VideoToolbox thread.
    using OutputCallback =
        std::function<void(const uint8_t* annexB, size_t len, bool keyframe, int64_t ptsNs)>;

    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    // Accept GPU-resident frames (IOSurface-backed CVPixelBuffers) via
    // submitTexture(). VideoToolbox takes those from any source, so this is
    // true whenever the GPU scaler/converter can be built. Call before start().
    bool enableGpuInput(const std::shared_ptr<GpuDevice>& device);
    bool usesGpuInput() const { return gpuInput_; }

    bool start(const EncoderConfig& cfg, OutputCallback onOutput);
    void stop();

    // Non-blocking. Returns false if the frame was dropped because the encoder
    // is backed up (more than kMaxQueue frames in flight).
    bool submit(const Nv12Buffer& frame, int64_t ptsNs);

    // The zero-copy path: `bgraPixelBuffer` is a CVPixelBufferRef, scaled and
    // converted to NV12 on the GPU and handed to the encoder as is.
    bool submitTexture(void* bgraPixelBuffer, int64_t ptsNs);

    void requestKeyframe();
    void setBitrate(int kbps);

    std::string describe() const { return description_; }
    bool        isHardware() const { return hardware_; }
    int         currentBitrateKbps() const { return bitrateKbps_.load(); }
    bool        usesQualityRateControl() const { return qualityRateControl_; }

    // Called from the VideoToolbox output callback; public only so that C
    // callback can reach it.
    void onEncoded(int status, void* sampleBuffer);

private:
    static constexpr int kMaxQueue = 3;

    bool createSession(bool requireHardware);
    void applyRateControl(int kbps);
    bool encodePixelBuffer(void* pixelBuffer, int64_t ptsNs);

    void*          session_ = nullptr;   // VTCompressionSessionRef
    EncoderConfig  cfg_{};
    OutputCallback onOutput_;
    std::string    description_;
    bool           hardware_ = false;
    bool           qualityRateControl_ = false;

    std::shared_ptr<GpuDevice> gpuDevice_;
    Nv12GpuConverter           gpuConverter_;
    bool                       gpuRequested_ = false;
    bool                       gpuInput_     = false;

    std::mutex           sessionMtx_;     // session_ create/destroy vs. property changes
    std::vector<uint8_t> scratch_;        // assembled output frame (VT thread only)

    std::atomic<bool> running_{false};
    std::atomic<bool> keyframeRequested_{false};
    std::atomic<int>  inFlight_{0};
    std::atomic<int>  bitrateKbps_{0};
    std::atomic<uint64_t> framesIn_{0}, framesOut_{0}, framesDropped_{0};
};

} // namespace soi
