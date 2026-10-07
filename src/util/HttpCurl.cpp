#include "util/HttpCurl.h"
#include "util/Platform.h"

#include <curl/curl.h>

#if defined(__APPLE__)
  #include <CFNetwork/CFNetwork.h>
  #include <CoreFoundation/CoreFoundation.h>
#endif

namespace soi {
namespace {

struct Sink {
    std::string* body;
    size_t       maxBytes;
    bool         overflow = false;
};

size_t onBody(char* data, size_t size, size_t count, void* user) {
    auto* sink = static_cast<Sink*>(user);
    const size_t n = size * count;
    if (sink->body->size() + n > sink->maxBytes) {
        sink->overflow = true;
        return 0;   // aborts the transfer
    }
    sink->body->append(data, n);
    return n;
}

size_t onHeader(char* data, size_t size, size_t count, void* user) {
    auto* location = static_cast<std::string*>(user);
    const size_t n = size * count;
    const std::string line(data, n);
    if (line.size() > 9 && equalsNoCase(std::string_view(line).substr(0, 9), "location:")) {
        std::string v = line.substr(9);
        while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' ')) v.pop_back();
        *location = v;
    }
    return n;
}

bool curlReady() {
    static const bool ok = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    return ok;
}

std::string hostOf(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    const size_t start = scheme + 3;
    const size_t end = url.find_first_of("/:?#", start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

#if defined(__APPLE__)
std::string cfString(CFStringRef s) {
    if (!s) return {};
    char buf[1024];
    return CFStringGetCString(s, buf, sizeof buf, kCFStringEncodingUTF8) ? std::string(buf)
                                                                         : std::string();
}

// The HTTPS proxy from System Settings > Network, unless this host is on its
// bypass list. Automatic configuration (PAC/WPAD) is not evaluated: the
// request goes direct in that case, as most such networks still allow it.
std::string systemProxyFor(const std::string& url) {
    CFDictionaryRef settings = CFNetworkCopySystemProxySettings();
    if (!settings) return {};

    const bool https = url.rfind("https://", 0) == 0;
    auto number = [&](CFStringRef key) {
        int v = 0;
        if (auto n = static_cast<CFNumberRef>(CFDictionaryGetValue(settings, key)))
            CFNumberGetValue(n, kCFNumberIntType, &v);
        return v;
    };
    std::string proxy;
    const CFStringRef enableKey = https ? kCFNetworkProxiesHTTPSEnable : kCFNetworkProxiesHTTPEnable;
    const CFStringRef hostKey   = https ? kCFNetworkProxiesHTTPSProxy  : kCFNetworkProxiesHTTPProxy;
    const CFStringRef portKey   = https ? kCFNetworkProxiesHTTPSPort   : kCFNetworkProxiesHTTPPort;
    if (number(enableKey)) {
        const std::string host = cfString(static_cast<CFStringRef>(CFDictionaryGetValue(settings, hostKey)));
        const int port = number(portKey);
        if (!host.empty()) proxy = "http://" + host + ":" + std::to_string(port ? port : 8080);
    }

    if (!proxy.empty()) {
        const std::string target = hostOf(url);
        if (auto list = static_cast<CFArrayRef>(
                CFDictionaryGetValue(settings, kCFNetworkProxiesExceptionsList))) {
            for (CFIndex i = 0; i < CFArrayGetCount(list); ++i) {
                std::string ex = cfString(static_cast<CFStringRef>(CFArrayGetValueAtIndex(list, i)));
                if (ex.rfind("*.", 0) == 0) ex.erase(0, 1);   // "*.corp" -> ".corp"
                const bool suffix = !ex.empty() && ex[0] == '.';
                if ((!suffix && equalsNoCase(target, ex)) ||
                    (suffix && target.size() > ex.size() &&
                     equalsNoCase(std::string_view(target).substr(target.size() - ex.size()), ex))) {
                    proxy.clear();
                    break;
                }
            }
        }
    }
    CFRelease(settings);
    return proxy;
}
#endif

} // namespace

CurlResponse curlPerform(const CurlRequest& req) {
    CurlResponse out;
    if (!curlReady()) { out.error = "libcurl could not be initialised"; return out; }

    CURL* curl = curl_easy_init();
    if (!curl) { out.error = "libcurl could not create a handle"; return out; }

    Sink sink{&out.body, req.maxBytes};
    curl_slist* headers = nullptr;
    for (const auto& h : req.headers) headers = curl_slist_append(headers, h.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, req.url.c_str());
    curl_easy_setopt(curl, CURLOPT_USERAGENT, req.userAgent.c_str());
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);   // safe off the main thread
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, static_cast<long>(req.connectTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(req.totalTimeoutMs));
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, req.followRedirects ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https,http");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, onBody);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, onHeader);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &out.location);
    if (headers) curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    if (req.method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
    } else if (req.method != "GET") {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, req.method.c_str());
        if (!req.body.empty()) {
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, req.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(req.body.size()));
        }
    }

#if defined(__APPLE__)
    // Environment variables win, exactly as curl itself would treat them.
    if (envVar("https_proxy").empty() && envVar("HTTPS_PROXY").empty() &&
        envVar("http_proxy").empty() && envVar("all_proxy").empty() &&
        envVar("ALL_PROXY").empty()) {
        const std::string proxy = systemProxyFor(req.url);
        if (!proxy.empty()) curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str());
    }
#endif

    char errbuf[CURL_ERROR_SIZE] = {};
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errbuf);

    const CURLcode rc = curl_easy_perform(curl);
    if (rc == CURLE_OK) {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        out.status      = static_cast<int>(status);
        out.transportOk = true;
    } else if (sink.overflow) {
        out.error = "response too large";
    } else {
        out.error = errbuf[0] ? errbuf : curl_easy_strerror(rc);
    }

    if (headers) curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return out;
}

} // namespace soi
