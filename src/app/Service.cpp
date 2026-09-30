#include "app/Service.h"
#include "util/Log.h"
#include "util/Win.h"

#include <windows.h>

#include <chrono>
#include <mutex>
#include <thread>

namespace soi {
namespace {

constexpr wchar_t kInstanceMutex[] = L"Local\\soi-share-instance";
constexpr wchar_t kStopEventName[] = L"Local\\soi-share-stop";

HANDLE g_instanceMutex = nullptr;
HANDLE g_stopEvent     = nullptr;

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

} // namespace soi
