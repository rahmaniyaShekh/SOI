//
// soi-share -- serverless peer-to-peer screen sharing. Terminal only, no GUI.
//
// The streaming process runs DETACHED: `start` spawns it with no console and
// returns immediately, so closing the launching terminal does not kill it, and
// `status` / `answer` / `stop` work from any other terminal.
//
// Session flow (no signalling server anywhere in it):
//   soi-share start --monitor 0     -> prints the offer blob, opens the viewer
//   (viewer produces an answer blob)
//   soi-share answer SOI1:...       -> hands it to the running daemon
//   soi-share stop                  -> shuts it down
//
#include "app/LocalHandover.h"
#include "app/Rendezvous.h"
#include "app/Service.h"
#include "capture/CaptureFactory.h"
#include "capture/CaptureProtect.h"
#include "encode/ColorConvert.h"
#include "encode/H264Encoder.h"
#include "encode/Quality.h"
#include "net/SignalBlob.h"
#include "net/Streamer.h"
#include "util/Log.h"
#include "util/Parallel.h"
#include "util/Win.h"

#include <windows.h>
#include <mfapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace soi;
using namespace std::chrono_literals;

namespace {

// Present only in Windows 10 1803+ SDKs.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

std::atomic<bool> g_running{true};

BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}

// ---------------------------------------------------------------------------
// Options
// ---------------------------------------------------------------------------

struct Options {
    CaptureTarget target       = CaptureTarget::Monitor;
    int           monitorIndex = 0;
    std::string   windowSpec;

    // Auto means "whatever can actually read this screen": Desktop Duplication
    // first, then Windows.Graphics.Capture, then GDI, with a live switch if the
    // running one stops delivering. Pinning a backend is a diagnostic, not a
    // normal way to run -- see capture/CaptureFactory.h.
    CaptureBackend backend = CaptureBackend::Auto;

    // Quality is chosen by NAME and can be changed by the viewer mid-session.
    // The level decides the encode height, the nominal frame rate and the
    // per-frame bit budget -- see encode/Quality.h for why frame rate is what
    // gives way on a slow link, not sharpness.
    std::string quality;          // empty => defaultQualityLevel()
    int  minFps         = 2;      // floor before per-frame quality has to suffer

    int  fps            = 30;     // hard cap on top of the level's nominal rate
    int  bitrateKbps    = 0;      // 0 => the level decides
    int  minBitrateKbps = 600;
    int  maxWidth       = 1920;
    int  gopSeconds     = 10;
    double idleRefreshSec = 2.0;

    bool cursor     = false;
    bool layered    = false;
    bool protect    = true;
    // Never open a browser on the sharing machine: this is a headless tool, and
    // a browser window here would be one more thing on the screen being shared.
    bool openViewer = false;
    bool verbose    = false;

    // Local handover: serve the viewer straight off this machine so the peer
    // only has to open a URL. See app/LocalHandover.h for the scope caveat.
    bool http     = true;
    int  httpPort = 8000;

    // Short-code rendezvous: the peer types 6 characters into a web page.
    // Default, because it is the only route that works from any network with
    // one thing to share. See app/Rendezvous.h for what the service can and
    // cannot see.
    bool        useCode    = true;
    std::string serviceUrl = "https://share.mdarif.online";

    // Survive network blips on either side: the share code stays valid and the
    // session re-negotiates itself when connectivity returns.
    bool reconnect = true;

    // Rotate the machine's saved code. Also the way to lock out anyone who
    // already has the old one.
    bool newCode = false;

    std::string passphrase;

    // Two independent free, no-signup STUN servers, queried in parallel: a second
    // one costs no extra gathering time and removes a single point of failure.
    std::vector<std::string> stunUrls = {
        "stun:stun.cloudflare.com:3478",
        "stun:stun.l.google.com:19302",
    };
    bool stunOverridden = false;

    std::string turnUrl, turnUser, turnPass;
};

