#include "app/Service.h"
#include "util/Log.h"
#include "util/Platform.h"

#if defined(_WIN32)
  #include "util/Win.h"
  #include <windows.h>
#else
  #include <dirent.h>
  #include <fcntl.h>
  #include <pwd.h>
  #include <spawn.h>
  #include <sys/file.h>
  #include <sys/stat.h>
  #include <unistd.h>
  #if defined(__APPLE__)
    #include <mach-o/dyld.h>
  #endif
  #include <atomic>
  #include <cerrno>
  #include <climits>
  #include <cstdio>
  #include <cstdlib>
  #include <cstring>
extern char** environ;
#endif

#include <chrono>
#include <mutex>
#include <thread>

namespace soi {
namespace {

#if defined(_WIN32)
constexpr wchar_t kInstanceMutex[] = L"Local\\soi-share-instance";
constexpr wchar_t kStopEventName[] = L"Local\\soi-share-stop";

HANDLE g_instanceMutex = nullptr;
HANDLE g_stopEvent     = nullptr;
#else
// The lock is an flock() on a file in the state folder: the kernel drops it
// when the process exits, however it exits, so a crash never leaves a stale
// lock behind.
int               g_lockFd = -1;
// No 1.0.x builds ever ran on this platform, so the stop "event" only has to
// work within this process; other processes ask over the control channel.
std::atomic<bool> g_stopRequested{false};
std::atomic<bool> g_stopArmed{false};
#endif

std::mutex                                       g_liveMutex;
std::vector<std::pair<std::string, std::string>> g_live;
std::string                                      g_pendingAnswer;

// Trims trailing whitespace; state files are written with no newline but a user
// may have edited answer.blob by hand.
std::string trim(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                          s.back() == ' '  || s.back() == '\t'))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && (s[start] == '\n' || s[start] == '\r' ||
                                s[start] == ' '  || s[start] == '\t'))
        ++start;
    return s.substr(start);
}

const char* stateTokenImpl(DaemonState s) {
    switch (s) {
        case DaemonState::NotRunning:     return "stopped";
        case DaemonState::Starting:       return "starting";
        case DaemonState::Gathering:      return "gathering";
        case DaemonState::AwaitingAnswer: return "awaiting-answer";
        case DaemonState::Connecting:     return "connecting";
        case DaemonState::Streaming:      return "streaming";
        case DaemonState::Stopping:       return "stopping";
    }
    return "stopped";
}

DaemonState tokenToStateImpl(const std::string& t) {
    if (t == "starting")        return DaemonState::Starting;
    if (t == "gathering")       return DaemonState::Gathering;
    if (t == "awaiting-answer") return DaemonState::AwaitingAnswer;
    if (t == "connecting")      return DaemonState::Connecting;
    if (t == "streaming")       return DaemonState::Streaming;
    if (t == "stopping")        return DaemonState::Stopping;
    return DaemonState::NotRunning;
}

} // namespace

#if defined(_WIN32)
// Quotes an argument for CommandLineToArgvW round-tripping.
std::wstring quoteArg(const std::string& arg) {
    const std::wstring w = toUtf16(arg);
    if (!w.empty() && w.find_first_of(L" \t\"") == std::wstring::npos) return w;

    std::wstring out = L"\"";
    size_t backslashes = 0;
    for (wchar_t c : w) {
        if (c == L'\\') { ++backslashes; continue; }
        if (c == L'"') {
            out.append(backslashes * 2 + 1, L'\\');
            out.push_back(L'"');
        } else {
            out.append(backslashes, L'\\');
            out.push_back(c);
        }
        backslashes = 0;
    }
    out.append(backslashes * 2, L'\\');
    out.push_back(L'"');
    return out;
}

std::string quoteArgument(const std::string& arg) { return toUtf8(quoteArg(arg)); }

std::string currentExePath() {
    // Long enough for any path Windows will actually run an exe from; a MAX_PATH
    // buffer truncates deep user profiles.
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    buf.resize(n);
    return toUtf8(buf);
}

// ---------------------------------------------------------------------------

std::string stateDirectory() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    std::string base = (n > 0 && n < MAX_PATH)
                           ? toUtf8(std::wstring_view(buf, n))
                           : std::string(".");

    const std::string dir = base + "\\soi-share";
    CreateDirectoryW(toUtf16(dir).c_str(), nullptr);
    return dir;
}

std::string stateFilePath(const std::string& leaf) {
    return stateDirectory() + "\\" + leaf;
}

