#pragma once
//
// The surface every backend hands to the encoder.
//
// It is a GDI DIB section rather than a plain heap allocation for one reason:
// DXGI Desktop Duplication and Windows.Graphics.Capture both exclude the mouse
// cursor from the image, and the only cheap way to draw a live cursor -- with
// its mask, its colour table and its per-monitor scaling already resolved -- is
// DrawIconEx, which needs an HDC. A DIB section is the one allocation that is
// both a flat BGRA array and a GDI drawing surface.
//
// Top-down (negative biHeight) so row 0 is the top row, which is what the NV12
// converter expects and saves a full-frame flip per frame.
//
#include "util/Win.h"

#include <cstdint>

namespace soi {

class FrameBuffer {
public:
    // Idempotent: a resize to the current size keeps the existing pixels.
    bool resize(int w, int h);
    void release();

    bool     valid()  const { return pixels_ != nullptr; }
    int      width()  const { return w_; }
    int      height() const { return h_; }
    int      stride() const { return w_ * 4; }
    uint8_t* pixels() const { return pixels_; }
    HDC      dc()     const { return dc_.get(); }

    void fillBlack();

    // Copies a BGRA block in at (x, y), clipping to the buffer. Used by the
    // multi-monitor DXGI path, where each output lands at its own desktop offset.
    void blitIn(const uint8_t* src, int srcStride, int x, int y, int w, int h);

    // GDI batches drawing calls and the DIB's memory is not coherent until the
    // batch is flushed. Every backend must call this after the last GDI draw and
    // before reading pixels(), or the frame tears.
    static void flushGdi();

private:
    ScopedDc     dc_;
    ScopedBitmap bmp_;
    uint8_t*     pixels_ = nullptr;
    int          w_ = 0, h_ = 0;
};

// Draws the live cursor into `dst` at its screen position, where (originX,
// originY) is the desktop coordinate of the surface's top-left pixel. No-op when
// the cursor is hidden. Shared by all three backends so the cursor looks the
// same whichever one is running.
void compositeCursor(HDC dst, int originX, int originY);

// Sampled FNV-1a over every 2nd row and every 4th pixel -- about 1/8 of the
// frame, which keeps the check under ~0.2 ms at 1080p. A sub-pixel change can
// slip through; the caller's --idle-refresh timer guarantees a frame regardless.
//
// Only the GDI backend needs this. DXGI and WGC are told by the OS whether new
// content arrived, which is both free and exact.
uint64_t sampledFrameHash(const uint8_t* pixels, int stride, int height);

} // namespace soi
