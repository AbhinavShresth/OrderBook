#include "core/Worker.hpp"
#include "codec/BinaryCodec.hpp"
#include <chrono>
#include <cstring>
#include <map>
#include <algorithm>
#if !defined(_WIN32)
#include <unistd.h>
#include <fcntl.h>
#endif

namespace core {

static uint64_t endpointKey(const sockaddr_in& sin) {
    uint64_t ip = sin.sin_addr.s_addr;
    uint64_t port = sin.sin_port;
    return (ip << 32) | port;
}

Worker::Worker(uint8_t worker_id,
               uint32_t group_id,
               const std::string& group_name,
               const std::vector<std::pair<uint16_t, std::string>>& tickers,
               size_t queue_capacity)
    : worker_id_(worker_id),
      group_id_(group_id),
      group_name_(group_name),
      tickers_(tickers),
      in_queue_(queue_capacity),
      out_queue_(queue_capacity),
      last_snapshot_time_(std::chrono::steady_clock::now()) {}

Worker::~Worker() {
#if defined(_WIN32)
    if (udp_sock_ != INVALID_SOCKET) {
        closesocket(udp_sock_);
    }
#else
    if (udp_sock_ >= 0) {
        close(udp_sock_);
    }
#endif
}

bool Worker::initUdpSocket(uint16_t bind_port) {
#if defined(_WIN32)
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udp_sock_ == INVALID_SOCKET) return false;
    BOOL reuse = TRUE;
    setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
    u_long nonblock = 1;
    ioctlsocket(udp_sock_, FIONBIO, &nonblock);
#else
    udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock_ < 0) return false;
    int reuse = 1;
    setsockopt(udp_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    fcntl(udp_sock_, F_SETFL, O_NONBLOCK);
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(bind_port);

    if (bind(udp_sock_, (sockaddr*)&addr, sizeof(addr)) != 0) {
        // Fallback to ephemeral port 0
        addr.sin_port = htons(0);
        bind(udp_sock_, (sockaddr*)&addr, sizeof(addr));
    }

#if defined(_WIN32)
    int len = sizeof(addr);
#else
    socklen_t len = sizeof(addr);
#endif
    getsockname(udp_sock_, (sockaddr*)&addr, &len);
    udp_port_ = ntohs(addr.sin_port);
    return true;
}

void Worker::step() {
    while (!pending_results_.empty()) {
        if (!out_queue_.push(pending_results_.front())) return;
        pending_results_.pop_front();
    }
    WorkItem item;
    // Process available work items
    size_t processed = 0;
    while (processed < 128 && in_queue_.pop(item)) {
        processed++;
        switch (item.item_type) {
            case WorkItemType::ORDER:
                handleOrder(item);
                break;
            case WorkItemType::CANCEL:
                handleCancel(item);
                break;
            case WorkItemType::SUBSCRIBE_CONTROL:
                handleSubscribe(item);
                break;
            case WorkItemType::UNSUBSCRIBE_CONTROL:
                handleUnsubscribe(item);
                break;
        }
    }

    // Adaptive yielding / snapshot interval check (Section 3.3, FR-9)
    auto now = std::chrono::steady_clock::now();
    double elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(now - last_snapshot_time_).count();
    double timeout_us = t_udp_ewma_us_ + dt_safety_us_;

    if (elapsed_us >= timeout_us) {
        publishSnapshots();
        last_snapshot_time_ = std::chrono::steady_clock::now();
    }
}

void Worker::emitResult(const WorkResult& result) {
    if (!pending_results_.empty() || !out_queue_.push(result))
        pending_results_.push_back(result);
}

