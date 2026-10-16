#include "heddle/osc_io.hpp"

// Portable UDP: Winsock2 on Windows, BSD sockets elsewhere.
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  define SOCKET_TYPE SOCKET
#  define SOCKET_VALID(s) ((s) != INVALID_SOCKET)
#  define SOCKET_CLOSE(s) (closesocket(s))
#  define SOCKET_ERRNO (WSAGetLastError())
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <netdb.h>
#  include <unistd.h>
#  include <fcntl.h>
#  include <errno.h>
#  define SOCKET_TYPE int
#  define SOCKET_VALID(s) ((s) >= 0)
#  define SOCKET_CLOSE(s) (::close(s))
#  define SOCKET_ERRNO (errno)
#endif

#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#include <windows.h>
#endif

namespace heddle {

namespace {
#ifdef _WIN32
struct WsaGuard {
    WSADATA data{};
    bool owned = false;
    WsaGuard() { owned = (WSAStartup(MAKEWORD(2, 2), &data) == 0); }
    ~WsaGuard() { if (owned) WSACleanup(); }
};
WsaGuard& wsa() {
    static WsaGuard g;
    return g;
}
#endif
}  // namespace

struct OscUdp::Impl {
    SOCKET_TYPE sock = -1;
    struct sockaddr_in remote{};
    bool haveRemote = false;
    struct sockaddr_in sender{};
    bool haveSender = false;
    Impl() {
#ifdef _WIN32
        (void)wsa();
        sock = INVALID_SOCKET;
#else
        sock = -1;
#endif
    }
};

OscUdp::OscUdp() : impl_(new Impl()) {}
OscUdp::~OscUdp() {
    close();
    delete impl_;
}

void OscUdp::close() {
    if (impl_ && SOCKET_VALID(impl_->sock)) {
        SOCKET_CLOSE(impl_->sock);
#ifdef _WIN32
        impl_->sock = INVALID_SOCKET;
#else
        impl_->sock = -1;
#endif
    }
    impl_->haveSender = false;
    localPort_ = 0;
}

// Resolve a host to an in_addr. IP literals go through inet_pton; otherwise
// use gethostbyname (legacy, but portable without pulling a resolver lib).
static bool resolveAddr(const std::string& host, struct in_addr& out) {
    if (host.empty()) return false;
    if (::inet_pton(AF_INET, host.c_str(), &out) == 1) return true;
    struct hostent* he = ::gethostbyname(host.c_str());
    if (!he || !he->h_addr_list[0]) return false;
    out = *(struct in_addr*)he->h_addr_list[0];
    return true;
}

bool OscUdp::bind(const std::string& host, int port, std::string* err) {
    close();
    SOCKET_TYPE fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (!SOCKET_VALID(fd)) {
        if (err) *err = "socket() failed";
        return false;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const char*)&one, sizeof one);
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (host.empty() || host == "0.0.0.0" || host == "::")
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    else if (!resolveAddr(host, addr.sin_addr)) {
        if (err) *err = "unknown host '" + host + "'";
        SOCKET_CLOSE(fd);
        return false;
    }
    if (::bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0) {
        if (err) *err = "bind() failed for " + host + ":" + std::to_string(port);
        SOCKET_CLOSE(fd);
        return false;
    }
    struct sockaddr_in got{};
    socklen_t len = sizeof(got);
    ::getsockname(fd, (struct sockaddr*)&got, &len);
    impl_->sock = fd;
    localPort_ = ntohs(got.sin_port);
    return true;
}

bool OscUdp::openOut(const std::string& host, int port, std::string* err) {
    if (!SOCKET_VALID(impl_->sock)) {
        if (err) *err = "call bind() first";
        return false;
    }
    struct sockaddr_in r{};
    r.sin_family = AF_INET;
    r.sin_port = htons(uint16_t(port));
    if (!resolveAddr(host, r.sin_addr)) {
        if (err) *err = "unknown remote host '" + host + "'";
        return false;
    }
    impl_->remote = r;
    impl_->haveRemote = true;
    return true;
}

bool OscUdp::send(const OscMsg& msg) {
    if (!SOCKET_VALID(impl_->sock)) return false;
    const std::string data = oscEncode(msg);
    struct sockaddr_in to{};
    if (impl_->haveRemote) {
        to = impl_->remote;
    } else {
        // Loopback fallback: send to self's port (tests / local-only).
        to.sin_family = AF_INET;
        to.sin_port = htons(uint16_t(localPort_));
        to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    }
    int n = ::sendto(impl_->sock, data.data(), (int)data.size(), 0,
                     (struct sockaddr*)&to, sizeof to);
    return n == (int)data.size();
}

bool OscUdp::poll(OscMsg& out, SourceAddr* src) {
    if (!SOCKET_VALID(impl_->sock)) return false;
    // Make the socket non-blocking for a zero-wait poll.
#ifdef _WIN32
    u_long nb = 1;
    ::ioctlsocket(impl_->sock, FIONBIO, &nb);
#else
    int flags = ::fcntl(impl_->sock, F_GETFL, 0);
    ::fcntl(impl_->sock, F_SETFL, flags | O_NONBLOCK);
#endif
    char buf[4096];
    struct sockaddr_in from{};
    socklen_t flen = sizeof(from);
    int n = ::recvfrom(impl_->sock, buf, (int)sizeof buf, 0,
                       (struct sockaddr*)&from, &flen);
    if (n <= 0) return false;
    impl_->sender = from;
    impl_->haveSender = true;
    if (src != nullptr) {
        src->port = ntohs(from.sin_port);
        char ipbuf[INET_ADDRSTRLEN] = {0};
#ifdef _WIN32
        (void)inet_ntop(AF_INET, &from.sin_addr, ipbuf, sizeof(ipbuf));
#else
        (void)inet_ntop(AF_INET, &from.sin_addr, ipbuf, (socklen_t)sizeof(ipbuf));
#endif
        src->ip = ipbuf;
    }
    return oscDecode(buf, size_t(n), out);
}

bool OscUdp::sendTo(const OscMsg& msg, const SourceAddr& dest) {
    if (!SOCKET_VALID(impl_->sock)) return false;
    const std::string data = oscEncode(msg);
    struct sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(uint16_t(dest.port));
    if (!resolveAddr(dest.ip, to.sin_addr)) return false;
    int n = ::sendto(impl_->sock, data.data(), (int)data.size(), 0,
                     (struct sockaddr*)&to, sizeof to);
    return n == (int)data.size();
}

}  // namespace heddle
