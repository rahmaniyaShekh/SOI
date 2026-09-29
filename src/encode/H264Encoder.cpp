#include "encode/H264Encoder.h"
#include "util/Log.h"

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wmcodecdsp.h>
#include <d3d11.h>

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

HRESULT setCodecUInt32(ICodecAPI* api, const GUID& prop, UINT32 value) {
    if (!api) return E_POINTER;
    VARIANT v;
    VariantInit(&v);
    v.vt    = VT_UI4;
    v.ulVal = value;
    const HRESULT hr = api->SetValue(&prop, &v);
    VariantClear(&v);
    return hr;
}

HRESULT setCodecBool(ICodecAPI* api, const GUID& prop, bool value) {
    if (!api) return E_POINTER;
    VARIANT v;
    VariantInit(&v);
    v.vt      = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    const HRESULT hr = api->SetValue(&prop, &v);
    VariantClear(&v);
    return hr;
}

// Returns the NAL type of the first NAL unit in an Annex-B buffer, or -1.
int firstNalType(const uint8_t* p, size_t len) {
    for (size_t i = 0; i + 3 < len; ++i) {
        if (p[i] == 0 && p[i + 1] == 0) {
            if (p[i + 2] == 1)                      return p[i + 3] & 0x1F;
            if (p[i + 2] == 0 && i + 4 < len && p[i + 3] == 1) return p[i + 4] & 0x1F;
        }
        if (i > 8) break;   // the header, if present, is at the very front
    }
    return -1;
}

} // namespace

// H.264 level selection. Getting this wrong is not cosmetic: a decoder that
// trusts an under-stated level may refuse the stream or allocate too few DPB
// slots. Table is (levelIdc, maxMacroblocksPerSecond, maxFrameMacroblocks).
int h264LevelForResolution(int width, int height, int fps) {
    struct Level { int idc; long long mbps; long long frameMbs; };
    static constexpr Level kLevels[] = {
        {30, 40500,  1620},  {31, 108000, 3600},  {32, 216000, 5120},
        {40, 245760, 8192},  {41, 245760, 8192},  {42, 522240, 8704},
        {50, 589824, 22080}, {51, 983040, 36864}, {52, 2073600, 36864},
    };

    const long long frameMbs =
        static_cast<long long>((width + 15) / 16) * ((height + 15) / 16);
    const long long mbps = frameMbs * (fps > 0 ? fps : 30);

    for (const auto& l : kLevels)
        if (frameMbs <= l.frameMbs && mbps <= l.mbps) return l.idc;
    return 52;
}

// ---------------------------------------------------------------------------

H264Encoder::H264Encoder()  = default;
H264Encoder::~H264Encoder() { stop(); }

bool H264Encoder::selectTransform() {
    MFT_REGISTER_TYPE_INFO inInfo{MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO outInfo{MFMediaType_Video, MFVideoFormat_H264};

    auto tryEnum = [&](UINT32 flags, bool hardware) -> bool {
        IMFActivate** activates = nullptr;
        UINT32        count     = 0;

        HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_ENCODER, flags, &inInfo, &outInfo,
                               &activates, &count);
        if (FAILED(hr) || count == 0) {
            if (activates) CoTaskMemFree(activates);
            return false;
        }

        bool ok = false;
        for (UINT32 i = 0; i < count && !ok; ++i) {
            ComPtr<IMFTransform> candidate;
            if (FAILED(activates[i]->ActivateObject(IID_PPV_ARGS(candidate.put()))))
                continue;

            wchar_t* name = nullptr;
            UINT32   nameLen = 0;
            activates[i]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &name, &nameLen);

            mft_        = candidate;
            hardware_   = hardware;
            description_ = soi::format("{} ({})",
                                       name ? toUtf8(std::wstring_view(name, nameLen))
                                            : "unnamed H.264 MFT",
                                       hardware ? "hardware" : "software");
            if (name) CoTaskMemFree(name);
            ok = true;
        }

        for (UINT32 i = 0; i < count; ++i) activates[i]->Release();
        CoTaskMemFree(activates);
        return ok;
    };

    if (cfg_.preferHardware &&
        tryEnum(MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_ASYNCMFT |
                    MFT_ENUM_FLAG_SORTANDFILTER,
                true)) {
        return true;
    }
    if (cfg_.preferHardware)
        logW("no hardware H.264 encoder available; falling back to software "
             "(expect noticeably higher CPU usage)");

    return tryEnum(MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER, false);
}

