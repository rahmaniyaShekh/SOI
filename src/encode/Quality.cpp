#include "encode/Quality.h"

#include <algorithm>
#include <cctype>

namespace soi {
namespace {

// Bitrates are for SCREEN content, not camera video, and are deliberately
// higher per pixel than a video-call ladder would use. Screens are full of hard
// edges and small text, which is exactly what a DCT spends bits on; the saving
// comes from most frames being identical and skipped entirely, not from
// starving the frames that do get sent.
const std::vector<QualityLevel>& table() {
    static const std::vector<QualityLevel> levels = {
        {"360p",   360, 30,  700},
        {"480p",   480, 30, 1200},
        {"720p",   720, 30, 2500},
        {"1080p", 1080, 30, 4500},
        {"source",   0, 30, 6000},   // whatever the display actually is
    };
    return levels;
}

char lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

int roundEven(int v) { return v & ~1; }

} // namespace

const std::vector<QualityLevel>& qualityLevels() { return table(); }

const QualityLevel* findQualityLevel(std::string_view name) {
    for (const auto& level : table()) {
        if (level.name.size() != name.size()) continue;
        bool same = true;
        for (size_t i = 0; i < name.size(); ++i)
            if (lower(level.name[i]) != lower(name[i])) { same = false; break; }
        if (same) return &level;
    }
    return nullptr;
}

const QualityLevel& defaultQualityLevel() {
    // 720p. High enough that text is comfortable to read, low enough that the
    // per-frame budget is reachable on an ordinary home upload.
    const QualityLevel* level = findQualityLevel("720p");
    return level ? *level : table().front();
}

void encodeSizeFor(const QualityLevel& level, int srcWidth, int srcHeight,
                   int& outWidth, int& outHeight) {
    outWidth  = roundEven(std::max(2, srcWidth));
    outHeight = roundEven(std::max(2, srcHeight));
    if (srcWidth <= 0 || srcHeight <= 0) return;

    // height == 0 means "source native", and any level taller than the source
    // is clamped to it: upscaling here would spend bitrate inventing detail the
    // capture never had.
    if (level.height <= 0 || level.height >= srcHeight) return;

    const int height = roundEven(level.height);
    // Scale width by the same factor and round to even, so the aspect ratio is
    // preserved to within half a pixel on a 16:10 or 16:9 display.
    const int width = roundEven(
        static_cast<int>((static_cast<long long>(srcWidth) * height + srcHeight / 2) / srcHeight));

    outWidth  = std::max(2, width);
    outHeight = std::max(2, height);
}

int bitsPerFrame(const QualityLevel& level) {
    const int fps = std::max(1, level.fps);
    return static_cast<int>((static_cast<long long>(level.bitrateKbps) * 1000) / fps);
}

int frameRateForBandwidth(const QualityLevel& level, int availableKbps, int minFps) {
    const int nominal = std::max(1, level.fps);
    minFps = std::clamp(minFps, 1, nominal);
    if (availableKbps <= 0) return minFps;

    // fps = available / (bitrate per frame), which is the same as scaling the
    // nominal rate by the fraction of the design bitrate we actually have.
    const long long scaled =
        (static_cast<long long>(availableKbps) * nominal) / std::max(1, level.bitrateKbps);

    return static_cast<int>(std::clamp<long long>(scaled, minFps, nominal));
}

int bitrateForFrameRate(const QualityLevel& level, int fps) {
    const int nominal = std::max(1, level.fps);
    fps = std::clamp(fps, 1, nominal);
    // bitsPerFrame(level) * fps, expressed in kbps and rounded to NEAREST.
    //
    // Truncating here would shave a little off every frame's budget at every
    // rate, always in the same direction -- a small systematic bias against the
    // one quantity this whole design exists to protect. Rounding makes the error
    // symmetric and bounded by the 1 kbps granularity of the setting itself.
    const long long scaled =
        (static_cast<long long>(level.bitrateKbps) * fps + nominal / 2) / nominal;
    return static_cast<int>(std::max<long long>(1, scaled));
}

int maxFrameMacroblocksForLevel(int levelIdc) {
    // Table A-1 of the H.264 spec, MaxFS column. Only the levels a screen share
    // could plausibly negotiate are listed.
    switch (levelIdc) {
        case 10: return 99;     case 11: return 396;    case 12: return 396;
        case 13: return 396;    case 20: return 396;    case 21: return 792;
        case 22: return 1620;   case 30: return 1620;   case 31: return 3600;
        case 32: return 5120;   case 40: return 8192;   case 41: return 8192;
        case 42: return 8704;   case 50: return 22080;  case 51: return 36864;
        case 52: return 36864;
        default: return 0;      // unknown: do not constrain on a guess
    }
}

int h264LevelFromSdp(const std::string& sdp) {
    const std::string key = "profile-level-id=";
    const size_t at = sdp.find(key);
    if (at == std::string::npos) return 0;

    const size_t start = at + key.size();
    if (start + 6 > sdp.size()) return 0;

    // profile_idc(2) profile_iop(2) level_idc(2), all hex. ALL six digits are
    // validated, not just the two that are wanted: a field that is malformed
    // anywhere is a field to distrust entirely, and this one decides whether
    // the far end can decode what gets sent to it.
    auto hex = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };

    int digits[6];
    for (int i = 0; i < 6; ++i) {
        digits[i] = hex(sdp[start + static_cast<size_t>(i)]);
        if (digits[i] < 0) return 0;
    }
    return digits[4] * 16 + digits[5];
}

void clampToMacroblocks(int maxMacroblocks, int& width, int& height) {
    if (maxMacroblocks <= 0 || width <= 0 || height <= 0) return;

    auto macroblocks = [](int w, int h) {
        return static_cast<long long>((w + 15) / 16) * ((h + 15) / 16);
    };
    if (macroblocks(width, height) <= maxMacroblocks) return;

    // Walk the height down in 16-pixel steps, keeping the aspect ratio, until it
    // fits. Stepping by a macroblock row avoids repeatedly landing just over the
    // limit through rounding.
    const double aspect = static_cast<double>(width) / height;
    int h = roundEven((height / 16) * 16);
    while (h >= 16) {
        const int w = roundEven(static_cast<int>(aspect * h + 0.5));
        if (w >= 2 && macroblocks(w, h) <= maxMacroblocks) {
            width = w;
            height = h;
            return;
        }
        h -= 16;
    }
    width = 16;
    height = 16;
}

// H.264 level selection. Getting this wrong is not cosmetic: a decoder that
// trusts an under-stated level may refuse the stream or allocate too few DPB
// slots. Table is (levelIdc, maxMacroblocksPerSecond, maxFrameMacroblocks).
int h264LevelForResolution(int width, int height, int fps) {
    struct Level { int idc; long long mbps; long long frameMbs; };
    static constexpr Level kLevels[] = {
        {30, 40500,  1620},  {31, 108000, 3600},  {32, 216000, 5120},
        {40, 245760, 8192},  {41, 245760, 8192},  {42, 522240, 8704},
        {50, 589824, 22080}, {51, 983040, 36864}, {52, 2073600, 36864},
    };

    const long long frameMbs =
        static_cast<long long>((width + 15) / 16) * ((height + 15) / 16);
    const long long mbps = frameMbs * (fps > 0 ? fps : 30);

    for (const auto& l : kLevels)
        if (frameMbs <= l.frameMbs && mbps <= l.mbps) return l.idc;
    return 52;
}

} // namespace soi
