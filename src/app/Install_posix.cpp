// macOS (POSIX) half of app/Install.h. The Windows half is Install.cpp.
#include "app/Install.h"
#include "app/Service.h"
#include "util/Log.h"
#include "util/Platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace soi {
namespace {

constexpr char kExeName[]     = "soi-share";
constexpr char kBlockBegin[]  = "# >>> soi-share >>>";
constexpr char kBlockEnd[]    = "# <<< soi-share <<<";

std::string home() {
    if (std::string h = envVar("HOME"); !h.empty()) return h;
    if (const passwd* pw = getpwuid(getuid()); pw && pw->pw_dir) return pw->pw_dir;
    return {};
}

std::string trimBlanks(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) --e;
    return s.substr(b, e - b);
}

std::vector<std::string> splitPathList(const std::string& value) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t colon = value.find(':', start);
        parts.push_back(value.substr(start, colon == std::string::npos ? std::string::npos
                                                                       : colon - start));
        if (colon == std::string::npos) break;
        start = colon + 1;
    }
    return parts;
}

std::string realPath(const std::string& p) {
    char buf[PATH_MAX];
    if (realpath(p.c_str(), buf)) return buf;
    // Not there (yet): resolve the parent and keep the leaf, so a path that is
    // about to be created still compares sensibly.
    const size_t slash = p.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && realpath(p.substr(0, slash).c_str(), buf))
        return std::string(buf) + p.substr(slash);
    return p;
}

// "~/.soi-share/bin" spelled so a shell expands it for whoever runs it, which
// keeps the profile line correct if the home folder is ever renamed.
std::string shellSpelling(const std::string& dir) {
    const std::string h = home();
    if (!h.empty() && dir.rfind(h + "/", 0) == 0) return "$HOME" + dir.substr(h.size());
    return dir;
}

bool readText(const std::string& path, std::string& out) {
    return readFileBytes(path, out);
}

// Writes `text` to `path` keeping its permissions, via write-then-rename so a
// crash cannot leave the user's profile half written.
bool writeTextKeepingMode(const std::string& path, const std::string& text) {
    struct stat st{};
    const bool existed = stat(path.c_str(), &st) == 0;
    const std::string tmp = path + ".soi-share.tmp";
    if (!writeFileBytes(tmp, text)) return false;
    chmod(tmp.c_str(), existed ? (st.st_mode & 07777) : 0644);
    if (std::rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

struct Profile {
    std::string path;
    bool        fish = false;
    bool        createIfMissing = false;
};

// Which startup files get the block. zsh has been the default shell since
// macOS 10.15, and .zshrc is read by every interactive zsh, login or not.
// bash (login, which is what Terminal opens) reads .bash_profile; fish its
// conf.d. Files for shells the user does not have are left alone.
std::vector<Profile> profiles() {
    const std::string h = home();
    std::vector<Profile> out;
    if (h.empty()) return out;
    const std::string shell = envVar("SHELL");
    const bool usesBash = shell.size() >= 4 && shell.compare(shell.size() - 4, 4, "bash") == 0;
    const bool usesFish = shell.size() >= 4 && shell.compare(shell.size() - 4, 4, "fish") == 0;

    out.push_back({h + "/.zshrc", false, !usesBash && !usesFish});
    out.push_back({h + "/.bash_profile", false, usesBash});
    if (dirExists(h + "/.config/fish") || usesFish)
        out.push_back({h + "/.config/fish/conf.d/soi-share.fish", true, true});
    return out;
}

void deleteOldIn(const std::string& dir) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (const dirent* e = readdir(d)) {
        const std::string name = e->d_name;
        if (name.rfind("soi-share", 0) == 0 && name.size() > 4 &&
            name.compare(name.size() - 4, 4, ".old") == 0)
            unlink((dir + "/" + name).c_str());
    }
    closedir(d);
}

} // namespace

// ---------------------------------------------------------------------------

std::string installDirFor(const std::string& base) {
    std::string b = base;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    return b + "/.soi-share/bin";
}