// Ask the selected MFT to take its input as D3D11 surfaces on our device.
//
// Silent about everything except an outright failure to talk to a transform that
// said it was D3D11-aware: "this encoder is not D3D11-aware" is an ordinary
// property of the Microsoft software MFT and of some older hardware ones, not a
// problem to report. The caller learns the outcome from usesGpuInput().
void H264Encoder::attachD3dManager() {
    gpuInput_ = false;
    if (!gpuRequested_ || !gpuDevice_ || !mft_) return;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(mft_->GetAttributes(attrs.put())) || !attrs) return;

    if (MFGetAttributeUINT32(attrs.get(), MF_SA_D3D11_AWARE, 0) == 0) {
        logI("encoder: {} does not take D3D11 input; using the CPU converter",
             description_);
        return;
    }

    UINT resetToken = 0;
    if (FAILED(MFCreateDXGIDeviceManager(&resetToken, gpuManager_.put())) || !gpuManager_) {
        logW("gpu: could not create a DXGI device manager; using the CPU converter");
        return;
    }
    HRESULT hr = gpuManager_->ResetDevice(gpuDevice_->device(), resetToken);
    if (FAILED(hr)) {
        logW("gpu: the device manager rejected our D3D11 device: {}", hrString(hr));
        gpuManager_.reset();
        return;
    }

    // ULONG_PTR, not a pointer: the MFT AddRefs the manager itself.
    hr = mft_->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                              reinterpret_cast<ULONG_PTR>(gpuManager_.get()));
    if (FAILED(hr)) {
        // A transform that advertised MF_SA_D3D11_AWARE and then refused the
        // manager is worth saying out loud -- it usually means the device is on
        // an adapter the encoder cannot reach.
        logW("gpu: {} advertised D3D11 support but refused the device: {}",
             description_, hrString(hr));
        gpuManager_.reset();
        return;
    }

    gpuInput_ = true;
}

bool H264Encoder::enableGpuInput(const std::shared_ptr<GpuDevice>& device) {
    if (running_.load()) return false;
    if (!device) return false;

    // The transform has to exist before we can ask whether it is D3D11-aware, so
    // select it now. start() reuses whatever this leaves behind.
    if (!mft_ && !selectTransform()) return false;

    ComPtr<IMFAttributes> attrs;
    if (FAILED(mft_->GetAttributes(attrs.put())) || !attrs ||
        MFGetAttributeUINT32(attrs.get(), MF_SA_D3D11_AWARE, 0) == 0) {
        logI("encoder: {} cannot take GPU input", description_);
        return false;
    }

    gpuDevice_    = device;
    gpuRequested_ = true;
    return true;
}