bool writeStateFile(const std::string& leaf, const std::string& contents) {
    // Write-then-rename so a reader never observes a half-written file.
    const std::string finalPath = stateFilePath(leaf);
    const std::string tempPath  = finalPath + ".tmp";

    HANDLE h = CreateFileW(toUtf16(tempPath).c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    DWORD written = 0;
    const BOOL ok = contents.empty()
                        ? TRUE
                        : WriteFile(h, contents.data(),
                                    static_cast<DWORD>(contents.size()), &written, nullptr);
    CloseHandle(h);
    if (!ok) return false;

    return MoveFileExW(toUtf16(tempPath).c_str(), toUtf16(finalPath).c_str(),
                       MOVEFILE_REPLACE_EXISTING) != FALSE;
}

bool readStateFile(const std::string& leaf, std::string& contents) {
    contents.clear();
    HANDLE h = CreateFileW(toUtf16(stateFilePath(leaf)).c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    char  buf[8192];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0)
        contents.append(buf, got);
    CloseHandle(h);

    contents = trim(std::move(contents));
    return true;
}

bool removeStateFile(const std::string& leaf) {
    return DeleteFileW(toUtf16(stateFilePath(leaf)).c_str()) != FALSE;
}

bool purgeState(int& filesRemoved, std::string& note) {
    filesRemoved = 0;

    // A running daemon holds soi.log open and is still writing status/stats;
    // wiping under it would race the very files it depends on. Make the caller
    // stop it first.
    if (legacyDaemonRunning()) {
        note = "soi-share is still running; stop it first (soi-share stop)";
        return false;
    }

    const std::string dir = stateDirectory();

    // Enumerate rather than delete a fixed list: this must also catch the .tmp
    // spill files write-then-rename can leave behind, the machine.code, and
    // anything a future version writes without this code being told about it.
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = toUtf16(dir) + L"\\*";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // . and ..
            const std::wstring full = toUtf16(dir) + L"\\" + fd.cFileName;
            // Clear read-only just in case, then delete.
            SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
            if (DeleteFileW(full.c_str())) ++filesRemoved;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    // Best-effort: the directory only goes away if it is now empty. It is not an
    // error if it lingers (a shell may have it open); the sensitive contents are
    // already gone.
    RemoveDirectoryW(toUtf16(dir).c_str());

    note = filesRemoved ? soi::format("removed {} file(s) from {}", filesRemoved, dir)
                        : "nothing to remove; no SOI state on disk";
    return true;
}

#else
// POSIX shell quoting: single quotes, with any embedded ' closed, escaped and
// reopened. What comes out is safe to paste into sh/zsh/bash as one word.
std::string quoteArgument(const std::string& arg) {
    if (!arg.empty() && arg.find_first_not_of(
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_./:=,+@%") ==
            std::string::npos)
        return arg;
    std::string out = "'";
    for (char c : arg) {
        if (c == '\'') out += "'\\''";
        else            out += c;
    }
    return out + "'";
}

std::string currentExePath() {
#if defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string raw(size, '\0');
    if (_NSGetExecutablePath(raw.data(), &size) != 0) return {};
    raw.resize(std::strlen(raw.c_str()));
#else
    char buf[PATH_MAX] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return {};
    std::string raw(buf, static_cast<size_t>(n));
#endif
    // Absolute and free of symlinks, so it compares equal to what proc_pidpath
    // reports for the same process.
    char real[PATH_MAX];
    return realpath(raw.c_str(), real) ? std::string(real) : raw;
}

// ---------------------------------------------------------------------------

std::string homeDirectory() {
    if (std::string home = envVar("HOME"); !home.empty()) return home;
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return ".";
}

std::string stateDirectory() {
#if defined(__APPLE__)
    // Where per-user application data lives on a Mac. The folder is created
    // 0700 so another account on the same machine cannot read the share code.
    const std::string support = homeDirectory() + "/Library/Application Support";
    mkdir(support.c_str(), 0700);
    const std::string dir = support + "/soi-share";
#else
    std::string base = envVar("XDG_STATE_HOME");
    if (base.empty()) {
        base = homeDirectory() + "/.local";
        mkdir(base.c_str(), 0700);
        base += "/state";
    }
    mkdir(base.c_str(), 0700);
    const std::string dir = base + "/soi-share";
#endif
    mkdir(dir.c_str(), 0700);
    return dir;
}

std::string stateFilePath(const std::string& leaf) {
    return stateDirectory() + "/" + leaf;
}

bool writeStateFile(const std::string& leaf, const std::string& contents) {
    // Write-then-rename so a reader never observes a half-written file.
    const std::string finalPath = stateFilePath(leaf);
    const std::string tempPath  = finalPath + ".tmp";
    if (!writeFileBytes(tempPath, contents)) return false;
    return std::rename(tempPath.c_str(), finalPath.c_str()) == 0;
}

bool readStateFile(const std::string& leaf, std::string& contents) {
    contents.clear();
    if (!readFileBytes(stateFilePath(leaf), contents)) return false;
    contents = trim(std::move(contents));
    return true;
}

bool removeStateFile(const std::string& leaf) {
    return removeFile(stateFilePath(leaf));
}

bool purgeState(int& filesRemoved, std::string& note) {
    filesRemoved = 0;
    const std::string dir = stateDirectory();

    // Enumerate rather than delete a fixed list: this must also catch the .tmp
    // spill files write-then-rename can leave behind, the machine.code, and
    // anything a future version writes without this code being told about it.
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = dir + "/" + name;
            if (fileExists(full) && removeFile(full)) ++filesRemoved;
        }
        closedir(d);
    }
    // Best-effort, as on Windows: the sensitive contents are already gone.
    rmdir(dir.c_str());

    note = filesRemoved ? soi::format("removed {} file(s) from {}", filesRemoved, dir)
                        : "nothing to remove; no SOI state on disk";
    return true;
}

