#pragma once
//
// The GPU pipeline: capture, colour-convert and encode without the frame ever
// leaving the graphics card.
//
// What the CPU path does today, per frame at 1920x1200:
//
//   duplication texture (GPU) -> staging texture -> Map -> ~9 MB read back
//   across the bus -> BGRA->NV12 on the CPU (SSE2, several threads) -> ~3.5 MB
//   of system memory handed to the encoder -> the hardware MFT uploads it to the
//   GPU again.
//
// Two full-frame bus crossings and a conversion pass, for pixels that started
// and ended on the same device. This module removes all three: Desktop
// Duplication's texture is composed into a BGRA texture, a pixel shader writes
// BT.709 limited-range NV12 straight into the two planes of an NV12 texture, and
// that texture goes to Media Foundation through an IMFDXGIDeviceManager. Nothing
// is ever mapped; the only thing that crosses the bus is the compressed
// bitstream.
//
// The idea is the same one RustFrame uses for its mirror window -- keep the
// capture texture on the GPU and let a shader do the work -- with the difference
// that our destination is an encoder rather than a swapchain, so the shader
// writes NV12 instead of presenting.
//
// What it costs, and why the CPU path is still here:
//
//   * The frame is no longer visible to the CPU, so anything that needs pixels
//     has to go. Cursor compositing is GDI, and the duplicate-frame hash reads
//     every Nth byte -- neither is possible without a readback that would undo
//     the point. Duplicate detection instead trusts what duplication already
//     tells us (nothing presented => nothing changed), which is both cheaper and
//     more accurate; the cursor is simply unavailable, and --cursor falls back.
//   * It needs one D3D11 device shared by capture and encoder, so every output
//     must be on one adapter and the encoder MFT must be D3D11-aware.
//
// Neither condition is checkable up front with any confidence, so nothing here
// asserts: every entry point reports failure and the caller uses the CPU path.
// --gpu turns that silence into a hard error, for when you want to know.
//
#include "util/Win.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct ID3D11Texture2D;
struct ID3D11PixelShader;
struct ID3D11VertexShader;
struct ID3D11SamplerState;
struct ID3D11Buffer;
struct ID3D11RenderTargetView;
struct ID3D11ShaderResourceView;
struct IDXGIAdapter1;

namespace soi {

// ---------------------------------------------------------------------------
// Which pipeline to run. Selected once at startup and never changed, because
// the two paths have different capabilities rather than different speeds --
// switching mid-session would make the cursor appear and disappear.
// ---------------------------------------------------------------------------
enum class Pipeline {
    Auto,   // GPU where every precondition holds, CPU otherwise. The default.
    Gpu,    // GPU or nothing: refuse to start rather than quietly fall back.
    Cpu,    // the SSE2 path, always. What --cursor needs, and the way to prove
            // a GPU-path bug is a GPU-path bug.
};

const char* pipelineName(Pipeline p);
bool        parsePipelineName(std::string_view name, Pipeline& out);

// ---------------------------------------------------------------------------
// One D3D11 device, shared by the duplication, the NV12 shader and the encoder.
//
// Sharing is not an optimisation here, it is the requirement: a texture can only
// be handed to the encoder without a copy if the encoder is on the same device.
// Multithread protection is switched on because Media Foundation's encoder
// thread and our capture thread both end up in the immediate context.
// ---------------------------------------------------------------------------
class GpuDevice {
public:
    // `adapter` is the one that owns the duplicated output; passing the wrong
    // one produces a device that cannot duplicate. Null means "let D3D pick",
    // which is only right when there is nothing to match.
    static std::shared_ptr<GpuDevice> create(IDXGIAdapter1* adapter);

    ~GpuDevice();
    GpuDevice(const GpuDevice&) = delete;
    GpuDevice& operator=(const GpuDevice&) = delete;

    ID3D11Device*        device()  const { return device_.get(); }
    ID3D11DeviceContext* context() const { return context_.get(); }
    LUID                 adapterLuid() const { return luid_; }
    const std::string&   describe() const { return description_; }

    // Held across any sequence of context calls that must not interleave with
    // another thread's. ID3D11Multithread makes each individual call safe; it
    // does not make "set six states then draw" safe.
    std::mutex& contextMutex() { return contextMtx_; }

private:
    GpuDevice() = default;

    ComPtr<ID3D11Device>        device_;
    ComPtr<ID3D11DeviceContext> context_;
    LUID                        luid_{};
    std::string                 description_;
    std::mutex                  contextMtx_;
};

// ---------------------------------------------------------------------------
// BGRA texture -> NV12 texture, entirely on the GPU.
//
// Two passes over one NV12 texture: an R8_UNORM render target view addresses the
// Y plane at full size, an R8G8_UNORM one addresses the interleaved UV plane at
// half. That is the standard way to write NV12 from a pixel shader, and it is
// why the texture needs D3D11_BIND_RENDER_TARGET.
//
// Downscaling is folded into the same pass, exactly as it is on the CPU side
// (README 3.2): each destination pixel box-averages its whole source footprint
// rather than taking one bilinear tap, because a single tap throws away most of
// the detail that makes small text readable.
// ---------------------------------------------------------------------------
class Nv12GpuConverter {
public:
    Nv12GpuConverter() = default;
    ~Nv12GpuConverter();
    Nv12GpuConverter(const Nv12GpuConverter&) = delete;
    Nv12GpuConverter& operator=(const Nv12GpuConverter&) = delete;

    // dstW/dstH must both be even -- NV12 is 4:2:0 and the UV plane is exactly
    // half of each. Returns false if this device cannot render to NV12, which
    // some older drivers genuinely cannot.
    bool init(const std::shared_ptr<GpuDevice>& dev, int dstW, int dstH);
    void reset();

    bool ready() const { return static_cast<bool>(device_); }
    int  width()  const { return dstW_; }
    int  height() const { return dstH_; }

    // Converts `src` and returns one of the ring's NV12 textures, still owned by
    // this object. Null on failure.
    //
    // The ring exists because the encoder holds its input texture until it has
    // finished with it, which is well after this returns -- writing the next
    // frame into the same texture would corrupt the one being encoded.
    ID3D11Texture2D* convert(ID3D11Texture2D* src);

private:
    bool compileShaders();
    bool createRing();
    bool ensureSourceView(ID3D11Texture2D* src);

    static constexpr int kRing = 4;

    struct Surface {
        ComPtr<ID3D11Texture2D>        texture;
        ComPtr<ID3D11RenderTargetView> yView;    // R8,   full size
        ComPtr<ID3D11RenderTargetView> uvView;   // R8G8, half size
    };

    std::shared_ptr<GpuDevice> device_;
    Surface                    ring_[kRing];
    int                        next_ = 0;
    int                        dstW_ = 0, dstH_ = 0;

    ComPtr<ID3D11VertexShader>      vs_;
    ComPtr<ID3D11PixelShader>       psY_, psUv_;
    ComPtr<ID3D11SamplerState>      sampler_;
    ComPtr<ID3D11Buffer>            params_;

    // Cached per source texture: rebuilding the SRV every frame is pointless
    // when the composed BGRA texture is the same object each time.
    ComPtr<ID3D11ShaderResourceView> srcView_;
    ID3D11Texture2D*                 srcCached_ = nullptr;   // identity only, not owned
    int                              srcW_ = 0, srcH_ = 0;
};

} // namespace soi
