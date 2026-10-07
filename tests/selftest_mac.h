#pragma once
// macOS-only helpers for the self-test (selftest_mac.mm): things that need
// Core Media / VideoToolbox / Core Video directly rather than through soi-core.
#include <cstdint>
#include <string>
#include <vector>

namespace soi {

// Decodes Annex-B access units (as H264Encoder emits them) with VideoToolbox.
// `decoded` counts frames that came out; width/height are the last frame's.
bool macDecodeAnnexB(const std::vector<std::vector<uint8_t>>& units, int& decoded, int& width,
                     int& height, std::string& detail);

// profile_idc from the first SPS in an access unit. False if there is none.
bool macFindSpsProfile(const std::vector<uint8_t>& unit, int& profileIdc);

// A BGRA, IOSurface-backed CVPixelBufferRef with a moving pattern, as
// ScreenCaptureKit would deliver one. Release with macReleasePixelBuffer.
void* macMakeTestPixelBuffer(int width, int height, int frameIndex);
void  macReleasePixelBuffer(void* pixelBuffer);

} // namespace soi
