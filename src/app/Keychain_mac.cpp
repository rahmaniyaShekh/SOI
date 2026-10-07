// The saved GitHub token on macOS: a generic password in the login Keychain.
//
// Access control, and why it is set the way it is
// -------------------------------------------------
// By default an item trusts only the binary that created it, identified by its
// code signature. soi-share is ad-hoc signed and `update` replaces the binary,
// so the next `update` would be a "different application" and macOS would put
// up a password dialog -- in the middle of a terminal program, possibly with
// nobody watching. The item is therefore created readable by any application
// of THIS user without a prompt, which is exactly the guarantee DPAPI gives on
// Windows: the user's own processes can read it, other accounts cannot, and it
// is never on disk in clear. The legacy SecAccess API is the only way to say
// that; it is deprecated but fully supported for login-keychain items.
#include "app/Update.h"
#include "util/Log.h"

#include <Security/Security.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"

namespace soi {
namespace {

constexpr char kService[] = "soi-share";

struct CfRelease {
    CFTypeRef ref;
    explicit CfRelease(CFTypeRef r) : ref(r) {}
    ~CfRelease() { if (ref) CFRelease(ref); }
    CfRelease(const CfRelease&) = delete;
    CfRelease& operator=(const CfRelease&) = delete;
};

CFStringRef cfString(const std::string& s) {
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8*>(s.data()),
                                   static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
}

CFMutableDictionaryRef baseQuery(const std::string& account) {
    CFMutableDictionaryRef q = CFDictionaryCreateMutable(
        nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFStringRef service = cfString(kService);
    CFStringRef acct    = cfString(account);
    CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(q, kSecAttrService, service);
    CFDictionarySetValue(q, kSecAttrAccount, acct);
    CFRelease(service);
    CFRelease(acct);
    return q;
}

// An access object whose decrypt ACL admits any application of this user,
// without a confirmation prompt. Null if it cannot be built, in which case the
// item gets the default (creator-only) access instead.
SecAccessRef anyApplicationAccess() {
    SecAccessRef access = nullptr;
    if (SecAccessCreate(CFSTR("soi-share GitHub token"), nullptr, &access) != errSecSuccess)
        return nullptr;
    CFArrayRef acls = SecAccessCopyMatchingACLList(access, kSecACLAuthorizationDecrypt);
    if (!acls) { CFRelease(access); return nullptr; }
    bool ok = CFArrayGetCount(acls) > 0;
    for (CFIndex i = 0; i < CFArrayGetCount(acls); ++i) {
        auto acl = static_cast<SecACLRef>(const_cast<void*>(CFArrayGetValueAtIndex(acls, i)));
        // A null application list means "any application"; 0 means no prompt.
        if (SecACLSetContents(acl, nullptr, CFSTR("soi-share GitHub token"), 0) != errSecSuccess)
            ok = false;
    }
    CFRelease(acls);
    if (!ok) { CFRelease(access); return nullptr; }
    return access;
}

} // namespace

bool keychainStore(const std::string& account, const std::string& secret) {
    // Replace rather than update: SecItemUpdate keeps the old item's ACL, and
    // an item written by an older build should not keep an older ACL.
    keychainDelete(account);

    CFMutableDictionaryRef add = baseQuery(account);
    CfRelease addGuard(add);
    CFDataRef data = CFDataCreate(nullptr, reinterpret_cast<const UInt8*>(secret.data()),
                                  static_cast<CFIndex>(secret.size()));
    CfRelease dataGuard(data);
    CFDictionarySetValue(add, kSecValueData, data);
    CFDictionarySetValue(add, kSecAttrLabel, CFSTR("soi-share GitHub token"));

    SecAccessRef access = anyApplicationAccess();
    CfRelease accessGuard(access);
    if (access) CFDictionarySetValue(add, kSecAttrAccess, access);

    OSStatus st = SecItemAdd(add, nullptr);
    if (st != errSecSuccess && access) {
        // Some keychains refuse a custom ACL; the default one still works.
        CFDictionaryRemoveValue(add, kSecAttrAccess);
        st = SecItemAdd(add, nullptr);
    }
    if (st != errSecSuccess) logT("keychain: could not store the token ({})", static_cast<int>(st));
    return st == errSecSuccess;
}

bool keychainLoad(const std::string& account, std::string& secret) {
    secret.clear();
    CFMutableDictionaryRef q = baseQuery(account);
    CfRelease guard(q);
    CFDictionarySetValue(q, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(q, kSecMatchLimit, kSecMatchLimitOne);
    // Never put up a dialog: in a terminal program nobody may be there to
    // answer it. A refusal reads as "no saved token" and the user is asked
    // for one at the prompt instead.
    CFDictionarySetValue(q, kSecUseAuthenticationUI, kSecUseAuthenticationUIFail);

    CFTypeRef result = nullptr;
    const OSStatus st = SecItemCopyMatching(q, &result);
    CfRelease resultGuard(result);
    if (st != errSecSuccess || !result || CFGetTypeID(result) != CFDataGetTypeID()) return false;
    auto data = static_cast<CFDataRef>(result);
    secret.assign(reinterpret_cast<const char*>(CFDataGetBytePtr(data)),
                  static_cast<size_t>(CFDataGetLength(data)));
    return true;
}

bool keychainDelete(const std::string& account) {
    CFMutableDictionaryRef q = baseQuery(account);
    CfRelease guard(q);
    return SecItemDelete(q) == errSecSuccess;
}

} // namespace soi
