#pragma once
//
// The macOS GPU pipeline. Included by gpu/GpuPipeline.h; see it for the idea.
//
//   ScreenCaptureKit  -> a BGRA CVPixelBuffer backed by an IOSurface, i.e. GPU
//                        memory the compositor wrote into.
//   Nv12GpuConverter  -> VTPixelTransferSession: scale to the encode size and
//                        convert BGRA -> NV12 (BT.709, video range) on the GPU,
//                        into a pooled IOSurface-backed CVPixelBuffer.
//   H264Encoder       -> VideoToolbox, which takes that buffer as it is.
//
// Nothing is read back to the CPU at any point. Core Video types are passed as
// void* so this header stays plain C++ for every file that includes it.
//
#include <memory>
#include <string>

namespace soi {

// There is no device object to share on macOS -- IOSurfaces are usable by any
// GPU client in the system -- so this only marks "frames are GPU-resident" and
// describes the GPU, matching the Windows interface.
class GpuDevice {
public:
    static std::shared_ptr<GpuDevice> create();
    const std::string& describe() const { return description_; }

private:
    std::string description_;
};

class Nv12GpuConverter {
public:
    Nv12GpuConverter() = default;
    ~Nv12GpuConverter();
    Nv12GpuConverter(const Nv12GpuConverter&) = delete;
    Nv12GpuConverter& operator=(const Nv12GpuConverter&) = delete;

    // dstW/dstH must both be even. False if a pixel-transfer session or the
    // output pool cannot be created.
    bool init(const std::shared_ptr<GpuDevice>& dev, int dstW, int dstH);
    void reset();

    bool ready() const { return session_ != nullptr; }
    int  width()  const { return dstW_; }
    int  height() const { return dstH_; }

    // Converts a BGRA CVPixelBufferRef. Returns a NEW NV12 CVPixelBufferRef
    // (+1 retained; the caller releases it), or null on failure. Buffers come
    // from a pool, so one still being encoded is never overwritten.
    void* convert(void* srcPixelBuffer);

private:
    void* session_ = nullptr;   // VTPixelTransferSessionRef
    void* pool_    = nullptr;   // CVPixelBufferPoolRef
    int   dstW_ = 0, dstH_ = 0;
};

} // namespace soi
