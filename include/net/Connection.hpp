#pragma once

#include "net/Poller.hpp"
#include "net/FrameReader.hpp"
#include <vector>
#include <chrono>
#include <cstdint>

namespace net {

struct Connection {
    socket_t sock{INVALID_SOCKET};
    uint32_t conn_id{0};
    uint64_t bound_client_id{0};
    uint16_t callback_port{0};
    std::string peer_ip;
    uint16_t peer_port{0};

    FrameReader frame_reader;
    std::vector<uint8_t> write_buf;

    std::chrono::steady_clock::time_point last_activity;
    bool is_draining{false};
};

struct ClientRecord {
    uint64_t client_id{0};
    uint32_t current_conn_id{0};
    std::string peer_ip;
    uint16_t callback_port{0};
    uint16_t udp_port{0};
    uint32_t subscribed_mask{0};
    uint32_t resting_count{0};

    // Bounded push mailbox (cap 1000, drop oldest on overflow) (Section 3.5)
    std::vector<std::vector<uint8_t>> mailbox;
};

} // namespace net
