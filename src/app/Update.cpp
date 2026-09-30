#include "app/Update.h"
#include "app/Service.h"
#include "util/Json.h"
#include "util/Log.h"
#include "util/Win.h"

#include <windows.h>
#include <bcrypt.h>
#include <dpapi.h>
#include <winhttp.h>

#include <cstdio>
#include <cstdlib>

#ifndef SOI_VERSION
#define SOI_VERSION "0.0.0-dev"
#endif

namespace soi {
namespace {

constexpr size_t kMaxDownloadBytes = 128u * 1024 * 1024;
constexpr char   kTokenFile[]      = "github-token.dpapi";
// Binds the DPAPI blob to this purpose: a blob some other program protected
// for the same user will not decrypt as our token.
constexpr char   kTokenEntropy[]   = "soi-share/github-token/v1";

bool isHex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

std::string lower(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return s;
}

struct HInternet {
    HINTERNET h = nullptr;
    explicit HInternet(HINTERNET v) : h(v) {}
    ~HInternet() { if (h) WinHttpCloseHandle(h); }
    HInternet(const HInternet&) = delete;
    HInternet& operator=(const HInternet&) = delete;
};

std::string hostOf(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t start = scheme + 3;
    const size_t end = url.find_first_of("/:?#", start);
    return lower(url.substr(start, end == std::string::npos ? std::string::npos : end - start));
}

std::string envVar(const wchar_t* name) {
    wchar_t buf[4096] = {};
    const DWORD n = GetEnvironmentVariableW(name, buf, 4096);
    if (n == 0 || n >= 4096) return {};
    std::string v = toUtf8(std::wstring_view(buf, n));
    while (!v.empty() && (v.back() == ' ' || v.back() == '\r' || v.back() == '\n')) v.pop_back();
    while (!v.empty() && v.front() == ' ') v.erase(0, 1);
    return v;
}

std::vector<std::string> apiHeaders(const std::string& token, const char* accept) {
    std::vector<std::string> h{std::string("Accept: ") + accept,
                               "X-GitHub-Api-Version: 2022-11-28"};
    if (!token.empty()) h.push_back("Authorization: Bearer " + token);
    return h;
}

} // namespace

// --- versions ---------------------------------------------------------------

bool parseVersion(std::string_view text, Version& out) {
    out = Version{};
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.remove_prefix(1);
    int parts[3] = {0, 0, 0};
    size_t pos = 0;
    for (int i = 0; i < 3; ++i) {
        if (pos >= text.size() || text[pos] < '0' || text[pos] > '9') return false;
        long v = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            v = v * 10 + (text[pos] - '0');
            if (v > 1000000) return false;
            ++pos;
        }
        parts[i] = static_cast<int>(v);
        if (i < 2) {
            if (pos >= text.size() || text[pos] != '.') return false;
            ++pos;
        }
    }
    if (pos < text.size() && text[pos] != '-' && text[pos] != '+') return false;
    out.major = parts[0];
    out.minor = parts[1];
    out.patch = parts[2];
    return true;
}

int compareVersions(const Version& a, const Version& b) {
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    return 0;
}

std::string versionString(const Version& v) {
    return std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
}

// --- checksums --------------------------------------------------------------