std::string localAppDataDir()  { return home(); }
std::string installDir()       { return installDirFor(localAppDataDir()); }
std::string installedExePath() { return installDir() + "/" + kExeName; }

bool samePath(const std::string& a, const std::string& b) {
    auto clean = [](std::string p) {
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        return realPath(p);
    };
    return clean(a) == clean(b);
}

bool runningFromInstallDir() {
    return samePath(directoryOf(currentExePath()), installDir());
}

// --- PATH list --------------------------------------------------------------

std::string normalizePathEntry(const std::string& entry) {
    std::string s = trimBlanks(entry);
    std::string unquoted;
    for (char c : s)
        if (c != '"' && c != '\'') unquoted += c;
    s = trimBlanks(unquoted);
    const std::string h = home();
    if (s == "~" || s.rfind("~/", 0) == 0)          s = h + s.substr(1);
    else if (s.rfind("$HOME", 0) == 0)              s = h + s.substr(5);
    else if (s.rfind("${HOME}", 0) == 0)            s = h + s.substr(7);
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

int pathListCount(const std::string& pathValue, const std::string& dir) {
    const std::string want = normalizePathEntry(dir);
    if (want.empty()) return 0;
    int n = 0;
    for (const auto& part : splitPathList(pathValue))
        if (normalizePathEntry(part) == want) ++n;
    return n;
}

bool pathListContains(const std::string& pathValue, const std::string& dir) {
    return pathListCount(pathValue, dir) > 0;
}

std::string pathListAdd(const std::string& pathValue, const std::string& dir, bool& changed) {
    changed = false;
    if (pathListContains(pathValue, dir)) return pathValue;
    changed = true;
    if (trimBlanks(pathValue).empty()) return dir;
    if (pathValue.back() == ':') return pathValue + dir + ":";
    return pathValue + ":" + dir;
}

std::string pathListRemove(const std::string& pathValue, const std::string& dir, int& removed) {
    removed = 0;
    const std::string want = normalizePathEntry(dir);
    std::string out;
    bool first = true;
    for (const auto& part : splitPathList(pathValue)) {
        if (!want.empty() && normalizePathEntry(part) == want) { ++removed; continue; }
        if (!first) out += ':';
        out += part;
        first = false;
    }
    return removed ? out : pathValue;
}

// --- shell profiles ------------------------------------------------------------

std::string profileRemovePathBlock(const std::string& content, int& removed) {
    removed = 0;
    std::string out;
    size_t pos = 0;
    while (pos < content.size()) {
        const size_t begin = content.find(kBlockBegin, pos);
        if (begin == std::string::npos) { out += content.substr(pos); break; }
        // Only a marker at the start of a line counts.
        if (begin > 0 && content[begin - 1] != '\n') {
            out += content.substr(pos, begin + 1 - pos);
            pos = begin + 1;
            continue;
        }
        const size_t end = content.find(kBlockEnd, begin);
        if (end == std::string::npos) { out += content.substr(pos); break; }   // damaged: keep
        size_t after = end + std::strlen(kBlockEnd);
        if (after < content.size() && content[after] == '\n') ++after;
        std::string kept = content.substr(pos, begin - pos);
        // Drop the blank line addToUserPath put in front of the block.
        if (kept.size() >= 2 && kept.compare(kept.size() - 2, 2, "\n\n") == 0) kept.pop_back();
        out += kept;
        pos = after;
        ++removed;
    }
    return removed ? out : content;
}

std::string profileAddPathBlock(const std::string& content, const std::string& dir,
                                bool fish, bool& changed) {
    int removed = 0;
    const std::string base = profileRemovePathBlock(content, removed);
    const std::string spelled = shellSpelling(dir);
    const std::string line = fish ? "set -gx PATH $PATH \"" + spelled + "\""
                                  : "export PATH=\"$PATH:" + spelled + "\"";
    std::string block = std::string(kBlockBegin) + "\n" +
                        "# Added by `soi-share install`; `soi-share uninstall` removes it.\n" +
                        line + "\n" + kBlockEnd + "\n";
    std::string out = base;
    if (!out.empty() && out.back() != '\n') out += '\n';
    if (!out.empty()) out += '\n';
    out += block;
    changed = out != content;
    return out;
}

bool addToUserPath(const std::string& dir, std::string& report) {
    std::vector<std::string> updated;
    bool any = false, failed = false;
    for (const auto& p : profiles()) {
        std::string text;
        const bool exists = readText(p.path, text);
        if (!exists && !p.createIfMissing) continue;
        if (p.fish) mkdir(directoryOf(p.path).c_str(), 0755);
        bool changed = false;
        const std::string next = profileAddPathBlock(text, dir, p.fish, changed);
        any = true;
        if (!changed) continue;
        if (writeTextKeepingMode(p.path, next)) updated.push_back(p.path);
        else failed = true;
    }
    if (!any) { report = "no shell profile to add it to (is HOME set?)"; return false; }
    if (failed) { report = "could not write a shell profile"; return false; }
    if (updated.empty()) { report = "already on your PATH"; return true; }
    std::string list;
    for (const auto& u : updated) {
        if (!list.empty()) list += ", ";
        const std::string h = home();
        list += u.rfind(h + "/", 0) == 0 ? "~" + u.substr(h.size()) : u;
    }
    report = "added to your PATH (" + list + ")";
    return true;
}

bool removeFromUserPath(const std::string& /*dir*/, bool& removed, std::string& report) {
    removed = false;
    bool failed = false;
    for (const auto& p : profiles()) {
        std::string text;
        if (!readText(p.path, text)) continue;
        int count = 0;
        const std::string next = profileRemovePathBlock(text, count);
        if (!count) continue;
        if (p.fish && next.find_first_not_of(" \t\n") == std::string::npos) {
            if (unlink(p.path.c_str()) == 0) removed = true;   // the file was ours alone
            else failed = true;
            continue;
        }
        if (writeTextKeepingMode(p.path, next)) removed = true;
        else failed = true;
    }
    if (failed) { report = "could not update a shell profile"; return false; }
    report = removed ? "removed from your shell profile" : "nothing to remove";
    return true;
}

// --- files ------------------------------------------------------------------

bool removeZoneIdentifier(const std::string& path) {
    if (removexattr(path.c_str(), "com.apple.quarantine", 0) == 0) return true;
    return errno == ENOATTR;
}

bool replaceExecutable(const std::string& target, const std::string& source, std::string& error) {
    std::string bytes;
    if (!readFileBytes(source, bytes) || bytes.empty()) {
        error = "could not read " + source;
        return false;
    }
    // Stage next to the target, so the final step is a rename on one volume:
    // atomic, and a running copy keeps its (now unlinked) file until it exits.
    const std::string staged = target + ".new";
    if (!writeFileBytes(staged, bytes) || chmod(staged.c_str(), 0755) != 0) {
        error = "could not write " + staged + ": " + std::strerror(errno);
        unlink(staged.c_str());
        return false;
    }
    if (std::rename(staged.c_str(), target.c_str()) != 0) {
        error = "could not move the new binary into place: " + std::string(std::strerror(errno));
        unlink(staged.c_str());
        return false;
    }
    return true;
}

void cleanupOldBinaries() {
    const std::string self = directoryOf(currentExePath());
    deleteOldIn(self);
    const std::string inst = installDir();
    if (!samePath(self, inst)) deleteOldIn(inst);
}

bool removeDirectoryTree(const std::string& dir) {
    if (DIR* d = opendir(dir.c_str())) {
        while (const dirent* e = readdir(d)) {
            const std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = dir + "/" + name;
            struct stat st{};
            if (lstat(full.c_str(), &st) != 0) continue;
            if (S_ISDIR(st.st_mode)) removeDirectoryTree(full);   // lstat: never follows links
            else unlink(full.c_str());
        }
        closedir(d);
    }
    rmdir(dir.c_str());
    struct stat st{};
    return lstat(dir.c_str(), &st) != 0;
}

bool scheduleDirectoryRemoval(const std::string& dir, std::string& error) {
    if (removeDirectoryTree(dir)) return true;
    error = "something in " + dir + " could not be deleted";
    return false;
}

} // namespace soi
