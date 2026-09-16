#pragma once
//
// Media Foundation H.264 encoder.
//
// Prefers a hardware MFT (NVENC / Quick Sync / AMF) and falls back to the
// Microsoft software encoder. Hardware MFTs are ASYNCHRONOUS -- they must be
// unlocked with MF_TRANSFORM_ASYNC_UNLOCK and driven by an event loop rather
// than the synchronous ProcessInput/ProcessOutput call-and-check pattern. Both
// modes are implemented here; getting this wrong is the usual reason a
// hand-rolled MF encoder "works on Intel but hangs on NVIDIA".
//
#include "encode/ColorConvert.h"
#include "util/Win.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct IMFTransform;
struct IMFMediaEventGenerator;
struct ICodecAPI;
struct IMFSample;

namespace soi {

// Smallest H.264 level (as a level_idc, e.g. 40 == level 4.0) that can carry the
// given resolution and frame rate. Used both to configure the encoder and to
// build a truthful profile-level-id in the SDP -- understating the level makes
// strict decoders reject the stream.
int h264LevelForResolution(int width, int height, int fps);

struct EncoderConfig {
    int  width       = 1920;
    int  height      = 1080;
    int  fps         = 30;
    int  bitrateKbps = 6000;
    int  gopSeconds  = 10;      // long GOP: IDRs are expensive, PLI gets us one on demand
    bool preferHardware = true;

    // Prefer peak-constrained VBR over CBR, so a frame full of small text can
    // spend more bits than a nearly-empty one instead of every frame being
    // squeezed into the same quota. `quality` biases that allocation where the
    // encoder honours it.
    //
    // Note this is about spending each frame's budget well, NOT about holding
    // quality steady as bandwidth moves -- the frame-rate rule in Quality.h does
    // that. When the mode is refused the encoder falls back to CBR and says so.
    int  quality = 80;
    bool preferQualityRateControl = true;
};

class H264Encoder {
public:
    // Annex-B byte stream (start-code separated NALs). Invoked on the encoder
    // thread in async mode, or the caller's thread in sync mode.
    using OutputCallback =
        std::function<void(const uint8_t* annexB, size_t len, bool keyframe, int64_t ptsNs)>;

    // Both are defined in the .cpp on purpose. The COM interfaces below are only
    // forward-declared here, and an inline (even defaulted) constructor would
    // instantiate ComPtr<ICodecAPI>::~ComPtr in every translation unit that
    // merely creates an encoder -- which needs the complete interface.
    H264Encoder();
    ~H264Encoder();

    H264Encoder(const H264Encoder&) = delete;
    H264Encoder& operator=(const H264Encoder&) = delete;

    bool start(const EncoderConfig& cfg, OutputCallback onOutput);
    void stop();

    // Non-blocking. Returns false if the frame was dropped because the encoder
    // is backed up (queue depth > kMaxQueue) -- dropping is correct here, since
    // a stale screen frame has no value.
    bool submit(const Nv12Buffer& frame, int64_t ptsNs);

    void requestKeyframe();
    void setBitrate(int kbps);

    std::string describe() const { return description_; }
    bool        isHardware() const { return hardware_; }
    int         currentBitrateKbps() const { return bitrateKbps_.load(); }

    // True when peak-constrained VBR actually took effect, so callers can report
    // what is really happening rather than what was asked for.
    bool usesQualityRateControl() const { return qualityRateControl_; }

private:
    static constexpr size_t kMaxQueue = 3;

    bool selectTransform();
    bool configureTypes();
    bool configureCodecApi();
    bool beginStreaming();
    void eventLoop();
    void pumpFeed();                       // moves queued samples into the MFT
    bool processInputLocked(IMFSample* sample);
    void drainOutputs();                   // sync mode
    bool processOneOutput();
    void emit(IMFSample* sample);
    void cacheSequenceHeader();
    bool applyPendingKeyframeRequest();

    ComPtr<IMFTransform>           mft_;
    ComPtr<IMFMediaEventGenerator> events_;
    ComPtr<ICodecAPI>              codec_;

    EncoderConfig  cfg_{};
    OutputCallback onOutput_;
    std::string    description_;
    bool           hardware_ = false;
    bool           qualityRateControl_ = false;
    bool           async_    = false;
    bool           mftProvidesSamples_ = false;
    DWORD          inputStreamId_  = 0;
    DWORD          outputStreamId_ = 0;

    std::vector<uint8_t> sequenceHeader_;   // cached SPS+PPS, Annex-B
    std::vector<uint8_t> scratch_;          // assembled output frame

    std::thread             thread_;
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::deque<ComPtr<IMFSample>> queue_;
    int                     needInput_ = 0;

    std::atomic<bool> running_{false};
    std::atomic<bool> keyframeRequested_{false};
    std::atomic<int>  bitrateKbps_{0};
    std::atomic<int>  pendingBitrateKbps_{0};
    std::atomic<uint64_t> framesIn_{0}, framesOut_{0}, framesDropped_{0};
};

} // namespace soi