bool findChecksum(const std::string& sumsText, const std::string& fileName, std::string& hexOut) {
    hexOut.clear();
    int matches = 0;
    size_t start = 0;
    while (start < sumsText.size()) {
        size_t end = sumsText.find('\n', start);
        if (end == std::string::npos) end = sumsText.size();
        std::string line = sumsText.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A UTF-8 BOM, as PowerShell's Out-File likes to add.
        if (line.size() >= 3 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        if (line.empty() || line[0] == '#') continue;

        if (line.size() < 66) continue;
        bool hex = true;
        for (int i = 0; i < 64; ++i) hex = hex && isHex(line[i]);
        if (!hex || (line[64] != ' ' && line[64] != '\t')) continue;

        size_t name = 64;
        while (name < line.size() && (line[name] == ' ' || line[name] == '\t')) ++name;
        if (name < line.size() && line[name] == '*') ++name;   // binary-mode marker
        const std::string entry = line.substr(name);

        if (_stricmp(entry.c_str(), fileName.c_str()) == 0) {
            const std::string h = lower(line.substr(0, 64));
            // Two different sums for one file means the file cannot be trusted.
            if (matches > 0 && h != hexOut) { hexOut.clear(); return false; }
            hexOut = h;
            ++matches;
        }
    }
    return matches > 0;
}

std::string sha256Hex(const void* data, size_t size) {
    BCRYPT_ALG_HANDLE  alg  = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    unsigned char digest[32] = {};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0;
    // BCryptHashData takes a ULONG; feed large inputs in pieces.
    const auto* p = static_cast<const unsigned char*>(data);
    size_t left = size;
    while (ok && left > 0) {
        const ULONG chunk = static_cast<ULONG>(left > (1u << 30) ? (1u << 30) : left);
        ok = BCryptHashData(hash, const_cast<PUCHAR>(p), chunk, 0) == 0;
        p += chunk;
        left -= chunk;
    }
    ok = ok && BCryptFinishHash(hash, digest, sizeof digest, 0) == 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};

    static const char* kHex = "0123456789abcdef";
    std::string out;
    for (unsigned char c : digest) { out += kHex[c >> 4]; out += kHex[c & 15]; }
    return out;
}

bool sha256File(const std::string& path, std::string& hexOut) {
    hexOut.clear();
    HANDLE f = CreateFileW(toUtf16(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string data;
    char buf[65536];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got > 0) data.append(buf, got);
    CloseHandle(f);
    hexOut = sha256Hex(data.data(), data.size());
    return !hexOut.empty();
}

// --- tokens -----------------------------------------------------------------

TokenKind classifyToken(const std::string& t) {
    if (t.rfind("github_pat_", 0) == 0) return TokenKind::FineGrained;
    if (t.rfind("ghp_", 0) == 0)        return TokenKind::Classic;
    if (t.rfind("gho_", 0) == 0)        return TokenKind::OAuth;
    if (t.size() == 40) {
        bool hex = true;
        for (char c : t) hex = hex && isHex(c);
        if (hex) return TokenKind::Classic;   // pre-2021 classic token format
    }
    return TokenKind::Other;
}

const char* describeTokenKind(TokenKind kind) {
    switch (kind) {
        case TokenKind::FineGrained: return "fine-grained token";
        case TokenKind::Classic:     return "classic personal access token";
        case TokenKind::OAuth:       return "OAuth login token (e.g. the GitHub CLI's)";
        case TokenKind::Other:       return "token of an unrecognised kind";
    }
    return "token";
}

bool protectSecret(const std::string& plain, std::string& blob) {
    DATA_BLOB in{static_cast<DWORD>(plain.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(plain.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kTokenEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kTokenEntropy))};
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"soi-share GitHub token", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    blob.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

bool unprotectSecret(const std::string& blob, std::string& plain) {
    plain.clear();
    DATA_BLOB in{static_cast<DWORD>(blob.size()),
                 reinterpret_cast<BYTE*>(const_cast<char*>(blob.data()))};
    DATA_BLOB entropy{static_cast<DWORD>(sizeof(kTokenEntropy) - 1),
                      reinterpret_cast<BYTE*>(const_cast<char*>(kTokenEntropy))};
    DATA_BLOB out{};
    if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &out))
        return false;
    plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return true;
}

std::string tokenFilePath() { return stateFilePath(kTokenFile); }

bool saveToken(const std::string& token) {
    std::string blob;
    // Stored as the raw DPAPI blob. writeStateFile is write-then-rename, so a
    // crash cannot leave half a blob behind.
    return protectSecret(token, blob) && writeStateFile(kTokenFile, blob);
}

