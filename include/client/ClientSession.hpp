#pragma once

#include "net/Poller.hpp"
#include "net/FrameReader.hpp"
#include "wire/Messages.hpp"
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <utility>
#include <atomic>
#if defined(_WIN32)
#include "mingw_thread.h"
#else
#include <thread>
#endif

namespace client {

class ClientSession {
public:
    ClientSession(const std::string& host, uint16_t port, uint16_t callback_port = 0);
    ~ClientSession();

    bool connect();
    void disconnect();
    bool isConnected() const;

    uint64_t clientId() const { return client_id_; }
    void setOrderUpdateCallback(std::function<void(const wire::OrderAck&)> callback) {
        order_update_callback_ = std::move(callback);
    }

    // Synchronous request APIs
    bool listFeed(wire::ListFeedResponse& out_rsp);
    bool placeBid(uint16_t symbol_id, wire::Price price, wire::Volume vol, wire::OrderAck& out_ack, wire::ErrorCode& out_err);
    bool placeAsk(uint16_t symbol_id, wire::Price price, wire::Volume vol, wire::OrderAck& out_ack, wire::ErrorCode& out_err);
    bool cancelOrder(uint64_t order_id, wire::CancelAck& out_ack, wire::ErrorCode& out_err);
    bool subscribe(uint16_t udp_port, uint32_t group_mask, uint32_t& out_subscribed_mask);
    bool unsubscribe(uint32_t group_mask, uint32_t& out_unsubscribed_mask);

private:
    bool sendFrame(wire::MsgType type, const uint8_t* payload, size_t len, uint32_t req_id);
    bool prepareConnection();
    bool receiveFrame(net::Frame& out_frame);
    bool receiveResponse(uint32_t req_id, wire::MsgType expected, net::Frame& out_frame);
    void dispatchOrderUpdate(const net::Frame& frame);
    void ensureConnected();

    std::string host_;
    uint16_t port_;
    uint16_t callback_port_;
    uint64_t client_id_{0};
    uint32_t next_req_id_{1};
    bool network_initialized_{false};

    socket_t sock_{INVALID_SOCKET};
    net::FrameReader frame_reader_;
    std::function<void(const wire::OrderAck&)> order_update_callback_;
};

// UDP Broadcast listener with sequence gap loss detection (FR-11, NFR-4)
class UdpListener {
public:
    using OnQuoteCallback = std::function<void(const wire::UdpMarketFeedDatagram&, bool loss_detected, uint64_t dropped)>;

    UdpListener(uint16_t listen_port, OnQuoteCallback callback);
    ~UdpListener();

    bool start();
    void stop();
    uint16_t port() const { return port_; }

    uint64_t packetsReceived() const { return packets_received_; }
    uint64_t packetsLost() const { return packets_lost_; }

private:
    void listenLoop();

    uint16_t port_;
    OnQuoteCallback callback_;
    socket_t sock_{INVALID_SOCKET};
    std::atomic<bool> running_{false};
    std::thread thread_;

    std::unordered_map<uint32_t, uint64_t> expected_seq_;
    std::atomic<uint64_t> packets_received_{0};
    std::atomic<uint64_t> packets_lost_{0};
};

} // namespace client
