//
// soi-selftest -- exercises everything that does not need a live peer.
//
// Three roles:
//   (no args)                     run the full suite
//   blob-encode <text> [pass]     print a signalling blob   (Node interop check)
//   blob-decode <blob> [pass]     print the decoded payload (Node interop check)
//   unit                          only the sections that need no screen, GPU or
//                                 media stack -- what CI runs on a headless runner
//
#include "app/Control.h"
#include "app/Install.h"
#include "app/Update.h"
#include "capture/CaptureFactory.h"
#include "capture/CaptureProtect.h"
#include "encode/ColorConvert.h"
#include "encode/H264Encoder.h"
#include "encode/Quality.h"
#include "net/SignalBlob.h"
#include "util/Json.h"
#include "util/Log.h"
#include "util/Parallel.h"
#include "util/Platform.h"

#if defined(_WIN32)
#include "capture/BitBltCapture.h"
#include "capture/DxgiCapture.h"
#include "capture/WgcCapture.h"
#include "util/Win.h"

#include <windows.h>
#include <dpapi.h>
#include <mfapi.h>
#include <fcntl.h>
#include <io.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#else
#include "capture/mac/MacCapture.h"
#include "gpu/GpuPipeline.h"
#include "selftest_mac.h"
#include <cstdlib>
#include <unistd.h>
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <map>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

using namespace soi;
using namespace std::chrono_literals;

namespace {

int g_pass = 0, g_fail = 0;
std::string g_section;

void section(const char* name) {
    g_section = name;
    std::printf("\n\x1b[1m== %s ==\x1b[0m\n", name);
}

void check(bool ok, const std::string& what, const std::string& detail = "") {
    if (ok) {
        ++g_pass;
        std::printf("  \x1b[32mPASS\x1b[0m  %s\n", what.c_str());
    } else {
        ++g_fail;
        std::printf("  \x1b[31mFAIL\x1b[0m  %s%s%s\n", what.c_str(),
                    detail.empty() ? "" : "  -- ", detail.c_str());
    }
}

void info(const std::string& text) {
    std::printf("        \x1b[90m%s\x1b[0m\n", text.c_str());
}

// `unit` mode runs on shared CI machines whose speed says nothing about this
// code, so a timing budget there is reported rather than enforced. The full
// suite, run on real hardware, still fails on it.
bool g_timingIsAdvisory = false;

void checkTiming(bool ok, const std::string& what, const std::string& detail) {
    if (ok || !g_timingIsAdvisory) { check(ok, what, detail); return; }
    std::printf("  \x1b[33mSLOW\x1b[0m  %s  -- %s (timing is advisory in unit mode)\n",
                what.c_str(), detail.c_str());
}

std::string sampleSdp() {
    // A realistic offer: this is what the compressor and crypto actually see.
    return
        "v=0\r\no=rtc 2394857 0 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "a=group:BUNDLE video control\r\na=msid-semantic:WMS *\r\n"
        "a=ice-options:trickle\r\na=fingerprint:sha-256 "
        "8E:4F:A1:22:9C:03:BE:71:5D:0A:44:E6:19:B2:77:38:A0:5C:14:6D:9E:31:88:"
        "F2:47:CB:60:15:3A:D9:02:7E\r\n"
        "m=video 51234 UDP/TLS/RTP/SAVPF 96\r\nc=IN IP4 192.168.1.42\r\n"
        "a=mid:video\r\na=sendonly\r\na=rtcp-mux\r\na=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 profile-level-id=4d0028;packetization-mode=1;"
        "level-asymmetry-allowed=1\r\n"
        "a=rtcp-fb:96 nack\r\na=rtcp-fb:96 nack pli\r\na=rtcp-fb:96 goog-remb\r\n"
        "a=ssrc:5439905 cname:soi\r\na=ice-ufrag:kPq2\r\na=ice-pwd:9xT4bGh1LmZ0\r\n"
        "a=candidate:1 1 UDP 2122317823 192.168.1.42 51234 typ host\r\n"
        "a=candidate:2 1 UDP 1686109951 203.0.113.77 51234 typ srflx "
        "raddr 192.168.1.42 rport 51234\r\n"
        "m=application 51234 UDP/DTLS/SCTP webrtc-datachannel\r\n"
        "a=mid:control\r\na=sctp-port:5000\r\n";
}

// ---------------------------------------------------------------------------
// base64url
// ---------------------------------------------------------------------------
void testBase64() {
    section("base64url");

    std::mt19937 rng(12345);
    bool allOk = true;
    std::string failAt;

    for (size_t len = 0; len <= 64 && allOk; ++len) {
        std::vector<uint8_t> data(len);
        for (auto& b : data) b = static_cast<uint8_t>(rng() & 0xFF);

        const std::string encoded = base64UrlEncode(data.data(), data.size());
        std::string decoded;
        if (!base64UrlDecode(encoded, decoded) ||
            decoded.size() != len ||
            (len && std::memcmp(decoded.data(), data.data(), len) != 0)) {
            allOk = false;
            failAt = "length " + std::to_string(len);
        }
    }
    check(allOk, "round-trips every length 0..64", failAt);

    const uint8_t urlProbe[] = {0xFB, 0xFF, 0xBE, 0xFF};
    const std::string enc = base64UrlEncode(urlProbe, sizeof urlProbe);
    check(enc.find('+') == std::string::npos && enc.find('/') == std::string::npos &&
              enc.find('=') == std::string::npos,
          "emits URL-safe alphabet with no padding", enc);

    std::string ignored;
    check(!base64UrlDecode("abc$def", ignored), "rejects invalid characters");
}

// ---------------------------------------------------------------------------
// Signalling blob
// ---------------------------------------------------------------------------
void testSignalBlob() {
    section("signalling blob");

    const std::string sdp = sampleSdp();

    // -- unencrypted --
    const std::string plain = encodeSignalBlob(sdp, "");
    check(!plain.empty() && plain.rfind("SOI1:", 0) == 0, "encodes with the SOI1: prefix");

    std::string out, err;
    check(decodeSignalBlob(plain, "", out, err) && out == sdp,
          "unencrypted round-trip is byte-exact", err);

    info(soi::format("{} byte SDP -> {} char blob ({:.0f}% of raw)",
                     sdp.size(), plain.size(), 100.0 * plain.size() / sdp.size()));
    check(plain.size() < sdp.size(),
          "compression actually shrinks a real SDP");

    // -- encrypted --
    const std::string pass = "correct-horse-battery-staple";
    const std::string enc  = encodeSignalBlob(sdp, pass);
    check(!enc.empty(), "encrypts without error");

    out.clear();
    check(decodeSignalBlob(enc, pass, out, err) && out == sdp,
          "encrypted round-trip is byte-exact", err);

    // Two encryptions of the same input must differ: salt and IV are random.
    const std::string enc2 = encodeSignalBlob(sdp, pass);
    check(enc != enc2, "salt/IV are fresh per encryption");

    // -- negative cases --
    out.clear();
    check(!decodeSignalBlob(enc, "wrong-passphrase", out, err),
          "rejects a wrong passphrase");
    info("reported as: " + err);

    out.clear();
    check(!decodeSignalBlob(enc, "", out, err),
          "rejects an encrypted blob with no passphrase");

    // Flip one ciphertext byte: GCM must catch it.
    std::string tampered = enc;
    const size_t mid = tampered.size() / 2;
    tampered[mid] = (tampered[mid] == 'A') ? 'B' : 'A';
    out.clear();
    check(!decodeSignalBlob(tampered, pass, out, err),
          "GCM tag catches a single flipped byte");

    check(!decodeSignalBlob("hello world", "", out, err), "rejects non-blob input");

    // -- input tolerance --
    out.clear();
    check(decodeSignalBlob("  \r\n" + plain + "  \n", "", out, err) && out == sdp,
          "tolerates surrounding whitespace");

    out.clear();
    check(decodeSignalBlob("file:///C:/x/viewer.html#" + plain, "", out, err) && out == sdp,
          "accepts a pasted URL with the blob in the fragment");
}

// ---------------------------------------------------------------------------
// Colour conversion
// ---------------------------------------------------------------------------
void testColorConvert() {
    section("BGRA -> NV12 (BT.709 limited)");

    info(soi::format("{} path, {} worker(s)",
                     colorConvertSimdName(), sharedPool().size()));

    struct Probe { const char* name; uint8_t b, g, r; double y, u, v; };
    // Reference values from the BT.709 studio-swing matrix, computed independently
    // of the implementation's fixed-point constants.
    auto refY = [](double r, double g, double b) { return 0.1826 * r + 0.6142 * g + 0.0620 * b + 16.0; };
    auto refU = [](double r, double g, double b) { return -0.1006 * r - 0.3386 * g + 0.4392 * b + 128.0; };
    auto refV = [](double r, double g, double b) { return 0.4392 * r - 0.3989 * g - 0.0403 * b + 128.0; };

    Probe probes[] = {
        {"black",  0,   0,   0,   0, 0, 0},
        {"white",  255, 255, 255, 0, 0, 0},
        {"red",    0,   0,   255, 0, 0, 0},
        {"green",  0,   255, 0,   0, 0, 0},
        {"blue",   255, 0,   0,   0, 0, 0},
        {"grey",   128, 128, 128, 0, 0, 0},
    };
    for (auto& p : probes) {
        p.y = refY(p.r, p.g, p.b);
        p.u = refU(p.r, p.g, p.b);
        p.v = refV(p.r, p.g, p.b);
    }

    const int W = 64, H = 64;
    std::vector<uint8_t> bgra(static_cast<size_t>(W) * H * 4);
    Nv12Buffer nv12;

    bool colourOk = true;
    std::string worst;
    double worstErr = 0;

    for (const auto& p : probes) {
        for (size_t i = 0; i < bgra.size(); i += 4) {
            bgra[i + 0] = p.b; bgra[i + 1] = p.g; bgra[i + 2] = p.r; bgra[i + 3] = 255;
        }
        bgraToNv12(bgra.data(), W * 4, W, H, nv12);

        const double gotY = nv12.y()[0];
        const double gotU = nv12.uv()[0];
        const double gotV = nv12.uv()[1];

        const double eY = std::fabs(gotY - p.y);
        const double eU = std::fabs(gotU - p.u);
        const double eV = std::fabs(gotV - p.v);
        const double e  = std::max({eY, eU, eV});

        if (e > worstErr) {
            worstErr = e;
            worst = soi::format("{}: Y {:.0f} (want {:.1f}), U {:.0f} (want {:.1f}), "
                                "V {:.0f} (want {:.1f})",
                                p.name, gotY, p.y, gotU, p.u, gotV, p.v);
        }
        if (e > 1.5) colourOk = false;
    }
    check(colourOk, "primaries convert within 1.5 of the BT.709 reference", worst);
    info("largest deviation -- " + worst);

    // Black must land on 16, not 0: getting this wrong means full-range output
    // that a limited-range decoder will crush.
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i + 0] = bgra[i + 1] = bgra[i + 2] = 0; bgra[i + 3] = 255;
    }
    bgraToNv12(bgra.data(), W * 4, W, H, nv12);
    check(nv12.y()[0] == 16, "black maps to Y=16 (limited range, not full)",
          soi::format("got {}", nv12.y()[0]));

    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i + 0] = bgra[i + 1] = bgra[i + 2] = 255; bgra[i + 3] = 255;
    }
    bgraToNv12(bgra.data(), W * 4, W, H, nv12);
    check(nv12.y()[0] >= 234 && nv12.y()[0] <= 236, "white maps to Y=235",
          soi::format("got {}", nv12.y()[0]));

    // Neutral grey must not drift off 128 -- that is what a colour cast looks like.
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i + 0] = bgra[i + 1] = bgra[i + 2] = 128; bgra[i + 3] = 255;
    }
    bgraToNv12(bgra.data(), W * 4, W, H, nv12);
    check(std::abs(int(nv12.uv()[0]) - 128) <= 1 && std::abs(int(nv12.uv()[1]) - 128) <= 1,
          "neutral grey keeps chroma at 128 (no colour cast)",
          soi::format("U={} V={}", nv12.uv()[0], nv12.uv()[1]));

    // Buffer geometry must be exactly NV12.
    bgraToNv12(bgra.data(), W * 4, W, H, nv12);
    check(nv12.size() == static_cast<size_t>(W) * H * 3 / 2,
          "NV12 buffer is exactly w*h*3/2");

    // A gradient exercises every code path, including the scalar tail of the
    // SIMD loop (width 66 is not a multiple of 8).
    const int GW = 66, GH = 34;
    std::vector<uint8_t> grad(static_cast<size_t>(GW) * GH * 4);
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < GW; ++x) {
            uint8_t* px = &grad[(static_cast<size_t>(y) * GW + x) * 4];
            px[0] = static_cast<uint8_t>(x * 3);
            px[1] = static_cast<uint8_t>(y * 7);
            px[2] = static_cast<uint8_t>((x + y) * 2);
            px[3] = 255;
        }
    bgraToNv12(grad.data(), GW * 4, GW, GH, nv12);

    bool tailOk = true;
    for (int x = 0; x < GW; ++x) {
        const uint8_t* px = &grad[static_cast<size_t>(x) * 4];
        const double want = refY(px[2], px[1], px[0]);
        if (std::fabs(nv12.y()[x] - want) > 1.5) { tailOk = false; break; }
    }
    check(tailOk, "SIMD tail handles a non-multiple-of-8 width (66px)");

    // Throughput, 1080p.
    const int PW = 1920, PH = 1080;
    std::vector<uint8_t> big(static_cast<size_t>(PW) * PH * 4, 0x80);
    bgraToNv12(big.data(), PW * 4, PW, PH, nv12);   // warm up

    const auto t0 = std::chrono::steady_clock::now();
    constexpr int kIters = 50;
    for (int i = 0; i < kIters; ++i) bgraToNv12(big.data(), PW * 4, PW, PH, nv12);
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
            .count() / kIters;

    info(soi::format("1080p 1:1 conversion: {:.2f} ms/frame ({:.0f} fps ceiling)",
                     ms, 1000.0 / ms));
    checkTiming(ms < 8.0, "1080p conversion fits a 30fps budget", soi::format("{:.2f} ms", ms));

    // --- downscaling path -----------------------------------------------------
    // This replaced GDI's HALFTONE StretchBlt, which cost ~16 ms/frame.
    const int SW = 1920, SH = 1200, DW = 1280, DH = 800;
    std::vector<uint8_t> srcBig(static_cast<size_t>(SW) * SH * 4);

    // Flat grey must survive a downscale untouched.
    for (size_t i = 0; i < srcBig.size(); i += 4) {
        srcBig[i] = srcBig[i + 1] = srcBig[i + 2] = 128; srcBig[i + 3] = 255;
    }
    bgraToNv12(srcBig.data(), SW * 4, SW, SH, DW, DH, nv12);

    check(nv12.width() == DW && nv12.height() == DH,
          "downscale produces the requested destination size",
          soi::format("{}x{}", nv12.width(), nv12.height()));

    const uint8_t greyY = nv12.y()[0];
    bool flatOk = true;
    for (int y = 0; y < DH && flatOk; ++y)
        for (int x = 0; x < DW; ++x)
            if (std::abs(int(nv12.y()[static_cast<size_t>(y) * nv12.stride() + x]) - int(greyY)) > 1) {
                flatOk = false; break;
            }
    check(flatOk, "a flat field downscales to a flat field (no ringing or edge artefacts)");
    check(std::abs(int(greyY) - int(refY(128, 128, 128))) <= 1,
          "downscaled grey keeps the right luma", soi::format("Y={}", greyY));
    check(std::abs(int(nv12.uv()[0]) - 128) <= 1 && std::abs(int(nv12.uv()[1]) - 128) <= 1,
          "downscaled grey keeps chroma neutral");

    // Half black / half white: the box filter must preserve both plateaus and
    // only blend at the seam. Nearest-neighbour would too, but a broken box
    // filter shows up as a wide grey band.
    for (int y = 0; y < SH; ++y)
        for (int x = 0; x < SW; ++x) {
            uint8_t* px = &srcBig[(static_cast<size_t>(y) * SW + x) * 4];
            const uint8_t v = (x < SW / 2) ? 0 : 255;
            px[0] = px[1] = px[2] = v; px[3] = 255;
        }
    bgraToNv12(srcBig.data(), SW * 4, SW, SH, DW, DH, nv12);

    const uint8_t leftY  = nv12.y()[DW / 4];
    const uint8_t rightY = nv12.y()[DW * 3 / 4];
    check(leftY == 16 && rightY >= 234,
          "downscale preserves black and white plateaus",
          soi::format("left Y={}, right Y={}", leftY, rightY));

    int blended = 0;
    for (int x = 0; x < DW; ++x) {
        const uint8_t v = nv12.y()[x];
        if (v > 20 && v < 230) ++blended;
    }
    check(blended <= 2, "only the seam blends (box filter, not a blur)",
          soi::format("{} intermediate columns", blended));

    bgraToNv12(srcBig.data(), SW * 4, SW, SH, DW, DH, nv12);   // warm up
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < kIters; ++i)
        bgraToNv12(srcBig.data(), SW * 4, SW, SH, DW, DH, nv12);
    const double msScaled =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1)
            .count() / kIters;

    info(soi::format("1920x1200 -> 1280x800 convert+downscale: {:.2f} ms/frame "
                     "(GDI HALFTONE StretchBlt was ~16.7 ms just to scale)", msScaled));
    checkTiming(msScaled < 8.0, "scaled conversion fits a 30fps budget",
          soi::format("{:.2f} ms", msScaled));
}

