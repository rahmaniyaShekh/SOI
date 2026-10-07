#include "util/Crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <vector>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace soi {
namespace {

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

private:
    BCRYPT_KEY_HANDLE h_ = nullptr;
};

// Opens AES in GCM chaining mode and imports the key.
bool makeGcmKey(const uint8_t key[32], AlgHandle& alg, KeyHandle& outKey) {
    if (!alg) return false;
    if (!NT_SUCCESS(BCryptSetProperty(
            alg.get(), BCRYPT_CHAINING_MODE,
            reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_GCM)),
            sizeof(BCRYPT_CHAIN_MODE_GCM), 0)))
        return false;
    return NT_SUCCESS(BCryptGenerateSymmetricKey(alg.get(), outKey.put(), nullptr, 0,
                                                 const_cast<PUCHAR>(key), 32, 0));
}

} // namespace

bool randomBytes(void* out, size_t len) {
    return NT_SUCCESS(BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(len),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

bool sha256(const void* data, size_t size, uint8_t digest[32]) {
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    // BCryptHashData takes a ULONG; feed large inputs in pieces.
    const auto* p = static_cast<const unsigned char*>(data);
    size_t left = size;
    while (ok && left > 0) {
        const ULONG chunk = static_cast<ULONG>(left > (1u << 30) ? (1u << 30) : left);
        ok = BCryptHashData(hash, const_cast<PUCHAR>(p), chunk, 0) == 0;
        p += chunk;
        left -= chunk;
    }
    ok = ok && BCryptFinishHash(hash, digest, 32, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

bool pbkdf2HmacSha256(std::string_view passphrase, const uint8_t* salt, size_t saltLen,
                      unsigned iterations, uint8_t* key, size_t keyLen) {
    AlgHandle alg(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!alg) return false;
    return NT_SUCCESS(BCryptDeriveKeyPBKDF2(
        alg.get(), reinterpret_cast<PUCHAR>(const_cast<char*>(passphrase.data())),
        static_cast<ULONG>(passphrase.size()), const_cast<PUCHAR>(salt),
        static_cast<ULONG>(saltLen), iterations, key, static_cast<ULONG>(keyLen), 0));
}

bool aes256GcmEncrypt(const uint8_t key[32], const uint8_t* iv, size_t ivLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* plain, size_t plainLen,
                      uint8_t* cipher, uint8_t tag[16]) {
    AlgHandle alg(BCRYPT_AES_ALGORITHM, 0);
    KeyHandle hKey;
    if (!makeGcmKey(key, alg, hKey)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(iv);
    info.cbNonce    = static_cast<ULONG>(ivLen);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag      = tag;
    info.cbTag      = 16;

    ULONG written = 0;
    // In authenticated modes the nonce travels in the mode info, so pbIV is null.
    const NTSTATUS st = BCryptEncrypt(hKey.get(), const_cast<PUCHAR>(plain),
                                      static_cast<ULONG>(plainLen), &info, nullptr, 0,
                                      plainLen ? cipher : nullptr,
                                      static_cast<ULONG>(plainLen), &written, 0);
    return NT_SUCCESS(st) && written == plainLen;
}

bool aes256GcmDecrypt(const uint8_t key[32], const uint8_t* iv, size_t ivLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* cipher, size_t cipherLen,
                      const uint8_t tag[16], uint8_t* plain) {
    AlgHandle alg(BCRYPT_AES_ALGORITHM, 0);
    KeyHandle hKey;
    if (!makeGcmKey(key, alg, hKey)) return false;

    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce    = const_cast<PUCHAR>(iv);
    info.cbNonce    = static_cast<ULONG>(ivLen);
    info.pbAuthData = const_cast<PUCHAR>(aad);
    info.cbAuthData = static_cast<ULONG>(aadLen);
    info.pbTag      = const_cast<PUCHAR>(tag);
    info.cbTag      = 16;

    ULONG written = 0;
    // Returns STATUS_AUTH_TAG_MISMATCH when the tag does not verify.
    const NTSTATUS st = BCryptDecrypt(hKey.get(), const_cast<PUCHAR>(cipher),
                                      static_cast<ULONG>(cipherLen), &info, nullptr, 0,
                                      cipherLen ? plain : nullptr,
                                      static_cast<ULONG>(cipherLen), &written, 0);
    return NT_SUCCESS(st) && written == cipherLen;
}

} // namespace soi
