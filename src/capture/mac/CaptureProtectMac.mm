// Capture exclusion on macOS: NSWindow.sharingType = NSWindowSharingNone, the
// counterpart of WDA_EXCLUDEFROMCAPTURE. The window server leaves such a
// window out of every user-mode capture, including our own.
//
// soi-share has no windows of its own, so in practice there is nothing to
// protect, and the terminal it runs in belongs to the terminal app -- macOS,
// like Windows, only lets a window's owner change how it is captured. Both
// facts are reported plainly rather than papered over.
#include "capture/CaptureProtect.h"
#include "util/Log.h"

#import <AppKit/AppKit.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace soi {
namespace {

NSWindowSharingType sharingFor(ProtectMode mode) {
    return mode == ProtectMode::None ? NSWindowSharingReadOnly : NSWindowSharingNone;
}

std::string terminalName() {
    const char* program = std::getenv("TERM_PROGRAM");
    if (!program || !*program) return "your terminal app";
    const std::string p = program;
    if (p == "Apple_Terminal") return "Terminal";
    if (p == "iTerm.app")      return "iTerm2";
    if (p == "vscode")         return "VS Code";
    return p;
}

} // namespace

bool excludeFromCaptureSupported() { return true; }

bool applyProtection(void* window, ProtectMode mode) {
    if (!window) return false;
    @autoreleasepool {
        NSWindow* w = (__bridge NSWindow*)window;
        w.sharingType = sharingFor(mode);
    }
    return true;
}

int protectOwnWindows(ProtectMode mode) {
    // No NSApplication means no windows: the common case for this tool.
    if (!NSApp) return 0;
    __block int count = 0;
    auto sweep = ^{
        for (NSWindow* w in [NSApp windows]) {
            w.sharingType = sharingFor(mode);
            ++count;
        }
    };
    if ([NSThread isMainThread]) sweep();
    else dispatch_sync(dispatch_get_main_queue(), sweep);
    return count;
}

bool protectConsoleWindow(ProtectMode mode, std::string& note) {
    if (mode == ProtectMode::None) { note = "protection off"; return true; }
    note = "the terminal window belongs to " + terminalName() +
           ", and macOS only lets a window's owner exclude it from capture; it IS "
           "visible to screen capture. soi-share itself has no windows.";
    return false;
}

struct ProtectionWatchdog::Impl {
    std::thread             thread;
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    stop = false;
    std::atomic<unsigned long long> sweeps{0};
};

ProtectionWatchdog::~ProtectionWatchdog() { stop(); }

void ProtectionWatchdog::start(ProtectMode mode, int intervalMs) {
    stop();
    impl_ = new Impl;
    Impl* im = impl_;
    im->thread = std::thread([im, mode, intervalMs] {
        std::unique_lock lk(im->mtx);
        while (!im->stop) {
            lk.unlock();
            // Only touches windows if this process ever creates any, which
            // it does not today; kept so a future UI is covered from day one.
            if (NSApp) protectOwnWindows(mode);
            ++im->sweeps;
            lk.lock();
            im->cv.wait_for(lk, std::chrono::milliseconds(intervalMs), [im] { return im->stop; });
        }
    });
}

void ProtectionWatchdog::stop() {
    if (!impl_) return;
    {
        std::lock_guard lk(impl_->mtx);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
    delete impl_;
    impl_ = nullptr;
}

unsigned long long ProtectionWatchdog::sweeps() const {
    return impl_ ? impl_->sweeps.load() : 0;
}

std::string describeProtection(ProtectMode mode) {
    switch (mode) {
        case ProtectMode::None:
            return "none -- every window appears in captures";
        case ProtectMode::BlackOut:
        case ProtectMode::ExcludeFromCapture:
            return "NSWindowSharingNone -- our own windows are left out of every capture "
                   "(a compositor convenience, not a security boundary)";
    }
    return "unknown";
}

} // namespace soi
