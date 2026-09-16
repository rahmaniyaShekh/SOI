#pragma once
//
// Windows.Graphics.Capture (Win10 1803+).
//
// The modern per-item capture path: you hand it a window or a monitor and DWM
// hands back composed GPU textures. It is what fills the two holes the other
// backends leave.
//
//   * A window that renders through DirectComposition -- Chrome, Edge, any
//     Electron app, most games -- is black under GDI PrintWindow. WGC captures
//     it correctly because it asks the compositor, not the window.
//   * A monitor inside an RDP session, a VM with a basic display driver, or a
//     hybrid-GPU laptop where the output hangs off the wrong adapter, is not
//     duplicable at all. WGC still works there.
//
// It is also the only backend that draws the cursor itself, so no GDI
// compositing pass is needed when --cursor is on.
//
// Written against the raw WinRT ABI rather than C++/WinRT: the SDK's cppwinrt
// headers do not compile under /std:c++20 with this toolchain, and the project
// already speaks plain COM everywhere else.
//
// It does not bypass SetWindowDisplayAffinity either -- see README 1.2.
//
#include "capture/FrameBuffer.h"
#include "capture/FrameSource.h"
#include "util/Win.h"

#include <memory>

namespace soi {

class WgcCapture final : public FrameSource {
public:
    explicit WgcCapture(CaptureConfig cfg);
    ~WgcCapture() override;

    // True when the API is present AND the machine reports it usable. Some
    // server SKUs and older builds have the types but not the capability.
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
    CaptureBackend backend()  const override { return CaptureBackend::Wgc; }

private:
    struct Impl;   // all the WinRT ABI lives behind this, out of every other TU

    bool resolveTarget();
    bool rebuildPool(int w, int h);

    CaptureConfig cfg_;
    std::string   description_;

    std::unique_ptr<Impl> impl_;
    FrameBuffer           buffer_;
    Frame                 frame_{};

    int srcX_ = 0, srcY_ = 0, srcW_ = 0, srcH_ = 0;
    int outW_ = 0, outH_ = 0;

    // A resized window is acted on at the START of the next capture, never at
    // the end of the current one: re-pooling reallocates the buffer the frame we
    // are about to return points into.
    int pendingW_ = 0, pendingH_ = 0;

    bool started_ = false;
    bool primed_  = false;
};

} // namespace soi
