#pragma once
//
// Quality levels, and the rule for what gives way when the link is too slow.
//
// The usual WebRTC answer is to drop the bitrate and let the encoder raise QP.
// For camera video that is right -- a slightly soft face is still a face. For a
// SCREEN it is the wrong trade entirely: raising QP on 9-pixel-tall text turns
// it into grey mush, and unreadable text at 30fps is worth less than perfectly
// sharp text at 4fps. Someone reading your terminal needs to READ it.
//
// So the adaptation rule here is inverted from the usual one:
//
//     resolution  -- fixed, chosen by the viewer
//     bits/frame  -- fixed, the level's design point
//     frame rate  -- whatever the available bandwidth leaves room for
//
// Concretely: a level defines a bitrate at a nominal frame rate, and their ratio
// is a per-frame bit budget. When only a fraction of that bitrate is available,
// we send that same fraction of the frames, each still carrying its full budget.
// Sharpness is preserved exactly; motion gets choppy. That is the correct
// failure mode for this application.
//
// Screen content makes this cheap: most frames are pixel-identical to the last
// one and are skipped anyway, so a lower frame-rate ceiling usually costs
// nothing at all on a static screen and only bites during scrolling or video.
//
#include <string>
#include <string_view>
#include <vector>

namespace soi {

struct QualityLevel {
    std::string name;          // "720p", as shown to the viewer
    int         height = 720;  // target encode height; 0 means "source native"
    int         fps    = 30;   // nominal frame rate, the design point
    int         bitrateKbps = 1800;   // bitrate AT that frame rate
};

// Ordered smallest first. This list is the protocol: the sender advertises it to
// the viewer, and the viewer picks by name.
const std::vector<QualityLevel>& qualityLevels();

// Case-insensitive lookup. Returns nullptr when the name is not a level, so a
// malformed request from the network is simply ignored.
const QualityLevel* findQualityLevel(std::string_view name);

// The level a share starts at when nothing else is specified.
const QualityLevel& defaultQualityLevel();

// Encode size for a level against a given source size.
//
// Preserves the source aspect ratio, never upscales (encoding 720p from a 480p
// window would cost bitrate to invent detail that is not there), and returns
// even dimensions because NV12 subsamples chroma 2x2.
void encodeSizeFor(const QualityLevel& level, int srcWidth, int srcHeight,
                   int& outWidth, int& outHeight);

// Bits available to each frame at the level's design point. This is the
// quantity held constant as bandwidth moves -- it is what "quality" means here.
int bitsPerFrame(const QualityLevel& level);

// The frame rate that fits `availableKbps` while still giving every frame the
// level's full bit budget.
//
// Below `minFps` there is nothing left to give up: the link genuinely cannot
// carry this resolution, and the caller has to let per-frame quality fall. The
// floor is deliberately low -- a few frames a second is still perfectly usable
// for reading a screen, and it keeps text sharp on links where the conventional
// strategy would have produced a smooth blur.
int frameRateForBandwidth(const QualityLevel& level, int availableKbps, int minFps);

// The bitrate to hand the encoder once the frame rate has been chosen, so that
// bits-per-frame lands back on the level's design point.
int bitrateForFrameRate(const QualityLevel& level, int fps);

// --- what the peer said it can actually decode ------------------------------
//
// An SDP answer carries profile-level-id, and its level byte bounds the frame
// size the far end will accept. Ignoring it is the difference between a quality
// setting that works and one that silently does nothing: browsers answer with
// level 3.1 by default, whose ceiling is 3600 macroblocks -- exactly 1280x720 --
// so every higher level was being sent outside the negotiated envelope, where a
// soft decoder freezes and a hardware one simply refuses.

// Largest frame, in 16x16 macroblocks, that an H.264 level_idc permits.
// Returns 0 for a level this build does not know.
int maxFrameMacroblocksForLevel(int levelIdc);

// Pulls level_idc out of the first profile-level-id in an SDP. Returns 0 when
// there is none, which callers should treat as "no constraint stated".
int h264LevelFromSdp(const std::string& sdp);

// Shrinks an encode size, preserving aspect ratio, until it fits within
// `maxMacroblocks`. A no-op when it already fits or the limit is unknown.
void clampToMacroblocks(int maxMacroblocks, int& width, int& height);

} // namespace soi
