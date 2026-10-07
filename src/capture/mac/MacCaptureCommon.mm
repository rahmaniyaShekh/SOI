#include "capture/mac/MacCapture.h"
#include "gpu/GpuPipeline.h"
#include "util/Log.h"

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <algorithm>
#include <mutex>
#include <unistd.h>

namespace soi {

// --- naming ------------------------------------------------------------------

const char* backendName(CaptureBackend backend) {
    switch (backend) {
        case CaptureBackend::Auto:    return "auto";
        case CaptureBackend::Sck:     return "sck";
        case CaptureBackend::Stream:  return "stream";
        case CaptureBackend::CgImage: return "cgimage";
    }
    return "unknown";
}

bool parseBackendName(std::string_view name, CaptureBackend& out) {
    if (name == "auto")                                 { out = CaptureBackend::Auto;    return true; }
    if (name == "sck" || name == "screencapturekit")    { out = CaptureBackend::Sck;     return true; }
    if (name == "stream" || name == "displaystream")    { out = CaptureBackend::Stream;  return true; }
    if (name == "cgimage" || name == "image")           { out = CaptureBackend::CgImage; return true; }
    return false;
}

const char* backendChoices() { return "auto, sck, stream, cgimage"; }

void computeEncodeSize(int srcW, int srcH, int maxWidth, int& outW, int& outH) {
    auto evenDown = [](int v) { return v & ~1; };
    if (maxWidth > 0 && srcW > maxWidth) {
        const double scale = static_cast<double>(maxWidth) / srcW;
        outW = evenDown(maxWidth);
        outH = evenDown(static_cast<int>(srcH * scale + 0.5));
    } else {
        outW = evenDown(srcW);
        outH = evenDown(srcH);
    }
}

// --- permission --------------------------------------------------------------

bool screenCapturePermitted(bool prompt) {
    ensureWindowServer();
    if (CGPreflightScreenCaptureAccess()) return true;
    // Lets a diagnosis proceed on a machine where the preflight answer is
    // known to be wrong (some remote-desktop and CI setups).
    if (const char* skip = getenv("SOI_SHARE_SKIP_PERMISSION_CHECK"); skip && skip[0] == '1')
        return true;
    if (prompt) CGRequestScreenCaptureAccess();
    return false;
}

// --- window server --------------------------------------------------------------

void ensureWindowServer() {
    static std::once_flag once;
    std::call_once(once, [] { (void)CGMainDisplayID(); });
}

std::shared_ptr<GpuDevice> sharedGpuDevice() {
    static std::shared_ptr<GpuDevice> dev = GpuDevice::create();
    return dev;
}

uint64_t sampledFrameHashMac(const uint8_t* pixels, int stride, int width, int height) {
    // FNV-1a over every 2nd row and every 4th pixel, as on Windows.
    uint64_t h = 1469598103934665603ull;
    for (int y = 0; y < height; y += 2) {
        const auto* row = reinterpret_cast<const uint32_t*>(pixels + static_cast<size_t>(y) * stride);
        for (int x = 0; x < width; x += 4) {
            h ^= row[x];
            h *= 1099511628211ull;
        }
    }
    return h;
}

// --- displays -----------------------------------------------------------------

std::vector<DisplayGeometry> orderedDisplays() {
    ensureWindowServer();
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(32, ids, &count) != kCGErrorSuccess) count = 0;

    const CGDirectDisplayID mainId = CGMainDisplayID();
    std::vector<DisplayGeometry> out;
    for (uint32_t i = 0; i < count; ++i) {
        // A mirror shows another display's content; listing it would offer the
        // same screen twice.
        if (CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) continue;
        DisplayGeometry g;
        g.id = ids[i];
        const CGRect b = CGDisplayBounds(ids[i]);
        g.x = b.origin.x;
        g.y = b.origin.y;
        g.pointW = b.size.width;
        g.pointH = b.size.height;
        g.pixelW = static_cast<int>(CGDisplayPixelsWide(ids[i]));
        g.pixelH = static_cast<int>(CGDisplayPixelsHigh(ids[i]));
        if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i])) {
            g.pixelW = static_cast<int>(CGDisplayModeGetPixelWidth(mode));
            g.pixelH = static_cast<int>(CGDisplayModeGetPixelHeight(mode));
            CGDisplayModeRelease(mode);
        }
        g.scale = g.pointW > 0 ? g.pixelW / g.pointW : 1.0;
        g.main  = ids[i] == mainId;
        out.push_back(g);
    }
    std::stable_sort(out.begin(), out.end(), [](const DisplayGeometry& a, const DisplayGeometry& b) {
        if (a.main != b.main) return a.main;
        if (a.x != b.x) return a.x < b.x;
        return a.y < b.y;
    });
    return out;
}

