#pragma once
#include "util/Format.h"

#include <string>
#include <string_view>
#include <cstdint>
#include <utility>

namespace soi {

enum class LogLevel { Trace, Info, Warn, Error };

void logSetVerbose(bool on);
bool logVerbose();

// Mirrors all output to a file. Required in detached mode, where there is no
// console for stderr to go to. Passing an empty path disables it again.
// `truncate` starts the file afresh; otherwise it is appended to.
void logSetFile(const std::string& path, bool truncate = false);
void logWrite(LogLevel lvl, std::string_view msg);

template <class... A> void logT(FormatString<A...> f, A&&... a) {
    if (logVerbose()) logWrite(LogLevel::Trace, soi::format(f, std::forward<A>(a)...));
}
template <class... A> void logI(FormatString<A...> f, A&&... a) {
    logWrite(LogLevel::Info, soi::format(f, std::forward<A>(a)...));
}
template <class... A> void logW(FormatString<A...> f, A&&... a) {
    logWrite(LogLevel::Warn, soi::format(f, std::forward<A>(a)...));
}
template <class... A> void logE(FormatString<A...> f, A&&... a) {
    logWrite(LogLevel::Error, soi::format(f, std::forward<A>(a)...));
}

// Formats an HRESULT as "0x80004005 (Unspecified error)".
std::string hrString(long hr);

} // namespace soi
