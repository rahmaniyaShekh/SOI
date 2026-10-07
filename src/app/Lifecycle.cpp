#include "app/Lifecycle.h"
#include "app/Control.h"
#include "app/Install.h"
#include "app/Service.h"
#include "app/Update.h"
#include "util/Log.h"
#include "util/Platform.h"

#if defined(_WIN32)
  #include "util/Win.h"
  #include <windows.h>
  #include <shlobj.h>
#else
  #include <miniz.h>
  #include <fcntl.h>
  #include <poll.h>
  #include <signal.h>
  #include <spawn.h>
  #include <sys/stat.h>
  #include <sys/wait.h>
  #include <unistd.h>
  #include <cerrno>
  #include <cstring>
extern char** environ;
#endif

#include <chrono>
#include <cstdio>
#include <thread>

#ifndef SOI_VERSION
#define SOI_VERSION "0.0.0-dev"
#endif
#ifndef SOI_GITHUB_REPO
#define SOI_GITHUB_REPO "rahmaniyaShekh/SOI"
#endif

namespace soi {
namespace {

using namespace std::chrono_literals;

#if defined(_WIN32)
// The release asset `update` downloads, and what it is called once installed.
constexpr char kExeAsset[]   = "soi-share.exe";
constexpr char kStagedName[] = "soi-share.download.exe";
constexpr char kUserKind[]   = "Windows user";
#else
// A zip rather than the bare binary: a browser download of a bare file loses
// its executable bit, and the zip is also what people download by hand.
constexpr char kExeAsset[]   = "soi-share-macos.zip";
constexpr char kBinaryName[] = "soi-share";
constexpr char kStagedName[] = "soi-share.download";
constexpr char kUserKind[]   = "macOS user, in the login Keychain";
#endif
constexpr char kSumsAsset[] = "SHA256SUMS.txt";

bool waitUntilGone(unsigned long pid, const std::string& exe, std::chrono::milliseconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (!processAlive(pid, exe)) return true;
        std::this_thread::sleep_for(100ms);
    }
    return !processAlive(pid, exe);
}

#if defined(_WIN32)
bool terminatePid(unsigned long pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    const BOOL ok = TerminateProcess(h, 1);
    WaitForSingleObject(h, 5000);
    CloseHandle(h);
    return ok != FALSE;
}

// Runs `exe` with `args` in this console and waits. Returns its exit code, or
// -1 if it could not be started. With `capture`, stdout is collected instead
// of shown.
int runChild(const std::string& exe, const std::string& args, std::string* capture,
             DWORD timeoutMs = INFINITE) {
    std::wstring cmd = quoteArg(exe) + L" " + toUtf16(args);

    STARTUPINFOW si{};
    si.cb = sizeof si;
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (capture) {
        SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
        if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return -1;
        SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
        si.dwFlags    = STARTF_USESTDHANDLES;
        si.hStdOutput = writePipe;
        si.hStdError  = writePipe;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(toUtf16(exe).c_str(), cmd.data(), nullptr, nullptr,
                                   capture ? TRUE : FALSE, 0, nullptr, nullptr, &si, &pi);
    if (writePipe) CloseHandle(writePipe);
    if (!ok) {
        if (readPipe) CloseHandle(readPipe);
        return -1;
    }
    if (capture) {
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(readPipe, buf, sizeof buf, &got, nullptr) && got > 0) capture->append(buf, got);
        CloseHandle(readPipe);
    }
    DWORD code = static_cast<DWORD>(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &code);
    else
        TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}
#else
bool terminatePid(unsigned long pid) {
    if (kill(static_cast<pid_t>(pid), SIGKILL) != 0) return false;
    for (int i = 0; i < 50 && processAlive(pid); ++i)
        std::this_thread::sleep_for(100ms);
    return true;
}

// Runs `exe` with `args` -- already quoted for a POSIX shell, as quoteArgument
// produces them -- in this terminal and waits. Returns its exit code, or -1 if
// it could not be started or timed out. With `capture`, stdout and stderr are
// collected instead of shown.
int runChild(const std::string& exe, const std::string& args, std::string* capture,
             int timeoutMs = -1) {
    const std::string command = "exec " + quoteArgument(exe) + (args.empty() ? "" : " " + args);
    const char* const argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};

