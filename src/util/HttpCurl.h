#pragma once
//
// One HTTP(S) request, synchronously, through libcurl -- the macOS (and any
// POSIX) counterpart of the WinHTTP calls in app/Rendezvous.cpp and
// app/Update.cpp.
//
// libcurl ships with every version of macOS (/usr/lib/libcurl.4.dylib), uses
// the system trust store, and so adds nothing to install. Proxies: the
// standard https_proxy / http_proxy / no_proxy variables when set, otherwise
// the HTTP(S) proxy configured in System Settings > Network.
//
#include <cstddef>
#include <string>
#include <vector>

namespace soi {

struct CurlResponse {
    bool        transportOk = false;   // a response arrived, whatever its status
    int         status = 0;
    std::string body;
    std::string error;                 // transport error text when !transportOk
    std::string location;              // Location header of a redirect
};

struct CurlRequest {
    std::string              url;
    std::string              method = "GET";
    std::vector<std::string> headers;      // complete "Name: value" lines
    std::string              body;
    int                      connectTimeoutMs = 5000;
    int                      totalTimeoutMs   = 15000;
    bool                     followRedirects  = false;
    size_t                   maxBytes         = 256 * 1024;
    std::string              userAgent        = "soi-share";
};

CurlResponse curlPerform(const CurlRequest& request);

} // namespace soi