#endif

void setLive(const std::string& key, const std::string& value) {
    // Single-line by contract: the control channel is one key=value per line.
    std::string clean = value;
    for (char& c : clean)
        if (c == '\n' || c == '\r') c = ' ';

    std::lock_guard lk(g_liveMutex);
    for (auto& kv : g_live)
        if (kv.first == key) { kv.second = std::move(clean); return; }
    g_live.emplace_back(key, std::move(clean));
}

void clearLive(const std::string& key) {
    std::lock_guard lk(g_liveMutex);
    for (auto it = g_live.begin(); it != g_live.end(); ++it)
        if (it->first == key) { g_live.erase(it); return; }
}

std::string getLive(const std::string& key) {
    std::lock_guard lk(g_liveMutex);
    for (const auto& kv : g_live)
        if (kv.first == key) return kv.second;
    return {};
}

std::vector<std::pair<std::string, std::string>> liveSnapshot() {
    std::lock_guard lk(g_liveMutex);
    return g_live;
}

void pushAnswer(const std::string& blob) {
    std::lock_guard lk(g_liveMutex);
    g_pendingAnswer = trim(blob);
}

bool takeAnswer(std::string& blob) {
    std::lock_guard lk(g_liveMutex);
    if (g_pendingAnswer.empty()) return false;
    blob = std::move(g_pendingAnswer);
    g_pendingAnswer.clear();
    return true;
}

void setDaemonState(DaemonState state) {
    setLive("state", stateTokenImpl(state));
}

DaemonState currentDaemonState() {
    return tokenToStateImpl(getLive("state"));
}

const char* stateToken(DaemonState state) { return stateTokenImpl(state); }
DaemonState stateFromToken(const std::string& token) { return tokenToStateImpl(token); }

std::string describeState(DaemonState state) {
    switch (state) {
        case DaemonState::NotRunning:     return "not running";
        case DaemonState::Starting:       return "starting up";
        case DaemonState::Gathering:      return "gathering ICE candidates";
        case DaemonState::AwaitingAnswer: return "waiting for the viewer's answer";
        case DaemonState::Connecting:     return "connecting to the viewer";
        case DaemonState::Streaming:      return "streaming";
        case DaemonState::Stopping:       return "shutting down";
    }
    return "unknown";
}

// ---------------------------------------------------------------------------

#if defined(_WIN32)
bool acquireInstanceLock() {
    g_instanceMutex = CreateMutexW(nullptr, TRUE, kInstanceMutex);
    if (!g_instanceMutex) return false;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(g_instanceMutex);
        g_instanceMutex = nullptr;
        return false;
    }
    return true;
}

bool legacyDaemonRunning(unsigned long* pidOut) {
    std::string raw;
    if (!readStateFile("soi.pid", raw) || raw.empty()) return false;

    const size_t nl = raw.find('\n');
    const std::string pidText = raw.substr(0, nl);
    const std::string image   = (nl == std::string::npos) ? "" : trim(raw.substr(nl + 1));

    unsigned long pid = std::strtoul(pidText.c_str(), nullptr, 10);
    if (pid == 0) return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) { removeStateFile("soi.pid"); return false; }

    DWORD exitCode = 0;
    bool  alive = GetExitCodeProcess(proc, &exitCode) && exitCode == STILL_ACTIVE;

    // Guard against pid reuse: a recycled pid belonging to some unrelated
    // process must never be reported as our daemon, or `stop` would kill it.
    if (alive && !image.empty()) {
        wchar_t buf[MAX_PATH] = {};
        DWORD   len = MAX_PATH;
        if (QueryFullProcessImageNameW(proc, 0, buf, &len)) {
            const std::string running = toUtf8(std::wstring_view(buf, len));
            if (_stricmp(running.c_str(), image.c_str()) != 0) alive = false;
        }
    }
    CloseHandle(proc);

    if (!alive) removeStateFile("soi.pid");   // left behind by a crashed 1.0.x daemon
    if (alive && pidOut) *pidOut = pid;
    return alive;
}