    int fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (capture) {
        if (pipe(fds) != 0) { posix_spawn_file_actions_destroy(&actions); return -1; }
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, fds[0]);
        posix_spawn_file_actions_addclose(&actions, fds[1]);
    }
    pid_t pid = 0;
    const int rc = posix_spawn(&pid, "/bin/sh", &actions, nullptr,
                               const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture) close(fds[1]);
    if (rc != 0) {
        if (capture) close(fds[0]);
        return -1;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    bool timedOut = false;
    if (capture) {
        char buf[4096];
        for (;;) {
            int wait = -1;
            if (timeoutMs >= 0) {
                const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - std::chrono::steady_clock::now()).count();
                if (left <= 0) { timedOut = true; break; }
                wait = static_cast<int>(left);
            }
            pollfd p{fds[0], POLLIN, 0};
            const int ready = poll(&p, 1, wait);
            if (ready < 0 && errno == EINTR) continue;
            if (ready == 0) { timedOut = true; break; }
            const ssize_t n = read(fds[0], buf, sizeof buf);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            capture->append(buf, static_cast<size_t>(n));
        }
        close(fds[0]);
    }

    int status = 0;
    for (;;) {
        const pid_t done = waitpid(pid, &status, timedOut ? WNOHANG : (timeoutMs < 0 ? 0 : WNOHANG));
        if (done == pid) break;
        if (done < 0 && errno != EINTR) return -1;
        if (timedOut || (timeoutMs >= 0 && std::chrono::steady_clock::now() >= deadline)) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            return -1;
        }
        std::this_thread::sleep_for(20ms);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Pulls the soi-share binary out of the release zip. The archive is the same
// one people download by hand: a folder holding the binary and its notices.
bool extractBinaryFromZip(const std::string& zip, std::string& binary, std::string& error) {
    binary.clear();
    mz_zip_archive archive{};
    if (!mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0)) {
        error = "the download is not a readable zip archive";
        return false;
    }
    bool found = false;
    const mz_uint count = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < count && !found; ++i) {
        mz_zip_archive_file_stat st{};
        if (!mz_zip_reader_file_stat(&archive, i, &st) || st.m_is_directory) continue;
        std::string name = st.m_filename;
        const size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);
        if (name != kBinaryName) continue;
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&archive, i, &size, 0);
        if (!data) { error = "could not decompress the binary from the zip"; break; }
        binary.assign(static_cast<const char*>(data), size);
        mz_free(data);
        found = true;
    }
    mz_zip_reader_end(&archive);
    if (!found && error.empty()) error = "the zip does not contain a soi-share binary";
    return found && !binary.empty();
}

bool makeDirectories(const std::string& dir) {
    if (dir.empty() || dirExists(dir)) return true;
    makeDirectories(directoryOf(dir));
    return mkdir(dir.c_str(), 0755) == 0 || dirExists(dir);
}
#endif

std::string trimmed(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    while (!s.empty() && (s.front() == '\n' || s.front() == '\r' || s.front() == ' ')) s.erase(0, 1);
    return s;
}

void printTokenHelp() {
    std::printf(
        "\n  To get a token (read-only, this repository only):\n"
        "    1. https://github.com/settings/personal-access-tokens/new\n"
        "    2. Resource owner: the owner of %s. Repository access: Only select\n"
        "       repositories -> %s.\n"
        "    3. Permissions -> Repository permissions -> Contents: Read-only.\n"
        "    4. Generate, copy the github_pat_... value, and run `soi-share update`\n"
        "       again; paste it when asked (it is not shown as you type).\n"
#if defined(_WIN32)
        "  Or set it for one terminal:  $env:SOI_SHARE_GITHUB_TOKEN = '<token>'\n"
#else
        "  Or set it for one terminal:  export SOI_SHARE_GITHUB_TOKEN='<token>'\n"
#endif
        "  Details: SETUP.md, \"Private repository: the access token\".\n",
        githubRepo(), githubRepo());
}

} // namespace

