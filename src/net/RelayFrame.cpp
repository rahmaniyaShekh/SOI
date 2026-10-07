#include "net/RelayFrame.h"
#include "net/SignalBlob.h"
#include "util/Crypto.h"
#include "util/Json.h"

namespace soi {
namespace {

constexpr size_t kIvLen  = 12;
constexpr size_t kTagLen = 16;

void put32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

} // namespace

std::vector<uint8_t> relaySeal(const RelayKey& key, uint8_t type, const uint8_t* payload,
                               size_t len) {
    if (key.size() != 32) return {};
    std::vector<uint8_t> plain(len + 1);
    plain[0] = type;
    if (len) std::copy(payload, payload + len, plain.begin() + 1);

    std::vector<uint8_t> out(kIvLen + plain.size() + kTagLen);
    if (!randomBytes(out.data(), kIvLen)) return {};
    if (!aes256GcmEncrypt(key.data(), out.data(), kIvLen, nullptr, 0, plain.data(),
                          plain.size(), out.data() + kIvLen,
                          out.data() + kIvLen + plain.size()))
        return {};
    return out;
}

bool relayOpen(const RelayKey& key, const uint8_t* data, size_t len, uint8_t& type,
               std::vector<uint8_t>& payload) {
    if (key.size() != 32 || len < kIvLen + 1 + kTagLen) return false;
    const size_t cipherLen = len - kIvLen - kTagLen;
    std::vector<uint8_t> plain(cipherLen);
    if (!aes256GcmDecrypt(key.data(), data, kIvLen, nullptr, 0, data + kIvLen, cipherLen,
                          data + kIvLen + cipherLen, plain.data()))
        return false;
    type = plain[0];
    payload.assign(plain.begin() + 1, plain.end());
    return true;
}

void relayAppendFrame(std::vector<uint8_t>& payload, bool keyframe, uint32_t timestampMs,
                      const uint8_t* annexB, size_t len) {
    payload.push_back(keyframe ? 1 : 0);
    put32(payload, timestampMs);
    put32(payload, static_cast<uint32_t>(len));
    payload.insert(payload.end(), annexB, annexB + len);
}

bool parseRelayRequest(const std::string& plaintext, RelayKey& key) {
    key.clear();
    // An SDP starts "v=0"; only a JSON object can be a relay request.
    if (plaintext.empty() || plaintext[0] != '{') return false;
    JsonValue v;
    if (!JsonValue::parse(plaintext, v) || !v.isObject()) return false;
    if (!v["relay"].isNumber() || v["relay"].num() != 1) return false;
    std::string raw;
    if (!v["key"].isString() || !base64UrlDecode(v["key"].str(), raw) || raw.size() != 32)
        return false;
    key.assign(raw.begin(), raw.end());
    return true;
}

} // namespace soi
