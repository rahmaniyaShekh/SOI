#include "app/Install.h"
#include "app/Service.h"
#include "util/Log.h"
#include "util/Win.h"

#include <windows.h>
#include <shlobj.h>

#include <vector>

namespace soi {
namespace {

constexpr wchar_t kExeName[] = L"soi-share.exe";

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
        const size_t semi = value.find(';', start);
        parts.push_back(value.substr(start, semi == std::string::npos ? std::string::npos
                                                                      : semi - start));
        if (semi == std::string::npos) break;
        start = semi + 1;
    }
    return parts;
}

bool equalNoCase(const std::string& a, const std::string& b) {
    const std::wstring wa = toUtf16(a), wb = toUtf16(b);
    return CompareStringOrdinal(wa.c_str(), static_cast<int>(wa.size()),
                                wb.c_str(), static_cast<int>(wb.size()), TRUE) == CSTR_EQUAL;
}

std::string expandEnv(const std::string& s) {
    if (s.find('%') == std::string::npos) return s;
    const std::wstring in = toUtf16(s);
    const DWORD need = ExpandEnvironmentStringsW(in.c_str(), nullptr, 0);
    if (need == 0) return s;
    std::wstring out(need, L'\0');
    const DWORD got = ExpandEnvironmentStringsW(in.c_str(), out.data(), need);
    if (got == 0 || got > need) return s;
    out.resize(got - 1);   // drop the terminator the count includes
    return toUtf8(out);
}

std::string fullPath(const std::string& p) {
    const std::wstring in = toUtf16(p);
    std::wstring out(32768, L'\0');
    DWORD n = GetFullPathNameW(in.c_str(), static_cast<DWORD>(out.size()), out.data(), nullptr);
    if (n == 0 || n >= out.size()) return p;
    out.resize(n);
    // Resolve 8.3 short names (C:\Users\JOHNSM~1) when the path exists.
    std::wstring longer(32768, L'\0');
    n = GetLongPathNameW(out.c_str(), longer.data(), static_cast<DWORD>(longer.size()));
    if (n > 0 && n < longer.size()) { longer.resize(n); out = longer; }
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return toUtf8(out);
}

std::string lastErrorText() {
    return hrString(static_cast<long>(HRESULT_FROM_WIN32(GetLastError())));
}

void deleteOldIn(const std::string& dir) {
    WIN32_FIND_DATAW fd{};
    const std::wstring pattern = toUtf16(dir) + L"\\soi-share*.old";
    HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        // Fails harmlessly while that old version is still running.
        DeleteFileW((toUtf16(dir) + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

} // namespace

// ---------------------------------------------------------------------------

std::string installDirFor(const std::string& localAppData) {
    std::string base = localAppData;
    while (!base.empty() && (base.back() == '\\' || base.back() == '/')) base.pop_back();
    return base + "\\Programs\\soi-share";
}

std::string localAppDataDir() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return toUtf8(std::wstring_view(buf, n));

    // A process started with a scrubbed environment still has a profile.
    PWSTR known = nullptr;
    std::string out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &known)) && known)
        out = toUtf8(known);
    CoTaskMemFree(known);
    return out;
}

std::string installDir()       { return installDirFor(localAppDataDir()); }
std::string installedExePath() { return installDir() + "\\soi-share.exe"; }

bool samePath(const std::string& a, const std::string& b) {
    return equalNoCase(fullPath(a), fullPath(b));
}

bool runningFromInstallDir() {
    return samePath(directoryOf(currentExePath()), installDir());
}

// --- PATH list --------------------------------------------------------------

std::string normalizePathEntry(const std::string& entry) {
    std::string s = trimBlanks(entry);
    std::string unquoted;
    for (char c : s)
        if (c != '"') unquoted += c;
    s = trimBlanks(expandEnv(unquoted));
    // "C:\x\" and "C:\x" are the same folder. Keep a bare drive root intact.
    while (s.size() > 3 && (s.back() == '\\' || s.back() == '/')) s.pop_back();
    return s;
}

int pathListCount(const std::string& pathValue, const std::string& dir) {
    const std::string want = normalizePathEntry(dir);
    if (want.empty()) return 0;
    int n = 0;
    for (const auto& part : splitPathList(pathValue))
        if (equalNoCase(normalizePathEntry(part), want)) ++n;
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
    // Appended, not prepended: an installer must not shadow anything the user
    // already has on PATH.
    // A PATH that ended in ';' keeps ending in ';', so that removing the entry
    // again gives back exactly the value that was there before.
    if (pathValue.back() == ';') return pathValue + dir + ";";
    return pathValue + ";" + dir;
}

