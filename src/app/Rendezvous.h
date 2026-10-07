#pragma once
//
// Short-code rendezvous client.
//
// Turns an ~800-character offer blob into a 6-character code a person can read
// aloud. That requires shared storage, which is the one part of signalling that
// genuinely cannot be done without a server (a 6-char code cannot carry an SDP,
// so it has to be a lookup key).
//
// What the server is not told
// ---------------------------
// The code never leaves this machine. We upload:
//
//     id      = SHA-256(code)                     -- a one-way hash
//     payload = AES-256-GCM(offer, PBKDF2(code))  -- sealed with the code
//
// so the service stores an opaque blob under an opaque key. It cannot recover
// the code, read the SDP, or learn either peer's IP address. Media never goes
// near it -- that stays peer-to-peer.
//
// This is therefore a rendezvous, not a signalling server in the usual sense:
// it relays bytes it cannot interpret.
//
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace soi {

// 6 characters from an unambiguous alphabet (no O/0, I/1, L), drawn from the
// system CSPRNG. 31^6 is about 887 million codes, and a wrong guess still only
// yields ciphertext.
std::string generateShareCode();

// Presentation form, "ABC-DEF". Purely cosmetic; the hyphen is stripped again
// before hashing, so either form can be typed.
std::string formatShareCode(const std::string& code);

// SHA-256(code) as lowercase hex -- the room id sent to the service.
std::string roomIdForCode(const std::string& code);

// 16 hex chars identifying ONE connection attempt.
//
// Answers are stored under the session, so a reconnect polls a different key and
// can never pick up the previous attempt's answer. Deleting the old answer
// instead is not sufficient: KV deletes are eventually consistent.
std::string generateSessionId();

struct RendezvousResult {
    bool        ok = false;
    std::string error;
};

// A random secret generated once per process and sent with every publish. The
// rendezvous keeps only its hash, and only the holder may join the relay for
// this room as the host.
const std::string& hostOwnerToken();

// Whether this build can carry a session over the rendezvous relay when the
// two networks cannot reach each other directly. Advertised on publish as
// caps:["relay"]; a viewer only asks for the relay when it is advertised.
bool relaySupported();

// Publishes the sealed offer under SHA-256(code). `baseUrl` is like
// "https://share.mdarif.online".
RendezvousResult publishOffer(const std::string& baseUrl, const std::string& roomId,
                              const std::string& sessionId,
                              const std::string& sealedOffer);

// One look for an answer to `sessionId`, without waiting. ok with an empty
// answer means nothing has arrived yet.
RendezvousResult pollAnswerOnce(const std::string& baseUrl, const std::string& roomId,
                                const std::string& sessionId, std::string& sealedAnswer);

// Polls for the viewer's sealed answer. Returns ok=false with an empty answer
// (and empty error) when the timeout elapses with nobody having joined.
// `shouldStop` lets a caller abort a long poll.
RendezvousResult waitForAnswer(const std::string& baseUrl, const std::string& roomId,
                               const std::string& sessionId, int timeoutSeconds,
                               std::string& sealedAnswer,
                               const std::function<bool()>& shouldStop);

// Removes the room, so a viewer sitting in a reconnect loop stops answering an
// offer nobody is listening to any more.
//
// Without this, a stopped sender keeps advertising a dead offer until the
// room's 10-minute expiry, and a viewer answers it over and over -- each try
// costing a full ICE timeout. Best effort: failing to clean up is not fatal.
RendezvousResult closeRoom(const std::string& baseUrl, const std::string& roomId);

// A WebSocket to the rendezvous relay. send* and receive may be called from
// different threads; close() unblocks both and may be called from any thread.
class RelaySocket {
public:
    virtual ~RelaySocket() = default;
    virtual bool sendBinary(const uint8_t* data, size_t len) = 0;
    virtual bool sendText(const std::string& text) = 0;
    // Blocks until a message arrives (true) or the socket closes (false).
    virtual bool receive(std::string& message, bool& binary) = 0;
    virtual void close() = 0;
};

// Joins the relay for `sessionId` as the host. Null with `error` set on failure,
// and always null where relaySupported() is false.
std::unique_ptr<RelaySocket> openRelaySocket(const std::string& baseUrl,
                                             const std::string& roomId,
                                             const std::string& sessionId,
                                             std::string& error);

} // namespace soi
