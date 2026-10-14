#include "net/Dispatcher.hpp"
#include "codec/BinaryCodec.hpp"
#include <iostream>
#include <cstring>

namespace net {

Dispatcher::Dispatcher(const std::string& host,
                       uint16_t tcp_port,
                       std::vector<std::unique_ptr<core::Worker>>& workers,
                       IdlePolicy idle_policy,
                       int idle_timeout_sec)
    : host_(host),
      tcp_port_(tcp_port),
      workers_(workers),
      idle_policy_(idle_policy),
      idle_timeout_sec_(idle_timeout_sec),
      poller_(std::make_unique<SystemPoller>()),
      rng_(1337) {

    // Build symbol_id -> worker routing table
    for (auto& w : workers_) {
        for (const auto& ticker_pair : w->tickers()) {
            symbol_to_worker_[ticker_pair.first] = w.get();
        }
    }
}

Dispatcher::~Dispatcher() {
    stop();
}

uint64_t Dispatcher::generateRandomClientId() {
    uint64_t id = 0;
    while (id == 0 || client_registry_.find(id) != client_registry_.end()) {
        id = rng_();
    }
    return id;
}

bool Dispatcher::start() {
    initNetworking();
    listen_sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock_ == INVALID_SOCKET) return false;

#if defined(_WIN32)
    BOOL reuse = TRUE;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
    int reuse = 1;
    setsockopt(listen_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    setNonBlocking(listen_sock_);

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(tcp_port_);

    if (bind(listen_sock_, (sockaddr*)&sin, sizeof(sin)) != 0) {
        // Fallback to ephemeral port 0 if specified port is busy
        sin.sin_port = htons(0);
        if (bind(listen_sock_, (sockaddr*)&sin, sizeof(sin)) != 0) {
            closeSocket(listen_sock_);
            listen_sock_ = INVALID_SOCKET;
            return false;
        }
    }

#if defined(_WIN32)
    int slen = sizeof(sin);
#else
    socklen_t slen = sizeof(sin);
#endif
    getsockname(listen_sock_, (sockaddr*)&sin, &slen);
    tcp_port_ = ntohs(sin.sin_port);

    if (listen(listen_sock_, 128) != 0) {
        closeSocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
        return false;
    }

    poller_->add(listen_sock_, true, false);
    running_ = true;
    return true;
}

void Dispatcher::stop() {
    if (!running_) return;
    running_ = false;

    for (auto& kv : connections_) {
        closeSocket(kv.second.sock);
    }
    connections_.clear();
    sock_to_conn_id_.clear();

    if (listen_sock_ != INVALID_SOCKET) {
        closeSocket(listen_sock_);
        listen_sock_ = INVALID_SOCKET;
    }
}

void Dispatcher::step() {
    if (!running_) return;

    // 1. Poll sockets with timeout 0 (busy-polling event loop D18, Section 3.2)
    poller_->poll(0);

    // 2. Accept new client connections
    if (poller_->isReadable(listen_sock_)) {
        acceptConnections();
    }

    // 3. Read ready connections (up to K frames per connection)
    std::vector<uint32_t> conn_ids_to_read;
    conn_ids_to_read.reserve(connections_.size());
    for (const auto& kv : connections_) {
        if (poller_->isReadable(kv.second.sock)) {
            conn_ids_to_read.push_back(kv.first);
        }
    }

    for (uint32_t cid : conn_ids_to_read) {
        auto it = connections_.find(cid);
        if (it != connections_.end()) {
            readFromConnection(it->second);
        }
    }

    // 4. Drain worker out-queues (round robin)
    drainWorkerOutQueues();

    // 5. Flush write buffers
    flushWriteBuffers();

    // 6. Check idle deadlines
    if (idle_policy_ == IdlePolicy::FIXED) {
        checkIdleDeadlines();
    }
}

void Dispatcher::acceptConnections() {
    while (true) {
        sockaddr_in client_addr{};
#if defined(_WIN32)
        int addr_len = sizeof(client_addr);
#else
        socklen_t addr_len = sizeof(client_addr);
#endif
        socket_t client_sock = accept(listen_sock_, (sockaddr*)&client_addr, &addr_len);
        if (client_sock == INVALID_SOCKET) break;

        setNonBlocking(client_sock);
        uint32_t cid = next_conn_id_++;

        Connection conn;
        conn.sock = client_sock;
        conn.conn_id = cid;
        conn.bound_client_id = 0;
        conn.peer_ip = inet_ntoa(client_addr.sin_addr);
        conn.peer_port = ntohs(client_addr.sin_port);
        conn.last_activity = std::chrono::steady_clock::now();

        connections_[cid] = std::move(conn);
        sock_to_conn_id_[client_sock] = cid;
        poller_->add(client_sock, true, false);
    }
}

void Dispatcher::readFromConnection(Connection& conn) {
    uint8_t temp_buf[4096];
    while (true) {
#if defined(_WIN32)
        int n = recv(conn.sock, reinterpret_cast<char*>(temp_buf), sizeof(temp_buf), 0);
#else
        ssize_t n = recv(conn.sock, temp_buf, sizeof(temp_buf), 0);
#endif
        if (n > 0) {
            conn.last_activity = std::chrono::steady_clock::now();
            try {
                conn.frame_reader.feed(temp_buf, static_cast<size_t>(n));
                Frame frame;
                size_t frames_processed = 0;
                while (frames_processed < 64 && conn.frame_reader.extractFrame(frame)) {
                    frames_processed++;
                    processClientFrame(conn, frame);
                }
            } catch (const std::exception&) {
                // Protocol violation (bad version / length) -> close connection immediately (NFR-4)
                closeConnection(conn.conn_id);
                return;
            }
        } else if (n == 0) {
            // EOF -> close connection
            closeConnection(conn.conn_id);
            return;
        } else {
            // EAGAIN / EWOULDBLOCK
            break;
        }
    }
}

void Dispatcher::processClientFrame(Connection& conn, const Frame& frame) {
    const auto& hdr = frame.header;

    // D11: client_id identity assignment and verification
    if (conn.bound_client_id == 0) {
        if (hdr.client_id == 0) {
            // First frame with client_id 0 -> generate fresh ID
            uint64_t new_id = generateRandomClientId();
            conn.bound_client_id = new_id;
            ClientRecord rec;
            rec.client_id = new_id;
            rec.current_conn_id = conn.conn_id;
            rec.peer_ip = conn.peer_ip;
            rec.callback_port = hdr.callback_port;
            client_registry_[new_id] = rec;
        } else {
            // Nonzero client_id on fresh connection: lookup registry
            auto it = client_registry_.find(hdr.client_id);
            if (it == client_registry_.end()) {
                sendErrorResponse(conn, hdr.req_id, 0, wire::ErrorCode::UNKNOWN_CLIENT);
                closeConnection(conn.conn_id);
                return;
            }
            conn.bound_client_id = hdr.client_id;
            it->second.current_conn_id = conn.conn_id;
            it->second.peer_ip = conn.peer_ip;
            it->second.callback_port = hdr.callback_port;
        }
    } else {
        // Already bound connection
        if (hdr.client_id != 0 && hdr.client_id != conn.bound_client_id) {
            sendErrorResponse(conn, hdr.req_id, conn.bound_client_id, wire::ErrorCode::CLIENT_MISMATCH);
            closeConnection(conn.conn_id);
            return;
        }
    }

    uint64_t client_id = conn.bound_client_id;
    conn.callback_port = hdr.callback_port;
    wire::ByteReader payload_reader(frame.payload.data(), frame.payload.size());

    switch (hdr.type) {
        case wire::MsgType::LIST_FEED: {
            wire::ListFeedResponse rsp;
            for (const auto& w : workers_) {
                wire::GroupInfo g;
                g.group_id = static_cast<uint8_t>(w->group_id());
                g.name = w->group_name();
                for (const auto& tp : w->tickers()) {
                    wire::TickerInfo ti;
                    ti.symbol_id = tp.first;
                    std::memset(ti.ticker, 0, 8);
                    std::strncpy(ti.ticker, tp.second.c_str(), 7);
                    g.tickers.push_back(ti);
                }
                rsp.groups.push_back(g);
            }
            wire::ByteWriter writer;
            codec::BinaryCodec::encodeListFeedRsp(rsp, writer);
            wire::FrameHeader out_hdr{};
            out_hdr.length = static_cast<uint32_t>(writer.size());
            out_hdr.req_id = hdr.req_id;
            out_hdr.client_id = client_id;
            out_hdr.type = wire::MsgType::LIST_FEED_RSP;
            out_hdr.version = 1;
            sendResponse(conn, out_hdr, writer.data(), writer.size());
            break;
        }

        case wire::MsgType::BID: {
            wire::BidRequest req;
            if (!codec::BinaryCodec::decodeBid(payload_reader, req) || req.price == 0 || req.volume == 0) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::INVALID_FIELD);
                return;
            }
            auto w_it = symbol_to_worker_.find(req.symbol_id);
            if (w_it == symbol_to_worker_.end()) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::UNKNOWN_SYMBOL);
                return;
            }
            core::WorkItem wi;
            wi.item_type = core::WorkItemType::ORDER;
            wi.client_id = client_id;
            wi.req_id = hdr.req_id;
            wi.type = wire::MsgType::BID;
            wi.symbol_id = req.symbol_id;
            wi.price = req.price;
            wi.volume = req.volume;
            if (!w_it->second->in_queue().push(wi)) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::QUEUE_FULL);
            }
            break;
        }

        case wire::MsgType::ASK: {
            wire::AskRequest req;
            if (!codec::BinaryCodec::decodeAsk(payload_reader, req) || req.price == 0 || req.volume == 0) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::INVALID_FIELD);
                return;
            }
            auto w_it = symbol_to_worker_.find(req.symbol_id);
            if (w_it == symbol_to_worker_.end()) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::UNKNOWN_SYMBOL);
                return;
            }
            core::WorkItem wi;
            wi.item_type = core::WorkItemType::ORDER;
            wi.client_id = client_id;
            wi.req_id = hdr.req_id;
            wi.type = wire::MsgType::ASK;
            wi.symbol_id = req.symbol_id;
            wi.price = req.price;
            wi.volume = req.volume;
            if (!w_it->second->in_queue().push(wi)) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::QUEUE_FULL);
            }
            break;
        }

        case wire::MsgType::CANCEL: {
            wire::CancelRequest req;
            if (!codec::BinaryCodec::decodeCancel(payload_reader, req)) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::MALFORMED);
                return;
            }
            // Ownership check (Section 7, D12)
            auto owner_it = order_to_client_.find(req.order_id);
            if (owner_it != order_to_client_.end() && owner_it->second != client_id) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::NOT_OWNER);
                return;
            }

            // D9: worker_id = order_id >> 56
            uint8_t target_worker = static_cast<uint8_t>(req.order_id >> 56);
            core::Worker* target = nullptr;
            for (auto& w : workers_) {
                if (w->worker_id() == target_worker) {
                    target = w.get();
                    break;
                }
            }
            if (!target) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::ORDER_NOT_FOUND);
                return;
            }

            core::WorkItem wi;
            wi.item_type = core::WorkItemType::CANCEL;
            wi.client_id = client_id;
            wi.req_id = hdr.req_id;
            wi.order_id = req.order_id;
            target->in_queue().push(wi);
            break;
        }

        case wire::MsgType::SUBSCRIBE: {
            wire::SubscribeRequest req;
            if (!codec::BinaryCodec::decodeSubscribe(payload_reader, req)) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::MALFORMED);
                return;
            }

            sockaddr_in sub_sin{};
            sub_sin.sin_family = AF_INET;
            sub_sin.sin_addr.s_addr = inet_addr(conn.peer_ip.c_str());
            sub_sin.sin_port = htons(req.udp_port);

            // Forward to affected workers
            for (auto& w : workers_) {
                if ((req.group_mask & (1U << (w->group_id() - 1))) != 0) {
                    core::WorkItem wi;
                    wi.item_type = core::WorkItemType::SUBSCRIBE_CONTROL;
                    wi.client_id = client_id;
                    wi.req_id = hdr.req_id;
                    wi.sub_endpoint = sub_sin;
                    wi.group_mask = req.group_mask;
                    w->in_queue().push(wi);
                }
            }
            break;
        }

        case wire::MsgType::UNSUBSCRIBE: {
            wire::UnsubscribeRequest req;
            if (!codec::BinaryCodec::decodeUnsubscribe(payload_reader, req)) {
                sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::MALFORMED);
                return;
            }

            sockaddr_in sub_sin{};
            sub_sin.sin_family = AF_INET;
            sub_sin.sin_addr.s_addr = inet_addr(conn.peer_ip.c_str());

            for (auto& w : workers_) {
                if ((req.group_mask & (1U << (w->group_id() - 1))) != 0) {
                    core::WorkItem wi;
                    wi.item_type = core::WorkItemType::UNSUBSCRIBE_CONTROL;
                    wi.client_id = client_id;
                    wi.req_id = hdr.req_id;
                    wi.sub_endpoint = sub_sin;
                    wi.group_mask = req.group_mask;
                    w->in_queue().push(wi);
                }
            }
            break;
        }

        default:
            sendErrorResponse(conn, hdr.req_id, client_id, wire::ErrorCode::UNKNOWN_TYPE);
            break;
    }
}