// ---------------------------------------------------------------------------

const char* appVersion() { return SOI_VERSION; }
const char* githubRepo() { return SOI_GITHUB_REPO; }

#if defined(_WIN32)
bool loadEmbeddedResource(int id, std::string& out) {
    out.clear();
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!res) return false;
    HGLOBAL mem = LoadResource(nullptr, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    if (!data) return false;
    out.assign(static_cast<const char*>(data), SizeofResource(nullptr, res));
    return !out.empty();
}
#endif
// Elsewhere loadEmbeddedResource() is generated at build time from the same
// files (cmake/EmbedResources.cmake), since there is no resource section.

int cmdVersion(bool shortForm) {
    if (shortForm) { std::printf("%s\n", appVersion()); return 0; }

    const std::string self = currentExePath();
    const std::string inst = installedExePath();
    std::printf("soi-share %s\n", appVersion());
    std::printf("  running from  %s\n", self.c_str());
    if (samePath(self, inst)) {
        std::printf("                (the installed copy)\n");
    } else if (fileExists(inst)) {
        std::printf("                (NOT the installed copy, which is %s)\n", inst.c_str());
    } else {
        std::printf("                (not installed; `soi-share install` puts it on your PATH)\n");
    }
    std::printf("  data          %s\n", stateDirectory().c_str());
    std::printf("  releases      https://github.com/%s/releases\n", githubRepo());
    return 0;
}

int cmdLicenses() {
    std::string text;
    if (!loadEmbeddedResource(kNoticesResourceId, text)) {
        std::printf("the license notices were not compiled into this build; see "
                    "THIRD_PARTY_NOTICES.md in the source tree\n");
        return 1;
    }
    std::fwrite(text.data(), 1, text.size(), stdout);
    return 0;
}

StopOutcome stopRunningInstance(bool force, bool verbose) {
    StopOutcome out;
    FoundInstance found = findInstance();
    if (found.cleanedStale && verbose)
        std::printf("(removed a stale record left by an earlier run that did not exit cleanly)\n");

    const unsigned long pid = found.record.pid;
    switch (found.state) {
        case InstanceState::None:
            out.stopped = true;
            if (verbose) std::printf("soi-share: not running\n");
            return out;

        case InstanceState::Running: {
            out.wasRunning = true;
            out.exe = found.record.exe;
            std::map<std::string, std::string> reply;
            if (controlRequest(found.record, "status", reply) && reply.count("args"))
                out.args = reply["args"];
            if (verbose) std::printf("stopping soi-share (pid %lu)...\n", pid);
            controlRequest(found.record, "stop", reply);
            // Shutdown withdraws the share code from the rendezvous (one HTTPS
            // call) and tears the peer connection down; give it time to.
            if (waitUntilGone(pid, found.record.exe, force ? 3s : 10s)) {
                out.stopped = true;
                if (verbose) std::printf("soi-share stopped\n");
                return out;
            }
            break;
        }

        case InstanceState::Unresponsive:
            out.wasRunning = true;
            out.exe = found.record.exe;
            if (!force) {
                if (verbose)
                    std::printf("soi-share (pid %lu) is running but not answering its control "
                                "channel.\nRun `soi-share stop --force` to end it.\n", pid);
                return out;
            }
            break;

        case InstanceState::Legacy:
            out.wasRunning = true;
            if (verbose) std::printf("stopping an older soi-share (pid %lu)...\n", pid);
            signalStop();
            for (int i = 0; i < (force ? 30 : 100) && legacyDaemonRunning(); ++i)
                std::this_thread::sleep_for(100ms);
            if (!legacyDaemonRunning()) {
                out.stopped = true;
                if (verbose) std::printf("soi-share stopped\n");
                return out;
            }
            break;
    }

    if (!force) {
        if (verbose)
            std::printf("soi-share (pid %lu) is still shutting down.\n"
                        "Wait a few seconds, or run `soi-share stop --force` to end it now.\n", pid);
        return out;
    }
    if (verbose) std::printf("ending pid %lu\n", pid);
    terminatePid(pid);
    // A terminated process cannot clean up after itself.
    if (found.state == InstanceState::Legacy) removeStateFile("soi.pid");
    else                                      removeStateFile("instance.txt");
    out.stopped = !processAlive(pid);
    if (verbose)
        std::printf(out.stopped ? "soi-share stopped (forced; the share code may take up to 10 "
                                  "minutes to disappear from the code page)\n"
                                : "could not end pid %lu\n", pid);
    return out;
}