bool displayForIndex(int index, DisplayGeometry& out) {
    const auto all = orderedDisplays();
    if (index < 0 || index >= static_cast<int>(all.size())) return false;
    out = all[static_cast<size_t>(index)];
    return true;
}

std::vector<MonitorInfo> enumerateMonitors() {
    std::vector<MonitorInfo> out;
    const auto displays = orderedDisplays();
    @autoreleasepool {
        for (size_t i = 0; i < displays.size(); ++i) {
            const auto& d = displays[i];
            MonitorInfo m;
            m.index   = static_cast<int>(i);
            // Pixels throughout, as on Windows: the size is what a capture of
            // this display produces, and the origin is scaled to match.
            m.x       = static_cast<int>(d.x * d.scale);
            m.y       = static_cast<int>(d.y * d.scale);
            m.width   = d.pixelW;
            m.height  = d.pixelH;
            m.primary = d.main;
            m.handle  = reinterpret_cast<void*>(static_cast<uintptr_t>(d.id));
            m.name    = "Display " + std::to_string(d.id);
            if (@available(macOS 10.15, *)) {
                for (NSScreen* screen in [NSScreen screens]) {
                    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
                    if (number && number.unsignedIntValue == d.id) {
                        m.name = [screen.localizedName UTF8String];
                        break;
                    }
                }
            }
            out.push_back(std::move(m));
        }
    }
    return out;
}

// --- windows --------------------------------------------------------------------

namespace {
double numberOf(CFDictionaryRef dict, CFStringRef key) {
    double v = 0;
    if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(dict, key)))
        CFNumberGetValue(n, kCFNumberDoubleType, &v);
    return v;
}
std::string stringOf(CFDictionaryRef dict, CFStringRef key) {
    auto s = static_cast<CFStringRef>(CFDictionaryGetValue(dict, key));
    if (!s) return {};
    char buf[1024];
    return CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? std::string(buf)
                                                                         : std::string();
}
double scaleAt(double cx, double cy) {
    for (const auto& d : orderedDisplays())
        if (cx >= d.x && cx < d.x + d.pointW && cy >= d.y && cy < d.y + d.pointH) return d.scale;
    DisplayGeometry main;
    return displayForIndex(0, main) ? main.scale : 1.0;
}
} // namespace

bool windowGeometry(uint32_t windowId, double& x, double& y, double& w, double& h,
                    double& scale) {
    ensureWindowServer();
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, windowId);
    if (!list) return false;
    bool found = false;
    if (CFArrayGetCount(list) > 0) {
        auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, 0));
        CGRect r{};
        if (auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
            bounds && CGRectMakeWithDictionaryRepresentation(bounds, &r) && r.size.width > 0) {
            x = r.origin.x; y = r.origin.y; w = r.size.width; h = r.size.height;
            scale = scaleAt(x + w / 2, y + h / 2);
            found = true;
        }
    }
    CFRelease(list);
    return found;
}

std::vector<WindowInfo> enumerateWindows() {
    ensureWindowServer();
    std::vector<WindowInfo> out;
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return out;
    const double self = static_cast<double>(getpid());
    for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
        auto info = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        // Layer 0 is ordinary application windows; menus, the Dock and the
        // menu bar live on other layers.
        if (numberOf(info, kCGWindowLayer) != 0) continue;
        if (numberOf(info, kCGWindowAlpha) <= 0) continue;
        if (numberOf(info, kCGWindowOwnerPID) == self) continue;
        CGRect r{};
        auto bounds = static_cast<CFDictionaryRef>(CFDictionaryGetValue(info, kCGWindowBounds));
        if (!bounds || !CGRectMakeWithDictionaryRepresentation(bounds, &r)) continue;
        if (r.size.width < 50 || r.size.height < 50) continue;

        WindowInfo w;
        w.handle  = reinterpret_cast<void*>(static_cast<uintptr_t>(numberOf(info, kCGWindowNumber)));
        w.process = stringOf(info, kCGWindowOwnerName);
        // Titles are only visible with the Screen Recording permission.
        w.title   = stringOf(info, kCGWindowName);
        if (w.title.empty()) w.title = "(" + w.process + ")";
        const double s = scaleAt(r.origin.x + r.size.width / 2, r.origin.y + r.size.height / 2);
        w.width  = static_cast<int>(r.size.width * s);
        w.height = static_cast<int>(r.size.height * s);
        out.push_back(std::move(w));
    }
    CFRelease(list);
    return out;
}

} // namespace soi
