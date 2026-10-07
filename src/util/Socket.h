#pragma once
//
// Winsock and BSD sockets differ in names more than in behaviour. This maps the
// Winsock spellings the control channel and the handover page were written
// against onto POSIX, so both platforms share one implementation of each.
//
#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>

namespace soi {
using SockLen = int;

inline bool socketsReady() {
    static const bool ok = [] {
        WSADATA wsa{};
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
}

inline int lastSocketError() { return WSAGetLastError(); }

inline void setSocketTimeouts(SOCKET s, int ms) {
    const DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}

inline void setRecvTimeout(SOCKET s, int ms) {
    const DWORD t = static_cast<DWORD>(ms);
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&t), sizeof t);
}

// 1 when `s` is readable (for a listener: a connection is waiting), 0 on
// timeout, -1 when the socket is gone.
inline int waitReadable(SOCKET s, int ms) {
    WSAPOLLFD p{};
    p.fd     = s;
    p.events = POLLRDNORM;
    const int rc = WSAPoll(&p, 1, ms);
    if (rc < 0) return -1;
    if (rc == 0) return 0;
    return (p.revents & (POLLERR | POLLNVAL)) ? -1 : 1;
}
} // namespace soi

#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <poll.h>
  #include <sys/socket.h>
  #include <sys/time.h>
  #include <unistd.h>

  #include <cerrno>
  #include <csignal>

using SOCKET = int;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int    SOCKET_ERROR   = -1;
constexpr int    SD_SEND        = SHUT_WR;
inline int closesocket(SOCKET s) { return ::close(s); }

namespace soi {
using SockLen = socklen_t;

// A peer that hangs up mid-reply must cost a failed send(), not a SIGPIPE that
// kills the whole process.
inline bool socketsReady() {
    static const bool ok = [] {
        std::signal(SIGPIPE, SIG_IGN);
        return true;
    }();
    return ok;
}

inline int lastSocketError() { return errno; }

inline void setSocketTimeouts(SOCKET s, int ms) {
    timeval tv{ms / 1000, static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000)};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

inline void setRecvTimeout(SOCKET s, int ms) {
    timeval tv{ms / 1000, static_cast<decltype(tv.tv_usec)>((ms % 1000) * 1000)};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

// 1 when `s` is readable (for a listener: a connection is waiting), 0 on
// timeout, -1 when the socket is gone. Listeners are polled rather than left
// blocked in accept(), because closing a socket from another thread does not
// wake an accept() on macOS the way closesocket() does on Windows.
inline int waitReadable(SOCKET s, int ms) {
    pollfd p{};
    p.fd     = s;
    p.events = POLLIN;
    const int rc = ::poll(&p, 1, ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (rc == 0) return 0;
    return (p.revents & (POLLERR | POLLNVAL)) ? -1 : 1;
}
} // namespace soi
#endif
