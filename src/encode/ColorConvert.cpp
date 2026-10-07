#include "encode/ColorConvert.h"
#include "util/Parallel.h"

#if defined(_M_X64) || defined(_M_IX86) || defined(__x86_64__) || defined(__i386__)
  #define SOI_X86 1
  #include <emmintrin.h>   // SSE2
  #include <tmmintrin.h>   // SSSE3 (_mm_hadd_epi32)
  #if defined(_MSC_VER)
    #include <intrin.h>
  #else
    #include <cpuid.h>
  #endif
#elif defined(__aarch64__) || defined(_M_ARM64)
  #define SOI_NEON 1
  #include <arm_neon.h>
#endif

// GCC and Clang only emit SSSE3 instructions in a function that asks for them;
// the binary as a whole still targets baseline x86-64, and the CPU is checked
// at run time before this path is taken. MSVC needs no such annotation.
#if defined(SOI_X86) && !defined(_MSC_VER)
  #define SOI_TARGET_SSSE3 __attribute__((target("ssse3")))
#else
  #define SOI_TARGET_SSSE3
#endif

#include <algorithm>
#include <cstring>

namespace soi {
namespace {

// ---------------------------------------------------------------------------
// BT.709 limited range, Q15 fixed point.
//   Y  =  0.1826R + 0.6142G + 0.0620B + 16
//   Cb = -0.1006R - 0.3386G + 0.4392B + 128
//   Cr =  0.4392R - 0.3989G - 0.0403B + 128
// The chroma coefficient triples each sum to zero, which keeps a neutral grey
// exactly at 128 and avoids a colour cast on desktop content.
// ---------------------------------------------------------------------------
constexpr int kShift = 15;
constexpr int kRound = 1 << (kShift - 1);

constexpr int kYR =  5983, kYG = 20128, kYB =  2032;
constexpr int kUR = -3297, kUG = -11096, kUB = 14392;
constexpr int kVR = 14392, kVG = -13071, kVB = -1321;

constexpr int kYOffset = (16  << kShift) + kRound;
constexpr int kCOffset = (128 << kShift) + kRound;

inline uint8_t clampByte(int v) {
    return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}

#if defined(SOI_X86)
bool detectSsse3() {
#if defined(_MSC_VER)
    int info[4] = {0, 0, 0, 0};
    __cpuid(info, 0);
    if (info[0] < 1) return false;
    __cpuid(info, 1);
    return (info[2] & (1 << 9)) != 0;   // ECX bit 9 = SSSE3
#else
    unsigned eax = 0, ebx = 0, ecx = 0, edx = 0;
    if (!__get_cpuid(1, &eax, &ebx, &ecx, &edx)) return false;
    return (ecx & (1u << 9)) != 0;      // ECX bit 9 = SSSE3
#endif
}

const bool g_ssse3 = detectSsse3();
#elif !defined(SOI_NEON)
const bool g_ssse3 = false;
#endif
// (NEON is part of every ARMv8 CPU, so there is nothing to detect; see yRowNeon.)

// ---------------------------------------------------------------------------
// Y plane, 4 pixels per iteration.
//
// _mm_madd_epi16 computes (B*cB + G*cG) and (R*cR + A*0) as two 32-bit lanes per
// pixel; _mm_hadd_epi32 then folds those pairs into one accumulator per pixel.
// Inputs are zero-extended bytes (0..255) so the signed-16-bit semantics of
// madd are safe, and the largest partial product (255 * 20128) stays well
// inside 32 bits.
// ---------------------------------------------------------------------------
#if defined(SOI_X86)
SOI_TARGET_SSSE3
void yRowSimd(const uint8_t* src, uint8_t* dstY, int width) {
    const __m128i zero   = _mm_setzero_si128();
    // BGRA order in memory -> coefficients must be laid out B, G, R, A.
    const __m128i coef   = _mm_setr_epi16(static_cast<short>(kYB), static_cast<short>(kYG),
                                          static_cast<short>(kYR), 0,
                                          static_cast<short>(kYB), static_cast<short>(kYG),
                                          static_cast<short>(kYR), 0);
    const __m128i offset = _mm_set1_epi32(kYOffset);

    int x = 0;
    for (; x + 8 <= width; x += 8) {
        const __m128i px0 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4));
        const __m128i px1 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(src + x * 4 + 16));