bool H264Encoder::configureTypes() {
    // Hardware MFTs are asynchronous and must be unlocked before any other call.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(mft_->GetAttributes(attrs.put())) && attrs) {
        async_ = MFGetAttributeUINT32(attrs.get(), MF_TRANSFORM_ASYNC, 0) != 0;
        if (async_) {
            const HRESULT hr = attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
            if (FAILED(hr)) {
                logE("MF_TRANSFORM_ASYNC_UNLOCK failed: {}", hrString(hr));
                return false;
            }
        }
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);   // best-effort
    }

    // The D3D device has to be handed over HERE: after the async unlock, because
    // a locked MFT rejects every call, and before the media types, because that
    // is when the MFT decides how it will allocate its input. Sending it later
    // is accepted and then quietly ignored, which looks exactly like the GPU
    // path working while every frame is still being copied through memory.
    attachD3dManager();

    DWORD inIds[1] = {0}, outIds[1] = {0};
    if (SUCCEEDED(mft_->GetStreamIDs(1, inIds, 1, outIds))) {
        inputStreamId_  = inIds[0];
        outputStreamId_ = outIds[0];
    }

    // Encoders require the OUTPUT type to be set first: the input format they
    // will accept depends on the compressed format they were configured for.
    ComPtr<IMFMediaType> outType;
    HRESULT hr = MFCreateMediaType(outType.put());
    if (FAILED(hr)) return false;

    const UINT32 bps = static_cast<UINT32>(cfg_.bitrateKbps) * 1000u;

    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bps);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, FALSE);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Main);
    outType->SetUINT32(MF_MT_MPEG2_LEVEL,
                       static_cast<UINT32>(h264LevelForResolution(cfg_.width, cfg_.height,
                                                                 cfg_.fps)));
    MFSetAttributeSize(outType.get(), MF_MT_FRAME_SIZE,
                       static_cast<UINT32>(cfg_.width), static_cast<UINT32>(cfg_.height));
    MFSetAttributeRatio(outType.get(), MF_MT_FRAME_RATE,
                        static_cast<UINT32>(cfg_.fps), 1);
    MFSetAttributeRatio(outType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = mft_->SetOutputType(outputStreamId_, outType.get(), 0);
    if (FAILED(hr)) {
        logE("SetOutputType({}x{} @{}fps, {} kbps) failed: {}",
             cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrateKbps, hrString(hr));
        return false;
    }

    ComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(inType.put());
    if (FAILED(hr)) return false;

    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    inType->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(cfg_.width));
    MFSetAttributeSize(inType.get(), MF_MT_FRAME_SIZE,
                       static_cast<UINT32>(cfg_.width), static_cast<UINT32>(cfg_.height));
    MFSetAttributeRatio(inType.get(), MF_MT_FRAME_RATE,
                        static_cast<UINT32>(cfg_.fps), 1);
    MFSetAttributeRatio(inType.get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);

    hr = mft_->SetInputType(inputStreamId_, inType.get(), 0);
    if (FAILED(hr)) {
        logE("SetInputType(NV12 {}x{}) failed: {}", cfg_.width, cfg_.height, hrString(hr));
        return false;
    }

    MFT_OUTPUT_STREAM_INFO info{};
    if (SUCCEEDED(mft_->GetOutputStreamInfo(outputStreamId_, &info))) {
        mftProvidesSamples_ =
            (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
    }
    return true;
}

namespace {
// Headroom a complex frame may burst into, above the average.
//
// Kept modest deliberately. Measured on the Quick Sync MFT with synthetic
// worst-case content, a 1.5x peak produced a two-second average of 4801 kbps
// against a 3000 kbps mean -- it rides the peak and then some. Whatever ratio
// is set here is effectively what the link must be able to absorb, so the
// caller reserves exactly this much headroom when it picks the frame rate.
constexpr int kPeakPercent = 125;
int peakFor(int meanKbps) { return std::max(1, meanKbps) * kPeakPercent / 100; }
} // namespace

