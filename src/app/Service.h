#pragma once
//
// Detached-process control.
//
// soi-share runs headless: no window, no console, no GUI. The streaming process
// is spawned detached so closing the terminal that started it does not kill it,
// and every control verb (status/offer/answer/stop) works from any other
// terminal in the same user session.
//
// State lives in %LOCALAPPDATA%\soi-share:
//
//   soi.pid      pid + image path of the running daemon (image path guards
//                against acting on a recycled pid)
//   status.txt   one word: starting | gathering | awaiting-answer | connecting |
//                streaming | stopping | stopped
//   offer.blob   written by the daemon once ICE gathering completes
//   answer.blob  written by `soi-share answer`, consumed and deleted by the daemon
//   soi.log      the daemon's log, since it has no console to write to
//
// Shutdown is cooperative: `stop` signals a named event and only escalates to
// TerminateProcess if the daemon does not exit in time.
//
#include <string>
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

void        setDaemonState(DaemonState state);
DaemonState currentDaemonState();
std::string describeState(DaemonState state);

// --- single instance --------------------------------------------------------

// Acquires the process-wide instance lock. Returns false if a daemon is already
// running. The lock is released when the process exits.
bool acquireInstanceLock();

// True if a live daemon owns the pid file. Verifies the recorded image path so a
// recycled pid is never mistaken for our daemon.
bool daemonRunning(unsigned long* pidOut = nullptr);

bool writePidFile();
void clearPidFile();

// --- stop signalling --------------------------------------------------------

// Created by the daemon, set by `soi-share stop`.
bool createStopEvent();
bool stopRequested();
bool signalStop();
void closeStopEvent();

// --- spawning ---------------------------------------------------------------

// Relaunches this executable with `args` plus the internal --daemon flag, fully
// detached (DETACHED_PROCESS | CREATE_NO_WINDOW). Returns the child pid, or 0.
unsigned long spawnDetached(const std::vector<std::string>& args);

// Polls until the daemon reaches one of `wanted`, or it dies, or timeout.
bool waitForState(const std::vector<DaemonState>& wanted, int timeoutMs,
                  DaemonState* reached = nullptr);

} // namespace soi
