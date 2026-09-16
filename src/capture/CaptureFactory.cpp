#include "capture/CaptureFactory.h"
#include "capture/BitBltCapture.h"
#include "capture/DxgiCapture.h"
#include "capture/WgcCapture.h"
#include "util/Log.h"

#include <chrono>
#include <map>

namespace soi {
namespace {

using Clock = std::chrono::steady_clock;

// How long capture() may keep returning nothing before the backend is written
// off. Generous on purpose: a UAC prompt blanks every capture path for as long
// as the user takes to read it, and changing backend would not help.
constexpr auto kFailureGrace = std::chrono::seconds(3);

std::unique_ptr<FrameSource> makeBackend(CaptureBackend backend, const CaptureConfig& cfg) {
    CaptureConfig c = cfg;
    c.backend = backend;
    switch (backend) {
        case CaptureBackend::Dxgi:   return std::make_unique<DxgiCapture>(c);
        case CaptureBackend::Wgc:    return std::make_unique<WgcCapture>(c);
        case CaptureBackend::BitBlt: return std::make_unique<BitBltCapture>(c);
        case CaptureBackend::Auto:   break;
    }
    return nullptr;
}

std::vector<CaptureBackend> preferenceOrder(const CaptureConfig& cfg) {
    if (cfg.backend != CaptureBackend::Auto) return {cfg.backend};

    if (cfg.target == CaptureTarget::Window)
        return {CaptureBackend::Wgc, CaptureBackend::BitBlt};

    return {CaptureBackend::Dxgi, CaptureBackend::Wgc, CaptureBackend::BitBlt};
}

// After a backend fails to start or dies, it is not tried again until this has
// passed. Long enough that a persistently-broken backend (a hybrid-GPU machine
// where duplication always dies on the second frame) is not hammered every
// recovery; short enough that a transient failure (a UAC prompt, a mode change)
// is re-tried almost immediately once it clears.
constexpr auto kBackendCooldown = std::chrono::seconds(2);

// ---------------------------------------------------------------------------
// The FrameSource the caller actually holds.
//
// It keeps the WHOLE preference order live, not just "the current one and the
// worse ones after it". Every (re)selection walks the order from the top and
// takes the first backend that is not cooling down and that starts -- so once a
// transient outage clears, capture climbs back to the BEST backend rather than
// being stranded on the GDI floor by a UAC prompt that happened to outlast the
// grace period. "Share exactly what is on screen, in any case" means the best
// reader wins whenever it can, not just at startup.
// ---------------------------------------------------------------------------
class ResilientCapture final : public FrameSource {
public:
    ResilientCapture(CaptureConfig cfg, std::vector<CaptureBackend> order)
        : cfg_(cfg), order_(std::move(order)) {}

    bool start() override {
        if (select()) return true;
        logE("capture: no backend could start for this target");
        return false;
    }

    void stop() override {
        if (active_) active_->stop();
        active_.reset();
    }

    bool retarget(const CaptureConfig& cfg) override {
        // Ask the running backend first: switching monitors inside DXGI keeps
        // the D3D device and is far cheaper than a full rebuild.
        if (active_ && active_->retarget(cfg)) {
            cfg_ = cfg;
            return true;
        }

        // It could not, so rebuild from the top of the preference list -- the
        // new target may well suit a backend the old one did not. A fresh target
        // deserves a fresh look, so the cooldowns are cleared too.
        const CaptureConfig previous = cfg_;
        stop();
        cfg_   = cfg;
        order_ = preferenceOrder(cfg_);
        cooldownUntil_.clear();
        if (select()) return true;

        logE("capture: could not switch target; restoring the previous one");
        stop();
        cfg_   = previous;
        order_ = preferenceOrder(cfg_);
        cooldownUntil_.clear();
        return select();
    }

    const Frame* capture() override {
        const auto now = Clock::now();

        if (active_) {
            if (const Frame* f = active_->capture()) {
                healthy_ = now;
                return f;
            }
            // A null is normal for a moment -- a mode change, a fullscreen
            // transition, a UAC prompt. Give the backend a grace period to
            // recover on its own before writing it off.
            if (now - healthy_ < kFailureGrace) return nullptr;

            logW("capture: the {} backend stopped producing frames",
                 backendName(active_->backend()));
            cooldownUntil_[active_->backend()] = now + kBackendCooldown;
            active_.reset();
        }

        // No active backend: either we just dropped one, or a previous
        // reselection came up empty (everything dark -- a secure desktop). Try
        // again from the top, but not more than a few times a second: during a
        // real outage every backend fails to start, and spinning on that would
        // burn the capture thread.
        if (now - lastSelect_ < std::chrono::milliseconds(200)) return nullptr;
        select();
        return nullptr;
    }

    int width()  const override { return active_ ? active_->width()  : 0; }
    int height() const override { return active_ ? active_->height() : 0; }
    int nativeWidth()  const override { return active_ ? active_->nativeWidth()  : 0; }
    int nativeHeight() const override { return active_ ? active_->nativeHeight() : 0; }

