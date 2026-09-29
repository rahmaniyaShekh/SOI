#pragma once
//
// DXGI Desktop Duplication.
//
// This is the backend that answers "why is half my screen black". GDI reads the
// desktop through the compositor's CPU-visible surface, which is not where the
// picture actually comes from any more: an exclusive-fullscreen game owns the
// swapchain outright, and a video player handed a hardware overlay plane (MPO)
// has its pixels merged by the display controller during scanout. Neither is in
// anything GDI can see, so GDI returns black or a stale frame.
//
// Desktop Duplication asks DWM for the composed output of a whole display --
// the same image the monitor is fed -- so all of that arrives. It is also about
// ten times cheaper: the copy is GPU-side and only the final readback crosses
// the bus.
//
// What it cannot do:
//   * single windows -- duplication is per-output. Use WgcCapture for those.
//   * rotated displays without an untested pixel transform, so it declines them
//     and lets WGC (which composes in desktop orientation) take over.
//   * anything protected with SetWindowDisplayAffinity, or the secure desktop.
//     Nothing in user mode can. See README 1.2.
//
// Multiple outputs are supported for --desktop: each is duplicated separately
// and blitted into its own corner of one virtual-desktop-sized buffer.
//
#include "capture/FrameBuffer.h"
#include "capture/FrameSource.h"
#include "gpu/GpuPipeline.h"
#include "util/Win.h"

#include <chrono>
#include <memory>
#include <vector>

struct ID3D11Texture2D;

namespace soi {

class DxgiCapture final : public FrameSource {
public:
    explicit DxgiCapture(CaptureConfig cfg);
    ~DxgiCapture() override;

    // Cheap probe: can this machine duplicate its first output at all? Used by
    // the factory to decide the backend order without building a whole capture.
    static bool available();

    bool start() override;
    void stop()  override;
    bool retarget(const CaptureConfig& cfg) override;

    const Frame* capture() override;

    int width()  const override { return outW_; }
    int height() const override { return outH_; }
    int nativeWidth()  const override { return srcW_; }
    int nativeHeight() const override { return srcH_; }

    std::string    describe() const override { return description_; }
    CaptureBackend backend()  const override { return CaptureBackend::Dxgi; }

    std::shared_ptr<GpuDevice> gpuDevice() const override { return gpu_; }

private:
    struct Output;   // one duplicated display; definition is in the .cpp

    using Clock = std::chrono::steady_clock;

    bool resolveTarget();                 // fills srcX_/srcY_/srcW_/srcH_
    bool collectOutputs();                // one Output per display we overlap
    bool openDuplication(Output& out);    // (re)creates the device + duplication
    bool pumpOutput(Output& out, int timeoutMs);   // one AcquireNextFrame cycle

    // Decides whether this target can run the GPU pipeline and, if so, builds
    // the shared device and the composed BGRA texture. Returns false for every
    // reason that is not an error -- a second adapter, --cursor, a driver that
    // will not make the texture -- and the caller carries on with the CPU path.
    bool setUpGpu();

    CaptureConfig cfg_;
    std::string   description_;

    // Target rectangle in virtual-desktop coordinates.
    int srcX_ = 0, srcY_ = 0, srcW_ = 0, srcH_ = 0;
    int outW_ = 0, outH_ = 0;

    std::vector<std::unique_ptr<Output>> outputs_;

    // Two surfaces, because only outputs that reported a change are re-copied.
    // Drawing the cursor straight into `desktop_` would leave a trail behind it
    // across every frame no output touched, so the cursor goes onto a scratch
    // copy instead. `composed_` is only allocated when --cursor is on.
    FrameBuffer desktop_;
    FrameBuffer composed_;
    Frame       frame_{};

    // The GPU pipeline. `gpu_` is non-null only while it is actually running, so
    // it doubles as the flag: everything downstream tests it rather than a
    // separate bool that could disagree with reality.
    //
    // `gpuDesktop_` is the virtual-desktop-sized BGRA texture each output's
    // duplication is copied into. Even with one output the copy is needed --
    // ReleaseFrame invalidates the acquired texture immediately, and it is a
    // GPU-to-GPU blit, not a bus crossing.
    std::shared_ptr<GpuDevice> gpu_;
    ComPtr<ID3D11Texture2D>    gpuDesktop_;

    // The cursor is not in the duplicated image, so a cursor move is a content
    // change even when no output reported one.
    POINT lastCursor_{-1, -1};
    bool  lastCursorShown_ = false;

    bool started_ = false;
};

} // namespace soi
