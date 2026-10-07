#include "capture/FrameBuffer.h"
#include "capture/FrameSource.h"
#include "util/Log.h"

#include <algorithm>
#include <cstring>

namespace soi {

bool FrameBuffer::resize(int w, int h) {
    if (w <= 0 || h <= 0) return false;
    if (pixels_ && w == w_ && h == h_) return true;

    release();

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = w;
    bi.bmiHeader.biHeight      = -h;          // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    HDC screen = GetDC(nullptr);
    if (!screen) return false;

    HDC mem = CreateCompatibleDC(screen);
    if (!mem) { ReleaseDC(nullptr, screen); return false; }

    void*   bits = nullptr;
    HBITMAP hb   = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);

    if (!hb || !bits) {
        if (hb) DeleteObject(hb);
        DeleteDC(mem);
        return false;
    }

    SelectObject(mem, hb);   // stays selected for the DC's lifetime
    dc_.reset(mem);
    bmp_.reset(hb);
    pixels_ = static_cast<uint8_t*>(bits);
    w_ = w;
    h_ = h;
    return true;
}

void FrameBuffer::release() {
    bmp_.reset();
    dc_.reset();
    pixels_ = nullptr;
    w_ = h_ = 0;
}

void FrameBuffer::fillBlack() {
    if (!pixels_) return;
    std::memset(pixels_, 0, static_cast<size_t>(stride()) * static_cast<size_t>(h_));
}

void FrameBuffer::blitIn(const uint8_t* src, int srcStride, int x, int y, int w, int h) {
    if (!pixels_ || !src) return;

    // Clip against both ends. A monitor can be unplugged between the moment the
    // layout was read and the moment its frame arrives, so the geometry handed
    // in here is not something to trust.
    const int sx = x < 0 ? -x : 0;
    const int sy = y < 0 ? -y : 0;
    const int dx = std::max(x, 0);
    const int dy = std::max(y, 0);
    const int cw = std::min(w - sx, w_ - dx);
    const int ch = std::min(h - sy, h_ - dy);
    if (cw <= 0 || ch <= 0) return;

    const size_t bytes = static_cast<size_t>(cw) * 4;
    for (int row = 0; row < ch; ++row) {
        const uint8_t* s = src + static_cast<size_t>(sy + row) * static_cast<size_t>(srcStride)
                               + static_cast<size_t>(sx) * 4;
        uint8_t* d = pixels_ + static_cast<size_t>(dy + row) * static_cast<size_t>(stride())
                             + static_cast<size_t>(dx) * 4;
        std::memcpy(d, s, bytes);
    }
}

void FrameBuffer::flushGdi() { GdiFlush(); }

// ---------------------------------------------------------------------------

void compositeCursor(HDC dst, int originX, int originY) {
    if (!dst) return;

    CURSORINFO ci{};
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING) || !ci.hCursor) return;

    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;

    // The surface is at native resolution here, so the cursor composites 1:1 and
    // is downscaled along with everything else during conversion.
    const int x = ci.ptScreenPos.x - originX - static_cast<int>(ii.xHotspot);
    const int y = ci.ptScreenPos.y - originY - static_cast<int>(ii.yHotspot);

    DrawIconEx(dst, x, y, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);

    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

uint64_t sampledFrameHash(const uint8_t* pixels, int stride, int height) {
    uint64_t h = 1469598103934665603ULL;
    for (int y = 0; y < height; y += 2) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            pixels + static_cast<size_t>(y) * static_cast<size_t>(stride));
        const int n = stride / 4;
        for (int x = 0; x < n; x += 4) {
            h ^= row[x];
            h *= 1099511628211ULL;
        }
    }
    return h;
}

// ---------------------------------------------------------------------------
// Shared FrameSource helpers
// ---------------------------------------------------------------------------

const char* backendName(CaptureBackend backend) {
    switch (backend) {
        case CaptureBackend::Auto:   return "auto";
        case CaptureBackend::Dxgi:   return "dxgi";
        case CaptureBackend::Wgc:    return "wgc";
        case CaptureBackend::BitBlt: return "bitblt";
    }
    return "unknown";
}

bool parseBackendName(std::string_view name, CaptureBackend& out) {
    if (name == "auto")                            { out = CaptureBackend::Auto;   return true; }
    if (name == "dxgi" || name == "duplication")   { out = CaptureBackend::Dxgi;   return true; }
    if (name == "wgc"  || name == "graphics")      { out = CaptureBackend::Wgc;    return true; }
    if (name == "bitblt" || name == "gdi")         { out = CaptureBackend::BitBlt; return true; }
    return false;
}

const char* backendChoices() { return "auto, dxgi, wgc, bitblt"; }

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

} // namespace soi
