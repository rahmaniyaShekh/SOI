#include "net/SignalBlob.h"
#include "util/Crypto.h"
#include "util/Log.h"

#include <miniz.h>

#include <array>
#include <cstring>
#include <vector>

namespace soi {
namespace {

constexpr char     kMagic[4]      = {'S', 'O', 'I', '1'};
constexpr uint8_t  kFlagEncrypted = 0x01;
constexpr size_t   kSaltLen       = 16;
constexpr size_t   kIvLen         = 12;
constexpr size_t   kTagLen        = 16;
constexpr size_t   kKeyLen        = 32;
constexpr unsigned kPbkdf2Iters   = 200000;

constexpr char kB64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int b64Value(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-' || c == '+') return 62;
    if (c == '_' || c == '/') return 63;
    return -1;
}

// Raw DEFLATE (no zlib header), matching the browser's
// CompressionStream('deflate-raw'). The negative window size is miniz's way of
// requesting the headerless variant.
bool deflateRaw(const void* src, size_t srcLen, std::vector<uint8_t>& out) {
    const mz_uint flags =
        tdefl_create_comp_flags_from_zip_params(9, -15, MZ_DEFAULT_STRATEGY);

    size_t outLen = 0;
    void*  buf = tdefl_compress_mem_to_heap(src, srcLen, &outLen, flags);
    if (!buf) return false;

    out.assign(static_cast<uint8_t*>(buf), static_cast<uint8_t*>(buf) + outLen);
    mz_free(buf);
    return true;
}

bool inflateRaw(const void* src, size_t srcLen, std::string& out) {
    size_t outLen = 0;
    void*  buf = tinfl_decompress_mem_to_heap(src, srcLen, &outLen, 0);  // 0 = raw
    if (!buf) return false;

    out.assign(static_cast<char*>(buf), outLen);
    mz_free(buf);
    return true;
}

// ---------------------------------------------------------------------------
// AES-256-GCM over PBKDF2-HMAC-SHA256, through util/Crypto.h (CNG on Windows,
// OpenSSL on macOS). The wire format is the same on both, so it interoperates
// byte-for-byte with the viewer's WebCrypto implementation.
// ---------------------------------------------------------------------------

bool deriveKey(std::string_view passphrase, const uint8_t* salt,
               std::array<uint8_t, kKeyLen>& key) {
    return pbkdf2HmacSha256(passphrase, salt, kSaltLen, kPbkdf2Iters, key.data(), kKeyLen);
}

bool gcmEncrypt(const std::array<uint8_t, kKeyLen>& key, const uint8_t* iv,
                const uint8_t* aad, size_t aadLen,
                const std::vector<uint8_t>& plain,
                std::vector<uint8_t>& cipher, uint8_t tag[kTagLen]) {
    cipher.resize(plain.size());
    return aes256GcmEncrypt(key.data(), iv, kIvLen, aad, aadLen, plain.data(), plain.size(),
                            cipher.data(), tag);
}

bool gcmDecrypt(const std::array<uint8_t, kKeyLen>& key, const uint8_t* iv,
                const uint8_t* aad, size_t aadLen,
                const uint8_t* cipher, size_t cipherLen,
                const uint8_t* tag, std::vector<uint8_t>& plain) {
    plain.resize(cipherLen);
    return aes256GcmDecrypt(key.data(), iv, kIvLen, aad, aadLen, cipher, cipherLen, tag,
                            plain.data());
}

} // namespace

std::string base64UrlEncode(const void* dataRaw, size_t len) {
    const auto* data = static_cast<const uint8_t*>(dataRaw);
    std::string out;
    out.reserve((len + 2) / 3 * 4);

    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8) | data[i + 2];
        out.push_back(kB64Chars[(v >> 18) & 63]);
        out.push_back(kB64Chars[(v >> 12) & 63]);
        out.push_back(kB64Chars[(v >> 6) & 63]);
        out.push_back(kB64Chars[v & 63]);
    }

    if (i + 1 == len) {
        const uint32_t v = static_cast<uint32_t>(data[i]) << 16;
        out.push_back(kB64Chars[(v >> 18) & 63]);
        out.push_back(kB64Chars[(v >> 12) & 63]);
    } else if (i + 2 == len) {
        const uint32_t v = (static_cast<uint32_t>(data[i]) << 16) |
                           (static_cast<uint32_t>(data[i + 1]) << 8);
        out.push_back(kB64Chars[(v >> 18) & 63]);
        out.push_back(kB64Chars[(v >> 12) & 63]);
        out.push_back(kB64Chars[(v >> 6) & 63]);
    }
    return out;   // unpadded, URL-safe
}