bool H264Encoder::configureCodecApi() {
    if (FAILED(mft_->QueryInterface(IID_PPV_ARGS(codec_.put())))) {
        logW("encoder does not expose ICodecAPI; latency and rate-control tuning skipped");
        return true;   // not fatal
    }

    auto attempt = [&](const char* what, HRESULT hr) {
        if (FAILED(hr)) logT("codec property '{}' rejected: {}", what, hrString(hr));
    };

    // Low-latency mode disables lookahead and B-frames. Without it, most
    // hardware encoders buffer several frames and add 50-100ms of latency.
    attempt("AVLowLatencyMode",
            setCodecBool(codec_.get(), CODECAPI_AVLowLatencyMode, true));

    // Rate control: peak-constrained VBR in preference to CBR.
    //
    // The point is bit ALLOCATION, not the average. A screen frame full of small
    // text needs more bits than a near-empty one, and VBR gives it them; CBR
    // pads the empty frame and then starves the busy one, which is exactly
    // backwards for legibility.
    //
    // It is peak-CONSTRAINED for a reason that was measured, not assumed:
    // unconstrained quality mode (Intel's ICQ) ignores the ceiling outright --
    // it produced 8316 kbps against a 3000 kbps cap in the encoder self-test.
    // On a real link that is not "high quality", it is packet loss. The peak cap
    // keeps bursts bounded while still letting a complex frame spend.
    //
    // Holding quality steady as bandwidth moves is NOT this setting's job -- the
    // frame-rate rule in Quality.h does that. This just spends each frame's
    // budget well.
    qualityRateControl_ = false;
    if (cfg_.preferQualityRateControl) {
        const HRESULT mode = setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonRateControlMode,
                                            eAVEncCommonRateControlMode_PeakConstrainedVBR);
        if (SUCCEEDED(mode) &&
            SUCCEEDED(setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonMeanBitRate,
                                     static_cast<UINT32>(cfg_.bitrateKbps) * 1000u))) {
            qualityRateControl_ = true;
            attempt("MaxBitRate",
                    setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonMaxBitRate,
                                   static_cast<UINT32>(peakFor(cfg_.bitrateKbps)) * 1000u));
            attempt("Quality",
                    setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonQuality,
                                   static_cast<UINT32>(std::clamp(cfg_.quality, 1, 100))));
        } else {
            logT("peak-constrained VBR unavailable; using CBR");
        }
    }

    if (!qualityRateControl_) {
        attempt("RateControlMode",
                setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonRateControlMode,
                               eAVEncCommonRateControlMode_CBR));
        attempt("MeanBitRate",
                setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonMeanBitRate,
                               static_cast<UINT32>(cfg_.bitrateKbps) * 1000u));
    }

    attempt("BPictureCount",
            setCodecUInt32(codec_.get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0));
    attempt("GOPSize",
            setCodecUInt32(codec_.get(), CODECAPI_AVEncMPVGOPSize,
                           static_cast<UINT32>(std::max(1, cfg_.fps * cfg_.gopSeconds))));
    // 0 = quality, 100 = speed. Screen content is latency-sensitive, not
    // archival, so bias toward speed without going to the extreme.
    attempt("QualityVsSpeed",
            setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonQualityVsSpeed, 33));

    bitrateKbps_.store(cfg_.bitrateKbps);
    return true;
}

bool H264Encoder::beginStreaming() {
    if (async_) {
        const HRESULT hr = mft_->QueryInterface(IID_PPV_ARGS(events_.put()));
        if (FAILED(hr)) {
            logE("async MFT has no IMFMediaEventGenerator: {}", hrString(hr));
            return false;
        }
    }

    mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    running_.store(true);
    if (async_) thread_ = std::thread([this] { eventLoop(); });
    return true;
}

bool H264Encoder::start(const EncoderConfig& cfg, OutputCallback onOutput) {
    if (running_.load()) return true;

    cfg_      = cfg;
    onOutput_ = std::move(onOutput);
    cfg_.width  &= ~1;
    cfg_.height &= ~1;

    // enableGpuInput() may already have selected one in order to interrogate it.
    if (!mft_ && !selectTransform()) {
        logE("no H.264 encoder MFT found (NV12 in, H264 out)");
        return false;
    }
    if (!configureTypes())  { mft_.reset(); return false; }
    if (!configureCodecApi()) { mft_.reset(); return false; }
    if (!beginStreaming())  { mft_.reset(); return false; }

    // The converter is built last, at the size the encoder was actually
    // configured for, so a resolution change rebuilds both together.
    if (gpuInput_ && !gpuConverter_.init(gpuDevice_, cfg_.width, cfg_.height)) {
        // The MFT holds our device either way, and every D3D11-aware MFT still
        // accepts system-memory samples, so dropping back here is safe rather
        // than fatal.
        logW("gpu: NV12 conversion is unavailable at {}x{}; using the CPU converter",
             cfg_.width, cfg_.height);
        gpuInput_ = false;
    }

    cacheSequenceHeader();

    logI("encoder: {} {}x{} @{}fps target {} kbps, GOP {}s, mode {}, input {}",
         description_, cfg_.width, cfg_.height, cfg_.fps, cfg_.bitrateKbps,
         cfg_.gopSeconds, async_ ? "async" : "sync",
         gpuInput_ ? "GPU textures (zero-copy)" : "system memory");
    return true;
}

