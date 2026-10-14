#pragma once

#include "core/SpscQueue.hpp"
#include "core/WorkItem.hpp"
#include "wire/Messages.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <atomic>
#include <deque>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#endif

namespace core {

class Worker {
public:
    Worker(uint8_t worker_id,
           uint32_t group_id,
           const std::string& group_name,
           const std::vector<std::pair<uint16_t, std::string>>& tickers,
           size_t queue_capacity = 8192);

    ~Worker();

    uint8_t worker_id() const { return worker_id_; }
    uint32_t group_id() const { return group_id_; }
    const std::string& group_name() const { return group_name_; }
    const std::vector<std::pair<uint16_t, std::string>>& tickers() const { return tickers_; }

    SpscQueue<WorkItem>& in_queue() { return in_queue_; }
    SpscQueue<WorkResult>& out_queue() { return out_queue_; }

    // Run one iteration of the worker loop (Section 3.3)
    void step();

    // Direct UDP socket binding and subscriber management
    bool initUdpSocket(uint16_t bind_port = 0);
    uint16_t udp_port() const { return udp_port_; }

private:
    uint64_t nextOrderId() {
        // D9: order_id = (worker_id << 56) | local_counter
        uint64_t counter = ++local_order_counter_;
        return (static_cast<uint64_t>(worker_id_) << 56) | (counter & 0x00FFFFFFFFFFFFFFULL);
    }

    void handleOrder(const WorkItem& item);
    void handleCancel(const WorkItem& item);
    void handleSubscribe(const WorkItem& item);
    void handleUnsubscribe(const WorkItem& item);
    void emitResult(const WorkResult& result);
    void publishSnapshots();

    uint8_t worker_id_;
    uint32_t group_id_;
    std::string group_name_;
    std::vector<std::pair<uint16_t, std::string>> tickers_;

    SpscQueue<WorkItem> in_queue_;
    SpscQueue<WorkResult> out_queue_;

    uint64_t local_order_counter_{0};
    uint64_t seq_counter_{0};
    uint64_t next_priority_{0};
    uint64_t next_trade_id_{0};
    std::deque<WorkResult> pending_results_;

    // Worker order book: order_id -> RestingOrder
    struct RestingOrder {
        uint16_t symbol_id{0};
        bool is_bid{true};
        wire::Price price{0};
        wire::Volume vol{0};
        uint64_t priority{0};
    };
    std::unordered_map<uint64_t, RestingOrder> resting_orders_;

    // Subscribers: endpoint hash -> (sockaddr_in, group_mask)
    struct EndpointHash {
        size_t operator()(const uint64_t& key) const { return std::hash<uint64_t>()(key); }
    };
    std::unordered_map<uint64_t, std::pair<sockaddr_in, uint32_t>> subscribers_;

    // UDP broadcast
#if defined(_WIN32)
    SOCKET udp_sock_{INVALID_SOCKET};
#else
    int udp_sock_{-1};
#endif
    uint16_t udp_port_{0};

    // Adaptive yielding / snapshot interval tracking (EWMA)
    std::chrono::steady_clock::time_point last_snapshot_time_;
    double t_udp_ewma_us_{100.0};
    double dt_safety_us_{2000000.0}; // 2.0s interval for periodic heartbeat snapshots
};

} // namespace core
