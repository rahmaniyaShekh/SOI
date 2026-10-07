#include "util/Platform.h"
#include "util/Win.h"

#include <windows.h>
#include <shellapi.h>

#include <cstring>

namespace soi {

unsigned long currentProcessId() { return GetCurrentProcessId(); }

bool readFileBytes(const std::string& path, std::string& out) {
    out.clear();
    HANDLE h = CreateFileW(toUtf16(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char  buf[65536];
    DWORD got = 0;
    while (ReadFile(h, buf, sizeof buf, &got, nullptr) && got > 0) out.append(buf, got);
    CloseHandle(h);
    return true;
}

bool writeFileBytes(const std::string& path, const std::string& bytes) {
    HANDLE f = CreateFileW(toUtf16(path).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const BOOL ok = bytes.empty() ||
                    WriteFile(f, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
    CloseHandle(f);
    return ok && written == bytes.size();
}

bool fileExists(const std::string& path) {
    const DWORD a = GetFileAttributesW(toUtf16(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string& path) {
    const DWORD a = GetFileAttributesW(toUtf16(path).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

bool removeFile(const std::string& path) {
    return DeleteFileW(toUtf16(path).c_str()) != FALSE;
}

std::string envVar(const char* name) {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(toUtf16(name).c_str(), buf, 4096);
    if (n == 0 || n >= 4096) return {};
    std::string v = toUtf8(std::wstring_view(buf, n));
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    return v;
}

bool copyToClipboard(const std::string& text) {
    const std::wstring wide = toUtf16(text);
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (wide.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* dst = GlobalLock(mem)) {
                std::memcpy(dst, wide.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);
        }
    }
    CloseClipboard();
    return ok;
}

bool openUrl(const std::string& url) {
    const auto rc = reinterpret_cast<INT_PTR>(
        ShellExecuteW(nullptr, L"open", toUtf16(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL));
    return rc > 32;
}

// Windows consoles do not interpret ANSI escapes unless asked. Ask once, and
// fall back to plain text when the handle is redirected to a file or a pipe --
// emitting raw escape bytes into a log would be worse than no emphasis.
bool enableAnsi() {
    static const bool ok = [] {
        const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
        if (out == INVALID_HANDLE_VALUE || GetFileType(out) != FILE_TYPE_CHAR) return false;
        DWORD mode = 0;
        if (!GetConsoleMode(out, &mode)) return false;
        return SetConsoleMode(out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
    }();
    return ok;
}

bool attachedToTerminal() { return GetConsoleWindow() != nullptr; }

} // namespace soi
