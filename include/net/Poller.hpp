#pragma once

#include <vector>
#include <cstdint>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
using socket_t = int;
constexpr socket_t INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif

namespace net {

inline bool initNetworking() {
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#else
    return true;
#endif
}

inline void cleanupNetworking() {
#if defined(_WIN32)
    WSACleanup();
#endif
}

inline bool setNonBlocking(socket_t sock) {
#if defined(_WIN32)
    u_long mode = 1;
    return ioctlsocket(sock, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(sock, F_SETFL, flags | O_NONBLOCK) == 0;
#endif
}

inline void closeSocket(socket_t sock) {
#if defined(_WIN32)
    if (sock != INVALID_SOCKET) closesocket(sock);
#else
    if (sock >= 0) close(sock);
#endif
}

// Swappable Poller interface (D18, Section 3.2)
class Poller {
public:
    virtual ~Poller() = default;
    virtual void add(socket_t sock, bool read, bool write) = 0;
    virtual void modify(socket_t sock, bool read, bool write) = 0;
    virtual void remove(socket_t sock) = 0;
    virtual int poll(int timeout_ms) = 0;
    virtual bool isReadable(socket_t sock) const = 0;
    virtual bool isWritable(socket_t sock) const = 0;
};

// Platform-independent Poller backend using select()
class SystemPoller : public Poller {
public:
    void add(socket_t sock, bool read, bool write) override;
    void modify(socket_t sock, bool read, bool write) override;
    void remove(socket_t sock) override;
    int poll(int timeout_ms) override;
    bool isReadable(socket_t sock) const override;
    bool isWritable(socket_t sock) const override;

private:
    struct PollEntry {
        socket_t sock;
        bool want_read;
        bool want_write;
        bool can_read;
        bool can_write;
    };
    std::vector<PollEntry> entries_;
};

} // namespace net
