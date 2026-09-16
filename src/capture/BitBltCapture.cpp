#include "capture/BitBltCapture.h"
#include "util/Log.h"

#include <windows.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace soi {
namespace {

// PrintWindow flag that asks the window to render its full content including
// DirectComposition-backed surfaces. Win 8.1+. Not in older SDK headers.
#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct MonitorCollector {
    std::vector<MonitorInfo>* out;
    int next = 0;
};

BOOL CALLBACK monitorProc(HMONITOR mon, HDC, LPRECT, LPARAM param) {
    auto* col = reinterpret_cast<MonitorCollector*>(param);

    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return TRUE;

    MonitorInfo info;
    info.index   = col->next++;
    info.x       = mi.rcMonitor.left;
    info.y       = mi.rcMonitor.top;
    info.width   = mi.rcMonitor.right - mi.rcMonitor.left;
    info.height  = mi.rcMonitor.bottom - mi.rcMonitor.top;
    info.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    info.name    = toUtf8(mi.szDevice);
    info.handle  = mon;          // Windows.Graphics.Capture needs the HMONITOR
    col->out->push_back(std::move(info));
    return TRUE;
}

bool windowIsCloaked(HWND wnd) {
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(wnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))))
        return cloaked != FALSE;
    return false;
}

std::string processNameOf(HWND wnd) {
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (!pid) return {};

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) return {};

    wchar_t path[MAX_PATH] = {};
    DWORD   len = MAX_PATH;
    std::string name;
    if (QueryFullProcessImageNameW(proc, 0, path, &len)) {
        std::wstring_view sv(path, len);
        const size_t slash = sv.find_last_of(L"\\/");
        name = toUtf8(slash == std::wstring_view::npos ? sv : sv.substr(slash + 1));
    }
    CloseHandle(proc);
    return name;
}

BOOL CALLBACK windowProc(HWND wnd, LPARAM param) {
    auto* out = reinterpret_cast<std::vector<WindowInfo>*>(param);

    if (!IsWindowVisible(wnd) || IsIconic(wnd)) return TRUE;
    if (GetWindow(wnd, GW_OWNER) != nullptr)     return TRUE;
    if (windowIsCloaked(wnd))                    return TRUE;

    const LONG_PTR ex = GetWindowLongPtrW(wnd, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;

    RECT r{};
    if (!GetWindowRect(wnd, &r)) return TRUE;
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w < 64 || h < 64) return TRUE;

    wchar_t title[512] = {};
    const int n = GetWindowTextW(wnd, title, 512);
    if (n <= 0) return TRUE;

    WindowInfo info;
    info.handle  = wnd;
    info.title   = toUtf8(std::wstring_view(title, static_cast<size_t>(n)));
    info.process = processNameOf(wnd);
    info.width   = w;
    info.height  = h;
    out->push_back(std::move(info));
    return TRUE;
}

} // namespace

// ---------------------------------------------------------------------------

std::vector<MonitorInfo> enumerateMonitors() {
    std::vector<MonitorInfo> out;
    MonitorCollector col{&out, 0};
    EnumDisplayMonitors(nullptr, nullptr, monitorProc, reinterpret_cast<LPARAM>(&col));
    return out;
}

std::vector<WindowInfo> enumerateWindows() {
    std::vector<WindowInfo> out;
    EnumWindows(windowProc, reinterpret_cast<LPARAM>(&out));
    return out;
}

// ---------------------------------------------------------------------------

BitBltCapture::BitBltCapture(CaptureConfig cfg) : cfg_(cfg) {}
BitBltCapture::~BitBltCapture() { stop(); }

bool BitBltCapture::resolveTarget() {
    switch (cfg_.target) {
        case CaptureTarget::VirtualDesktop: {
            srcX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
            srcY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
            srcW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            srcH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            description_ = soi::format("virtual desktop {}x{} at ({},{})",
                                       srcW_, srcH_, srcX_, srcY_);
            break;
        }
        case CaptureTarget::Monitor: {
            const auto mons = enumerateMonitors();
            if (mons.empty()) { logE("no monitors found"); return false; }
            if (cfg_.monitorIndex < 0 ||
                cfg_.monitorIndex >= static_cast<int>(mons.size())) {
                logE("monitor index {} out of range (found {})",
                     cfg_.monitorIndex, mons.size());
                return false;
            }
            const auto& m = mons[static_cast<size_t>(cfg_.monitorIndex)];
            srcX_ = m.x; srcY_ = m.y; srcW_ = m.width; srcH_ = m.height;
            description_ = soi::format("monitor {} ({}) {}x{}",
                                       m.index, m.name, srcW_, srcH_);
            break;
        }
        case CaptureTarget::Window: {
            HWND wnd = static_cast<HWND>(cfg_.windowHandle);
            if (!wnd || !IsWindow(wnd)) { logE("target window is not valid"); return false; }

            // Prefer the DWM extended frame bounds: GetWindowRect includes the
            // invisible resize border on Win10+, which would show as a black edge.
            RECT r{};
            if (FAILED(DwmGetWindowAttribute(wnd, DWMWA_EXTENDED_FRAME_BOUNDS,
                                             &r, sizeof(r))) ||
                (r.right - r.left) <= 0) {
                if (!GetWindowRect(wnd, &r)) { logE("GetWindowRect failed"); return false; }
            }
            srcX_ = 0; srcY_ = 0;
            srcW_ = r.right - r.left;
            srcH_ = r.bottom - r.top;

            wchar_t title[256] = {};
            GetWindowTextW(wnd, title, 256);
            description_ = soi::format("window '{}' {}x{}", toUtf8(title), srcW_, srcH_);
            break;
        }
    }

    if (srcW_ <= 0 || srcH_ <= 0) {
        logE("capture target has zero size");
        return false;
    }
    return true;
}