// ---------------------------------------------------------------------------

int cmdInstall(bool quiet) {
    const std::string self   = currentExePath();
    const std::string dir    = installDir();
    const std::string target = installedExePath();

#if defined(_WIN32)
    if (dir.rfind("\\Programs\\soi-share") == std::string::npos || dir.size() < 24) {
        std::printf("cannot work out where %%LOCALAPPDATA%% is, so there is nowhere to install to\n");
        return 1;
    }
    std::printf("Installing soi-share %s for this Windows user (no admin needed)\n", appVersion());

    if (SHCreateDirectoryExW(nullptr, toUtf16(dir).c_str(), nullptr) != ERROR_SUCCESS &&
        !dirExists(dir)) {
        std::printf("could not create %s\n", dir.c_str());
        return 1;
    }
#else
    if (dir.rfind("/.soi-share/bin") == std::string::npos || dir.size() < 17) {
        std::printf("cannot work out where your home folder is, so there is nowhere to install to\n");
        return 1;
    }
    std::printf("Installing soi-share %s for this macOS user (no admin needed)\n", appVersion());

    if (!makeDirectories(dir)) {
        std::printf("could not create %s\n", dir.c_str());
        return 1;
    }
#endif

    StopOutcome stopped;
    if (samePath(self, target)) {
        std::printf("  program   %s (already here)\n", target.c_str());
    } else {
        // A share running from the copy being replaced keeps running the old
        // code. Stop it, and bring it back on the new version afterwards.
        const FoundInstance running = findInstance();
        if (running.state != InstanceState::None && samePath(running.record.exe, target)) {
            stopped = stopRunningInstance(true, true);
            if (!stopped.stopped) {
                std::printf("could not stop the running copy; run `soi-share stop --force` "
                            "and try again\n");
                return 1;
            }
        }
        std::string error;
        if (!replaceExecutable(target, self, error)) {
            std::printf("could not install: %s\n", error.c_str());
            return 1;
        }
        std::printf("  program   %s\n", target.c_str());
    }

    // Copied from a download, the file carries the internet zone mark and
    // SmartScreen / Gatekeeper would stop it again on the next run.
    removeZoneIdentifier(target);

    // The binary redistributes third-party code; its notices go with it.
    if (std::string notices; loadEmbeddedResource(kNoticesResourceId, notices))
        writeFileBytes(joinPath(dir, "THIRD_PARTY_NOTICES.md"), notices);

    std::string pathReport;
    if (!addToUserPath(dir, pathReport)) {
        std::printf("  PATH      could not update: %s\n"
                    "            You can still run it by its full path.\n", pathReport.c_str());
    } else {
        std::printf("  PATH      %s %s\n", dir.c_str(), pathReport.c_str());
    }
    std::printf("  data      %s (your share code; kept across updates)\n",
                stateDirectory().c_str());

    // install.ps1 hands its token over in the environment, never on a command
    // line, where any process could read it.
    const TokenChoice token = tokenFromEnvironmentOrStore();
    if (token.source == "SOI_SHARE_GITHUB_TOKEN") {
        if (token.saveable && saveToken(token.token))
            std::printf("  token     saved for `soi-share update` (encrypted for this %s)\n", kUserKind);
        else if (!token.saveable)
            std::printf("  token     NOT saved: it is a %s, which can reach every repository\n"
                        "            your account can. `soi-share update` will ask for a\n"
                        "            fine-grained read-only one when it needs it.\n",
                        describeTokenKind(classifyToken(token.token)));
    }

    if (stopped.wasRunning) {
        std::printf("\nRestarting your share on the new version...\n");
        std::fflush(stdout);
        runChild(target, "start " + stopped.args, nullptr);
    }

    if (!quiet) {
        // This process inherited its PATH from the shell; a fresh terminal is
        // the only way that shell sees the change.
        const std::string current = envVar("PATH");
        if (pathListContains(current, dir))
            std::printf("\nDone. Run:  soi-share start\n");
        else
#if defined(_WIN32)
            std::printf("\nDone. Open a NEW terminal (this one still has the old PATH), then run:\n"
                        "    soi-share start\n");
#else
            std::printf("\nDone. Open a NEW terminal window (this one still has the old PATH),\n"
                        "or run this here:  export PATH=\"$PATH:%s\"\n"
                        "Then:  soi-share start\n", dir.c_str());
#endif
    }
    return 0;
}