        const __m128i a0 = _mm_madd_epi16(_mm_unpacklo_epi8(px0, zero), coef);
        const __m128i a1 = _mm_madd_epi16(_mm_unpackhi_epi8(px0, zero), coef);
        const __m128i b0 = _mm_madd_epi16(_mm_unpacklo_epi8(px1, zero), coef);
        const __m128i b1 = _mm_madd_epi16(_mm_unpackhi_epi8(px1, zero), coef);

        __m128i lo = _mm_hadd_epi32(a0, a1);   // Y0..Y3
        __m128i hi = _mm_hadd_epi32(b0, b1);   // Y4..Y7

        lo = _mm_srai_epi32(_mm_add_epi32(lo, offset), kShift);
        hi = _mm_srai_epi32(_mm_add_epi32(hi, offset), kShift);

        const __m128i packed = _mm_packus_epi16(_mm_packs_epi32(lo, hi), zero);
        _mm_storel_epi64(reinterpret_cast<__m128i*>(dstY + x), packed);
    }

    for (; x < width; ++x) {
        const uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
        dstY[x] = clampByte((kYR * r + kYG * g + kYB * b + kYOffset) >> kShift);
    }
}
#endif

#if defined(SOI_NEON)
// ---------------------------------------------------------------------------
// Y plane on ARM NEON, 8 pixels per iteration. vld4 de-interleaves BGRA into
// four 8-lane planes; the Y coefficients are all positive and below 2^15, so
// unsigned 16x16->32 multiply-accumulates are exact, and the result is the
// same Q15 arithmetic as the scalar path, bit for bit.
// ---------------------------------------------------------------------------
void yRowNeon(const uint8_t* src, uint8_t* dstY, int width) {
    const uint32x4_t offset = vdupq_n_u32(static_cast<uint32_t>(kYOffset));
    int x = 0;
    for (; x + 8 <= width; x += 8) {
        const uint8x8x4_t px = vld4_u8(src + x * 4);   // val[0]=B, [1]=G, [2]=R
        const uint16x8_t b = vmovl_u8(px.val[0]);
        const uint16x8_t g = vmovl_u8(px.val[1]);
        const uint16x8_t r = vmovl_u8(px.val[2]);

        uint32x4_t lo = vmlal_n_u16(offset, vget_low_u16(r), kYR);
        lo = vmlal_n_u16(lo, vget_low_u16(g), kYG);
        lo = vmlal_n_u16(lo, vget_low_u16(b), kYB);
        uint32x4_t hi = vmlal_n_u16(offset, vget_high_u16(r), kYR);
        hi = vmlal_n_u16(hi, vget_high_u16(g), kYG);
        hi = vmlal_n_u16(hi, vget_high_u16(b), kYB);

        const uint16x8_t y16 = vcombine_u16(vshrn_n_u32(lo, kShift), vshrn_n_u32(hi, kShift));
        vst1_u8(dstY + x, vqmovn_u16(y16));
    }
    for (; x < width; ++x) {
        const uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
        dstY[x] = clampByte((kYR * r + kYG * g + kYB * b + kYOffset) >> kShift);
    }
}
#endif

[[maybe_unused]] void yRowScalar(const uint8_t* src, uint8_t* dstY, int width) {
    for (int x = 0; x < width; ++x) {
        const uint8_t b = src[x * 4 + 0], g = src[x * 4 + 1], r = src[x * 4 + 2];
        dstY[x] = clampByte((kYR * r + kYG * g + kYB * b + kYOffset) >> kShift);
    }
}

// ---------------------------------------------------------------------------
// UV plane. Quarter the pixel count of Y, so scalar is fine and correctness is
// easier to see. Each output sample box-averages a 2x2 block *before*
// converting -- point-sampling instead produces visible chroma crawl on the
// 1px-wide coloured text and window borders that dominate desktop content.
// ---------------------------------------------------------------------------
void uvRow(const uint8_t* row0, const uint8_t* row1, uint8_t* dstUv, int width) {
    for (int x = 0; x < width; x += 2) {
        const uint8_t* p00 = row0 + x * 4;
        const uint8_t* p01 = p00 + 4;
        const uint8_t* p10 = row1 + x * 4;
        const uint8_t* p11 = p10 + 4;

        const int b = p00[0] + p01[0] + p10[0] + p11[0];
        const int g = p00[1] + p01[1] + p10[1] + p11[1];
        const int r = p00[2] + p01[2] + p10[2] + p11[2];

        // Coefficients are applied to the 4-pixel sum, so divide by 4 via the shift.
        const int u = (kUR * r + kUG * g + kUB * b + 4 * kCOffset) >> (kShift + 2);
        const int v = (kVR * r + kVG * g + kVB * b + 4 * kCOffset) >> (kShift + 2);

        dstUv[x + 0] = clampByte(u);
        dstUv[x + 1] = clampByte(v);
    }
}