bool base64UrlDecode(std::string_view text, std::string& out) {
    out.clear();
    out.reserve(text.size() * 3 / 4 + 3);

    uint32_t acc = 0;
    int      bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
        const int v = b64Value(c);
        if (v < 0) return false;

        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

std::string encodeSignalBlob(std::string_view sdp, std::string_view passphrase) {
    std::vector<uint8_t> compressed;
    if (!deflateRaw(sdp.data(), sdp.size(), compressed)) {
        logE("signal blob: deflate failed");
        return {};
    }

    const bool encrypt = !passphrase.empty();

    std::vector<uint8_t> payload;
    payload.reserve(5 + kSaltLen + kIvLen + compressed.size() + kTagLen);
    payload.insert(payload.end(), kMagic, kMagic + 4);
    payload.push_back(encrypt ? kFlagEncrypted : 0);

    if (!encrypt) {
        payload.insert(payload.end(), compressed.begin(), compressed.end());
        return std::string(kBlobPrefix) +
               base64UrlEncode(payload.data(), payload.size());
    }

    uint8_t salt[kSaltLen], iv[kIvLen];
    if (!randomBytes(salt, kSaltLen) || !randomBytes(iv, kIvLen)) {
        logE("signal blob: the system random generator failed; refusing to emit a weakly-keyed blob");
        return {};
    }

    payload.insert(payload.end(), salt, salt + kSaltLen);
    payload.insert(payload.end(), iv, iv + kIvLen);

    std::array<uint8_t, kKeyLen> key{};
    if (!deriveKey(passphrase, salt, key)) {
        logE("signal blob: PBKDF2 failed");
        return {};
    }

    std::vector<uint8_t> cipher;
    uint8_t              tag[kTagLen];
    // AAD is the whole prefix, so flags/salt/iv are all authenticated.
    if (!gcmEncrypt(key, iv, payload.data(), payload.size(), compressed, cipher, tag)) {
        logE("signal blob: AES-256-GCM encryption failed");
        return {};
    }

    payload.insert(payload.end(), cipher.begin(), cipher.end());
    payload.insert(payload.end(), tag, tag + kTagLen);

    return std::string(kBlobPrefix) + base64UrlEncode(payload.data(), payload.size());
}

bool decodeSignalBlob(std::string_view blob, std::string_view passphrase,
                      std::string& sdpOut, std::string& error) {
    error.clear();

    // Tolerate surrounding whitespace and a pasted URL fragment.
    while (!blob.empty() && (blob.front() == ' ' || blob.front() == '\n' ||
                            blob.front() == '\r' || blob.front() == '\t'))
        blob.remove_prefix(1);
    while (!blob.empty() && (blob.back() == ' ' || blob.back() == '\n' ||
                            blob.back() == '\r' || blob.back() == '\t'))
        blob.remove_suffix(1);

    if (const size_t hash = blob.find('#'); hash != std::string_view::npos)
        blob.remove_prefix(hash + 1);

    if (blob.size() < kBlobPrefix.size() || blob.substr(0, kBlobPrefix.size()) != kBlobPrefix) {
        error = "not a SOI signalling blob (expected it to start with \"SOI1:\")";
        return false;
    }
    blob.remove_prefix(kBlobPrefix.size());

    std::string raw;
    if (!base64UrlDecode(blob, raw)) {
        error = "blob contains characters that are not valid base64url";
        return false;
    }

    const auto*  bytes = reinterpret_cast<const uint8_t*>(raw.data());
    const size_t len   = raw.size();

    if (len < 5 || std::memcmp(bytes, kMagic, 4) != 0) {
        error = "blob header is missing or corrupt";
        return false;
    }

    const uint8_t flags     = bytes[4];
    const bool    encrypted = (flags & kFlagEncrypted) != 0;

    if (!encrypted) {
        if (!passphrase.empty())
            logW("a passphrase was supplied but this blob is not encrypted");
        if (!inflateRaw(bytes + 5, len - 5, sdpOut)) {
            error = "decompression failed; the blob is truncated or corrupt";
            return false;
        }
        return true;
    }

    if (passphrase.empty()) {
        error = "this blob is encrypted; re-run with --pass <passphrase>";
        return false;
    }

    const size_t headerLen = 5 + kSaltLen + kIvLen;
    if (len < headerLen + kTagLen) {
        error = "encrypted blob is too short to contain a salt, IV and tag";
        return false;
    }

    const uint8_t* salt   = bytes + 5;
    const uint8_t* iv     = salt + kSaltLen;
    const size_t   cipherLen = len - headerLen - kTagLen;
    const uint8_t* cipher = bytes + headerLen;
    const uint8_t* tag    = cipher + cipherLen;

    std::array<uint8_t, kKeyLen> key{};
    if (!deriveKey(passphrase, salt, key)) {
        error = "key derivation failed";
        return false;
    }

    std::vector<uint8_t> plain;
    if (!gcmDecrypt(key, iv, bytes, headerLen, cipher, cipherLen, tag, plain)) {
        // GCM cannot tell a wrong key from a tampered blob, so do not claim to.
        error = "authentication failed: wrong passphrase, or the blob was altered "
                "in transit";
        return false;
    }

    if (!inflateRaw(plain.data(), plain.size(), sdpOut)) {
        error = "decompression failed after decryption";
        return false;
    }
    return true;
}

} // namespace soi
