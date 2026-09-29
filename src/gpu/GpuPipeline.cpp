#include "gpu/GpuPipeline.h"
#include "util/Log.h"

#include <d3d11_4.h>          // ID3D11Multithread
#include <d3dcompiler.h>
#include <dxgi1_2.h>

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

constexpr D3D_FEATURE_LEVEL kFeatureLevels[] = {
    D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
};

// One constant buffer, shared by both passes. 16-byte aligned because D3D
// requires it; the padding is not decoration.
struct alignas(16) ConvertParams {
    float srcSize[2];
    float dstSize[2];
    float boxSize[2];    // source pixels covered by one destination pixel
    float pad[2];
};

// ---------------------------------------------------------------------------
// The shader.
//
// Compiled at run time rather than offline: it is eighty lines, it compiles in
// about a millisecond once per session, and doing it this way keeps fxc out of
// the build entirely. d3dcompiler_47.dll ships with Windows.
// ---------------------------------------------------------------------------
const char kShaderSource[] = R"HLSL(
cbuffer Params : register(b0) {
    float2 srcSize;
    float2 dstSize;
    float2 boxSize;
    float2 pad_;
};

Texture2D<float4> src : register(t0);

struct VSOut { float4 pos : SV_Position; };

// A fullscreen triangle from the vertex id alone -- no vertex buffer, no index
// buffer, no input layout. One triangle rather than two also avoids the
// diagonal seam where quads meet.
VSOut VS(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Average every source pixel this destination pixel covers.
//
// The CPU path box-filters for a reason (README 3.2): one bilinear tap reads two
// pixels out of the nine a 3x downscale covers, and the seven it skips are the
// ones that made the text legible. Load() rather than Sample() because the
// footprint is integer and we want no filtering of our own on top.
float3 boxSample(float2 dstPixel, float2 box) {
    float2 start = dstPixel * box;
    int2 i0 = int2(floor(start));
    int2 i1 = int2(ceil(start + box)) - 1;

    int2 maxIndex = int2(srcSize) - 1;
    i1 = min(i1, maxIndex);
    // Bound the loop. A 16x16 footprint is a 16x downscale, far past anything
    // this tool produces, and an unbounded loop here is a driver hang waiting
    // for a bad constant buffer.
    i1 = min(i1, i0 + 15);
    i0 = clamp(i0, int2(0, 0), maxIndex);
    i1 = max(i1, i0);

    float3 sum = float3(0.0, 0.0, 0.0);
    float  n   = 0.0;
    for (int y = i0.y; y <= i1.y; ++y) {
        for (int x = i0.x; x <= i1.x; ++x) {
            sum += src.Load(int3(x, y, 0)).rgb;
            n   += 1.0;
        }
    }
    return sum / max(n, 1.0);
}

// BT.709, limited range (16-235 luma, 16-240 chroma). The same matrix the SSE2
// path uses -- BT.601 here is the usual cause of "the colours look washed out".
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

float PSY(VSOut i) : SV_Target {
    float3 c = boxSample(floor(i.pos.xy), boxSize);
    float  y = dot(c, kLuma);
    return (16.0 + 219.0 * y) / 255.0;
}

float2 PSUV(VSOut i) : SV_Target {
    // One chroma sample stands for a 2x2 block of luma, so its source footprint
    // is twice as wide and twice as tall. Averaging the whole block here is what
    // makes this a proper 4:2:0 subsample rather than a point sample of one
    // corner of it.
    float3 c = boxSample(floor(i.pos.xy), boxSize * 2.0);
    float  y = dot(c, kLuma);
    float  u = (c.b - y) / 1.8556;
    float  v = (c.r - y) / 1.5748;
    return float2((128.0 + 224.0 * u) / 255.0,
                  (128.0 + 224.0 * v) / 255.0);
}
)HLSL";

