#include "client/ClientSession.hpp"
#include "codec/BinaryCodec.hpp"
#include "wire/ByteReaderWriter.hpp"
#include <iostream>

namespace client {

ClientSession::ClientSession(const std::string& host, uint16_t port, uint16_t callback_port)
    : host_(host), port_(port), callback_port_(callback_port) {}

ClientSession::~ClientSession() {
    disconnect();
}

bool ClientSession::connect() {
    if (!network_initialized_) {
        if (!net::initNetworking()) return false;
        network_initialized_ = true;
    }
    sock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock_ == INVALID_SOCKET) return false;

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = inet_addr(host_.c_str());
    sin.sin_port = htons(port_);

    if (::connect(sock_, (sockaddr*)&sin, sizeof(sin)) != 0) {
        net::closeSocket(sock_);
        sock_ = INVALID_SOCKET;
        return false;
    }

    frame_reader_.clear();
    return true;
}

void ClientSession::disconnect() {
    if (sock_ != INVALID_SOCKET) {
        net::closeSocket(sock_);
        sock_ = INVALID_SOCKET;
    }
}

bool ClientSession::isConnected() const {
    return sock_ != INVALID_SOCKET;
}

void ClientSession::ensureConnected() {
    if (!isConnected()) {
        connect();
    }
}

bool ClientSession::sendFrame(wire::MsgType type, const uint8_t* payload, size_t len, uint32_t req_id) {
    if (!prepareConnection()) return false;

    wire::FrameHeader hdr{};
    hdr.length = static_cast<uint32_t>(len);
    hdr.req_id = req_id;
    hdr.client_id = client_id_;
    hdr.callback_port = callback_port_;
    hdr.type = type;
    hdr.version = 1;

    wire::ByteWriter writer(wire::FRAME_HEADER_LEN + len);
    codec::BinaryCodec::encodeHeader(hdr, writer);
    if (payload && len > 0) {
        writer.writeBytes(payload, len);
    }

#if defined(_WIN32)
    int n = send(sock_, reinterpret_cast<const char*>(writer.data()), static_cast<int>(writer.size()), 0);
#else
    ssize_t n = send(sock_, writer.data(), writer.size(), 0);
#endif
    return n == static_cast<int>(writer.size());
}

