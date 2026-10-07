#include "encode/H264Encoder.h"
#include "util/Log.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <VideoToolbox/VideoToolbox.h>

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

// Same headroom as the Windows encoder (see kPeakPercent there): the burst a
// complex frame may spend above the average, which the caller reserves out of
// the link estimate when it picks the frame rate.
constexpr int kPeakPercent = 125;
int peakFor(int meanKbps) { return std::max(1, meanKbps) * kPeakPercent / 100; }

void outputCallback(void* refcon, void* /*frameRefcon*/, OSStatus status,
                    VTEncodeInfoFlags /*flags*/, CMSampleBufferRef sample) {
    static_cast<H264Encoder*>(refcon)->onEncoded(static_cast<int>(status), sample);
}

void setInt(VTCompressionSessionRef s, CFStringRef key, int value, const char* what) {
    CFNumberRef n = CFNumberCreate(nullptr, kCFNumberIntType, &value);
    const OSStatus st = VTSessionSetProperty(s, key, n);
    CFRelease(n);
    if (st != noErr) logT("encoder property '{}' rejected: {}", what, hrString(st));
}

bool setBool(VTCompressionSessionRef s, CFStringRef key, bool value, const char* what) {
    const OSStatus st = VTSessionSetProperty(s, key, value ? kCFBooleanTrue : kCFBooleanFalse);
    if (st != noErr) logT("encoder property '{}' rejected: {}", what, hrString(st));
    return st == noErr;
}

const uint8_t kStartCode[4] = {0, 0, 0, 1};

} // namespace

H264Encoder::H264Encoder()  = default;
H264Encoder::~H264Encoder() { stop(); }

bool H264Encoder::enableGpuInput(const std::shared_ptr<GpuDevice>& device) {
    if (!device) return false;
    // A converter at a token size proves the pixel-transfer path exists here.
    Nv12GpuConverter probe;
    if (!probe.init(device, 64, 64)) return false;
    gpuDevice_    = device;
    gpuRequested_ = true;
    if (description_.empty())
        description_ = "VideoToolbox H.264 (takes IOSurface frames directly)";
    return true;
}