bool compileOne(const char* entry, const char* target, ID3DBlob** blob) {
    ComPtr<ID3DBlob> errors;
    const UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;

    const HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1,
                                  "soi-nv12", nullptr, nullptr, entry, target,
                                  flags, 0, blob, errors.put());
    if (FAILED(hr)) {
        // The compiler's message is the only useful thing here; an HRESULT alone
        // would say "E_FAIL" and nothing about which line.
        logW("gpu: shader {} failed to compile: {}", entry,
             errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no detail");
        return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

const char* pipelineName(Pipeline p) {
    switch (p) {
        case Pipeline::Auto: return "auto";
        case Pipeline::Gpu:  return "gpu";
        case Pipeline::Cpu:  return "cpu";
    }
    return "auto";
}

bool parsePipelineName(std::string_view name, Pipeline& out) {
    if (name == "auto") { out = Pipeline::Auto; return true; }
    if (name == "gpu")  { out = Pipeline::Gpu;  return true; }
    if (name == "cpu")  { out = Pipeline::Cpu;  return true; }
    return false;
}

// ---------------------------------------------------------------------------
// GpuDevice
// ---------------------------------------------------------------------------

GpuDevice::~GpuDevice() = default;

std::shared_ptr<GpuDevice> GpuDevice::create(IDXGIAdapter1* adapter) {
    auto self = std::shared_ptr<GpuDevice>(new GpuDevice());

    // BGRA_SUPPORT for the duplication format, VIDEO_SUPPORT because writing
    // NV12 render targets is a video-device capability on several drivers.
    const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;

    // D3D_DRIVER_TYPE_UNKNOWN is mandatory when an adapter is named, and
    // HARDWARE is mandatory when one is not. Getting this backwards is
    // E_INVALIDARG with no explanation.
    HRESULT hr = D3D11CreateDevice(
        adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
        nullptr, flags, kFeatureLevels, ARRAYSIZE(kFeatureLevels),
        D3D11_SDK_VERSION, self->device_.put(), nullptr, self->context_.put());

    if (FAILED(hr)) {
        // Some drivers refuse VIDEO_SUPPORT on a device they will otherwise
        // create. Worth one retry: without it we lose the whole GPU path.
        logT("gpu: device creation with video support failed ({}); retrying without",
             hrString(hr));
        hr = D3D11CreateDevice(
            adapter, adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, kFeatureLevels,
            ARRAYSIZE(kFeatureLevels), D3D11_SDK_VERSION,
            self->device_.put(), nullptr, self->context_.put());
    }
    if (FAILED(hr)) {
        logT("gpu: could not create a D3D11 device: {}", hrString(hr));
        return nullptr;
    }

    // Media Foundation drives this device from its own encoder thread while the
    // capture thread is using it. Without this the two race inside the driver,
    // which shows up as sporadic corrupt frames rather than a clean crash.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(self->device_.as(mt))) {
        mt->SetMultithreadProtected(TRUE);
    } else {
        logT("gpu: ID3D11Multithread unavailable; declining the GPU path");
        return nullptr;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    ComPtr<IDXGIAdapter> owned;
    DXGI_ADAPTER_DESC desc{};
    if (SUCCEEDED(self->device_.as(dxgiDevice)) &&
        SUCCEEDED(dxgiDevice->GetAdapter(owned.put())) &&
        SUCCEEDED(owned->GetDesc(&desc))) {
        self->luid_        = desc.AdapterLuid;
        self->description_ = toUtf8(desc.Description);
    } else {
        self->description_ = "unknown adapter";
    }

    return self;
}

// ---------------------------------------------------------------------------
// Nv12GpuConverter
// ---------------------------------------------------------------------------

Nv12GpuConverter::~Nv12GpuConverter() { reset(); }

void Nv12GpuConverter::reset() {
    for (auto& s : ring_) { s.yView.reset(); s.uvView.reset(); s.texture.reset(); }
    srcView_.reset();
    srcCached_ = nullptr;
    srcW_ = srcH_ = 0;
    params_.reset();
    sampler_.reset();
    psY_.reset();
    psUv_.reset();
    vs_.reset();
    device_.reset();
    dstW_ = dstH_ = 0;
    next_ = 0;
}

bool Nv12GpuConverter::init(const std::shared_ptr<GpuDevice>& dev, int dstW, int dstH) {
    reset();
    if (!dev || dstW < 16 || dstH < 16) return false;

    // 4:2:0 has exactly half as many chroma samples in each direction, so an odd
    // size has no correct answer. computeEncodeSize() already rounds down; this
    // is the guard for anyone who calls in directly.
    if ((dstW & 1) || (dstH & 1)) {
        logW("gpu: {}x{} is not even; NV12 needs both dimensions even", dstW, dstH);
        return false;
    }

    device_ = dev;
    dstW_   = dstW;
    dstH_   = dstH;

    UINT support = 0;
    if (FAILED(device_->device()->CheckFormatSupport(DXGI_FORMAT_NV12, &support)) ||
        !(support & D3D11_FORMAT_SUPPORT_RENDER_TARGET)) {
        logT("gpu: this device cannot render to NV12; using the CPU converter");
        reset();
        return false;
    }

    if (!compileShaders() || !createRing()) { reset(); return false; }

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth      = sizeof(ConvertParams);
    bd.Usage          = D3D11_USAGE_DYNAMIC;
    bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->device()->CreateBuffer(&bd, nullptr, params_.put()))) {
        logT("gpu: constant buffer creation failed");
        reset();
        return false;
    }

    D3D11_SAMPLER_DESC sd{};
    sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;   // boxSample uses Load(); this is belt and braces
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD   = D3D11_FLOAT32_MAX;
    if (FAILED(device_->device()->CreateSamplerState(&sd, sampler_.put()))) {
        logT("gpu: sampler creation failed");
        reset();
        return false;
    }

    logI("gpu: NV12 converter ready at {}x{} on {}", dstW_, dstH_, device_->describe());
    return true;
}

