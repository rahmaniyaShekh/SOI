#pragma once
//
// Detached-process control.
//
// soi-share runs headless: no window, no console, no GUI. `start` relaunches
// this exe as `start --foreground --log-file <path>` fully detached --
// DETACHED_PROCESS on Windows, a new session (setsid) with no terminal on
// macOS -- so closing the terminal that started it does not kill it, and every
// control verb (status/offer/answer/stop) works from any other terminal in the
// same user session.
//
// What the running instance is doing lives in ITS memory (setLive / the answer
// slot below) and is read back over the loopback control channel in
// app/Control.h -- never from a file written at startup, which goes stale the
// moment the process crashes.
//
// Files in the state directory -- %LOCALAPPDATA%\soi-share on Windows,
// ~/Library/Application Support/soi-share on macOS:
//
//   machine.code   this PC's persistent share code (the per-device identity)
//   instance.txt   pid, control port and secret of the running instance
//   soi-share.log  the instance's log, truncated on each start
//
// Shutdown is cooperative: `stop` asks over the control channel, which sets the
// same event a console Ctrl+C would, and only `stop --force` escalates to
// TerminateProcess (SIGKILL on macOS).
//
#include <string>
#include <utility>
#include <vector>

namespace soi {

enum class DaemonState {
    NotRunning, Starting, Gathering, AwaitingAnswer, Connecting, Streaming, Stopping
};

std::string stateDirectory();                   // creates it if absent
std::string stateFilePath(const std::string& leaf);

bool writeStateFile(const std::string& leaf, const std::string& contents);
bool readStateFile(const std::string& leaf, std::string& contents);
bool removeStateFile(const std::string& leaf);

// Deletes every file SOI has ever written and the state directory itself --
// log, status, signalling blobs, stats, and the persistent share code. The
// "leave no trace on disk" control. Refuses while a daemon is running (its
// files are in use); `filesRemoved` reports how many were deleted.
bool purgeState(int& filesRemoved, std::string& note);

// --- live state -------------------------------------------------------------
//
// Key/value facts about the running session, held in memory and served by the
// control channel: "state", "code", "service", "urls", "offer", "stats", ...
// Values are single-line; a newline would break the wire format.

void setLive(const std::string& key, const std::string& value);
void clearLive(const std::string& key);
std::string getLive(const std::string& key);
std::vector<std::pair<std::string, std::string>> liveSnapshot();

// The viewer's answer, from `soi-share answer` (over the control channel) or
// from the local handover page. One slot: the latest delivery wins.
void pushAnswer(const std::string& blob);
bool takeAnswer(std::string& blob);

void        setDaemonState(DaemonState state);   // sets live "state"
DaemonState currentDaemonState();                // this process's own state
std::string describeState(DaemonState state);
const char* stateToken(DaemonState state);
DaemonState stateFromToken(const std::string& token);

// --- single instance --------------------------------------------------------

// Acquires the process-wide instance lock. Returns false if a daemon is already
// running. The lock is released when the process exits.
bool acquireInstanceLock();

// True if a soi-share 1.0.x daemon owns the legacy soi.pid file. Those builds
// have no control channel, so this is how a new exe finds -- and can stop -- a
// share an older version started. Verifies the recorded image path so a
// recycled pid is never mistaken for our daemon. Removes a stale soi.pid.
bool legacyDaemonRunning(unsigned long* pidOut = nullptr);

// --- stop signalling --------------------------------------------------------

// Created by the instance. Set by the control channel's "stop", and by
// signalStop() for 1.0.x daemons, which only listen for the named event.
bool createStopEvent();
bool stopRequested();
void requestStop();      // this process: sets its own stop event
bool signalStop();       // another process: sets the named event
void closeStopEvent();

// --- spawning ---------------------------------------------------------------

// Relaunches this executable with exactly `args`, fully detached (no console or
// controlling terminal at all) and with the state directory as its working
// directory, so it never pins the folder the user launched it from. Returns
// the child pid, or 0.
unsigned long spawnDetached(const std::vector<std::string>& args);

#if defined(_WIN32)
// Quotes one argument so CommandLineToArgvW / the CRT reproduce it exactly.
std::wstring quoteArg(const std::string& arg);
#endif

// One argument quoted for this platform's command line, as UTF-8: the CRT's
// rules on Windows, POSIX shell quoting elsewhere. A string of these joined by
// spaces is what `stop`/`update` hand back to `start` to restart a share.
std::string quoteArgument(const std::string& arg);

// Absolute path of this executable.
std::string currentExePath();

} // namespace soi