int cmdUninstall(bool purge) {
    const std::string dir = installDir();
    std::printf("Uninstalling soi-share\n");

    const StopOutcome stopped = stopRunningInstance(true, false);
    if (stopped.wasRunning)
#if defined(_WIN32)
        std::printf(stopped.stopped ? "  share     stopped\n"
                                    : "  share     could NOT be stopped; end soi-share.exe in "
                                      "Task Manager and run uninstall again\n");
#else
        std::printf(stopped.stopped ? "  share     stopped\n"
                                    : "  share     could NOT be stopped; quit soi-share in "
                                      "Activity Monitor and run uninstall again\n");
#endif
    if (stopped.wasRunning && !stopped.stopped) return 1;

    std::string error;
    {
        bool removed = false;
        std::string report;
        removeFromUserPath(dir, removed, report);
        std::printf("  PATH      %s\n", report.c_str());
    }

    if (!dirExists(dir)) {
        std::printf("  program   nothing installed at %s\n", dir.c_str());
    } else if (runningFromInstallDir()) {
        // Everything but this very exe can go now; the folder itself goes as
        // soon as this process has exited.
        removeDirectoryTree(dir);
        if (!scheduleDirectoryRemoval(dir, error)) {
            std::printf("  program   could not schedule removal of %s: %s\n", dir.c_str(), error.c_str());
            return 1;
        }
#if defined(_WIN32)
        std::printf("  program   %s is removed a few seconds after this exits\n", dir.c_str());
#else
        // A running binary can be unlinked here, so it is already gone.
        rmdir(directoryOf(dir).c_str());   // ~/.soi-share, if nothing else is in it
        std::printf("  program   removed %s\n", dir.c_str());
#endif
    } else if (removeDirectoryTree(dir)) {
#if !defined(_WIN32)
        rmdir(directoryOf(dir).c_str());   // ~/.soi-share, if nothing else is in it
#endif
        std::printf("  program   removed %s\n", dir.c_str());
    } else {
        std::printf("  program   could not remove everything in %s (is a file open?)\n", dir.c_str());
    }

    if (purge) {
        int files = 0;
        std::string note;
        if (purgeState(files, note))
            std::printf("  data      %s (share code, log, saved token)\n", note.c_str());
        else
            std::printf("  data      not removed: %s\n", note.c_str());
    } else {
        std::printf("  data      kept in %s: your share code comes back if you reinstall.\n"
                    "            `soi-share uninstall --purge` removes it too.\n",
                    stateDirectory().c_str());
    }
    std::printf("\nsoi-share is uninstalled. Terminals that are already open keep the old "
                "PATH until you close them.\n");
    return 0;
}

// ---------------------------------------------------------------------------

