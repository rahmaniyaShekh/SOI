#include "capture/DxgiCapture.h"
#include "util/Log.h"

#include <d3d11.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

using Clock = std::chrono::steady_clock;

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

const D3D_FEATURE_LEVEL kFeatureLevels[] = {
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
};

bool rectsIntersect(const RECT& a, const RECT& b) {
    return a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom;
}

const char* rotationName(DXGI_MODE_ROTATION r) {
    switch (r) {
        case DXGI_MODE_ROTATION_ROTATE90:  return "90";
        case DXGI_MODE_ROTATION_ROTATE180: return "180";
        case DXGI_MODE_ROTATION_ROTATE270: return "270";
        default: return "0";
    }
}

// DuplicateOutput's failures are not interchangeable and the difference decides
// whether retrying is worth anything.
const char* duplicationFailure(HRESULT hr) {
    if (hr == E_ACCESSDENIED)
        return "access denied -- a secure desktop (UAC / Ctrl+Alt+Del) is up";
    if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE)
        return "the per-machine limit on concurrent duplications is reached";
    if (hr == DXGI_ERROR_UNSUPPORTED)
        return "this adapter does not support duplication (RDP session, or a "
               "hybrid-GPU configuration where the output belongs elsewhere)";
    if (hr == DXGI_ERROR_SESSION_DISCONNECTED)
        return "the session is disconnected";
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// One duplicated display.
// ---------------------------------------------------------------------------
struct DxgiCapture::Output {
    ComPtr<IDXGIAdapter1>          adapter;
    ComPtr<IDXGIOutput1>           output;
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGIOutputDuplication> dup;
    ComPtr<ID3D11Texture2D>        staging;
    UINT                           stagingW = 0, stagingH = 0;
    DXGI_FORMAT                    stagingFormat = DXGI_FORMAT_UNKNOWN;

    int          dstX = 0, dstY = 0;    // offset within the composed buffer
    int          w = 0, h = 0;          // desktop-space size
    std::wstring name;

    Clock::time_point retryAt{};
    bool              primed = false;   // has ever delivered a frame
};

// ---------------------------------------------------------------------------

DxgiCapture::DxgiCapture(CaptureConfig cfg) : cfg_(cfg) {}
DxgiCapture::~DxgiCapture() { stop(); }

bool DxgiCapture::available() {
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.putVoid())))
        return false;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.as(output1))) continue;

            ComPtr<ID3D11Device>        device;
            ComPtr<ID3D11DeviceContext> context;
            if (FAILED(D3D11CreateDevice(adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                         kFeatureLevels, ARRAYSIZE(kFeatureLevels),
                                         D3D11_SDK_VERSION, device.put(), nullptr,
                                         context.put())))
                continue;

            ComPtr<IDXGIOutputDuplication> dup;
            if (SUCCEEDED(output1->DuplicateOutput(device.get(), dup.put())))
                return true;
        }
    }
    return false;
}

// ---------------------------------------------------------------------------

bool DxgiCapture::resolveTarget() {
    switch (cfg_.target) {
        case CaptureTarget::VirtualDesktop:
            srcX_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
            srcY_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
            srcW_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
            srcH_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
            description_ = soi::format("virtual desktop {}x{} at ({},{})",
                                       srcW_, srcH_, srcX_, srcY_);
            break;

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

        case CaptureTarget::Window:
            // Duplication has no notion of a window. The factory never routes a
            // window target here; this is the guard for anyone who does.
            logT("dxgi: window targets are not duplicable");
            return false;
    }

    if (srcW_ <= 0 || srcH_ <= 0) { logE("capture target has zero size"); return false; }
    return true;
}