bool H264Encoder::createSession(bool requireHardware) {
    @autoreleasepool {
        NSMutableDictionary* spec = [NSMutableDictionary dictionary];
        if (cfg_.preferHardware) {
            spec[(id)kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder] = @YES;
            if (requireHardware)
                spec[(id)kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = @YES;
        }

        // What submit() and the GPU converter hand over: NV12, video range,
        // IOSurface-backed so a hardware encoder reads it without a copy.
        NSDictionary* source = @{
            (id)kCVPixelBufferPixelFormatTypeKey :
                @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
            (id)kCVPixelBufferWidthKey  : @(cfg_.width),
            (id)kCVPixelBufferHeightKey : @(cfg_.height),
            (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
        };

        VTCompressionSessionRef session = nullptr;
        const OSStatus st = VTCompressionSessionCreate(
            kCFAllocatorDefault, cfg_.width, cfg_.height, kCMVideoCodecType_H264,
            (__bridge CFDictionaryRef)spec, (__bridge CFDictionaryRef)source, kCFAllocatorDefault,
            outputCallback, this, &session);
        if (st != noErr || !session) {
            logT("VTCompressionSessionCreate ({}hardware) failed: {}",
                 requireHardware ? "" : "any ", hrString(st));
            return false;
        }
        session_ = session;

        CFBooleanRef usingHw = nullptr;
        if (VTSessionCopyProperty(session, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                                  kCFAllocatorDefault, &usingHw) == noErr && usingHw) {
            hardware_ = CFBooleanGetValue(usingHw);
            CFRelease(usingHw);
        } else {
            hardware_ = requireHardware;
        }
        description_ = hardware_ ? "VideoToolbox H.264 (hardware)"
                                 : "VideoToolbox H.264 (Apple software encoder)";
    }
    return true;
}

void H264Encoder::applyRateControl(int kbps) {
    auto s = static_cast<VTCompressionSessionRef>(session_);
    if (!s) return;
    kbps = std::max(50, kbps);
    setInt(s, kVTCompressionPropertyKey_AverageBitRate, kbps * 1000, "AverageBitRate");

    // DataRateLimits caps the bytes in any one-second window: peak-constrained
    // VBR, the same allocation policy as the Windows encoder.
    qualityRateControl_ = false;
    if (cfg_.preferQualityRateControl) {
        @autoreleasepool {
            const double bytesPerSecond = peakFor(kbps) * 1000.0 / 8.0;
            NSArray* limits = @[ @(bytesPerSecond), @(1.0) ];
            const OSStatus st = VTSessionSetProperty(s, kVTCompressionPropertyKey_DataRateLimits,
                                                     (__bridge CFArrayRef)limits);
            qualityRateControl_ = st == noErr;
            if (st != noErr) logT("DataRateLimits rejected ({}); average bitrate only", hrString(st));
        }
    }
    bitrateKbps_.store(kbps);
}

bool H264Encoder::start(const EncoderConfig& cfg, OutputCallback onOutput) {
    if (running_.load()) return true;
    cfg_        = cfg;
    onOutput_   = std::move(onOutput);
    cfg_.width  &= ~1;
    cfg_.height &= ~1;

    {
        std::lock_guard lk(sessionMtx_);
        if (!(cfg_.preferHardware && createSession(true)) && !createSession(false)) {
            logE("no H.264 encoder is available from VideoToolbox");
            return false;
        }
        auto s = static_cast<VTCompressionSessionRef>(session_);

        setBool(s, kVTCompressionPropertyKey_RealTime, true, "RealTime");
        // Main, as the SDP's profile-level-id says; the level is derived from
        // the size and rate, which is what h264LevelForResolution assumes.
        VTSessionSetProperty(s, kVTCompressionPropertyKey_ProfileLevel,
                             kVTProfileLevel_H264_Main_AutoLevel);
        // No B-frames: they need reordering, which a real-time stream cannot
        // afford and WebRTC does not do.
        setBool(s, kVTCompressionPropertyKey_AllowFrameReordering, false, "AllowFrameReordering");
        setInt(s, kVTCompressionPropertyKey_MaxKeyFrameInterval,
               std::max(1, cfg_.fps * cfg_.gopSeconds), "MaxKeyFrameInterval");
        setInt(s, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration,
               std::max(1, cfg_.gopSeconds), "MaxKeyFrameIntervalDuration");
        setInt(s, kVTCompressionPropertyKey_ExpectedFrameRate, cfg_.fps, "ExpectedFrameRate");
        if (@available(macOS 10.14, *))
            setInt(s, kVTCompressionPropertyKey_MaxFrameDelayCount, 0, "MaxFrameDelayCount");
        // The same colour description the Windows encoder writes into the VUI.
        VTSessionSetProperty(s, kVTCompressionPropertyKey_ColorPrimaries,
                             kCVImageBufferColorPrimaries_ITU_R_709_2);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_TransferFunction,
                             kCVImageBufferTransferFunction_ITU_R_709_2);
        VTSessionSetProperty(s, kVTCompressionPropertyKey_YCbCrMatrix,
                             kCVImageBufferYCbCrMatrix_ITU_R_709_2);
        {
            float q = std::clamp(cfg_.quality, 1, 100) / 100.0f;
            CFNumberRef n = CFNumberCreate(nullptr, kCFNumberFloatType, &q);
            VTSessionSetProperty(s, kVTCompressionPropertyKey_Quality, n);   // advisory
            CFRelease(n);
        }
        applyRateControl(cfg_.bitrateKbps);

        const OSStatus st = VTCompressionSessionPrepareToEncodeFrames(s);
        if (st != noErr) logT("PrepareToEncodeFrames: {}", hrString(st));
    }

    gpuInput_ = false;
    if (gpuRequested_) {
        if (gpuConverter_.init(gpuDevice_, cfg_.width, cfg_.height)) {
            gpuInput_ = true;
        } else {
            logW("gpu: NV12 conversion is unavailable at {}x{}; using the CPU converter",
                 cfg_.width, cfg_.height);
        }
    }

    inFlight_.store(0);
    framesIn_ = framesOut_ = framesDropped_ = 0;
    running_.store(true);
    logI("encoder: {} {}x{} @{}fps target {} kbps, GOP {}s, input {}", description_, cfg_.width,
         cfg_.height, cfg_.fps, cfg_.bitrateKbps, cfg_.gopSeconds,
         gpuInput_ ? "IOSurface (zero-copy)" : "system memory");
    return true;
}

void H264Encoder::stop() {
    const bool wasRunning = running_.exchange(false);
    std::lock_guard lk(sessionMtx_);
    if (session_) {
        auto s = static_cast<VTCompressionSessionRef>(session_);
        // Flush synchronously so no callback can arrive after this returns.
        VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
        VTCompressionSessionInvalidate(s);
        CFRelease(s);
        session_ = nullptr;
    }
    gpuConverter_.reset();
    gpuInput_ = false;
    if (wasRunning)
        logT("encoder stopped: {} in, {} out, {} dropped", framesIn_.load(), framesOut_.load(),
             framesDropped_.load());
}

bool H264Encoder::encodePixelBuffer(void* pixelBuffer, int64_t ptsNs) {
    std::lock_guard lk(sessionMtx_);
    auto s = static_cast<VTCompressionSessionRef>(session_);
    if (!s || !running_.load()) return false;

    NSDictionary* props = nil;
    if (keyframeRequested_.exchange(false))
        props = @{(id)kVTEncodeFrameOptionKey_ForceKeyFrame : @YES};

    inFlight_.fetch_add(1);
    const OSStatus st = VTCompressionSessionEncodeFrame(
        s, static_cast<CVPixelBufferRef>(pixelBuffer), CMTimeMake(ptsNs, 1000000000),
        kCMTimeInvalid, (__bridge CFDictionaryRef)props, nullptr, nullptr);
    if (st != noErr) {
        inFlight_.fetch_sub(1);
        if (props) keyframeRequested_.store(true);   // try again on the next frame
        logT("VTCompressionSessionEncodeFrame failed: {}", hrString(st));
        return false;
    }
    ++framesIn_;
    return true;
}

bool H264Encoder::submit(const Nv12Buffer& frame, int64_t ptsNs) {
    if (!running_.load()) return false;
    // A stale screen frame has no value: drop rather than queue behind it.
    if (inFlight_.load() >= kMaxQueue) { ++framesDropped_; return false; }

    CVPixelBufferRef buffer = nullptr;
    {
        std::lock_guard lk(sessionMtx_);
        auto s = static_cast<VTCompressionSessionRef>(session_);
        if (!s) return false;
        CVPixelBufferPoolRef pool = VTCompressionSessionGetPixelBufferPool(s);
        if (!pool || CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &buffer) !=
                         kCVReturnSuccess)
            return false;
    }

    // The encoder's size can differ from the frame's for one frame around a
    // quality change; copy the overlap and never write out of bounds.
    CVPixelBufferLockBaseAddress(buffer, 0);
    const int w = std::min(frame.width(), static_cast<int>(CVPixelBufferGetWidth(buffer)));
    const int h = std::min(frame.height(), static_cast<int>(CVPixelBufferGetHeight(buffer)));
    auto* yDst  = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 0));
    auto* uvDst = static_cast<uint8_t*>(CVPixelBufferGetBaseAddressOfPlane(buffer, 1));
    const size_t yStride  = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0);
    const size_t uvStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1);
    const uint8_t* src = frame.data();
    const size_t   srcStride = static_cast<size_t>(frame.stride());
    const uint8_t* srcUv = src + srcStride * static_cast<size_t>(frame.height());
    for (int y = 0; y < h; ++y)
        std::memcpy(yDst + y * yStride, src + y * srcStride, static_cast<size_t>(w));
    for (int y = 0; y < h / 2; ++y)
        std::memcpy(uvDst + y * uvStride, srcUv + y * srcStride, static_cast<size_t>(w));
    CVPixelBufferUnlockBaseAddress(buffer, 0);

    const bool ok = encodePixelBuffer(buffer, ptsNs);
    CVPixelBufferRelease(buffer);
    return ok;
}