bool loadSavedToken(std::string& token) {
    token.clear();
    // Not readStateFile: that trims whitespace, which would corrupt a binary blob.
    HANDLE f = CreateFileW(toUtf16(tokenFilePath()).c_str(), GENERIC_READ, FILE_SHARE_READ,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    std::string blob;
    char buf[4096];
    DWORD got = 0;
    while (ReadFile(f, buf, sizeof buf, &got, nullptr) && got > 0) blob.append(buf, got);
    CloseHandle(f);
    return !blob.empty() && unprotectSecret(blob, token) && !token.empty();
}

bool forgetSavedToken() {
    return DeleteFileW(toUtf16(tokenFilePath()).c_str()) != FALSE;
}

TokenChoice tokenFromEnvironmentOrStore() {
    TokenChoice c;
    if (auto t = envVar(L"SOI_SHARE_GITHUB_TOKEN"); !t.empty()) {
        c.token    = t;
        c.source   = "SOI_SHARE_GITHUB_TOKEN";
        c.saveable = classifyToken(t) == TokenKind::FineGrained;
        return c;
    }
    for (const wchar_t* name : {L"GH_TOKEN", L"GITHUB_TOKEN"}) {
        if (auto t = envVar(name); !t.empty()) {
            c.token  = t;
            c.source = toUtf8(name);
            return c;
        }
    }
    if (std::string saved; loadSavedToken(saved)) {
        c.token  = saved;
        c.source = "saved";
    }
    return c;
}

bool promptHidden(const char* prompt, std::string& out) {
    out.clear();
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (in == INVALID_HANDLE_VALUE || !GetConsoleMode(in, &mode)) return false;

    std::fputs(prompt, stdout);
    std::fflush(stdout);
    SetConsoleMode(in, (mode & ~ENABLE_ECHO_INPUT) | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT);
    FlushConsoleInputBuffer(in);
    wchar_t buf[1024] = {};
    DWORD read = 0;
    const BOOL ok = ReadConsoleW(in, buf, 1023, &read, nullptr);
    SetConsoleMode(in, mode);
    std::fputs("\n", stdout);
    if (!ok) return false;

    std::wstring w(buf, read);
    SecureZeroMemory(buf, sizeof buf);
    while (!w.empty() && (w.back() == L'\r' || w.back() == L'\n' || w.back() == L' ')) w.pop_back();
    while (!w.empty() && w.front() == L' ') w.erase(0, 1);
    out = toUtf8(w);
    return !out.empty();
}

// --- HTTP -------------------------------------------------------------------

HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers,
                     bool followRedirects) {
    HttpResponse r;
    const std::wstring wurl = toUtf16(url);

    URL_COMPONENTS uc{};
    uc.dwStructSize      = sizeof uc;
    uc.dwHostNameLength  = static_cast<DWORD>(-1);
    uc.dwUrlPathLength   = static_cast<DWORD>(-1);
    uc.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &uc)) { r.error = "invalid URL"; return r; }
    const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
    std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
    if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    const std::wstring agent = toUtf16(std::string("soi-share/") + SOI_VERSION);
    // Automatic proxy (WPAD / PAC as configured for the user) where available,
    // so the updater works on the same networks the browser does.
    HInternet session(WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                  WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session.h)
        session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session.h) { r.error = "WinHttpOpen failed"; return r; }

    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2 | 0x00002000 /* TLS 1.3 */;
    if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols)) {
        protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
        WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols);
    }
    WinHttpSetTimeouts(session.h, 15000, 15000, 30000, 60000);

    HInternet connect(WinHttpConnect(session.h, host.c_str(), uc.nPort, 0));
    if (!connect.h) { r.error = "cannot connect to " + toUtf8(host); return r; }

    HInternet request(WinHttpOpenRequest(connect.h, L"GET", path.c_str(), nullptr,
                                         WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                         secure ? WINHTTP_FLAG_SECURE : 0));
    if (!request.h) { r.error = "WinHttpOpenRequest failed"; return r; }

    if (!followRedirects) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(request.h, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
    }

    std::wstring allHeaders;
    for (const auto& h : headers) allHeaders += toUtf16(h) + L"\r\n";

    if (!WinHttpSendRequest(request.h,
                            allHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : allHeaders.c_str(),
                            allHeaders.empty() ? 0 : static_cast<DWORD>(-1),
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.h, nullptr)) {
        r.error = "could not reach " + toUtf8(host) + ": " +
                  hrString(static_cast<long>(HRESULT_FROM_WIN32(GetLastError())));
        return r;
    }

    DWORD status = 0, size = sizeof status;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
    r.status = static_cast<int>(status);

    DWORD locBytes = 0;
    WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                        nullptr, &locBytes, WINHTTP_NO_HEADER_INDEX);
    if (locBytes > 0) {
        std::wstring loc(locBytes / sizeof(wchar_t) + 1, L'\0');
        if (WinHttpQueryHeaders(request.h, WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                loc.data(), &locBytes, WINHTTP_NO_HEADER_INDEX)) {
            loc.resize(locBytes / sizeof(wchar_t));
            r.location = toUtf8(loc);
        }
    }

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.h, &available) || available == 0) break;
        if (r.body.size() + available > kMaxDownloadBytes) {
            r.error = "response too large";
            r.status = 0;
            return r;
        }
        const size_t at = r.body.size();
        r.body.resize(at + available);
        DWORD read = 0;
        if (!WinHttpReadData(request.h, r.body.data() + at, available, &read)) {
            r.error = "connection dropped during download";
            r.status = 0;
            return r;
        }
        r.body.resize(at + read);
        if (read == 0) break;
    }
    return r;
}