void Dispatcher::drainWorkerOutQueues() {
    for (auto& w : workers_) {
        core::WorkResult res;
        size_t count = 0;
        while (count < 64 && w->out_queue().pop(res)) {
            count++;
            uint64_t target_client_id = res.client_id;
            if (!res.is_error && res.type == wire::MsgType::ORDER_UPDATE) {
                auto owner = order_to_client_.find(res.order_ack.order_id);
                if (owner == order_to_client_.end()) continue;
                target_client_id = owner->second;
                if (res.order_ack.status == wire::OrderStatus::FILLED)
                    order_to_client_.erase(owner);
            }
            auto cl_it = client_registry_.find(target_client_id);
            if (cl_it == client_registry_.end()) continue;

            uint32_t cid = cl_it->second.current_conn_id;
            auto conn_it = connections_.find(cid);

            wire::ByteWriter writer;
            wire::FrameHeader out_hdr{};
            out_hdr.req_id = (res.type == wire::MsgType::ORDER_UPDATE) ? 0 : res.req_id;
            out_hdr.client_id = target_client_id;
            out_hdr.version = 1;

            if (res.is_error) {
                wire::ErrorResponse err{res.error_code};
                codec::BinaryCodec::encodeError(err, writer);
                out_hdr.type = wire::MsgType::ERROR_RSP;
            } else if (res.type == wire::MsgType::ORDER_ACK) {
                // Only live orders need an ownership entry for later updates/cancels.
                if (res.order_ack.status == wire::OrderStatus::RESTING ||
                    res.order_ack.status == wire::OrderStatus::PARTIAL_FILL)
                    order_to_client_[res.order_ack.order_id] = target_client_id;
                else
                    order_to_client_.erase(res.order_ack.order_id);
                codec::BinaryCodec::encodeOrderAck(res.order_ack, writer);
                out_hdr.type = wire::MsgType::ORDER_ACK;
            } else if (res.type == wire::MsgType::ORDER_UPDATE) {
                codec::BinaryCodec::encodeOrderAck(res.order_ack, writer);
                out_hdr.type = wire::MsgType::ORDER_UPDATE;
            } else if (res.type == wire::MsgType::CANCEL_ACK) {
                order_to_client_.erase(res.cancel_ack.order_id);
                codec::BinaryCodec::encodeCancelAck(res.cancel_ack, writer);
                out_hdr.type = wire::MsgType::CANCEL_ACK;
            } else if (res.type == wire::MsgType::SUBSCRIBE_ACK) {
                codec::BinaryCodec::encodeSubscribeAck(res.sub_ack, writer);
                out_hdr.type = wire::MsgType::SUBSCRIBE_ACK;
            } else if (res.type == wire::MsgType::UNSUBSCRIBE_ACK) {
                codec::BinaryCodec::encodeUnsubscribeAck(res.unsub_ack, writer);
                out_hdr.type = wire::MsgType::UNSUBSCRIBE_ACK;
            }

            out_hdr.length = static_cast<uint32_t>(writer.size());

            if (conn_it != connections_.end()) {
                sendResponse(conn_it->second, out_hdr, writer.data(), writer.size());
            } else {
                // Mailbox delivery for push
                wire::ByteWriter frame_bytes;
                codec::BinaryCodec::encodeHeader(out_hdr, frame_bytes);
                frame_bytes.writeBytes(writer.data(), writer.size());
                if (cl_it->second.mailbox.size() >= 1000) {
                    cl_it->second.mailbox.erase(cl_it->second.mailbox.begin()); // Drop oldest
                }
                cl_it->second.mailbox.push_back(frame_bytes.buffer());
            }
        }
    }
}

