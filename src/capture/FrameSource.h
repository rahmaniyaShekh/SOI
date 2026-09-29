#pragma once
//
// Capture abstraction.
//
// Three backends implement it, because no single Windows capture API can see
// everything that is on the screen:
//
//   DxgiCapture   Desktop Duplication. Reads the scanout image the display
//                 controller is actually showing, so it picks up exclusive
//                 fullscreen games, hardware video overlays (MPO) and anything
//                 else that never round-trips through GDI. Whole outputs only.
//   WgcCapture    Windows.Graphics.Capture. Per-window or per-monitor, composed
//                 by DWM, and the only one of the three that works inside an RDP
//                 session or on a display adapter that refuses duplication.
//   BitBltCapture GDI. Slowest and blindest, but has no dependencies at all and
//                 can read an occluded window. The floor everything falls back
//                 to.
//
// CaptureFactory.h picks between them and re-picks at run time if the chosen one
// stops producing frames. See README 1.2 for what none of them can do.
//
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace soi {

class GpuDevice;

// A captured frame in top-down BGRA8888. `data` points into storage owned by the
// FrameSource and is valid only until the next capture() call.
struct Frame {
    const uint8_t* data   = nullptr;
    int            width  = 0;
    int            height = 0;
    int            stride = 0;       // bytes per row
    int64_t        timeNs = 0;       // steady-clock capture time
    bool           duplicate = false; // pixel-identical to the previous frame

    // On the GPU pipeline the pixels never come back to the CPU: `data` is null
    // and this is a BGRA ID3D11Texture2D on the shared device instead, valid
    // until the next capture() call. See gpu/GpuPipeline.h.
    void*          gpuTexture = nullptr;
};

enum class CaptureTarget { VirtualDesktop, Monitor, Window };

// Which API does the reading. Auto is the only value most callers should use;
// the rest exist so a user hitting a driver bug can pin a working one and so the
// self-test can exercise each in isolation.
enum class CaptureBackend { Auto, Dxgi, Wgc, BitBlt };

struct CaptureConfig {
    CaptureTarget  target       = CaptureTarget::Monitor;
    CaptureBackend backend      = CaptureBackend::Auto;
    int            monitorIndex = 0;
    void*          windowHandle = nullptr;   // HWND, when target == Window
    bool           captureCursor = false;
    // CAPTUREBLT pulls layered/transparent windows into a GDI blit. BitBlt only;
    // the other two backends read the composed desktop and get them regardless.
    bool           includeLayered = false;
    int            maxWidth = 1920;          // downscale above this; 0 disables
    bool           detectDuplicates = true;

    // Keep frames on the graphics card all the way to the encoder. Only the DXGI
    // backend can serve this, and only when nothing needs CPU-visible pixels --
    // captureCursor forces it off, because compositing the cursor is GDI. A
    // backend that cannot honour it simply runs the CPU path; the caller finds
    // out from gpuDevice() rather than being refused.
    bool           preferGpu = false;
};

class FrameSource {
public:
    virtual ~FrameSource() = default;

    virtual bool start() = 0;
    virtual void stop()  = 0;

    // Point the capture at a different target -- another monitor, say -- without
    // disturbing anything else in the session. The peer connection, the share
    // code and the viewer all stay exactly as they are; only the pixels change.
    //
    // On failure the previous target is restored, so a bad monitor index leaves
    // a working stream rather than a dead one -- and false is STILL returned.
    // The answer is "am I now on the target you asked for", not "am I alive":
    // a restored backend that reported success would stop the factory trying a
    // backend that could have served the new target, and would leave the caller
    // labelling the stream as a screen it is not showing.
    virtual bool retarget(const CaptureConfig& cfg) = 0;

    // Returns nullptr on a recoverable failure (e.g. the target window was
    // closed, or a secure-desktop transition). Callers should keep polling.
    virtual const Frame* capture() = 0;

    // The size the encoder should be configured for, after --max-width.
    virtual int width()  const = 0;
    virtual int height() const = 0;

    // The size frames actually come back at.
    virtual int nativeWidth()  const = 0;
    virtual int nativeHeight() const = 0;

    virtual std::string describe() const = 0;

    // Which API is doing the reading right now. Reported in --status so a black
    // picture can be diagnosed without a debugger.
    virtual CaptureBackend backend() const = 0;

    // Non-null only while this source is actually running the GPU pipeline, in
    // which case Frame::gpuTexture is live and Frame::data is not. The encoder
    // must be put on this same device or there is nothing zero-copy about it.
    virtual std::shared_ptr<GpuDevice> gpuDevice() const { return nullptr; }
};

// --- Backend naming ---------------------------------------------------------

const char* backendName(CaptureBackend backend);
bool        parseBackendName(std::string_view name, CaptureBackend& out);

// Shared by every backend: the blit is always 1:1 and --max-width only decides
// what the NV12 conversion pass downscales to, so the arithmetic belongs in one
// place. Both results are rounded down to even, which H.264 4:2:0 requires.
void computeEncodeSize(int srcW, int srcH, int maxWidth, int& outW, int& outH);

// --- Target enumeration, for --list-monitors / --list-windows ---------------

struct MonitorInfo {
    int         index = 0;
    int         x = 0, y = 0, width = 0, height = 0;
    bool        primary = false;
    std::string name;
    void*       handle = nullptr;   // HMONITOR, for Windows.Graphics.Capture
};

struct WindowInfo {
    void*       handle = nullptr;
    std::string title;
    std::string process;
    int         width = 0, height = 0;
};

std::vector<MonitorInfo> enumerateMonitors();
std::vector<WindowInfo>  enumerateWindows();

} // namespace soi