// ---------------------------------------------------------------------------
// Thread pool
// ---------------------------------------------------------------------------
void testThreadPool() {
    section("thread pool");

    ThreadPool pool;
    info(soi::format("{} participating thread(s)", pool.size()));

    // Every index must be visited exactly once, across many sizes.
    bool coverageOk = true;
    std::string detail;
    for (size_t n : {size_t(0), size_t(1), size_t(2), size_t(7), size_t(64),
                     size_t(1000), size_t(4099)}) {
        std::vector<std::atomic<int>> hits(n ? n : 1);
        for (auto& h : hits) h.store(0);

        pool.parallelFor(n, [&](size_t begin, size_t end) {
            for (size_t i = begin; i < end; ++i) hits[i].fetch_add(1);
        });

        for (size_t i = 0; i < n; ++i) {
            if (hits[i].load() != 1) {
                coverageOk = false;
                detail = soi::format("n={} index {} visited {}x", n, i, hits[i].load());
                break;
            }
        }
        if (!coverageOk) break;
    }
    check(coverageOk, "every index visited exactly once across 7 sizes", detail);

    // Back-to-back jobs are where a lingering worker from the previous job could
    // corrupt the completion counter. 5000 tiny jobs makes that window likely.
    std::atomic<uint64_t> total{0};
    const auto t0 = std::chrono::steady_clock::now();
    for (int iter = 0; iter < 5000; ++iter) {
        pool.parallelFor(97, [&](size_t begin, size_t end) {
            uint64_t local = 0;
            for (size_t i = begin; i < end; ++i) local += i;
            total.fetch_add(local, std::memory_order_relaxed);
        });
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    const uint64_t expected = 5000ULL * (96ULL * 97ULL / 2ULL);
    check(total.load() == expected,
          "5000 back-to-back jobs produce the exact expected sum",
          soi::format("got {}, want {}", total.load(), expected));
    info(soi::format("5000 dispatches in {:.2f}s ({:.0f} us each)",
                     elapsed, elapsed * 1e6 / 5000));

    // Nested/serial sanity: a job that itself takes uneven time per chunk.
    std::atomic<int> uneven{0};
    pool.parallelFor(64, [&](size_t begin, size_t end) {
        for (size_t i = begin; i < end; ++i) {
            volatile double sink = 0;
            for (int k = 0; k < static_cast<int>(i) * 200; ++k) sink += k;
            (void)sink;
            uneven.fetch_add(1);
        }
    });
    check(uneven.load() == 64, "work-stealing completes an unbalanced job");
}

// ---------------------------------------------------------------------------
// H.264 level table
// ---------------------------------------------------------------------------
void testLevels() {
    section("H.264 level selection");

    struct Case { int w, h, fps, want; const char* why; };
    const Case cases[] = {
        {1280, 720,  30, 31, "720p30 fits level 3.1"},
        {1920, 1080, 30, 40, "1080p30 needs 4.0, NOT the commonly hardcoded 3.1"},
        {1920, 1080, 60, 42, "1080p60 needs 4.2"},
        {3840, 2160, 30, 51, "4K30 needs 5.1"},
        {640,  480,  30, 30, "480p30 fits level 3.0"},
    };

    for (const auto& c : cases) {
        const int got = h264LevelForResolution(c.w, c.h, c.fps);
        check(got == c.want, soi::format("{}x{}@{} -> level {}", c.w, c.h, c.fps, c.want),
              got == c.want ? "" : soi::format("got {} ({})", got, c.why));
    }

    // The SDP advertises this as two hex digits; 1080p30 must read "4d0028".
    check(soi::format("4d00{:02x}", h264LevelForResolution(1920, 1080, 30)) == "4d0028",
          "profile-level-id for 1080p30 is 4d0028 (Main 4.0)");
}

// ---------------------------------------------------------------------------
// Quality levels and the adaptation rule
//
// These pin the property the whole strategy rests on: when bandwidth falls,
// BITS PER FRAME must stay put and the FRAME RATE must absorb it. Get this
// backwards and small text turns to mush on exactly the slow links where
// someone most needs to read it.
// ---------------------------------------------------------------------------
void testQuality() {
    section("quality levels");

    check(defaultQualityLevel().name == "720p", "the default level is 720p");
    check(findQualityLevel("720P") != nullptr, "level lookup is case-insensitive");
    check(findQualityLevel("notalevel") == nullptr, "an unknown level name is rejected");
    check(findQualityLevel("") == nullptr, "an empty level name is rejected");

    // Every level must be at least 480p-capable except the explicit low one, and
    // the ladder must be monotonic or the viewer's menu would be nonsense.
    {
        bool ordered = true;
        int prevH = -1, prevRate = -1;
        for (const auto& l : qualityLevels()) {
            if (l.height != 0) {                 // 0 == source, always last
                if (l.height <= prevH) ordered = false;
                prevH = l.height;
            }
            if (l.bitrateKbps <= prevRate) ordered = false;
            prevRate = l.bitrateKbps;
        }
        check(ordered, "levels are ordered smallest to largest");
    }

    // --- encode sizing ------------------------------------------------------
    struct SizeCase { const char* level; int srcW, srcH, wantW, wantH; const char* why; };
    const SizeCase sizes[] = {
        {"720p",  1920, 1200, 1152, 720, "16:10 source keeps its aspect ratio"},
        {"720p",  1920, 1080, 1280, 720, "16:9 source gives exactly 1280x720"},
        {"480p",  1920, 1200,  768, 480, "480p from 16:10"},
        {"1080p", 1280,  720, 1280, 720, "never upscales past the source"},
        {"source",1920, 1200, 1920, 1200,"source level is the capture size"},
        {"720p",  1366,  768, 1280, 720, "odd widths still land on even numbers"},
    };
    for (const auto& c : sizes) {
        const QualityLevel* level = findQualityLevel(c.level);
        int w = 0, h = 0;
        if (level) encodeSizeFor(*level, c.srcW, c.srcH, w, h);
        check(level && w == c.wantW && h == c.wantH,
              soi::format("{} from {}x{} -> {}x{}", c.level, c.srcW, c.srcH, c.wantW, c.wantH),
              level ? soi::format("got {}x{} ({})", w, h, c.why) : "no such level");
    }

    {
        bool allEven = true;
        for (const auto& l : qualityLevels())
            for (const auto& src : {std::pair{1920, 1200}, std::pair{1366, 768},
                                    std::pair{1024, 600},  std::pair{2560, 1440}}) {
                int w = 0, h = 0;
                encodeSizeFor(l, src.first, src.second, w, h);
                if ((w & 1) || (h & 1) || w <= 0 || h <= 0) allEven = false;
            }
        check(allEven, "every level yields even, positive dimensions (NV12 needs 2x2 chroma)");
    }

    // --- the adaptation rule ------------------------------------------------
    const QualityLevel& hd = *findQualityLevel("720p");
    const int budget = bitsPerFrame(hd);
    check(budget == hd.bitrateKbps * 1000 / hd.fps, "bits/frame is bitrate divided by frame rate");

    check(frameRateForBandwidth(hd, hd.bitrateKbps, 2) == hd.fps,
          "full bandwidth runs at the nominal frame rate");
    check(frameRateForBandwidth(hd, hd.bitrateKbps / 2, 2) == hd.fps / 2,
          "half the bandwidth halves the FRAME RATE");
    check(frameRateForBandwidth(hd, hd.bitrateKbps * 4, 2) == hd.fps,
          "surplus bandwidth does not push past the nominal rate");
    check(frameRateForBandwidth(hd, 1, 2) == 2, "a starved link falls back to the floor, not to 0");
    check(frameRateForBandwidth(hd, 0, 3) == 3, "no estimate yet means the floor");

    // The property that matters: across the whole usable range, the bits each
    // frame gets never falls below the level's design budget.
    {
        bool held = true;
        int worst = budget;
        for (int avail = hd.bitrateKbps; avail >= hd.bitrateKbps / 10; avail -= 25) {
            const int fps  = frameRateForBandwidth(hd, avail, 2);
            const int kbps = bitrateForFrameRate(hd, fps);
            const int per  = (kbps * 1000) / std::max(1, fps);
            worst = std::min(worst, per);
            // The bitrate is expressed in whole kbps, so half a kbps of rounding
            // is the tightest guarantee available -- that is 500/fps bits per
            // frame. Anything worse than that is a real loss of quality, not
            // arithmetic.
            if (per < budget - (500 / std::max(1, fps)) - 1) held = false;
        }
        check(held,
              "bits per frame never drop as bandwidth falls -- sharpness is held, fps gives way",
              soi::format("design {} bits/frame, worst observed {}", budget, worst));
    }

    // And the bitrate handed to the encoder must actually fit the link, or we
    // would be holding quality by overrunning the pipe.
    {
        bool fits = true;
        for (int avail = hd.bitrateKbps; avail >= 100; avail -= 37) {
            const int fps = frameRateForBandwidth(hd, avail, 2);
            if (fps > 2 && bitrateForFrameRate(hd, fps) > avail) fits = false;
        }
        check(fits, "the chosen bitrate never exceeds what the link reported");
    }

    check(bitrateForFrameRate(hd, hd.fps) == hd.bitrateKbps,
          "at the nominal frame rate the bitrate is the level's own");

    // --- respecting what the peer can decode --------------------------------
    //
    // This is the bug that made the quality control look broken. Chrome answers
    // with profile-level-id=4d001f -- level 3.1, ceiling 3600 macroblocks, which
    // is EXACTLY 1280x720 -- no matter what the offer asked for. Everything
    // above 720p was therefore sent outside the negotiated envelope: measured
    // against a real Chrome at 1920x1200, four freezes totalling 2.1 seconds in
    // a five-second sample.
    check(h264LevelFromSdp("a=fmtp:96 profile-level-id=4d001f;packetization-mode=1") == 0x1f,
          "reads level 3.1 out of the fmtp line Chrome actually sends");
    check(h264LevelFromSdp("a=fmtp:96 profile-level-id=4d0033") == 0x33,
          "reads a raised level 5.1");
    check(h264LevelFromSdp("v=0\r\na=sendrecv\r\n") == 0,
          "no fmtp means no stated constraint");
    check(h264LevelFromSdp("profile-level-id=zzzz33") == 0, "malformed fmtp is not trusted");

    check(maxFrameMacroblocksForLevel(0x1f) == 3600, "level 3.1 allows 3600 macroblocks");
    check(maxFrameMacroblocksForLevel(0x33) == 36864, "level 5.1 allows 36864 macroblocks");
    check(maxFrameMacroblocksForLevel(99) == 0, "an unknown level constrains nothing");

    {
        // 1280x720 is 80x45 = 3600 macroblocks: level 3.1 exactly, so untouched.
        int w = 1280, h = 720;
        clampToMacroblocks(3600, w, h);
        check(w == 1280 && h == 720, "720p is left alone under level 3.1",
              soi::format("got {}x{}", w, h));
    }
    {
        // 1920x1200 is 9000 macroblocks -- two and a half times what 3.1 allows.
        int w = 1920, h = 1200;
        clampToMacroblocks(3600, w, h);
        const long long mbs = static_cast<long long>((w + 15) / 16) * ((h + 15) / 16);
        check(mbs <= 3600 && w > 0 && h > 0 && !(w & 1) && !(h & 1),
              "an oversized frame is shrunk to fit the peer's level",
              soi::format("1920x1200 -> {}x{} = {} macroblocks", w, h, mbs));
        check(std::abs((static_cast<double>(w) / h) - (1920.0 / 1200.0)) < 0.05,
              "and keeps its aspect ratio while doing so",
              soi::format("{}x{}", w, h));
    }
    {
        int w = 1920, h = 1200;
        clampToMacroblocks(0, w, h);
        check(w == 1920 && h == 1200, "an unknown peer level does not shrink anything");
    }
}

#if defined(_WIN32)
// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------
void testCapture() {
    section("BitBlt capture");

    const auto monitors = enumerateMonitors();
    check(!monitors.empty(), "enumerates at least one monitor");
    for (const auto& m : monitors)
        info(soi::format("monitor {}: {} {}x{} at ({},{}){}", m.index, m.name,
                         m.width, m.height, m.x, m.y, m.primary ? " [primary]" : ""));

    const auto windows = enumerateWindows();
    check(!windows.empty(), "enumerates capturable windows");
    info(soi::format("{} capturable window(s); first few:", windows.size()));
    for (size_t i = 0; i < windows.size() && i < 3; ++i)
        info(soi::format("  {} ({}) {}x{}", windows[i].title, windows[i].process,
                         windows[i].width, windows[i].height));

    if (monitors.empty()) return;

    CaptureConfig cfg;
    cfg.target       = CaptureTarget::Monitor;
    cfg.monitorIndex = 0;
    cfg.maxWidth     = 1280;          // force the scaling path

    BitBltCapture capture(cfg);
    check(capture.start(), "starts on monitor 0");
    if (capture.width() == 0) return;

    info("target: " + capture.describe());
    check(capture.width() % 2 == 0 && capture.height() % 2 == 0,
          "output dimensions are even (required for NV12)",
          soi::format("{}x{}", capture.width(), capture.height()));
    check(capture.width() <= 1280, "honours --max-width downscaling",
          soi::format("width {}", capture.width()));

    const Frame* f = capture.capture();
    check(f && f->data, "captures a frame");
    if (!f || !f->data) return;

    // --- switching target mid-session ---------------------------------------
    //
    // The viewer can change which screen it is watching without the share code,
    // the peer connection or anything else being disturbed. What matters most
    // here is the FAILURE path: asking for a monitor that is not there must
    // leave a working capture, not a dead one, because the alternative is the
    // person watching loses the picture by clicking the wrong menu entry.
    {
        CaptureConfig missing = cfg;
        missing.monitorIndex = 99;
        const int wasW = capture.width(), wasH = capture.height();

        check(!capture.retarget(missing),
              "a retarget to a monitor that does not exist reports failure");
        check(capture.width() == wasW && capture.height() == wasH,
              "and restores the previous target exactly",
              soi::format("{}x{} -> {}x{}", wasW, wasH, capture.width(), capture.height()));

        const Frame* after = capture.capture();
        check(after && after->data, "and still captures frames afterwards");
    }
    {
        // Retargeting to the monitor already in use is a legitimate no-op.
        check(capture.retarget(cfg), "retargets to a valid monitor");
        const Frame* again = capture.capture();
        check(again && again->data, "captures a frame after retargeting");
    }
    if (monitors.size() > 1) {
        CaptureConfig other = cfg;
        other.monitorIndex = monitors[1].index;
        check(capture.retarget(other), "retargets to a second monitor");
        check(capture.width() > 0 && capture.height() > 0 &&
                  !(capture.width() & 1) && !(capture.height() & 1),
              "second monitor yields usable even dimensions",
              soi::format("{}x{}", capture.width(), capture.height()));
        capture.retarget(cfg);
    } else {
        info("only one monitor present; the multi-monitor switch path is untested here");
    }

    // Frames come back at NATIVE resolution; width()/height() are the post-scale
    // size the encoder is configured for, and the conversion pass bridges them.
    check(f->width == capture.nativeWidth() && f->height == capture.nativeHeight() &&
              f->stride == f->width * 4,
          "frames are native-resolution and stride is w*4",
          soi::format("frame {}x{} stride {}, native {}x{}, encode {}x{}",
                      f->width, f->height, f->stride, capture.nativeWidth(),
                      capture.nativeHeight(), capture.width(), capture.height()));

    // A real desktop is never uniformly one colour. An all-identical buffer means
    // the blit silently produced nothing -- the classic BitBlt black-frame failure.
    uint64_t sum = 0;
    bool     varied = false;
    const uint32_t first = *reinterpret_cast<const uint32_t*>(f->data);
    for (int y = 0; y < f->height; y += 4) {
        const auto* row = reinterpret_cast<const uint32_t*>(f->data + static_cast<size_t>(y) * f->stride);
        for (int x = 0; x < f->width; x += 4) {
            if (row[x] != first) varied = true;
            sum += row[x] & 0xFF;
        }
    }
    check(varied, "captured pixels are not a uniform block (blit really ran)");
    info(soi::format("mean blue channel across samples: {}",
                     sum / std::max<uint64_t>(1, (f->height / 4) * (f->width / 4))));

    // Duplicate detection: a static screen must be recognised on the second grab.
    const Frame* f1 = capture.capture();
    const Frame* f2 = capture.capture();
    check(f1 && f2, "captures repeatedly");
    if (f2) {
        info(soi::format("second consecutive frame flagged duplicate: {}",
                         f2->duplicate ? "yes" : "no (screen changed)"));
    }

    // Throughput, measured both with and without CAPTUREBLT. The flag is what
    // pulls layered/transparent windows into the blit, and it is widely
    // recommended online without mentioning what it costs -- so measure it.
    auto benchmark = [](bool layered, int& outW, int& outH) -> double {
        CaptureConfig c;
        c.target = CaptureTarget::Monitor;
        c.monitorIndex = 0;
        c.maxWidth = 1280;
        c.includeLayered = layered;

        BitBltCapture cap(c);
        if (!cap.start()) return -1.0;
        // Report the NATIVE size: the blit is 1:1, so that is what is being timed.
        outW = cap.nativeWidth();
        outH = cap.nativeHeight();

        cap.capture();   // warm up

        constexpr int kIters = 20;
        const auto t0 = std::chrono::steady_clock::now();
        int ok = 0;
        for (int i = 0; i < kIters; ++i)
            if (cap.capture()) ++ok;
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count() / kIters;
        cap.stop();
        return ok == kIters ? ms : -1.0;
    };

    int bw = 0, bh = 0;
    const double msPlain   = benchmark(false, bw, bh);
    const double msLayered = benchmark(true,  bw, bh);

    check(msPlain > 0 && msLayered > 0, "20 consecutive captures succeed in both modes");
    info(soi::format("{}x{}  without CAPTUREBLT: {:.2f} ms/frame ({:.0f} fps ceiling)",
                     bw, bh, msPlain, 1000.0 / msPlain));
    info(soi::format("{}x{}  with    CAPTUREBLT: {:.2f} ms/frame ({:.0f} fps ceiling)  "
                     "-- {:.1f}x slower",
                     bw, bh, msLayered, 1000.0 / msLayered, msLayered / msPlain));

    check(msPlain < 33.0, "default capture path sustains 30fps",
          soi::format("{:.2f} ms", msPlain));

    capture.stop();
}

// ---------------------------------------------------------------------------
// The other two backends, and the automatic choice between all three.
//
// The claim being tested is the one the whole design rests on: whatever is on
// the screen reaches the encoder, whichever API happens to work on this machine.
// So every backend is held to the same bar -- it starts, it produces a frame,
// and the frame is not a black rectangle -- and then the factory is checked to
// have picked a good one without being told.
// ---------------------------------------------------------------------------

// A real desktop is never one flat colour. This is the check that catches the
// failure mode the whole exercise is about: an API that reports success and
// hands back nothing.
bool frameHasContent(const Frame& f) {
    if (!f.data || f.width <= 0 || f.height <= 0) return false;
    const uint32_t first = *reinterpret_cast<const uint32_t*>(f.data);
    for (int y = 0; y < f.height; y += 4) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            f.data + static_cast<size_t>(y) * f.stride);
        for (int x = 0; x < f.width; x += 4)
            if (row[x] != first) return true;
    }
    return false;
}

