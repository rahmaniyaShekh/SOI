#pragma once
//
// H.264 encoder: Media Foundation on Windows (below), VideoToolbox on macOS
// (encode/H264EncoderMac.h, the same class and interface).
//
// Windows:
// Prefers a hardware MFT (NVENC / Quick Sync / AMF) and falls back to the
// Microsoft software encoder. Hardware MFTs are ASYNCHRONOUS -- they must be
// unlocked with MF_TRANSFORM_ASYNC_UNLOCK and driven by an event loop rather
// than the synchronous ProcessInput/ProcessOutput call-and-check pattern. Both
// modes are implemented here; getting this wrong is the usual reason a
// hand-rolled MF encoder "works on Intel but hangs on NVIDIA".
//
#include "encode/ColorConvert.h"
#include "encode/Quality.h"   // h264LevelForResolution
#include "gpu/GpuPipeline.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace soi {

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

} // namespace soi

#if !defined(_WIN32)
#include "encode/H264EncoderMac.h"
#else
#include "util/Win.h"

struct IMFTransform;
struct IMFMediaEventGenerator;
struct IMFDXGIDeviceManager;
struct ICodecAPI;
struct IMFSample;
struct IMFMediaBuffer;
struct ID3D11Texture2D;

namespace soi {

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

    // Offer the encoder a D3D11 device to take its input on, instead of system
    // memory. Call BEFORE start(); it selects the transform in order to ask
    // whether that transform is D3D11-aware, and the answer decides which
    // submit() the caller may use.
    //
    // Returns false when this machine's encoder cannot take GPU input -- the
    // Microsoft software MFT never can, and some older hardware ones do not
    // either. That is not an error, it is the CPU path.
    bool enableGpuInput(const std::shared_ptr<GpuDevice>& device);

    // True once start() has confirmed the MFT really took the device. Only then
    // is submitTexture() usable.
    bool usesGpuInput() const { return gpuInput_; }

    bool start(const EncoderConfig& cfg, OutputCallback onOutput);
    void stop();

    // Non-blocking. Returns false if the frame was dropped because the encoder
    // is backed up (queue depth > kMaxQueue) -- dropping is correct here, since
    // a stale screen frame has no value.
    bool submit(const Nv12Buffer& frame, int64_t ptsNs);

    // The zero-copy path: `bgra` stays on the GPU, is converted to NV12 by a
    // shader on the encoder's own device, and is handed to the MFT as a DXGI
    // surface. Nothing crosses the bus.
    //
    // Only valid when usesGpuInput() is true. Same drop-when-backed-up contract
    // as submit().
    bool submitTexture(ID3D11Texture2D* bgra, int64_t ptsNs);

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
    void attachD3dManager();
    bool configureTypes();
    bool configureCodecApi();
    bool beginStreaming();
    void eventLoop();
    // Shared tail of submit() and submitTexture(): sample, queue, drop policy.
    bool dispatch(::IMFMediaBuffer* buffer, int64_t ptsNs);
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

    // The GPU input path. `gpuDevice_` is shared with the capture side so a
    // captured texture can be converted and encoded without ever being copied
    // to system memory; `gpuManager_` is what Media Foundation wants that device
    // wrapped in.
    std::shared_ptr<GpuDevice>      gpuDevice_;
    ComPtr<IMFDXGIDeviceManager>    gpuManager_;
    Nv12GpuConverter                gpuConverter_;
    bool                            gpuRequested_ = false;
    bool                            gpuInput_     = false;

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

#endif // _WIN32