bool DxgiCapture::collectOutputs() {
    outputs_.clear();

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1), factory.putVoid());
    if (FAILED(hr)) {
        logT("dxgi: CreateDXGIFactory1 failed: {}", hrString(hr));
        return false;
    }

    const RECT want{srcX_, srcY_, srcX_ + srcW_, srcY_ + srcH_};

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapters1(i, adapter.put()) != DXGI_ERROR_NOT_FOUND; ++i) {
        ComPtr<IDXGIOutput> output;
        for (UINT j = 0; adapter->EnumOutputs(j, output.put()) != DXGI_ERROR_NOT_FOUND; ++j) {
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || !desc.AttachedToDesktop) continue;
            if (!rectsIntersect(desc.DesktopCoordinates, want)) continue;

            // A rotated display duplicates into a surface in the panel's native
            // orientation, which would need a per-pixel transform on the way
            // out. Rather than ship a rotation that has never been run against
            // a rotated panel, decline and let the factory pick WGC, which
            // composes in desktop orientation and gets it right for free.
            if (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED) {
                logI("dxgi: {} is rotated {} degrees; duplication declines rotated "
                     "outputs, falling back",
                     toUtf8(desc.DeviceName), rotationName(desc.Rotation));
                outputs_.clear();
                return false;
            }

            ComPtr<IDXGIOutput1> output1;
            if (FAILED(output.as(output1))) continue;

            auto o      = std::make_unique<Output>();
            o->adapter  = adapter;
            o->output   = output1;
            o->name     = desc.DeviceName;
            o->w        = desc.DesktopCoordinates.right  - desc.DesktopCoordinates.left;
            o->h        = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            o->dstX     = desc.DesktopCoordinates.left - srcX_;
            o->dstY     = desc.DesktopCoordinates.top  - srcY_;
            outputs_.push_back(std::move(o));
        }
    }

    if (outputs_.empty()) {
        logT("dxgi: no attached output overlaps the target rectangle");
        return false;
    }
    return true;
}

// Every reason this returns false is an ordinary property of the machine or the
// options, not a fault, so it logs at trace and the caller carries on with the
// CPU path. --gpu is what turns "we quietly did not" into an error, and it does
// that in main.cpp where the user's intent is known.
bool DxgiCapture::setUpGpu() {
    gpu_.reset();
    gpuDesktop_.reset();

    if (!cfg_.preferGpu) return false;

    // The cursor is drawn with GDI onto CPU pixels. Keeping the frame on the
    // card means there are no CPU pixels, so the two are mutually exclusive and
    // the explicit request wins.
    if (cfg_.captureCursor) {
        logI("gpu: --cursor needs CPU pixels to draw on; using the CPU pipeline");
        return false;
    }

    if (outputs_.empty()) return false;

    // One device can only duplicate outputs on its own adapter. Spanning two
    // adapters would need a shared texture and a cross-adapter copy, which costs
    // about what the readback we are avoiding costs.
    LUID first{};
    for (size_t i = 0; i < outputs_.size(); ++i) {
        DXGI_ADAPTER_DESC ad{};
        if (!outputs_[i]->adapter || FAILED(outputs_[i]->adapter->GetDesc(&ad))) return false;
        if (i == 0) { first = ad.AdapterLuid; continue; }
        if (ad.AdapterLuid.LowPart != first.LowPart ||
            ad.AdapterLuid.HighPart != first.HighPart) {
            logI("gpu: this target spans two graphics adapters; using the CPU pipeline");
            return false;
        }
    }

    auto dev = GpuDevice::create(outputs_[0]->adapter.get());
    if (!dev) return false;

    // The composed desktop, at native size. SHADER_RESOURCE because the NV12
    // shader reads it, RENDER_TARGET only so start() can clear it once -- the
    // outputs themselves are blitted in with CopySubresourceRegion, not drawn.
    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(srcW_);
    td.Height           = static_cast<UINT>(srcH_);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    td.BindFlags        = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;

    ComPtr<ID3D11Texture2D> composed;
    if (FAILED(dev->device()->CreateTexture2D(&td, nullptr, composed.put()))) {
        logT("dxgi: could not create the {}x{} composed GPU texture", srcW_, srcH_);
        return false;
    }

    gpu_        = std::move(dev);
    gpuDesktop_ = std::move(composed);
    logI("gpu: capture stays on {} ({}x{} BGRA, {} output(s))",
         gpu_->describe(), srcW_, srcH_, outputs_.size());
    return true;
}