void H264Encoder::stop() {
    if (!running_.exchange(false)) {
        gpuConverter_.reset();
        gpuManager_.reset();
        gpuInput_ = false;
        mft_.reset();
        events_.reset();
        codec_.reset();
        return;
    }

    // DRAIN always terminates with METransformDrainComplete, which is what
    // unblocks the event thread's GetEvent().
    if (mft_) mft_->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();

    if (mft_) {
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        mft_->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }

    {
        std::lock_guard lk(mtx_);
        queue_.clear();
        needInput_ = 0;
    }

    // The converter's textures are still referenced by any sample the MFT has
    // not finished with, so it goes after the drain above, never before.
    gpuConverter_.reset();
    gpuManager_.reset();
    gpuInput_ = false;

    events_.reset();
    codec_.reset();
    mft_.reset();

    logI("encoder stopped: {} frames in, {} out, {} dropped",
         framesIn_.load(), framesOut_.load(), framesDropped_.load());
}

bool H264Encoder::submit(const Nv12Buffer& frame, int64_t ptsNs) {
    if (!running_.load() || !mft_) return false;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer(static_cast<DWORD>(frame.size()), buffer.put());
    if (FAILED(hr)) return false;

    BYTE* dst = nullptr;
    DWORD maxLen = 0;
    hr = buffer->Lock(&dst, &maxLen, nullptr);
    if (FAILED(hr)) return false;
    std::memcpy(dst, frame.data(), frame.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(frame.size()));

    return dispatch(buffer.get(), ptsNs);
}

bool H264Encoder::submitTexture(ID3D11Texture2D* bgra, int64_t ptsNs) {
    if (!running_.load() || !mft_ || !gpuInput_ || !bgra) return false;

    // Capture texture -> NV12 texture, on the GPU, no readback.
    ID3D11Texture2D* nv12 = gpuConverter_.convert(bgra);
    if (!nv12) {
        framesDropped_.fetch_add(1);
        return false;
    }

    // Wrap the texture as an MF buffer. This takes a reference on the texture,
    // which is exactly why the converter hands out a ring rather than one
    // surface: the encoder is still reading this frame when the next arrives.
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), nv12, 0, FALSE,
                                           buffer.put());
    if (FAILED(hr)) {
        logT("gpu: MFCreateDXGISurfaceBuffer failed: {}", hrString(hr));
        framesDropped_.fetch_add(1);
        return false;
    }

    // A DXGI buffer starts with a current length of zero, and an MFT handed a
    // zero-length sample silently encodes nothing. IMF2DBuffer knows the real
    // size including the driver's row padding, so ask it rather than computing
    // w*h*3/2 and hoping the stride matches.
    ComPtr<IMF2DBuffer> twoD;
    DWORD length = 0;
    if (SUCCEEDED(buffer.as(twoD)) && SUCCEEDED(twoD->GetContiguousLength(&length)))
        buffer->SetCurrentLength(length);

    return dispatch(buffer.get(), ptsNs);
}

// Everything after "we have a buffer": wrap it in a sample, and either drive the
// MFT directly (sync) or queue it for the event thread (async). Shared so the
// system-memory and GPU paths cannot drift apart in their drop policy.
bool H264Encoder::dispatch(::IMFMediaBuffer* buffer, int64_t ptsNs) {
    ComPtr<IMFSample> sample;
    HRESULT hr = MFCreateSample(sample.put());
    if (FAILED(hr)) return false;
    sample->AddBuffer(buffer);
    sample->SetSampleTime(ptsNs / 100);                       // MF uses 100ns units
    sample->SetSampleDuration(10'000'000LL / std::max(1, cfg_.fps));

    framesIn_.fetch_add(1);

    if (!async_) {
        applyPendingKeyframeRequest();
        {
            std::lock_guard lk(mtx_);
            hr = mft_->ProcessInput(inputStreamId_, sample.get(), 0);
        }
        if (hr == MF_E_NOTACCEPTING) {
            drainOutputs();
            std::lock_guard lk(mtx_);
            hr = mft_->ProcessInput(inputStreamId_, sample.get(), 0);
        }
        if (FAILED(hr)) {
            logT("ProcessInput failed: {}", hrString(hr));
            framesDropped_.fetch_add(1);
            return false;
        }
        drainOutputs();
        return true;
    }

    {
        std::lock_guard lk(mtx_);
        // A stale screen frame has no value; drop the oldest rather than adding
        // latency by growing the queue.
        while (queue_.size() >= kMaxQueue) {
            queue_.pop_front();
            framesDropped_.fetch_add(1);
        }
        queue_.push_back(sample);
    }
    pumpFeed();
    return true;
}

