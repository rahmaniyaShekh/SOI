#include "capture/WgcCapture.h"
#include "util/Log.h"

#include <d3d11.h>
#include <dwmapi.h>
#include <dxgi1_2.h>
#include <inspectable.h>
#include <roapi.h>
#include <winstring.h>

#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>

#include <algorithm>
#include <chrono>
#include <thread>

namespace soi {
namespace {

namespace wgc = ABI::Windows::Graphics::Capture;
namespace wgd = ABI::Windows::Graphics::DirectX;
using wgd::Direct3D11::IDirect3DDevice;
using wgd::Direct3D11::IDirect3DSurface;
using DxgiAccess = ::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess;

using Clock = std::chrono::steady_clock;

int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               Clock::now().time_since_epoch())
        .count();
}

// Windows.Graphics.Capture.IGraphicsCaptureSession3, Win11 22H2. Not in the
// 10.0.19041 SDK this builds against, and there is no other way to turn off the
// yellow capture border the OS draws around the captured item. Declaring it here
// is safe in both directions: on an older build QueryInterface just returns
// E_NOINTERFACE and the border stays.
MIDL_INTERFACE("f2cdd966-22ae-5ea1-9596-3a289344c3be")
IGraphicsCaptureSession3 : public IInspectable {
public:
    virtual HRESULT STDMETHODCALLTYPE get_IsBorderRequired(boolean* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE put_IsBorderRequired(boolean value) = 0;
};

// HSTRING is refcounted and RoGetActivationFactory needs one for every call.
class HStr {
public:
    explicit HStr(const wchar_t* s) {
        WindowsCreateString(s, static_cast<UINT32>(wcslen(s)), &h_);
    }
    ~HStr() { if (h_) WindowsDeleteString(h_); }
    HStr(const HStr&) = delete;
    HStr& operator=(const HStr&) = delete;
    HSTRING get() const { return h_; }
private:
    HSTRING h_{};
};

// WinRT activation needs an initialised apartment on the calling thread. main()
// already puts the process thread in the MTA; this is for every other one, and
// is deliberately never torn down -- the cost is a refcount and unwinding it
// correctly across the capture thread's lifetime buys nothing.
void ensureApartment() {
    static thread_local bool done = [] {
        const HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
        // RPC_E_CHANGED_MODE just means somebody already chose an STA here.
        // Free-threaded objects still activate; only the marshalling changes.
        if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
            logT("wgc: RoInitialize failed: {}", hrString(hr));
        return true;
    }();
    (void)done;
}

template <class T>
HRESULT activationFactory(const wchar_t* runtimeClass, ComPtr<T>& out) {
    ensureApartment();
    HStr name(runtimeClass);
    if (!name.get()) return E_OUTOFMEMORY;
    return RoGetActivationFactory(name.get(), __uuidof(T), out.putVoid());
}

void closeWinrt(IInspectable* obj) {
    if (!obj) return;
    ComPtr<ABI::Windows::Foundation::IClosable> closable;
    if (SUCCEEDED(obj->QueryInterface(__uuidof(ABI::Windows::Foundation::IClosable),
                                      closable.putVoid())))
        closable->Close();
}

} // namespace

// ---------------------------------------------------------------------------

struct WgcCapture::Impl {
    ComPtr<ID3D11Device>                 device;
    ComPtr<ID3D11DeviceContext>          context;
    ComPtr<IDirect3DDevice>              winrtDevice;
    ComPtr<wgc::IGraphicsCaptureItem>    item;
    ComPtr<wgc::IDirect3D11CaptureFramePool> pool;
    ComPtr<wgc::IGraphicsCaptureSession> session;
    ComPtr<ID3D11Texture2D>              staging;
    UINT                                 stagingW = 0, stagingH = 0;
    int                                  poolW = 0, poolH = 0;

    ~Impl() {
        if (session) closeWinrt(session.get());
        if (pool)    closeWinrt(pool.get());
    }
};

WgcCapture::WgcCapture(CaptureConfig cfg) : cfg_(cfg) {}
WgcCapture::~WgcCapture() { stop(); }

bool WgcCapture::available() {
    ComPtr<wgc::IGraphicsCaptureSessionStatics> statics;
    if (FAILED(activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession,
                                 statics)))
        return false;

    boolean supported = false;
    return SUCCEEDED(statics->IsSupported(&supported)) && supported;
}

// ---------------------------------------------------------------------------

