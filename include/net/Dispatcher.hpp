#pragma once

#include "net/Poller.hpp"
#include "net/Connection.hpp"
#include "core/Worker.hpp"
#include <memory>
#include <unordered_map>
#include <deque>
#include <random>

namespace net {

enum class IdlePolicy {
    NEVER,
    FIXED
};

class Dispatcher {
public:
    Dispatcher(const std::string& host,
               uint16_t tcp_port,
               std::vector<std::unique_ptr<core::Worker>>& workers,
               IdlePolicy idle_policy = IdlePolicy::FIXED,
               int idle_timeout_sec = 1);

    ~Dispatcher();

    bool start();
    void stop();

    // Runs a single non-blocking iteration of the event loop (Section 3.2)
    void step();

    uint16_t port() const { return tcp_port_; }
    size_t activeConnections() const { return connections_.size(); }
    size_t registeredClients() const { return client_registry_.size(); }

private:
    uint64_t generateRandomClientId();

    void acceptConnections();
    void readFromConnection(Connection& conn);
    void processClientFrame(Connection& conn, const Frame& frame);

    void drainWorkerOutQueues();
    void flushWriteBuffers();
    void checkIdleDeadlines();

    void closeConnection(uint32_t conn_id);
    void sendResponse(Connection& conn, const wire::FrameHeader& hdr, const uint8_t* payload, size_t len);
    void sendErrorResponse(Connection& conn, uint32_t req_id, uint64_t client_id, wire::ErrorCode code);

    std::string host_;
    uint16_t tcp_port_;
    std::vector<std::unique_ptr<core::Worker>>& workers_;
    IdlePolicy idle_policy_;
    int idle_timeout_sec_;

    bool running_{false};
    socket_t listen_sock_{INVALID_SOCKET};
    std::unique_ptr<Poller> poller_;

    uint32_t next_conn_id_{1};
    std::unordered_map<uint32_t, Connection> connections_;
    std::unordered_map<socket_t, uint32_t> sock_to_conn_id_;

    // Client registry keyed by client_id (D11, D12, Section 3.1)
    std::unordered_map<uint64_t, ClientRecord> client_registry_;

    // Dispatcher routing tables
    std::unordered_map<uint64_t, uint64_t> order_to_client_; // order_id -> client_id
    std::unordered_map<uint16_t, core::Worker*> symbol_to_worker_; // symbol_id -> Worker*

    std::mt19937_64 rng_;
};

} // namespace net
