//
// soi-share -- serverless peer-to-peer screen sharing. Terminal only, no GUI.
//
// The streaming process runs DETACHED: `start` relaunches this exe as
// `start --foreground --log-file <path>` with no console and returns, so closing
// the launching terminal does not kill it, and `status` / `answer` / `stop`
// reach it from any other terminal over a loopback control channel.
//
// Session flow (no signalling server anywhere in it):
//   soi-share start                 -> prints a 6-character code
//   (the viewer types it at the code page)
//   soi-share stop                  -> shuts it down
//
// The exe also manages itself: install / update / uninstall (app/Lifecycle.h).
//
#include "app/Control.h"
#include "app/Install.h"
#include "app/Lifecycle.h"
#include "app/LocalHandover.h"
#include "app/Rendezvous.h"
#include "app/Service.h"
#include "capture/CaptureFactory.h"
#include "capture/CaptureProtect.h"
#include "encode/ColorConvert.h"
#include "encode/H264Encoder.h"
#include "encode/Quality.h"
#include "gpu/GpuPipeline.h"
#include "net/SignalBlob.h"
#include "net/Streamer.h"
#include "util/Log.h"
#include "util/Parallel.h"
#include "util/Platform.h"

#if defined(_WIN32)
  #include "capture/DxgiCapture.h"
  #include "util/Win.h"
  #include <windows.h>
  #include <conio.h>
  #include <mfapi.h>
  #include <shellapi.h>
  // gpu-check builds a converter to prove the shader path really works, and its
  // ComPtr members need the complete D3D11 interfaces to destruct.
  #include <d3d11.h>
#else
  #include <csignal>
  #include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

using namespace soi;
using namespace std::chrono_literals;

namespace {

#if defined(_WIN32)
// Present only in Windows 10 1803+ SDKs.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

std::atomic<bool> g_running{true};

// True for `start --foreground` in a terminal: the join instructions are
// printed there as well as the log. False for the detached background copy.
bool g_attached = false;

bool printJoinInfo(const std::map<std::string, std::string>& live);

#if defined(_WIN32)
BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        g_running.store(false);
        return TRUE;
    }
    return FALSE;
}
#else
// Ctrl+C, `kill`, and the terminal window closing (SIGHUP) all mean the same as
// a console close on Windows: stop cleanly, withdrawing the share code.
// std::atomic<bool> is lock-free here, so the store is async-signal-safe.
void signalHandler(int) { g_running.store(false); }
#endif

// How to type this program at the prompt. On macOS a binary that was simply
// unzipped somewhere is not on PATH, so `soi-share status` would be "command
// not found"; the hints say `./soi-share` there instead.
std::string selfCommand() {
#if defined(_WIN32)
    return "soi-share";
#else
    static const std::string cmd = [] {
        const std::string dir = directoryOf(currentExePath());
        const std::string path = envVar("PATH");
        size_t start = 0;
        while (start <= path.size()) {
            size_t end = path.find(':', start);
            if (end == std::string::npos) end = path.size();
            std::string entry = path.substr(start, end - start);
            while (entry.size() > 1 && entry.back() == '/') entry.pop_back();
            if (!entry.empty() && entry == dir) return std::string("soi-share");
            start = end + 1;
        }
        char cwd[4096] = {};
        if (getcwd(cwd, sizeof cwd) && dir == cwd) return std::string("./soi-share");
        return currentExePath();
    }();
    return cmd;
#endif
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

    // Where the colour conversion happens, and whether the frame ever comes back
    // to the CPU at all. See gpu/GpuPipeline.h -- this is a capability choice,
    // not only a speed one, which is why it is picked at start and held.
    Pipeline pipeline = Pipeline::Auto;

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

    // Whether the viewer may change WHAT is shared, as opposed to how it looks.
    //
    // Sharing one window is already an explicit narrowing -- the operator picked
    // that window and nothing else -- so a window share is ALWAYS locked and
    // this flag is not consulted. It exists to apply the same rule to a screen
    // share: --monitor 1 --lock-target shares monitor 1 and only ever monitor 1.
    bool lockTarget = false;
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

    // Process management, not session settings: stay attached instead of
    // detaching, and where the instance writes its log.
    bool        foreground = false;
    std::string logFile;
};