bool WgcCapture::resolveTarget() {
    switch (cfg_.target) {
        case CaptureTarget::Monitor:
        case CaptureTarget::VirtualDesktop: {
            const auto mons = enumerateMonitors();
            if (mons.empty()) { logE("no monitors found"); return false; }

            int index = cfg_.monitorIndex;
            if (cfg_.target == CaptureTarget::VirtualDesktop) {
                // There is no capture item for "all displays". On the common
                // single-display machine the virtual desktop IS that display, so
                // --desktop is still serviceable here; with more than one it is
                // not, and the factory falls through to a backend that can.
                if (mons.size() != 1) {
                    logT("wgc: no capture item spans {} displays", mons.size());
                    return false;
                }
                index = 0;
            }

            if (index < 0 || index >= static_cast<int>(mons.size())) {
                logE("monitor index {} out of range (found {})", index, mons.size());
                return false;
            }
            const auto& m = mons[static_cast<size_t>(index)];
            srcX_ = m.x; srcY_ = m.y; srcW_ = m.width; srcH_ = m.height;
            description_ = soi::format("monitor {} ({}) {}x{}",
                                       m.index, m.name, srcW_, srcH_);
            return true;
        }

        case CaptureTarget::Window: {
            HWND wnd = static_cast<HWND>(cfg_.windowHandle);
            if (!wnd || !IsWindow(wnd)) { logE("target window is not valid"); return false; }

            RECT r{};
            if (FAILED(DwmGetWindowAttribute(wnd, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r))) ||
                (r.right - r.left) <= 0) {
                if (!GetWindowRect(wnd, &r)) { logE("GetWindowRect failed"); return false; }
            }
            srcX_ = 0; srcY_ = 0;
            srcW_ = r.right - r.left;
            srcH_ = r.bottom - r.top;

            wchar_t title[256] = {};
            GetWindowTextW(wnd, title, 256);
            description_ = soi::format("window '{}' {}x{}", toUtf8(title), srcW_, srcH_);
            return true;
        }
    }
    return false;
}

bool WgcCapture::rebuildPool(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    ComPtr<wgc::IDirect3D11CaptureFramePoolStatics2> statics;
    if (FAILED(activationFactory(
            RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool, statics)))
        return false;

    const ABI::Windows::Graphics::SizeInt32 size{w, h};

    if (impl_->pool) {
        // Recreate keeps the existing session attached, which matters: tearing
        // the session down and starting a new one makes the OS re-evaluate the
        // capture and flashes the border on Win11.
        if (FAILED(impl_->pool->Recreate(impl_->winrtDevice.get(),
                                         wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                         2, size)))
            return false;
    } else {
        // Free-threaded so TryGetNextFrame can be polled straight from the
        // capture loop; the dispatcher-bound variant would need a message pump
        // this process does not have.
        if (FAILED(statics->CreateFreeThreaded(impl_->winrtDevice.get(),
                                               wgd::DirectXPixelFormat_B8G8R8A8UIntNormalized,
                                               2, size, impl_->pool.put())))
            return false;
    }

    impl_->poolW = w;
    impl_->poolH = h;

    if (!buffer_.resize(w, h)) return false;
    buffer_.fillBlack();
    return true;
}