// ---------------------------------------------------------------------------
// 1:1 conversion -- SIMD Y plane, subsampled UV.
// ---------------------------------------------------------------------------
void convertDirect(const uint8_t* src, int srcStride, int width, int height,
                   Nv12Buffer& dst) {
    uint8_t* yPlane  = dst.y();
    uint8_t* uvPlane = dst.uv();
    const int yStride  = dst.stride();
    const int uvStride = dst.stride();

    const int rowPairs = height / 2;
#if defined(SOI_X86)
    auto yFn = g_ssse3 ? yRowSimd : yRowScalar;
#elif defined(SOI_NEON)
    auto yFn = yRowNeon;
#else
    auto yFn = yRowScalar;
#endif

    // One work item = one 2-row band, so Y and UV for the same band stay on the
    // same core and the source rows are read once while hot in L1/L2.
    sharedPool().parallelFor(static_cast<size_t>(rowPairs), [&](size_t begin, size_t end) {
        for (size_t pair = begin; pair < end; ++pair) {
            const int y0 = static_cast<int>(pair) * 2;
            const uint8_t* row0 = src + static_cast<size_t>(y0) * srcStride;
            const uint8_t* row1 = row0 + srcStride;

            yFn(row0, yPlane + static_cast<size_t>(y0) * yStride, width);
            yFn(row1, yPlane + static_cast<size_t>(y0 + 1) * yStride, width);
            uvRow(row0, row1, uvPlane + pair * uvStride, width);
        }
    });
}