bool H264Encoder::submitTexture(void* bgra, int64_t ptsNs) {
    if (!running_.load() || !gpuInput_ || !bgra) return false;
    if (inFlight_.load() >= kMaxQueue) { ++framesDropped_; return false; }
    void* nv12 = gpuConverter_.convert(bgra);
    if (!nv12) return false;
    const bool ok = encodePixelBuffer(nv12, ptsNs);
    CVPixelBufferRelease(static_cast<CVPixelBufferRef>(nv12));
    return ok;
}

void H264Encoder::requestKeyframe() { keyframeRequested_.store(true); }

void H264Encoder::setBitrate(int kbps) {
    if (kbps <= 0 || kbps == bitrateKbps_.load()) return;
    std::lock_guard lk(sessionMtx_);
    applyRateControl(kbps);
    logT("encoder bitrate -> {} kbps{}", kbps,
         qualityRateControl_ ? soi::format(" (peak {})", peakFor(kbps)) : "");
}

// ---------------------------------------------------------------------------
// Output: AVCC (4-byte big-endian lengths) -> Annex-B, SPS/PPS before each IDR.
// ---------------------------------------------------------------------------
void H264Encoder::onEncoded(int status, void* sampleRef) {
    inFlight_.fetch_sub(1);
    auto sample = static_cast<CMSampleBufferRef>(sampleRef);
    if (status != noErr || !sample || !CMSampleBufferDataIsReady(sample)) {
        if (status != noErr) logT("encode callback error: {}", hrString(status));
        return;
    }

    bool keyframe = true;
    if (CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        attachments && CFArrayGetCount(attachments) > 0) {
        auto dict = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        keyframe = !CFDictionaryContainsKey(dict, kCMSampleAttachmentKey_NotSync);
    }

    scratch_.clear();
    if (keyframe) {
        CMFormatDescriptionRef fmt = CMSampleBufferGetFormatDescription(sample);
        size_t count = 0;
        int headerLen = 4;
        if (fmt && CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, 0, nullptr, nullptr,
                                                                      &count, &headerLen) == noErr) {
            for (size_t i = 0; i < count; ++i) {
                const uint8_t* ps = nullptr;
                size_t len = 0;
                if (CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fmt, i, &ps, &len, nullptr,
                                                                       nullptr) == noErr) {
                    scratch_.insert(scratch_.end(), kStartCode, kStartCode + 4);
                    scratch_.insert(scratch_.end(), ps, ps + len);
                }
            }
        }
    }

    CMBlockBufferRef block = CMSampleBufferGetDataBuffer(sample);
    size_t total = 0;
    char*  data = nullptr;
    if (!block || CMBlockBufferGetDataPointer(block, 0, nullptr, &total, &data) != noErr) return;
    // Rarely non-contiguous; copy out in that case.
    std::vector<uint8_t> contiguous;
    if (!CMBlockBufferIsRangeContiguous(block, 0, total)) {
        contiguous.resize(total);
        CMBlockBufferCopyDataBytes(block, 0, total, contiguous.data());
        data = reinterpret_cast<char*>(contiguous.data());
    }

    size_t off = 0;
    while (off + 4 <= total) {
        const auto* p = reinterpret_cast<const uint8_t*>(data + off);
        const uint32_t nal = (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
                             (uint32_t(p[2]) << 8) | uint32_t(p[3]);
        off += 4;
        if (nal == 0 || off + nal > total) break;
        scratch_.insert(scratch_.end(), kStartCode, kStartCode + 4);
        scratch_.insert(scratch_.end(), p + 4, p + 4 + nal);
        off += nal;
    }
    if (scratch_.empty()) return;

    const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sample);
    const int64_t ptsNs = CMTIME_IS_VALID(pts)
                              ? static_cast<int64_t>(CMTimeGetSeconds(pts) * 1e9)
                              : 0;
    ++framesOut_;
    if (onOutput_) onOutput_(scratch_.data(), scratch_.size(), keyframe, ptsNs);
}

} // namespace soi