bool ClientSession::prepareConnection() {
    ensureConnected();
    if (!isConnected()) return false;

    // The server's fixed idle policy sends DISCONNECT before closing. Check for
    // that frame (or an already delivered FIN) before sending a new request;
    // reconnecting here is safe because this request has not been transmitted.
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(sock_, &readable);
    timeval timeout{};
#if defined(_WIN32)
    int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
    int ready = select(sock_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready <= 0 || !FD_ISSET(sock_, &readable)) return true;

    uint8_t byte = 0;
#if defined(_WIN32)
    int peeked = recv(sock_, reinterpret_cast<char*>(&byte), 1, MSG_PEEK);
#else
    ssize_t peeked = recv(sock_, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
#endif
    if (peeked == 0) {
        disconnect();
        return connect();
    }
    if (peeked < 0) return true; // A transient readiness race; normal send handles it.

    net::Frame frame;
    if (!receiveFrame(frame)) return connect();
    if (frame.header.type == wire::MsgType::DISCONNECT) {
        disconnect();
        return connect();
    }

    if (frame.header.type == wire::MsgType::ORDER_UPDATE) {
        dispatchOrderUpdate(frame);
        return true;
    }
    // Do not mistake an unrelated response for the next command's reply.
    disconnect();
    return false;
}

void ClientSession::dispatchOrderUpdate(const net::Frame& frame) {
    wire::ByteReader reader(frame.payload.data(), frame.payload.size());
    wire::OrderAck update;
    if (codec::BinaryCodec::decodeOrderAck(reader, update) && order_update_callback_)
        order_update_callback_(update);
}

bool ClientSession::receiveResponse(uint32_t req_id, wire::MsgType expected, net::Frame& out_frame) {
    while (receiveFrame(out_frame)) {
        if (out_frame.header.type == wire::MsgType::ORDER_UPDATE) {
            dispatchOrderUpdate(out_frame);
            continue;
        }
        if (out_frame.header.req_id != req_id) return false;
        return out_frame.header.type == expected || out_frame.header.type == wire::MsgType::ERROR_RSP;
    }
    return false;
}

bool ClientSession::receiveFrame(net::Frame& out_frame) {
    while (true) {
        if (frame_reader_.extractFrame(out_frame)) {
            if (client_id_ == 0 && out_frame.header.client_id != 0) {
                client_id_ = out_frame.header.client_id;
            }
            return true;
        }

        uint8_t buf[2048];
#if defined(_WIN32)
        int n = recv(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
        ssize_t n = recv(sock_, buf, sizeof(buf), 0);
#endif
        if (n <= 0) {
            disconnect();
            return false;
        }
        frame_reader_.feed(buf, static_cast<size_t>(n));
    }
}

bool ClientSession::listFeed(wire::ListFeedResponse& out_rsp) {
    uint32_t req_id = next_req_id_++;
    if (!sendFrame(wire::MsgType::LIST_FEED, nullptr, 0, req_id)) return false;
    net::Frame frame;
    if (!receiveResponse(req_id, wire::MsgType::LIST_FEED_RSP, frame) || frame.header.type != wire::MsgType::LIST_FEED_RSP) return false;
    wire::ByteReader r(frame.payload.data(), frame.payload.size());
    return codec::BinaryCodec::decodeListFeedRsp(r, out_rsp);
}

bool ClientSession::placeBid(uint16_t symbol_id, wire::Price price, wire::Volume vol,
                             wire::OrderAck& out_ack, wire::ErrorCode& out_err) {
    out_err = wire::ErrorCode::MALFORMED;
    uint32_t req_id = next_req_id_++;
    wire::BidRequest req{symbol_id, price, vol};
    wire::ByteWriter w;
    codec::BinaryCodec::encodeBid(req, w);

    if (!sendFrame(wire::MsgType::BID, w.data(), w.size(), req_id)) return false;
    net::Frame frame;
    if (!receiveResponse(req_id, wire::MsgType::ORDER_ACK, frame)) return false;

    wire::ByteReader r(frame.payload.data(), frame.payload.size());
    if (frame.header.type == wire::MsgType::ERROR_RSP) {
        wire::ErrorResponse err;
        codec::BinaryCodec::decodeError(r, err);
        out_err = err.code;
        return false;
    }
    return codec::BinaryCodec::decodeOrderAck(r, out_ack);
}

bool ClientSession::placeAsk(uint16_t symbol_id, wire::Price price, wire::Volume vol,
                             wire::OrderAck& out_ack, wire::ErrorCode& out_err) {
    out_err = wire::ErrorCode::MALFORMED;
    uint32_t req_id = next_req_id_++;
    wire::AskRequest req{symbol_id, price, vol};
    wire::ByteWriter w;
    codec::BinaryCodec::encodeAsk(req, w);

    if (!sendFrame(wire::MsgType::ASK, w.data(), w.size(), req_id)) return false;
    net::Frame frame;
    if (!receiveResponse(req_id, wire::MsgType::ORDER_ACK, frame)) return false;

    wire::ByteReader r(frame.payload.data(), frame.payload.size());
    if (frame.header.type == wire::MsgType::ERROR_RSP) {
        wire::ErrorResponse err;
        codec::BinaryCodec::decodeError(r, err);
        out_err = err.code;
        return false;
    }
    return codec::BinaryCodec::decodeOrderAck(r, out_ack);
}

bool ClientSession::cancelOrder(uint64_t order_id, wire::CancelAck& out_ack, wire::ErrorCode& out_err) {
    out_err = wire::ErrorCode::MALFORMED;
    uint32_t req_id = next_req_id_++;
    wire::CancelRequest req{order_id};
    wire::ByteWriter w;
    codec::BinaryCodec::encodeCancel(req, w);

    if (!sendFrame(wire::MsgType::CANCEL, w.data(), w.size(), req_id)) return false;
    net::Frame frame;
    if (!receiveResponse(req_id, wire::MsgType::CANCEL_ACK, frame)) return false;

    wire::ByteReader r(frame.payload.data(), frame.payload.size());
    if (frame.header.type == wire::MsgType::ERROR_RSP) {
        wire::ErrorResponse err;
        codec::BinaryCodec::decodeError(r, err);
        out_err = err.code;
        return false;
    }
    return codec::BinaryCodec::decodeCancelAck(r, out_ack);
}

bool ClientSession::subscribe(uint16_t udp_port, uint32_t group_mask, uint32_t& out_subscribed_mask) {
    uint32_t req_id = next_req_id_++;
    wire::SubscribeRequest req{udp_port, group_mask};
    wire::ByteWriter w;
    codec::BinaryCodec::encodeSubscribe(req, w);

    if (!sendFrame(wire::MsgType::SUBSCRIBE, w.data(), w.size(), req_id)) return false;
    net::Frame frame;
    if (!receiveResponse(req_id, wire::MsgType::SUBSCRIBE_ACK, frame) || frame.header.type != wire::MsgType::SUBSCRIBE_ACK) return false;

    wire::ByteReader r(frame.payload.data(), frame.payload.size());
    wire::SubscribeAck ack;
    if (codec::BinaryCodec::decodeSubscribeAck(r, ack)) {
        out_subscribed_mask = ack.group_mask;
        return true;
    }
    return false;
}

bool ClientSession::unsubscribe(uint32_t group_mask, uint32_t& out_unsubscribed_mask) {
    uint32_t req_id = next_req_id_++;
    wire::UnsubscribeRequest req{group_mask};
    wire::ByteWriter w;
    codec::BinaryCodec::encodeUnsubscribe(req, w);

    if (!sendFrame(wire::MsgType::UNSUBSCRIBE, w.data(), w.size(), req_id)) return false;
    net::Frame frame;
    if (!receiveResponse(req_id, wire::MsgType::UNSUBSCRIBE_ACK, frame) || frame.header.type != wire::MsgType::UNSUBSCRIBE_ACK) return false;

    wire::ByteReader r(frame.payload.data(), frame.payload.size());
    wire::UnsubscribeAck ack;
    if (codec::BinaryCodec::decodeUnsubscribeAck(r, ack)) {
        out_unsubscribed_mask = ack.group_mask;
        return true;
    }
    return false;
}

// UdpListener Implementation
UdpListener::UdpListener(uint16_t listen_port, OnQuoteCallback callback)
    : port_(listen_port), callback_(callback) {}

UdpListener::~UdpListener() {
    stop();
}

bool UdpListener::start() {
    net::initNetworking();
    sock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock_ == INVALID_SOCKET) return false;

#if defined(_WIN32)
    BOOL reuse = TRUE;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuse, sizeof(reuse));
#else
    int reuse = 1;
    setsockopt(sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(port_);

    if (bind(sock_, (sockaddr*)&sin, sizeof(sin)) != 0) {
        sin.sin_port = htons(0);
        bind(sock_, (sockaddr*)&sin, sizeof(sin));
    }

#if defined(_WIN32)
    int slen = sizeof(sin);
#else
    socklen_t slen = sizeof(sin);
#endif
    getsockname(sock_, (sockaddr*)&sin, &slen);
    port_ = ntohs(sin.sin_port);

    running_ = true;
    thread_ = std::thread(&UdpListener::listenLoop, this);
    return true;
}

void UdpListener::stop() {
    if (running_) {
        running_ = false;
        if (sock_ != INVALID_SOCKET) {
            net::closeSocket(sock_);
            sock_ = INVALID_SOCKET;
        }
        if (thread_.joinable()) {
            thread_.join();
        }
    }
}

void UdpListener::listenLoop() {
    uint8_t buf[512];
    while (running_) {
#if defined(_WIN32)
        int n = recv(sock_, reinterpret_cast<char*>(buf), sizeof(buf), 0);
#else
        ssize_t n = recv(sock_, buf, sizeof(buf), 0);
#endif
        if (n == static_cast<int>(wire::UDP_DATAGRAM_LEN)) {
            packets_received_++;
            wire::ByteReader r(buf, static_cast<size_t>(n));
            wire::UdpMarketFeedDatagram dgram;
            if (codec::BinaryCodec::decodeUdpDatagram(r, dgram)) {
                bool loss = false;
                uint64_t dropped = 0;
                auto it = expected_seq_.find(dgram.group_id);
                if (it != expected_seq_.end()) {
                    if (dgram.seq > it->second) {
                        loss = true;
                        dropped = dgram.seq - it->second;
                        packets_lost_ += dropped;
                    }
                }
                expected_seq_[dgram.group_id] = dgram.seq + 1;

                if (callback_) {
                    callback_(dgram, loss, dropped);
                }
            }
        }
    }
}

} // namespace client
