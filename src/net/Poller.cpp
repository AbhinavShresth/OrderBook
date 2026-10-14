#include "net/Poller.hpp"
#include <algorithm>

namespace net {

void SystemPoller::add(socket_t sock, bool read, bool write) {
    PollEntry entry{};
    entry.sock = sock;
    entry.want_read = read;
    entry.want_write = write;
    entry.can_read = false;
    entry.can_write = false;
    entries_.push_back(entry);
}

void SystemPoller::modify(socket_t sock, bool read, bool write) {
    for (auto& entry : entries_) {
        if (entry.sock == sock) {
            entry.want_read = read;
            entry.want_write = write;
            return;
        }
    }
}

void SystemPoller::remove(socket_t sock) {
    entries_.erase(
        std::remove_if(entries_.begin(), entries_.end(),
                       [sock](const PollEntry& e) { return e.sock == sock; }),
        entries_.end()
    );
}

int SystemPoller::poll(int timeout_ms) {
    if (entries_.empty()) return 0;

    fd_set read_fds;
    fd_set write_fds;
    FD_ZERO(&read_fds);
    FD_ZERO(&write_fds);

    socket_t max_fd = 0;
    for (auto& e : entries_) {
        e.can_read = false;
        e.can_write = false;
        if (e.want_read) {
            FD_SET(e.sock, &read_fds);
            if (e.sock > max_fd) max_fd = e.sock;
        }
        if (e.want_write) {
            FD_SET(e.sock, &write_fds);
            if (e.sock > max_fd) max_fd = e.sock;
        }
    }

    timeval tv{};
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int res = select(static_cast<int>(max_fd + 1), &read_fds, &write_fds, nullptr, &tv);
    if (res > 0) {
        for (auto& e : entries_) {
            if (e.want_read && FD_ISSET(e.sock, &read_fds)) {
                e.can_read = true;
            }
            if (e.want_write && FD_ISSET(e.sock, &write_fds)) {
                e.can_write = true;
            }
        }
    }
    return res;
}

bool SystemPoller::isReadable(socket_t sock) const {
    for (const auto& e : entries_) {
        if (e.sock == sock) {
            return e.can_read;
        }
    }
    return false;
}

bool SystemPoller::isWritable(socket_t sock) const {
    for (const auto& e : entries_) {
        if (e.sock == sock) {
            return e.can_write;
        }
    }
    return false;
}

} // namespace net
