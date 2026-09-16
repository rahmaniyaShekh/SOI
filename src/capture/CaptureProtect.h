#pragma once
//
// Window capture exclusion.
//
// This is the mechanism that actually answers "make it safe from other screen
// capture" -- it has nothing to do with BitBlt vs WGC vs DXGI. Exclusion is
// enforced by DWM at composition time, so it applies uniformly to every
// user-mode capture path, including our own.
//
// Scope, stated plainly: this is a compositor convenience, NOT a security
// boundary. It does not defeat kernel-mode capture, mirror drivers, injected
// DLLs reading process memory, hardware HDMI capture, or a camera pointed at
// the screen. See README 1.2.
//
#include <string>

namespace soi {

enum class ProtectMode {
    None,               // WDA_NONE   - normal, appears in captures
    BlackOut,           // WDA_MONITOR - renders black in captures. Win7+.
    ExcludeFromCapture  // WDA_EXCLUDEFROMCAPTURE - absent from captures but
                        // visible on the physical monitor. Win10 2004+.
};

// True if the running OS supports WDA_EXCLUDEFROMCAPTURE (build >= 19041).
// Below that, applyProtection() silently degrades to BlackOut.
bool excludeFromCaptureSupported();

// Applies the affinity to a window. Returns false if the call was rejected --
// most commonly because the window belongs to another process, or because the
// window has a DWM-incompatible style.
bool applyProtection(void* hwnd, ProtectMode mode);

// Marks every top-level window owned by this process. Called after any UI is
// created so the sender's own windows never appear in its own stream.
int protectOwnWindows(ProtectMode mode);

// Attempts to protect the console/terminal window this process is attached to.
//
// This frequently FAILS, and the failure is important rather than incidental:
// under Windows Terminal (the default on Windows 11) the visible window belongs
// to WindowsTerminal.exe, and SetWindowDisplayAffinity only works on windows
// owned by the calling process. When it fails, the terminal -- including any
// signalling blob printed in it -- IS visible to screen capture, including our
// own. `note` receives a human-readable explanation either way.
bool protectConsoleWindow(ProtectMode mode, std::string& note);

// Re-applies protection on a timer.
//
// A single sweep at startup is not enough: any window created later (a dialog,
// a tooltip, a tray balloon) would be unprotected, and a window that is
// destroyed and recreated loses its affinity. The watchdog closes that window of
// exposure to at most one interval.
class ProtectionWatchdog {
public:
    ProtectionWatchdog() = default;
    ~ProtectionWatchdog();

    ProtectionWatchdog(const ProtectionWatchdog&) = delete;
    ProtectionWatchdog& operator=(const ProtectionWatchdog&) = delete;

    void start(ProtectMode mode, int intervalMs = 1000);
    void stop();

    // Number of sweeps performed, for diagnostics and tests.
    unsigned long long sweeps() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
};

std::string describeProtection(ProtectMode mode);

} // namespace soi
