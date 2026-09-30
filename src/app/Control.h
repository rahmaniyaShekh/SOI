#pragma once
//
// The control channel: how `status`, `stop`, `answer` and `offer` talk to the
// running instance from any terminal.
//
// The instance listens on 127.0.0.1 only, on an ephemeral port, and records
// that port -- with its pid, image path and a random per-run secret -- in
// %LOCALAPPDATA%\soi-share\instance.txt. A client reads the record, connects,
// and sends one line:
//
//     <secret> <command> [argument]\n
//
// and reads key=value lines back until the instance closes the connection.
//
// Why loopback TCP plus a secret: it is reachable from every terminal and
// every shell of the same user, never from the network (bound to 127.0.0.1,
// SO_EXCLUSIVEADDRUSE), and the secret keeps another account on the same PC
// from reading the share code or stopping the share -- instance.txt lives in
// the user's own profile, which other standard users cannot read.
//
// What `status` reports is read from the live process, so it can never show a
// code or a state from a run that has already died. A record whose process is
// gone (a crash, a TerminateProcess, a reboot) is detected and deleted.
//
#include <map>
#include <string>

namespace soi {

struct InstanceRecord {
    unsigned long pid = 0;
    int           port = 0;
    std::string   secret;
    std::string   exe;          // image path, guards against pid reuse
    std::string   version;
    long long     startedUnix = 0;
};

// instance.txt is key=value lines, the same shape as a control response.
std::string serializeKeyValues(const std::map<std::string, std::string>& kv);
std::map<std::string, std::string> parseKeyValues(const std::string& text);
bool parseInstanceRecord(const std::string& text, InstanceRecord& out);

// --- server (the running instance) ------------------------------------------

// Binds 127.0.0.1:0, writes instance.txt, and serves requests on a background
// thread until stopControlServer(). "stop" sets this process's stop event.
bool startControlServer(const std::string& version, std::string& error);
void stopControlServer();   // closes the listener and deletes instance.txt

// --- client -----------------------------------------------------------------

enum class InstanceState {
    None,          // nothing running
    Running,       // live, and answering on its control channel
    Unresponsive,  // the process is alive but its channel does not answer
    Legacy,        // a 1.0.x daemon: no control channel, only the named event
};

struct FoundInstance {
    InstanceState  state = InstanceState::None;
    InstanceRecord record;
    bool           cleanedStale = false;   // a dead run's record was removed
};

// Finds the running instance, deleting the record of a dead one on the way.
FoundInstance findInstance();

// Sends one command; `reply` receives the parsed key=value response.
bool controlRequest(const InstanceRecord& rec, const std::string& command,
                    std::map<std::string, std::string>& reply, int timeoutMs = 3000);

// True while `pid` is a live process whose image is `exe` (or, when `exe` is
// empty, any live process).
bool processAlive(unsigned long pid, const std::string& exe = {});

} // namespace soi
