#include "util/Crypto.h"

#include <vector>

namespace soi {

std::string toHex(const uint8_t* data, size_t len) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        out.push_back(kHex[data[i] >> 4]);
        out.push_back(kHex[data[i] & 0x0F]);
    }
    return out;
}

std::string randomHex(size_t len) {
    std::vector<uint8_t> raw(len);
    if (!randomBytes(raw.data(), raw.size())) return {};
    return toHex(raw.data(), raw.size());
}

} // namespace soi