#if defined(_WIN32)
void printUsage() {
    std::printf("soi-share %s -- serverless P2P screen sharing from the terminal\n", appVersion());
    std::puts(R"(
QUICK START
  soi-share start          start sharing in the background; prints a 6-character
                           code your friend types at https://share.mdarif.online
  soi-share status         is it running? pid, uptime, the code, live stats
  soi-share stop           stop sharing

SHARING
  start [options]          start in the background and return to the prompt
  start --foreground       stay attached with a live log instead (Ctrl+C stops)
  status                   pid, uptime, state and code of the running share
  stop [--force]           stop it; --force ends it if it will not stop
  answer <blob>            hand a viewer's answer to the running share (--no-code)
  offer                    reprint the pending offer blob (--no-code)
  list-monitors            enumerate monitors
  list-windows             enumerate capturable windows

INSTALL AND UPDATE (per-user; never needs admin)
  install                  copy this exe to %LOCALAPPDATA%\Programs\soi-share and
                           add that folder to your PATH
  update                   download the latest release, verify its SHA-256 and
                           replace the installed exe; a running share restarts on it
  update --check           only say whether a newer release exists
  update --force           reinstall the latest release even if this is current
  update --forget-token    delete the saved GitHub token
  uninstall [--purge]      stop, remove from PATH, delete the program folder. Your
                           share code is kept unless --purge
  version                  version, and whether this is the installed copy
  licenses                 third-party license notices
  help                     this text

DIAGNOSTICS
  capture-check            try every capture backend and report what works
  gpu-check                report whether frames can stay on the GPU, and why not
  run [options]            foreground session that reads the answer from stdin
  purge                    erase all on-disk state (log, share code, saved token)

CAPTURE TARGET
  --monitor <N>          share monitor N (default: 0)
  --desktop              share every screen at once, as one wide picture
  --window <hwnd|text>   share one window, by handle or title substring

                         This only sets where the session STARTS. On a screen
                         share the viewer is shown how many screens this PC has
                         and can switch between them -- or to all of them at
                         once -- from their browser, without touching this PC.
                         `soi-share list-monitors` shows what they will see.

                         A WINDOW share is never switchable: the operator chose
                         one window, and no message from the far end can widen
                         that to a screen. Use --lock-target to pin a screen
                         share the same way.

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

PIPELINE
  --gpu                  keep every frame on the graphics card: Desktop
                         Duplication's texture is colour-converted to NV12 by a
                         shader and handed straight to the hardware encoder. No
                         readback, no CPU conversion, no upload. Fails to start
                         if this machine cannot do it, rather than falling back.

  --cpu                  always read frames back and convert with SSE2. Slower,
                         and the only path that can composite --cursor.

  --pipeline <mode>      auto | gpu | cpu   (default: auto)
                         Auto is --gpu where every precondition holds and --cpu
                         otherwise. `soi-share gpu-check` says which you get and
                         why.

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
  --lock-target              pin the share to the target named above. The viewer
                             gets no screen picker and is not told what other
                             screens exist. Implied by --window.
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
  --foreground           (start) stay attached with a live log
  --log-file <path>      (start) log here instead of
                         %LOCALAPPDATA%\soi-share\soi-share.log)");
}
#else
void printUsage() {
    std::printf("soi-share %s -- serverless P2P screen sharing from the terminal\n", appVersion());
    if (selfCommand() != "soi-share")
        std::printf("\nNot on your PATH: from this folder, type ./soi-share instead of soi-share\n"
                    "(or run `./soi-share install` once to put it on your PATH).\n");
    std::puts(R"(
QUICK START
  soi-share start          start sharing in the background; prints a 6-character
                           code your friend types at https://share.mdarif.online
  soi-share status         is it running? pid, uptime, the code, live stats
  soi-share stop           stop sharing

  The first start asks macOS for Screen Recording permission for your terminal
  app: allow it in System Settings > Privacy & Security > Screen Recording, then
  quit and reopen the terminal.

SHARING
  start [options]          start in the background and return to the prompt
  start --foreground       stay attached with a live log instead (Ctrl+C stops)
  status                   pid, uptime, state and code of the running share
  stop [--force]           stop it; --force ends it if it will not stop
  answer <blob>            hand a viewer's answer to the running share (--no-code)
  offer                    reprint the pending offer blob (--no-code)
  list-monitors            enumerate displays
  list-windows             enumerate capturable windows

INSTALL AND UPDATE (per-user; never needs admin; entirely optional)
  install                  copy this binary to ~/.soi-share/bin and add that
                           folder to your PATH (in ~/.zshrc, ~/.bash_profile)
  update                   download the latest release, verify its SHA-256 and
                           replace this binary; a running share restarts on it
  update --check           only say whether a newer release exists
  update --force           reinstall the latest release even if this is current
  update --forget-token    delete the saved GitHub token
  uninstall [--purge]      stop, remove from PATH, delete the program folder. Your
                           share code is kept unless --purge
  version                  version, and whether this is the installed copy
  licenses                 third-party license notices
  help                     this text

DIAGNOSTICS
  capture-check            try every capture backend and report what works
  gpu-check                report whether frames can stay on the GPU, and why not
  run [options]            foreground session that reads the answer from stdin
  purge                    erase all on-disk state (log, share code, saved token)

CAPTURE TARGET
  --monitor <N>          share display N (default: 0, the main display)
  --desktop              share every display at once, as one wide picture
  --window <id|text>     share one window, by window id or title substring

                         This only sets where the session STARTS. On a screen
                         share the viewer is shown how many displays this Mac
                         has and can switch between them -- or to all of them at
                         once -- from their browser, without touching this Mac.
                         `soi-share list-monitors` shows what they will see.

                         A WINDOW share is never switchable: the operator chose
                         one window, and no message from the far end can widen
                         that to a screen. Use --lock-target to pin a screen
                         share the same way.

  --capture <backend>    auto | sck | stream | cgimage   (default: auto)
                         Auto is what makes the share show everything on the
                         screen: ScreenCaptureKit (macOS 12.3+) reads displays
                         and windows from the compositor, GPU-resident and with
                         the cursor drawn by the system; CGDisplayStream covers
                         older macOS; CGImage capture is the floor. If the
                         running backend stops producing frames the next one
                         takes over mid-session.

                         Naming one pins it and disables that fallback, which is
                         for diagnosing a problem, not for sharing. Run
                         `soi-share capture-check` first.

PIPELINE
  --gpu                  keep every frame on the graphics card: ScreenCaptureKit's
                         IOSurface is scaled and converted to NV12 by the GPU and
                         handed straight to the VideoToolbox encoder. No readback,
                         no CPU conversion. Fails to start if this Mac cannot do
                         it, rather than falling back.

  --cpu                  always read frames back and convert on the CPU (NEON on
                         Apple silicon, SSSE3 on Intel). Slower.

  --pipeline <mode>      auto | gpu | cpu   (default: auto)
                         Auto is --gpu where every precondition holds and --cpu
                         otherwise. `soi-share gpu-check` says which you get and
                         why.

QUALITY
  --quality <level>      360p | 480p | 720p | 1080p | source   (default 720p)
                         The VIEWER can change this at any time from their
                         browser; this only sets where the session starts.

                         On a slow link the resolution is HELD and the frame
                         rate drops instead, so text stays sharp and readable
                         rather than being smeared to fit the bitrate.

  --min-fps <N>          how far the frame rate may fall before per-frame
                         quality has to give way too (default 2)
  --fps <N>              hard cap on the frame rate, 1-30 (default 30)
  --bitrate <kbps>       override the level's bitrate ceiling
  --min-bitrate <kbps>   floor for congestion control (default 600)
  --max-width <px>       cap the capture size before scaling (default 1920)
  --gop <seconds>        keyframe interval (default 10)
  --idle-refresh <sec>   force a frame when the screen is static (default 2)
  --cursor               show the mouse pointer in the stream

PRIVACY
  --protect / --no-protect   exclude our own windows from capture (default on)
  --lock-target              pin the share to the target named above. The viewer
                             gets no screen picker and is not told what other
                             displays exist. Implied by --window.
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
  --new-code             rotate this Mac's share code (revokes the old one)
  --no-reconnect         exit when the connection drops instead of re-offering
                         the same code
  --no-code              do not use the short-code service; print a long blob
  --service <url>        rendezvous base URL (default https://share.mdarif.online)
  --port <N>             port for the local handover page (default 8000)
  --no-http              disable the handover page; use the send-a-file flow
  --open-viewer          also open the viewer on THIS machine (off by default)
  --verbose              trace logging
  --foreground           (start) stay attached with a live log
  --log-file <path>      (start) log here instead of
                         ~/Library/Application Support/soi-share/soi-share.log)");
}
#endif

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

        if (a == "--daemon")                 { continue; }   // 1.0.x marker, ignored
        else if (a == "--foreground")        { o.foreground = true; }
        else if (a == "--log-file") {
            const char* v = next("--log-file");
            if (!v) return false;
            o.logFile = v;
        }
        else if (a == "--desktop")           { o.target = CaptureTarget::VirtualDesktop; }
        else if (a == "--cursor")            { o.cursor = true; }
        else if (a == "--layered")           { o.layered = true; }
        else if (a == "--protect")           { o.protect = true; }
        else if (a == "--no-protect")        { o.protect = false; }
        else if (a == "--lock-target")       { o.lockTarget = true; }
        else if (a == "--gpu")               { o.pipeline = Pipeline::Gpu; }
        else if (a == "--cpu")               { o.pipeline = Pipeline::Cpu; }
        else if (a == "--pipeline") {
            const char* v = next("--pipeline");
            if (!v) return false;
            if (!parsePipelineName(v, o.pipeline)) {
                logE("unknown pipeline '{}'; choose one of: auto, gpu, cpu", v);
                return false;
            }
        }
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
                logE("unknown capture backend '{}'; choose one of: {}", v, backendChoices());
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

// The user's own options, minus the process-management ones, so the detached
// child gets exactly what was typed -- and cannot be told to detach again.
std::vector<std::string> sessionArgs(int argc, char** argv, int first) {
    std::vector<std::string> args;
    for (int i = first; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--foreground" || a == "--daemon") continue;
        if (a == "--log-file") { ++i; continue; }
        args.push_back(a);
    }
    return args;
}

std::string joinQuoted(const std::vector<std::string>& args) {
    std::string out;
    for (const auto& a : args) {
        if (!out.empty()) out += ' ';
        out += quoteArgument(a);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// The bounding box of every monitor together -- what --desktop captures, and
// what the viewer gets when they pick "all screens". On a single-monitor machine
// it is that monitor; with several it is wider than any one of them, and it
// includes the dead space between mismatched screens.
void virtualDesktopSize(int& width, int& height) {
#if defined(_WIN32)
    width  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
#else
    // The bounding box of every display in the global coordinate space.
    int left = 0, top = 0, right = 0, bottom = 0;
    bool first = true;
    for (const auto& m : enumerateMonitors()) {
        if (first || m.x < left) left = m.x;
        if (first || m.y < top) top = m.y;
        if (first || m.x + m.width > right) right = m.x + m.width;
        if (first || m.y + m.height > bottom) bottom = m.y + m.height;
        first = false;
    }
    width  = right - left;
    height = bottom - top;
#endif
}

void listMonitors() {
    const auto mons = enumerateMonitors();
    std::printf("\n%-6s %-16s %-14s %-10s %s\n", "INDEX", "DEVICE", "SIZE", "ORIGIN", "PRIMARY");
    for (const auto& m : mons)
        std::printf("%-6d %-16s %-14s %-10s %s\n", m.index, m.name.c_str(),
                    soi::format("{}x{}", m.width, m.height).c_str(),
                    soi::format("{},{}", m.x, m.y).c_str(), m.primary ? "yes" : "");

    int deskW = 0, deskH = 0;
    virtualDesktopSize(deskW, deskH);
    std::printf("\n%zu monitor(s); all screens together are %dx%d\n",
                mons.size(), deskW, deskH);
    std::printf("The viewer chooses between these from their browser. To stop "
                "that, add --lock-target.\n");
}

// The hand-rolled control JSON has no escaping of its own, and a device name
// like \\.\DISPLAY1 is mostly backslashes -- emitting it raw produces a message
// the viewer cannot parse, which silently costs it the whole screen list.
std::string jsonEscape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    for (const unsigned char c : in) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) out += soi::format("\\u{:04x}", static_cast<int>(c));
                else          out += static_cast<char>(c);
        }
    }
    return out;
}