std::string pathListRemove(const std::string& pathValue, const std::string& dir, int& removed) {
    removed = 0;
    const std::string want = normalizePathEntry(dir);
    std::string out;
    bool first = true;
    for (const auto& part : splitPathList(pathValue)) {
        if (!want.empty() && equalNoCase(normalizePathEntry(part), want)) { ++removed; continue; }
        if (!first) out += ';';
        out += part;
        first = false;
    }
    return removed ? out : pathValue;
}

// --- registry ---------------------------------------------------------------

bool readUserPath(std::string& value, bool& exists, std::string& error) {
    value.clear();
    exists = false;
    HKEY key = nullptr;
    LSTATUS st = RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_QUERY_VALUE, &key);
    if (st == ERROR_FILE_NOT_FOUND) return true;   // no user environment at all
    if (st != ERROR_SUCCESS) { error = "cannot open HKCU\\Environment"; return false; }

    // RegQueryValueEx, not RegGetValue: the latter expands REG_EXPAND_SZ, and
    // writing an expanded copy back would silently freeze every %VAR% entry.
    DWORD type = 0, bytes = 0;
    st = RegQueryValueExW(key, L"Path", nullptr, &type, nullptr, &bytes);
    if (st == ERROR_FILE_NOT_FOUND) { RegCloseKey(key); return true; }
    if (st != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) {
        RegCloseKey(key);
        error = "HKCU\\Environment\\Path is not a string value";
        return false;
    }
    std::wstring buf(bytes / sizeof(wchar_t) + 1, L'\0');
    st = RegQueryValueExW(key, L"Path", nullptr, &type, reinterpret_cast<BYTE*>(buf.data()), &bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) { error = "cannot read HKCU\\Environment\\Path"; return false; }
    buf.resize(wcsnlen(buf.c_str(), buf.size()));
    value  = toUtf8(buf);
    exists = true;
    return true;
}