bool Nv12GpuConverter::compileShaders() {
    ComPtr<ID3DBlob> vsBlob, yBlob, uvBlob;
    if (!compileOne("VS",   "vs_5_0", vsBlob.put()) ||
        !compileOne("PSY",  "ps_5_0", yBlob.put())  ||
        !compileOne("PSUV", "ps_5_0", uvBlob.put()))
        return false;

    ID3D11Device* d = device_->device();
    if (FAILED(d->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                     nullptr, vs_.put())) ||
        FAILED(d->CreatePixelShader(yBlob->GetBufferPointer(), yBlob->GetBufferSize(),
                                    nullptr, psY_.put())) ||
        FAILED(d->CreatePixelShader(uvBlob->GetBufferPointer(), uvBlob->GetBufferSize(),
                                    nullptr, psUv_.put()))) {
        logT("gpu: shader object creation failed");
        return false;
    }
    return true;
}

bool Nv12GpuConverter::createRing() {
    ID3D11Device* d = device_->device();

    D3D11_TEXTURE2D_DESC td{};
    td.Width            = static_cast<UINT>(dstW_);
    td.Height           = static_cast<UINT>(dstH_);
    td.MipLevels        = 1;
    td.ArraySize        = 1;
    td.Format           = DXGI_FORMAT_NV12;
    td.SampleDesc.Count = 1;
    td.Usage            = D3D11_USAGE_DEFAULT;
    // SHADER_RESOURCE as well as RENDER_TARGET because the encoder MFT binds it
    // as an input, and some drivers reject a render-target-only NV12 surface.
    td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    for (auto& s : ring_) {
        if (FAILED(d->CreateTexture2D(&td, nullptr, s.texture.put()))) {
            logT("gpu: NV12 texture {}x{} creation failed", dstW_, dstH_);
            return false;
        }

        // The two planes of one NV12 texture, addressed as separate render
        // targets: R8 covers Y at full size, R8G8 covers interleaved UV at half.
        // This is the whole trick that lets a pixel shader write NV12.
        D3D11_RENDER_TARGET_VIEW_DESC rt{};
        rt.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

        rt.Format = DXGI_FORMAT_R8_UNORM;
        if (FAILED(d->CreateRenderTargetView(s.texture.get(), &rt, s.yView.put()))) {
            logT("gpu: Y-plane render target view failed");
            return false;
        }

        rt.Format = DXGI_FORMAT_R8G8_UNORM;
        if (FAILED(d->CreateRenderTargetView(s.texture.get(), &rt, s.uvView.put()))) {
            logT("gpu: UV-plane render target view failed");
            return false;
        }
    }
    return true;
}