void Dispatcher::flushWriteBuffers() {
    for (auto& kv : connections_) {
        Connection& conn = kv.second;
        if (!conn.write_buf.empty()) {
#if defined(_WIN32)
            int sent = send(conn.sock, reinterpret_cast<const char*>(conn.write_buf.data()), static_cast<int>(conn.write_buf.size()), 0);
#else
            ssize_t sent = send(conn.sock, conn.write_buf.data(), conn.write_buf.size(), 0);
#endif
            if (sent > 0) {
                conn.write_buf.erase(conn.write_buf.begin(), conn.write_buf.begin() + sent);
                if (conn.write_buf.empty()) {
                    poller_->modify(conn.sock, true, false);
                }
            } else if (sent < 0) {
#if defined(_WIN32)
                int err = WSAGetLastError();
                if (err != WSAEWOULDBLOCK) closeConnection(conn.conn_id);
#else
                if (errno != EAGAIN && errno != EWOULDBLOCK) closeConnection(conn.conn_id);
#endif
            }
        }
    }
}

void Dispatcher::checkIdleDeadlines() {
    auto now = std::chrono::steady_clock::now();
    std::vector<uint32_t> to_disconnect;
    for (const auto& kv : connections_) {
        const Connection& conn = kv.second;
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - conn.last_activity).count();
        if (elapsed >= idle_timeout_sec_) {
            to_disconnect.push_back(conn.conn_id);
        }
    }

    for (uint32_t cid : to_disconnect) {
        auto it = connections_.find(cid);
        if (it != connections_.end()) {
            // Send DISCONNECT frame (D13, Section 3.5)
            wire::DisconnectMessage dm{0}; // reason 0 = idle
            wire::ByteWriter writer;
            codec::BinaryCodec::encodeDisconnect(dm, writer);
            wire::FrameHeader hdr{};
            hdr.length = static_cast<uint32_t>(writer.size());
            hdr.type = wire::MsgType::DISCONNECT;
            hdr.client_id = it->second.bound_client_id;
            hdr.version = 1;
            sendResponse(it->second, hdr, writer.data(), writer.size());
            closeConnection(cid);
        }
    }
}

