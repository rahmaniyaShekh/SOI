// ScreenCaptureKit capture (macOS 12.3+).
//
// The system pushes frames to us on a dispatch queue; capture() hands out the
// latest one. A frame whose status is Idle means "nothing changed", which is
// the compositor telling us for free what the GDI path on Windows has to hash
// the screen to find out -- those are reported as duplicates.
//
// Frames arrive already scaled to the encode size on the GPU (the stream is
// configured with that size), as BGRA IOSurfaces. With preferGpu they are
// handed out as they are; otherwise they are copied once into system memory.
#include "capture/mac/MacCapture.h"
#include "gpu/GpuPipeline.h"
#include "util/Log.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

namespace soi {
struct SckShared {
    std::mutex              mtx;
    std::condition_variable cv;
    CVPixelBufferRef        latest = nullptr;   // retained
    bool                    fresh  = false;
    std::atomic<bool>       failed{false};
    std::string             error;

    ~SckShared() { if (latest) CVPixelBufferRelease(latest); }
};
} // namespace soi

// Holds its own reference to the shared state, so a late callback from a
// stream that is being torn down can never touch state that has gone.
API_AVAILABLE(macos(12.3))
@interface SoiSckOutput : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
    std::shared_ptr<soi::SckShared> shared;
}
@end

@implementation SoiSckOutput
- (void)stream:(SCStream*)stream
    didOutputSampleBuffer:(CMSampleBufferRef)sample
                   ofType:(SCStreamOutputType)type {
    if (type != SCStreamOutputTypeScreen) return;
    auto state = self->shared;
    if (!state) return;

    SCFrameStatus status = SCFrameStatusComplete;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        NSDictionary* info = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
        if (NSNumber* s = info[SCStreamFrameInfoStatus]) status = (SCFrameStatus)s.integerValue;
    }
    if (status != SCFrameStatusComplete) return;   // idle, blank, suspended...

    CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
    if (!buffer) return;
    CVPixelBufferRetain(buffer);
    {
        std::lock_guard lk(state->mtx);
        if (state->latest) CVPixelBufferRelease(state->latest);
        state->latest = buffer;
        state->fresh  = true;
    }
    state->cv.notify_all();
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    auto state = self->shared;
    if (!state) return;
    {
        std::lock_guard lk(state->mtx);
        state->error = error ? [[error localizedDescription] UTF8String] : "stopped";
    }
    state->failed.store(true);
    state->cv.notify_all();
}
@end

namespace soi {

struct SckCapture::Impl {
    CaptureConfig cfg;
    int           w = 0, h = 0;
    std::string   description;
    bool          running = false;

    std::shared_ptr<SckShared> shared;
    id                         stream = nil;   // SCStream*
    id                         output = nil;   // SoiSckOutput*
    dispatch_queue_t           queue  = nullptr;

    Frame                frame{};
    CVPixelBufferRef     held = nullptr;     // what frame.gpuTexture points at
    std::vector<uint8_t> cpu;                // CPU-path copy
    bool                 haveFrame = false;

    ~Impl() { if (held) CVPixelBufferRelease(held); }
};

namespace {

template <class T>
bool waitFor(dispatch_semaphore_t sem, T seconds) {
    return dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW,
                                                      static_cast<int64_t>(seconds * NSEC_PER_SEC))) == 0;
}

} // namespace

SckCapture::SckCapture(const CaptureConfig& cfg) : impl_(std::make_unique<Impl>()) {
    impl_->cfg = cfg;
}

SckCapture::~SckCapture() { stop(); }

