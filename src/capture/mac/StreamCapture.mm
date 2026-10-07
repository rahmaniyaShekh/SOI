// CGDisplayStream capture: the compositor pushes each new frame of one display
// as an IOSurface, scaled to the size we ask for. Available on every macOS this
// build supports, which makes it the backend for 10.15 - 12.2, where
// ScreenCaptureKit does not exist. Apple has deprecated it in favour of
// ScreenCaptureKit but it still works, which is all a fallback needs to do.
#include "capture/mac/MacCapture.h"
#include "util/Log.h"

#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace soi {

struct StreamCapture::Impl {
    CaptureConfig cfg;
    int           w = 0, h = 0;
    std::string   description;

    CGDisplayStreamRef stream = nullptr;
    dispatch_queue_t   queue  = nullptr;

    std::mutex              mtx;
    std::condition_variable cv;
    IOSurfaceRef            latest = nullptr;   // retained
    bool                    fresh  = false;
    std::atomic<bool>       stopped{false};

    Frame                frame{};
    std::vector<uint8_t> cpu;
    bool                 haveFrame = false;
};

StreamCapture::StreamCapture(const CaptureConfig& cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = cfg;
}

StreamCapture::~StreamCapture() { stop(); }

bool StreamCapture::start() {
    Impl& im = *impl_;
    if (im.stream) return true;
    if (im.cfg.target != CaptureTarget::Monitor) {
        logT("stream: CGDisplayStream reads one display, not a window or the whole desktop");
        return false;
    }
    if (!screenCapturePermitted(false)) {
        // Without the permission the stream still runs, but shows only the
        // wallpaper and menu bar -- worse than failing, so fail.
        logT("stream: no Screen Recording permission");
        return false;
    }
    DisplayGeometry g;
    if (!displayForIndex(im.cfg.monitorIndex, g)) {
        logT("stream: there is no display {}", im.cfg.monitorIndex);
        return false;
    }
    computeEncodeSize(g.pixelW, g.pixelH, im.cfg.maxWidth, im.w, im.h);
    im.description = soi::format("display {} ({}x{}) via CGDisplayStream", im.cfg.monitorIndex,
                                 g.pixelW, g.pixelH);

    im.queue = dispatch_queue_create("soi.stream.frames", DISPATCH_QUEUE_SERIAL);
    im.stopped.store(false);
    Impl* self = &im;

    @autoreleasepool {
        NSDictionary* props = @{
            (__bridge NSString*)kCGDisplayStreamShowCursor : im.cfg.captureCursor ? @YES : @NO,
            (__bridge NSString*)kCGDisplayStreamMinimumFrameTime : @(1.0 / 60.0),
            (__bridge NSString*)kCGDisplayStreamQueueDepth : @(4),
            (__bridge NSString*)kCGDisplayStreamColorSpace :
                (__bridge_transfer id)CGColorSpaceCreateWithName(kCGColorSpaceSRGB),
        };
        im.stream = CGDisplayStreamCreateWithDispatchQueue(
            g.id, static_cast<size_t>(im.w), static_cast<size_t>(im.h), 'BGRA',
            (__bridge CFDictionaryRef)props, im.queue,
            ^(CGDisplayStreamFrameStatus status, uint64_t, IOSurfaceRef surface,
              CGDisplayStreamUpdateRef) {
                if (status == kCGDisplayStreamFrameStatusStopped) {
                    self->stopped.store(true);
                    self->cv.notify_all();
                    return;
                }
                if (status != kCGDisplayStreamFrameStatusFrameComplete || !surface) return;
                CFRetain(surface);
                {
                    std::lock_guard lk(self->mtx);
                    if (self->latest) CFRelease(self->latest);
                    self->latest = surface;
                    self->fresh  = true;
                }
                self->cv.notify_all();
            });
    }
    if (!im.stream) {
        logT("stream: CGDisplayStreamCreate failed");
        return false;
    }
    if (CGDisplayStreamStart(im.stream) != kCGErrorSuccess) {
        logT("stream: CGDisplayStreamStart failed");
        CFRelease(im.stream);
        im.stream = nullptr;
        return false;
    }

    std::unique_lock lk(im.mtx);
    im.cv.wait_for(lk, std::chrono::seconds(2), [&] { return im.latest || im.stopped.load(); });
    const bool ok = im.latest != nullptr;
    lk.unlock();
    if (!ok) {
        logT("stream: started but delivered no frame");
        stop();
        return false;
    }
    im.haveFrame = false;
    return true;
}

void StreamCapture::stop() {
    Impl& im = *impl_;
    if (im.stream) {
        CGDisplayStreamStop(im.stream);
        // Let the queue drain the "stopped" callback before the block's
        // captured state can go away.
        if (im.queue) dispatch_sync(im.queue, ^{});
        CFRelease(im.stream);
        im.stream = nullptr;
    }
    im.queue = nullptr;
    std::lock_guard lk(im.mtx);
    if (im.latest) { CFRelease(im.latest); im.latest = nullptr; }
    im.fresh = false;
    im.haveFrame = false;
}

bool StreamCapture::retarget(const CaptureConfig& cfg) {
    const CaptureConfig previous = impl_->cfg;
    stop();
    impl_->cfg = cfg;
    if (start()) return true;
    impl_->cfg = previous;
    start();
    return false;
}

const Frame* StreamCapture::capture() {
    Impl& im = *impl_;
    if (!im.stream || im.stopped.load()) return nullptr;

    IOSurfaceRef surface = nullptr;
    bool fresh = false;
    {
        std::lock_guard lk(im.mtx);
        if (!im.latest) return nullptr;
        surface = im.latest;
        CFRetain(surface);
        fresh = im.fresh;
        im.fresh = false;
    }

    if (fresh || im.cpu.empty()) {
        IOSurfaceLock(surface, kIOSurfaceLockReadOnly, nullptr);
        const auto* base = static_cast<const uint8_t*>(IOSurfaceGetBaseAddress(surface));
        const size_t srcStride = IOSurfaceGetBytesPerRow(surface);
        im.frame.width  = static_cast<int>(IOSurfaceGetWidth(surface));
        im.frame.height = static_cast<int>(IOSurfaceGetHeight(surface));
        const size_t rowBytes = static_cast<size_t>(im.frame.width) * 4;
        im.cpu.resize(rowBytes * static_cast<size_t>(im.frame.height));
        for (int y = 0; y < im.frame.height; ++y)
            std::memcpy(im.cpu.data() + y * rowBytes, base + y * srcStride, rowBytes);
        IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, nullptr);
    }
    CFRelease(surface);

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    im.frame.timeNs    = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    im.frame.duplicate = im.haveFrame && !fresh;
    im.frame.data      = im.cpu.data();
    im.frame.stride    = im.frame.width * 4;
    im.haveFrame = true;
    return &im.frame;
}

int StreamCapture::width()  const { return impl_->w; }
int StreamCapture::height() const { return impl_->h; }
int StreamCapture::nativeWidth()  const { return impl_->w; }
int StreamCapture::nativeHeight() const { return impl_->h; }
std::string StreamCapture::describe() const { return impl_->description; }

} // namespace soi
