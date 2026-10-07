#include "util/Crypto.h"

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <memory>

namespace soi {
namespace {

struct CtxFree {
    void operator()(EVP_CIPHER_CTX* c) const { EVP_CIPHER_CTX_free(c); }
};
using CipherCtx = std::unique_ptr<EVP_CIPHER_CTX, CtxFree>;

} // namespace

bool randomBytes(void* out, size_t len) {
    if (len == 0) return true;
    return RAND_bytes(static_cast<unsigned char*>(out), static_cast<int>(len)) == 1;
}

bool sha256(const void* data, size_t len, uint8_t digest[32]) {
    return SHA256(static_cast<const unsigned char*>(data), len, digest) != nullptr;
}

bool pbkdf2HmacSha256(std::string_view passphrase, const uint8_t* salt, size_t saltLen,
                      unsigned iterations, uint8_t* key, size_t keyLen) {
    return PKCS5_PBKDF2_HMAC(passphrase.data(), static_cast<int>(passphrase.size()), salt,
                             static_cast<int>(saltLen), static_cast<int>(iterations),
                             EVP_sha256(), static_cast<int>(keyLen), key) == 1;
}

bool aes256GcmEncrypt(const uint8_t key[32], const uint8_t* iv, size_t ivLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* plain, size_t plainLen,
                      uint8_t* cipher, uint8_t tag[16]) {
    CipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    int n = 0;
    if (EVP_EncryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(ivLen), nullptr) != 1 ||
        EVP_EncryptInit_ex(ctx.get(), nullptr, nullptr, key, iv) != 1)
        return false;
    if (aadLen && EVP_EncryptUpdate(ctx.get(), nullptr, &n, aad, static_cast<int>(aadLen)) != 1)
        return false;
    int written = 0;
    if (plainLen) {
        if (EVP_EncryptUpdate(ctx.get(), cipher, &n, plain, static_cast<int>(plainLen)) != 1)
            return false;
        written = n;
    }
    if (EVP_EncryptFinal_ex(ctx.get(), cipher + written, &n) != 1) return false;
    written += n;
    return written == static_cast<int>(plainLen) &&
           EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
}

bool aes256GcmDecrypt(const uint8_t key[32], const uint8_t* iv, size_t ivLen,
                      const uint8_t* aad, size_t aadLen,
                      const uint8_t* cipher, size_t cipherLen,
                      const uint8_t tag[16], uint8_t* plain) {
    CipherCtx ctx(EVP_CIPHER_CTX_new());
    if (!ctx) return false;
    int n = 0;
    if (EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_IVLEN, static_cast<int>(ivLen), nullptr) != 1 ||
        EVP_DecryptInit_ex(ctx.get(), nullptr, nullptr, key, iv) != 1)
        return false;
    if (aadLen && EVP_DecryptUpdate(ctx.get(), nullptr, &n, aad, static_cast<int>(aadLen)) != 1)
        return false;
    int written = 0;
    if (cipherLen) {
        if (EVP_DecryptUpdate(ctx.get(), plain, &n, cipher, static_cast<int>(cipherLen)) != 1)
            return false;
        written = n;
    }
    if (EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) != 1)
        return false;
    // Fails exactly when the tag does not verify.
    if (EVP_DecryptFinal_ex(ctx.get(), plain + written, &n) != 1) return false;
    written += n;
    return written == static_cast<int>(cipherLen);
}

} // namespace soi
