#pragma once
//
// The three macOS capture backends, and what they share. See
// capture/FrameSource.h for where each sits in the fallback order.
//
// The classes are plain C++ with their Objective-C state behind a pimpl, so
// CaptureFactory.cpp (and the self-test) can create them without being
// Objective-C++ themselves.
//
#include "capture/FrameSource.h"

#include <cstdint>
#include <memory>
#include <vector>

namespace soi {

// ---------------------------------------------------------------------------
// Shared helpers (MacCaptureCommon.mm)
// ---------------------------------------------------------------------------

struct DisplayGeometry {
    uint32_t id = 0;                 // CGDirectDisplayID
    double   x = 0, y = 0;           // origin in global POINTS
    double   pointW = 0, pointH = 0; // size in points
    int      pixelW = 0, pixelH = 0; // native size in pixels (Retina: 2x the points)
    double   scale = 1.0;            // pixels per point
    bool     main = false;
};

// Active displays in the order --monitor numbers them: the main display (the
// one with the menu bar) is 0, the rest left to right, then top to bottom.
std::vector<DisplayGeometry> orderedDisplays();
bool displayForIndex(int index, DisplayGeometry& out);

// The window's bounds in points and the backing scale of the display it is
// mostly on. False if no such on-screen window exists.
bool windowGeometry(uint32_t windowId, double& x, double& y, double& w, double& h,
                    double& scale);

// Connects this process to the window server. Command-line tools are not
// connected until they first touch CoreGraphics; capture APIs that assume an
// app do not always do that themselves.
void ensureWindowServer();

// The shared "frames live on the GPU" marker for the SCK backend.
std::shared_ptr<GpuDevice> sharedGpuDevice();

uint64_t sampledFrameHashMac(const uint8_t* pixels, int stride, int width, int height);

// ---------------------------------------------------------------------------
// Backends
// ---------------------------------------------------------------------------

// ScreenCaptureKit, macOS 12.3+. Displays and windows; GPU-resident frames.
class SckCapture final : public FrameSource {
public:
    explicit SckCapture(const CaptureConfig& cfg);
    ~SckCapture() override;

    bool start() override;
    void stop() override;
    bool retarget(const CaptureConfig& cfg) override;
    const Frame* capture() override;
    int width()  const override;
    int height() const override;
    int nativeWidth()  const override;
    int nativeHeight() const override;
    std::string describe() const override;
    CaptureBackend backend() const override { return CaptureBackend::Sck; }
    std::shared_ptr<GpuDevice> gpuDevice() const override;

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// CGDisplayStream. Displays only; compositor-driven; macOS 10.8 and later.
class StreamCapture final : public FrameSource {
public:
    explicit StreamCapture(const CaptureConfig& cfg);
    ~StreamCapture() override;

    bool start() override;
    void stop() override;
    bool retarget(const CaptureConfig& cfg) override;
    const Frame* capture() override;
    int width()  const override;
    int height() const override;
    int nativeWidth()  const override;
    int nativeHeight() const override;
    std::string describe() const override;
    CaptureBackend backend() const override { return CaptureBackend::Stream; }

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

// CGDisplayCreateImage / CGWindowListCreateImage. Every target, including all
// displays at once; polled, so slow, but dependable.
class CgImageCapture final : public FrameSource {
public:
    explicit CgImageCapture(const CaptureConfig& cfg);
    ~CgImageCapture() override;

    bool start() override;
    void stop() override;
    bool retarget(const CaptureConfig& cfg) override;
    const Frame* capture() override;
    int width()  const override;
    int height() const override;
    int nativeWidth()  const override;
    int nativeHeight() const override;
    std::string describe() const override;
    CaptureBackend backend() const override { return CaptureBackend::CgImage; }

    struct Impl;
private:
    std::unique_ptr<Impl> impl_;
};

} // namespace soi