int cmdUpdate(bool checkOnly, bool force, bool forgetToken) {
    if (forgetToken) {
        std::printf(forgetSavedToken() ? "The saved GitHub token is deleted.\n"
                                       : "There was no saved GitHub token.\n");
        return 0;
    }

    Version current;
    parseVersion(appVersion(), current);

    TokenChoice token = tokenFromEnvironmentOrStore();
    Release release;
    std::printf("checking https://github.com/%s for a newer release...\n", githubRepo());
    std::fflush(stdout);
    HttpResponse r = fetchLatestRelease(githubRepo(), token.token, release);

    if (r.status == 0) {
        std::printf("could not reach GitHub: %s\nCheck your internet connection (or proxy) and "
                    "try again.\n", r.error.c_str());
        return 1;
    }

    // Anonymous, and GitHub says it is not there: for a private repository
    // that is what "you are not allowed to see it" looks like.
    if ((r.status == 404 || r.status == 401) && token.token.empty()) {
        std::printf("\nThe repository %s is private: a token is needed to download updates.\n",
                    githubRepo());
        std::string typed;
        if (promptHidden("Paste a read-only GitHub token (hidden; Enter to cancel): ", typed)) {
            token.token    = typed;
            token.source   = "prompt";
            token.saveable = classifyToken(typed) == TokenKind::FineGrained;
            r = fetchLatestRelease(githubRepo(), token.token, release);
            if (r.status == 0) {
                std::printf("could not reach GitHub: %s\n", r.error.c_str());
                return 1;
            }
        } else {
            printTokenHelp();
            return 1;
        }
    }

    if (r.status == 401) {
        std::printf("GitHub rejected the token (from %s): it has expired or been revoked.\n",
                    token.source == "saved" ? "your saved token" : token.source.c_str());
        if (token.source == "saved")
            std::printf("Run `soi-share update --forget-token`, then `soi-share update` to enter "
                        "a new one.\n");
        printTokenHelp();
        return 1;
    }
    if (r.status == 404) {
        std::printf("GitHub returned 404 for %s's latest release, using the token from %s.\n"
                    "Either the token cannot see this repository (it needs access to %s with\n"
                    "Contents: Read-only), or no release has been published yet.\n",
                    githubRepo(), token.source == "saved" ? "your saved token" : token.source.c_str(),
                    githubRepo());
        return 1;
    }
    if (r.status == 403 || r.status == 429) {
        std::printf("GitHub refused the request (HTTP %d), most likely a rate limit.\n"
                    "Wait a while and try again%s.\n",
                    r.status, token.token.empty() ? ", or use a token" : "");
        return 1;
    }
    if (r.status != 200) {
        std::printf("unexpected answer from GitHub: HTTP %d\n", r.status);
        return 1;
    }

    // The token worked. Remember it only if it is narrow enough to keep.
    if (token.source == "prompt" || token.source == "SOI_SHARE_GITHUB_TOKEN") {
        if (token.saveable) {
            std::string saved;
            if (!loadSavedToken(saved) || saved != token.token) {
                if (saveToken(token.token))
                    std::printf("Saved your token for next time (encrypted for this %s; "
                                "`soi-share update --forget-token` deletes it).\n", kUserKind);
            }
        } else {
            std::printf("Not saving this %s: it can reach every repository your account can.\n"
                        "Use a fine-grained read-only token to have it remembered.\n",
                        describeTokenKind(classifyToken(token.token)));
        }
    }

    Version latest;
    if (!parseVersion(release.tag, latest)) {
        std::printf("the latest release is tagged '%s', which is not a version number\n",
                    release.tag.c_str());
        return 1;
    }
    const int cmp = compareVersions(current, latest);
    std::printf("  this version     %s\n  latest release   %s\n", appVersion(),
                versionString(latest).c_str());

    if (checkOnly) {
        std::printf(cmp < 0 ? "An update is available: run `soi-share update`.\n"
                            : "You are up to date.\n");
        return 0;
    }
    if (cmp >= 0 && !force) {
        std::printf("Already up to date.\n");
        return 0;
    }

    const ReleaseAsset* exeAsset  = release.asset(kExeAsset);
    const ReleaseAsset* sumsAsset = release.asset(kSumsAsset);
    if (!exeAsset || !sumsAsset) {
        std::printf("release %s is missing %s; refusing to install a binary that cannot be "
                    "verified.\n", release.tag.c_str(), !exeAsset ? kExeAsset : kSumsAsset);
        return 1;
    }

    std::string sums;
    r = downloadAsset(*sumsAsset, token.token, sums);
    std::string expected;
    if (r.status != 200 || !findChecksum(sums, kExeAsset, expected)) {
        std::printf("could not read a checksum for %s from %s (HTTP %d%s%s); refusing to update.\n",
                    kExeAsset, kSumsAsset, r.status, r.error.empty() ? "" : ", ", r.error.c_str());
        return 1;
    }

    std::printf("downloading %s %s (%.1f MB)...\n", kExeAsset, release.tag.c_str(),
                exeAsset->size / 1048576.0);
    std::fflush(stdout);
    std::string exeBytes;
    r = downloadAsset(*exeAsset, token.token, exeBytes);
    if (r.status != 200 || exeBytes.empty()) {
        std::printf("download failed (HTTP %d%s%s)\n", r.status, r.error.empty() ? "" : ": ",
                    r.error.c_str());
        return 1;
    }

    const std::string actual = sha256Hex(exeBytes.data(), exeBytes.size());
    if (actual != expected) {
        std::printf("CHECKSUM MISMATCH -- refusing to install.\n  expected %s\n  got      %s\n"
                    "The download was corrupted or tampered with. Nothing was changed.\n",
                    expected.c_str(), actual.c_str());
        return 1;
    }
    std::printf("  sha256 verified  %s\n", actual.c_str());

#if !defined(_WIN32)
    // The checksum covers the zip, so what comes out of it is covered too.
    {
        std::string binary, zipError;
        if (!extractBinaryFromZip(exeBytes, binary, zipError)) {
            std::printf("could not unpack %s: %s; nothing was changed.\n", kExeAsset,
                        zipError.c_str());
            return 1;
        }
        exeBytes = std::move(binary);
    }
#endif

    // Update the installed copy; a portable copy that was never installed
    // updates itself in place.
    const std::string target = fileExists(installedExePath()) ? installedExePath() : currentExePath();

    // Staged beside the target (same volume, so the final move is a rename),
    // and run once to prove it is a working soi-share of the promised version
    // before anything is replaced.
    const std::string staged = joinPath(directoryOf(target), kStagedName);
    if (!writeFileBytes(staged, exeBytes)) {
        std::printf("could not write %s\n", staged.c_str());
        return 1;
    }
#if !defined(_WIN32)
    chmod(staged.c_str(), 0755);
#endif
    std::string reported;
    const int probe = runChild(staged, "version --short", &reported, 15000);
    if (probe != 0 || trimmed(reported) != versionString(latest)) {
        removeFile(staged);
        std::printf("the downloaded exe did not run correctly (exit %d, reported '%s'); "
                    "nothing was changed.\n", probe, trimmed(reported).c_str());
        return 1;
    }

    const StopOutcome stopped = stopRunningInstance(true, true);
    if (!stopped.stopped) {
        removeFile(staged);
        std::printf("could not stop the running share; run `soi-share stop --force` and "
                    "`soi-share update` again.\n");
        return 1;
    }

    std::string error;
    const bool replaced = replaceExecutable(target, staged, error);
    removeFile(staged);
    if (!replaced) {
        std::printf("could not replace %s: %s\n", target.c_str(), error.c_str());
        return 1;
    }
    removeZoneIdentifier(target);
    std::printf("Updated soi-share %s -> %s  (%s)\n", appVersion(), versionString(latest).c_str(),
                target.c_str());

    if (stopped.wasRunning) {
        std::printf("\nRestarting your share on the new version (same code)...\n");
        std::fflush(stdout);
        runChild(target, "start " + stopped.args, nullptr);
    }
    return 0;
}

} // namespace soi
