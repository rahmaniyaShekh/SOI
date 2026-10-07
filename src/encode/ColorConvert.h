#pragma once
//
// BGRA8888 -> NV12 (BT.709, limited/studio range 16-235).
//
// BT.709 is the correct matrix for HD content. Using BT.601 here is the single
// most common cause of "the colours look slightly washed out / too contrasty"
// in hand-rolled screen sharers, because the decoder will apply the 709 inverse
// that the SPS advertises.
//
#include <cstdint>
#include <vector>

namespace soi {

// Tightly packed NV12: Y plane of w*h followed by interleaved UV of w*(h/2).
class Nv12Buffer {
public:
    void resize(int w, int h) {
        w_ = w; h_ = h;
        data_.resize(static_cast<size_t>(w) * h * 3 / 2);
    }
    uint8_t* y()  { return data_.data(); }
    uint8_t* uv() { return data_.data() + static_cast<size_t>(w_) * h_; }
    const uint8_t* data() const { return data_.data(); }
    size_t   size() const { return data_.size(); }
    int      width()  const { return w_; }
    int      height() const { return h_; }
    int      stride() const { return w_; }   // both planes, by construction

private:
    std::vector<uint8_t> data_;
    int w_ = 0, h_ = 0;
};

// Converts, and box-filter downscales in the same pass when dst != src size.
//
// Folding the scale in here rather than using GDI's StretchBlt is worth ~16 ms
// per frame: HALFTONE StretchBlt is CPU-bound and costs about as much as the
// capture itself, while this pass already reads every source pixel anyway.
// Box filtering also beats HALFTONE's approximation on text.
//
// `dstWidth` and `dstHeight` must both be even, and must not exceed the source
// (this downscales only; it never upsamples).
void bgraToNv12(const uint8_t* src, int srcStride, int srcWidth, int srcHeight,
                int dstWidth, int dstHeight, Nv12Buffer& dst);

// 1:1 convenience overload -- takes the SIMD fast path.
inline void bgraToNv12(const uint8_t* src, int srcStride, int width, int height,
                       Nv12Buffer& dst) {
    bgraToNv12(src, srcStride, width, height, width & ~1, height & ~1, dst);
}

// True if a SIMD fast path is in use on this CPU -- SSSE3 on x86, NEON on ARM
// -- rather than the scalar fallback.
bool colorConvertUsesSimd();

// "SSSE3", "NEON" or "scalar", for logs.
const char* colorConvertSimdName();

} // namespace soi