bool SckCapture::start() {
    if (@available(macOS 12.3, *)) {
        Impl& im = *impl_;
        if (im.running) return true;
        if (im.cfg.target == CaptureTarget::VirtualDesktop) {
            // One SCStream reads one display; "every display at once" is the
            // CGImage backend's job.
            logT("sck: a stream covers one display, not the whole desktop");
            return false;
        }
        ensureWindowServer();

        @autoreleasepool {
            __block SCShareableContent* content = nil;
            __block NSError* contentError = nil;
            dispatch_semaphore_t got = dispatch_semaphore_create(0);
            [SCShareableContent getShareableContentExcludingDesktopWindows:NO
                                                       onScreenWindowsOnly:YES
                                                         completionHandler:^(SCShareableContent* c,
                                                                             NSError* e) {
                                                             content = c;
                                                             contentError = e;
                                                             dispatch_semaphore_signal(got);
                                                         }];
            if (!waitFor(got, 5.0) || !content) {
                logT("sck: no shareable content ({})",
                     contentError ? [[contentError localizedDescription] UTF8String] : "timed out");
                return false;
            }

            SCContentFilter* filter = nil;
            int srcW = 0, srcH = 0;
            if (im.cfg.target == CaptureTarget::Window) {
                const auto wanted = static_cast<CGWindowID>(reinterpret_cast<uintptr_t>(im.cfg.windowHandle));
                SCWindow* window = nil;
                for (SCWindow* candidate in content.windows)
                    if (candidate.windowID == wanted) { window = candidate; break; }
                if (!window) { logT("sck: window {} is not on screen", wanted); return false; }
                double x, y, ww, hh, scale = 1.0;
                if (!windowGeometry(wanted, x, y, ww, hh, scale)) {
                    ww = window.frame.size.width;
                    hh = window.frame.size.height;
                }
                srcW = static_cast<int>(ww * scale);
                srcH = static_cast<int>(hh * scale);
                filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:window];
                im.description = soi::format("window {} \"{}\" via ScreenCaptureKit", wanted,
                                             window.title ? [window.title UTF8String] : "");
            } else {
                DisplayGeometry g;
                if (!displayForIndex(im.cfg.monitorIndex, g)) {
                    logT("sck: there is no display {}", im.cfg.monitorIndex);
                    return false;
                }
                SCDisplay* display = nil;
                for (SCDisplay* candidate in content.displays)
                    if (candidate.displayID == g.id) { display = candidate; break; }
                if (!display) { logT("sck: display {} is not shareable", g.id); return false; }
                srcW = g.pixelW;
                srcH = g.pixelH;
                filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:@[]];
                im.description = soi::format("display {} ({}x{}) via ScreenCaptureKit",
                                             im.cfg.monitorIndex, srcW, srcH);
            }
            if (srcW < 2 || srcH < 2) return false;

            // The GPU scales to the size the encoder will want anyway.
            computeEncodeSize(srcW, srcH, im.cfg.maxWidth, im.w, im.h);

            SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
            config.width       = static_cast<size_t>(im.w);
            config.height      = static_cast<size_t>(im.h);
            config.pixelFormat = kCVPixelFormatType_32BGRA;
            config.showsCursor = im.cfg.captureCursor ? YES : NO;
            config.minimumFrameInterval = CMTimeMake(1, 60);
            config.queueDepth  = 5;
            config.colorSpaceName = kCGColorSpaceSRGB;   // what the NV12 matrices assume
            config.scalesToFit = YES;

            im.shared = std::make_shared<SckShared>();
            SoiSckOutput* output = [[SoiSckOutput alloc] init];
            output->shared = im.shared;
            im.output = output;
            im.queue  = dispatch_queue_create("soi.sck.frames", DISPATCH_QUEUE_SERIAL);

            SCStream* stream = [[SCStream alloc] initWithFilter:filter
                                                  configuration:config
                                                       delegate:output];
            NSError* addError = nil;
            if (![stream addStreamOutput:output
                                    type:SCStreamOutputTypeScreen
                      sampleHandlerQueue:im.queue
                                   error:&addError]) {
                logT("sck: addStreamOutput failed: {}", [[addError localizedDescription] UTF8String]);
                im.output = nil;
                return false;
            }

            __block NSError* startError = nil;
            dispatch_semaphore_t started = dispatch_semaphore_create(0);
            [stream startCaptureWithCompletionHandler:^(NSError* e) {
                startError = e;
                dispatch_semaphore_signal(started);
            }];
            if (!waitFor(started, 5.0) || startError) {
                // The usual reason: the terminal has not been granted Screen
                // Recording, which SCK reports as "the user declined TCCs".
                logT("sck: could not start the stream: {}",
                     startError ? [[startError localizedDescription] UTF8String] : "timed out");
                im.output = nil;
                return false;
            }
            im.stream  = stream;
            im.running = true;
        }

        // Wait for the first frame, so a caller that captures right away (the
        // probe in capture-check, the first encode) gets one.
        {
            std::unique_lock lk(im.shared->mtx);
            im.shared->cv.wait_for(lk, std::chrono::seconds(2), [&] {
                return im.shared->latest != nullptr || im.shared->failed.load();
            });
            if (!im.shared->latest) {
                lk.unlock();
                logT("sck: the stream started but delivered no frame");
                stop();
                return false;
            }
        }
        im.haveFrame = false;
        logT("sck: {} -> {}x{}", im.description, im.w, im.h);
        return true;
    } else {
        logT("sck: ScreenCaptureKit needs macOS 12.3 or later");
        return false;
    }
}