// Grabs until something with new content turns up. DXGI and WGC both answer
// "nothing changed" instantly on a still desktop, so a single grab proves
// nothing either way.
const Frame* captureUntilContent(FrameSource& src, int attempts = 40) {
    const Frame* last = nullptr;
    for (int i = 0; i < attempts; ++i) {
        if (const Frame* f = src.capture()) {
            last = f;
            if (frameHasContent(*f)) return f;
        }
        std::this_thread::sleep_for(15ms);
    }
    return last;
}

void testCaptureBackends() {
    section("capture backends (DXGI duplication, WGC, and the automatic choice)");

    if (enumerateMonitors().empty()) { info("no monitors; skipped"); return; }

    const bool haveDxgi = DxgiCapture::available();
    const bool haveWgc  = WgcCapture::available();
    info(soi::format("DXGI Desktop Duplication available: {}", haveDxgi ? "yes" : "no"));
    info(soi::format("Windows.Graphics.Capture available: {}", haveWgc ? "yes" : "no"));

    CaptureConfig cfg;
    cfg.target       = CaptureTarget::Monitor;
    cfg.monitorIndex = 0;
    cfg.maxWidth     = 1280;

    // --- each backend on its own ---------------------------------------------
    struct Result { bool started = false; bool content = false; double ms = 0; };
    Result results[3];
    const CaptureBackend backends[3] = {CaptureBackend::Dxgi, CaptureBackend::Wgc,
                                        CaptureBackend::BitBlt};

    for (int i = 0; i < 3; ++i) {
        CaptureConfig c = cfg;
        c.backend = backends[i];

        std::unique_ptr<FrameSource> src;
        switch (backends[i]) {
            case CaptureBackend::Dxgi:   src = std::make_unique<DxgiCapture>(c);   break;
            case CaptureBackend::Wgc:    src = std::make_unique<WgcCapture>(c);    break;
            default:                     src = std::make_unique<BitBltCapture>(c); break;
        }

        const char* name = backendName(backends[i]);
        results[i].started = src->start();
        if (!results[i].started) {
            info(soi::format("{}: does not start on this machine", name));
            continue;
        }

        check(src->width() % 2 == 0 && src->height() % 2 == 0,
              soi::format("{}: encode dimensions are even (required for NV12)", name),
              soi::format("{}x{}", src->width(), src->height()));
        check(src->width() <= 1280, soi::format("{}: honours --max-width", name),
              soi::format("width {}", src->width()));
        check(src->backend() == backends[i],
              soi::format("{}: reports its own backend", name));

        const Frame* f = captureUntilContent(*src);
        results[i].content = f && frameHasContent(*f);
        check(results[i].content,
              soi::format("{}: captures a frame with real screen content", name),
              f ? soi::format("{}x{} stride {}", f->width, f->height, f->stride)
                : "no frame at all");

        if (f) {
            check(f->width == src->nativeWidth() && f->height == src->nativeHeight() &&
                      f->stride == f->width * 4,
                  soi::format("{}: frames are native-resolution, stride w*4", name));
        }

        constexpr int kIters = 20;
        const auto t0 = std::chrono::steady_clock::now();
        for (int n = 0; n < kIters; ++n) src->capture();
        results[i].ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count() / kIters;
        info(soi::format("{}: {:.2f} ms per grab on a still screen", name, results[i].ms));

        src->stop();
    }

    check(results[0].content || results[1].content || results[2].content,
          "at least one backend can read this screen");

    // The point of adding duplication: it costs a fraction of what re-reading
    // the screen through GDI costs, because the compositor answers "nothing
    // changed" instead of us having to look.
    if (results[0].content && results[2].content && results[2].ms > 0) {
        info(soi::format("dxgi is {:.0f}x cheaper than bitblt per grab",
                         results[2].ms / std::max(0.001, results[0].ms)));
    }

    // --- the automatic choice -------------------------------------------------
    {
        auto src = createFrameSource(cfg);
        check(src->start(), "the factory starts something for a monitor target");
        if (src->width() > 0) {
            const CaptureBackend chose = src->backend();
            info(soi::format("factory chose: {}", backendName(chose)));

            // Order matters, not just success: picking GDI while duplication
            // works would mean fullscreen games silently go black again.
            if (haveDxgi)
                check(chose == CaptureBackend::Dxgi,
                      "prefers Desktop Duplication when it is available");
            else if (haveWgc)
                check(chose == CaptureBackend::Wgc,
                      "falls to Windows.Graphics.Capture when duplication is not available");

            const Frame* f = captureUntilContent(*src);
            check(f && frameHasContent(*f),
                  "the automatically chosen backend produces real content");

            // Retarget has to survive a bad index without killing the capture --
            // the viewer can ask for a monitor that was just unplugged.
            CaptureConfig missing = cfg;
            missing.monitorIndex = 99;
            const int wasW = src->width(), wasH = src->height();
            check(!src->retarget(missing),
                  "a retarget to a monitor that does not exist reports failure");
            check(src->width() == wasW && src->height() == wasH,
                  "and restores the previous target exactly",
                  soi::format("{}x{} -> {}x{}", wasW, wasH, src->width(), src->height()));
            check(captureUntilContent(*src) != nullptr, "and still captures afterwards");

            // "All screens" is what a viewer picks from the browser, and on a
            // multi-monitor PC it is the one target WGC cannot serve -- so this
            // is the path that proves a refused leaf backend really does fall
            // through to one that can, instead of reporting a switch it did not
            // make. On a single-monitor machine it is a plain no-op.
            CaptureConfig everything = cfg;
            everything.target = CaptureTarget::VirtualDesktop;
            check(src->retarget(everything),
                  "retargets to every screen at once",
                  soi::format("{}x{} via {}", src->width(), src->height(),
                              backendName(src->backend())));
            check(captureUntilContent(*src) != nullptr,
                  "and captures real content from all screens");
            check(src->width() >= wasW && src->height() >= wasH,
                  "which is at least as large as the single screen it replaced",
                  soi::format("{}x{} -> {}x{}", wasW, wasH, src->width(), src->height()));

            src->retarget(cfg);
        }
        src->stop();
    }

    // --- window targets -------------------------------------------------------
    //
    // Duplication cannot address a window, so the factory must not try it. This
    // is also the case GDI is worst at: PrintWindow returns black for anything
    // DirectComposition-backed, which today is most of what people share.
    {
        const auto windows = enumerateWindows();
        if (windows.empty()) {
            info("no capturable windows; the window path is untested here");
        } else {
            CaptureConfig w;
            w.target       = CaptureTarget::Window;
            w.windowHandle = windows[0].handle;
            w.maxWidth     = 1280;

            DxgiCapture dxgiWindow(w);
            check(!dxgiWindow.start(),
                  "duplication refuses a window target rather than pretending");

            auto src = createFrameSource(w);
            if (src->start()) {
                info(soi::format("window '{}' captured via {}", windows[0].title,
                                 backendName(src->backend())));
                check(src->backend() != CaptureBackend::Dxgi,
                      "the factory never routes a window to duplication");
                if (haveWgc)
                    check(src->backend() == CaptureBackend::Wgc,
                          "and prefers WGC for windows, which GDI renders black");
                src->stop();
            } else {
                info("no backend could capture the first enumerated window");
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Overlays.
//
// "Overlay" on Windows means two different things and they capture differently:
//
//   * a LAYERED, topmost window -- Discord's in-game overlay, Steam, most
//     on-screen widgets. It is composed by DWM, so DXGI and WGC see it. Plain
//     GDI BitBlt does NOT, unless you pass CAPTUREBLT (--layered).
//   * a HARDWARE overlay plane (MPO) -- a video player handing its frames
//     straight to the display controller. Nothing composes it into a CPU
//     surface... until a duplication or WGC capture goes active, at which point
//     the OS disables the overlay optimisation and composites normally, so the
//     capture is complete. That path needs a video playing and cannot be
//     asserted in a headless test; it is documented in README 1.2a.
//
// This proves the first, which is the one that is testable and the one people
// actually mean when they say "does it get the overlay".
// ---------------------------------------------------------------------------
void testOverlayCapture() {
    section("overlays (a layered, topmost window across every backend)");

    if (enumerateMonitors().empty()) { info("no monitors; skipped"); return; }

    constexpr COLORREF kOverlayColour = RGB(0, 255, 0);   // pure green
    constexpr uint32_t kOverlayBgra   = 0x0000FF00;
    constexpr int X = 200, Y = 200, W = 360, H = 240;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(kOverlayColour);
    wc.lpszClassName = L"SoiOverlayProbe";
    RegisterClassExW(&wc);

    // WS_EX_LAYERED is the flag that makes this an overlay for capture purposes.
    HWND overlay = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
                                   wc.lpszClassName, L"soi overlay probe",
                                   WS_POPUP | WS_VISIBLE, X, Y, W, H,
                                   nullptr, nullptr, wc.hInstance, nullptr);
    if (!overlay) { check(false, "creates a layered overlay window"); return; }
    // Fully opaque on purpose. WS_EX_LAYERED is what makes a window an "overlay"
    // for capture -- a plain screen BitBlt historically misses it while DXGI/WGC
    // compose it in. Alpha < 255 would blend the fill with whatever is behind it,
    // so the captured pixels would no longer be pure green and an exact-colour
    // count would (correctly) find none. 255 keeps the pixels pure while leaving
    // the window genuinely layered.
    SetLayeredWindowAttributes(overlay, 0, 255, LWA_ALPHA);

    auto pump = [](int ms) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        MSG msg;
        while (std::chrono::steady_clock::now() < until) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            std::this_thread::sleep_for(10ms);
        }
    };
    SetWindowPos(overlay, HWND_TOPMOST, X, Y, W, H, SWP_SHOWWINDOW | SWP_NOACTIVATE);
    // A layered popup with no owner is never invalidated on its own, so force the
    // erase-to-green and let DWM compose it before anything is captured.
    InvalidateRect(overlay, nullptr, TRUE);
    UpdateWindow(overlay);
    pump(500);

    int mon0X = 0, mon0Y = 0;
    if (const auto mons = enumerateMonitors(); !mons.empty()) {
        mon0X = mons[0].x; mon0Y = mons[0].y;
    }

    auto countOverlay = [&](FrameSource& src) -> int {
        const Frame* f = captureUntilContent(src);
        if (!f || !f->data) return -1;
        const int rx = X - mon0X, ry = Y - mon0Y;
        int hits = 0;
        for (int y = std::max(0, ry); y < std::min(ry + H, f->height); ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                f->data + static_cast<size_t>(y) * f->stride);
            for (int x = std::max(0, rx); x < std::min(rx + W, f->width); ++x)
                if ((row[x] & 0x00FFFFFF) == kOverlayBgra) ++hits;
        }
        return hits;
    };

    CaptureConfig base;
    base.target = CaptureTarget::Monitor;
    base.monitorIndex = 0;
    base.maxWidth = 0;
    base.detectDuplicates = false;

    // DXGI: reads the composed scanout, so a layered window is simply in it.
    if (DxgiCapture::available()) {
        CaptureConfig c = base; c.backend = CaptureBackend::Dxgi;
        DxgiCapture dxgi(c);
        if (dxgi.start()) {
            const int hits = countOverlay(dxgi);
            check(hits > 10000, "DXGI Desktop Duplication captures a layered overlay",
                  soi::format("{} overlay px", hits));
            dxgi.stop();
        }
    }

    // WGC: composes through DWM, same result.
    if (WgcCapture::available()) {
        CaptureConfig c = base; c.backend = CaptureBackend::Wgc;
        WgcCapture wgc(c);
        if (wgc.start()) {
            const int hits = countOverlay(wgc);
            check(hits > 10000, "Windows.Graphics.Capture captures a layered overlay",
                  soi::format("{} overlay px", hits));
            wgc.stop();
        }
    }

    // GDI: needs CAPTUREBLT for layered windows. Prove BOTH halves of that so the
    // --layered flag's reason to exist is documented, not folklore.
    {
        CaptureConfig plain = base; plain.backend = CaptureBackend::BitBlt;
        plain.includeLayered = false;
        BitBltCapture gdi(plain);
        if (gdi.start()) {
            const int hits = countOverlay(gdi);
            // On a modern DWM desktop a screen-DC BitBlt reads the COMPOSED
            // desktop, so it usually does pick up layered windows too -- the
            // pre-DWM "BitBlt misses layered windows" rule no longer holds here.
            // Report what was actually measured rather than assert the old rule.
            info(soi::format("GDI BitBlt without CAPTUREBLT: {} overlay px {}",
                             hits, hits > 10000 ? "(DWM composited it in anyway)"
                                                : "(missed -- CAPTUREBLT would be needed)"));
            gdi.stop();
        }

        CaptureConfig layered = base; layered.backend = CaptureBackend::BitBlt;
        layered.includeLayered = true;
        BitBltCapture gdiL(layered);
        if (gdiL.start()) {
            const int hits = countOverlay(gdiL);
            check(hits > 10000, "GDI BitBlt WITH CAPTUREBLT (--layered) captures the overlay",
                  soi::format("{} overlay px", hits));
            gdiL.stop();
        }
    }

    // The bottom line the whole design turns on: the default automatic path gets
    // the overlay with no flag at all, because it prefers a backend that composes.
    {
        auto src = createFrameSource(base);
        if (src->start()) {
            const int hits = countOverlay(*src);
            check(hits > 10000,
                  "the automatic path captures the overlay with no --layered flag",
                  soi::format("via {}: {} overlay px", backendName(src->backend()), hits));
            src->stop();
        }
    }

    DestroyWindow(overlay);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// ---------------------------------------------------------------------------
// Capture protection
// ---------------------------------------------------------------------------
void testProtection() {
    section("capture protection");

    const bool supported = excludeFromCaptureSupported();
    info(soi::format("WDA_EXCLUDEFROMCAPTURE supported: {}", supported ? "yes" : "no"));
    info(describeProtection(ProtectMode::ExcludeFromCapture));

    // Create a real window rather than borrowing the console: under a test runner
    // there may be no console at all, and this is the one feature that must never
    // silently go untested. Placed off-screen so it disturbs nothing.
    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SoiProtectionProbe";
    RegisterClassExW(&wc);

    HWND probe = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"probe",
                                 WS_POPUP, -20000, -20000, 120, 80,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    check(probe != nullptr, "creates a probe window to test against");
    if (!probe) return;
    ShowWindow(probe, SW_SHOWNA);

    // Baseline: a fresh window must report WDA_NONE.
    DWORD affinity = 0xFFFFFFFF;
    check(GetWindowDisplayAffinity(probe, &affinity) && affinity == 0,
          "a fresh window starts at WDA_NONE", soi::format("got 0x{:02X}", affinity));

    check(applyProtection(probe, ProtectMode::ExcludeFromCapture),
          "applies WDA_EXCLUDEFROMCAPTURE");

    // Verify by asking Windows back, not by trusting our own return value.
    affinity = 0xFFFFFFFF;
    check(GetWindowDisplayAffinity(probe, &affinity) != FALSE,
          "GetWindowDisplayAffinity reads the setting back");
    info(soi::format("affinity now 0x{:02X} (0x11 = excluded, 0x01 = blacked out)",
                     affinity));
    check(affinity == (supported ? 0x11u : 0x01u),
          "Windows reports exactly the affinity we asked for");

    check(applyProtection(probe, ProtectMode::BlackOut), "can switch to WDA_MONITOR");
    GetWindowDisplayAffinity(probe, &affinity);
    check(affinity == 0x01u, "WDA_MONITOR reads back as 0x01",
          soi::format("got 0x{:02X}", affinity));

    check(applyProtection(probe, ProtectMode::None), "clears protection again");
    GetWindowDisplayAffinity(probe, &affinity);
    check(affinity == 0, "affinity is back to WDA_NONE");

    // protectOwnWindows must find and mark this window, since we own it.
    const int marked = protectOwnWindows(ProtectMode::ExcludeFromCapture);
    check(marked >= 1, "protectOwnWindows marks our own top-level windows",
          soi::format("marked {}", marked));
    GetWindowDisplayAffinity(probe, &affinity);
    check(affinity == (supported ? 0x11u : 0x01u),
          "the probe window really was marked by the sweep");

    DestroyWindow(probe);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// ---------------------------------------------------------------------------
// Capture protection, proved against real captured pixels.
//
// Every other check in this file trusts SetWindowDisplayAffinity's return value.
// This one puts a uniquely-coloured window on the actual screen, captures the
// desktop the same way any other capture tool would, and counts the pixels.
// ---------------------------------------------------------------------------
void testProtectionEffective() {
    section("capture protection -- proved against captured pixels");

    // A colour that does not occur on an ordinary desktop.
    constexpr COLORREF kProbeColour = RGB(255, 0, 255);
    constexpr uint32_t kProbeBgra   = 0x00FF00FF;   // B=FF, G=00, R=FF

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(kProbeColour);
    wc.lpszClassName = L"SoiVisibleProbe";
    RegisterClassExW(&wc);

    constexpr int X = 140, Y = 140, W = 420, H = 280;
    HWND probe = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"soi probe",
                                 WS_POPUP | WS_VISIBLE, X, Y, W, H,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    check(probe != nullptr, "creates a visible on-screen probe window");
    if (!probe) return;

    auto pump = [](int ms) {
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(ms);
        MSG msg;
        while (std::chrono::steady_clock::now() < until) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            std::this_thread::sleep_for(10ms);
        }
    };

    CaptureConfig cfg;
    cfg.target       = CaptureTarget::Monitor;
    cfg.monitorIndex = 0;
    cfg.maxWidth     = 0;              // 1:1, so screen coords map directly
    cfg.detectDuplicates = false;

    BitBltCapture capture(cfg);
    if (!capture.start()) { check(false, "capture starts for the proof"); return; }

    // Counts probe-coloured pixels inside the window's screen rectangle.
    auto countProbePixels = [&]() -> int {
        const Frame* f = capture.capture();
        if (!f || !f->data) return -1;
        int hits = 0;
        const int y1 = std::min(Y + H, f->height);
        const int x1 = std::min(X + W, f->width);
        for (int y = Y; y < y1; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                f->data + static_cast<size_t>(y) * f->stride);
            for (int x = X; x < x1; ++x)
                if ((row[x] & 0x00FFFFFF) == kProbeBgra) ++hits;
        }
        return hits;
    };

    SetForegroundWindow(probe);
    pump(400);

    const int before = countProbePixels();
    check(before > 10000, "the unprotected probe window IS captured",
          soi::format("{} probe-coloured pixels", before));

    // --- the actual claim -----------------------------------------------------
    check(applyProtection(probe, ProtectMode::ExcludeFromCapture),
          "applies WDA_EXCLUDEFROMCAPTURE to the visible window");
    pump(400);

    const int excluded = countProbePixels();
    check(excluded == 0,
          "WDA_EXCLUDEFROMCAPTURE: the window is COMPLETELY absent from the capture",
          soi::format("{} probe-coloured pixels leaked", excluded));
    info(soi::format("probe pixels: {} unprotected -> {} excluded", before, excluded));

    // --- WDA_MONITOR blacks out instead of hiding ------------------------------
    check(applyProtection(probe, ProtectMode::BlackOut), "switches to WDA_MONITOR");
    pump(400);
    const int blacked = countProbePixels();
    check(blacked == 0, "WDA_MONITOR also keeps the colour out of the capture",
          soi::format("{} pixels", blacked));

    // --- and it comes back when cleared, proving the capture really was live ---
    check(applyProtection(probe, ProtectMode::None), "clears protection");
    pump(400);
    const int after = countProbePixels();
    check(after > 10000,
          "the window reappears once cleared (so the capture was genuinely live)",
          soi::format("{} probe-coloured pixels", after));

    // --- the watchdog re-protects a window it has never seen before ------------
    {
        ProtectionWatchdog watchdog;
        watchdog.start(ProtectMode::ExcludeFromCapture, 200);
        pump(700);

        DWORD affinity = 0;
        GetWindowDisplayAffinity(probe, &affinity);
        check(affinity != 0,
              "the watchdog re-protects a window created after startup",
              soi::format("affinity 0x{:02X} after {} sweeps", affinity,
                          watchdog.sweeps()));

        const int watched = countProbePixels();
        check(watched == 0, "and the re-protected window is absent from capture",
              soi::format("{} pixels", watched));
        watchdog.stop();
    }

    capture.stop();
    DestroyWindow(probe);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    // --- what protection does NOT cover, stated by the code, not just the docs -
    std::string note;
    const bool consoleOk = protectConsoleWindow(ProtectMode::ExcludeFromCapture, note);
    info(soi::format("console/terminal window: {}", note));
    if (!consoleOk)
        info("=> anything printed in the terminal (including the offer blob) IS "
             "visible to screen capture. Share a window, or a different monitor.");
}

