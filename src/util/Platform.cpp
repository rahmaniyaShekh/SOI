// The parts of util/Platform.h that are the same everywhere.
#include "util/Platform.h"

namespace soi {

namespace {
bool isSep(char c) {
#if defined(_WIN32)
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}
char lowerAscii(char c) { return static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c); }
} // namespace

std::string joinPath(const std::string& dir, const std::string& leaf) {
    if (dir.empty()) return leaf;
    if (isSep(dir.back())) return dir + leaf;
    return dir + kPathSep + leaf;
}

std::string directoryOf(const std::string& path) {
#if defined(_WIN32)
    const size_t slash = path.find_last_of("\\/");
#else
    const size_t slash = path.find_last_of('/');
#endif
    if (slash == std::string::npos) return ".";
    if (slash == 0) return path.substr(0, 1);   // "/x" -> "/"
    return path.substr(0, slash);
}

bool equalsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lowerAscii(a[i]) != lowerAscii(b[i])) return false;
    return true;
}

bool containsNoCase(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > hay.size()) return false;
    for (size_t i = 0; i + needle.size() <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle.size() && lowerAscii(hay[i + j]) == lowerAscii(needle[j])) ++j;
        if (j == needle.size()) return true;
    }
    return false;
}

} // namespace soi