void H264Encoder::pumpFeed() {
    // The lock is deliberately held across ProcessInput. pumpFeed() runs on both
    // the caller's thread (from submit) and the encoder's event thread (from
    // METransformNeedInput), and an MFT does not tolerate two concurrent
    // ProcessInput calls. ProcessOutput stays outside this lock -- async MFTs are
    // explicitly designed for input and output to be driven from different
    // threads, and serialising them would halve throughput.
    std::lock_guard lk(mtx_);
    while (running_.load() && needInput_ > 0 && !queue_.empty()) {
        ComPtr<IMFSample> sample = std::move(queue_.front());
        queue_.pop_front();
        --needInput_;

        applyPendingKeyframeRequest();
        const HRESULT hr = mft_->ProcessInput(inputStreamId_, sample.get(), 0);
        if (FAILED(hr)) {
            logT("async ProcessInput failed: {}", hrString(hr));
            framesDropped_.fetch_add(1);
        }
    }
}

bool H264Encoder::applyPendingKeyframeRequest() {
    bool acted = false;

    if (keyframeRequested_.exchange(false) && codec_) {
        // Must be set immediately before the input sample that should become the
        // IDR; it is a one-shot property, not a mode.
        const HRESULT hr =
            setCodecUInt32(codec_.get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
        if (FAILED(hr)) logT("force keyframe rejected: {}", hrString(hr));
        acted = true;
    }

    const int pending = pendingBitrateKbps_.exchange(0);
    if (pending > 0 && codec_) {
        // In VBR both numbers move together: the average is the new target and
        // the peak keeps its headroom above it. Moving only the mean would leave
        // a stale peak from a much faster link still authorising bursts the
        // current one cannot absorb.
        HRESULT hr = setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonMeanBitRate,
                                    static_cast<UINT32>(pending) * 1000u);
        if (SUCCEEDED(hr) && qualityRateControl_)
            setCodecUInt32(codec_.get(), CODECAPI_AVEncCommonMaxBitRate,
                           static_cast<UINT32>(peakFor(pending)) * 1000u);
        if (SUCCEEDED(hr)) {
            bitrateKbps_.store(pending);
            logT("encoder bitrate -> {} kbps{}", pending,
                 qualityRateControl_ ? soi::format(" (peak {})", peakFor(pending)) : "");
        } else {
            logT("dynamic bitrate change rejected: {}", hrString(hr));
        }
        acted = true;
    }
    return acted;
}

void H264Encoder::requestKeyframe() { keyframeRequested_.store(true); }

void H264Encoder::setBitrate(int kbps) {
    if (kbps <= 0) return;
    if (kbps == bitrateKbps_.load()) return;
    pendingBitrateKbps_.store(kbps);
}

void H264Encoder::eventLoop() {
    // MF objects live in the MTA; the event thread must join it explicitly.
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOwned = SUCCEEDED(coHr);

    while (running_.load()) {
        ComPtr<IMFMediaEvent> ev;
        const HRESULT hr = events_->GetEvent(0, ev.put());   // blocking
        if (FAILED(hr)) {
            if (running_.load()) logT("GetEvent failed: {}", hrString(hr));
            break;
        }

        MediaEventType type = MEUnknown;
        ev->GetType(&type);

        switch (type) {
            case METransformNeedInput: {
                { std::lock_guard lk(mtx_); ++needInput_; }
                pumpFeed();
                break;
            }
            case METransformHaveOutput:
                processOneOutput();
                break;
            case METransformDrainComplete:
                if (!running_.load()) { if (comOwned) CoUninitialize(); return; }
                mft_->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
                break;
            case MEError:
                logE("encoder reported MEError; stopping encode loop");
                running_.store(false);
                break;
            default:
                break;
        }
    }

    if (comOwned) CoUninitialize();
}