// ---------------------------------------------------------------------------
// Protection against EVERY user-mode capture path, not just ours.
//
// WDA_EXCLUDEFROMCAPTURE is enforced by DWM at composition time, so in principle
// it applies to any capture API. "In principle" is not evidence, so this checks
// the three paths real screen recorders actually use:
//
//   1. GDI BitBlt from the screen DC     -- old capture tools, our own capture
//   2. GDI PrintWindow on the window     -- per-window capture
//   3. DXGI Desktop Duplication          -- OBS, Discord, Teams, TeamViewer
//
// Windows.Graphics.Capture is the fourth modern path. It is not exercised here
// (it needs a WinRT capture-item interop setup); it goes through the same DWM
// composition step as Desktop Duplication, so the same enforcement applies.
// ---------------------------------------------------------------------------
class DesktopDuplication {
public:
    bool init() {
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                       nullptr, 0, D3D11_SDK_VERSION,
                                       dev_.put(), &level, ctx_.put());
        if (FAILED(hr)) return false;

        ComPtr<IDXGIDevice> dxgiDev;
        if (FAILED(dev_->QueryInterface(IID_PPV_ARGS(dxgiDev.put())))) return false;

        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(dxgiDev->GetAdapter(adapter.put()))) return false;

        ComPtr<IDXGIOutput> output;
        if (FAILED(adapter->EnumOutputs(0, output.put()))) return false;

        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output->QueryInterface(IID_PPV_ARGS(output1.put())))) return false;

        return SUCCEEDED(output1->DuplicateOutput(dev_.get(), dup_.put()));
    }

    // Counts pixels of `bgr` inside the given desktop rectangle, or -1 on failure.
    int countColour(uint32_t bgr, int rx, int ry, int rw, int rh) {
        if (!dup_) return -1;

        ComPtr<IDXGIResource> resource;
        DXGI_OUTDUPL_FRAME_INFO info{};

        // The first acquire after a change can return an empty frame; retry.
        HRESULT hr = DXGI_ERROR_WAIT_TIMEOUT;
        for (int attempt = 0; attempt < 12; ++attempt) {
            resource.reset();
            hr = dup_->AcquireNextFrame(500, &info, resource.put());
            if (SUCCEEDED(hr)) break;
            if (hr == DXGI_ERROR_WAIT_TIMEOUT) continue;
            return -1;
        }
        if (FAILED(hr) || !resource) return -1;

        ComPtr<ID3D11Texture2D> frame;
        if (FAILED(resource->QueryInterface(IID_PPV_ARGS(frame.put())))) {
            dup_->ReleaseFrame();
            return -1;
        }

        D3D11_TEXTURE2D_DESC desc{};
        frame->GetDesc(&desc);

        if (!staging_) {
            D3D11_TEXTURE2D_DESC sd = desc;
            sd.Usage          = D3D11_USAGE_STAGING;
            sd.BindFlags      = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags      = 0;
            if (FAILED(dev_->CreateTexture2D(&sd, nullptr, staging_.put()))) {
                dup_->ReleaseFrame();
                return -1;
            }
        }

        ctx_->CopyResource(staging_.get(), frame.get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(ctx_->Map(staging_.get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            dup_->ReleaseFrame();
            return -1;
        }

        int hits = 0;
        const int yEnd = std::min<int>(ry + rh, static_cast<int>(desc.Height));
        const int xEnd = std::min<int>(rx + rw, static_cast<int>(desc.Width));
        for (int y = ry; y < yEnd; ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                static_cast<const uint8_t*>(mapped.pData) +
                static_cast<size_t>(y) * mapped.RowPitch);
            for (int x = rx; x < xEnd; ++x)
                if ((row[x] & 0x00FFFFFF) == bgr) ++hits;
        }

        ctx_->Unmap(staging_.get(), 0);
        dup_->ReleaseFrame();
        return hits;
    }

private:
    ComPtr<ID3D11Device>           dev_;
    ComPtr<ID3D11DeviceContext>    ctx_;
    ComPtr<IDXGIOutputDuplication> dup_;
    ComPtr<ID3D11Texture2D>        staging_;
};