    std::string describe() const override {
        return active_ ? active_->describe() : std::string("no capture backend (recovering)");
    }
    CaptureBackend backend() const override {
        return active_ ? active_->backend() : CaptureBackend::Auto;
    }

private:
    // Walk the preference order and adopt the first backend that is eligible and
    // starts. Records the previous encode size only to note a change in the log;
    // the encoder is never rebuilt here, because the conversion pass rescales any
    // native size to the encoder's fixed one.
    bool select() {
        lastSelect_ = Clock::now();

        const CaptureBackend had = active_ ? active_->backend() : CaptureBackend::Auto;
        const int prevW = active_ ? active_->width()  : 0;
        const int prevH = active_ ? active_->height() : 0;
        active_.reset();

        for (CaptureBackend b : order_) {
            if (auto it = cooldownUntil_.find(b);
                it != cooldownUntil_.end() && Clock::now() < it->second)
                continue;

            auto source = makeBackend(b, cfg_);
            if (!source || !source->start()) {
                cooldownUntil_[b] = Clock::now() + kBackendCooldown;
                logT("capture: the {} backend did not start; cooling it down",
                     backendName(b));
                continue;
            }

            if (had != CaptureBackend::Auto && b != had)
                logW("capture: switched from {} to {}", backendName(had), backendName(b));
            else if (had == CaptureBackend::Auto)
                logI("capture: {} selected", backendName(b));

            if (prevW && (source->width() != prevW || source->height() != prevH))
                logT("capture: encode size held; the new backend's {}x{} is "
                     "rescaled during conversion", source->width(), source->height());

            active_  = std::move(source);
            healthy_ = Clock::now();
            return true;
        }
        return false;
    }

    CaptureConfig                cfg_;
    std::vector<CaptureBackend>  order_;
    std::unique_ptr<FrameSource> active_;
    std::map<CaptureBackend, Clock::time_point> cooldownUntil_;
    Clock::time_point            healthy_{};
    Clock::time_point            lastSelect_{};
};

bool frameIsAllBlack(const Frame& f) {
    // Sampled, not exhaustive: a grid every 16 pixels is enough to tell a black
    // rectangle from a desktop, and costs nothing at 4K.
    for (int y = 0; y < f.height; y += 16) {
        const auto* row = reinterpret_cast<const uint32_t*>(
            f.data + static_cast<size_t>(y) * static_cast<size_t>(f.stride));
        for (int x = 0; x < f.width; x += 16)
            if ((row[x] & 0x00FFFFFFu) != 0) return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------

std::unique_ptr<FrameSource> createFrameSource(const CaptureConfig& cfg) {
    return std::make_unique<ResilientCapture>(cfg, preferenceOrder(cfg));
}

std::vector<BackendReport> probeBackends(const CaptureConfig& cfg) {
    const CaptureBackend all[] = {CaptureBackend::Dxgi, CaptureBackend::Wgc,
                                  CaptureBackend::BitBlt};

    std::vector<BackendReport> out;
    for (CaptureBackend backend : all) {
        BackendReport r;
        r.backend = backend;

        if (backend == CaptureBackend::Dxgi && cfg.target == CaptureTarget::Window) {
            r.detail = "not applicable: duplication cannot address a window";
            out.push_back(std::move(r));
            continue;
        }

        CaptureConfig c = cfg;
        c.backend = backend;
        // Duplicate detection stays ON: how often a backend reports "nothing
        // changed" is the most interesting number here. DXGI and WGC are told by
        // the compositor and answer in microseconds; GDI has to re-read and hash
        // the whole screen to find out, which is the 16 ms.
        c.detectDuplicates = true;

        auto source = makeBackend(backend, c);
        if (!source || !source->start()) {
            r.detail = "did not start";
            out.push_back(std::move(r));
            continue;
        }

        r.started = true;
        r.width   = source->nativeWidth();
        r.height  = source->nativeHeight();

        // Two throwaway grabs first: duplication and WGC both settle on the
        // second or third frame, and timing the first one would measure setup.
        source->capture();
        source->capture();

        constexpr int kIters = 10;
        const auto t0 = std::chrono::steady_clock::now();
        const Frame* last = nullptr;
        int ok = 0, fresh = 0;
        for (int i = 0; i < kIters; ++i) {
            const Frame* f = source->capture();
            if (!f) continue;
            ++ok;
            last = f;
            if (!f->duplicate) ++fresh;
        }
        r.msPerFrame =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                .count() / kIters;

        if (last && last->data) {
            r.gotFrame = true;
            r.allBlack = frameIsAllBlack(*last);
            // The black check is the point of the whole command: a backend can
            // start, run fast and still hand back nothing but a black rectangle,
            // which is exactly what a protected window looks like from here.
            r.detail = r.allBlack
                           ? "starts, but every frame is entirely black"
                           : soi::format("working ({} of {} grabs had new content)",
                                         fresh, kIters);
        } else {
            r.detail = soi::format("started but returned no frame ({}/{} grabs)", ok, kIters);
        }

        source->stop();
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace soi