bool DxgiCapture::openDuplication(Output& o) {
    o.dup.reset();
    o.staging.reset();
    o.stagingW = o.stagingH = 0;
    o.stagingFormat = DXGI_FORMAT_UNKNOWN;

    // On the GPU pipeline every output duplicates onto the ONE shared device, so
    // its texture and the encoder's are on the same device and can be handed
    // over without a copy. That is the whole reason the device is shared.
    if (gpu_) {
        o.device.attach(gpu_->device());
        o.device->AddRef();
        o.context.attach(gpu_->context());
        o.context->AddRef();
    }

    if (!o.device) {
        HRESULT hr = D3D11CreateDevice(o.adapter.get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                       D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                       kFeatureLevels, ARRAYSIZE(kFeatureLevels),
                                       D3D11_SDK_VERSION, o.device.put(), nullptr,
                                       o.context.put());
        if (FAILED(hr)) {
            logT("dxgi: D3D11CreateDevice failed for {}: {}",
                 toUtf8(o.name), hrString(hr));
            o.device.reset();
            o.context.reset();
            return false;
        }
    }

    // DuplicateOutput1 lets us pin BGRA8, which is what keeps an HDR display
    // from handing back float16 the rest of the pipeline cannot read. It needs
    // the process to be per-monitor DPI aware; DuplicateOutput is the fallback
    // when it is not.
    HRESULT hr = E_FAIL;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(o.output.as(output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(o.device.get(), 0, ARRAYSIZE(formats), formats,
                                       o.dup.put());
    }
    if (FAILED(hr))
        hr = o.output->DuplicateOutput(o.device.get(), o.dup.put());

    if (FAILED(hr)) {
        o.dup.reset();
        const char* why = duplicationFailure(hr);
        logT("dxgi: DuplicateOutput failed for {}: {}{}{}",
             toUtf8(o.name), hrString(hr), *why ? " -- " : "", why);
        return false;
    }

    o.primed = false;
    return true;
}

// ---------------------------------------------------------------------------

namespace {
// AcquireNextFrame must be matched by exactly one ReleaseFrame, and the frame's
// resources have to go first. Every exit path from pumpOutput runs through this.
struct FrameGuard {
    IDXGIOutputDuplication*  dup;
    ComPtr<IDXGIResource>*   res;
    ComPtr<ID3D11Texture2D>* tex;
    ~FrameGuard() {
        tex->reset();
        res->reset();
        dup->ReleaseFrame();
    }
};
} // namespace

bool DxgiCapture::pumpOutput(Output& o, int timeoutMs) {
    DXGI_OUTDUPL_FRAME_INFO info{};
    ComPtr<IDXGIResource>   res;

    HRESULT hr = o.dup->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info, res.put());
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return false;
    if (FAILED(hr)) {
        // ACCESS_LOST is routine: a mode change, a fullscreen transition or a
        // trip through the secure desktop all invalidate the duplication. The
        // caller reopens it.
        logT("dxgi: AcquireNextFrame on {} failed: {}", toUtf8(o.name), hrString(hr));
        o.dup.reset();
        return false;
    }

    ComPtr<ID3D11Texture2D> tex;
    FrameGuard guard{o.dup.get(), &res, &tex};

    // LastPresentTime == 0 means no desktop update came with this frame, only a
    // pointer move -- and the pointer is not in the image. It is tempting to make
    // an exception for the very first acquire, on the grounds that it ought to
    // carry the current desktop. It does not: the first frame after
    // DuplicateOutput comes back with AccumulatedFrames == 0 and a surface that
    // is entirely black, every time. Taking it is how you ship a black stream.
    if (info.LastPresentTime.QuadPart == 0) return false;

    if (FAILED(res.as(tex))) return false;

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    // Not on the GPU path: there is nothing to read back, so there is nothing to
    // stage. Allocating one anyway would cost a full-frame surface per output
    // for no reason.
    if (!gpu_ && (!o.staging || td.Width != o.stagingW || td.Height != o.stagingH ||
                  td.Format != o.stagingFormat)) {
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width          = td.Width;
        sd.Height         = td.Height;
        sd.MipLevels      = 1;
        sd.ArraySize      = 1;
        sd.Format         = td.Format;
        sd.SampleDesc.Count = 1;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

        o.staging.reset();
        if (FAILED(o.device->CreateTexture2D(&sd, nullptr, o.staging.put()))) {
            logT("dxgi: staging texture {}x{} failed for {}",
                 td.Width, td.Height, toUtf8(o.name));
            o.staging.reset();
            return false;
        }
        o.stagingW      = td.Width;
        o.stagingH      = td.Height;
        o.stagingFormat = td.Format;
    }

    // Anything other than BGRA8 would need a conversion pass. Pinning the format
    // in DuplicateOutput1 means this should not happen; refusing loudly beats
    // shipping scrambled colour.
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        logT("dxgi: {} duplicates as format {}, not BGRA8",
             toUtf8(o.name), static_cast<int>(td.Format));
        return false;
    }

    // --- the GPU path: stay on the card ------------------------------------
    //
    // Blit this output into its own rectangle of the composed desktop texture
    // and stop. No staging texture, no Map, nothing across the bus. The copy
    // itself is not avoidable: ReleaseFrame (the FrameGuard above) invalidates
    // the acquired texture the moment this returns, so its pixels have to live
    // somewhere we own.
    if (gpu_ && gpuDesktop_) {
        const UINT w = std::min<UINT>(td.Width,  static_cast<UINT>(o.w));
        const UINT h = std::min<UINT>(td.Height, static_cast<UINT>(o.h));
        const D3D11_BOX box{0, 0, 0, w, h, 1};

        std::lock_guard lk(gpu_->contextMutex());
        o.context->CopySubresourceRegion(gpuDesktop_.get(), 0,
                                         static_cast<UINT>(o.dstX),
                                         static_cast<UINT>(o.dstY), 0,
                                         tex.get(), 0, &box);
        o.primed = true;
        return true;
    }

    o.context->CopyResource(o.staging.get(), tex.get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(o.context->Map(o.staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;

    desktop_.blitIn(static_cast<const uint8_t*>(mapped.pData),
                    static_cast<int>(mapped.RowPitch),
                    o.dstX, o.dstY,
                    std::min<int>(o.w, static_cast<int>(td.Width)),
                    std::min<int>(o.h, static_cast<int>(td.Height)));

    o.context->Unmap(o.staging.get(), 0);
    o.primed = true;
    return true;
}

// ---------------------------------------------------------------------------

bool DxgiCapture::start() {
    if (started_) return true;
    if (!resolveTarget()) return false;

    computeEncodeSize(srcW_, srcH_, cfg_.maxWidth, outW_, outH_);
    if (outW_ < 16 || outH_ < 16) { logE("capture output too small"); return false; }

    if (!collectOutputs()) return false;

    // Before the duplications open, because it decides which device they open
    // on. Failure here is not failure to start -- it just means the CPU path.
    setUpGpu();

    // The CPU-side surfaces are pure waste on the GPU path: nothing ever reads
    // or writes them.
    if (!gpu_ && !desktop_.resize(srcW_, srcH_)) {
        logE("dxgi: could not allocate a {}x{} frame buffer", srcW_, srcH_);
        return false;
    }
    // A virtual desktop is not necessarily a rectangle -- two monitors of
    // different heights leave a gap that no output covers. Black is the honest
    // value for it, and clearing once means those pixels are never garbage.
    //
    // The GPU texture needs the same treatment for the same reason, and D3D
    // gives no guarantee about a freshly created texture's contents. Clearing it
    // needs a render target view, so it is done once here rather than kept
    // around.
    if (gpu_) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format        = DXGI_FORMAT_B8G8R8A8_UNORM;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> clearView;
        if (SUCCEEDED(gpu_->device()->CreateRenderTargetView(gpuDesktop_.get(), &rv,
                                                             clearView.put()))) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            std::lock_guard lk(gpu_->contextMutex());
            gpu_->context()->ClearRenderTargetView(clearView.get(), black);
        }
    } else {
        desktop_.fillBlack();
    }

    if (cfg_.captureCursor && !composed_.resize(srcW_, srcH_)) {
        logE("dxgi: could not allocate the cursor compositing buffer");
        return false;
    }

    int opened = 0;
    for (auto& o : outputs_)
        if (openDuplication(*o)) ++opened;

    if (!opened) {
        logT("dxgi: no output could be duplicated");
        stop();
        return false;
    }

    // Duplication only yields on change, so a machine sitting at a static
    // desktop can hand back WAIT_TIMEOUT indefinitely. Wait here for the first
    // real frame instead of streaming black and hoping: if it does not arrive,
    // saying so now lets the factory move to the next backend before the viewer
    // ever connects.
    //
    // A second is generous on purpose. DWM's idle heartbeat measures around
    // 500 ms between presents on a desktop nobody is touching, and giving up
    // early would cost the one backend that can see fullscreen content.
    const auto deadline = Clock::now() + std::chrono::milliseconds(1200);
    int primed = 0;
    for (;;) {
        primed = 0;
        int waiting = 0;
        for (auto& o : outputs_) {
            if (!o->dup) continue;
            if (o->primed) { ++primed; continue; }
            ++waiting;
            if (pumpOutput(*o, 50)) ++primed;
        }
        // `waiting` also guards against spinning: an output that lost its
        // duplication mid-loop no longer blocks in AcquireNextFrame, so without
        // this the deadline would be burned at full speed.
        if (!waiting || Clock::now() >= deadline) break;
    }

    if (!primed) {
        logT("dxgi: duplication produced no frame within 500 ms");
        stop();
        return false;
    }

    started_ = true;
    logI("capture: {} via Desktop Duplication ({}/{} output(s) live) -> {}x{}{}",
         description_, primed, static_cast<int>(outputs_.size()), outW_, outH_,
         (outW_ != srcW_ || outH_ != srcH_) ? " (downscaled during conversion)" : "");
    return true;
}

