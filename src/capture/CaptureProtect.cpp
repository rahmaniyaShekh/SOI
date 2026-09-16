#include "capture/CaptureProtect.h"
#include "util/Log.h"
#include "util/Win.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace soi {
namespace {

// Not present in older SDK headers.
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#ifndef WDA_MONITOR
#define WDA_MONITOR 0x00000001
#endif
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif

// GetVersionEx is version-lied to for unmanifested apps, and VerifyVersionInfo
// is deprecated. RtlGetVersion is the one that reports the truth.
bool osBuildAtLeast(DWORD build) {
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    static const DWORD actual = [] () -> DWORD {
        HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        if (!nt) return 0;
        auto fn = reinterpret_cast<RtlGetVersionFn>(
            reinterpret_cast<void*>(GetProcAddress(nt, "RtlGetVersion")));
        if (!fn) return 0;

        RTL_OSVERSIONINFOW vi{};
        vi.dwOSVersionInfoSize = sizeof(vi);
        if (fn(&vi) != 0) return 0;
        return vi.dwBuildNumber;
    }();

    return actual >= build;
}

DWORD affinityValue(ProtectMode mode) {
    switch (mode) {
        case ProtectMode::None:               return WDA_NONE;
        case ProtectMode::BlackOut:           return WDA_MONITOR;
        case ProtectMode::ExcludeFromCapture: return WDA_EXCLUDEFROMCAPTURE;
    }
    return WDA_NONE;
}

struct OwnWindowCollector {
    DWORD                pid;
    std::vector<HWND>*   out;
};

BOOL CALLBACK collectOwnWindows(HWND wnd, LPARAM param) {
    auto* col = reinterpret_cast<OwnWindowCollector*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(wnd, &pid);
    if (pid == col->pid) col->out->push_back(wnd);
    return TRUE;
}

// Silent variant for the watchdog: it runs every second, so logging each sweep
// would bury everything else.
int protectOwnWindowsQuiet(ProtectMode mode) {
    std::vector<HWND> windows;
    OwnWindowCollector col{GetCurrentProcessId(), &windows};
    EnumWindows(collectOwnWindows, reinterpret_cast<LPARAM>(&col));

    const DWORD want = affinityValue(
        mode == ProtectMode::ExcludeFromCapture && !excludeFromCaptureSupported()
            ? ProtectMode::BlackOut
            : mode);

    int applied = 0;
    for (HWND w : windows) {
        // Skip windows already in the right state: SetWindowDisplayAffinity is a
        // cross-process call into the compositor and is not free at 1 Hz x N.
        DWORD current = 0;
        if (GetWindowDisplayAffinity(w, &current) && current == want) { ++applied; continue; }
        if (SetWindowDisplayAffinity(w, want)) ++applied;
    }

    // The console is usually the only thing on screen belonging to this session,
    // so keep retrying it -- a terminal can be reattached.
    if (HWND console = GetConsoleWindow()) {
        DWORD current = 0;
        if (!(GetWindowDisplayAffinity(console, &current) && current == want))
            SetWindowDisplayAffinity(console, want);
    }
    return applied;
}

} // namespace

bool excludeFromCaptureSupported() {
    return osBuildAtLeast(19041);   // Windows 10 2004
}

bool applyProtection(void* hwndRaw, ProtectMode mode) {
    HWND wnd = static_cast<HWND>(hwndRaw);
    if (!wnd || !IsWindow(wnd)) return false;

    ProtectMode effective = mode;
    if (mode == ProtectMode::ExcludeFromCapture && !excludeFromCaptureSupported()) {
        effective = ProtectMode::BlackOut;
        logW("WDA_EXCLUDEFROMCAPTURE needs Windows 10 build 19041+; "
             "falling back to WDA_MONITOR (window will render black in captures)");
    }

    if (!SetWindowDisplayAffinity(wnd, affinityValue(effective))) {
        logW("SetWindowDisplayAffinity failed for {:p}: {}",
             hwndRaw, hrString(static_cast<long>(HRESULT_FROM_WIN32(GetLastError()))));
        return false;
    }
    return true;
}

int protectOwnWindows(ProtectMode mode) {
    std::vector<HWND> windows;
    OwnWindowCollector col{GetCurrentProcessId(), &windows};
    EnumWindows(collectOwnWindows, reinterpret_cast<LPARAM>(&col));

    int applied = 0;
    for (HWND w : windows)
        if (applyProtection(w, mode)) ++applied;

    if (applied)
        logI("capture protection: {} applied to {} own window(s)",
             describeProtection(mode), applied);
    return applied;
}

bool protectConsoleWindow(ProtectMode mode, std::string& note) {
    HWND console = GetConsoleWindow();
    if (!console) {
        note = "no console window is attached to this process";
        return false;
    }

    DWORD consolePid = 0;
    GetWindowThreadProcessId(console, &consolePid);
    const bool ours = (consolePid == GetCurrentProcessId());

    if (applyProtection(console, mode)) {
        // Trust Windows, not our own return value.
        DWORD affinity = 0;
        if (GetWindowDisplayAffinity(console, &affinity) && affinity != 0) {
            note = "console window protected";
            return true;
        }
        note = "SetWindowDisplayAffinity reported success but the affinity did "
               "not stick";
        return false;
    }

    note = ours
               ? "the console window rejected the display-affinity change"
               : "the visible terminal belongs to another process (Windows "
                 "Terminal / conhost), so it cannot be excluded from capture "
                 "from here -- anything printed in it WILL appear in the stream";
    return false;
}

// ---------------------------------------------------------------------------

struct ProtectionWatchdog::Impl {
    std::thread             thread;
    std::mutex              mtx;
    std::condition_variable cv;
    bool                    stop = false;
    ProtectMode             mode = ProtectMode::ExcludeFromCapture;
    int                     intervalMs = 1000;
    std::atomic<unsigned long long> sweeps{0};
};

ProtectionWatchdog::~ProtectionWatchdog() { stop(); }

void ProtectionWatchdog::start(ProtectMode mode, int intervalMs) {
    if (impl_) return;
    impl_ = new Impl();
    impl_->mode       = mode;
    impl_->intervalMs = intervalMs > 0 ? intervalMs : 1000;

    impl_->thread = std::thread([this] {
        Impl* s = impl_;
        for (;;) {
            protectOwnWindowsQuiet(s->mode);
            s->sweeps.fetch_add(1);

            std::unique_lock lk(s->mtx);
            if (s->cv.wait_for(lk, std::chrono::milliseconds(s->intervalMs),
                               [s] { return s->stop; }))
                return;
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
        case ProtectMode::None:     return "WDA_NONE (visible to capture)";
        case ProtectMode::BlackOut: return "WDA_MONITOR (renders black in capture)";
        case ProtectMode::ExcludeFromCapture:
            return excludeFromCaptureSupported()
                       ? "WDA_EXCLUDEFROMCAPTURE (absent from capture)"
                       : "WDA_EXCLUDEFROMCAPTURE -> degraded to WDA_MONITOR";
    }
    return "unknown";
}

} // namespace soi
