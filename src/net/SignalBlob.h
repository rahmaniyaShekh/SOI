#pragma once
//
// Serverless signalling payload codec.
//
// An SDP offer/answer is the only thing that must travel out-of-band before a
// WebRTC connection exists. This packs one into a single text token the user can
// paste anywhere.
//
//   "SOI1:" + base64url( header || [salt||iv] || deflate-raw(sdp) || [gcm tag] )
//
//   header : magic "SOI1" (4 bytes) || flags (1 byte); bit 0 = encrypted
//   salt   : 16 bytes, PBKDF2-HMAC-SHA256, 200'000 iterations -> 32-byte key
//   iv     : 12 bytes, AES-256-GCM
//   tag    : 16 bytes, GCM authentication tag
//   AAD    : header || salt || iv
//
// Encryption is not decoration. A raw SDP discloses your private LAN addresses,
// your public IP, and your DTLS fingerprint. Pasting one into a group chat leaks
// your network topology to everyone in the room.
//
// The viewer (viewer/viewer.html) implements the identical format on WebCrypto +
// DecompressionStream('deflate-raw'); any change here must be mirrored there.
//
#include <string>
#include <string_view>

namespace soi {

inline constexpr std::string_view kBlobPrefix = "SOI1:";

// Returns an empty string on failure (and logs the reason).
// An empty passphrase produces an unencrypted, compressed blob.
std::string encodeSignalBlob(std::string_view sdp, std::string_view passphrase);

// `error` receives a human-readable reason on failure. A wrong passphrase is
// reported as an authentication failure, not as corrupt data, because GCM
// cannot distinguish the two and guessing would mislead the user.
bool decodeSignalBlob(std::string_view blob, std::string_view passphrase,
                      std::string& sdpOut, std::string& error);

// Exposed for the URL-fragment hand-off and for tests.
std::string base64UrlEncode(const void* data, size_t len);
bool        base64UrlDecode(std::string_view text, std::string& out);

} // namespace soi