bool Nv12GpuConverter::ensureSourceView(ID3D11Texture2D* src) {
    if (srcCached_ == src && srcView_) return true;

    D3D11_TEXTURE2D_DESC td{};
    src->GetDesc(&td);
    if (td.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
        logT("gpu: source texture is format {}, not BGRA8", static_cast<int>(td.Format));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format              = td.Format;
    sv.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
    sv.Texture2D.MipLevels = 1;

    srcView_.reset();
    if (FAILED(device_->device()->CreateShaderResourceView(src, &sv, srcView_.put()))) {
        logT("gpu: source shader resource view failed");
        srcCached_ = nullptr;
        return false;
    }

    srcCached_ = src;
    srcW_      = static_cast<int>(td.Width);
    srcH_      = static_cast<int>(td.Height);
    return true;
}

ID3D11Texture2D* Nv12GpuConverter::convert(ID3D11Texture2D* src) {
    if (!device_ || !src) return nullptr;

    std::lock_guard lk(device_->contextMutex());
    if (!ensureSourceView(src)) return nullptr;

    ID3D11DeviceContext* ctx = device_->context();
    Surface& dst = ring_[next_];
    next_ = (next_ + 1) % kRing;

    ConvertParams p{};
    p.srcSize[0] = static_cast<float>(srcW_);
    p.srcSize[1] = static_cast<float>(srcH_);
    p.dstSize[0] = static_cast<float>(dstW_);
    p.dstSize[1] = static_cast<float>(dstH_);
    // How many source pixels one destination pixel stands for. 1.0 when there is
    // no downscale, in which case boxSample degenerates to a single Load.
    p.boxSize[0] = static_cast<float>(srcW_) / static_cast<float>(dstW_);
    p.boxSize[1] = static_cast<float>(srcH_) / static_cast<float>(dstH_);

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(ctx->Map(params_.get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return nullptr;
    std::memcpy(mapped.pData, &p, sizeof p);
    ctx->Unmap(params_.get(), 0);

    // No vertex or index buffer: the vertex shader builds the triangle from
    // SV_VertexID, so there is nothing to bind and nothing to upload.
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.get(), nullptr, 0);

    ID3D11Buffer* cb = params_.get();
    ctx->VSSetConstantBuffers(0, 1, &cb);
    ctx->PSSetConstantBuffers(0, 1, &cb);

    ID3D11ShaderResourceView* srv = srcView_.get();
    ID3D11SamplerState*       smp = sampler_.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ctx->PSSetSamplers(0, 1, &smp);
    ctx->RSSetState(nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    ctx->OMSetDepthStencilState(nullptr, 0);

    auto pass = [&](ID3D11RenderTargetView* rtv, ID3D11PixelShader* ps, int w, int h) {
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(w);
        vp.Height   = static_cast<float>(h);
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->OMSetRenderTargets(1, &rtv, nullptr);
        ctx->PSSetShader(ps, nullptr, 0);
        ctx->Draw(3, 0);
    };

    pass(dst.yView.get(),  psY_.get(),  dstW_,     dstH_);
    pass(dst.uvView.get(), psUv_.get(), dstW_ / 2, dstH_ / 2);

    // Unbind before returning: the encoder is about to read this texture, and a
    // render target still bound as an output cannot be read as an input. The
    // debug layer says so loudly; the retail runtime just gives you black.
    ID3D11RenderTargetView* none = nullptr;
    ctx->OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* noSrv = nullptr;
    ctx->PSSetShaderResources(0, 1, &noSrv);

    return dst.texture.get();
}

} // namespace soi
