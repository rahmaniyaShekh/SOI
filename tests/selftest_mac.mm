#include "selftest_mac.h"

#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>

#include <atomic>

namespace soi {
namespace {

// Splits an Annex-B buffer into NAL units (without start codes).
std::vector<std::pair<const uint8_t*, size_t>> splitNals(const std::vector<uint8_t>& au) {
    std::vector<std::pair<const uint8_t*, size_t>> out;
    const uint8_t* p = au.data();
    const size_t n = au.size();
    size_t i = 0, start = std::string::npos;
    while (i + 3 <= n) {
        size_t sc = 0;
        if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) sc = 3;
        else if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1) sc = 4;
        if (sc) {
            if (start != std::string::npos) out.emplace_back(p + start, i - start);
            i += sc;
            start = i;
        } else {
            ++i;
        }
    }
    if (start != std::string::npos && start < n) out.emplace_back(p + start, n - start);
    return out;
}

struct DecodeState {
    std::atomic<int> decoded{0};
    std::atomic<int> errors{0};
    int width = 0, height = 0;
};

void onDecoded(void* refcon, void*, OSStatus status, VTDecodeInfoFlags, CVImageBufferRef image,
               CMTime, CMTime) {
    auto* st = static_cast<DecodeState*>(refcon);
    if (status != noErr || !image) { ++st->errors; return; }
    st->width  = static_cast<int>(CVPixelBufferGetWidth(image));
    st->height = static_cast<int>(CVPixelBufferGetHeight(image));
    ++st->decoded;
}

} // namespace

bool macFindSpsProfile(const std::vector<uint8_t>& unit, int& profileIdc) {
    for (const auto& [nal, len] : splitNals(unit))
        if (len >= 2 && (nal[0] & 0x1F) == 7) { profileIdc = nal[1]; return true; }
    return false;
}

bool macDecodeAnnexB(const std::vector<std::vector<uint8_t>>& units, int& decoded, int& width,
                     int& height, std::string& detail) {
    decoded = width = height = 0;
    std::vector<uint8_t> sps, pps;
    for (const auto& u : units) {
        for (const auto& [nal, len] : splitNals(u)) {
            const int type = nal[0] & 0x1F;
            if (type == 7 && sps.empty()) sps.assign(nal, nal + len);
            if (type == 8 && pps.empty()) pps.assign(nal, nal + len);
        }
        if (!sps.empty() && !pps.empty()) break;
    }
    if (sps.empty() || pps.empty()) { detail = "no SPS/PPS in the stream"; return false; }

    const uint8_t* sets[2] = {sps.data(), pps.data()};
    const size_t   sizes[2] = {sps.size(), pps.size()};
    CMVideoFormatDescriptionRef format = nullptr;
    OSStatus st = CMVideoFormatDescriptionCreateFromH264ParameterSets(kCFAllocatorDefault, 2, sets,
                                                                      sizes, 4, &format);
    if (st != noErr) { detail = "SPS/PPS rejected: " + std::to_string(st); return false; }

    DecodeState state;
    VTDecompressionOutputCallbackRecord cb{onDecoded, &state};
    VTDecompressionSessionRef session = nullptr;
    st = VTDecompressionSessionCreate(kCFAllocatorDefault, format, nullptr, nullptr, &cb, &session);
    if (st != noErr) {
        CFRelease(format);
        detail = "VTDecompressionSessionCreate: " + std::to_string(st);
        return false;
    }

    for (const auto& u : units) {
        // AVCC: each slice NAL with a 4-byte big-endian length.
        std::vector<uint8_t> avcc;
        for (const auto& [nal, len] : splitNals(u)) {
            const int type = nal[0] & 0x1F;
            if (type == 7 || type == 8 || type == 9) continue;   // parameter sets, AUD
            const uint32_t l = static_cast<uint32_t>(len);
            const uint8_t hdr[4] = {uint8_t(l >> 24), uint8_t(l >> 16), uint8_t(l >> 8), uint8_t(l)};
            avcc.insert(avcc.end(), hdr, hdr + 4);
            avcc.insert(avcc.end(), nal, nal + len);
        }
        if (avcc.empty()) continue;

        CMBlockBufferRef block = nullptr;
        st = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, nullptr, avcc.size(),
                                                kCFAllocatorDefault, nullptr, 0, avcc.size(), 0,
                                                &block);
        if (st != noErr) continue;
        CMBlockBufferReplaceDataBytes(avcc.data(), block, 0, avcc.size());
        CMSampleBufferRef sample = nullptr;
        const size_t sampleSize = avcc.size();
        st = CMSampleBufferCreateReady(kCFAllocatorDefault, block, format, 1, 0, nullptr, 1,
                                       &sampleSize, &sample);
        CFRelease(block);
        if (st != noErr) continue;
        VTDecompressionSessionDecodeFrame(session, sample, 0, nullptr, nullptr);
        CFRelease(sample);
    }
    VTDecompressionSessionWaitForAsynchronousFrames(session);
    VTDecompressionSessionInvalidate(session);
    CFRelease(session);
    CFRelease(format);

    decoded = state.decoded.load();
    width   = state.width;
    height  = state.height;
    if (state.errors.load()) detail = std::to_string(state.errors.load()) + " decode error(s)";
    return decoded > 0;
}

void* macMakeTestPixelBuffer(int width, int height, int n) {
    NSDictionary* attrs = @{(id)kCVPixelBufferIOSurfacePropertiesKey : @{}};
    CVPixelBufferRef buffer = nullptr;
    if (CVPixelBufferCreate(kCFAllocatorDefault, static_cast<size_t>(width),
                            static_cast<size_t>(height), kCVPixelFormatType_32BGRA,
                            (__bridge CFDictionaryRef)attrs, &buffer) != kCVReturnSuccess)
        return nullptr;
    CVPixelBufferLockBaseAddress(buffer, 0);
    auto* base = static_cast<uint8_t*>(CVPixelBufferGetBaseAddress(buffer));
    const size_t stride = CVPixelBufferGetBytesPerRow(buffer);
    for (int y = 0; y < height; ++y) {
        uint8_t* row = base + y * stride;
        for (int x = 0; x < width; ++x) {
            row[x * 4 + 0] = static_cast<uint8_t>((x + n * 23) & 0xFF);
            row[x * 4 + 1] = static_cast<uint8_t>((y + n * 11) & 0xFF);
            row[x * 4 + 2] = static_cast<uint8_t>((x ^ y) + n * 17);
            row[x * 4 + 3] = 255;
        }
    }
    CVPixelBufferUnlockBaseAddress(buffer, 0);
    return buffer;
}

void macReleasePixelBuffer(void* buffer) {
    if (buffer) CVPixelBufferRelease(static_cast<CVPixelBufferRef>(buffer));
}

} // namespace soi
