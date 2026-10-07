// CGImage capture: ask the window server for a snapshot each frame. The
// slowest backend, and the only one that can take every display at once (one
// composited image of the whole desktop) -- so it is the floor of the fallback
// order and the backend behind --desktop.
#include "capture/mac/MacCapture.h"
#include "util/Log.h"

#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <chrono>
#include <vector>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace soi {

struct CgImageCapture::Impl {
    CaptureConfig cfg;
    int           w = 0, h = 0;       // the size frames are produced at
    std::string   description;
    bool          running = false;

    CGDirectDisplayID display = 0;
    CGWindowID        window  = 0;
    CGRect            desktop{};      // points, for --desktop

    std::vector<uint8_t> pixels;
    CGContextRef         context = nullptr;
    CGColorSpaceRef      space   = nullptr;
    Frame                frame{};
    uint64_t             lastHash = 0;
    bool                 haveFrame = false;

    ~Impl() {
        if (context) CGContextRelease(context);
        if (space) CGColorSpaceRelease(space);
    }

    // Draws `image` into our BGRA buffer at w x h; GPU-assisted scaling via
    // Quartz, top-down rows.
    bool draw(CGImageRef image) {
        if (!image) return false;
        if (!context) {
            pixels.assign(static_cast<size_t>(w) * h * 4, 0);
            if (!space) space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            context = CGBitmapContextCreate(pixels.data(), static_cast<size_t>(w),
                                            static_cast<size_t>(h), 8, static_cast<size_t>(w) * 4,
                                            space,
                                            kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
            if (!context) return false;
            CGContextSetInterpolationQuality(context, kCGInterpolationMedium);
        }
        CGContextSetRGBFillColor(context, 0, 0, 0, 1);
        CGContextFillRect(context, CGRectMake(0, 0, w, h));
        CGContextDrawImage(context, CGRectMake(0, 0, w, h), image);
        return true;
    }

    CGImageRef grab() const {
        switch (cfg.target) {
            case CaptureTarget::Monitor:
                return CGDisplayCreateImage(display);
            case CaptureTarget::Window:
                return CGWindowListCreateImage(CGRectNull, kCGWindowListOptionIncludingWindow, window,
                                               kCGWindowImageBoundsIgnoreFraming |
                                                   kCGWindowImageBestResolution);
            case CaptureTarget::VirtualDesktop:
                return CGWindowListCreateImage(desktop, kCGWindowListOptionOnScreenOnly,
                                               kCGNullWindowID, kCGWindowImageBestResolution);
        }
        return nullptr;
    }
};

CgImageCapture::CgImageCapture(const CaptureConfig& cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = cfg;
}

CgImageCapture::~CgImageCapture() { stop(); }

bool CgImageCapture::start() {
    Impl& im = *impl_;
    if (im.running) return true;
    if (!screenCapturePermitted(false)) {
        // Without the permission these calls succeed but return only the
        // wallpaper -- worse than failing, so fail.
        logT("cgimage: no Screen Recording permission");
        return false;
    }

    int srcW = 0, srcH = 0;
    switch (im.cfg.target) {
        case CaptureTarget::Monitor: {
            DisplayGeometry g;
            if (!displayForIndex(im.cfg.monitorIndex, g)) {
                logT("cgimage: there is no display {}", im.cfg.monitorIndex);
                return false;
            }
            im.display = g.id;
            srcW = g.pixelW;
            srcH = g.pixelH;
            im.description = soi::format("display {} ({}x{}) via CGDisplayCreateImage",
                                         im.cfg.monitorIndex, srcW, srcH);
            break;
        }
        case CaptureTarget::Window: {
            im.window = static_cast<CGWindowID>(reinterpret_cast<uintptr_t>(im.cfg.windowHandle));
            double x, y, ww, hh, scale;
            if (!windowGeometry(im.window, x, y, ww, hh, scale)) {
                logT("cgimage: window {} is not on screen", im.window);
                return false;
            }
            srcW = static_cast<int>(ww * scale);
            srcH = static_cast<int>(hh * scale);
            im.description = soi::format("window {} via CGWindowListCreateImage", im.window);
            break;
        }
        case CaptureTarget::VirtualDesktop: {
            const auto displays = orderedDisplays();
            if (displays.empty()) return false;
            double left = 1e9, top = 1e9, right = -1e9, bottom = -1e9, scale = 1.0;
            for (const auto& d : displays) {
                left   = std::min(left, d.x);
                top    = std::min(top, d.y);
                right  = std::max(right, d.x + d.pointW);
                bottom = std::max(bottom, d.y + d.pointH);
                scale  = std::max(scale, d.scale);
            }
            im.desktop = CGRectMake(left, top, right - left, bottom - top);
            srcW = static_cast<int>((right - left) * scale);
            srcH = static_cast<int>((bottom - top) * scale);
            im.description = soi::format("every display ({}x{}) via CGWindowListCreateImage",
                                         srcW, srcH);
            break;
        }
    }
    if (srcW < 2 || srcH < 2) return false;
    computeEncodeSize(srcW, srcH, im.cfg.maxWidth, im.w, im.h);

    // Prove it works before claiming to have started.
    CGImageRef first = im.grab();
    const bool ok = first && im.draw(first);
    if (first) CGImageRelease(first);
    if (!ok) {
        logT("cgimage: the first grab failed");
        return false;
    }
    im.running = true;
    im.haveFrame = false;
    return true;
}

void CgImageCapture::stop() {
    Impl& im = *impl_;
    im.running = false;
    if (im.context) { CGContextRelease(im.context); im.context = nullptr; }
    im.haveFrame = false;
}

bool CgImageCapture::retarget(const CaptureConfig& cfg) {
    const CaptureConfig previous = impl_->cfg;
    stop();
    impl_->cfg = cfg;
    if (start()) return true;
    impl_->cfg = previous;
    start();
    return false;
}

const Frame* CgImageCapture::capture() {
    Impl& im = *impl_;
    if (!im.running) return nullptr;
    CGImageRef image = im.grab();
    if (!image) return nullptr;   // a closed window, a display that went away
    const bool ok = im.draw(image);
    CGImageRelease(image);
    if (!ok) return nullptr;

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    im.frame.timeNs = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    im.frame.data   = im.pixels.data();
    im.frame.width  = im.w;
    im.frame.height = im.h;
    im.frame.stride = im.w * 4;
    if (im.cfg.detectDuplicates) {
        const uint64_t hash = sampledFrameHashMac(im.frame.data, im.frame.stride, im.w, im.h);
        im.frame.duplicate = im.haveFrame && hash == im.lastHash;
        im.lastHash = hash;
    } else {
        im.frame.duplicate = false;
    }
    im.haveFrame = true;
    return &im.frame;
}

int CgImageCapture::width()  const { return impl_->w; }
int CgImageCapture::height() const { return impl_->h; }
int CgImageCapture::nativeWidth()  const { return impl_->w; }
int CgImageCapture::nativeHeight() const { return impl_->h; }
std::string CgImageCapture::describe() const { return impl_->description; }

} // namespace soi
