#pragma once
//
// The handful of primitives the signalling, rendezvous, control and update
// code need, behind one interface.
//
//   Windows  CNG (bcrypt), which ships with the OS -- see Crypto_win.cpp.
//   macOS    OpenSSL, which is already statically linked for libdatachannel's
//            DTLS -- see Crypto_openssl.cpp.
//
// Both produce byte-identical output, so a blob sealed on one platform opens
// on the other and in the viewer's WebCrypto implementation.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace soi {

// From the system CSPRNG. False if it is unavailable, in which case the caller
// must refuse to continue rather than fall back to anything predictable.
bool randomBytes(void* out, size_t len);

// 32 bytes into `digest`.
bool sha256(const void* data, size_t len, uint8_t digest[32]);

bool pbkdf2HmacSha256(std::string_view passphrase, const uint8_t* salt, size_t saltLen,
                      unsigned iterations, uint8_t* key, size_t keyLen);

// AES-256-GCM with a 16-byte tag. `cipher` receives exactly plainLen bytes.
bool aes256GcmEncrypt(const uint8_t key[32], const uint8_t* iv, size_t ivLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* plain, size_t plainLen,
                      uint8_t* cipher, uint8_t tag[16]);

// False when the tag does not verify (wrong key or tampered data).
bool aes256GcmDecrypt(const uint8_t key[32], const uint8_t* iv, size_t ivLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* cipher, size_t cipherLen,
                      const uint8_t tag[16], uint8_t* plain);

// Lowercase hex of `len` random bytes; empty on failure.
std::string randomHex(size_t len);

// Lowercase hex encoding.
std::string toHex(const uint8_t* data, size_t len);

} // namespace soi
