#include "util/Log.h"

#if defined(_WIN32)
  #include "util/Win.h"
  #include <windows.h>
#else
  #include <cstdlib>
  #include <cstring>
  #include <unistd.h>
#endif
#include <cstdio>
#include <mutex>
#include <atomic>
#include <chrono>

namespace soi {
namespace {

std::atomic<bool> g_verbose{false};
std::mutex        g_mutex;
std::string       g_logPath;
FILE*             g_logFile = nullptr;

#if defined(_WIN32)
// Windows consoles do not enable VT sequences by default; do it once, and fall
// back to uncoloured output if the handle refuses.
bool enableVirtualTerminal() {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == nullptr) return false;
    DWORD mode = 0;
    if (!GetConsoleMode(h, &mode)) return false;
    return SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != FALSE;
}
#else
// Colour only for a real terminal; a log redirected to a file stays plain.
bool enableVirtualTerminal() {
    if (!isatty(STDERR_FILENO)) return false;
    const char* term = std::getenv("TERM");
    return !(term && std::strcmp(term, "dumb") == 0);
}
#endif

const bool g_vt = enableVirtualTerminal();

const char* tag(LogLevel l) {
    if (!g_vt) {
        switch (l) {
            case LogLevel::Trace: return "[trc]";
            case LogLevel::Info:  return "[inf]";
            case LogLevel::Warn:  return "[wrn]";
            case LogLevel::Error: return "[err]";
        }
        return "[???]";
    }
    switch (l) {
        case LogLevel::Trace: return "\x1b[90m[trc]\x1b[0m";
        case LogLevel::Info:  return "\x1b[36m[inf]\x1b[0m";
        case LogLevel::Warn:  return "\x1b[33m[wrn]\x1b[0m";
        case LogLevel::Error: return "\x1b[31m[err]\x1b[0m";
    }
    return "[???]";
}

double uptimeSeconds() {
    using clock = std::chrono::steady_clock;
    static const clock::time_point start = clock::now();
    return std::chrono::duration<double>(clock::now() - start).count();
}

} // namespace

void logSetVerbose(bool on) { g_verbose.store(on, std::memory_order_relaxed); }
bool logVerbose()           { return g_verbose.load(std::memory_order_relaxed); }

void logSetFile(const std::string& path, bool truncate) {
    std::lock_guard lk(g_mutex);
    if (g_logFile) { std::fclose(g_logFile); g_logFile = nullptr; }
    g_logPath = path;
    if (path.empty()) return;

    // Each `start` truncates, so the log is about this run and cannot grow
    // without bound on a machine that shares every day.
    //
    // Opened in binary mode with NO "ccs=" encoding. Passing ccs=UTF-8 would put
    // the stream into wide orientation, after which every narrow fprintf below
    // silently writes nothing and the log contains only a BOM. Our strings are
    // already UTF-8, so raw bytes are exactly what we want.
#if defined(_WIN32)
    g_logFile = _wfopen(toUtf16(path).c_str(), truncate ? L"wb" : L"ab");
#else
    g_logFile = std::fopen(path.c_str(), truncate ? "wb" : "ab");
#endif
}

void logWrite(LogLevel lvl, std::string_view msg) {
    std::lock_guard lk(g_mutex);
    std::fprintf(stderr, "%8.3f %s %.*s\n", uptimeSeconds(), tag(lvl),
                 static_cast<int>(msg.size()), msg.data());
    std::fflush(stderr);

    if (g_logFile) {
        static const char* const kPlain[] = {"[trc]", "[inf]", "[wrn]", "[err]"};
        std::fprintf(g_logFile, "%8.3f %s %.*s\n", uptimeSeconds(),
                     kPlain[static_cast<int>(lvl)],
                     static_cast<int>(msg.size()), msg.data());
        std::fflush(g_logFile);
    }
}

std::string hrString(long hr) {
#if !defined(_WIN32)
    // On macOS the codes passed here are OSStatus values from VideoToolbox,
    // CoreMedia or ScreenCaptureKit; there is no system table to look them up
    // in, so show the number both ways for searching.
    char buf[64];
    std::snprintf(buf, sizeof buf, "%ld (0x%08lX)", hr, static_cast<unsigned long>(hr));
    return buf;
#else
    char* text = nullptr;
    const DWORD n = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, static_cast<DWORD>(hr), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&text), 0, nullptr);

    std::string detail;
    if (n && text) {
        detail.assign(text, n);
        while (!detail.empty() && (detail.back() == '\r' || detail.back() == '\n' ||
                                   detail.back() == ' '))
            detail.pop_back();
    }
    if (text) LocalFree(text);

    char buf[64];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return detail.empty() ? std::string(buf)
                          : std::string(buf) + " (" + detail + ")";
#endif
}

} // namespace soi
