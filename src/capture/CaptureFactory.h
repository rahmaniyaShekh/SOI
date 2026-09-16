#pragma once
//
// Backend selection.
//
// The point of having three capture backends is that the caller never has to
// know which one works here. createFrameSource() returns a FrameSource that
// starts on the best backend for the target and quietly moves to the next one
// if that stops producing pixels -- a driver reset, a duplication the OS
// revokes, a display that goes away mid-session.
//
// Order, and why:
//
//   monitor / desktop    dxgi -> wgc -> bitblt
//       Duplication sees the scanout image, so fullscreen games and hardware
//       video overlays are in it. WGC is next because it still works where
//       duplication is refused (RDP, some virtual adapters). GDI is the floor.
//
//   window               wgc -> bitblt
//       Duplication cannot address a window at all. WGC is first because GDI
//       PrintWindow returns black for anything DirectComposition-backed, which
//       today is most things.
//
// Pinning one with --capture skips the ordering entirely: exactly the named
// backend is used, and if it fails the capture fails, which is what you want
// when you are diagnosing rather than sharing.
//
#include "capture/FrameSource.h"

#include <memory>
#include <string>
#include <vector>

namespace soi {

std::unique_ptr<FrameSource> createFrameSource(const CaptureConfig& cfg);

// What each backend reports for a given target: whether it starts, what size it
// produces, and whether the first frame it hands back is entirely black. Backs
// `soi-share capture-check`, which is the fastest way to find out why a share
// is showing a blank rectangle.
struct BackendReport {
    CaptureBackend backend = CaptureBackend::Auto;
    bool           started = false;
    bool           gotFrame = false;
    bool           allBlack = false;
    int            width = 0, height = 0;
    double         msPerFrame = 0.0;
    std::string    detail;
};

std::vector<BackendReport> probeBackends(const CaptureConfig& cfg);

} // namespace soi