void printUsage() {
    std::puts(R"(soi-share -- serverless P2P screen sharing (BitBlt capture + WebRTC)

USAGE
  soi-share start [options]    start sharing (detached; returns immediately)
  soi-share answer <blob>      hand the viewer's answer to the running daemon
  soi-share status             show what the daemon is doing
  soi-share offer              reprint the current offer blob
  soi-share stop               stop the daemon (works from any terminal)
  soi-share run [options]      run in the foreground instead (for debugging)
  soi-share list-monitors      enumerate monitors
  soi-share list-windows       enumerate capturable windows
  soi-share capture-check      try every capture backend and report what works
  soi-share purge              erase all on-disk state (log, share code, blobs)

CAPTURE TARGET
  --monitor <N>          share monitor N (default: 0)
  --desktop              share the whole virtual desktop
  --window <hwnd|text>   share one window, by handle or title substring

  --capture <backend>    auto | dxgi | wgc | bitblt   (default: auto)
                         Auto is what makes the share show everything on the
                         screen: Desktop Duplication reads the image the display
                         is actually being sent, so exclusive-fullscreen games
                         and hardware-overlay video are in it; Windows.Graphics.
                         Capture covers windows and the machines duplication
                         refuses; GDI is the floor. If the running backend stops
                         producing frames the next one takes over mid-session.

                         Naming one pins it and disables that fallback, which is
                         for diagnosing a problem, not for sharing. Run
                         `soi-share capture-check` first.

QUALITY
  --quality <level>      360p | 480p | 720p | 1080p | source   (default 720p)
                         The VIEWER can change this at any time from their
                         browser; this only sets where the session starts.

                         On a slow link the resolution is HELD and the frame
                         rate drops instead, so text stays sharp and readable
                         rather than being smeared to fit the bitrate. That is
                         the right trade for a screen; it is the wrong one for
                         camera video, which is why other tools do the opposite.

  --min-fps <N>          how far the frame rate may fall before per-frame
                         quality has to give way too (default 2)
  --fps <N>              hard cap on the frame rate, 1-30 (default 30)
  --bitrate <kbps>       override the level's bitrate ceiling
  --min-bitrate <kbps>   floor for congestion control (default 600)
  --max-width <px>       cap the capture size before scaling (default 1920)
  --gop <seconds>        keyframe interval (default 10)
  --idle-refresh <sec>   force a frame when the screen is static (default 2)
  --cursor               composite the mouse cursor into the stream
  --layered              include layered/transparent windows (CAPTUREBLT)

PRIVACY
  --protect / --no-protect   exclude our own windows from capture (default on)
  --pass <passphrase>        encrypt signalling blobs (AES-256-GCM)

NETWORK
  --stun <url>           STUN server; repeat to use several. Defaults to
                         stun.cloudflare.com and stun.l.google.com (free, no
                         signup). The first --stun replaces the defaults.
  --no-stun              host candidates only -- same-LAN, zero external contact
  --turn <url>           TURN relay (turn:host:3478 / turns:host:5349). Fallback
                         only. The relay forwards DTLS-SRTP ciphertext and
                         cannot see your screen.
  --turn-user <name>     TURN username
  --turn-pass <secret>   TURN credential

LOW BANDWIDTH
  --low                  preset for ~1 Mbps links: 480p, held sharp, with the
                         frame rate free to fall as far as --min-fps.

MISC
  --new-code             rotate this machine's share code (revokes the old one)
  --no-reconnect         exit when the connection drops instead of re-offering
                         the same code. Reconnect is on by default and is the
                         recommended setting: the viewer rejoins by itself when
                         the network comes back, with nobody touching either PC.
  --no-code              do not use the short-code service; print a long blob
  --service <url>        rendezvous base URL (default https://share.mdarif.online)
  --port <N>             port for the local handover page (default 8000)
  --no-http              disable the handover page; use the send-a-file flow
  --open-viewer          also open the viewer on THIS machine (off by default)
  --verbose              trace logging
  --help)");
}

bool parseInt(const char* text, int& out) {
    if (!text) return false;
    char* end = nullptr;
    const long v = std::strtol(text, &end, 10);
    if (end == text || *end != '\0') return false;
    out = static_cast<int>(v);
    return true;
}

// Parses from `first`, so the subcommand token is skipped.
bool parseOptions(int argc, char** argv, int first, Options& o) {
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { logE("{} requires a value", name); return nullptr; }
            return argv[++i];
        };

        if (a == "--daemon")                 { continue; }   // internal marker
        else if (a == "--desktop")           { o.target = CaptureTarget::VirtualDesktop; }
        else if (a == "--cursor")            { o.cursor = true; }
        else if (a == "--layered")           { o.layered = true; }
        else if (a == "--protect")           { o.protect = true; }
        else if (a == "--no-protect")        { o.protect = false; }
        else if (a == "--no-viewer")         { o.openViewer = false; }
        else if (a == "--open-viewer")       { o.openViewer = true; }
        else if (a == "--no-http")           { o.http = false; }
        else if (a == "--no-code")           { o.useCode = false; }
        else if (a == "--no-reconnect")      { o.reconnect = false; }
        else if (a == "--new-code")          { o.newCode = true; }
        else if (a == "--service") {
            const char* v = next("--service");
            if (!v) return false;
            o.serviceUrl = v;
        }
        else if (a == "--quality") {
            const char* v = next("--quality");
            if (!v) return false;
            if (!findQualityLevel(v)) {
                std::string names;
                for (const auto& l : qualityLevels()) { if (!names.empty()) names += ", "; names += l.name; }
                logE("unknown quality '{}'; choose one of: {}", v, names);
                return false;
            }
            o.quality = v;
        }
        else if (a == "--min-fps") {
            const char* v = next("--min-fps");
            if (!v || !parseInt(v, o.minFps)) return false;
        }
        else if (a == "--low") {
            // For a ~1 Mbps link. Note what this does NOT do: it does not drop
            // the resolution to 360p. 480p is kept and the frame rate is allowed
            // to fall instead, because readable text at a few frames a second
            // beats smooth motion you cannot read.
            o.quality        = "480p";
            o.minBitrateKbps = 250;
        }
        else if (a == "--port") {
            const char* v = next("--port");
            if (!v || !parseInt(v, o.httpPort)) return false;
        }
        else if (a == "--verbose")           { o.verbose = true; }
        else if (a == "--no-stun")           { o.stunUrls.clear(); o.stunOverridden = true; }
        else if (a == "--monitor") {
            const char* v = next("--monitor");
            if (!v || !parseInt(v, o.monitorIndex)) return false;
            o.target = CaptureTarget::Monitor;
        }
        else if (a == "--window") {
            const char* v = next("--window");
            if (!v) return false;
            o.windowSpec = v;
            o.target = CaptureTarget::Window;
        }
        else if (a == "--capture") {
            const char* v = next("--capture");
            if (!v) return false;
            if (!parseBackendName(v, o.backend)) {
                logE("unknown capture backend '{}'; choose one of: auto, dxgi, "
                     "wgc, bitblt", v);
                return false;
            }
        }
        else if (a == "--fps")          { const char* v = next("--fps");          if (!v || !parseInt(v, o.fps)) return false; }
        else if (a == "--bitrate")      { const char* v = next("--bitrate");      if (!v || !parseInt(v, o.bitrateKbps)) return false; }
        else if (a == "--min-bitrate")  { const char* v = next("--min-bitrate");  if (!v || !parseInt(v, o.minBitrateKbps)) return false; }
        else if (a == "--max-width")    { const char* v = next("--max-width");    if (!v || !parseInt(v, o.maxWidth)) return false; }
        else if (a == "--gop")          { const char* v = next("--gop");          if (!v || !parseInt(v, o.gopSeconds)) return false; }
        else if (a == "--idle-refresh") {
            const char* v = next("--idle-refresh");
            if (!v) return false;
            o.idleRefreshSec = std::strtod(v, nullptr);
        }
        else if (a == "--pass") { const char* v = next("--pass"); if (!v) return false; o.passphrase = v; }
        else if (a == "--stun") {
            const char* v = next("--stun");
            if (!v) return false;
            if (!o.stunOverridden) { o.stunUrls.clear(); o.stunOverridden = true; }
            o.stunUrls.emplace_back(v);
        }
        else if (a == "--turn")      { const char* v = next("--turn");      if (!v) return false; o.turnUrl = v; }
        else if (a == "--turn-user") { const char* v = next("--turn-user"); if (!v) return false; o.turnUser = v; }
        else if (a == "--turn-pass") { const char* v = next("--turn-pass"); if (!v) return false; o.turnPass = v; }
        else { logE("unknown option: {}", a); return false; }
    }

    // Capped at 30 by design. GDI screen capture is pinned to the compositor's
    // refresh rate (measured ~16.9 ms/frame), so 30fps leaves real headroom
    // while 60 would sit right on the limit and jitter.
    o.fps            = std::clamp(o.fps, 1, 30);
    // 0 means "let the quality level decide", so only clamp a real override.
    if (o.bitrateKbps > 0) o.bitrateKbps = std::clamp(o.bitrateKbps, 200, 50000);
    o.minBitrateKbps = std::clamp(o.minBitrateKbps, 100,
                                  o.bitrateKbps > 0 ? o.bitrateKbps : 50000);
    o.minFps         = std::clamp(o.minFps, 1, o.fps);
    o.gopSeconds     = std::clamp(o.gopSeconds, 1, 600);
    return true;
}

// Re-serialises the user's options so the detached child gets exactly them.
std::vector<std::string> rebuildArgs(int argc, char** argv, int first) {
    std::vector<std::string> args{"--daemon"};
    for (int i = first; i < argc; ++i) args.emplace_back(argv[i]);
    return args;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void listMonitors() {
    const auto mons = enumerateMonitors();
    std::printf("\n%-6s %-16s %-14s %-10s %s\n", "INDEX", "DEVICE", "SIZE", "ORIGIN", "PRIMARY");
    for (const auto& m : mons)
        std::printf("%-6d %-16s %-14s %-10s %s\n", m.index, m.name.c_str(),
                    soi::format("{}x{}", m.width, m.height).c_str(),
                    soi::format("{},{}", m.x, m.y).c_str(), m.primary ? "yes" : "");
    std::printf("\n%zu monitor(s)\n", mons.size());
}

void listWindows() {
    const auto wins = enumerateWindows();
    std::printf("\n%-18s %-12s %-24s %s\n", "HWND", "SIZE", "PROCESS", "TITLE");
    for (const auto& w : wins) {
        std::string title = w.title;
        if (title.size() > 60) title = title.substr(0, 57) + "...";
        std::printf("0x%-16llX %-12s %-24s %s\n",
                    reinterpret_cast<unsigned long long>(w.handle),
                    soi::format("{}x{}", w.width, w.height).c_str(),
                    w.process.c_str(), title.c_str());
    }
    std::printf("\n%zu window(s)\n", wins.size());
}

HWND resolveWindowSpec(const std::string& spec) {
    if (spec.rfind("0x", 0) == 0 || spec.rfind("0X", 0) == 0) {
        const auto value = std::strtoull(spec.c_str() + 2, nullptr, 16);
        if (value) return reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
    }
    if (!spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos) {
        const auto value = std::strtoull(spec.c_str(), nullptr, 10);
        if (value) return reinterpret_cast<HWND>(static_cast<uintptr_t>(value));
    }
    for (const auto& w : enumerateWindows())
        if (containsNoCase(w.title, spec)) {
            logI("matched window '{}' ({})", w.title, w.process);
            return static_cast<HWND>(w.handle);
        }
    logE("no visible window matches '{}'; try: soi-share list-windows", spec);
    return nullptr;
}

// Why is the share black? This answers it without a debugger: every backend is
// started against the requested target, timed, and its frames inspected for
// being entirely black -- which is what a protected window or a target nothing
// can read looks like from here.
void captureCheck(const Options& opt) {
    CaptureConfig cfg;
    cfg.target        = opt.target;
    cfg.monitorIndex  = opt.monitorIndex;
    cfg.captureCursor = opt.cursor;
    cfg.maxWidth      = opt.maxWidth;

    if (opt.target == CaptureTarget::Window) {
        cfg.windowHandle = resolveWindowSpec(opt.windowSpec);
        if (!cfg.windowHandle) return;
    }

    const char* what = opt.target == CaptureTarget::Window           ? "window"
                       : opt.target == CaptureTarget::VirtualDesktop ? "virtual desktop"
                                                                     : "monitor";
    std::printf("\nprobing every capture backend against the %s...\n\n", what);
    std::printf("%-9s %-8s %-12s %-9s %s\n",
                "BACKEND", "STARTS", "SIZE", "MS/GRAB", "RESULT");

    int working = 0;
    for (const auto& r : probeBackends(cfg)) {
        if (r.started && r.gotFrame && !r.allBlack) ++working;

        const std::string size = r.started ? soi::format("{}x{}", r.width, r.height) : "-";
        const std::string ms   = r.gotFrame ? soi::format("{:.2f}", r.msPerFrame) : "-";
        std::printf("%-9s %-8s %-12s %-9s %s\n",
                    backendName(r.backend), r.started ? "yes" : "no",
                    size.c_str(), ms.c_str(), r.detail.c_str());
    }

    std::printf(
        "\nMS/GRAB on a still screen is mostly the cost of ASKING whether anything\n"
        "changed. dxgi and wgc are told by the compositor; bitblt has to re-read\n"
        "and hash the whole screen to find out, which is the 16 ms.\n");
    std::printf("\n%d of 3 backends can read this target.\n", working);
    if (working) {
        std::printf("Sharing picks a working one automatically; no flag needed.\n\n");
    } else {
        std::printf(
            "\nNothing can read it. That normally means the target is protected with\n"
            "SetWindowDisplayAffinity, or is only visible on the secure desktop. No\n"
            "user-mode capture API on Windows can read either -- see README 1.2.\n\n");
    }
}

// Windows consoles do not interpret ANSI escapes unless asked. Ask once, and
// fall back to plain text when the handle is redirected to a file or a pipe --
// emitting raw escape bytes into a log would be worse than no emphasis.
bool enableAnsi() {
    static const bool ok = [] {
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == INVALID_HANDLE_VALUE || GetFileType(out) != FILE_TYPE_CHAR) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(out, &mode)) return false;
        return SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }();
    return ok;
}