bool WgcCapture::start() {
    if (started_) return true;
    if (!available()) {
        logT("wgc: Windows.Graphics.Capture is not supported on this machine");
        return false;
    }
    if (!resolveTarget()) return false;

    impl_ = std::make_unique<Impl>();

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, impl_->device.put(), nullptr,
                                   impl_->context.put());
    if (FAILED(hr)) {
        // WARP is slow but it is still a working capture on a machine with no
        // usable 3D driver, which is exactly where the other backends struggle.
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
                               D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                               D3D11_SDK_VERSION, impl_->device.put(), nullptr,
                               impl_->context.put());
        if (FAILED(hr)) {
            logT("wgc: D3D11CreateDevice failed: {}", hrString(hr));
            stop();
            return false;
        }
        logW("wgc: no hardware D3D11 device; falling back to the WARP renderer");
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(impl_->device.as(dxgiDevice))) { stop(); return false; }

    ComPtr<IInspectable> inspectable;
    hr = CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.get(), inspectable.put());
    if (FAILED(hr) || FAILED(inspectable.as(impl_->winrtDevice))) {
        logT("wgc: CreateDirect3D11DeviceFromDXGIDevice failed: {}", hrString(hr));
        stop();
        return false;
    }

    ComPtr<IGraphicsCaptureItemInterop> interop;
    if (FAILED(activationFactory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem,
                                 interop))) {
        stop();
        return false;
    }

    if (cfg_.target == CaptureTarget::Window) {
        hr = interop->CreateForWindow(static_cast<HWND>(cfg_.windowHandle),
                                      __uuidof(wgc::IGraphicsCaptureItem),
                                      impl_->item.putVoid());
    } else {
        const auto mons = enumerateMonitors();
        const int  index = cfg_.target == CaptureTarget::VirtualDesktop ? 0 : cfg_.monitorIndex;
        HMONITOR   mon = nullptr;
        if (index >= 0 && index < static_cast<int>(mons.size()))
            mon = static_cast<HMONITOR>(mons[static_cast<size_t>(index)].handle);
        if (!mon) { logE("wgc: no HMONITOR for monitor {}", index); stop(); return false; }

        hr = interop->CreateForMonitor(mon, __uuidof(wgc::IGraphicsCaptureItem),
                                       impl_->item.putVoid());
    }
    if (FAILED(hr) || !impl_->item) {
        logT("wgc: could not create a capture item: {}", hrString(hr));
        stop();
        return false;
    }

    // The item knows the real capture size -- for a window that is the composed
    // bounds, which is not the same as either GetWindowRect or the DWM frame.
    ABI::Windows::Graphics::SizeInt32 itemSize{};
    if (SUCCEEDED(impl_->item->get_Size(&itemSize)) && itemSize.Width > 0 && itemSize.Height > 0) {
        srcW_ = itemSize.Width;
        srcH_ = itemSize.Height;
    }

    computeEncodeSize(srcW_, srcH_, cfg_.maxWidth, outW_, outH_);
    if (outW_ < 16 || outH_ < 16) { logE("capture output too small"); stop(); return false; }

    if (!rebuildPool(srcW_, srcH_)) {
        logT("wgc: could not create a {}x{} frame pool", srcW_, srcH_);
        stop();
        return false;
    }

    if (FAILED(impl_->pool->CreateCaptureSession(impl_->item.get(), impl_->session.put()))) {
        logT("wgc: CreateCaptureSession failed");
        stop();
        return false;
    }

    // WGC draws the cursor itself, correctly scaled and masked, which is both
    // cheaper and more accurate than compositing one with DrawIconEx.
    ComPtr<wgc::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(impl_->session.as(session2))) {
        session2->put_IsCursorCaptureEnabled(cfg_.captureCursor ? TRUE : FALSE);
    } else if (!cfg_.captureCursor) {
        logT("wgc: IsCursorCaptureEnabled needs Windows 10 2004+; the cursor will "
             "be in the stream");
    }

    ComPtr<IGraphicsCaptureSession3> session3;
    if (SUCCEEDED(impl_->session.as(session3)))
        session3->put_IsBorderRequired(FALSE);

    if (FAILED(impl_->session->StartCapture())) {
        logT("wgc: StartCapture failed");
        stop();
        return false;
    }

    // Same reasoning as the duplication backend: frames only arrive when the
    // content is presented, so confirm one really does before claiming the
    // backend works. Failing here lets the factory move on instead of the viewer
    // getting a black rectangle.
    const auto deadline = Clock::now() + std::chrono::milliseconds(500);
    while (!primed_ && Clock::now() < deadline) {
        if (capture()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    if (!primed_) {
        logT("wgc: no frame arrived within 500 ms");
        stop();
        return false;
    }

    started_ = true;
    logI("capture: {} via Windows.Graphics.Capture -> {}x{}{}",
         description_, outW_, outH_,
         (outW_ != srcW_ || outH_ != srcH_) ? " (downscaled during conversion)" : "");
    return true;
}

void WgcCapture::stop() {
    impl_.reset();
    buffer_.release();
    started_ = false;
    primed_  = false;
}

bool WgcCapture::retarget(const CaptureConfig& cfg) {
    const CaptureConfig previous = cfg_;

    stop();
    cfg_ = cfg;
    if (start()) return true;

    // Restore, then report failure regardless. This is the case that matters
    // most in practice: WGC has no capture item spanning several displays, so a
    // viewer picking "all screens" while WGC is live MUST fall through to
    // duplication rather than be told the switch succeeded.
    logE("wgc: could not switch capture target; restoring the previous one");
    stop();
    cfg_ = previous;
    if (!start()) logE("wgc: the previous capture target did not come back either");
    return false;
}

const Frame* WgcCapture::capture() {
    if (!impl_ || !impl_->pool) return nullptr;

    // A window that has gone away produces no frames and never will. Say so,
    // rather than repeating its last picture forever.
    if (cfg_.target == CaptureTarget::Window &&
        !IsWindow(static_cast<HWND>(cfg_.windowHandle)))
        return nullptr;

    // A resize noticed last time round. Doing it here rather than there is what
    // keeps the frame we returned last call valid for as long as we promised.
    if (pendingW_ > 0 && pendingH_ > 0) {
        const int w = std::exchange(pendingW_, 0);
        const int h = std::exchange(pendingH_, 0);
        logT("wgc: content resized to {}x{}; recreating the frame pool", w, h);
        if (rebuildPool(w, h)) { srcW_ = w; srcH_ = h; }
    }

    // Drain: the pool buffers frames, and after any hitch there can be several
    // waiting. Only the newest is worth encoding.
    ComPtr<wgc::IDirect3D11CaptureFrame> frame;
    bool                                 got = false;
    int                                  contentW = 0, contentH = 0;

    for (;;) {
        ComPtr<wgc::IDirect3D11CaptureFrame> next;
        if (FAILED(impl_->pool->TryGetNextFrame(next.put())) || !next) break;
        if (frame) closeWinrt(frame.get());
        frame = next;
    }

    if (frame) {
        ABI::Windows::Graphics::SizeInt32 content{};
        if (SUCCEEDED(frame->get_ContentSize(&content))) {
            contentW = content.Width;
            contentH = content.Height;
        }

        ComPtr<IDirect3DSurface> surface;
        ComPtr<DxgiAccess>       access;
        ComPtr<ID3D11Texture2D>  tex;

        if (SUCCEEDED(frame->get_Surface(surface.put())) &&
            SUCCEEDED(surface.as(access)) &&
            SUCCEEDED(access->GetInterface(__uuidof(ID3D11Texture2D), tex.putVoid()))) {

            D3D11_TEXTURE2D_DESC td{};
            tex->GetDesc(&td);

            if (!impl_->staging || td.Width != impl_->stagingW ||
                td.Height != impl_->stagingH) {
                D3D11_TEXTURE2D_DESC sd{};
                sd.Width            = td.Width;
                sd.Height           = td.Height;
                sd.MipLevels        = 1;
                sd.ArraySize        = 1;
                sd.Format           = td.Format;
                sd.SampleDesc.Count = 1;
                sd.Usage            = D3D11_USAGE_STAGING;
                sd.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;

                impl_->staging.reset();
                if (SUCCEEDED(impl_->device->CreateTexture2D(&sd, nullptr,
                                                             impl_->staging.put()))) {
                    impl_->stagingW = td.Width;
                    impl_->stagingH = td.Height;
                } else {
                    impl_->staging.reset();
                }
            }

            if (impl_->staging && td.Format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                impl_->context->CopyResource(impl_->staging.get(), tex.get());

                D3D11_MAPPED_SUBRESOURCE mapped{};
                if (SUCCEEDED(impl_->context->Map(impl_->staging.get(), 0,
                                                  D3D11_MAP_READ, 0, &mapped))) {
                    // The surface is pool-sized; ContentSize is the part of it
                    // that is real. Copying the whole thing would pull in the
                    // uninitialised margin left after a window shrinks.
                    const int cw = contentW > 0
                                       ? std::min<int>(contentW, static_cast<int>(td.Width))
                                       : static_cast<int>(td.Width);
                    const int ch = contentH > 0
                                       ? std::min<int>(contentH, static_cast<int>(td.Height))
                                       : static_cast<int>(td.Height);
                    buffer_.blitIn(static_cast<const uint8_t*>(mapped.pData),
                                   static_cast<int>(mapped.RowPitch), 0, 0, cw, ch);
                    impl_->context->Unmap(impl_->staging.get(), 0);
                    got     = true;
                    primed_ = true;
                }
            }
        }

        closeWinrt(frame.get());
        frame.reset();
    }

    if (!primed_) return nullptr;

    // A resized window changes ContentSize. Queue a re-pool at the new size; the
    // frame just copied is clipped to the old one and still good to send.
    if (contentW > 0 && contentH > 0 &&
        (contentW != impl_->poolW || contentH != impl_->poolH)) {
        pendingW_ = contentW;
        pendingH_ = contentH;
    }

    frame_.data      = buffer_.pixels();
    frame_.width     = buffer_.width();
    frame_.height    = buffer_.height();
    frame_.stride    = buffer_.stride();
    frame_.timeNs    = nowNs();
    // No hashing: the frame pool only produces a frame when the content was
    // presented, which is the same question, answered by the compositor.
    frame_.duplicate = cfg_.detectDuplicates && !got;
    return &frame_;
}

} // namespace soi