void testProtectionAllModes() {
    section("protection vs EVERY capture mode");

    constexpr COLORREF kProbeColour = RGB(255, 0, 255);
    constexpr uint32_t kProbeBgra   = 0x00FF00FF;
    constexpr int X = 160, Y = 160, W = 420, H = 280;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = DefWindowProcW;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hbrBackground = CreateSolidBrush(kProbeColour);
    wc.lpszClassName = L"SoiAllModesProbe";
    RegisterClassExW(&wc);

    HWND probe = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName, L"soi probe",
                                 WS_POPUP | WS_VISIBLE, X, Y, W, H,
                                 nullptr, nullptr, wc.hInstance, nullptr);
    if (!probe) { check(false, "creates the probe window"); return; }

    auto pump = [](int ms) {
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(ms);
        MSG msg;
        while (std::chrono::steady_clock::now() < until) {
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            std::this_thread::sleep_for(10ms);
        }
    };

    // --- mode 1: GDI BitBlt from the screen DC -------------------------------
    CaptureConfig cfg;
    cfg.target = CaptureTarget::Monitor;
    cfg.monitorIndex = 0;
    cfg.maxWidth = 0;
    cfg.detectDuplicates = false;
    BitBltCapture bitblt(cfg);
    bitblt.start();

    auto countBitBlt = [&]() -> int {
        const Frame* f = bitblt.capture();
        if (!f || !f->data) return -1;
        int hits = 0;
        for (int y = Y; y < std::min(Y + H, f->height); ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                f->data + static_cast<size_t>(y) * f->stride);
            for (int x = X; x < std::min(X + W, f->width); ++x)
                if ((row[x] & 0x00FFFFFF) == kProbeBgra) ++hits;
        }
        return hits;
    };

    // --- mode 2: GDI PrintWindow directly on the window ----------------------
    auto countPrintWindow = [&]() -> int {
        HDC screen = GetDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HDC mem = CreateCompatibleDC(screen);
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(mem, bmp);
        ReleaseDC(nullptr, screen);

        PrintWindow(probe, mem, 2 /* PW_RENDERFULLCONTENT */);
        GdiFlush();

        int hits = 0;
        const auto* px = static_cast<const uint32_t*>(bits);
        for (int i = 0; i < W * H; ++i)
            if ((px[i] & 0x00FFFFFF) == kProbeBgra) ++hits;

        DeleteObject(bmp);
        DeleteDC(mem);
        return hits;
    };

    // --- mode 3: DXGI Desktop Duplication ------------------------------------
    DesktopDuplication ddup;
    const bool ddupOk = ddup.init();
    info(soi::format("DXGI Desktop Duplication available: {}", ddupOk ? "yes" : "no"));

    // --- mode 4: Windows.Graphics.Capture ------------------------------------
    //
    // The path README 1.2 previously called "inference, not measurement": it
    // composes through the same DWM step as duplication, so the same enforcement
    // *should* apply. Now that a real WGC capture exists in-process, prove it
    // instead of arguing it. This is the whole point of the exercise for the
    // sharer -- WGC is exactly the API a screen recorder would use, and if our
    // own console leaked through it while we shared, the offer blob would be
    // sitting in the friend's stream.
    CaptureConfig wgcCfg;
    wgcCfg.target           = CaptureTarget::Monitor;
    wgcCfg.monitorIndex     = 0;
    wgcCfg.maxWidth         = 0;       // native, so screen coords map 1:1
    wgcCfg.detectDuplicates = false;
    WgcCapture wgc(wgcCfg);
    const bool wgcOk = wgc.start();
    info(soi::format("Windows.Graphics.Capture available: {}", wgcOk ? "yes" : "no"));

    // Monitor 0's origin, so the probe's screen rectangle can be expressed in the
    // monitor-relative coordinates a WGC frame uses.
    int mon0X = 0, mon0Y = 0;
    if (const auto mons = enumerateMonitors(); !mons.empty()) {
        mon0X = mons[0].x;
        mon0Y = mons[0].y;
    }

    // WGC only yields a frame when the content changes, so pump first to let the
    // recomposite land, then take the freshest of a few grabs.
    auto countWgc = [&]() -> int {
        if (!wgcOk) return -1;
        const Frame* f = nullptr;
        for (int i = 0; i < 40; ++i) {
            if (const Frame* n = wgc.capture()) f = n;
            std::this_thread::sleep_for(12ms);
            if (f && i >= 6) break;   // have something, and gave it time to settle
        }
        if (!f || !f->data) return -1;

        const int rx = X - mon0X, ry = Y - mon0Y;
        int hits = 0;
        for (int y = std::max(0, ry); y < std::min(ry + H, f->height); ++y) {
            const auto* row = reinterpret_cast<const uint32_t*>(
                f->data + static_cast<size_t>(y) * f->stride);
            for (int x = std::max(0, rx); x < std::min(rx + W, f->width); ++x)
                if ((row[x] & 0x00FFFFFF) == kProbeBgra) ++hits;
        }
        return hits;
    };

    SetForegroundWindow(probe);
    pump(500);

    const int bitbltBefore = countBitBlt();
    const int printBefore  = countPrintWindow();
    const int ddupBefore   = ddupOk ? ddup.countColour(kProbeBgra, X, Y, W, H) : -1;
    const int wgcBefore    = countWgc();

    check(bitbltBefore > 10000, "unprotected: visible to GDI BitBlt",
          soi::format("{} px", bitbltBefore));
    check(printBefore > 10000, "unprotected: visible to GDI PrintWindow",
          soi::format("{} px", printBefore));
    if (ddupOk)
        check(ddupBefore > 10000, "unprotected: visible to DXGI Desktop Duplication",
              soi::format("{} px", ddupBefore));
    if (wgcOk)
        check(wgcBefore > 10000, "unprotected: visible to Windows.Graphics.Capture",
              soi::format("{} px", wgcBefore));

    // --- protect, then re-check every path -----------------------------------
    applyProtection(probe, ProtectMode::ExcludeFromCapture);
    pump(500);

    const int bitbltAfter = countBitBlt();
    const int printAfter  = countPrintWindow();
    const int ddupAfter   = ddupOk ? ddup.countColour(kProbeBgra, X, Y, W, H) : -1;
    const int wgcAfter    = countWgc();

    check(bitbltAfter == 0, "PROTECTED: absent from GDI BitBlt",
          soi::format("{} px leaked", bitbltAfter));
    check(printAfter == 0, "PROTECTED: absent from GDI PrintWindow",
          soi::format("{} px leaked", printAfter));
    if (ddupOk)
        check(ddupAfter == 0, "PROTECTED: absent from DXGI Desktop Duplication",
              soi::format("{} px leaked", ddupAfter));
    if (wgcOk)
        check(wgcAfter == 0, "PROTECTED: absent from Windows.Graphics.Capture",
              soi::format("{} px leaked", wgcAfter));

    info(soi::format("BitBlt {} -> {} | PrintWindow {} -> {} | DXGI {} -> {} | WGC {} -> {}",
                     bitbltBefore, bitbltAfter, printBefore, printAfter,
                     ddupBefore, ddupAfter, wgcBefore, wgcAfter));

    wgc.stop();
    bitblt.stop();
    DestroyWindow(probe);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}

// ---------------------------------------------------------------------------
// Encoder
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// The GPU pipeline, end to end.
//
// Everything else about this path can look right and still produce nothing: the
// shader can compile, the device can be shared, the MFT can accept the D3D
// manager -- and if the NV12 render target views address the wrong plane, or the
// DXGI buffer goes in with a zero length, the encoder emits a green rectangle or
// no bytes at all and says so to nobody.
//
// So this drives the real thing: duplicate the real screen onto the GPU, convert
// with the real shader, hand the real texture to the real hardware encoder, and
// insist on an H.264 bitstream coming out the other side.
// ---------------------------------------------------------------------------
// The shader's arithmetic, against the same reference values the SSE2 path is
// held to (black -> 16, white -> 235, grey chroma exactly 128).
//
// This is the check the end-to-end test cannot make. A shader that writes the
// Y plane into the UV view, or uses BT.601, or gets the limited-range scaling
// wrong, still encodes to a plausible number of bytes -- it just looks wrong,
// and nothing but a human would notice. Reading the NV12 back here (test only;
// the real path never does) turns that into an assertion.
void testGpuConvert() {
    section("GPU BGRA -> NV12 shader (BT.709 limited)");

    auto device = GpuDevice::create(nullptr);
    if (!device) { info("no D3D11 device available; skipped"); return; }
    info("device: " + device->describe());

    constexpr int kW = 64, kH = 64;

    // Four quadrants of known colour. Quadrant-sized rather than per-pixel so
    // the box filter has a flat interior to average, and any plane mix-up moves
    // a whole block rather than a sliver.
    struct Quad { uint8_t b, g, r; const char* name; int wantY; };
    const Quad quads[4] = {
        {0,   0,   0,   "black", 16},
        {255, 255, 255, "white", 235},
        {128, 128, 128, "grey",  126},   // 16 + 219*(128/255)
        {0,   0,   255, "red",    63},   // 16 + 219*0.2126
    };

    std::vector<uint32_t> src(static_cast<size_t>(kW) * kH);
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) {
            const Quad& q = quads[(y < kH / 2 ? 0 : 2) + (x < kW / 2 ? 0 : 1)];
            src[static_cast<size_t>(y) * kW + x] =
                (0xFFu << 24) | (static_cast<uint32_t>(q.r) << 16) |
                (static_cast<uint32_t>(q.g) << 8) | q.b;
        }

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kW; td.Height = kH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = src.data();
    init.SysMemPitch = kW * 4;

    ComPtr<ID3D11Texture2D> source;
    if (FAILED(device->device()->CreateTexture2D(&td, &init, source.put()))) {
        info("could not create the source texture; skipped");
        return;
    }

    Nv12GpuConverter conv;
    if (!conv.init(device, kW, kH)) {
        info("this device cannot render NV12; skipped");
        return;
    }

    ID3D11Texture2D* nv12 = conv.convert(source.get());
    check(nv12 != nullptr, "the shader converts a BGRA texture to NV12");
    if (!nv12) return;

    // Read it back. Only the test does this -- it is precisely the bus crossing
    // the GPU pipeline exists to avoid.
    D3D11_TEXTURE2D_DESC sd{};
    nv12->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device->device()->CreateTexture2D(&sd, nullptr, staging.put()))) {
        info("could not create a staging texture to read the result back");
        return;
    }

    D3D11_MAPPED_SUBRESOURCE map{};
    {
        std::lock_guard lk(device->contextMutex());
        device->context()->CopyResource(staging.get(), nv12);
        if (FAILED(device->context()->Map(staging.get(), 0, D3D11_MAP_READ, 0, &map))) {
            info("could not map the NV12 result");
            return;
        }
    }

    const auto* base = static_cast<const uint8_t*>(map.pData);
    const int   pitch = static_cast<int>(map.RowPitch);
    // NV12 is planar: the UV plane starts exactly `height` rows after Y.
    const uint8_t* yPlane  = base;
    const uint8_t* uvPlane = base + static_cast<size_t>(pitch) * kH;

    auto sampleY = [&](int x, int y) { return yPlane[static_cast<size_t>(y) * pitch + x]; };

    bool lumaOk = true;
    std::string detail;
    for (int q = 0; q < 4; ++q) {
        // Well inside each quadrant, away from the seam the box filter blends.
        const int x = (q % 2 == 0 ? kW / 4 : kW * 3 / 4);
        const int y = (q < 2 ? kH / 4 : kH * 3 / 4);
        const int got = sampleY(x, y);
        if (std::abs(got - quads[q].wantY) > 2) lumaOk = false;
        detail += soi::format("{} {}(want {}) ", quads[q].name, got, quads[q].wantY);
    }
    check(lumaOk, "luma matches the BT.709 limited-range reference", detail);

    // Grey is the sharpest chroma test there is: any hue error at all moves U or
    // V off 128, and a BT.601 matrix would not land here either.
    const int greyX = kW / 4, greyY = kH * 3 / 4;
    const int u = uvPlane[static_cast<size_t>(greyY / 2) * pitch + (greyX / 2) * 2];
    const int v = uvPlane[static_cast<size_t>(greyY / 2) * pitch + (greyX / 2) * 2 + 1];
    check(std::abs(u - 128) <= 1 && std::abs(v - 128) <= 1,
          "grey is exactly neutral chroma, so the UV plane is addressed correctly",
          soi::format("U={} V={}", u, v));

    // Red must push V well above neutral and U below it. This is the assertion
    // that fails outright if the two chroma channels are swapped.
    const int redX = kW * 3 / 4, redY = kH * 3 / 4;
    const int ru = uvPlane[static_cast<size_t>(redY / 2) * pitch + (redX / 2) * 2];
    const int rv = uvPlane[static_cast<size_t>(redY / 2) * pitch + (redX / 2) * 2 + 1];
    check(rv > 180 && ru < 120,
          "red lands on the right chroma axis (U and V are not swapped)",
          soi::format("U={} V={}", ru, rv));

    {
        std::lock_guard lk(device->contextMutex());
        device->context()->Unmap(staging.get(), 0);
    }
}

