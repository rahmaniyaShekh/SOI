#pragma once
#include "capture/FrameBuffer.h"
#include "capture/FrameSource.h"
#include "util/Win.h"

#include <cstdint>

namespace soi {

// GDI BitBlt / PrintWindow capture.
//
// The fallback of last resort, and the only backend with no dependency beyond
// user32/gdi32 -- it works on Windows 7, inside a session with no D3D device,
// and against an occluded window, which is more than either of the other two
// can say.
//
// Its costs are real though (see README 1.2): a synchronous GPU->CPU readback
// pinned to the display refresh rate, and black pixels for anything
// DirectComposition-backed -- Chrome, Electron, exclusive-fullscreen games.
// PrintWindow with PW_RENDERFULLCONTENT is tried first for window targets and
// fixes most of that, but not all of it. That is precisely why DxgiCapture and
// WgcCapture exist and are preferred.
class BitBltCapture final : public FrameSource {
public:
    explicit BitBltCapture(CaptureConfig cfg);
    ~BitBltCapture() override;

    bool start() override;
    void stop()  override;
    bool retarget(const CaptureConfig& cfg) override;

    // Returns a frame at the target's NATIVE resolution. Downscaling is not done
    // here: GDI's only quality scaler (HALFTONE StretchBlt) costs ~16 ms/frame,
    // as much as the capture itself, whereas the NV12 conversion pass can box
    // filter for free because it already reads every pixel. See ColorConvert.h.
    const Frame* capture() override;

    // The size the encoder should be configured for, after --max-width. Callers
    // pass this to bgraToNv12 as the destination size.
    int width()  const override { return outW_; }
    int height() const override { return outH_; }

    // The size frames actually come back at.
    int nativeWidth()  const override { return srcW_; }
    int nativeHeight() const override { return srcH_; }

    std::string    describe() const override { return description_; }
    CaptureBackend backend()  const override { return CaptureBackend::BitBlt; }

private:
    bool resolveTarget();        // fills srcX_/srcY_/srcW_/srcH_
    bool blitDesktopOrMonitor(HDC dst);
    bool blitWindow(HDC dst);
    bool isDuplicate();

    CaptureConfig cfg_;
    std::string   description_;

    // Source rectangle in virtual-desktop coordinates (or window-local for windows).
    int srcX_ = 0, srcY_ = 0, srcW_ = 0, srcH_ = 0;
    int outW_ = 0, outH_ = 0;      // post-downscale target, for the encoder

    WindowDc    screenDc_;       // desktop DC, for screen targets
    FrameBuffer buffer_;         // full-resolution BGRA the blit lands in

    Frame    frame_{};
    uint64_t lastHash_ = 0;
    bool     haveLastHash_ = false;
    bool     started_ = false;
};

} // namespace soi
