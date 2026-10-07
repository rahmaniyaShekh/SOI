#pragma once
//
// The relay's wire format, shared by the host (net/RelayLink.cpp) and the
// viewer page (cloud/public/index.html).
//
// When two networks cannot reach each other directly, host and viewer each open
// a WebSocket to the rendezvous, which forwards every message verbatim to the
// other side. The viewer chooses a fresh 32-byte key and sends it inside its
// code-sealed answer, so the rendezvous forwards ciphertext it has no key for:
//
//     message = iv[12] | AES-256-GCM( type[1] | payload ) | tag[16]
//
//     type 1  video, host -> viewer: one or more
//             flags[1] (bit 0 = keyframe) | timestamp_ms[4] | length[4] | Annex B
//     type 2  control JSON, both directions (what the data channel carries on
//             the direct path)
//
// Integers are big-endian. Byte-identical to the page's WebCrypto code; the
// selftest's relay-seal / relay-open commands and tests/interop.js check it.
//
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace soi {

constexpr uint8_t kRelayVideo   = 1;
constexpr uint8_t kRelayControl = 2;

using RelayKey = std::vector<uint8_t>;   // 32 bytes

// Seals one message. Empty on failure (a bad key or no system random source).
std::vector<uint8_t> relaySeal(const RelayKey& key, uint8_t type, const uint8_t* payload,
                               size_t len);

// Opens one message. False when it was not sealed with this key, or was altered.
bool relayOpen(const RelayKey& key, const uint8_t* data, size_t len, uint8_t& type,
               std::vector<uint8_t>& payload);

// Appends one video frame to a type-1 payload.
void relayAppendFrame(std::vector<uint8_t>& payload, bool keyframe, uint32_t timestampMs,
                      const uint8_t* annexB, size_t len);

// The viewer's sealed answer is either an SDP or, when it wants the relay,
// {"relay":1,"key":"<base64url of 32 bytes>"}. True only for a well-formed
// relay request, with the key in `key`.
bool parseRelayRequest(const std::string& plaintext, RelayKey& key);

} // namespace soi