void Worker::handleOrder(const WorkItem& item) {
    WorkResult res;
    res.client_id = item.client_id;
    res.req_id = item.req_id;
    res.type = wire::MsgType::ORDER_ACK;

    // Validate symbol exists in this worker
    bool symbol_found = false;
    for (const auto& p : tickers_) {
        if (p.first == item.symbol_id) {
            symbol_found = true;
            break;
        }
    }

    if (!symbol_found) {
        res.is_error = true;
        res.error_code = wire::ErrorCode::UNKNOWN_SYMBOL;
        emitResult(res);
        return;
    }

    const uint64_t oid = nextOrderId();
    const bool is_bid = item.type == wire::MsgType::BID;
    wire::Volume remaining = item.volume;
    std::vector<std::pair<uint64_t, RestingOrder*>> eligible;
    for (auto& entry : resting_orders_) {
        auto& resting = entry.second;
        if (resting.symbol_id != item.symbol_id || resting.is_bid == is_bid) continue;
        if ((is_bid && resting.price <= item.price) || (!is_bid && resting.price >= item.price))
            eligible.emplace_back(entry.first, &resting);
    }
    std::sort(eligible.begin(), eligible.end(), [is_bid](const auto& a, const auto& b) {
        if (a.second->price != b.second->price)
            return is_bid ? a.second->price < b.second->price : a.second->price > b.second->price;
        return a.second->priority < b.second->priority;
    });

    res.order_ack.order_id = oid;
    res.order_ack.trades.clear();
    size_t first = 0;
    while (remaining && first < eligible.size()) {
        size_t last = first + 1;
        const wire::Price price = eligible[first].second->price;
        while (last < eligible.size() && eligible[last].second->price == price) ++last;
        wire::Volume level_total = 0;
        for (size_t i = first; i < last; ++i) level_total += eligible[i].second->vol;
        const wire::Volume at_level = std::min(remaining, level_total);
        std::vector<wire::Volume> shares(last - first, 0);
        if (at_level == level_total) {
            for (size_t i = first; i < last; ++i) shares[i - first] = eligible[i].second->vol;
        } else {
            wire::Volume allocated = 0;
            for (size_t i = first; i < last; ++i) {
                // D20 bounds the prototype's integer arithmetic to wire-width values.
                shares[i - first] = (at_level * eligible[i].second->vol) / level_total;
                allocated += shares[i - first];
            }
            wire::Volume leftover = at_level - allocated;
            for (size_t i = first; i < last && leftover; ++i) {
                if (shares[i - first] < eligible[i].second->vol) {
                    ++shares[i - first];
                    --leftover;
                }
            }
        }
        for (size_t i = first; i < last; ++i) {
            const wire::Volume fill = shares[i - first];
            if (!fill) continue;
            auto& resting = *eligible[i].second;
            resting.vol -= fill;
            remaining -= fill;
            const uint64_t trade_id = ++next_trade_id_;
            res.order_ack.trades.push_back({trade_id, price, fill, eligible[i].first});
            WorkResult update;
            update.client_id = 0;
            update.req_id = 0;
            update.type = wire::MsgType::ORDER_UPDATE;
            update.order_ack.order_id = eligible[i].first;
            update.order_ack.status = resting.vol ? wire::OrderStatus::PARTIAL_FILL : wire::OrderStatus::FILLED;
            update.order_ack.remaining_vol = resting.vol;
            update.order_ack.trades.push_back({trade_id, price, fill, oid});
            emitResult(update);
        }
        first = last;
    }
    for (auto it = resting_orders_.begin(); it != resting_orders_.end();) {
        if (it->second.vol == 0) it = resting_orders_.erase(it);
        else ++it;
    }
    if (remaining) {
        RestingOrder ro;
        ro.symbol_id = item.symbol_id;
        ro.is_bid = is_bid;
        ro.price = item.price;
        ro.vol = remaining;
        ro.priority = ++next_priority_;
        resting_orders_[oid] = ro;
    }
    res.order_ack.status = remaining == item.volume ? wire::OrderStatus::RESTING :
                           remaining == 0 ? wire::OrderStatus::FILLED : wire::OrderStatus::PARTIAL_FILL;
    res.order_ack.remaining_vol = remaining;
    emitResult(res);
    publishSnapshots();
}

void Worker::handleCancel(const WorkItem& item) {
    WorkResult res;
    res.client_id = item.client_id;
    res.req_id = item.req_id;
    res.type = wire::MsgType::CANCEL_ACK;

    auto it = resting_orders_.find(item.order_id);
    if (it == resting_orders_.end()) {
        res.is_error = true;
        res.error_code = wire::ErrorCode::ORDER_NOT_FOUND;
    } else {
        resting_orders_.erase(it);
        res.is_error = false;
        res.cancel_ack.order_id = item.order_id;
        res.cancel_ack.status = wire::OrderStatus::CANCELLED;
        publishSnapshots();
    }

    emitResult(res);
}