void DxgiCapture::stop() {
    // Outputs first: each holds a reference to the shared device, and the
    // duplications must be gone before the device they were made on.
    outputs_.clear();
    gpuDesktop_.reset();
    gpu_.reset();
    desktop_.release();
    composed_.release();
    started_ = false;
    frame_ = Frame{};
    lastCursor_ = POINT{-1, -1};
    lastCursorShown_ = false;
}

bool DxgiCapture::retarget(const CaptureConfig& cfg) {
    const CaptureConfig previous = cfg_;

    stop();
    cfg_ = cfg;
    if (start()) return true;

    // Restore, then report failure regardless -- see BitBltCapture::retarget for
    // why the answer is "am I on the requested target", not "am I alive".
    logE("dxgi: could not switch capture target; restoring the previous one");
    stop();
    cfg_ = previous;
    if (!start()) logE("dxgi: the previous capture target did not come back either");
    return false;
}

const Frame* DxgiCapture::capture() {
    if (!started_) return nullptr;

    const auto now = Clock::now();
    bool changed = false;
    int  live = 0, primed = 0;

    for (auto& up : outputs_) {
        Output& o = *up;

        if (!o.dup) {
            if (now < o.retryAt) continue;
            if (!openDuplication(o)) {
                // A secure desktop lasts as long as the user takes to click, so
                // there is no point hammering it.
                o.retryAt = now + std::chrono::milliseconds(500);
                continue;
            }
            changed = true;
        }
        ++live;

        // An output that has never delivered anything gets a real wait: its
        // region of the buffer is still black and the cost of blocking is
        // nothing compared to streaming a hole.
        if (pumpOutput(o, o.primed ? 0 : 30)) changed = true;
        if (!o.dup) o.retryAt = now + std::chrono::milliseconds(250);
        if (o.primed) ++primed;
    }

    // Every duplication is gone -- a driver reset, a disconnected session. Say
    // so rather than repeating a stale frame; the resilient wrapper decides
    // whether to wait it out or change backend.
    if (!live || !primed) return nullptr;

    // The GPU path: the composed texture IS the frame. No pixels come back, so
    // there is no cursor to composite (setUpGpu refused --cursor) and no hash to
    // take -- duplication already told us whether anything changed, which is
    // both cheaper and exact.
    if (gpu_) {
        frame_.data       = nullptr;
        frame_.gpuTexture = gpuDesktop_.get();
        frame_.width      = srcW_;
        frame_.height     = srcH_;
        frame_.stride     = 0;
        frame_.timeNs     = nowNs();
        frame_.duplicate  = cfg_.detectDuplicates && !changed;
        return &frame_;
    }

    const uint8_t* pixels = desktop_.pixels();
    int            stride = desktop_.stride();

    if (cfg_.captureCursor) {
        CURSORINFO ci{};
        ci.cbSize = sizeof(ci);
        const bool shown = GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) && ci.hCursor;
        const bool moved = shown && (ci.ptScreenPos.x != lastCursor_.x ||
                                     ci.ptScreenPos.y != lastCursor_.y);
        if (shown != lastCursorShown_ || moved) changed = true;
        lastCursorShown_ = shown;
        if (shown) lastCursor_ = ci.ptScreenPos;

        // Recomposite whenever anything moved. Drawing onto a fresh copy is what
        // keeps the cursor from smearing across outputs that did not update.
        if (changed) {
            std::memcpy(composed_.pixels(), desktop_.pixels(),
                        static_cast<size_t>(stride) * static_cast<size_t>(srcH_));
            compositeCursor(composed_.dc(), srcX_, srcY_);
            FrameBuffer::flushGdi();
        }
        pixels = composed_.pixels();
        stride = composed_.stride();
    }

    frame_.data       = pixels;
    frame_.gpuTexture = nullptr;
    frame_.width     = srcW_;
    frame_.height    = srcH_;
    frame_.stride    = stride;
    frame_.timeNs    = nowNs();
    // No hashing: duplication reports content change exactly, for free.
    frame_.duplicate = cfg_.detectDuplicates && !changed;
    return &frame_;
}

} // namespace soi