// ---------------------------------------------------------------------------
// Downscaling conversion.
//
// Each output pixel box-averages its source footprint. The per-output-pixel
// channel sums are computed once for both rows of an output row pair, then Y is
// derived per row and UV by combining the four sums of the 2x2 block -- so every
// source pixel is read exactly once, not three times.
// ---------------------------------------------------------------------------
void convertScaled(const uint8_t* src, int srcStride, int srcW, int srcH,
                   int dstW, int dstH, Nv12Buffer& dst) {
    uint8_t*  yPlane  = dst.y();
    uint8_t*  uvPlane = dst.uv();
    const int yStride  = dst.stride();
    const int uvStride = dst.stride();

    // Column boundaries are identical for every row, so compute them once.
    std::vector<int> colEdge(static_cast<size_t>(dstW) + 1);
    for (int x = 0; x <= dstW; ++x)
        colEdge[static_cast<size_t>(x)] =
            static_cast<int>(static_cast<int64_t>(x) * srcW / dstW);
    for (int x = 0; x < dstW; ++x)
        if (colEdge[static_cast<size_t>(x) + 1] <= colEdge[static_cast<size_t>(x)])
            colEdge[static_cast<size_t>(x) + 1] = colEdge[static_cast<size_t>(x)] + 1;
    colEdge[static_cast<size_t>(dstW)] = std::min(colEdge[static_cast<size_t>(dstW)], srcW);

    const int rowPairs = dstH / 2;

    sharedPool().parallelFor(static_cast<size_t>(rowPairs), [&](size_t begin, size_t end) {
        // Reused across frames: this is the per-frame hot path, and a malloc per
        // chunk per frame would show up at 60fps.
        thread_local std::vector<int> scratch;
        scratch.resize(static_cast<size_t>(dstW) * 4 * 2);   // {b,g,r,count} x 2 rows

        for (size_t pair = begin; pair < end; ++pair) {
            for (int sub = 0; sub < 2; ++sub) {
                const int dy = static_cast<int>(pair) * 2 + sub;

                int y0 = static_cast<int>(static_cast<int64_t>(dy) * srcH / dstH);
                int y1 = static_cast<int>(static_cast<int64_t>(dy + 1) * srcH / dstH);
                if (y1 <= y0) y1 = y0 + 1;
                if (y1 > srcH) y1 = srcH;
                if (y0 >= srcH) y0 = srcH - 1;

                int* acc = scratch.data() + static_cast<size_t>(sub) * dstW * 4;
                std::fill(acc, acc + static_cast<size_t>(dstW) * 4, 0);

                for (int sy = y0; sy < y1; ++sy) {
                    const uint8_t* srow = src + static_cast<size_t>(sy) * srcStride;
                    for (int x = 0; x < dstW; ++x) {
                        const int x0 = colEdge[static_cast<size_t>(x)];
                        const int x1 = std::min(colEdge[static_cast<size_t>(x) + 1], srcW);
                        int b = 0, g = 0, r = 0;
                        for (int sx = x0; sx < x1; ++sx) {
                            const uint8_t* p = srow + static_cast<size_t>(sx) * 4;
                            b += p[0]; g += p[1]; r += p[2];
                        }
                        int* a = acc + static_cast<size_t>(x) * 4;
                        a[0] += b; a[1] += g; a[2] += r; a[3] += (x1 - x0);
                    }
                }

                uint8_t* yrow = yPlane + static_cast<size_t>(dy) * yStride;
                for (int x = 0; x < dstW; ++x) {
                    const int* a = acc + static_cast<size_t>(x) * 4;
                    const int  n = a[3] > 0 ? a[3] : 1;
                    const int  b = a[0] / n, g = a[1] / n, r = a[2] / n;
                    yrow[x] = clampByte((kYR * r + kYG * g + kYB * b + kYOffset) >> kShift);
                }
            }

            // UV: combine the 2x2 block of already-accumulated sums.
            const int* acc0 = scratch.data();
            const int* acc1 = scratch.data() + static_cast<size_t>(dstW) * 4;
            uint8_t*   uvrow = uvPlane + pair * uvStride;

            for (int x = 0; x < dstW; x += 2) {
                const int* a00 = acc0 + static_cast<size_t>(x) * 4;
                const int* a01 = acc0 + static_cast<size_t>(x + 1) * 4;
                const int* a10 = acc1 + static_cast<size_t>(x) * 4;
                const int* a11 = acc1 + static_cast<size_t>(x + 1) * 4;

                const int b = a00[0] + a01[0] + a10[0] + a11[0];
                const int g = a00[1] + a01[1] + a10[1] + a11[1];
                const int r = a00[2] + a01[2] + a10[2] + a11[2];
                const int n = std::max(1, a00[3] + a01[3] + a10[3] + a11[3]);

                const int bn = b / n, gn = g / n, rn = r / n;
                uvrow[x + 0] = clampByte((kUR * rn + kUG * gn + kUB * bn + kCOffset) >> kShift);
                uvrow[x + 1] = clampByte((kVR * rn + kVG * gn + kVB * bn + kCOffset) >> kShift);
            }
        }
    });
}

} // namespace

#if defined(SOI_NEON)
bool colorConvertUsesSimd() { return true; }
const char* colorConvertSimdName() { return "NEON"; }
#else
bool colorConvertUsesSimd() { return g_ssse3; }
const char* colorConvertSimdName() { return g_ssse3 ? "SSSE3" : "scalar"; }
#endif

void bgraToNv12(const uint8_t* src, int srcStride, int srcWidth, int srcHeight,
                int dstWidth, int dstHeight, Nv12Buffer& dst) {
    if (srcWidth <= 0 || srcHeight <= 0 || dstWidth <= 0 || dstHeight <= 0) return;

    dstWidth  &= ~1;
    dstHeight &= ~1;
    if (dstWidth <= 0 || dstHeight <= 0) return;

    // Upsampling is not supported; clamp rather than read out of bounds.
    dstWidth  = std::min(dstWidth,  srcWidth  & ~1);
    dstHeight = std::min(dstHeight, srcHeight & ~1);

    if (dst.width() != dstWidth || dst.height() != dstHeight)
        dst.resize(dstWidth, dstHeight);

    if (dstWidth == (srcWidth & ~1) && dstHeight == (srcHeight & ~1))
        convertDirect(src, srcStride, dstWidth, dstHeight, dst);
    else
        convertScaled(src, srcStride, srcWidth, srcHeight, dstWidth, dstHeight, dst);
}

} // namespace soi
