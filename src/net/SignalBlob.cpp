#include "net/SignalBlob.h"
#include "util/Log.h"

#include <miniz.h>

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <cstring>
#include <vector>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace soi {
namespace {

constexpr char     kMagic[4]      = {'S', 'O', 'I', '1'};
constexpr uint8_t  kFlagEncrypted = 0x01;
constexpr size_t   kSaltLen       = 16;
constexpr size_t   kIvLen         = 12;
constexpr size_t   kTagLen        = 16;
constexpr size_t   kKeyLen        = 32;
constexpr ULONGLONG kPbkdf2Iters  = 200000;

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
// Crypto via Windows CNG (bcrypt).
//
// This is deliberately not OpenSSL: the sender is Windows-only anyway, and CNG
// removes a heavyweight external dependency from the signalling path. The wire
// format is unchanged, so it still interoperates byte-for-byte with the
// viewer's WebCrypto implementation.
// ---------------------------------------------------------------------------

// RAII for a CNG algorithm provider.
class AlgHandle {
public:
    AlgHandle(LPCWSTR algId, ULONG flags) {
        if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&h_, algId, nullptr, flags)))
            h_ = nullptr;
    }
    ~AlgHandle() { if (h_) BCryptCloseAlgorithmProvider(h_, 0); }
    AlgHandle(const AlgHandle&) = delete;
    AlgHandle& operator=(const AlgHandle&) = delete;

    BCRYPT_ALG_HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    BCRYPT_ALG_HANDLE h_ = nullptr;
};

class KeyHandle {
public:
    ~KeyHandle() { if (h_) BCryptDestroyKey(h_); }
    KeyHandle() = default;
    KeyHandle(const KeyHandle&) = delete;
    KeyHandle& operator=(const KeyHandle&) = delete;

    BCRYPT_KEY_HANDLE* put() { return &h_; }
    BCRYPT_KEY_HANDLE  get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    BCRYPT_KEY_HANDLE h_ = nullptr;
};

bool randomBytes(uint8_t* out, size_t len) {
    return NT_SUCCESS(BCryptGenRandom(nullptr, out, static_cast<ULONG>(len),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool deriveKey(std::string_view passphrase, const uint8_t* salt,
               std::array<uint8_t, kKeyLen>& key) {
    AlgHandle alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!alg) return false;

    return NT_SUCCESS(BCryptDeriveKeyPBKDF2(
        alg.get(),
        reinterpret_cast<PUCHAR>(const_cast<char*>(passphrase.data())),
        static_cast<ULONG>(passphrase.size()),
        const_cast<PUCHAR>(salt), static_cast<ULONG>(kSaltLen),
        kPbkdf2Iters, key.data(), static_cast<ULONG>(kKeyLen), 0));
}

// Opens AES in GCM chaining mode and imports the derived key.
bool makeGcmKey(const std::array<uint8_t, kKeyLen>& key, AlgHandle& alg,
                KeyHandle& outKey) {
    if (!alg) return false;

    if (!NT_SUCCESS(BCryptSetProperty(
            alg.get(), BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
        return false;

    return NT_SUCCESS(BCryptGenerateSymmetricKey(
        alg.get(), outKey.put(), nullptr, 0,
        const_cast<PUCHAR>(key.data()), static_cast<ULONG>(kKeyLen), 0));
}

bool gcmEncrypt(const std::array<uint8_t, kKeyLen>& key, const uint8_t* iv,
                const uint8_t* aad, size_t aadLen,
                const std::vector<uint8_t>& plain,
                std::vector<uint8_t>& cipher, uint8_t tag[kTagLen]) {
    AlgHandle alg(BCRYPT_AES_ALGORITHM, 0);
    KeyHandle hKey;
    if (!makeGcmKey(key, alg, hKey)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(iv);
    info.cbNonce    = static_cast<ULONG>(kIvLen);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag      = tag;
    info.cbTag      = static_cast<ULONG>(kTagLen);

    cipher.resize(plain.size());
    ULONG written = 0;

    // In authenticated modes the nonce travels in the mode info, so pbIV is null.
    const NTSTATUS st = BCryptEncrypt(
        hKey.get(), const_cast<PUCHAR>(plain.data()), static_cast<ULONG>(plain.size()),
        &info, nullptr, 0,
        cipher.empty() ? nullptr : cipher.data(), static_cast<ULONG>(cipher.size()),
        &written, 0);

    if (!NT_SUCCESS(st)) return false;
    cipher.resize(written);
    return true;
}

bool gcmDecrypt(const std::array<uint8_t, kKeyLen>& key, const uint8_t* iv,
                const uint8_t* aad, size_t aadLen,
                const uint8_t* cipher, size_t cipherLen,
                const uint8_t* tag, std::vector<uint8_t>& plain) {
    AlgHandle alg(BCRYPT_AES_ALGORITHM, 0);
    KeyHandle hKey;
    if (!makeGcmKey(key, alg, hKey)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(iv);
    info.cbNonce    = static_cast<ULONG>(kIvLen);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag      = const_cast<PUCHAR>(tag);
    info.cbTag      = static_cast<ULONG>(kTagLen);

    plain.resize(cipherLen);
    ULONG written = 0;

    // Returns STATUS_AUTH_TAG_MISMATCH when the tag does not verify.
    const NTSTATUS st = BCryptDecrypt(
        hKey.get(), const_cast<PUCHAR>(cipher), static_cast<ULONG>(cipherLen),
        &info, nullptr, 0,
        plain.empty() ? nullptr : plain.data(), static_cast<ULONG>(plain.size()),
        &written, 0);

    if (!NT_SUCCESS(st)) return false;
    plain.resize(written);
    return true;
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
        logE("signal blob: BCryptGenRandom failed; refusing to emit a weakly-keyed blob");
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