// ---------------------------------------------------------------------------

bool createStopEvent() {
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    if (!g_stopEvent) return false;
    ResetEvent(g_stopEvent);   // a stale set state would stop us instantly
    return true;
}

bool stopRequested() {
    return g_stopEvent && WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0;
}

void requestStop() {
    if (g_stopEvent) SetEvent(g_stopEvent);
}

bool signalStop() {
    HANDLE h = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!h) return false;
    const BOOL ok = SetEvent(h);
    CloseHandle(h);
    return ok != FALSE;
}

void closeStopEvent() {
    if (g_stopEvent) { CloseHandle(g_stopEvent); g_stopEvent = nullptr; }
}

// ---------------------------------------------------------------------------

unsigned long spawnDetached(const std::vector<std::string>& args) {
    std::wstring cmd = quoteArg(currentExePath());
    for (const auto& a : args) {
        cmd += L' ';
        cmd += quoteArg(a);
    }

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    // DETACHED_PROCESS: no console at all -- not merely a hidden one -- so
    // closing the launching terminal cannot deliver CTRL_CLOSE_EVENT to us.
    // CREATE_BREAKAWAY_FROM_JOB: some terminals put children in a job object
    // that kills the whole tree when the terminal exits.
    DWORD flags = DETACHED_PROCESS | CREATE_BREAKAWAY_FROM_JOB;

    // Run from the state directory, not the caller's. A long-lived process
    // holds its working directory open, which would otherwise stop the user
    // deleting whatever folder they happened to type `start` in -- including
    // the install folder during `uninstall`.
    const std::wstring cwd = toUtf16(stateDirectory());

    std::wstring mutableCmd = cmd;
    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                             flags, nullptr, cwd.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        // The job object forbids breakaway; retry without it.
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        mutableCmd = cmd;
        ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                            flags, nullptr, cwd.c_str(), &si, &pi);
    }
    if (!ok) {
        logE("CreateProcess failed: {}",
             hrString(static_cast<long>(HRESULT_FROM_WIN32(GetLastError()))));
        return 0;
    }

    const DWORD pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return pid;
}

#else
bool acquireInstanceLock() {
    const std::string path = stateFilePath("instance.lock");
    const int fd = open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return false;
    }
    g_lockFd = fd;   // held, deliberately, until the process exits
    return true;
}

bool legacyDaemonRunning(unsigned long* /*pidOut*/) {
    return false;   // there was never a 1.0.x build for this platform
}

// ---------------------------------------------------------------------------

bool createStopEvent() {
    g_stopRequested.store(false);   // a stale set state would stop us instantly
    g_stopArmed.store(true);
    return true;
}

bool stopRequested() { return g_stopArmed.load() && g_stopRequested.load(); }

void requestStop() {
    if (g_stopArmed.load()) g_stopRequested.store(true);
}

bool signalStop() { return false; }   // other processes use the control channel

void closeStopEvent() { g_stopArmed.store(false); }

// ---------------------------------------------------------------------------

unsigned long spawnDetached(const std::vector<std::string>& args) {
    const std::string exe = currentExePath();
    std::vector<std::string> owned;
    owned.push_back(exe);
    owned.insert(owned.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (auto& a : owned) argv.push_back(a.data());
    argv.push_back(nullptr);

    // A new session (setsid) means no controlling terminal: closing the window
    // that ran `start` sends SIGHUP to that terminal's session, and this child
    // is not in it. stdio goes to /dev/null -- the child logs to its file.
    //
    // The working directory is the state folder, not the caller's, so a
    // long-lived share never pins whatever folder `start` was typed in.
    const std::string cwd = stateDirectory();

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    short flags = 0;
#if defined(POSIX_SPAWN_SETSID)
    flags |= POSIX_SPAWN_SETSID;
#endif
#if defined(POSIX_SPAWN_CLOEXEC_DEFAULT)
    // Nothing of ours -- the instance lock, a socket -- leaks into the child.
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attr, flags);

    // posix_spawn has no working-directory attribute before macOS 26, so the
    // directory is changed around the call. This runs on the main thread of a
    // short-lived `start` command with no other threads alive.
    char previous[PATH_MAX] = {};
    const bool havePrevious = getcwd(previous, sizeof previous) != nullptr;
    const bool moved = chdir(cwd.c_str()) == 0;

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), environ);

    if (moved && havePrevious) (void)chdir(previous);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0) {
        logE("could not start the background process: {}", std::strerror(rc));
        return 0;
    }
    return static_cast<unsigned long>(pid);
}

#endif

} // namespace soi