// --- releases ---------------------------------------------------------------

const ReleaseAsset* Release::asset(const std::string& name) const {
    for (const auto& a : assets)
        if (_stricmp(a.name.c_str(), name.c_str()) == 0) return &a;
    return nullptr;
}

bool parseRelease(const std::string& json, Release& out, std::string& error) {
    out = Release{};
    JsonValue doc;
    if (!JsonValue::parse(json, doc, &error)) return false;
    if (!doc.isObject()) { error = "release is not a JSON object"; return false; }
    out.tag     = doc["tag_name"].str();
    out.htmlUrl = doc["html_url"].str();
    if (out.tag.empty()) { error = "release has no tag_name"; return false; }
    for (const auto& a : doc["assets"].items()) {
        ReleaseAsset asset;
        asset.name   = a["name"].str();
        asset.apiUrl = a["url"].str();
        asset.size   = static_cast<long long>(a["size"].num());
        if (!asset.name.empty() && asset.apiUrl.rfind("https://", 0) == 0)
            out.assets.push_back(std::move(asset));
    }
    return true;
}

HttpResponse fetchLatestRelease(const std::string& repo, const std::string& token, Release& out) {
    HttpResponse r = httpGet("https://api.github.com/repos/" + repo + "/releases/latest",
                             apiHeaders(token, "application/vnd.github+json"), false);
    if (r.status == 200) {
        std::string err;
        if (!parseRelease(r.body, out, err)) {
            r.error  = "unreadable release metadata: " + err;
            r.status = 0;
        }
    }
    return r;
}

HttpResponse downloadAsset(const ReleaseAsset& asset, const std::string& token, std::string& bytes) {
    bytes.clear();
    std::string url = asset.apiUrl;
    const std::string apiHost = hostOf(url);
    HttpResponse r;
    for (int hop = 0; hop < 6; ++hop) {
        // The token goes to the API host and nowhere else. The pre-signed
        // storage URL GitHub redirects to carries its own credentials and
        // refuses a request that also has an Authorization header.
        const bool toApi = hostOf(url) == apiHost;
        r = httpGet(url, apiHeaders(toApi ? token : std::string(), "application/octet-stream"), false);
        if (r.status == 301 || r.status == 302 || r.status == 303 ||
            r.status == 307 || r.status == 308) {
            if (r.location.rfind("https://", 0) != 0) {
                r.error  = "download redirected somewhere unexpected";
                r.status = 0;
                return r;
            }
            url = r.location;
            continue;
        }
        if (r.status == 200) bytes = std::move(r.body);
        r.body.clear();
        return r;
    }
    r.error  = "too many redirects";
    r.status = 0;
    return r;
}

} // namespace soi
