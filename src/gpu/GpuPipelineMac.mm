#include "gpu/GpuPipeline.h"
#include "util/Log.h"

#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#import <VideoToolbox/VideoToolbox.h>

namespace soi {

const char* pipelineName(Pipeline p) {
    switch (p) {
        case Pipeline::Auto: return "auto";
        case Pipeline::Gpu:  return "gpu";
        case Pipeline::Cpu:  return "cpu";
    }
    return "auto";
}

bool parsePipelineName(std::string_view name, Pipeline& out) {
    if (name == "auto") { out = Pipeline::Auto; return true; }
    if (name == "gpu")  { out = Pipeline::Gpu;  return true; }
    if (name == "cpu")  { out = Pipeline::Cpu;  return true; }
    return false;
}

std::shared_ptr<GpuDevice> GpuDevice::create() {
    auto dev = std::make_shared<GpuDevice>();
    @autoreleasepool {
        id<MTLDevice> mtl = MTLCreateSystemDefaultDevice();
        dev->description_ = mtl ? std::string([[mtl name] UTF8String]) + " (IOSurface, zero-copy)"
                                : std::string("system GPU (IOSurface, zero-copy)");
    }
    return dev;
}

// ---------------------------------------------------------------------------

Nv12GpuConverter::~Nv12GpuConverter() { reset(); }

void Nv12GpuConverter::reset() {
    if (session_) {
        VTPixelTransferSessionInvalidate(static_cast<VTPixelTransferSessionRef>(session_));
        CFRelease(session_);
        session_ = nullptr;
    }
    if (pool_) {
        CVPixelBufferPoolRelease(static_cast<CVPixelBufferPoolRef>(pool_));
        pool_ = nullptr;
    }
    dstW_ = dstH_ = 0;
}

bool Nv12GpuConverter::init(const std::shared_ptr<GpuDevice>& /*dev*/, int dstW, int dstH) {
    reset();
    dstW &= ~1;
    dstH &= ~1;
    if (dstW <= 0 || dstH <= 0) return false;

    VTPixelTransferSessionRef session = nullptr;
    OSStatus st = VTPixelTransferSessionCreate(kCFAllocatorDefault, &session);
    if (st != noErr || !session) {
        logW("gpu: VTPixelTransferSessionCreate failed: {}", hrString(st));
        return false;
    }
    // Box-like downscale quality, and the BT.709 matrix the SDP and the CPU
    // converter both promise -- the decoder applies the 709 inverse.
    VTSessionSetProperty(session, kVTPixelTransferPropertyKey_ScalingMode,
                         kVTScalingMode_Normal);
    VTSessionSetProperty(session, kVTPixelTransferPropertyKey_DestinationYCbCrMatrix,
                         kCVImageBufferYCbCrMatrix_ITU_R_709_2);
    VTSessionSetProperty(session, kVTPixelTransferPropertyKey_DestinationColorPrimaries,
                         kCVImageBufferColorPrimaries_ITU_R_709_2);
    VTSessionSetProperty(session, kVTPixelTransferPropertyKey_DestinationTransferFunction,
                         kCVImageBufferTransferFunction_ITU_R_709_2);

    NSDictionary* attrs = @{
        (id)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange),
        (id)kCVPixelBufferWidthKey           : @(dstW),
        (id)kCVPixelBufferHeightKey          : @(dstH),
        (id)kCVPixelBufferIOSurfacePropertiesKey : @{},
    };
    CVPixelBufferPoolRef pool = nullptr;
    const CVReturn cr = CVPixelBufferPoolCreate(kCFAllocatorDefault, nullptr,
                                                (__bridge CFDictionaryRef)attrs, &pool);
    if (cr != kCVReturnSuccess || !pool) {
        logW("gpu: could not create the NV12 buffer pool ({})", static_cast<int>(cr));
        VTPixelTransferSessionInvalidate(session);
        CFRelease(session);
        return false;
    }
    session_ = session;
    pool_    = pool;
    dstW_    = dstW;
    dstH_    = dstH;
    return true;
}

void* Nv12GpuConverter::convert(void* src) {
    if (!session_ || !pool_ || !src) return nullptr;
    CVPixelBufferRef out = nullptr;
    if (CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault,
                                           static_cast<CVPixelBufferPoolRef>(pool_), &out) !=
            kCVReturnSuccess || !out)
        return nullptr;
    const OSStatus st = VTPixelTransferSessionTransferImage(
        static_cast<VTPixelTransferSessionRef>(session_), static_cast<CVPixelBufferRef>(src), out);
    if (st != noErr) {
        logT("gpu: pixel transfer failed: {}", hrString(st));
        CVPixelBufferRelease(out);
        return nullptr;
    }
    // Tag the buffer so the encoder writes matching VUI into the SPS.
    CVBufferSetAttachment(out, kCVImageBufferYCbCrMatrixKey, kCVImageBufferYCbCrMatrix_ITU_R_709_2,
                          kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(out, kCVImageBufferColorPrimariesKey,
                          kCVImageBufferColorPrimaries_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    CVBufferSetAttachment(out, kCVImageBufferTransferFunctionKey,
                          kCVImageBufferTransferFunction_ITU_R_709_2, kCVAttachmentMode_ShouldPropagate);
    return out;
}

} // namespace soi