void H264Encoder::drainOutputs() {
    while (processOneOutput()) {}
}

bool H264Encoder::processOneOutput() {
    if (!mft_) return false;

    MFT_OUTPUT_DATA_BUFFER out{};
    out.dwStreamID = outputStreamId_;

    ComPtr<IMFSample> owned;
    if (!mftProvidesSamples_) {
        MFT_OUTPUT_STREAM_INFO info{};
        if (FAILED(mft_->GetOutputStreamInfo(outputStreamId_, &info))) return false;

        if (FAILED(MFCreateSample(owned.put()))) return false;
        ComPtr<IMFMediaBuffer> buf;
        const DWORD size = std::max<DWORD>(info.cbSize, 1u << 20);
        if (FAILED(MFCreateMemoryBuffer(size, buf.put()))) return false;
        owned->AddBuffer(buf.get());
        out.pSample = owned.get();
    }

    DWORD         status = 0;
    const HRESULT hr = mft_->ProcessOutput(0, 1, &out, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        if (out.pEvents) out.pEvents->Release();
        return false;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        // The encoder renegotiated its output format (some hardware MFTs do this
        // once, right after the first frame). Re-accept it and re-read SPS/PPS.
        if (out.pEvents) out.pEvents->Release();
        ComPtr<IMFMediaType> newType;
        if (SUCCEEDED(mft_->GetOutputAvailableType(outputStreamId_, 0, newType.put())))
            mft_->SetOutputType(outputStreamId_, newType.get(), 0);
        cacheSequenceHeader();
        return true;
    }

    if (FAILED(hr)) {
        if (out.pEvents) out.pEvents->Release();
        logT("ProcessOutput failed: {}", hrString(hr));
        return false;
    }

    if (out.pSample) {
        emit(out.pSample);
        framesOut_.fetch_add(1);
    }

    if (out.pEvents) out.pEvents->Release();
    // When the MFT allocates, we own the returned reference.
    if (mftProvidesSamples_ && out.pSample) out.pSample->Release();
    return true;
}

void H264Encoder::cacheSequenceHeader() {
    if (!mft_) return;

    ComPtr<IMFMediaType> type;
    if (FAILED(mft_->GetOutputCurrentType(outputStreamId_, type.put())) || !type) return;

    UINT32 size = 0;
    if (FAILED(type->GetBlobSize(MF_MT_MPEG_SEQUENCE_HEADER, &size)) || size == 0) return;

    sequenceHeader_.resize(size);
    if (FAILED(type->GetBlob(MF_MT_MPEG_SEQUENCE_HEADER, sequenceHeader_.data(), size,
                             nullptr))) {
        sequenceHeader_.clear();
        return;
    }
    logT("cached SPS/PPS sequence header, {} bytes", size);
}

void H264Encoder::emit(IMFSample* sample) {
    if (!onOutput_) return;

    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || !buffer) return;

    BYTE* data = nullptr;
    DWORD maxLen = 0, curLen = 0;
    if (FAILED(buffer->Lock(&data, &maxLen, &curLen)) || curLen == 0) {
        if (data) buffer->Unlock();
        return;
    }

    LONGLONG t100ns = 0;
    sample->GetSampleTime(&t100ns);
    const bool keyframe =
        MFGetAttributeUINT32(sample, MFSampleExtension_CleanPoint, 0) != 0;

    // A viewer that joins mid-stream, or one recovering from a PLI, cannot decode
    // an IDR without the parameter sets. Most MFTs inline them; the ones that do
    // not are why this fallback exists.
    const bool hasParamSets = firstNalType(data, curLen) == 7;   // 7 = SPS
    if (keyframe && !hasParamSets && !sequenceHeader_.empty()) {
        scratch_.clear();
        scratch_.reserve(sequenceHeader_.size() + curLen);
        scratch_.insert(scratch_.end(), sequenceHeader_.begin(), sequenceHeader_.end());
        scratch_.insert(scratch_.end(), data, data + curLen);
        onOutput_(scratch_.data(), scratch_.size(), true, t100ns * 100);
    } else {
        onOutput_(data, curLen, keyframe, t100ns * 100);
    }

    buffer->Unlock();
}

} // namespace soi