void testGpuPipeline() {
    section("GPU pipeline (capture -> shader -> encoder, no readback)");

    if (enumerateMonitors().empty()) { info("no monitors; skipped"); return; }

    CaptureConfig cfg;
    cfg.target       = CaptureTarget::Monitor;
    cfg.backend      = CaptureBackend::Dxgi;
    cfg.monitorIndex = 0;
    cfg.maxWidth     = 1280;
    cfg.preferGpu    = true;

    DxgiCapture capture(cfg);
    if (!capture.start()) {
        info("Desktop Duplication unavailable here; GPU pipeline skipped");
        return;
    }

    auto device = capture.gpuDevice();
    check(device != nullptr,
          "the DXGI backend keeps frames on the GPU when asked",
          device ? device->describe() : "no GPU device");
    if (!device) { capture.stop(); return; }

    // A GPU frame is a texture and nothing else: `data` being null is the
    // property the whole pipeline depends on, so assert it rather than assume.
    const Frame* f = nullptr;
    for (int i = 0; i < 200 && !f; ++i) {
        f = capture.capture();
        if (f && !f->gpuTexture) f = nullptr;
        if (!f) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    check(f != nullptr, "produces a GPU frame");
    if (!f) { capture.stop(); return; }

    check(f->data == nullptr && f->gpuTexture != nullptr,
          "the frame is a texture, with no CPU copy taken",
          soi::format("{}x{}", f->width, f->height));

    // --- and now the encoder, on that same device --------------------------
    EncoderConfig ecfg;
    ecfg.width = capture.width() & ~1;
    ecfg.height = capture.height() & ~1;
    ecfg.fps = 30;
    ecfg.bitrateKbps = 3000;
    ecfg.gopSeconds = 2;

    std::mutex mtx;
    int    frames = 0, keyframes = 0;
    size_t bytes  = 0;
    bool   annexB = true;

    H264Encoder encoder;
    const bool gpuOffered = encoder.enableGpuInput(device);
    if (!gpuOffered) {
        info("this machine's H.264 MFT does not take D3D11 input; GPU encode skipped");
        capture.stop();
        return;
    }

    const bool started = encoder.start(ecfg,
        [&](const uint8_t* nal, size_t len, bool key, int64_t) {
            std::lock_guard lk(mtx);
            ++frames;
            bytes += len;
            if (key) ++keyframes;
            if (len < 4 || nal[0] != 0 || nal[1] != 0 ||
                !((nal[2] == 1) || (nal[2] == 0 && nal[3] == 1)))
                annexB = false;
        });

    check(started, "starts an encoder on the capture's own device");
    if (!started) { capture.stop(); return; }

    check(encoder.usesGpuInput(),
          "the encoder really took the D3D11 device (zero-copy input)",
          encoder.describe());

    if (!encoder.usesGpuInput()) { encoder.stop(); capture.stop(); return; }

    encoder.requestKeyframe();

    // Paced rather than blasted: the submit queue drops the oldest when it is
    // full by design, so a tight loop would measure the drop policy instead of
    // the pipeline.
    int submitted = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (submitted < 30 && std::chrono::steady_clock::now() < deadline) {
        const Frame* g = capture.capture();
        if (g && g->gpuTexture) {
            if (encoder.submitTexture(static_cast<ID3D11Texture2D*>(g->gpuTexture),
                                      static_cast<int64_t>(submitted) * 33'333'333LL))
                ++submitted;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    // The encoder is asynchronous; give the last frames a moment to come back.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    encoder.stop();
    capture.stop();

    check(submitted > 0, "textures are accepted by the encoder",
          soi::format("{} submitted", submitted));

    std::lock_guard lk(mtx);
    check(frames > 0, "the GPU pipeline produces encoded frames",
          soi::format("{} frames, {} bytes from {} textures", frames, bytes, submitted));
    check(keyframes > 0, "including at least one keyframe");
    check(annexB, "every access unit is Annex-B, exactly as on the CPU path");
    // A shader writing to the wrong NV12 plane still encodes -- into a few
    // hundred bytes of flat colour per frame. Real screen content does not
    // compress anywhere near that far, so a size floor catches it.
    check(bytes > static_cast<size_t>(frames) * 200,
          "the bitstream carries real content, not a flat frame",
          soi::format("{} bytes over {} frames", bytes, frames));
}
#endif // _WIN32

void testEncoder() {
#if defined(_WIN32)
    section("Media Foundation H.264 encoder");
#else
    section("VideoToolbox H.264 encoder");
#endif

    EncoderConfig cfg;
    cfg.width = 1280; cfg.height = 720; cfg.fps = 30;
    cfg.bitrateKbps = 3000; cfg.gopSeconds = 2;

    std::mutex              mtx;
    std::condition_variable cv;
    int                     frames = 0, keyframes = 0;
    size_t                  totalBytes = 0;
    bool                    sawSps = false, allAnnexB = true;

    // Start-code census. The RTP packetizer must be told which Annex-B separator
    // the encoder actually uses; if Media Foundation mixes 3-byte and 4-byte
    // start codes (it does) and the packetizer is configured for 4-byte only,
    // every NAL after the first is split wrong and the peer decodes nothing.
    int startCode3 = 0, startCode4 = 0;

    // Everything the encoder emitted, so it can be decoded again below.
    std::vector<std::vector<uint8_t>> accessUnits;

    H264Encoder encoder;
    const bool started = encoder.start(cfg,
        [&](const uint8_t* nal, size_t len, bool key, int64_t /*pts*/) {
            std::lock_guard lk(mtx);
            ++frames;
            totalBytes += len;
            if (key) ++keyframes;
            accessUnits.emplace_back(nal, nal + len);

            for (size_t i = 0; i + 3 < len; ++i) {
                if (nal[i] == 0 && nal[i + 1] == 0) {
                    if (nal[i + 2] == 1) { ++startCode3; i += 2; }
                    else if (nal[i + 2] == 0 && nal[i + 3] == 1) { ++startCode4; i += 3; }
                }
            }

            // Every access unit must begin with an Annex-B start code.
            if (len < 4 || nal[0] != 0 || nal[1] != 0 ||
                !((nal[2] == 1) || (nal[2] == 0 && nal[3] == 1)))
                allAnnexB = false;

            // A keyframe without SPS (NAL type 7) cannot be decoded by a peer
            // that just joined. This is the check that matters most.
            if (key) {
                for (size_t i = 0; i + 4 < len && i < 16; ++i) {
                    if (nal[i] == 0 && nal[i + 1] == 0) {
                        int t = -1;
                        if (nal[i + 2] == 1)                       t = nal[i + 3] & 0x1F;
                        else if (nal[i + 2] == 0 && nal[i + 3] == 1) t = nal[i + 4] & 0x1F;
                        if (t == 7) { sawSps = true; break; }
                    }
                }
            }
            cv.notify_all();
        });

    check(started, "starts an encoder");
    if (!started) return;

    info("using: " + encoder.describe());
    info(soi::format("hardware: {}", encoder.isHardware() ? "yes" : "no (software fallback)"));

    // Pre-render a short cycle of distinct frames. A static image compresses to
    // almost nothing and would not prove the encoder is really working, but
    // regenerating every frame inline would measure the generator, not the codec.
    constexpr int kFrames = 60;
    constexpr int kCycle  = 10;
    std::vector<Nv12Buffer> cycle(kCycle);
    {
        std::vector<uint8_t> bgra(static_cast<size_t>(cfg.width) * cfg.height * 4);
        for (int n = 0; n < kCycle; ++n) {
            for (int y = 0; y < cfg.height; ++y)
                for (int x = 0; x < cfg.width; ++x) {
                    uint8_t* px = &bgra[(static_cast<size_t>(y) * cfg.width + x) * 4];
                    px[0] = static_cast<uint8_t>((x + n * 23) & 0xFF);
                    px[1] = static_cast<uint8_t>((y + n * 11) & 0xFF);
                    px[2] = static_cast<uint8_t>((x ^ y) + n * 17);
                    px[3] = 255;
                }
            bgraToNv12(bgra.data(), cfg.width * 4, cfg.width, cfg.height, cycle[n]);
        }
    }

    // Submit paced at the configured frame rate, exactly as the real capture loop
    // does. Bursting instead would hit the deliberate drop-stale-frames policy and
    // measure nothing useful.
    const auto frameInterval = std::chrono::nanoseconds(1'000'000'000LL / cfg.fps);
    const auto t0 = std::chrono::steady_clock::now();
    auto next = t0;

    for (int n = 0; n < kFrames; ++n) {
        encoder.submit(cycle[n % kCycle], static_cast<int64_t>(n) * 33'333'333LL);
        next += frameInterval;
        std::this_thread::sleep_until(next);
    }

    // Async hardware MFTs produce output on their own thread; allow a drain window.
    {
        std::unique_lock lk(mtx);
        cv.wait_for(lk, 3s, [&] { return frames >= kFrames - 4; });
    }
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    int  encoded = 0, keys = 0;
    size_t bytes = 0;
    bool   annexB = false, sps = false;
    {
        std::lock_guard lk(mtx);
        encoded = frames; keys = keyframes; bytes = totalBytes;
        annexB = allAnnexB; sps = sawSps;
    }

    check(encoded > 0, "produces encoded output", soi::format("{} frames", encoded));
    // A throughput budget, so advisory on shared CI machines like the others.
    checkTiming(encoded >= kFrames * 9 / 10, "encodes at least 90% of a paced 30fps feed",
                soi::format("{} of {}", encoded, kFrames));
    check(keys > 0, "emits at least one keyframe");
    check(annexB, "every access unit starts with an Annex-B start code");
    check(sps, "keyframes carry SPS so a late-joining viewer can decode");

    info(soi::format("Annex-B start codes emitted: {} x 3-byte (00 00 01), "
                     "{} x 4-byte (00 00 00 01)", startCode3, startCode4));
    check(!(startCode3 > 0 && startCode4 > 0) ||
              true,  // reported, not fatal -- but it dictates the packetizer config
          "start-code census recorded");
    if (startCode3 > 0 && startCode4 > 0)
        info("=> MIXED start codes: the RTP packetizer MUST use "
             "NalUnit::Separator::StartSequence, not LongStartSequence");

    if (encoded) {
        const double kbps = bytes * 8.0 / 1000.0 / std::max(0.001, elapsed);
        info(soi::format("{} frames ({} key) in {:.2f}s, {} bytes, ~{:.0f} kbps "
                         "(target {}), avg {} B/frame",
                         encoded, keys, elapsed, bytes, kbps, cfg.bitrateKbps,
                         bytes / encoded));
        // This pattern is deliberately incompressible -- full-frame change every
        // frame, no temporal redundancy -- so it is the worst case any rate
        // control will ever see, and nothing hits its target on it. Measured on
        // the Quick Sync MFT against this same 3000 kbps request:
        //
        //     unconstrained quality (ICQ)   8316 kbps   2.77x   <- rejected
        //     CBR                           4247 kbps   1.42x
        //     peak-constrained VBR          4491 kbps   1.50x   <- chosen
        //
        // VBR costs 6% more overshoot than CBR and in exchange spends bits where
        // the detail actually is, which is what keeps small text readable. The
        // bound below sits above the chosen mode and well below unconstrained,
        // so a regression back to an uncapped mode fails here rather than in
        // someone's living room.
        check(kbps < cfg.bitrateKbps * 1.6,
              "rate control stays within its configured burst headroom",
              soi::format("{:.0f} vs {} kbps", kbps, cfg.bitrateKbps));
    }

    encoder.stop();

#if !defined(_WIN32)
    // Decoding what was encoded is the strongest check there is short of a
    // browser: it proves the Annex-B framing, the SPS/PPS and every slice.
    {
        std::vector<std::vector<uint8_t>> units;
        {
            std::lock_guard lk(mtx);
            units = accessUnits;
        }
        int decoded = 0, width = 0, height = 0;
        std::string detail;
        const bool ok = macDecodeAnnexB(units, decoded, width, height, detail);
        check(ok && decoded >= static_cast<int>(units.size()) * 9 / 10,
              "VideoToolbox decodes the stream back (Annex-B, SPS/PPS, slices all valid)",
              soi::format("{} of {} decoded{}{}", decoded, units.size(),
                          detail.empty() ? "" : ": ", detail));
        check(width == cfg.width && height == cfg.height, "decoded frames have the encoded size",
              soi::format("{}x{}", width, height));
        int profileIdc = 0;
        for (const auto& u : units)
            if (macFindSpsProfile(u, profileIdc)) break;
        check(profileIdc == 77, "the SPS says Main profile, as the SDP advertises (4d00xx)",
              soi::format("profile_idc {}", profileIdc));
    }
#endif

    // --- the drop policy itself, asserted rather than tripped over ------------
    // A burst must be discarded, not queued: a stale screen frame has no value
    // and buffering it would add latency the viewer can never recover.
    {
        H264Encoder burst;
        std::atomic<int> got{0};
        const bool ok = burst.start(cfg, [&](const uint8_t*, size_t, bool, int64_t) {
            got.fetch_add(1);
        });
        check(ok, "starts a second encoder for the burst test");
        if (ok) {
            for (int n = 0; n < 200; ++n)
                burst.submit(cycle[n % kCycle], static_cast<int64_t>(n) * 33'333'333LL);
            std::this_thread::sleep_for(500ms);
            burst.stop();
            check(got.load() < 200,
                  "a 200-frame burst is dropped rather than queued (latency guard)",
                  soi::format("{} encoded", got.load()));
            info(soi::format("burst of 200 -> {} encoded, rest dropped by design",
                             got.load()));
        }
    }
}

// ---------------------------------------------------------------------------
// Interop helper modes
// ---------------------------------------------------------------------------
// Writes bytes verbatim. Windows text-mode stdout rewrites every \n as \r\n,
// which turns an SDP's \r\n line endings into \r\r\n and silently corrupts any
// byte-exact comparison downstream.

// ---------------------------------------------------------------------------
// install / update / control -- the pure logic behind the self-managing exe
// ---------------------------------------------------------------------------
#if defined(_WIN32)
void testPathList() {
    section("user PATH editing");
    const std::string dir = R"(C:\Users\me\AppData\Local\Programs\soi-share)";
    bool changed = false;
    int removed = 0;

    check(pathListAdd("", dir, changed) == dir && changed, "adds to an empty PATH with no separator");
    check(pathListAdd("C:\\a;C:\\b", dir, changed) == "C:\\a;C:\\b;" + dir && changed,
          "appends, never prepends");
    check(pathListAdd("C:\\a;", dir, changed) == "C:\\a;" + dir + ";",
          "a trailing ';' does not produce ';;', and is kept");
    {
        const std::string before = "C:\\a;%USERPROFILE%\\bin;";
        std::string after = pathListAdd(before, dir, changed);
        after = pathListRemove(after, dir, removed);
        check(after == before, "add then remove gives back the exact original value", after);
    }

    const std::string variants[] = {
        dir,
        R"(c:\users\ME\appdata\local\programs\SOI-SHARE)",   // case
        dir + "\\",                                           // trailing backslash
        dir + "\\\\",                                         // several
        "\"" + dir + "\"",                                    // quoted
        "  " + dir + "  ",                                    // blanks
    };
    bool allDedupe = true;
    for (const auto& v : variants) {
        const std::string path = "C:\\a;" + v + ";C:\\b";
        const std::string out = pathListAdd(path, dir, changed);
        if (changed || out != path) { allDedupe = false; info("not recognised: " + v); }
    }
    check(allDedupe, "an existing entry is recognised despite case, quotes, blanks, trailing \\");

    // %LOCALAPPDATA% form, as a hand-edited PATH might hold it.
    const std::string viaEnv = installDirFor(localAppDataDir());
    const std::string envForm = R"(%LOCALAPPDATA%\Programs\soi-share)";
    pathListAdd("C:\\a;" + envForm, viaEnv, changed);
    check(!changed, "an entry written as %LOCALAPPDATA%\\... counts as present");

    check(pathListCount("x;" + dir + ";y;" + dir + "\\;\"" + dir + "\"", dir) == 3,
          "counts every spelling of the entry");

    const std::string mixed = "%SystemRoot%\\system32;" + dir + ";C:\\Tools;" + dir + "\\;C:\\x;";
    const std::string after = pathListRemove(mixed, dir, removed);
    check(removed == 2 && after == "%SystemRoot%\\system32;C:\\Tools;C:\\x;",
          "remove drops every spelling and keeps the rest byte for byte (incl. %VARS%)", after);

    const std::string lookalikes = dir + "2;" + dir + "\\bin;C:\\soi-share";
    check(pathListRemove(lookalikes, dir, removed) == lookalikes && removed == 0,
          "remove leaves look-alike folders alone");
    check(pathListRemove("C:\\a", dir, removed) == "C:\\a" && removed == 0,
          "removing an absent entry changes nothing");
    check(pathListRemove(dir, dir, removed).empty() && removed == 1,
          "removing the only entry leaves an empty PATH");

    std::string round = pathListAdd("C:\\a;C:\\b", dir, changed);
    round = pathListAdd(round, dir, changed);   // idempotent
    check(!changed && pathListCount(round, dir) == 1, "adding twice leaves exactly one entry");
    check(pathListRemove(round, dir, removed) == "C:\\a;C:\\b", "add then remove restores the value");
}

void testInstallDir() {
    section("install location");
    check(installDirFor(R"(C:\Users\me\AppData\Local)") ==
              R"(C:\Users\me\AppData\Local\Programs\soi-share)",
          "%LOCALAPPDATA%\\Programs\\soi-share");
    check(installDirFor(R"(C:\Users\me\AppData\Local\)") ==
              R"(C:\Users\me\AppData\Local\Programs\soi-share)",
          "a trailing backslash on LOCALAPPDATA is tolerated");
    check(!localAppDataDir().empty(), "LOCALAPPDATA resolves", localAppDataDir());
    check(installedExePath() == installDir() + "\\soi-share.exe", "installed exe path");
    check(samePath("C:\\Windows\\", "c:/windows"), "paths compare like Windows does");
    check(samePath("C:\\Windows\\System32\\..", "C:\\Windows"), "'..' is resolved before comparing");
    check(!samePath("C:\\Windows", "C:\\Windows2"), "different folders differ");
}

#else
void testPathList() {
    section("user PATH editing (shell profiles)");
    const std::string dir = installDirFor(localAppDataDir());
    bool changed = false;
    int removed = 0;

    // ':'-separated PATH, used to tell whether this shell already has the dir.
    check(pathListAdd("", dir, changed) == dir && changed, "adds to an empty PATH");
    check(pathListAdd("/usr/bin:/bin", dir, changed) == "/usr/bin:/bin:" + dir && changed,
          "appends, never prepends");
    pathListAdd("/usr/bin:" + dir + "/", dir, changed);
    check(!changed, "a trailing slash is the same folder");
    pathListAdd("/usr/bin:~/.soi-share/bin", dir, changed);
    check(!changed, "~/ is expanded before comparing");
    pathListAdd("/usr/bin:$HOME/.soi-share/bin", dir, changed);
    check(!changed, "$HOME is expanded before comparing");
    check(pathListRemove("/a:" + dir + ":/b", dir, removed) == "/a:/b" && removed == 1,
          "remove drops the entry and keeps the rest");
    check(pathListRemove(dir + "2:/x", dir, removed) == dir + "2:/x" && removed == 0,
          "remove leaves look-alike folders alone");

    // The block written into ~/.zshrc and friends.
    const std::string original = "export EDITOR=vim\nalias ll='ls -l'\n";
    std::string withBlock = profileAddPathBlock(original, dir, false, changed);
    check(changed && withBlock.find("export PATH=\"$PATH:$HOME/.soi-share/bin\"") != std::string::npos,
          "adds an export line, spelled with $HOME", withBlock);
    check(withBlock.rfind(original, 0) == 0, "everything the user wrote is kept, in place");
    std::string twice = profileAddPathBlock(withBlock, dir, false, changed);
    check(!changed && twice == withBlock, "adding twice changes nothing (one block only)");
    check(profileRemovePathBlock(withBlock, removed) == original && removed == 1,
          "remove gives back the exact original file", profileRemovePathBlock(withBlock, removed));
    check(profileRemovePathBlock(original, removed) == original && removed == 0,
          "removing from a file without the block changes nothing");
    const std::string noNewline = "export A=1";
    check(profileRemovePathBlock(profileAddPathBlock(noNewline, dir, false, changed), removed) ==
              noNewline + "\n",
          "a file without a final newline gets one, and nothing else");
    const std::string fish = profileAddPathBlock("", dir, true, changed);
    check(fish.find("set -gx PATH $PATH") != std::string::npos, "fish syntax for fish", fish);
    const std::string mentioned = "# >>> soi-share >>> mentioned mid-line is not a block\n";
    check(profileRemovePathBlock("echo x " + mentioned, removed) == "echo x " + mentioned &&
              removed == 0,
          "a marker that does not start a line is left alone");
}

void testInstallDir() {
    section("install location");
    check(installDirFor("/Users/me") == "/Users/me/.soi-share/bin", "~/.soi-share/bin");
    check(installDirFor("/Users/me/") == "/Users/me/.soi-share/bin",
          "a trailing slash on HOME is tolerated");
    check(!localAppDataDir().empty(), "HOME resolves", localAppDataDir());
    check(installedExePath() == installDir() + "/soi-share", "installed binary path");
    check(samePath("/usr/bin/", "/usr/bin"), "trailing slashes do not matter");
    check(samePath("/usr/bin/../bin", "/usr/bin"), "'..' is resolved before comparing");
    check(!samePath("/usr/bin", "/usr/sbin"), "different folders differ");
    char tmp[] = "/tmp/soi-selftest-XXXXXX";
    if (mkdtemp(tmp)) {
        const std::string real = std::string(tmp) + "/real";
        const std::string link = std::string(tmp) + "/link";
        writeFileBytes(real, "x");
        symlink(real.c_str(), link.c_str());
        check(samePath(real, link), "a symlink and its target are the same path");
        std::string error;
        const std::string target = std::string(tmp) + "/target";
        writeFileBytes(target, "old");
        check(replaceExecutable(target, real, error), "replaces a binary in place", error);
        std::string got;
        readFileBytes(target, got);
        check(got == "x", "the new contents are in place");
        check(removeDirectoryTree(tmp), "removes a folder tree (without following the link)");
        check(fileExists(real) == false, "and everything in it is gone");
    }
}

#endif

void testChecksums() {
    section("SHA256SUMS.txt");
    check(sha256Hex("abc", 3) ==
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
          "SHA-256 matches the FIPS 180-2 test vector");
    check(sha256Hex("", 0) ==
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
          "SHA-256 of nothing");

    const std::string a(64, 'a'), b(64, 'B');
    std::string hex;
    const std::string sums = "\xEF\xBB\xBF" + a + "  soi-share.exe\r\n" + b + " *install.ps1\r\n";
    check(findChecksum(sums, "soi-share.exe", hex) && hex == a, "finds a text-mode entry (BOM, CRLF)");
    check(findChecksum(sums, "install.ps1", hex) && hex == std::string(64, 'b'),
          "finds a binary-mode (*) entry, lowercased");
    check(findChecksum(sums, "SOI-SHARE.EXE", hex), "file names compare case-insensitively");
    check(!findChecksum(sums, "viewer.html", hex), "a missing file is not found");
    check(!findChecksum(sums, "soi-share", hex), "a prefix of a name is not a match");
    check(!findChecksum(a + "  soi-share.exe\n" + b + "  soi-share.exe\n", "soi-share.exe", hex),
          "two DIFFERENT sums for one file are refused");
    check(findChecksum(a + "  soi-share.exe\n" + a + "  soi-share.exe\n", "soi-share.exe", hex),
          "a repeated identical line is fine");
    check(!findChecksum(std::string(63, 'a') + "  soi-share.exe\n", "soi-share.exe", hex),
          "a short hash is ignored");
    check(!findChecksum(std::string(63, 'a') + "g  soi-share.exe\n", "soi-share.exe", hex),
          "non-hex is ignored");
}

void testVersions() {
    section("versions");
    Version v;
    check(parseVersion("v1.2.3", v) && v.major == 1 && v.minor == 2 && v.patch == 3, "v1.2.3");
    check(parseVersion("10.20.30-rc.1", v) && v.major == 10 && v.patch == 30, "suffix ignored");
    check(!parseVersion("1.2", v) && !parseVersion("", v) && !parseVersion("v1.2.x", v) &&
              !parseVersion("1.2.3.4", v),
          "malformed versions are rejected");
    Version a, b;
    parseVersion("1.9.9", a);
    parseVersion("1.10.0", b);
    check(compareVersions(a, b) < 0 && compareVersions(b, a) > 0 && compareVersions(a, a) == 0,
          "compares numerically (1.9.9 < 1.10.0)");
}

#if defined(_WIN32)
void testTokens() {
    section("saved GitHub token (DPAPI)");
    const std::string token = "github_pat_11ABCDEFG0123456789_abcdefghijklmnopqrstuvwxyz";
    std::string blob, back;
    check(protectSecret(token, blob) && !blob.empty(), "encrypts");
    check(blob.find("github_pat_") == std::string::npos, "the blob does not contain the token");
    check(unprotectSecret(blob, back) && back == token, "decrypts to the same token");

    std::string tampered = blob;
    tampered[tampered.size() / 2] = static_cast<char>(tampered[tampered.size() / 2] ^ 0x01);
    check(!unprotectSecret(tampered, back), "a single flipped bit is detected");
    check(!unprotectSecret(blob.substr(0, blob.size() - 4), back), "a truncated blob is refused");

    // A DPAPI blob with some other program's entropy must not pass for ours.
    DATA_BLOB in{static_cast<DWORD>(token.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(token.data()))};
    DATA_BLOB out{};
    if (CryptProtectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        const std::string foreign(reinterpret_cast<char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        check(!unprotectSecret(foreign, back), "a blob made without our entropy is refused");
    }

    check(classifyToken(token) == TokenKind::FineGrained, "github_pat_ is fine-grained");
    check(classifyToken("ghp_abc") == TokenKind::Classic, "ghp_ is classic");
    check(classifyToken("gho_abc") == TokenKind::OAuth, "gho_ (GitHub CLI) is OAuth");
    check(classifyToken(std::string(40, 'f')) == TokenKind::Classic, "40 hex chars is a legacy classic token");
}

#else
void testTokens() {
    section("saved GitHub token (login Keychain)");
    const std::string token = "github_pat_11ABCDEFG0123456789_abcdefghijklmnopqrstuvwxyz";
    const std::string account = "selftest-" + std::to_string(currentProcessId());
    if (!keychainStore(account, token)) {
        // A CI runner or an ssh session may have no unlocked login keychain;
        // that is the environment, not the code.
        std::printf("  \x1b[33mSKIP\x1b[0m  no usable login keychain in this session\n");
    } else {
        std::string back;
        check(keychainLoad(account, back) && back == token, "stores and reads back the token");
        check(keychainStore(account, token + "2") && keychainLoad(account, back) &&
                  back == token + "2",
              "storing again replaces it");
        check(keychainDelete(account), "deletes it");
        check(!keychainLoad(account, back), "and it is gone");
    }
    check(classifyToken(token) == TokenKind::FineGrained, "github_pat_ is fine-grained");
    check(classifyToken("ghp_abc") == TokenKind::Classic, "ghp_ is classic");
    check(classifyToken("gho_abc") == TokenKind::OAuth, "gho_ (GitHub CLI) is OAuth");
    check(classifyToken(std::string(40, 'f')) == TokenKind::Classic, "40 hex chars is a legacy classic token");
}

#endif

void testJson() {
    section("JSON reader (release metadata)");
    const std::string release = R"({
      "tag_name": "v1.2.0", "html_url": "https://github.com/o/r/releases/tag/v1.2.0",
      "assets": [
        {"url": "https://api.github.com/repos/o/r/releases/assets/11", "id": 11,
         "name": "soi-share.exe", "size": 7340032,
         "uploader": {"login": "x", "url": "https://api.github.com/users/x", "name": "SHA256SUMS.txt"}},
        {"url": "https://api.github.com/repos/o/r/releases/assets/12", "id": 12,
         "name": "SHA256SUMS.txt", "size": 300, "label": null, "draft": false}
      ]})";
    Release rel;
    std::string err;
    check(parseRelease(release, rel, err) && rel.tag == "v1.2.0" && rel.assets.size() == 2, "parses a release", err);
    const ReleaseAsset* exe = rel.asset("soi-share.exe");
    const ReleaseAsset* sums = rel.asset("SHA256SUMS.txt");
    check(exe && exe->apiUrl.find("/assets/11") != std::string::npos && exe->size == 7340032,
          "asset url and size come from the asset, not a nested object");
    check(sums && sums->apiUrl.find("/assets/12") != std::string::npos,
          "a nested field with the same value does not confuse the lookup");

    JsonValue v;
    check(JsonValue::parse(R"({"s":"a\"b\\c\u00e9\ud83d\ude00\n"})", v) &&
              v["s"].str() == "a\"b\\c\xC3\xA9\xF0\x9F\x98\x80\n",
          "escapes, \\u and surrogate pairs decode to UTF-8");
    check(JsonValue::parse("[1, -2.5e3, true, null]", v) && v.items().size() == 4 &&
              v.items()[1].num() == -2500.0,
          "arrays and numbers");
    const char* bad[] = {"", "{", "{\"a\":}", "[1,]", "{\"a\":1}x", "\"\\ud800\"", "tru",
                         "{\"a\" 1}", "\"a\nb\""};
    bool allBad = true;
    for (const char* b : bad) allBad = allBad && !JsonValue::parse(b, v);
    check(allBad, "malformed documents are rejected");
    std::string deep(200, '[');
    check(!JsonValue::parse(deep, v), "absurd nesting is rejected, not recursed into");
}

void testControlFormat() {
    section("control channel wire format");
    const std::map<std::string, std::string> kv{
        {"code", "ABC-DEF"}, {"offer", "SOI1:abc=def=="}, {"stats", "line\nbreak"}, {"empty", ""}};
    const auto back = parseKeyValues(serializeKeyValues(kv));
    check(back.at("code") == "ABC-DEF" && back.at("offer") == "SOI1:abc=def==",
          "values containing '=' survive a round trip");
    check(back.at("stats") == "line break", "a newline in a value cannot inject a key");
    check(back.count("empty") && back.at("empty").empty(), "empty values are kept");
    check(parseKeyValues("a=1\r\nb=2\r\n=bad\nnoequals\n").size() == 2, "CRLF, and junk lines are skipped");

    InstanceRecord rec;
    check(parseInstanceRecord("pid=1234\nport=50123\nsecret=abcd\nexe=C:\\x.exe\nstarted=1700000000\n", rec) &&
              rec.pid == 1234 && rec.port == 50123 && rec.secret == "abcd" && rec.startedUnix == 1700000000,
          "parses an instance record");
    check(!parseInstanceRecord("pid=1234\nport=0\nsecret=abcd\n", rec), "a record with no port is invalid");
    check(!parseInstanceRecord("pid=1234\nport=5000\n", rec), "a record with no secret is invalid");
    check(!processAlive(0) && processAlive(currentProcessId()), "process liveness");
    check(!processAlive(currentProcessId(), "C:\\not\\this.exe"),
          "a live pid with a different image is not ours (pid reuse)");
}

void writeRaw(const std::string& text) {
    std::fflush(stdout);
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

int runBlobEncode(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: blob-encode <text> [pass]\n"); return 2; }
    const std::string text = argv[2];
    const std::string pass = argc > 3 ? argv[3] : "";
    const std::string blob = encodeSignalBlob(text, pass);
    if (blob.empty()) return 1;
    writeRaw(blob);
    return 0;
}

#if defined(_WIN32)
// Isolates where GDI capture time actually goes, below the BitBltCapture class:
// raw blit vs CAPTUREBLT vs each StretchBlt quality mode vs a two-step
// blit-then-scale. Run with: soi-selftest capture-bench
int runCaptureBench() {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    const int sw = GetSystemMetrics(SM_CXSCREEN);
    const int sh = GetSystemMetrics(SM_CYSCREEN);
    const int dw = 1280, dh = (sh * 1280) / sw & ~1;
    std::printf("\nscreen %dx%d, scaled target %dx%d\n\n", sw, sh, dw, dh);

    HDC screen = GetDC(nullptr);

    auto makeDib = [&](int w, int h, HDC& dc, HBITMAP& bmp) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        dc  = CreateCompatibleDC(screen);
        bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        SelectObject(dc, bmp);
    };

    HDC fullDc, scaleDc;
    HBITMAP fullBmp, scaleBmp;
    makeDib(sw, sh, fullDc, fullBmp);
    makeDib(dw, dh, scaleDc, scaleBmp);

    auto bench = [](const char* label, int iters, auto&& fn) {
        fn();   // warm up
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < iters; ++i) fn();
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count() / iters;
        std::printf("  %-52s %7.2f ms  (%4.0f fps)\n", label, ms, 1000.0 / ms);
        return ms;
    };

    bench("BitBlt screen->mem, 1:1, SRCCOPY", 20, [&] {
        BitBlt(fullDc, 0, 0, sw, sh, screen, 0, 0, SRCCOPY);
        GdiFlush();
    });
    bench("BitBlt screen->mem, 1:1, SRCCOPY|CAPTUREBLT", 20, [&] {
        BitBlt(fullDc, 0, 0, sw, sh, screen, 0, 0, SRCCOPY | CAPTUREBLT);
        GdiFlush();
    });
    bench("BitBlt screen->mem, 1:1, no GdiFlush", 20, [&] {
        BitBlt(fullDc, 0, 0, sw, sh, screen, 0, 0, SRCCOPY);
    });

    SetStretchBltMode(scaleDc, HALFTONE);
    SetBrushOrgEx(scaleDc, 0, 0, nullptr);
    bench("StretchBlt screen->mem, scaled, HALFTONE", 20, [&] {
        StretchBlt(scaleDc, 0, 0, dw, dh, screen, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
    });

    SetStretchBltMode(scaleDc, COLORONCOLOR);
    bench("StretchBlt screen->mem, scaled, COLORONCOLOR", 20, [&] {
        StretchBlt(scaleDc, 0, 0, dw, dh, screen, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
    });

    SetStretchBltMode(scaleDc, HALFTONE);
    bench("BitBlt 1:1 then StretchBlt mem->mem HALFTONE", 20, [&] {
        BitBlt(fullDc, 0, 0, sw, sh, screen, 0, 0, SRCCOPY);
        StretchBlt(scaleDc, 0, 0, dw, dh, fullDc, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
    });

    SetStretchBltMode(scaleDc, COLORONCOLOR);
    bench("BitBlt 1:1 then StretchBlt mem->mem COLORONCOLOR", 20, [&] {
        BitBlt(fullDc, 0, 0, sw, sh, screen, 0, 0, SRCCOPY);
        StretchBlt(scaleDc, 0, 0, dw, dh, fullDc, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
    });

    bench("mem->mem StretchBlt alone, COLORONCOLOR", 50, [&] {
        StretchBlt(scaleDc, 0, 0, dw, dh, fullDc, 0, 0, sw, sh, SRCCOPY);
        GdiFlush();
    });

    std::printf("\n");
    DeleteObject(scaleBmp); DeleteDC(scaleDc);
    DeleteObject(fullBmp);  DeleteDC(fullDc);
    ReleaseDC(nullptr, screen);
    return 0;
}
#endif

int runBlobDecode(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: blob-decode <blob> [pass]\n"); return 2; }
    const std::string blob = argv[2];
    const std::string pass = argc > 3 ? argv[3] : "";
    std::string out, err;
    if (!decodeSignalBlob(blob, pass, out, err)) {
        std::fprintf(stderr, "decode failed: %s\n", err.c_str());
        return 1;
    }
    writeRaw(out);
    return 0;
}

#if !defined(_WIN32)
// ---------------------------------------------------------------------------
// macOS: capture and the zero-copy encode path
// ---------------------------------------------------------------------------
void testMacDisplays() {
    section("displays and windows");
    const auto mons = enumerateMonitors();
    check(!mons.empty(), "at least one display", soi::format("{}", mons.size()));
    for (const auto& m : mons)
        info(soi::format("{}: {} {}x{} at {},{}{}", m.index, m.name, m.width, m.height, m.x, m.y,
                         m.primary ? " (main)" : ""));
    if (!mons.empty())
        check(mons[0].primary, "display 0 is the main display");
    info(soi::format("{} capturable window(s)", enumerateWindows().size()));
    info(soi::format("Screen Recording permission: {}",
                     screenCapturePermitted(false) ? "granted" : "NOT granted"));
}

void testMacCapture() {
    section("capture backends (ScreenCaptureKit, CGDisplayStream, CGImage)");
    if (!screenCapturePermitted(false)) {
        std::printf("  \x1b[33mSKIP\x1b[0m  no Screen Recording permission for this terminal\n");
        return;
    }
    CaptureConfig cfg;
    cfg.target = CaptureTarget::Monitor;
    int working = 0;
    for (const auto& r : probeBackends(cfg)) {
        info(soi::format("{:<8} {}", backendName(r.backend), r.detail));
        if (r.started && r.gotFrame) ++working;
    }
    check(working > 0, "at least one backend reads the main display");

    cfg.target = CaptureTarget::VirtualDesktop;
    auto desk = createFrameSource(cfg);
    const bool deskOk = desk->start() && desk->capture() != nullptr;
    check(deskOk, "every display at once (--desktop) produces a frame", desk->describe());
    desk->stop();

    // The automatic source, as `start` uses it, with the GPU path on.
    cfg.target    = CaptureTarget::Monitor;
    cfg.preferGpu = true;
    auto src = createFrameSource(cfg);
    if (src->start()) {
        const Frame* f = nullptr;
        for (int i = 0; i < 20 && !f; ++i) f = src->capture();
        check(f != nullptr, "the automatic source delivers frames", src->describe());
        info(soi::format("backend {}, frames {} the GPU", backendName(src->backend()),
                         src->gpuDevice() ? "stay on" : "are copied off"));
        src->stop();
    } else {
        check(false, "the automatic source starts");
    }
}

void testMacGpuEncode() {
    section("zero-copy encode (IOSurface -> GPU NV12 -> VideoToolbox)");
    auto device = sharedGpuDevice();
    H264Encoder encoder;
    check(encoder.enableGpuInput(device), "the encoder accepts GPU frames", device->describe());

    EncoderConfig cfg;
    cfg.width = 1280; cfg.height = 720; cfg.fps = 30; cfg.bitrateKbps = 3000; cfg.gopSeconds = 2;
    std::mutex mtx;
    std::vector<std::vector<uint8_t>> units;
    const bool started = encoder.start(cfg, [&](const uint8_t* nal, size_t len, bool, int64_t) {
        std::lock_guard lk(mtx);
        units.emplace_back(nal, nal + len);
    });
    check(started && encoder.usesGpuInput(), "starts with GPU input");
    if (!started) return;

    // 1920x1080 BGRA in, 1280x720 NV12 out: the scale happens on the GPU too.
    int fed = 0;
    for (int n = 0; n < 30; ++n) {
        void* frame = macMakeTestPixelBuffer(1920, 1080, n);
        if (!frame) break;
        if (encoder.submitTexture(frame, static_cast<int64_t>(n) * 33'333'333LL)) ++fed;
        macReleasePixelBuffer(frame);
        std::this_thread::sleep_for(33ms);
    }
    std::this_thread::sleep_for(500ms);
    encoder.stop();

    std::vector<std::vector<uint8_t>> got;
    {
        std::lock_guard lk(mtx);
        got = units;
    }
    check(fed >= 25 && got.size() >= 25, "IOSurface frames are encoded",
          soi::format("{} fed, {} encoded", fed, got.size()));
    int decoded = 0, w = 0, h = 0;
    std::string detail;
    check(macDecodeAnnexB(got, decoded, w, h, detail) && decoded > 0 && w == 1280 && h == 720,
          "and decode back at the encode size", soi::format("{} decoded, {}x{} {}", decoded, w, h, detail));
}
#endif

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD  mode = 0;
    if (GetConsoleMode(hOut, &mode))
        SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
#endif

    int rc = 0;
    if (argc > 1 && std::strcmp(argv[1], "blob-encode") == 0) {
        rc = runBlobEncode(argc, argv);
    } else if (argc > 1 && std::strcmp(argv[1], "blob-decode") == 0) {
        rc = runBlobDecode(argc, argv);
#if defined(_WIN32)
    } else if (argc > 1 && std::strcmp(argv[1], "capture-bench") == 0) {
        rc = runCaptureBench();
#endif
    } else if (argc > 1 && std::strcmp(argv[1], "unit") == 0) {
#if defined(_WIN32)
        std::printf("\x1b[1msoi-selftest unit\x1b[0m (no screen, GPU or media stack needed)\n");
#else
        // VideoToolbox is part of every macOS install, so the encoder -- the
        // piece most likely to differ between Macs -- is part of the unit run.
        std::printf("\x1b[1msoi-selftest unit\x1b[0m (no screen needed)\n");
#endif
        g_timingIsAdvisory = true;
        testBase64();
        testSignalBlob();
        testColorConvert();
        testThreadPool();
        testLevels();
        testQuality();
        testPathList();
        testInstallDir();
        testChecksums();
        testVersions();
        testTokens();
        testJson();
        testControlFormat();
#if !defined(_WIN32)
        testEncoder();
        testMacGpuEncode();
#endif

        std::printf("\n\x1b[1m== summary ==\x1b[0m\n");
        std::printf("  \x1b[32m%d passed\x1b[0m", g_pass);
        if (g_fail) std::printf(", \x1b[31m%d FAILED\x1b[0m", g_fail);
        std::printf("\n\n");
        rc = g_fail ? 1 : 0;
    } else {
#if defined(_WIN32)
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
#endif

        std::printf("\x1b[1msoi-selftest\x1b[0m\n");

        testBase64();
        testSignalBlob();
        testColorConvert();
        testThreadPool();
        testLevels();
        testQuality();
#if defined(_WIN32)
        testCapture();
        testCaptureBackends();
        testOverlayCapture();
        testProtection();
        testProtectionEffective();
        testProtectionAllModes();
        testEncoder();
        testGpuConvert();
        testGpuPipeline();
#else
        testMacDisplays();
        testMacCapture();
        testEncoder();
        testMacGpuEncode();
#endif
        testPathList();
        testInstallDir();
        testChecksums();
        testVersions();
        testTokens();
        testJson();
        testControlFormat();

        std::printf("\n\x1b[1m== summary ==\x1b[0m\n");
        std::printf("  \x1b[32m%d passed\x1b[0m", g_pass);
        if (g_fail) std::printf(", \x1b[31m%d FAILED\x1b[0m", g_fail);
        std::printf("\n\n");
        rc = g_fail ? 1 : 0;
    }

#if defined(_WIN32)
    MFShutdown();
    CoUninitialize();
#endif
    return rc;
}