bool writeUserPath(const std::string& value, std::string& error) {
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(HKEY_CURRENT_USER, L"Environment", 0, nullptr, 0,
                                 KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) { error = "cannot open HKCU\\Environment for writing"; return false; }
    const std::wstring w = toUtf16(value);
    st = RegSetValueExW(key, L"Path", 0, REG_EXPAND_SZ, reinterpret_cast<const BYTE*>(w.c_str()),
                        static_cast<DWORD>((w.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) { error = "cannot write HKCU\\Environment\\Path"; return false; }
    return true;
}

void broadcastEnvironmentChange() {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0,
                        reinterpret_cast<LPARAM>(L"Environment"),
                        SMTO_ABORTIFHUNG, 5000, &result);
}

bool addToUserPath(const std::string& dir, std::string& report) {
    std::string value, error;
    bool exists = false;
    if (!readUserPath(value, exists, error)) { report = error; return false; }

    const int present = pathListCount(value, dir);
    if (present == 1) { report = "already on your PATH"; return true; }

    std::string next = value;
    if (present > 1) {   // tidy duplicates an older installer left behind
        int removed = 0;
        next = pathListRemove(next, dir, removed);
    }
    bool changed = false;
    next = pathListAdd(next, dir, changed);
    if (!writeUserPath(next, error)) { report = error; return false; }
    broadcastEnvironmentChange();
    report = present > 1 ? "on your PATH (removed duplicate entries)" : "added to your PATH";
    return true;
}

bool removeFromUserPath(const std::string& dir, bool& removed, std::string& report) {
    removed = false;
    std::string value, error;
    bool exists = false;
    if (!readUserPath(value, exists, error)) { report = "could not read: " + error; return false; }
    int count = 0;
    const std::string next = pathListRemove(value, dir, count);
    if (!count) { report = "nothing to remove"; return true; }
    if (!writeUserPath(next, error)) { report = "could not update: " + error; return false; }
    broadcastEnvironmentChange();
    removed = true;
    report = "removed " + dir;
    return true;
}

// --- files ------------------------------------------------------------------

bool removeZoneIdentifier(const std::string& path) {
    const std::wstring stream = toUtf16(path) + L":Zone.Identifier";
    if (DeleteFileW(stream.c_str())) return true;
    const DWORD err = GetLastError();
    return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
}

// Retries `step` for up to five seconds. A process that has just exited, or an
// antivirus scan of a file that was just written, holds it for a moment, and
// an update that gives up on the first sharing violation has already stopped
// the share it was meant to restart.
template <typename Step>
bool retryFileStep(Step step) {
    const ULONGLONG deadline = GetTickCount64() + 5000;
    for (;;) {
        if (step()) return true;
        const DWORD err = GetLastError();
        if (err != ERROR_SHARING_VIOLATION && err != ERROR_ACCESS_DENIED &&
            err != ERROR_LOCK_VIOLATION)
            return false;
        if (GetTickCount64() >= deadline) return false;
        Sleep(100);
        SetLastError(err);
    }
}

bool replaceExecutable(const std::string& target, const std::string& source, std::string& error) {
    const std::wstring wTarget = toUtf16(target);
    const std::wstring wStage  = toUtf16(target + ".new");

    // Stage next to the target first, so the final step is a rename on one
    // volume: atomic, and it cannot leave a half-written exe in place.
    if (!retryFileStep([&] { return CopyFileW(toUtf16(source).c_str(), wStage.c_str(), FALSE); })) {
        error = "could not copy the new exe into " + directoryOf(target) + ": " + lastErrorText();
        return false;
    }

    if (MoveFileExW(wStage.c_str(), wTarget.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;

    // The target is running (ours, or a share still in progress) and cannot be
    // overwritten -- 0x80070020 -- but a running image CAN be renamed. Move it
    // aside under a fresh name, then move the new one in.
    std::wstring aside = wTarget + L".old";
    const bool movedAside = retryFileStep([&] {
        if (MoveFileExW(wTarget.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        // An even older *.old may itself still be running; pick a fresh name.
        const DWORD err = GetLastError();
        aside = wTarget + L"." + std::to_wstring(GetTickCount64()) + L".old";
        if (MoveFileExW(wTarget.c_str(), aside.c_str(), 0)) return true;
        if (GetLastError() == ERROR_ALREADY_EXISTS) SetLastError(err);
        return false;
    });
    if (!movedAside) {
        error = "could not move the old " + target + " aside: " + lastErrorText();
        DeleteFileW(wStage.c_str());
        return false;
    }
    if (!retryFileStep([&] { return MoveFileExW(wStage.c_str(), wTarget.c_str(), MOVEFILE_WRITE_THROUGH); })) {
        error = "could not move the new exe into place: " + lastErrorText();
        MoveFileExW(aside.c_str(), wTarget.c_str(), 0);   // roll back
        DeleteFileW(wStage.c_str());
        return false;
    }
    // Deleted now if nothing runs it; otherwise by cleanupOldBinaries() on a
    // later start.
    DeleteFileW(aside.c_str());
    return true;
}

void cleanupOldBinaries() {
    const std::string self = directoryOf(currentExePath());
    deleteOldIn(self);
    const std::string inst = installDir();
    if (!samePath(self, inst)) deleteOldIn(inst);
}

bool removeDirectoryTree(const std::string& dir) {
    const std::wstring root = toUtf16(dir);
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = root + L"\\" + name;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
                removeDirectoryTree(toUtf8(full));
            } else if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirectoryW(full.c_str());   // a junction: unlink, never follow
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(root.c_str());
    return GetFileAttributesW(root.c_str()) == INVALID_FILE_ATTRIBUTES;
}

bool scheduleDirectoryRemoval(const std::string& dir, std::string& error) {
    wchar_t sys[MAX_PATH] = {};
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { error = "cannot locate the system directory"; return false; }
    const std::wstring system = sys;
    const std::wstring cmdExe = system + L"\\cmd.exe";
    const std::wstring d      = toUtf16(dir);

    // Built by hand and handed to CreateProcessW unmodified. cmd.exe does not
    // follow the CRT's quoting rules -- an argv-style quoter would turn the
    // inner quotes into \" and cmd would look for a folder with a backslash in
    // its name. /s strips exactly the outer pair of quotes and nothing else.
    //
    // `ping` is the wait: `timeout` refuses to run without a console. The loop
    // retries for ~20 s in case this process takes a moment to exit.
    const std::wstring command =
        L"\"" + cmdExe + L"\" /d /s /c \""
        L"ping -n 3 127.0.0.1 >nul & "
        L"for /l %i in (1,1,20) do @if exist \"" + d + L"\\\" "
        L"(rmdir /s /q \"" + d + L"\" 2>nul & ping -n 2 127.0.0.1 >nul)\"";

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = command;
    // CREATE_NO_WINDOW rather than DETACHED_PROCESS: cmd and ping are console
    // programs and behave best with a (hidden) console of their own. The
    // working directory is System32 so the helper never pins the folder it is
    // deleting.
    DWORD flags = CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB;
    BOOL ok = CreateProcessW(cmdExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                             flags, nullptr, system.c_str(), &si, &pi);
    if (!ok && GetLastError() == ERROR_ACCESS_DENIED) {
        flags &= ~CREATE_BREAKAWAY_FROM_JOB;
        mutableCmd = command;
        ok = CreateProcessW(cmdExe.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                            flags, nullptr, system.c_str(), &si, &pi);
    }
    if (!ok) { error = "could not start cmd.exe: " + lastErrorText(); return false; }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace soi