bool BitBltCapture::start() {
    if (started_) return true;
    if (!resolveTarget()) return false;

    // The blit is always 1:1; outW_/outH_ only tell the converter what to
    // downscale to.
    computeEncodeSize(srcW_, srcH_, cfg_.maxWidth, outW_, outH_);
    if (outW_ < 16 || outH_ < 16) { logE("capture output too small"); return false; }

    screenDc_ = WindowDc(nullptr, GetDC(nullptr));
    if (!screenDc_) { logE("GetDC(NULL) failed"); return false; }

    if (!buffer_.resize(srcW_, srcH_)) {
        logE("CreateDIBSection failed for {}x{} capture buffer", srcW_, srcH_);
        return false;
    }

    started_ = true;
    logI("capture: {} via GDI BitBlt -> {}x{}{}", description_, outW_, outH_,
         (outW_ != srcW_ || outH_ != srcH_) ? " (downscaled during conversion)" : "");
    return true;
}

bool BitBltCapture::retarget(const CaptureConfig& cfg) {
    const CaptureConfig previous = cfg_;

    stop();
    cfg_ = cfg;
    if (start()) return true;

    // Put it back. A failed switch must not cost the viewer the picture they
    // already had -- the far end asked for a different monitor, not for the
    // share to end.
    logE("could not switch capture target; restoring the previous one");
    stop();
    cfg_ = previous;
    return start();
}

void BitBltCapture::stop() {
    if (!started_) return;
    buffer_.release();
    screenDc_ = WindowDc();
    started_ = false;
    haveLastHash_ = false;
}

bool BitBltCapture::blitDesktopOrMonitor(HDC dst) {
    // CAPTUREBLT pulls in layered/transparent windows. Measured cost on a
    // 1920x1200 desktop: 16.67 ms vs 16.95 ms without -- i.e. within noise, since
    // both are pinned to the compositor's refresh rate. It can still make the
    // cursor flicker, so it stays opt-in.
    DWORD rop = SRCCOPY;
    if (cfg_.includeLayered) rop |= CAPTUREBLT;

    return BitBlt(dst, 0, 0, srcW_, srcH_,
                  screenDc_.get(), srcX_, srcY_, rop) != FALSE;
}

bool BitBltCapture::blitWindow(HDC dst) {
    HWND wnd = static_cast<HWND>(cfg_.windowHandle);
    if (!IsWindow(wnd)) return false;

    // PW_RENDERFULLCONTENT asks the window to re-render itself, which is the only
    // way to get pixels out of DirectComposition-backed windows (Chrome, Electron).
    // It fails or returns black on some targets, hence the BitBlt fallback --
    // and hence WgcCapture, which does not have the problem at all.
    if (PrintWindow(wnd, dst, PW_RENDERFULLCONTENT)) return true;
    if (PrintWindow(wnd, dst, 0)) return true;

    WindowDc wdc(wnd, GetWindowDC(wnd));
    if (!wdc) return false;
    DWORD rop = SRCCOPY;
    if (cfg_.includeLayered) rop |= CAPTUREBLT;
    return BitBlt(dst, 0, 0, srcW_, srcH_, wdc.get(), 0, 0, rop) != FALSE;
}

bool BitBltCapture::isDuplicate() {
    // GDI gives no damage information, so the only way to know a frame is
    // unchanged is to look at it. The other two backends are told by the OS.
    const uint64_t h = sampledFrameHash(buffer_.pixels(), buffer_.stride(), srcH_);
    const bool dup = haveLastHash_ && h == lastHash_;
    lastHash_     = h;
    haveLastHash_ = true;
    return dup;
}

const Frame* BitBltCapture::capture() {
    if (!started_) return nullptr;

    const bool ok = (cfg_.target == CaptureTarget::Window)
                        ? blitWindow(buffer_.dc())
                        : blitDesktopOrMonitor(buffer_.dc());

    if (!ok) {
        logT("blit failed (target may have closed or a secure desktop is active)");
        return nullptr;
    }

    if (cfg_.captureCursor) {
        // Window blits land at the window's origin, so that is where the cursor
        // has to be measured from.
        int originX = srcX_, originY = srcY_;
        if (cfg_.target == CaptureTarget::Window) {
            RECT r{};
            HWND wnd = static_cast<HWND>(cfg_.windowHandle);
            if (FAILED(DwmGetWindowAttribute(wnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))))
                GetWindowRect(wnd, &r);
            originX = r.left; originY = r.top;
        }
        compositeCursor(buffer_.dc(), originX, originY);
    }

    // GDI batches drawing calls; the DIB section's memory is not guaranteed
    // coherent until the batch is flushed. Reading without this yields tearing.
    FrameBuffer::flushGdi();

    frame_.data      = buffer_.pixels();
    frame_.width     = srcW_;
    frame_.height    = srcH_;
    frame_.stride    = buffer_.stride();
    frame_.timeNs    = nowNs();
    frame_.duplicate = cfg_.detectDuplicates && isDuplicate();
    return &frame_;
}

} // namespace soi