void Dispatcher::sendResponse(Connection& conn, const wire::FrameHeader& hdr, const uint8_t* payload, size_t len) {
    wire::ByteWriter writer(wire::FRAME_HEADER_LEN + len);
    codec::BinaryCodec::encodeHeader(hdr, writer);
    if (payload && len > 0) {
        writer.writeBytes(payload, len);
    }

    conn.write_buf.insert(conn.write_buf.end(), writer.data(), writer.data() + writer.size());
    poller_->modify(conn.sock, true, true);
}

void Dispatcher::sendErrorResponse(Connection& conn, uint32_t req_id, uint64_t client_id, wire::ErrorCode code) {
    wire::ErrorResponse err{code};
    wire::ByteWriter writer;
    codec::BinaryCodec::encodeError(err, writer);

    wire::FrameHeader hdr{};
    hdr.length = static_cast<uint32_t>(writer.size());
    hdr.req_id = req_id;
    hdr.client_id = client_id;
    hdr.type = wire::MsgType::ERROR_RSP;
    hdr.version = 1;

    sendResponse(conn, hdr, writer.data(), writer.size());
}

void Dispatcher::closeConnection(uint32_t conn_id) {
    auto it = connections_.find(conn_id);
    if (it != connections_.end()) {
        poller_->remove(it->second.sock);
        closeSocket(it->second.sock);
        sock_to_conn_id_.erase(it->second.sock);

        // Update ClientRecord
        if (it->second.bound_client_id != 0) {
            auto cl_it = client_registry_.find(it->second.bound_client_id);
            if (cl_it != client_registry_.end() && cl_it->second.current_conn_id == conn_id) {
                cl_it->second.current_conn_id = 0;
            }
        }
        connections_.erase(it);
    }
}

} // namespace net