void SckCapture::stop() {
    Impl& im = *impl_;
    if (@available(macOS 12.3, *)) {
        if (im.stream) {
            SCStream* stream = im.stream;
            dispatch_semaphore_t done = dispatch_semaphore_create(0);
            [stream stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(done); }];
            waitFor(done, 3.0);
            im.stream = nil;
        }
    }
    im.output  = nil;
    im.queue   = nullptr;
    im.running = false;
    if (im.held) { CVPixelBufferRelease(im.held); im.held = nullptr; }
    im.frame = Frame{};
    im.haveFrame = false;
}

bool SckCapture::retarget(const CaptureConfig& cfg) {
    const CaptureConfig previous = impl_->cfg;
    stop();
    impl_->cfg = cfg;
    if (start()) return true;
    impl_->cfg = previous;
    start();
    return false;
}

const Frame* SckCapture::capture() {
    Impl& im = *impl_;
    if (!im.running || !im.shared || im.shared->failed.load()) return nullptr;

    CVPixelBufferRef buffer = nullptr;
    bool fresh = false;
    {
        std::lock_guard lk(im.shared->mtx);
        if (!im.shared->latest) return nullptr;
        buffer = CVPixelBufferRetain(im.shared->latest);
        fresh = im.shared->fresh;
        im.shared->fresh = false;
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    im.frame.timeNs    = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    im.frame.duplicate = im.haveFrame && !fresh;
    im.frame.width     = static_cast<int>(CVPixelBufferGetWidth(buffer));
    im.frame.height    = static_cast<int>(CVPixelBufferGetHeight(buffer));

    if (im.cfg.preferGpu) {
        if (im.held) CVPixelBufferRelease(im.held);
        im.held = buffer;   // kept alive until the next capture()
        im.frame.gpuTexture = buffer;
        im.frame.data   = nullptr;
        im.frame.stride = static_cast<int>(CVPixelBufferGetBytesPerRow(buffer));
    } else {
        if (fresh || im.cpu.empty()) {
            CVPixelBufferLockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
            const auto* base = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
            const size_t srcStride = CVPixelBufferGetBytesPerRow(buffer);
            const size_t rowBytes  = static_cast<size_t>(im.frame.width) * 4;
            im.cpu.resize(rowBytes * static_cast<size_t>(im.frame.height));
            for (int y = 0; y < im.frame.height; ++y)
                std::memcpy(im.cpu.data() + y * rowBytes, base + y * srcStride, rowBytes);
            CVPixelBufferUnlockBaseAddress(buffer, kCVPixelBufferLock_ReadOnly);
        }
        CVPixelBufferRelease(buffer);
        im.frame.gpuTexture = nullptr;
        im.frame.data   = im.cpu.data();
        im.frame.stride = im.frame.width * 4;
    }
    im.haveFrame = true;
    return &im.frame;
}

int SckCapture::width()  const { return impl_->w; }
int SckCapture::height() const { return impl_->h; }
int SckCapture::nativeWidth()  const { return impl_->w; }
int SckCapture::nativeHeight() const { return impl_->h; }
std::string SckCapture::describe() const { return impl_->description; }

std::shared_ptr<GpuDevice> SckCapture::gpuDevice() const {
    return impl_->running && impl_->cfg.preferGpu ? sharedGpuDevice() : nullptr;
}

} // namespace soi
