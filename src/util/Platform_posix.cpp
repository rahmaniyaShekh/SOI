#include "util/Platform.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>

extern char** environ;

namespace soi {
namespace {

// Runs `argv` with `input` on its stdin and waits. Used for the two jobs macOS
// does best through its own small tools: the clipboard (pbcopy) and opening a
// URL in the default browser (open).
bool runWithInput(const char* const argv[], const std::string& input) {
    int fds[2];
    if (pipe(fds) != 0) return false;

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[0], STDIN_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, argv[0], &actions, nullptr,
                               const_cast<char* const*>(argv), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[0]);
    if (rc != 0) { close(fds[1]); return false; }

    size_t off = 0;
    while (off < input.size()) {
        const ssize_t n = write(fds[1], input.data() + off, input.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    close(fds[1]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return off == input.size() && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace

unsigned long currentProcessId() { return static_cast<unsigned long>(getpid()); }

bool readFileBytes(const std::string& path, std::string& out) {
    out.clear();
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char buf[65536];
    size_t got = 0;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) out.append(buf, got);
    std::fclose(f);
    return true;
}

bool writeFileBytes(const std::string& path, const std::string& bytes) {
    // 0600: what lands in the state folder (share code, token, control secret)
    // is nobody else's business, and the file mode is what enforces that here.
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    size_t off = 0;
    while (off < bytes.size()) {
        const ssize_t n = write(fd, bytes.data() + off, bytes.size() - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        off += static_cast<size_t>(n);
    }
    const bool ok = off == bytes.size();
    return close(fd) == 0 && ok;
}

bool fileExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool dirExists(const std::string& path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool removeFile(const std::string& path) { return unlink(path.c_str()) == 0; }

std::string envVar(const char* name) {
    const char* raw = std::getenv(name);
    if (!raw) return {};
    std::string v = raw;
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    return v;
}

bool copyToClipboard(const std::string& text) {
#if defined(__APPLE__)
    const char* const argv[] = {"/usr/bin/pbcopy", nullptr};
    return runWithInput(argv, text);
#else
    (void)text;
    return false;
#endif
}

bool openUrl(const std::string& url) {
#if defined(__APPLE__)
    const char* const argv[] = {"/usr/bin/open", url.c_str(), nullptr};
#else
    const char* const argv[] = {"/usr/bin/xdg-open", url.c_str(), nullptr};
#endif
    return runWithInput(argv, {});
}

bool enableAnsi() {
    static const bool ok = [] {
        if (!isatty(STDOUT_FILENO)) return false;
        const char* term = std::getenv("TERM");
        return !(term && std::string(term) == "dumb");
    }();
    return ok;
}

bool attachedToTerminal() {
    return isatty(STDIN_FILENO) || isatty(STDOUT_FILENO) || isatty(STDERR_FILENO);
}

} // namespace soi