bool copyToClipboard(const std::string& text) {
    const std::wstring wide = toUtf16(text);
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(mem)) {
                std::memcpy(dst, wide.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

std::string exeDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring_view sv(path, n);
    const size_t slash = sv.find_last_of(L"\\/");
    return toUtf8(slash == std::wstring_view::npos ? sv : sv.substr(0, slash));
}

bool readFileText(const std::string& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(toUtf16(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char  buf[16384];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return !out.empty();
}

bool fileExists(const std::string& path) {
    const DWORD attrs = GetFileAttributesW(toUtf16(path).c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// file:// URL with the offer in the fragment. Browsers never transmit a
// fragment, and this is a local file regardless.
std::string buildViewerUrl(const std::string& viewerPath, const std::string& blob) {
    std::string url = "file:///";
    for (char c : viewerPath) {
        if (c == '\\')      url += '/';
        else if (c == ' ')  url += "%20";
        else if (c == '#')  url += "%23";
        else if (c == '?')  url += "%3F";
        else if (c == '%')  url += "%25";
        else                url += c;
    }
    return url + "#" + blob;
}

// A high-resolution waitable timer keeps frame pacing tight. The default Windows
// timer granularity is ~15.6ms, which at 30fps would alias badly.
class FramePacer {
public:
    explicit FramePacer(int fps)
        : interval_(std::chrono::nanoseconds(1'000'000'000LL / std::max(1, fps))) {
        fps_ = std::max(1, fps);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        next_ = std::chrono::steady_clock::now();
    }
    ~FramePacer() { if (timer_) CloseHandle(timer_); }
    FramePacer(const FramePacer&) = delete;
    FramePacer& operator=(const FramePacer&) = delete;

    // Retuned live as bandwidth moves: this is the knob the quality strategy
    // actually turns. Takes effect on the next frame; the schedule is not reset,
    // so raising the rate does not produce a burst.
    void setFps(int fps) {
        fps = std::max(1, fps);
        if (fps == fps_) return;
        fps_ = fps;
        interval_ = std::chrono::nanoseconds(1'000'000'000LL / fps);
    }
    int fps() const { return fps_; }

    void waitNext() {
        next_ += interval_;
        const auto now = std::chrono::steady_clock::now();
        if (next_ < now) { next_ = now; return; }   // fell behind: do not spiral

        const auto delay = next_ - now;
        if (!timer_) { std::this_thread::sleep_for(delay); return; }

        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count() / 100);
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, INFINITE);
        else
            std::this_thread::sleep_for(delay);
    }

private:
    HANDLE                                timer_ = nullptr;
    std::chrono::nanoseconds              interval_;
    std::chrono::steady_clock::time_point next_;
    int                                   fps_ = 30;
};

// Must match kPeakPercent in H264Encoder.cpp: the burst headroom the encoder is
// configured with, and therefore the headroom that has to be reserved out of
// the link estimate when choosing a frame rate.
constexpr int kEncoderPeakPercent = 125;

// The ceiling congestion control is allowed to discover. Taken from the level
// table so adding a level cannot leave the estimator clamped below it.
int highestLevelBitrateKbps() {
    int best = 0;
    for (const auto& level : qualityLevels()) best = std::max(best, level.bitrateKbps);
    return std::max(best, 1000);
}

// ---------------------------------------------------------------------------
// Quality control
//
// Owns the encoder, because changing resolution means rebuilding it, and owns
// the frame-rate decision, because that is what absorbs a slow link.
//
// Two inputs, from two different threads:
//   * the viewer picking a level, over the control channel
//   * the congestion controller reporting what the link will carry
// Both are recorded as pending state and applied on the capture thread, so the
// encoder is only ever touched from one place.
// ---------------------------------------------------------------------------
class QualityDirector {
public:
    QualityDirector(Streamer& streamer, FrameSource& capture, const CaptureConfig& capCfg,
                    const Options& opt, int peerMaxMacroblocks)
        : streamer_(streamer), capture_(capture), capCfg_(capCfg), opt_(opt),
          srcW_(capture.width()), srcH_(capture.height()),
          peerMaxMacroblocks_(peerMaxMacroblocks) {
        const QualityLevel* wanted =
            opt.quality.empty() ? nullptr : findQualityLevel(opt.quality);
        level_ = wanted ? wanted : &defaultQualityLevel();
        available_.store(level_->bitrateKbps);
        recomputeSize();
        fps_ = cappedNominal();
    }

    // --- capture thread ----------------------------------------------------

    bool startEncoder() {
        EncoderConfig cfg;
        cfg.width       = encW_;
        cfg.height      = encH_;
        cfg.fps         = cappedNominal();
        cfg.bitrateKbps = ceilingKbps();
        cfg.gopSeconds  = opt_.gopSeconds;
        cfg.quality     = 80;

        if (!encoder_.start(cfg, [this](const uint8_t* nal, size_t len, bool key, int64_t pts) {
                streamer_.sendFrame(nal, len, key, pts);
            }))
            return false;

        logI("quality {}: {}x{} up to {} fps, {} at {} kbps",
             level_->name, encW_, encH_, cfg.fps,
             encoder_.usesQualityRateControl() ? "peak-constrained VBR" : "CBR",
             cfg.bitrateKbps);
        encoder_.requestKeyframe();
        announce();
        return true;
    }

    void stopEncoder() { encoder_.stop(); }

    // Applies anything the other threads asked for. Returns false if a rebuild
    // was needed and failed, which ends the session.
    bool tick() {
        std::string wanted;
        int wantedMonitor = -1;
        {
            std::lock_guard lk(mtx_);
            wanted.swap(pendingLevel_);
            wantedMonitor = std::exchange(pendingMonitor_, -1);
        }

        if (wantedMonitor >= 0 && !switchMonitor(wantedMonitor)) return false;
        if (!wanted.empty()) {
            if (const QualityLevel* next = findQualityLevel(wanted); next && next != level_) {
                logI("viewer asked for {}", next->name);
                level_ = next;
                recomputeSize();
                // Resolution is baked into the encoder's media type, so a change
                // means a rebuild. The viewer's decoder copes: the new IDR
                // carries new SPS/PPS and browsers resize on the fly.
                encoder_.stop();
                if (!startEncoder()) {
                    logE("could not restart the encoder at {}", level_->name);
                    return false;
                }
                // The estimate was earned at the old resolution; re-derive the
                // frame rate immediately rather than coasting on it.
                applyFrameRate(true);
                return true;
            }
            if (!findQualityLevel(wanted)) logT("ignoring unknown quality level '{}'", wanted);
        }
        applyFrameRate(false);
        return true;
    }

    bool submit(const Nv12Buffer& frame, int64_t ptsNs) { return encoder_.submit(frame, ptsNs); }
    void requestKeyframe() { encoder_.requestKeyframe(); }

    int encodeWidth()  const { return encW_; }
    int encodeHeight() const { return encH_; }
    int frameRate()    const { return fps_; }
    const QualityLevel& level() const { return *level_; }

    // --- other threads ------------------------------------------------------

    void requestLevel(const std::string& name) {
        std::lock_guard lk(mtx_);
        pendingLevel_ = name;
    }

    // The viewer picking a different monitor. Validated on the capture thread
    // against the live monitor list, so an index that no longer exists (someone
    // unplugged a screen) is ignored rather than acted on.
    void requestMonitor(int index) {
        std::lock_guard lk(mtx_);
        pendingMonitor_ = index;
    }

    // From REMB / receiver reports. Just recorded; the capture thread decides
    // what to do with it.
    void setAvailableBitrate(int kbps) {
        if (kbps > 0) available_.store(kbps);
    }

    // Tells the viewer what is on offer and what is running. Safe from the
    // control-channel thread.
    void announce() {
        std::string levels;
        for (const auto& l : qualityLevels()) {
            if (!levels.empty()) levels += ',';
            levels += '"' + l.name + '"';
        }

        // The monitor list is enumerated fresh every time rather than cached:
        // screens get plugged in and unplugged while a share is running, and a
        // viewer offering a monitor that no longer exists is worse than useless.
        std::string monitors;
        for (const auto& m : enumerateMonitors()) {
            if (!monitors.empty()) monitors += ',';
            monitors += soi::format(R"({{"i":{},"w":{},"h":{},"primary":{}}})",
                                    m.index, m.width, m.height, m.primary ? "true" : "false");
        }
        const int current = capCfg_.target == CaptureTarget::Monitor ? capCfg_.monitorIndex : -1;

        streamer_.sendControl(soi::format(
            R"({{"type":"quality","level":"{}","w":{},"h":{},"fps":{},"levels":[{}],)"
            R"("monitor":{},"monitors":[{}]}})",
            level_->name, encW_, encH_, fps_, levels, current, monitors));
    }

private:
    // Move the capture to another monitor mid-session. The peer connection and
    // the share code are untouched; only the pixels being read change. A new
    // monitor usually has a different size, so the encode size and the encoder
    // both have to be rebuilt -- same path a quality change takes.
    bool switchMonitor(int index) {
        const auto monitors = enumerateMonitors();
        const auto found = std::find_if(monitors.begin(), monitors.end(),
                                        [&](const MonitorInfo& m) { return m.index == index; });
        if (found == monitors.end()) {
            logW("viewer asked for monitor {}, which does not exist; ignoring", index);
            announce();          // correct the viewer's idea of what is available
            return true;
        }
        if (capCfg_.target == CaptureTarget::Monitor && capCfg_.monitorIndex == index)
            return true;         // already there

        logI("viewer asked for monitor {} ({}x{})", index, found->width, found->height);

        CaptureConfig next = capCfg_;
        next.target       = CaptureTarget::Monitor;
        next.monitorIndex = index;
        next.windowHandle = nullptr;

        if (!capture_.retarget(next)) {
            logE("could not switch to monitor {}", index);
            return false;
        }
        capCfg_ = next;
        srcW_ = capture_.width();
        srcH_ = capture_.height();

        recomputeSize();
        encoder_.stop();
        if (!startEncoder()) {
            logE("could not restart the encoder after switching monitor");
            return false;
        }
        applyFrameRate(true);
        return true;
    }

    int cappedNominal() const { return std::clamp(opt_.fps, 1, level_->fps); }

    // What the encoder is allowed to spend. In quality-targeted mode this is a
    // ceiling the encoder usually stays well under on a static screen.
    int ceilingKbps() const {
        const int base = opt_.bitrateKbps > 0 ? opt_.bitrateKbps : level_->bitrateKbps;
        return std::max(opt_.minBitrateKbps, base);
    }

    void recomputeSize() {
        encodeSizeFor(*level_, srcW_, srcH_, encW_, encH_);
        // Never exceed what the viewer's answer said its decoder accepts.
        // Sending a bigger frame than the negotiated level does not fail
        // cleanly -- it freezes, or a hardware decoder drops the stream -- so
        // this is the difference between a quality control that works and one
        // that appears to do nothing.
        if (peerMaxMacroblocks_ > 0) {
            const int wantW = encW_, wantH = encH_;
            clampToMacroblocks(peerMaxMacroblocks_, encW_, encH_);
            if (encW_ != wantW || encH_ != wantH)
                logW("{} ({}x{}) exceeds what the viewer's decoder accepts; "
                     "sending {}x{} instead", level_->name, wantW, wantH, encW_, encH_);
        }
    }

    // The heart of it: convert "what the link will carry" into a frame rate that
    // leaves every frame its full bit budget, rather than into a lower bitrate
    // that would leave every frame blurry.
    void applyFrameRate(bool force) {
        const int avail = available_.load();
        const int nominal = cappedNominal();
        // Reserve the encoder's burst headroom out of the link budget BEFORE
        // choosing a frame rate. The encoder is allowed to peak above its mean
        // for a complex frame, and that peak -- not the mean -- is what has to
        // fit down the wire. Sizing the frame rate off the raw estimate would
        // authorise bursts the link cannot carry, and the loss that follows
        // costs far more than the frames this gives up.
        const int budget = std::max(1, avail * 100 / kEncoderPeakPercent);
        int want = frameRateForBandwidth(*level_, budget, std::clamp(opt_.minFps, 1, nominal));
        want = std::clamp(want, 1, nominal);

        // Hysteresis: a frame rate that chases every receiver report visibly
        // stutters. Move only on a real change.
        if (!force && std::abs(want - fps_) * 4 < fps_) return;
        if (want == fps_ && !force) return;

        fps_ = want;
        // Hand the encoder the matching bitrate so bits-per-frame lands back on
        // the level's design point.
        const int kbps = std::min(ceilingKbps(), bitrateForFrameRate(*level_, want));
        encoder_.setBitrate(std::max(opt_.minBitrateKbps, kbps));
        logT("link {} kbps -> {} fps at {} (holding {} bits/frame)",
             avail, want, level_->name, bitsPerFrame(*level_));
        announce();
    }

    Streamer&      streamer_;
    FrameSource&   capture_;
    CaptureConfig  capCfg_;
    const Options& opt_;
    H264Encoder    encoder_;

    const QualityLevel* level_ = nullptr;
    int srcW_ = 0, srcH_ = 0;
    int encW_ = 0, encH_ = 0;
    int fps_  = 30;
    int peerMaxMacroblocks_ = 0;   // 0 => the answer stated no usable constraint

    std::atomic<int> available_{0};
    std::mutex       mtx_;
    std::string      pendingLevel_;
    int              pendingMonitor_ = -1;
};

// ---------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------

void runCaptureLoop(FrameSource& capture, QualityDirector& director, Streamer& streamer,
                    const Options& opt, bool daemon,
                    const std::atomic<bool>& rejoinWanted) {
    Nv12Buffer nv12;
    FramePacer pacer(director.frameRate());

    const auto idleRefresh =
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(opt.idleRefreshSec));

    auto lastSent  = std::chrono::steady_clock::now() - idleRefresh;
    auto lastStats = std::chrono::steady_clock::now();
    uint64_t skipped = 0, captured = 0;
    uint64_t prevBytes = 0, prevFrames = 0;

    if (!daemon) std::printf("\nstreaming. Ctrl+C to stop.\n\n");
    logI("streaming started");

    while (g_running.load() && !stopRequested() && streamer.isConnected() &&
           !rejoinWanted.load()) {
        // Apply anything the viewer or the congestion controller asked for, and
        // follow the resulting frame rate. Doing this here means the encoder is
        // only ever reconfigured from this one thread.
        if (!director.tick()) break;
        pacer.setFps(director.frameRate());

        const Frame* frame = capture.capture();
        if (frame && frame->data) {
            ++captured;
            const auto now = std::chrono::steady_clock::now();

            // A static screen costs nothing: skip conversion, encoding and
            // transmission entirely, but never go longer than --idle-refresh
            // without a frame or a late-joining viewer would see nothing.
            const bool forced = (now - lastSent) >= idleRefresh;
            if (frame->duplicate && !forced) {
                ++skipped;
            } else {
                // Frames arrive at native resolution; the conversion pass box
                // filters down to the ENCODER's current size in the same sweep,
                // so a quality switch costs nothing extra here.
                bgraToNv12(frame->data, frame->stride, frame->width, frame->height,
                           director.encodeWidth(), director.encodeHeight(), nv12);
                director.submit(nv12, frame->timeNs);
                lastSent = now;
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastStats >= 2s) {
            const double secs = std::chrono::duration<double>(now - lastStats).count();
            const auto   s    = streamer.stats();

            const double kbps  = (s.bytesSent - prevBytes) * 8.0 / 1000.0 / secs;
            const double fpsTx = (s.framesSent - prevFrames) / secs;
            prevBytes  = s.bytesSent;
            prevFrames = s.framesSent;

            const std::string line = soi::format(
                "{} {}x{} [{}] | {:.0f} kbps | {:.1f}/{:.1f} fps sent/cap (cap {}) | "
                "{} static | loss {:.1f}% | rtt {:.0f} ms | link {} kbps{}",
                director.level().name, director.encodeWidth(), director.encodeHeight(),
                backendName(capture.backend()),
                kbps, fpsTx, captured / secs, director.frameRate(), skipped,
                s.lossFraction * 100.0, s.rttMs, s.targetBitrateKbps,
                s.rembSeen ? " [remb]" : "");

            if (daemon) {
                writeStateFile("stats.txt", line);
                logT("{}", line);
            } else {
                std::printf("\r  %s   ", line.c_str());
                std::fflush(stdout);
            }

            skipped = captured = 0;
            lastStats = now;
        }

        pacer.waitNext();
    }
    if (!daemon) std::printf("\n");
    logI("streaming stopped");
}

// Waits for `soi-share answer <blob>` to drop answer.blob into the state dir.
bool waitForAnswerFile(std::string& blobOut, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (!g_running.load() || stopRequested()) return false;

        std::string blob;
        if (readStateFile("answer.blob", blob) && !blob.empty()) {
            removeStateFile("answer.blob");   // one-shot
            blobOut = blob;
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(250ms);
    }
}

// One connection attempt: negotiate, stream until it ends, tear down.
// `recoverable` is set when the session ended because the network went away
// rather than because the operator asked us to stop.
int runOneSession(const Options& opt, bool daemon, const std::string& shareCode,
                  const std::string& roomId, bool& recoverable) {
    recoverable = false;
    CaptureConfig capCfg;
    capCfg.target         = opt.target;
    capCfg.backend        = opt.backend;
    capCfg.monitorIndex   = opt.monitorIndex;
    capCfg.captureCursor  = opt.cursor;
    capCfg.includeLayered = opt.layered;
    capCfg.maxWidth       = opt.maxWidth;

    if (opt.target == CaptureTarget::Window) {
        capCfg.windowHandle = resolveWindowSpec(opt.windowSpec);
        if (!capCfg.windowHandle) return 1;
    }

    // Protection is established for the whole process in runSession(), BEFORE
    // any capture object exists, and stays up between attempts. It is
    // deliberately not started here.
    //
    // The factory hands back a source that picks its own backend and can change
    // it mid-session, so nothing below this line knows or cares which API is
    // reading the screen.
    auto captureOwner = createFrameSource(capCfg);
    if (!captureOwner->start()) return 1;
    FrameSource& capture = *captureOwner;

    logI("colour conversion: {} path, {} worker(s)",
         colorConvertUsesSimd() ? "SSSE3" : "scalar", sharedPool().size());

    StreamerConfig netCfg;
    netCfg.stunUrls       = opt.stunUrls;
    netCfg.turnUrl        = opt.turnUrl;
    netCfg.turnUser       = opt.turnUser;
    netCfg.turnPass       = opt.turnPass;
    // Advertise the LARGEST size this session could ever encode, not the size it
    // starts at. The viewer can switch up to "source" AND to a different, bigger
    // monitor mid-stream, while the SDP's profile-level-id is fixed at
    // negotiation time -- understating it here would let a strict decoder reject
    // the very frames the viewer just asked for.
    netCfg.width  = capture.width();
    netCfg.height = capture.height();
    if (opt.target == CaptureTarget::Monitor) {
        for (const auto& m : enumerateMonitors()) {
            int w = m.width, h = m.height;
            if (opt.maxWidth > 0 && w > opt.maxWidth) {
                h = static_cast<int>(h * (static_cast<double>(opt.maxWidth) / w) + 0.5);
                w = opt.maxWidth;
            }
            if (static_cast<long long>(w) * h >
                static_cast<long long>(netCfg.width) * netCfg.height) {
                netCfg.width  = w & ~1;
                netCfg.height = h & ~1;
            }
        }
    }
    netCfg.fps            = opt.fps;
    // Likewise the congestion-control ceiling: it has to cover the highest
    // level, or REMB would be clamped below what a viewer on a fast link could
    // actually use.
    netCfg.maxBitrateKbps = opt.bitrateKbps > 0 ? opt.bitrateKbps : highestLevelBitrateKbps();
    netCfg.minBitrateKbps = opt.minBitrateKbps;

    Streamer streamer;
    if (!streamer.start(netCfg)) return 1;

    setDaemonState(DaemonState::Gathering);
    if (!streamer.waitForGathering(15s))
        logW("ICE gathering did not complete in 15s; using what we have");

    const std::string sdp = streamer.localDescriptionSdp();
    if (sdp.empty()) { logE("no local description was produced"); return 1; }

    const std::string blob =
        encodeSignalBlob(sdp, shareCode.empty() ? opt.passphrase : shareCode);
    if (blob.empty()) return 1;

    // A fresh session per attempt, so a reconnect never collects the previous
    // attempt's answer.
    const std::string sessionId = generateSessionId();

    std::string liveCode = shareCode;
    if (!liveCode.empty() && !sessionId.empty()) {
        const auto published = publishOffer(opt.serviceUrl, roomId, sessionId, blob);
        if (!published.ok) {
            logE("could not publish the share code: {}", published.error);
            // Do NOT quietly downgrade to the manual route and keep running: the
            // code the user just read out would be dead for the rest of the
            // session with nothing on screen to say so. Treat it like any other
            // network failure -- back off and republish the SAME code.
            if (opt.reconnect) {
                logW("the code is not reachable right now; retrying shortly");
                recoverable = true;
                return 1;
            }
            logW("falling back to the manual code route");
            liveCode.clear();
        } else {
            writeStateFile("code.txt", formatShareCode(shareCode));
            writeStateFile("service.txt", opt.serviceUrl);
            // The room carries a 10-minute idle expiry, but polling for the
            // answer refreshes it, so in practice the code is good for as long
            // as this process is running. Saying "valid for 10 minutes" was
            // wrong and would send someone chasing an expiry that never fires.
            logI("share code published and held open while this session runs");
        }
    }

    writeStateFile("offer.blob", blob);
    setDaemonState(DaemonState::AwaitingAnswer);
    // The blob is sealed with the share code in code mode, and only falls back
    // to --pass when there is no code. Reporting on `passphrase` alone used to
    // print "NOT encrypted" for every short-code session, which was alarming
    // and wrong.
    logI("offer ready, {} chars ({})", blob.size(),
         !liveCode.empty()          ? "AES-256-GCM, sealed with the share code"
         : !opt.passphrase.empty()  ? "AES-256-GCM, sealed with --pass"
                                    : "NOT encrypted -- it reveals your IP addresses; use --pass");

    // ---- local handover: serve the viewer so the peer only opens a URL ----
    LocalHandover handover;
    removeStateFile("urls.txt");
    if (opt.http) {
        std::string viewerHtml;
        if (readFileText(exeDirectory() + "\\viewer.html", viewerHtml)) {
            const bool up = handover.start(
                opt.httpPort, viewerHtml, blob,
                [](const std::string& answer) {
                    // Reuse the same one-shot file the `answer` command writes,
                    // so both delivery routes converge on one code path.
                    return writeStateFile("answer.blob", answer);
                });

            if (up) {
                std::string list;
                for (const auto& url : handover.urls()) list += url + "\n";
                writeStateFile("urls.txt", list);
                logI("handover page ready on port {}", handover.port());
            } else {
                logW("handover page unavailable; fall back to sending viewer.html "
                     "and pasting the answer");
            }
        } else {
            logW("viewer.html not found next to the executable; handover disabled");
        }
    }

    std::string answerBlob;

    // Preferred path: poll the rendezvous for the viewer's sealed answer.
    //
    // There is deliberately no deadline here beyond a day. The share code is
    // the promise this tool makes -- "give this to your friend and they can
    // come in" -- and it has to hold for as long as the daemon is running, not
    // for ten minutes. Polling also keeps the room from expiring, so a friend
    // who joins an hour later finds the code exactly as it was read out.
    if (!liveCode.empty()) {
        const auto got = waitForAnswer(opt.serviceUrl, roomId, sessionId, 24 * 60 * 60,
                                       answerBlob,
                                       [] { return stopRequested() || !g_running.load(); });
        if (!got.ok) {
            const bool askedToStop = stopRequested() || !g_running.load();
            if (!got.error.empty()) logE("share code failed: {}", got.error);
            else if (askedToStop) logI("stop requested while waiting for someone to join");

            // A manual answer may still have been delivered meanwhile.
            std::string manual;
            if (readStateFile("answer.blob", manual) && !manual.empty()) {
                removeStateFile("answer.blob");
                answerBlob = manual;
            } else {
                // Not the operator stopping us: the rendezvous went wrong, or
                // the room lapsed. Re-offer the SAME code rather than exiting
                // and leaving the friend with a code that no longer works.
                if (!askedToStop) recoverable = true;
                return 1;
            }
        }
    }

    if (answerBlob.empty() && daemon) {
        // No stdin in detached mode: the answer arrives via `soi-share answer`.
        if (!waitForAnswerFile(answerBlob, 10 * 60 * 1000)) {
            // Distinguish a real timeout from an operator-requested stop; the
            // two mean very different things when reading the log afterwards.
            if (stopRequested() || !g_running.load())
                logI("stop requested while waiting for the answer");
            else
                logE("no answer received within 10 minutes");
            return 1;
        }
    } else if (answerBlob.empty()) {
        std::printf("\n=============================== OFFER ===============================\n"
                    "%s\n"
                    "=====================================================================\n",
                    blob.c_str());
        if (copyToClipboard(blob)) std::printf("  copied to clipboard\n");
        std::printf("\nPaste the ANSWER blob here.\n> ");
        std::fflush(stdout);
        if (!std::getline(std::cin, answerBlob) || answerBlob.empty()) {
            logE("no answer provided");
            return 1;
        }
    }

    // The answer is sealed with whatever sealed the offer: the share code in
    // code mode, otherwise --pass.
    const std::string& answerKey = liveCode.empty() ? opt.passphrase : liveCode;

    std::string answerSdp, error;
    if (!decodeSignalBlob(answerBlob, answerKey, answerSdp, error)) {
        logE("could not read the answer: {}", error);
        return 1;
    }
    if (!streamer.acceptAnswer(answerSdp, error)) {
        logE("the answer was rejected: {}", error);
        return 1;
    }

    setDaemonState(DaemonState::Connecting);
    logI("connecting...");
    // 15s, deliberately SHORTER than the viewer's 25s attempt timeout.
    //
    // The ordering is the contract that makes reconnection converge: whoever
    // gives up first decides what happens next, and it has to be the sender,
    // because only the sender can publish a new offer. The viewer waits out its
    // own attempt, sees a NEW session id appear, and answers that instead of
    // re-answering the corpse of this one.
    if (!streamer.waitForConnected(15s)) {
        if (streamer.hasFailed())
            logE("ICE failed -- if this repeats, both peers are likely behind "
                 "symmetric NAT, which needs a TURN relay (see --turn)");
        else
            logE("the viewer did not complete the handshake within 15s");

        // Retryable: the viewer may simply have gone away mid-handshake, or be
        // about to come back. Exiting here would strand the session.
        recoverable = true;
        return 1;
    }

    // What the viewer's decoder actually accepts, read from its answer rather
    // than assumed. Browsers answer with level 3.1 unless the page raises it,
    // and 3.1 stops at exactly 1280x720 -- so without reading this the sender
    // happily encodes 1080p that the far end cannot properly decode.
    const int peerLevel = h264LevelFromSdp(answerSdp);
    const int peerMaxMbs = maxFrameMacroblocksForLevel(peerLevel);
    if (peerLevel)
        logI("viewer decodes up to H.264 level {}.{} ({} macroblocks per frame)",
             peerLevel / 10, peerLevel % 10, peerMaxMbs ? peerMaxMbs : 0);
    else
        logW("the viewer's answer states no H.264 level; not constraining resolution");

    // The director owns the encoder from here on, because changing quality means
    // rebuilding it.
    QualityDirector director(streamer, capture, capCfg, opt, peerMaxMbs);
    if (!director.startEncoder()) return 1;

    streamer.setKeyframeRequestHandler([&director] { director.requestKeyframe(); });
    // Congestion control no longer drives the encoder's bitrate directly. It
    // reports what the link will carry, and the director converts that into a
    // frame rate -- so a slow link costs smoothness, not sharpness.
    streamer.setBitrateTargetHandler([&director](int kbps) { director.setAvailableBitrate(kbps); });
    streamer.setQualityRequestHandler([&director](const std::string& level) {
        director.requestLevel(level);
    });
    streamer.setMonitorRequestHandler([&director](int index) { director.requestMonitor(index); });
    streamer.setControlReadyHandler([&director] { director.announce(); });
    director.requestKeyframe();   // the viewer cannot decode until the first IDR

    setDaemonState(DaemonState::Streaming);
    removeStateFile("offer.blob");   // consumed; do not leave it lying around

    // ---- let the viewer come back at any time, without touching this PC ----
    //
    // The offer stays published while streaming, and a watcher keeps polling
    // for an answer to it. Only someone holding the code can produce one, and
    // the only reason to produce one is to join -- so an answer arriving here
    // means "a viewer wants in".
    //
    // We cannot feed a second answer to a peer connection that already has one,
    // so the knock is used purely as a signal: end this session, republish, and
    // let them complete the handshake against the fresh offer. Costs one extra
    // round trip and takes a few seconds.
    //
    // This is what makes rejoining work when the viewer left deliberately --
    // closed the tab, pressed Disconnect -- rather than by losing the network.
    // In that case nothing tells this process its peer is gone, and without the
    // knock it would keep streaming to nobody until ICE consent finally expires.
    // A knock is only honoured once the current session has had a fair run.
    //
    // This tool serves ONE viewer. If two people hold the code, both their
    // pages answer the offer, and without this each would knock the other out
    // the instant it connected -- neither ever seeing a picture. Observed
    // exactly that with two tabs open. A minimum uptime turns a thrash into
    // slow alternation, which is visibly "someone else is joining" rather than
    // a broken stream.
    constexpr auto kMinSessionBeforeHandover = 15s;
    const auto sessionStart = std::chrono::steady_clock::now();

    std::atomic<bool> rejoinWanted{false};
    std::thread knockWatcher;
    if (!liveCode.empty()) {
        knockWatcher = std::thread([&] {
            // Keep watching for as long as the session lasts: a viewer may knock
            // once, go away, and knock again much later.
            while (!rejoinWanted.load() && !stopRequested() && g_running.load() &&
                   streamer.isConnected()) {
                std::string knock;
                const auto got = waitForAnswer(
                    opt.serviceUrl, roomId, sessionId, 24 * 60 * 60, knock,
                    [&] {
                        return rejoinWanted.load() || stopRequested() || !g_running.load() ||
                               !streamer.isConnected();
                    });
                if (!got.ok || knock.empty()) return;

                // Ignore a re-read of the answer this session was built from.
                // The handshake consumes it, but a retry or a duplicate delivery
                // can put the very same bytes back, and treating that as "a new
                // viewer is knocking" tears down a perfectly healthy session --
                // which then republishes, gets answered, and tears down again,
                // forever. Only a DIFFERENT answer means somebody new.
                if (knock == answerBlob) {
                    logT("ignoring a repeat of the answer this session already used");
                    continue;
                }
                const auto age = std::chrono::steady_clock::now() - sessionStart;
                if (age < kMinSessionBeforeHandover) {
                    logT("a knock arrived {} ms in; holding this session until {} s",
                         std::chrono::duration_cast<std::chrono::milliseconds>(age).count(),
                         std::chrono::duration_cast<std::chrono::seconds>(
                             kMinSessionBeforeHandover).count());
                    std::this_thread::sleep_for(1s);
                    continue;
                }
                logI("a viewer is asking to join -- re-offering the session");
                rejoinWanted.store(true);
                return;
            }
        });
    }

    runCaptureLoop(capture, director, streamer, opt, daemon, rejoinWanted);

    const bool handingOver = rejoinWanted.load();
    rejoinWanted.store(true);            // release the watcher if it is still polling
    if (knockWatcher.joinable()) knockWatcher.join();

    // Distinguish "the network died" or "a viewer wants back in" from "the
    // operator stopped us": only the first two should re-offer the code.
    recoverable = g_running.load() && !stopRequested();
    if (handingOver) logI("session ended to let a viewer rejoin");

    setDaemonState(DaemonState::Stopping);
    handover.stop();
    director.stopEncoder();
    streamer.stop();
    capture.stop();
    return 0;
}

// Drives connection attempts until the operator stops us.
//
// The share code is generated ONCE and reused across reconnects: after a
// network drop on either side, the sender republishes a fresh offer under the
// same code and the viewer rejoins by itself. Nobody has to read out a new
// code, which is the whole point.
int runSession(const Options& opt, bool daemon) {
    // ---- capture protection, for the life of the process -------------------
    //
    // Established here rather than per attempt, and before a capture object
    // exists at all. Two gaps this closes:
    //
    //   * it used to start AFTER capture.start(), so there was a window -- small,
    //     but real -- where the pipeline was live and exclusion was not;
    //   * it used to stop at the end of every attempt and restart at the top of
    //     the next, leaving the reconnect backoff (up to 15 seconds) completely
    //     unprotected. Anything that appeared in that gap would have been
    //     captured the instant the next attempt began.
    //
    // The sweep interval is short because the guarantee wanted here is "at any
    // instant", and the cost is one EnumThreadWindows pass over this process's
    // own windows -- microseconds.
    //
    // What this is NOT: a security boundary. Exclusion is enforced by DWM at
    // composition time, so it covers every user-mode capture path uniformly --
    // BitBlt, PrintWindow, DXGI Desktop Duplication, Windows.Graphics.Capture --
    // and none of the kernel-mode ones. See CaptureProtect.h.
    ProtectionWatchdog watchdog;
    if (opt.protect) {
        logI("capture protection: {}", describeProtection(ProtectMode::ExcludeFromCapture));
        protectOwnWindows(ProtectMode::ExcludeFromCapture);

        std::string note;
        const bool consoleOk = protectConsoleWindow(ProtectMode::ExcludeFromCapture, note);
        logI("console window: {}", note);
        if (!consoleOk && !daemon)
            logW("this terminal IS visible to screen capture -- use "
                 "'soi-share start' so nothing of ours is on screen at all");

        watchdog.start(ProtectMode::ExcludeFromCapture, 200);
    } else {
        logW("capture protection disabled");
    }

    // The code is tied to this machine, not to this session.
    //
    // It is generated once on first run and kept in the state directory, so a
    // friend who has it can reconnect tomorrow without anyone resending
    // anything. --new-code rotates it, which is also how you revoke access from
    // someone who already has the old one.
    std::string shareCode, roomId;
    if (opt.useCode) {
        if (!opt.newCode) readStateFile("machine.code", shareCode);

        // Guard against a truncated or hand-edited file.
        if (shareCode.size() != 6) shareCode.clear();

        if (shareCode.empty()) {
            shareCode = generateShareCode();
            if (shareCode.empty()) { logE("could not generate a share code"); return 1; }
            writeStateFile("machine.code", shareCode);
            logI("generated a new share code for this machine");
        } else {
            logI("reusing this machine's existing share code");
        }

        roomId = roomIdForCode(shareCode);
        if (roomId.empty()) { logE("could not derive the room id"); return 1; }
    }

    // Clear the room on the way out, whichever way we leave. A stopped sender
    // that keeps advertising an offer makes a viewer's reconnect loop answer a
    // peer that no longer exists, once per retry, at the cost of a full ICE
    // timeout each time.
    struct RoomGuard {
        const Options&     opt;
        const std::string& roomId;
        ~RoomGuard() {
            if (roomId.empty()) return;
            const auto closed = closeRoom(opt.serviceUrl, roomId);
            if (closed.ok) logI("share code withdrawn from the rendezvous");
            else           logT("could not withdraw the share code: {}", closed.error);
        }
    } roomGuard{opt, roomId};

    int backoffSeconds = 2;

    for (int attempt = 1; g_running.load() && !stopRequested(); ++attempt) {
        bool recoverable = false;
        const int rc = runOneSession(opt, daemon, shareCode, roomId, recoverable);

        if (!g_running.load() || stopRequested()) return 0;
        if (!opt.reconnect) return rc;

        // A setup failure that is not a dropped connection (bad monitor, no
        // encoder) will fail identically every time; retrying is just a spin.
        if (!recoverable && rc != 0) return rc;

        if (recoverable)
            logW("connection lost after attempt {} -- the code stays valid, "
                 "waiting for the viewer to come back", attempt);
        else
            logI("nobody joined; republishing the same code");

        // A session that actually streamed proves the path works, so the next
        // failure starts from a short delay again. Without this reset a long
        // session followed by a blip inherits a 15s wait for no reason.
        if (rc == 0 && recoverable) backoffSeconds = 2;

        // Back off gently so a long outage does not hammer the rendezvous,
        // but stay responsive when the network returns quickly.
        for (int i = 0; i < backoffSeconds * 10; ++i) {
            if (!g_running.load() || stopRequested()) return 0;
            std::this_thread::sleep_for(100ms);
        }
        backoffSeconds = std::min(backoffSeconds * 2, 15);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommands
// ---------------------------------------------------------------------------

int cmdStart(int argc, char** argv) {
    if (daemonRunning()) {
        std::printf("soi-share is already running. Use 'soi-share status', or "
                    "'soi-share stop' first.\n");
        return 1;
    }

    Options opt;
    if (!parseOptions(argc, argv, 2, opt)) return 2;

    // Clear stale state before spawning so we never read a previous run's offer.
    removeStateFile("offer.blob");
    removeStateFile("answer.blob");
    removeStateFile("stats.txt");
    removeStateFile("code.txt");   // display copy only; machine.code persists

    const unsigned long pid = spawnDetached(rebuildArgs(argc, argv, 2));
    if (!pid) { std::printf("failed to start the background process\n"); return 1; }

    std::printf("soi-share started (pid %lu), detached.\n", pid);
    std::printf("log: %s\n", stateFilePath("soi.log").c_str());

    DaemonState reached = DaemonState::NotRunning;
    if (!waitForState({DaemonState::AwaitingAnswer}, 25000, &reached)) {
        std::printf("\nthe daemon did not produce an offer (state: %s).\n"
                    "see the log for why.\n", describeState(reached).c_str());
        return 1;
    }

    std::string blob;
    if (!readStateFile("offer.blob", blob) || blob.empty()) {
        std::printf("offer file missing\n");
        return 1;
    }

    // The share code is the headline: one thing to say out loud.
    std::string code, service;
    readStateFile("code.txt", code);
    readStateFile("service.txt", service);
    if (service.empty()) service = "https://share.mdarif.online";

    // The code is the whole interface. Anything printed near it competes with
    // it, so the long fallback blob is now behind --no-code rather than being
    // dumped under every single start.
    if (!code.empty()) {
        std::string host = service;
        if (host.rfind("https://", 0) == 0) host.erase(0, 8);
        else if (host.rfind("http://", 0) == 0) host.erase(0, 7);

        std::printf("\n  Your friend opens   %s\n", host.c_str());
        std::printf("  and enters\n\n");
        std::printf(enableAnsi() ? "      \x1b[1;97m%s\x1b[0m\n\n" : "      %s\n\n",
                    code.c_str());
        copyToClipboard(code);
        std::printf("  The screen appears as soon as they type it. Any network --\n"
                    "  they do not have to be on your Wi-Fi. The code belongs to this\n"
                    "  PC and stays the same next time. (copied to your clipboard)\n");
    }

    std::string urls;
    readStateFile("urls.txt", urls);

    if (!urls.empty()) {
        std::string first;
        size_t pos = 0;
        while (pos < urls.size()) {
            size_t nl = urls.find('\n', pos);
            if (nl == std::string::npos) nl = urls.size();
            const std::string url = urls.substr(pos, nl - pos);
            if (!url.empty() && first.empty()) first = url;
            pos = nl + 1;
        }
        if (!first.empty())
            std::printf("\n  On this Wi-Fi they can also just open  %s\n", first.c_str());
    }

    // Only when there is no short code to say out loud. Eight hundred characters
    // of base64 is not something to print by default.
    if (code.empty()) {
        std::printf("\n  Send your friend viewer.html once, then this offer:\n\n%s\n\n"
                    "  and run:  soi-share answer <what-they-send-back>\n",
                    blob.c_str());
        std::printf("  %zu chars%s\n", blob.size(),
                    opt.passphrase.empty()
                        ? "  (NOT encrypted -- it reveals your IP addresses; use --pass)"
                        : "  (AES-256-GCM encrypted)");

        const std::string viewerPath = exeDirectory() + "\\viewer.html";
        if (fileExists(viewerPath))
            std::printf("  viewer.html: %s\n", viewerPath.c_str());
    }

    // Opening a browser here would put a window on the very screen being shared,
    // so it is strictly opt-in.
    const std::string viewerPath = exeDirectory() + "\\viewer.html";
    if (opt.openViewer && fileExists(viewerPath)) {
        ShellExecuteW(nullptr, L"open",
                      toUtf16(buildViewerUrl(viewerPath, blob)).c_str(),
                      nullptr, nullptr, SW_SHOWNORMAL);
        std::printf("  (viewer also opened locally, as requested)\n");
    }

    std::printf("\n  status: soi-share status      stop: soi-share stop\n");
    return 0;
}

int cmdAnswer(int argc, char** argv) {
    if (!daemonRunning()) {
        std::printf("soi-share is not running. Start it with 'soi-share start'.\n");
        return 1;
    }
    if (argc < 3) { std::printf("usage: soi-share answer <blob>\n"); return 2; }

    // Accept the blob as one argument, or as the remaining words joined: a blob
    // is a single token, but shells and copy-paste vary.
    std::string blob;
    for (int i = 2; i < argc; ++i) blob += argv[i];

    if (!writeStateFile("answer.blob", blob)) {
        std::printf("could not hand the answer to the daemon\n");
        return 1;
    }
    std::printf("answer delivered, connecting...\n");

    DaemonState reached = DaemonState::NotRunning;
    if (waitForState({DaemonState::Streaming}, 45000, &reached)) {
        std::printf("connected -- streaming.\n");
        return 0;
    }
    std::printf("did not reach streaming (state: %s). See %s\n",
                describeState(reached).c_str(), stateFilePath("soi.log").c_str());
    return 1;
}

int cmdStatus() {
    unsigned long pid = 0;
    if (!daemonRunning(&pid)) { std::printf("soi-share: not running\n"); return 1; }

    const DaemonState state = currentDaemonState();
    std::printf("soi-share: %s (pid %lu)\n", describeState(state).c_str(), pid);

    std::string stats;
    if (state == DaemonState::Streaming && readStateFile("stats.txt", stats) &&
        !stats.empty())
        std::printf("  %s\n", stats.c_str());

    std::printf("  log: %s\n", stateFilePath("soi.log").c_str());
    return 0;
}

int cmdOffer() {
    std::string blob;
    if (!readStateFile("offer.blob", blob) || blob.empty()) {
        std::printf("no pending offer (either not started, or already connected)\n");
        return 1;
    }
    std::printf("%s\n", blob.c_str());
    copyToClipboard(blob);
    return 0;
}

int cmdStop() {
    unsigned long pid = 0;
    if (!daemonRunning(&pid)) { std::printf("soi-share: not running\n"); return 0; }

    if (!signalStop())
        std::printf("could not signal the daemon; it may still be starting up\n");

    // Give it a few seconds to shut the encoder and peer connection down cleanly.
    for (int i = 0; i < 60 && daemonRunning(); ++i)
        std::this_thread::sleep_for(100ms);

    if (daemonRunning()) {
        std::printf("daemon did not exit; terminating pid %lu\n", pid);
        if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, pid)) {
            TerminateProcess(h, 1);
            CloseHandle(h);
        }
        std::this_thread::sleep_for(300ms);
        clearPidFile();
    }

    setDaemonState(DaemonState::NotRunning);
    removeStateFile("offer.blob");
    removeStateFile("stats.txt");
    removeStateFile("code.txt");   // display copy only; machine.code persists
    std::printf("soi-share stopped\n");
    return 0;
}

// Wipe SOI's entire on-disk footprint. Unlike `stop`, this also removes the log
// and the persistent share code, so the next `start` behaves like a first run on
// a fresh machine. There is nothing to undo, so it says exactly what it did.
int cmdPurge() {
    int         removed = 0;
    std::string note;
    if (!purgeState(removed, note)) {
        std::printf("cannot purge: %s\n", note.c_str());
        return 1;
    }
    std::printf("%s\n", note.c_str());
    if (removed)
        std::printf("the share code is gone; the next start mints a new one.\n");
    return 0;
}

int runDaemon(int argc, char** argv) {
    if (!acquireInstanceLock()) return 1;   // another daemon owns this session

    logSetFile(stateFilePath("soi.log"));
    createStopEvent();
    writePidFile();
    setDaemonState(DaemonState::Starting);

    Options opt;
    int rc = 2;
    if (parseOptions(argc, argv, 2, opt)) {
        logSetVerbose(opt.verbose);
        logI("--- soi-share daemon starting (pid {}) ---", GetCurrentProcessId());
        rc = runSession(opt, true);
    }

    setDaemonState(DaemonState::NotRunning);
    removeStateFile("offer.blob");
    removeStateFile("stats.txt");
    removeStateFile("code.txt");   // display copy only; machine.code persists
    clearPidFile();
    logI("--- soi-share daemon exiting (rc {}) ---", rc);
    closeStopEvent();
    return rc;
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(consoleHandler, TRUE);

    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) { logE("CoInitializeEx failed: {}", hrString(coHr)); return 1; }

    const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
    if (FAILED(mfHr)) {
        logE("MFStartup failed: {}", hrString(mfHr));
        CoUninitialize();
        return 1;
    }

    const std::string cmd = argc > 1 ? argv[1] : "";
    int rc = 0;

    if (cmd.empty() || cmd == "--help" || cmd == "-h" || cmd == "help") {
        printUsage();
    } else if (cmd == "--daemon") {
        rc = runDaemon(argc, argv);
    } else if (cmd == "start") {
        rc = cmdStart(argc, argv);
    } else if (cmd == "answer") {
        rc = cmdAnswer(argc, argv);
    } else if (cmd == "status") {
        rc = cmdStatus();
    } else if (cmd == "offer") {
        rc = cmdOffer();
    } else if (cmd == "stop") {
        rc = cmdStop();
    } else if (cmd == "purge") {
        rc = cmdPurge();
    } else if (cmd == "list-monitors") {
        listMonitors();
    } else if (cmd == "list-windows") {
        listWindows();
    } else if (cmd == "capture-check") {
        Options opt;
        if (!parseOptions(argc, argv, 2, opt)) { rc = 2; }
        else { logSetVerbose(opt.verbose); captureCheck(opt); }
    } else if (cmd == "run") {
        Options opt;
        if (!parseOptions(argc, argv, 2, opt)) { rc = 2; }
        else {
            logSetVerbose(opt.verbose);
            createStopEvent();
            rc = runSession(opt, false);
            closeStopEvent();
        }
    } else {
        logE("unknown command: {}", cmd);
        printUsage();
        rc = 2;
    }

    MFShutdown();
    CoUninitialize();
    return rc;
}
