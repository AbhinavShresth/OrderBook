#pragma once

#include "wire/Messages.hpp"
#include "wire/ByteReaderWriter.hpp"
#include <vector>

namespace codec {

class BinaryCodec {
public:
    // Header serialization & deserialization (20 bytes)
    static void encodeHeader(const wire::FrameHeader& hdr, wire::ByteWriter& writer);
    static bool decodeHeader(wire::ByteReader& reader, wire::FrameHeader& out_hdr);

    // Request payload serializers & deserializers
    static void encodeBid(const wire::BidRequest& req, wire::ByteWriter& writer);
    static bool decodeBid(wire::ByteReader& reader, wire::BidRequest& out_req);

    static void encodeAsk(const wire::AskRequest& req, wire::ByteWriter& writer);
    static bool decodeAsk(wire::ByteReader& reader, wire::AskRequest& out_req);

    static void encodeCancel(const wire::CancelRequest& req, wire::ByteWriter& writer);
    static bool decodeCancel(wire::ByteReader& reader, wire::CancelRequest& out_req);

    static void encodeSubscribe(const wire::SubscribeRequest& req, wire::ByteWriter& writer);
    static bool decodeSubscribe(wire::ByteReader& reader, wire::SubscribeRequest& out_req);

    static void encodeUnsubscribe(const wire::UnsubscribeRequest& req, wire::ByteWriter& writer);
    static bool decodeUnsubscribe(wire::ByteReader& reader, wire::UnsubscribeRequest& out_req);

    // Response payload serializers & deserializers
    static void encodeOrderAck(const wire::OrderAck& ack, wire::ByteWriter& writer);
    static bool decodeOrderAck(wire::ByteReader& reader, wire::OrderAck& out_ack);

    static void encodeCancelAck(const wire::CancelAck& ack, wire::ByteWriter& writer);
    static bool decodeCancelAck(wire::ByteReader& reader, wire::CancelAck& out_ack);

    static void encodeListFeedRsp(const wire::ListFeedResponse& rsp, wire::ByteWriter& writer);
    static bool decodeListFeedRsp(wire::ByteReader& reader, wire::ListFeedResponse& out_rsp);

    static void encodeSubscribeAck(const wire::SubscribeAck& ack, wire::ByteWriter& writer);
    static bool decodeSubscribeAck(wire::ByteReader& reader, wire::SubscribeAck& out_ack);

    static void encodeUnsubscribeAck(const wire::UnsubscribeAck& ack, wire::ByteWriter& writer);
    static bool decodeUnsubscribeAck(wire::ByteReader& reader, wire::UnsubscribeAck& out_ack);

    static void encodeError(const wire::ErrorResponse& err, wire::ByteWriter& writer);
    static bool decodeError(wire::ByteReader& reader, wire::ErrorResponse& out_err);

    static void encodeDisconnect(const wire::DisconnectMessage& msg, wire::ByteWriter& writer);
    static bool decodeDisconnect(wire::ByteReader& reader, wire::DisconnectMessage& out_msg);

    // UDP 128-byte datagram serializer & deserializer
    static void encodeUdpDatagram(const wire::UdpMarketFeedDatagram& dgram, wire::ByteWriter& writer);
    static bool decodeUdpDatagram(wire::ByteReader& reader, wire::UdpMarketFeedDatagram& out_dgram);
};

} // namespace codec
