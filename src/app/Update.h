#pragma once
//
// Self-update from GitHub Releases.
//
// The release carries soi-share.exe (Windows), soi-share-macos.zip (macOS) and
// SHA256SUMS.txt. A binary is only ever installed if its SHA-256 matches the
// published sum; there is no "skip verification" switch.
//
// A private repository returns 404 to anonymous requests, so the updater can
// authenticate with a read-only token. Where it comes from, in order:
//
//   1. SOI_SHARE_GITHUB_TOKEN   -- the variable install.ps1 sets. Saved.
//   2. GH_TOKEN, GITHUB_TOKEN   -- used for this run only, never saved: on a
//                                  developer's machine they are usually
//                                  broad-scope tokens.
//   3. the saved token          -- DPAPI-encrypted for this Windows user, or
//                                  in the login Keychain on macOS.
//   4. a hidden prompt, once.
//
// A token is saved only if it is a fine-grained token (github_pat_...). A
// classic or OAuth token -- like the GitHub CLI's login -- can reach every repo
// the account can, so it is used for the one run and then forgotten.
//
// Downloads go through the API's asset URL with Accept: application/octet-stream.
// GitHub answers with a redirect to a pre-signed storage URL, and that request
// must NOT carry the Authorization header (the storage host rejects a request
// with two credentials) -- so redirects are followed by hand, without it.
//
#include <string>
#include <string_view>
#include <vector>

namespace soi {

// --- versions -----------------------------------------------------------------

struct Version {
    int major = 0, minor = 0, patch = 0;
};

// Accepts "1.2.3" and "v1.2.3"; ignores any "-suffix" / "+build".
bool parseVersion(std::string_view text, Version& out);
int  compareVersions(const Version& a, const Version& b);   // <0, 0, >0
std::string versionString(const Version& v);

// --- checksums ----------------------------------------------------------------

// Finds `fileName` in sha256sum-format text ("<64 hex>  name" or "<hex> *name"),
// returning the lowercase hex. Refuses malformed or duplicated entries.
bool findChecksum(const std::string& sumsText, const std::string& fileName, std::string& hexOut);

std::string sha256Hex(const void* data, size_t size);
bool        sha256File(const std::string& path, std::string& hexOut);

// --- tokens -------------------------------------------------------------------

enum class TokenKind { FineGrained, Classic, OAuth, Other };
TokenKind   classifyToken(const std::string& token);
const char* describeTokenKind(TokenKind kind);

#if defined(_WIN32)
// DPAPI (CryptProtectData) bound to the current Windows user, with
// application-specific entropy so another program's DPAPI blob can't be
// passed off as ours. Tampering makes unprotect fail.
bool protectSecret(const std::string& plain, std::string& blob);
bool unprotectSecret(const std::string& blob, std::string& plain);
#else
// A generic password in the user's login Keychain, service "soi-share", under
// `account`. Like a DPAPI blob it is readable by this user's processes and by
// nobody else; unlike a file it is never on disk in clear. See Keychain_mac.cpp.
bool keychainStore(const std::string& account, const std::string& secret);
bool keychainLoad(const std::string& account, std::string& secret);
bool keychainDelete(const std::string& account);   // true if one was deleted
#endif

// Where the saved token lives, for messages.
std::string tokenFilePath();
bool saveToken(const std::string& token);
bool loadSavedToken(std::string& token);
bool forgetSavedToken();                     // true if one was deleted

struct TokenChoice {
    std::string token;
    std::string source;       // "SOI_SHARE_GITHUB_TOKEN", "saved", ...
    bool        saveable = false;
};
TokenChoice tokenFromEnvironmentOrStore();

// Reads a line from the console without echoing it. False if stdin is not an
// interactive console.
bool promptHidden(const char* prompt, std::string& out);

// --- GitHub -------------------------------------------------------------------

struct HttpResponse {
    int         status = 0;          // 0 => no HTTP response at all
    std::string body;
    std::string error;               // transport error text when status == 0
    std::string location;            // Location header of a redirect
};

// One GET. `headers` are complete "Name: value" lines. Redirects are NOT
// followed when `followRedirects` is false.
HttpResponse httpGet(const std::string& url, const std::vector<std::string>& headers,
                     bool followRedirects);

struct ReleaseAsset {
    std::string name;
    std::string apiUrl;
    long long   size = 0;
};

struct Release {
    std::string               tag;
    std::string               htmlUrl;
    std::vector<ReleaseAsset> assets;

    const ReleaseAsset* asset(const std::string& name) const;
};

bool parseRelease(const std::string& json, Release& out, std::string& error);

// GET /repos/<repo>/releases/latest. `token` may be empty.
HttpResponse fetchLatestRelease(const std::string& repo, const std::string& token, Release& out);

// Downloads an asset's bytes via its API URL (see the redirect note above).
HttpResponse downloadAsset(const ReleaseAsset& asset, const std::string& token, std::string& bytes);

} // namespace soi