void Worker::handleSubscribe(const WorkItem& item) {
    uint64_t key = endpointKey(item.sub_endpoint);
    subscribers_[key] = {item.sub_endpoint, item.group_mask};

    WorkResult res;
    res.client_id = item.client_id;
    res.req_id = item.req_id;
    res.type = wire::MsgType::SUBSCRIBE_ACK;
    res.is_error = false;
    res.sub_ack.group_mask = item.group_mask;
    emitResult(res);

    // Push immediate snapshot so subscriber has instant market depth
    publishSnapshots();
}

void Worker::handleUnsubscribe(const WorkItem& item) {
    uint64_t key = endpointKey(item.sub_endpoint);
    uint32_t removed_mask = 0;
    auto it = subscribers_.find(key);
    if (it != subscribers_.end()) {
        removed_mask = it->second.second & item.group_mask;
        it->second.second &= ~item.group_mask;
        if (it->second.second == 0) {
            subscribers_.erase(it);
        }
    }

    WorkResult res;
    res.client_id = item.client_id;
    res.req_id = item.req_id;
    res.type = wire::MsgType::UNSUBSCRIBE_ACK;
    res.is_error = false;
    res.unsub_ack.group_mask = removed_mask;
    emitResult(res);
}

void Worker::publishSnapshots() {
#if defined(_WIN32)
    if (udp_sock_ == INVALID_SOCKET || subscribers_.empty()) return;
#else
    if (udp_sock_ < 0 || subscribers_.empty()) return;
#endif

    auto now_epoch = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (const auto& ticker_pair : tickers_) {
        uint16_t sym_id = ticker_pair.first;
        wire::UdpMarketFeedDatagram dgram{};
        dgram.seq = ++seq_counter_;
        dgram.timestamp_us = static_cast<uint64_t>(now_epoch);
        dgram.group_id = group_id_;
        std::memset(dgram.ticker, 0, 8);
        std::strncpy(dgram.ticker, ticker_pair.second.c_str(), 7);

        // Aggregate volume per price level for this symbol
        std::map<wire::Price, wire::Volume, std::greater<wire::Price>> bid_levels; // High to low
        std::map<wire::Price, wire::Volume, std::less<wire::Price>> ask_levels;    // Low to high

        for (const auto& kv : resting_orders_) {
            const auto& o = kv.second;
            if (o.symbol_id == sym_id) {
                if (o.is_bid) {
                    bid_levels[o.price] += o.vol;
                } else {
                    ask_levels[o.price] += o.vol;
                }
            }
        }

        // Fill top-3 bid levels
        uint8_t nb = 0;
        for (const auto& bp : bid_levels) {
            if (nb >= 3) break;
            dgram.bids[nb] = {bp.first, bp.second};
            nb++;
        }
        dgram.n_bids = nb;

        // Fill top-3 ask levels.
        uint8_t na = 0;
        for (const auto& ap : ask_levels) {
            if (na >= 3) break;
            dgram.asks[na] = {ap.first, ap.second};
            na++;
        }
        dgram.n_asks = na;


        wire::ByteWriter writer(wire::UDP_DATAGRAM_LEN);
        codec::BinaryCodec::encodeUdpDatagram(dgram, writer);

        auto t_send_start = std::chrono::steady_clock::now();

        // Direct UDP unicast fan-out (D17, D29)
        for (const auto& sub_entry : subscribers_) {
            const auto& sin = sub_entry.second.first;
            uint32_t mask = sub_entry.second.second;
            if ((mask & (1U << (group_id_ - 1))) != 0) {
                sendto(udp_sock_,
                       reinterpret_cast<const char*>(writer.data()),
                       static_cast<int>(writer.size()),
                       0,
                       reinterpret_cast<const sockaddr*>(&sin),
                       sizeof(sin));
            }
        }

        auto t_send_end = std::chrono::steady_clock::now();
        double send_duration_us = std::chrono::duration_cast<std::chrono::microseconds>(
            t_send_end - t_send_start).count();

        // Update EWMA: T_UDP = (1 - alpha) * T_UDP + alpha * duration
        t_udp_ewma_us_ = 0.8 * t_udp_ewma_us_ + 0.2 * send_duration_us;
    }
}

} // namespace core