void listWindows() {
    const auto wins = enumerateWindows();
#if defined(_WIN32)
    std::printf("\n%-18s %-12s %-24s %s\n", "HWND", "SIZE", "PROCESS", "TITLE");
    const char* idFormat = "0x%-16llX %-12s %-24s %s\n";
#else
    std::printf("\n%-18s %-12s %-24s %s\n", "WINDOW ID", "SIZE", "APP", "TITLE");
    const char* idFormat = "%-18llu %-12s %-24s %s\n";
#endif
    for (const auto& w : wins) {
        std::string title = w.title;
        if (title.size() > 60) title = title.substr(0, 57) + "...";
        std::printf(idFormat,
                    static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(w.handle)),
                    soi::format("{}x{}", w.width, w.height).c_str(),
                    w.process.c_str(), title.c_str());
    }
    std::printf("\n%zu window(s)\n", wins.size());
}

// A window handle (HWND) on Windows, a CGWindowID on macOS -- both carried in
// a void* so the capture layer can stay agnostic.
void* resolveWindowSpec(const std::string& spec) {
    if (spec.rfind("0x", 0) == 0 || spec.rfind("0X", 0) == 0) {
        const auto value = std::strtoull(spec.c_str() + 2, nullptr, 16);
        if (value) return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
    }
    if (!spec.empty() && spec.find_first_not_of("0123456789") == std::string::npos) {
        const auto value = std::strtoull(spec.c_str(), nullptr, 10);
        if (value) return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
    }
    for (const auto& w : enumerateWindows())
        if (containsNoCase(w.title, spec) || containsNoCase(w.process, spec)) {
            logI("matched window '{}' ({})", w.title, w.process);
            return w.handle;
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

#if defined(_WIN32)
    std::printf(
        "\nMS/GRAB on a still screen is mostly the cost of ASKING whether anything\n"
        "changed. dxgi and wgc are told by the compositor; bitblt has to re-read\n"
        "and hash the whole screen to find out, which is the 16 ms.\n");
#else
    std::printf(
        "\nMS/GRAB on a still screen is mostly the cost of ASKING whether anything\n"
        "changed. sck and stream are told by the compositor; cgimage has to copy\n"
        "and hash the whole screen to find out.\n");
#endif
    std::printf("\n%d of 3 backends can read this target.\n", working);
    if (working) {
        std::printf("Sharing picks a working one automatically; no flag needed.\n\n");
    } else {
#if defined(_WIN32)
        std::printf(
            "\nNothing can read it. That normally means the target is protected with\n"
            "SetWindowDisplayAffinity, or is only visible on the secure desktop. No\n"
            "user-mode capture API on Windows can read either -- see README 1.2.\n\n");
#else
        std::printf(
            "\nNothing can read it. Almost always that is the Screen Recording\n"
            "permission: allow your terminal app in System Settings > Privacy &\n"
            "Security > Screen Recording, then quit and reopen the terminal. A window\n"
            "that marks itself as not shareable also reads as black.\n\n");
#endif
    }
}

// "Why am I not getting the GPU pipeline?" answered without a debugger.
//
// Each precondition is checked by actually doing it, not by inferring it from a
// driver version: the capture is started, the device is shared, the encoder is
// asked whether it will take that device. Anything that fails here is exactly
// what would have failed at `start`.
#if defined(_WIN32)
void gpuCheck(const Options& opt) {
    CaptureConfig cfg;
    cfg.target        = opt.target;
    cfg.monitorIndex  = opt.monitorIndex;
    cfg.captureCursor = opt.cursor;
    cfg.maxWidth      = opt.maxWidth;
    cfg.backend       = CaptureBackend::Dxgi;
    cfg.preferGpu     = true;

    std::printf("\nchecking whether frames can stay on the graphics card...\n\n");

    if (opt.target == CaptureTarget::Window) {
        std::printf("  target      NO -- a window share is Windows.Graphics.Capture, and\n"
                    "                    only Desktop Duplication has a GPU path today.\n\n"
                    "Result: the CPU pipeline. Share a screen for the GPU one.\n\n");
        return;
    }
    std::printf("  target      yes -- a screen, which Desktop Duplication can read\n");

    if (opt.cursor) {
        std::printf("  --cursor    NO -- the cursor is drawn with GDI onto CPU pixels,\n"
                    "                    which the GPU path does not have.\n\n"
                    "Result: the CPU pipeline. Drop --cursor for the GPU one.\n\n");
        return;
    }

    DxgiCapture capture(cfg);
    if (!capture.start()) {
        std::printf("  capture     NO -- Desktop Duplication would not start at all.\n"
                    "                    Run `soi-share capture-check` for why.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }

    auto device = capture.gpuDevice();
    if (!device) {
        capture.stop();
        std::printf("  capture     NO -- duplication started, but not on a shared device.\n"
                    "                    Usually two graphics adapters, or a driver that\n"
                    "                    refused the texture. --verbose says which.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }
    std::printf("  capture     yes -- %s\n", device->describe().c_str());

    H264Encoder probe;
    const bool takesGpu = probe.enableGpuInput(device);
    std::printf("  encoder     %s -- %s\n", takesGpu ? "yes" : "NO ",
                probe.describe().c_str());

    if (!takesGpu) {
        capture.stop();
        std::printf("\n  This encoder is not D3D11-aware, so it can only be fed from\n"
                    "  system memory. That is normal for the Microsoft software MFT.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }

    Nv12GpuConverter conv;
    const int w = capture.width() & ~1, h = capture.height() & ~1;
    const bool shader = conv.init(device, w, h);
    std::printf("  shader      %s -- BGRA to NV12 at %dx%d\n", shader ? "yes" : "NO ", w, h);
    conv.reset();
    capture.stop();

    if (!shader) {
        std::printf("\n  This device cannot render to NV12, so the conversion has to\n"
                    "  happen on the CPU even though everything else lined up.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }

    std::printf("\nResult: the GPU pipeline. `soi-share start --monitor %d` uses it\n"
                "automatically; --gpu makes it an error if it ever stops being\n"
                "available, and --cpu forces the SSE2 path instead.\n\n",
                opt.monitorIndex);
}
#else
// The macOS GPU path is ScreenCaptureKit's IOSurface -> VTPixelTransferSession
// (scale + BGRA->NV12 on the GPU) -> a VideoToolbox encoder. Each link is
// checked by actually building it.
void gpuCheck(const Options& opt) {
    CaptureConfig cfg;
    cfg.target        = opt.target;
    cfg.monitorIndex  = opt.monitorIndex;
    cfg.captureCursor = opt.cursor;
    cfg.maxWidth      = opt.maxWidth;
    cfg.backend       = CaptureBackend::Sck;
    cfg.preferGpu     = true;

    std::printf("\nchecking whether frames can stay on the graphics card...\n\n");

    if (opt.target == CaptureTarget::Window) {
        cfg.windowHandle = resolveWindowSpec(opt.windowSpec);
        if (!cfg.windowHandle) return;
    }

    auto capture = createFrameSource(cfg);
    if (!capture->start()) {
        std::printf("  capture     NO -- ScreenCaptureKit would not start. It needs macOS\n"
                    "                    12.3 or later and the Screen Recording permission;\n"
                    "                    `soi-share capture-check` says which.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }
    auto device = capture->gpuDevice();
    if (!device) {
        capture->stop();
        std::printf("  capture     NO -- capture started, but not with GPU-resident frames.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }
    std::printf("  capture     yes -- %s\n", device->describe().c_str());

    H264Encoder probe;
    const bool takesGpu = probe.enableGpuInput(device);
    std::printf("  encoder     %s -- %s\n", takesGpu ? "yes" : "NO ", probe.describe().c_str());
    if (!takesGpu) {
        capture->stop();
        std::printf("\n  No hardware H.264 encoder is available, so frames are encoded in\n"
                    "  software from system memory.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }

    Nv12GpuConverter conv;
    const int w = capture->width() & ~1, h = capture->height() & ~1;
    const bool converter = conv.init(device, w, h);
    std::printf("  converter   %s -- BGRA to NV12 at %dx%d\n", converter ? "yes" : "NO ", w, h);
    conv.reset();
    capture->stop();
    if (!converter) {
        std::printf("\n  The GPU pixel-transfer session could not be created, so the\n"
                    "  conversion has to happen on the CPU.\n\n"
                    "Result: the CPU pipeline.\n\n");
        return;
    }
    std::printf("\nResult: the GPU pipeline. `soi-share start` uses it automatically;\n"
                "--gpu makes it an error if it ever stops being available, and --cpu\n"
                "forces the CPU path instead.\n\n");
}
#endif

std::string exeDirectory() { return directoryOf(currentExePath()); }

bool readFileText(const std::string& path, std::string& out) {
    return readFileBytes(path, out) && !out.empty();
}

bool embeddedViewer(std::string& out) {
    return loadEmbeddedResource(kViewerResourceId, out);
}

// file:// URL with the offer in the fragment. Browsers never transmit a
// fragment, and this is a local file regardless.
std::string buildViewerUrl(const std::string& viewerPath, const std::string& blob) {
    // "file:///C:/x" on Windows; a POSIX path already starts with the slash.
    std::string url = viewerPath.rfind("/", 0) == 0 ? "file://" : "file:///";
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
// timer granularity is ~15.6ms, which at 30fps would alias badly. macOS timers
// are already sub-millisecond, so a plain sleep is as good there.
class FramePacer {
public:
    explicit FramePacer(int fps)
        : interval_(std::chrono::nanoseconds(1'000'000'000LL / std::max(1, fps))) {
        fps_ = std::max(1, fps);
#if defined(_WIN32)
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
#endif
        next_ = std::chrono::steady_clock::now();
    }
#if defined(_WIN32)
    ~FramePacer() { if (timer_) CloseHandle(timer_); }
#endif
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

#if defined(_WIN32)
        const auto delay = next_ - now;
        if (!timer_) { std::this_thread::sleep_for(delay); return; }

        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(delay).count() / 100);
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, INFINITE);
        else
            std::this_thread::sleep_for(delay);
#else
        std::this_thread::sleep_until(next_);
#endif
    }

private:
#if defined(_WIN32)
    HANDLE                                timer_ = nullptr;
#endif
    std::chrono::nanoseconds              interval_;
    std::chrono::steady_clock::time_point next_;
    int                                   fps_ = 30;
};

// Must match kPeakPercent in H264Encoder.cpp: the burst headroom the encoder is
// configured with, and therefore the headroom that has to be reserved out of
// the link estimate when choosing a frame rate.
constexpr int kEncoderPeakPercent = 125;

// The least time between two screen switches. Each one tears down a capture
// backend and rebuilds the encoder, so this is both a courtesy to the person
// watching -- who gets a settled picture instead of a flicker -- and the thing
// that stops a viewer from turning a picker into a way to keep this machine
// rebuilding its encoder. Short enough that a deliberate click feels immediate.
constexpr auto kSwitchCooldown = std::chrono::milliseconds(750);

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
          peerMaxMacroblocks_(peerMaxMacroblocks),
          // Decided ONCE, here, from what the operator asked for on the command
          // line -- never from anything that arrives over the wire. A window
          // share is locked unconditionally: widening "share this window" into
          // "share this screen" on the strength of a message from the far end
          // would hand the viewer more than the operator agreed to.
          allowSwitch_(!opt.lockTarget && capCfg.target != CaptureTarget::Window) {
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

        // Put the encoder on the capture's own device, so its input texture and
        // our output texture are the same kind of thing on the same adapter.
        // Offered before start() because the answer changes how the MFT
        // allocates. Null when the capture is on the CPU path -- including after
        // a mid-session fall back to WGC or GDI, which is why this is asked
        // again on every encoder rebuild rather than cached.
        if (auto dev = capture_.gpuDevice()) encoder_.enableGpuInput(dev);

        if (!encoder_.start(cfg, [this](const uint8_t* nal, size_t len, bool key, int64_t pts) {
                streamer_.sendFrame(nal, len, key, pts);
            }))
            return false;

        logI("quality {}: {}x{} up to {} fps, {} at {} kbps, {} pipeline",
             level_->name, encW_, encH_, cfg.fps,
             encoder_.usesQualityRateControl() ? "peak-constrained VBR" : "CBR",
             cfg.bitrateKbps, encoder_.usesGpuInput() ? "GPU" : "CPU");
        encoder_.requestKeyframe();
        announce();
        return true;
    }

    void stopEncoder() { encoder_.stop(); }

    // The capture has moved onto a backend whose frames live on the GPU (it
    // climbed back to the best one after an outage), but the encoder was built
    // for system-memory input. Rebuild it so it is offered the GPU path again.
    bool restartEncoder() {
        // Once per device: if the rebuilt encoder still refuses GPU input,
        // rebuilding again on every frame would only spin.
        const auto dev = capture_.gpuDevice();
        if (!dev || dev.get() == gpuRebuildTried_) return true;
        gpuRebuildTried_ = dev.get();
        encoder_.stop();
        return startEncoder();
    }

    // Applies anything the other threads asked for. Returns false if a rebuild
    // was needed and failed, which ends the session.
    bool tick() {
        std::string wanted, wantedKind;
        int wantedIndex = -1;
        {
            std::lock_guard lk(mtx_);
            wanted.swap(pendingLevel_);

            // A switch costs an encoder rebuild, so only one is taken per
            // cooldown. The rest of a burst collapses into the LATEST request
            // rather than being dropped, so a viewer clicking twice quickly
            // lands on the second screen -- and a viewer holding the key down
            // cannot make this process rebuild the encoder in a tight loop.
            if (!pendingKind_.empty() &&
                std::chrono::steady_clock::now() - lastSwitch_ >= kSwitchCooldown) {
                wantedKind.swap(pendingKind_);
                wantedIndex = pendingIndex_;
            }
        }

        if (!wantedKind.empty() && !switchTarget(wantedKind, wantedIndex)) return false;
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
    bool submitTexture(void* bgra, int64_t ptsNs) {
#if defined(_WIN32)
        return encoder_.submitTexture(static_cast<ID3D11Texture2D*>(bgra), ptsNs);
#else
        return encoder_.submitTexture(bgra, ptsNs);   // a CVPixelBufferRef
#endif
    }
    bool onGpu() const { return encoder_.usesGpuInput(); }
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

    // The viewer picking something else to watch. Recorded only -- every check
    // that matters happens on the capture thread, against the live state of the
    // machine and the permission decided at construction.
    void requestTarget(const std::string& kind, int index) {
        std::lock_guard lk(mtx_);
        pendingKind_  = kind;
        pendingIndex_ = index;
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

        // What the viewer may switch to.
        //
        // When the share is locked this stays EMPTY, and that is the point: the
        // list is not merely ignored on the way back in, it is never sent. It
        // would otherwise tell a viewer who was given one window how many
        // screens this PC has and how big each one is -- information the
        // operator never offered, leaking out of a feature they did not enable.
        //
        // When switching IS allowed the list is enumerated fresh each time
        // rather than cached: screens get plugged in and unplugged while a share
        // is running, and offering one that no longer exists is worse than
        // useless.
        std::string monitors;
        int screens = 0, deskW = 0, deskH = 0;
        if (allowSwitch_) {
            for (const auto& m : enumerateMonitors()) {
                if (!monitors.empty()) monitors += ',';
                monitors += soi::format(
                    R"({{"i":{},"w":{},"h":{},"primary":{},"name":"{}"}})",
                    m.index, m.width, m.height, m.primary ? "true" : "false",
                    jsonEscape(m.name));
                ++screens;
            }
            virtualDesktopSize(deskW, deskH);
        }

        const char* kind = capCfg_.target == CaptureTarget::Window           ? "window"
                           : capCfg_.target == CaptureTarget::VirtualDesktop ? "desktop"
                                                                             : "monitor";
        const int current = capCfg_.target == CaptureTarget::Monitor ? capCfg_.monitorIndex : -1;

        streamer_.sendControl(soi::format(
            R"({{"type":"quality","level":"{}","w":{},"h":{},"fps":{},"levels":[{}],)"
            R"("target":"{}","monitor":{},"canSwitch":{},"screens":{},)"
            R"("desktopW":{},"desktopH":{},"monitors":[{}]}})",
            level_->name, encW_, encH_, fps_, levels,
            kind, current, allowSwitch_ ? "true" : "false", screens,
            deskW, deskH, monitors));
    }

private:
    // Move the capture somewhere else mid-session. The peer connection, the
    // share code and the viewer's page are all untouched; only the pixels being
    // read change. A different screen usually has a different size, so the
    // encode size and the encoder are rebuilt -- the same path a quality change
    // takes.
    //
    // This runs on the capture thread and re-derives everything from scratch:
    // the request carries a name and a number, and neither is believed until it
    // has been matched against a permission decided before the peer existed and
    // a monitor list read a moment ago.
    //
    // Returning false ends the session, so it is reserved for "the encoder is
    // gone". A request that is refused, stale or simply impossible re-announces
    // the true state and returns true -- the viewer keeps the picture they had.
    bool switchTarget(const std::string& kind, int index) {
        lastSwitch_ = std::chrono::steady_clock::now();   // capture thread only

        if (!allowSwitch_) {
            logW("the viewer asked for '{}' but this share is locked to its "
                 "target; refusing", kind);
            announce();          // correct the viewer's idea of what it may do
            return true;
        }

        CaptureConfig next = capCfg_;
        next.windowHandle = nullptr;

        if (kind == "desktop") {
            if (capCfg_.target == CaptureTarget::VirtualDesktop) return true;
            int w = 0, h = 0;
            virtualDesktopSize(w, h);
            logI("viewer asked for every screen at once ({}x{})", w, h);
            next.target = CaptureTarget::VirtualDesktop;
        } else if (kind == "monitor") {
            const auto monitors = enumerateMonitors();
            const auto found = std::find_if(monitors.begin(), monitors.end(),
                                            [&](const MonitorInfo& m) { return m.index == index; });
            if (found == monitors.end()) {
                logW("viewer asked for monitor {}, which does not exist; ignoring", index);
                announce();      // correct the viewer's idea of what is available
                return true;
            }
            if (capCfg_.target == CaptureTarget::Monitor && capCfg_.monitorIndex == index)
                return true;     // already there
            logI("viewer asked for monitor {} ({}x{})", index, found->width, found->height);
            next.target       = CaptureTarget::Monitor;
            next.monitorIndex = index;
        } else {
            logT("ignoring an unrecognised capture target '{}'", kind);
            announce();
            return true;
        }

        if (!capture_.retarget(next)) {
            // retarget() restores the previous target before returning false, so
            // the stream is still live. Ending the session over a screen that
            // would not open is a worse answer than telling the viewer no.
            logE("could not switch to the requested screen; staying where we are");
            announce();
            return true;
        }
        capCfg_ = next;
        srcW_ = capture_.width();
        srcH_ = capture_.height();

        recomputeSize();
        encoder_.stop();
        if (!startEncoder()) {
            logE("could not restart the encoder after switching screen");
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

    // Fixed for the life of the session, from the operator's own command line.
    // const so no later code path can talk itself into flipping it.
    const bool allowSwitch_;

    std::atomic<int> available_{0};
    std::mutex       mtx_;
    std::string      pendingLevel_;
    std::string      pendingKind_;         // empty => nothing pending
    int              pendingIndex_ = -1;

    // Capture thread only, so it needs no synchronisation of its own; it is read
    // inside tick()'s lock merely because that is where the pending request is.
    std::chrono::steady_clock::time_point lastSwitch_{};

    // Identity only: the GPU device restartEncoder() last rebuilt for.
    const GpuDevice* gpuRebuildTried_ = nullptr;
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
        // Either kind of frame counts: on the GPU pipeline `data` is null and the
        // pixels are a texture instead.
        if (frame && (frame->data || frame->gpuTexture)) {
            ++captured;
            const auto now = std::chrono::steady_clock::now();

            // A static screen costs nothing: skip conversion, encoding and
            // transmission entirely, but never go longer than --idle-refresh
            // without a frame or a late-joining viewer would see nothing.
            const bool forced = (now - lastSent) >= idleRefresh;
            if (frame->duplicate && !forced) {
                ++skipped;
            } else if (frame->gpuTexture && director.onGpu()) {
                // The GPU path. The frame never left the card: a shader on the
                // encoder's own device converts it to NV12 and the texture goes
                // straight into the MFT. Nothing to convert here, nothing to
                // copy, and the downscale to the encoder's current size happens
                // inside that same shader pass.
                director.submitTexture(frame->gpuTexture, frame->timeNs);
                lastSent = now;
            } else if (frame->gpuTexture && !frame->data) {
                // GPU-only frames, but an encoder that cannot take them.
                logI("capture moved to a GPU-resident backend; rebuilding the encoder for it");
                if (!director.restartEncoder()) break;
            } else if (frame->data) {
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
                "{} {}x{} [{}/{}] | {:.0f} kbps | {:.1f}/{:.1f} fps sent/cap (cap {}) | "
                "{} static | loss {:.1f}% | rtt {:.0f} ms | link {} kbps{}",
                director.level().name, director.encodeWidth(), director.encodeHeight(),
                backendName(capture.backend()), director.onGpu() ? "gpu" : "cpu",
                kbps, fpsTx, captured / secs, director.frameRate(), skipped,
                s.lossFraction * 100.0, s.rttMs, s.targetBitrateKbps,
                s.rembSeen ? " [remb]" : "");

            if (daemon) {
                setLive("stats", line);
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

// Waits for an answer delivered by `soi-share answer <blob>` (over the control
// channel) or by the local handover page.
bool waitForPushedAnswer(std::string& blobOut, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    for (;;) {
        if (!g_running.load() || stopRequested()) return false;

        if (std::string blob; takeAnswer(blob)) {
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
    capCfg.preferGpu      = opt.pipeline != Pipeline::Cpu;

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

    // --gpu is a demand, not a preference: if the frame cannot stay on the card
    // then say so and stop, rather than running the CPU path under a flag that
    // says otherwise. Auto is where the quiet fallback belongs.
    if (opt.pipeline == Pipeline::Gpu && !capture.gpuDevice()) {
        logE("--gpu was requested but this capture cannot keep frames on the GPU. "
             "Run 'soi-share gpu-check' for the reason, or use --pipeline auto.");
        return 1;
    }

    if (capture.gpuDevice())
        logI("pipeline: GPU -- frames are converted and encoded without leaving "
             "the graphics card");
    else
        logI("pipeline: CPU -- {} colour conversion, {} worker(s)",
             colorConvertSimdName(), sharedPool().size());

    // Say plainly, in the log, how much of this machine the far end can reach.
    // "What am I actually sharing" should never need reasoning about.
    if (opt.lockTarget || opt.target == CaptureTarget::Window) {
        logI("locked to {} -- the viewer cannot change what is shared, and is "
             "not told what else is on this PC", capture.describe());
    } else {
        int deskW = 0, deskH = 0;
        virtualDesktopSize(deskW, deskH);
        logI("the viewer may switch between {} screen(s) or all of them at once "
             "({}x{}); --lock-target prevents this",
             enumerateMonitors().size(), deskW, deskH);
    }

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
    // A locked share can only ever encode the one target it started on, so its
    // advertised level is exactly that -- no reason to claim headroom for
    // screens this session will never read.
    if (opt.target != CaptureTarget::Window && !opt.lockTarget) {
        auto consider = [&](int w, int h) {
            if (w <= 0 || h <= 0) return;
            if (opt.maxWidth > 0 && w > opt.maxWidth) {
                h = static_cast<int>(h * (static_cast<double>(opt.maxWidth) / w) + 0.5);
                w = opt.maxWidth;
            }
            if (static_cast<long long>(w) * h >
                static_cast<long long>(netCfg.width) * netCfg.height) {
                netCfg.width  = w & ~1;
                netCfg.height = h & ~1;
            }
        };
        for (const auto& m : enumerateMonitors()) consider(m.width, m.height);

        // And every screen at once, which on a multi-monitor machine is larger
        // than any single one of them. Leaving this out is what would make
        // "all screens" the one choice in the picker that produces a frozen
        // picture on a strict decoder.
        int deskW = 0, deskH = 0;
        virtualDesktopSize(deskW, deskH);
        consider(deskW, deskH);
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
            setLive("code", formatShareCode(shareCode));
            setLive("service", opt.serviceUrl);
            // The room carries a 10-minute idle expiry, but polling for the
            // answer refreshes it, so in practice the code is good for as long
            // as this process is running. Saying "valid for 10 minutes" was
            // wrong and would send someone chasing an expiry that never fires.
            logI("share code published and held open while this session runs");
        }
    }

    setLive("offer", blob);
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
    clearLive("urls");
    if (opt.http) {
        std::string viewerHtml;
        if (readFileText(joinPath(exeDirectory(), "viewer.html"), viewerHtml) ||
            embeddedViewer(viewerHtml)) {
            const bool up = handover.start(
                opt.httpPort, viewerHtml, blob,
                [](const std::string& answer) {
                    // The same slot the `answer` command fills, so both
                    // delivery routes converge on one code path.
                    pushAnswer(answer);
                    return true;
                });

            if (up) {
                std::string list;
                for (const auto& url : handover.urls()) list += (list.empty() ? "" : " ") + url;
                setLive("urls", list);
                logI("handover page ready on port {}", handover.port());
            } else {
                logW("handover page unavailable; fall back to sending viewer.html "
                     "and pasting the answer");
            }
        } else {
            logW("no viewer page available (none next to the exe, none embedded); "
                 "handover disabled");
        }
    }

    // Only now is everything `start` shows in place: the code, and the LAN
    // address of the handover page.
    setDaemonState(DaemonState::AwaitingAnswer);
    if (g_attached && !liveCode.empty()) {
        static bool shown = false;   // the code is the same on every reconnect
        if (!shown) {
            std::map<std::string, std::string> live;
            for (const auto& [k, v] : liveSnapshot()) live[k] = v;
            printJoinInfo(live);
            std::printf("\n  Ctrl+C stops sharing. Live log:\n\n");
            std::fflush(stdout);
            shown = true;
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
            if (std::string manual; takeAnswer(manual)) {
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
        if (!waitForPushedAnswer(answerBlob, 10 * 60 * 1000)) {
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
    streamer.setTargetRequestHandler([&director](const std::string& kind, int index) {
        director.requestTarget(kind, index);
    });
    streamer.setControlReadyHandler([&director] { director.announce(); });
    director.requestKeyframe();   // the viewer cannot decode until the first IDR

    setDaemonState(DaemonState::Streaming);
    clearLive("offer");   // consumed

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

// The join instructions, printed identically by every command that shows them,
// so the code looks the same whether it came from a fresh start or a repeat one.
// Returns false when there is no share code (the --no-code route).
bool printJoinInfo(const std::map<std::string, std::string>& live) {
    auto get = [&](const char* key) {
        const auto it = live.find(key);
        return it == live.end() ? std::string() : it->second;
    };
    // Present only once the code is actually published, so a code that nobody
    // can join with is never shown.
    const std::string code = get("code");
    if (code.empty()) return false;

    std::string host = get("service");
    if (host.empty()) host = "https://share.mdarif.online";
    if (host.rfind("https://", 0) == 0) host.erase(0, 8);
    else if (host.rfind("http://", 0) == 0) host.erase(0, 7);

    std::printf("\n  Your friend opens   %s\n", host.c_str());
    std::printf("  and enters\n\n");
    std::printf(enableAnsi() ? "      \x1b[1;97m%s\x1b[0m\n\n" : "      %s\n\n", code.c_str());
    const bool copied = copyToClipboard(code);
    std::printf("  The screen appears as soon as they type it. Any network --\n"
                "  they do not have to be on your Wi-Fi. The code belongs to this\n"
                "  PC and stays the same next time.%s\n",
                copied ? " (copied to your clipboard)" : "");

    const std::string urls  = get("urls");
    const std::string first = urls.substr(0, urls.find(' '));
    if (!first.empty())
        std::printf("\n  On this Wi-Fi they can also just open  %s\n", first.c_str());
    return true;
}

void printControlHint() {
    const std::string c = selfCommand();
    std::printf("\n  next:  %s status     %s stop     %s help\n", c.c_str(), c.c_str(), c.c_str());
}

std::string describeUptime(long long secs) {
    if (secs < 60)   return soi::format("{}s", secs);
    if (secs < 3600) return soi::format("{}m {:02}s", secs / 60, secs % 60);
    return soi::format("{}h {:02}m", secs / 3600, (secs / 60) % 60);
}

std::map<std::string, std::string> liveStatus(const InstanceRecord& rec) {
    std::map<std::string, std::string> reply;
    if (!controlRequest(rec, "status", reply) || !reply.count("ok")) reply.clear();
    return reply;
}

std::string defaultLogPath() { return stateFilePath("soi-share.log"); }

// The end of the log, for when the background copy died before it could say
// anything over the control channel.
void printLogTail(const std::string& path, int lines) {
    std::string text;
    if (!readFileText(path, text)) {
        std::printf("  (the log %s is empty)\n", path.c_str());
        return;
    }
    size_t pos = text.size();
    for (int n = 0; n <= lines && pos > 0; ) {
        pos = text.find_last_of('\n', pos - 1);
        if (pos == std::string::npos) { pos = 0; break; }
        ++n;
    }
    std::printf("  last lines of %s:\n%s\n", path.c_str(), text.substr(pos).c_str());
}

// Prints what to do about an instance that is already there. Returns false if
// it is not in a state that can be used.
bool reportExisting(const FoundInstance& found) {
    const unsigned long pid = found.record.pid;
    switch (found.state) {
        case InstanceState::Running: {
            const auto live = liveStatus(found.record);
            std::printf("soi-share is already running (pid %lu, up %s). Not starting a second one.\n",
                        pid, describeUptime(std::atoll(live.count("uptime") ? live.at("uptime").c_str() : "0")).c_str());
            if (!printJoinInfo(live)) std::printf("  reprint the offer with: soi-share offer\n");
            printControlHint();
            return true;
        }
        case InstanceState::Unresponsive:
            std::printf("soi-share (pid %lu) is running but not answering its control channel.\n"
                        "Run `soi-share stop --force`, then start again.\n", pid);
            return false;
        case InstanceState::Legacy:
            std::printf("An older version of soi-share (pid %lu) is running.\n"
                        "Run `soi-share stop` first, then start again.\n", pid);
            return false;
        case InstanceState::None:
            break;
    }
    return true;
}

// The instance itself: `start --foreground`, attached to a terminal or -- as the
// background copy -- to nothing but its log.
int runInstance(const Options& opt, const std::vector<std::string>& args) {
    if (!acquireInstanceLock()) {
        std::printf("another soi-share is already running or starting; see `soi-share status`\n");
        return 1;
    }

    const std::string logPath = opt.logFile.empty() ? defaultLogPath() : opt.logFile;
    logSetFile(logPath, /*truncate=*/true);
    // Files 1.0.x kept its live state in; that state lives in memory now.
    for (const char* stale : {"soi.log", "status.txt", "code.txt", "service.txt", "urls.txt",
                              "stats.txt", "offer.blob", "answer.blob"})
        removeStateFile(stale);

    createStopEvent();
    setLive("args", joinQuoted(args));
    setLive("log", logPath);
    setDaemonState(DaemonState::Starting);
    g_attached = attachedToTerminal();
    logSetVerbose(opt.verbose);
#if !defined(_WIN32)
    if (!g_attached) {
        // The background copy. spawnDetached already started it in a session
        // of its own; this makes sure of it on systems where posix_spawn could
        // not, so the launching terminal closing can never reach it.
        setsid();
        std::signal(SIGHUP, SIG_IGN);
    }
#endif

    std::string error;
    if (!startControlServer(appVersion(), error)) {
        logE("cannot open the control channel: {}", error);
        closeStopEvent();
        return 1;
    }
    logI("--- soi-share {} starting (pid {}, {}) ---", appVersion(), currentProcessId(),
         g_attached ? "in a terminal" : "no console");

    const int rc = runSession(opt, true);

    setDaemonState(DaemonState::NotRunning);
    stopControlServer();
    logI("--- soi-share exiting (rc {}) ---", rc);
    closeStopEvent();
    return rc;
}

int cmdStart(int argc, char** argv) {
    const FoundInstance found = findInstance();
    if (found.cleanedStale)
        std::printf("(removed a stale record left by an earlier run that did not exit cleanly)\n");
    if (found.state != InstanceState::None) return reportExisting(found) ? 0 : 1;

    // Parsed here as well as in the child, so a typo is reported in THIS
    // terminal instead of in a log nobody is looking at.
    Options opt;
    if (!parseOptions(argc, argv, 2, opt)) {
        std::printf("see `soi-share help` for the options\n");
        return 2;
    }
    const std::vector<std::string> args = sessionArgs(argc, argv, 2);

#if defined(__APPLE__)
    // Ask for Screen Recording HERE, in the terminal, not in the background
    // copy: macOS attributes the permission to the terminal app, shows its
    // prompt only to something with a terminal, and only applies a new grant
    // once the terminal app has been restarted. Finding out now beats a share
    // whose viewer sees nothing but a black rectangle.
    if (!screenCapturePermitted(/*prompt=*/true)) {
        std::printf(
            "\nsoi-share needs the Screen Recording permission, which macOS grants to\n"
            "your terminal app (Terminal, iTerm, VS Code, ...):\n\n"
            "  1. Open System Settings > Privacy & Security > Screen Recording\n"
            "     (macOS 15: \"Screen & System Audio Recording\").\n"
            "  2. Turn it on for your terminal app. If it is not listed, click +\n"
            "     and add it from Applications (Utilities, for Terminal).\n"
            "  3. QUIT the terminal app completely (Cmd+Q) and open it again --\n"
            "     macOS only applies the permission to a freshly started app.\n"
            "  4. Run `%s start` again.\n\n", selfCommand().c_str());
        return 1;
    }
#endif

    if (opt.foreground) return runInstance(opt, args);

    const std::string logPath = opt.logFile.empty() ? defaultLogPath() : opt.logFile;
    std::vector<std::string> childArgs{"start", "--foreground", "--log-file", logPath};
    childArgs.insert(childArgs.end(), args.begin(), args.end());

    const unsigned long pid = spawnDetached(childArgs);
    if (!pid) {
        std::printf("could not start the background process; try `soi-share start --foreground` "
                    "to see why\n");
        return 1;
    }
    std::printf("soi-share started in the background (pid %lu)\n", pid);
    std::printf("getting a share code...\n");
    std::fflush(stdout);

    // Long enough to cover publishOffer's own retries on a slow network, so a
    // start that will succeed always ends with the code on screen.
    std::map<std::string, std::string> live;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    bool ready = false, died = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!processAlive(pid)) { died = true; break; }
        const FoundInstance now = findInstance();
        if (now.state == InstanceState::Running && now.record.pid == pid) {
            live = liveStatus(now.record);
            const DaemonState st = stateFromToken(live.count("state") ? live["state"] : "");
            if (st == DaemonState::AwaitingAnswer || st == DaemonState::Connecting ||
                st == DaemonState::Streaming) { ready = true; break; }
        }
        std::this_thread::sleep_for(150ms);
    }

    if (!ready) {
        std::printf(died ? "\nsoi-share stopped before it produced a share code.\n"
                         : "\nsoi-share has not produced a share code after 60 s "
                           "(it is still trying; `soi-share status` shows how far it got).\n");
        printLogTail(logPath, 12);
        return 1;
    }

    // The code is the whole interface. The long fallback blob is printed only
    // when there is no short code to say out loud (--no-code).
    const std::string blob = live.count("offer") ? live["offer"] : "";
    if (!printJoinInfo(live)) {
        std::printf("\n  Send your friend viewer.html once, then this offer:\n\n%s\n\n"
                    "  and run:  soi-share answer <what-they-send-back>\n",
                    blob.c_str());
        std::printf("  %zu chars%s\n", blob.size(),
                    opt.passphrase.empty()
                        ? "  (NOT encrypted -- it reveals your IP addresses; use --pass)"
                        : "  (AES-256-GCM encrypted)");

        const std::string viewerPath = joinPath(exeDirectory(), "viewer.html");
        if (fileExists(viewerPath))
            std::printf("  viewer.html: %s\n", viewerPath.c_str());
    }

    // Opening a browser here would put a window on the very screen being shared,
    // so it is strictly opt-in.
    const std::string viewerPath = joinPath(exeDirectory(), "viewer.html");
    if (opt.openViewer && fileExists(viewerPath) && !blob.empty()) {
        openUrl(buildViewerUrl(viewerPath, blob));
        std::printf("  (viewer also opened locally, as requested)\n");
    }

    std::printf("\n  log: %s\n", logPath.c_str());
    printControlHint();
    return 0;
}

// For the commands that need a running share: finds it, or says why not.
bool requireRunning(FoundInstance& found) {
    found = findInstance();
    if (found.cleanedStale)
        std::printf("(removed a stale record left by an earlier run that did not exit cleanly)\n");
    switch (found.state) {
        case InstanceState::Running: return true;
        case InstanceState::None:
            std::printf("soi-share is not running. Start it with `soi-share start`.\n");
            return false;
        case InstanceState::Unresponsive:
            std::printf("soi-share (pid %lu) is running but not answering its control channel.\n"
                        "Run `soi-share stop --force`, then `soi-share start`.\n", found.record.pid);
            return false;
        case InstanceState::Legacy:
            std::printf("An older version of soi-share (pid %lu) is running and cannot be asked\n"
                        "for details. Run `soi-share stop`, then `soi-share start`.\n",
                        found.record.pid);
            return false;
    }
    return false;
}

int cmdAnswer(int argc, char** argv) {
    if (argc < 3) { std::printf("usage: soi-share answer <blob>\n"); return 2; }
    FoundInstance found;
    if (!requireRunning(found)) return 1;

    // Accept the blob as one argument, or as the remaining words joined: a blob
    // is a single token, but shells and copy-paste vary.
    std::string blob;
    for (int i = 2; i < argc; ++i) blob += argv[i];

    std::map<std::string, std::string> reply;
    if (!controlRequest(found.record, "answer " + blob, reply) || !reply.count("ok")) {
        std::printf("could not hand the answer to soi-share (pid %lu)\n", found.record.pid);
        return 1;
    }
    std::printf("answer delivered, connecting...\n");

    const auto deadline = std::chrono::steady_clock::now() + 45s;
    std::string state;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto live = liveStatus(found.record);
        if (live.empty()) break;
        state = live.at("state");
        if (stateFromToken(state) == DaemonState::Streaming) {
            std::printf("connected -- streaming.\n");
            return 0;
        }
        std::this_thread::sleep_for(250ms);
    }
    std::printf("did not reach streaming (state: %s). See the log: %s\n",
                describeState(stateFromToken(state)).c_str(), defaultLogPath().c_str());
    return 1;
}

int cmdStatus() {
    const FoundInstance found = findInstance();
    if (found.cleanedStale)
        std::printf("(removed a stale record left by an earlier run that did not exit cleanly)\n");
    if (found.state == InstanceState::None) {
        std::printf("soi-share: not running\n  start it with: soi-share start\n");
        return 1;
    }
    FoundInstance running;
    if (!requireRunning(running)) return 1;

    const auto live = liveStatus(running.record);
    if (live.empty()) {
        std::printf("soi-share (pid %lu) did not answer; try again, or `soi-share stop --force`\n",
                    running.record.pid);
        return 1;
    }
    auto get = [&](const char* key) {
        const auto it = live.find(key);
        return it == live.end() ? std::string() : it->second;
    };
    const DaemonState state = stateFromToken(get("state"));
    std::printf("soi-share %s: %s\n", get("version").c_str(), describeState(state).c_str());
    std::printf("  pid      %s\n", get("pid").c_str());
    std::printf("  uptime   %s\n", describeUptime(std::atoll(get("uptime").c_str())).c_str());
    if (const auto code = get("code"); !code.empty()) {
        std::string host = get("service");
        if (host.rfind("https://", 0) == 0) host.erase(0, 8);
        std::printf("  code     %s   (at %s)\n", code.c_str(), host.c_str());
    }
    if (const auto urls = get("urls"); !urls.empty())
        std::printf("  on LAN   %s\n", urls.substr(0, urls.find(' ')).c_str());
    if (const auto stats = get("stats"); !stats.empty() && state == DaemonState::Streaming)
        std::printf("  stream   %s\n", stats.c_str());
    std::printf("  exe      %s\n", get("exe").c_str());
    std::printf("  log      %s\n", get("log").c_str());
    return 0;
}

int cmdOffer() {
    FoundInstance found;
    if (!requireRunning(found)) return 1;
    const auto live = liveStatus(found.record);
    const auto it = live.find("offer");
    if (it == live.end() || it->second.empty()) {
        std::printf("no pending offer (already connected, or still starting up)\n");
        return 1;
    }
    std::printf("%s\n", it->second.c_str());
    copyToClipboard(it->second);
    return 0;
}

int cmdStop(bool force) {
    const StopOutcome out = stopRunningInstance(force, true);
    return out.stopped ? 0 : 1;
}

// Wipe SOI's entire on-disk footprint. Unlike `stop`, this also removes the log
// and the persistent share code, so the next `start` behaves like a first run on
// a fresh machine. There is nothing to undo, so it says exactly what it did.
int cmdPurge() {
    if (findInstance().state != InstanceState::None) {
        std::printf("cannot purge: soi-share is still running; stop it first (soi-share stop)\n");
        return 1;
    }
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

#if defined(_WIN32)
// Double-clicked in Explorer. A terminal program opened that way gets a console
// of its own that closes the instant it exits, so: install, say what this is
// and how to use it, and wait for Enter.
int doubleClicked(int argc, char** argv) {
    std::printf("soi-share %s\n\n", appVersion());
    const int rc = cmdInstall(/*quiet=*/true);
    std::printf(
        "\n"
        "soi-share is a TERMINAL program; there is no window to open.\n"
        "Open a new terminal (right-click Start -> Terminal) and type:\n"
        "\n"
        "    soi-share start      start sharing; prints a code for your friend\n"
        "    soi-share status     see what it is doing\n"
        "    soi-share stop       stop sharing\n"
        "    soi-share help       everything else\n"
        "\n");
    std::printf(rc == 0 ? "Press Enter to close, or type S and Enter to start sharing now: "
                        : "Press Enter to close: ");
    std::fflush(stdout);

    std::string line;
    std::getline(std::cin, line);
    if (rc == 0 && !line.empty() && (line[0] == 's' || line[0] == 'S')) {
        char* startArgv[] = {argv[0], const_cast<char*>("start")};
        (void)argc;
        cmdStart(2, startArgv);
        std::printf("\nSharing continues in the background after this window closes.\n"
                    "Press Enter to close: ");
        std::fflush(stdout);
        std::getline(std::cin, line);
    }
    return rc;
}
#endif

bool hasFlag(int argc, char** argv, const char* flag) {
    for (int i = 2; i < argc; ++i)
        if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

// Rejects anything but `allowed` after the subcommand, so a typo like
// `uninstall --purg` does not silently do the non-purging thing.
bool onlyFlags(int argc, char** argv, std::initializer_list<const char*> allowed) {
    for (int i = 2; i < argc; ++i) {
        bool ok = false;
        for (const char* a : allowed) ok = ok || std::strcmp(argv[i], a) == 0;
        if (!ok) {
            std::printf("unknown option for %s: %s (see `soi-share help`)\n", argv[1], argv[i]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#else
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);
    std::signal(SIGHUP, signalHandler);
    std::signal(SIGPIPE, SIG_IGN);
#endif

    // An exe that `update` or `install` moved aside is deleted on the next run
    // of any command, once nothing is executing it any more.
    cleanupOldBinaries();

    const std::string cmd = argc > 1 ? argv[1] : "";

#if defined(_WIN32)
    const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(coHr)) { logE("CoInitializeEx failed: {}", hrString(coHr)); return 1; }

    // Media Foundation only for the commands that capture or encode. "N"
    // editions of Windows ship without it, and install / update / help must
    // still work there -- if only to tell the user what is missing.
    const bool needsMedia = cmd == "start" || cmd == "run" || cmd == "--daemon" ||
                            cmd == "capture-check" || cmd == "gpu-check";
    // `start` without --foreground only launches the background copy, which
    // starts Media Foundation itself.
    const bool launchesOnly = cmd == "start" && !hasFlag(argc, argv, "--foreground");
    bool mediaUp = false;
    if (needsMedia && !launchesOnly) {
        const HRESULT mfHr = MFStartup(MF_VERSION, MFSTARTUP_LITE);
        if (FAILED(mfHr)) {
            logE("Media Foundation is not available ({}). On an \"N\" edition of Windows, "
                 "install the Media Feature Pack: Settings > Apps > Optional features > "
                 "Add a feature > Media Feature Pack.", hrString(mfHr));
            CoUninitialize();
            return 1;
        }
        mediaUp = true;
    }
#endif

    int rc = 0;

#if defined(_WIN32)
    // Double-clicked in Explorer: no arguments, and the console was created for
    // this process alone.
    DWORD consolePids[2] = {};
    const bool ownConsole = GetConsoleProcessList(consolePids, 2) == 1;
#else
    // Double-clicked in Finder opens a Terminal window in the home folder and
    // runs this with no arguments. The usage text says how to run it from its
    // own folder, so nothing special is needed.
    const bool ownConsole = false;
#endif

    if (cmd.empty() && ownConsole) {
#if defined(_WIN32)
        rc = doubleClicked(argc, argv);
#endif
    } else if (cmd.empty() || cmd == "--help" || cmd == "-h" || cmd == "help" || cmd == "/?") {
        printUsage();
    } else if (cmd == "version" || cmd == "--version" || cmd == "-v") {
        rc = cmdVersion(hasFlag(argc, argv, "--short"));
    } else if (cmd == "install") {
        // install.ps1 fixes up its own session's PATH, so the exe's "open a
        // new terminal" advice would be wrong there.
        const bool quiet = envVar("SOI_SHARE_INSTALLER") == "1";
        rc = onlyFlags(argc, argv, {}) ? cmdInstall(quiet) : 2;
    } else if (cmd == "uninstall") {
        rc = onlyFlags(argc, argv, {"--purge"}) ? cmdUninstall(hasFlag(argc, argv, "--purge")) : 2;
    } else if (cmd == "update") {
        rc = onlyFlags(argc, argv, {"--check", "--force", "--forget-token"})
                 ? cmdUpdate(hasFlag(argc, argv, "--check"), hasFlag(argc, argv, "--force"),
                             hasFlag(argc, argv, "--forget-token"))
                 : 2;
    } else if (cmd == "licenses") {
        rc = cmdLicenses();
    } else if (cmd == "--daemon") {
        // How 1.0.x launched its background copy. Kept so a stale shortcut or
        // script still does something sensible.
        Options opt;
        if (!parseOptions(argc, argv, 2, opt)) rc = 2;
        else rc = runInstance(opt, sessionArgs(argc, argv, 2));
    } else if (cmd == "start") {
        rc = cmdStart(argc, argv);
    } else if (cmd == "answer") {
        rc = cmdAnswer(argc, argv);
    } else if (cmd == "status") {
        rc = cmdStatus();
    } else if (cmd == "offer") {
        rc = cmdOffer();
    } else if (cmd == "stop") {
        rc = onlyFlags(argc, argv, {"--force"}) ? cmdStop(hasFlag(argc, argv, "--force")) : 2;
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
    } else if (cmd == "gpu-check") {
        Options opt;
        if (!parseOptions(argc, argv, 2, opt)) { rc = 2; }
        else { logSetVerbose(opt.verbose); gpuCheck(opt); }
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
        std::printf("unknown command: %s\nRun `soi-share help` for the list of commands.\n",
                    cmd.c_str());
        rc = 2;
    }

#if defined(_WIN32)
    if (mediaUp) MFShutdown();
    CoUninitialize();
#endif
    return rc;
}
