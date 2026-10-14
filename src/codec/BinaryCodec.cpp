#include "codec/BinaryCodec.hpp"
#include <cstring>

namespace codec {

void BinaryCodec::encodeHeader(const wire::FrameHeader& hdr, wire::ByteWriter& writer) {
    writer.writeU32(hdr.length);
    writer.writeU32(hdr.req_id);
    writer.writeU64(hdr.client_id);
    writer.writeU16(hdr.callback_port);
    writer.writeU8(static_cast<uint8_t>(hdr.type));
    writer.writeU8(hdr.version);
}

bool BinaryCodec::decodeHeader(wire::ByteReader& reader, wire::FrameHeader& out_hdr) {
    if (reader.remaining() < wire::FRAME_HEADER_LEN) {
        return false;
    }
    out_hdr.length = reader.readU32();
    out_hdr.req_id = reader.readU32();
    out_hdr.client_id = reader.readU64();
    out_hdr.callback_port = reader.readU16();
    out_hdr.type = static_cast<wire::MsgType>(reader.readU8());
    out_hdr.version = reader.readU8();
    return true;
}

void BinaryCodec::encodeBid(const wire::BidRequest& req, wire::ByteWriter& writer) {
    writer.writeU16(req.symbol_id);
    writer.writeU64(req.price);
    writer.writeU64(req.volume);
}

bool BinaryCodec::decodeBid(wire::ByteReader& reader, wire::BidRequest& out_req) {
    if (reader.remaining() < 18) return false;
    out_req.symbol_id = reader.readU16();
    out_req.price = reader.readU64();
    out_req.volume = reader.readU64();
    return true;
}

void BinaryCodec::encodeAsk(const wire::AskRequest& req, wire::ByteWriter& writer) {
    writer.writeU16(req.symbol_id);
    writer.writeU64(req.price);
    writer.writeU64(req.volume);
}

bool BinaryCodec::decodeAsk(wire::ByteReader& reader, wire::AskRequest& out_req) {
    if (reader.remaining() < 18) return false;
    out_req.symbol_id = reader.readU16();
    out_req.price = reader.readU64();
    out_req.volume = reader.readU64();
    return true;
}

void BinaryCodec::encodeCancel(const wire::CancelRequest& req, wire::ByteWriter& writer) {
    writer.writeU64(req.order_id);
}

bool BinaryCodec::decodeCancel(wire::ByteReader& reader, wire::CancelRequest& out_req) {
    if (reader.remaining() < 8) return false;
    out_req.order_id = reader.readU64();
    return true;
}

void BinaryCodec::encodeSubscribe(const wire::SubscribeRequest& req, wire::ByteWriter& writer) {
    writer.writeU16(req.udp_port);
    writer.writeU32(req.group_mask);
}

bool BinaryCodec::decodeSubscribe(wire::ByteReader& reader, wire::SubscribeRequest& out_req) {
    if (reader.remaining() < 6) return false;
    out_req.udp_port = reader.readU16();
    out_req.group_mask = reader.readU32();
    return true;
}

void BinaryCodec::encodeUnsubscribe(const wire::UnsubscribeRequest& req, wire::ByteWriter& writer) {
    writer.writeU32(req.group_mask);
}

bool BinaryCodec::decodeUnsubscribe(wire::ByteReader& reader, wire::UnsubscribeRequest& out_req) {
    if (reader.remaining() < 4) return false;
    out_req.group_mask = reader.readU32();
    return true;
}

void BinaryCodec::encodeOrderAck(const wire::OrderAck& ack, wire::ByteWriter& writer) {
    writer.writeU64(ack.order_id);
    writer.writeU8(static_cast<uint8_t>(ack.status));
    writer.writeU64(ack.remaining_vol);
    writer.writeU16(static_cast<uint16_t>(ack.trades.size()));
    for (const auto& t : ack.trades) {
        writer.writeU64(t.trade_id);
        writer.writeU64(t.price);
        writer.writeU64(t.vol);
        writer.writeU64(t.counterpart_id);
    }
}

bool BinaryCodec::decodeOrderAck(wire::ByteReader& reader, wire::OrderAck& out_ack) {
    if (reader.remaining() < 19) return false;
    out_ack.order_id = reader.readU64();
    out_ack.status = static_cast<wire::OrderStatus>(reader.readU8());
    out_ack.remaining_vol = reader.readU64();
    uint16_t n_trades = reader.readU16();
    if (reader.remaining() < n_trades * 32ULL) return false;
    out_ack.trades.clear();
    out_ack.trades.reserve(n_trades);
    for (uint16_t i = 0; i < n_trades; ++i) {
        wire::TradeRecord t;
        t.trade_id = reader.readU64();
        t.price = reader.readU64();
        t.vol = reader.readU64();
        t.counterpart_id = reader.readU64();
        out_ack.trades.push_back(t);
    }
    return true;
}

void BinaryCodec::encodeCancelAck(const wire::CancelAck& ack, wire::ByteWriter& writer) {
    writer.writeU64(ack.order_id);
    writer.writeU8(static_cast<uint8_t>(ack.status));
}

bool BinaryCodec::decodeCancelAck(wire::ByteReader& reader, wire::CancelAck& out_ack) {
    if (reader.remaining() < 9) return false;
    out_ack.order_id = reader.readU64();
    out_ack.status = static_cast<wire::OrderStatus>(reader.readU8());
    return true;
}

void BinaryCodec::encodeListFeedRsp(const wire::ListFeedResponse& rsp, wire::ByteWriter& writer) {
    writer.writeU8(static_cast<uint8_t>(rsp.groups.size()));
    for (const auto& g : rsp.groups) {
        writer.writeU8(g.group_id);
        uint8_t name_len = static_cast<uint8_t>(g.name.size());
        writer.writeU8(name_len);
        writer.writeBytes(g.name.data(), name_len);
        writer.writeU8(static_cast<uint8_t>(g.tickers.size()));
        for (const auto& t : g.tickers) {
            writer.writeU16(t.symbol_id);
            writer.writeBytes(t.ticker, 8);
        }
    }
}

bool BinaryCodec::decodeListFeedRsp(wire::ByteReader& reader, wire::ListFeedResponse& out_rsp) {
    if (reader.remaining() < 1) return false;
    uint8_t n_groups = reader.readU8();
    out_rsp.groups.clear();
    out_rsp.groups.reserve(n_groups);
    for (uint8_t i = 0; i < n_groups; ++i) {
        if (reader.remaining() < 2) return false;
        wire::GroupInfo g;
        g.group_id = reader.readU8();
        uint8_t name_len = reader.readU8();
        if (reader.remaining() < name_len + 1ULL) return false;
        g.name = reader.readString(name_len);
        uint8_t n_tickers = reader.readU8();
        if (reader.remaining() < n_tickers * 10ULL) return false;
        g.tickers.clear();
        g.tickers.reserve(n_tickers);
        for (uint8_t j = 0; j < n_tickers; ++j) {
            wire::TickerInfo t;
            t.symbol_id = reader.readU16();
            reader.readBytes(t.ticker, 8);
            g.tickers.push_back(t);
        }
        out_rsp.groups.push_back(g);
    }
    return true;
}

void BinaryCodec::encodeSubscribeAck(const wire::SubscribeAck& ack, wire::ByteWriter& writer) {
    writer.writeU32(ack.group_mask);
}

bool BinaryCodec::decodeSubscribeAck(wire::ByteReader& reader, wire::SubscribeAck& out_ack) {
    if (reader.remaining() < 4) return false;
    out_ack.group_mask = reader.readU32();
    return true;
}

void BinaryCodec::encodeUnsubscribeAck(const wire::UnsubscribeAck& ack, wire::ByteWriter& writer) {
    writer.writeU32(ack.group_mask);
}

bool BinaryCodec::decodeUnsubscribeAck(wire::ByteReader& reader, wire::UnsubscribeAck& out_ack) {
    if (reader.remaining() < 4) return false;
    out_ack.group_mask = reader.readU32();
    return true;
}

void BinaryCodec::encodeError(const wire::ErrorResponse& err, wire::ByteWriter& writer) {
    writer.writeU16(static_cast<uint16_t>(err.code));
}

bool BinaryCodec::decodeError(wire::ByteReader& reader, wire::ErrorResponse& out_err) {
    if (reader.remaining() < 2) return false;
    out_err.code = static_cast<wire::ErrorCode>(reader.readU16());
    return true;
}

void BinaryCodec::encodeDisconnect(const wire::DisconnectMessage& msg, wire::ByteWriter& writer) {
    writer.writeU8(msg.reason);
}

bool BinaryCodec::decodeDisconnect(wire::ByteReader& reader, wire::DisconnectMessage& out_msg) {
    if (reader.remaining() < 1) return false;
    out_msg.reason = reader.readU8();
    return true;
}

void BinaryCodec::encodeUdpDatagram(const wire::UdpMarketFeedDatagram& dgram, wire::ByteWriter& writer) {
    writer.writeU64(dgram.seq);
    writer.writeU64(dgram.timestamp_us);
    writer.writeU32(dgram.group_id);
    writer.writeBytes(dgram.ticker, 8);
    writer.writeU8(dgram.n_bids);
    writer.writeU8(dgram.n_asks);
    writer.writeU16(dgram.reserved);
    for (int i = 0; i < 3; ++i) {
        writer.writeU64(dgram.bids[i].price);
        writer.writeU64(dgram.bids[i].vol);
    }
    for (int i = 0; i < 3; ++i) {
        writer.writeU64(dgram.asks[i].price);
        writer.writeU64(dgram.asks[i].vol);
    }
}

bool BinaryCodec::decodeUdpDatagram(wire::ByteReader& reader, wire::UdpMarketFeedDatagram& out_dgram) {
    if (reader.remaining() < wire::UDP_DATAGRAM_LEN) return false;
    out_dgram.seq = reader.readU64();
    out_dgram.timestamp_us = reader.readU64();
    out_dgram.group_id = reader.readU32();
    reader.readBytes(out_dgram.ticker, 8);
    out_dgram.n_bids = reader.readU8();
    out_dgram.n_asks = reader.readU8();
    out_dgram.reserved = reader.readU16();
    for (int i = 0; i < 3; ++i) {
        out_dgram.bids[i].price = reader.readU64();
        out_dgram.bids[i].vol = reader.readU64();
    }
    for (int i = 0; i < 3; ++i) {
        out_dgram.asks[i].price = reader.readU64();
        out_dgram.asks[i].vol = reader.readU64();
    }
    return true;
}

} // namespace codec
