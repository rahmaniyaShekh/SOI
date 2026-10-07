#pragma once
//
// The small operating-system services shared code needs, with one
// implementation per platform (Platform_win.cpp, Platform_posix.cpp).
//
// Everything here speaks UTF-8; the Windows implementation converts at the
// Win32 boundary.
//
#include <string>
#include <string_view>

namespace soi {

#if defined(_WIN32)
inline constexpr char kPathSep = '\\';
#else
inline constexpr char kPathSep = '/';
#endif

// `dir` + separator + `leaf`, without doubling a trailing separator.
std::string joinPath(const std::string& dir, const std::string& leaf);

// Everything before the last separator; "." when there is none.
std::string directoryOf(const std::string& path);

unsigned long currentProcessId();

bool readFileBytes(const std::string& path, std::string& out);
// Creates or truncates.
bool writeFileBytes(const std::string& path, const std::string& bytes);
bool fileExists(const std::string& path);   // a regular file, not a folder
bool dirExists(const std::string& path);
bool removeFile(const std::string& path);

// Trimmed value of an environment variable; empty if unset.
std::string envVar(const char* name);

// ASCII case-insensitive comparisons.
bool equalsNoCase(std::string_view a, std::string_view b);
// Case-insensitive substring test, used for --window title matching.
bool containsNoCase(std::string_view hay, std::string_view needle);

// Text onto the system clipboard. False if there is none to use.
bool copyToClipboard(const std::string& text);

// Opens a URL (or file:// URL) with the user's default handler.
bool openUrl(const std::string& url);

// True when stdout is an interactive terminal that understands ANSI escapes.
// On Windows this also switches the console into VT mode, once.
bool enableAnsi();

// True when this process has a terminal to print to at all.
bool attachedToTerminal();

} // namespace soi
