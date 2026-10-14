#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <array>

namespace wire {

using Price = uint64_t;
using Volume = uint64_t;

enum class MsgType : uint8_t {
    BID             = 0x01,
    ASK             = 0x02,
    CANCEL          = 0x03,
    LIST_FEED       = 0x04,
    SUBSCRIBE       = 0x05,
    UNSUBSCRIBE     = 0x06,
    ORDER_ACK       = 0x81,
    CANCEL_ACK      = 0x83,
    LIST_FEED_RSP   = 0x84,
    SUBSCRIBE_ACK   = 0x85,
    UNSUBSCRIBE_ACK = 0x86,
    ERROR_RSP       = 0x8F,
    ORDER_UPDATE    = 0xC1,
    DISCONNECT      = 0xC2
};

enum class OrderStatus : uint8_t {
    RESTING      = 0,
    PARTIAL_FILL = 1,
    FILLED       = 2,
    CANCELLED    = 3
};

enum class ErrorCode : uint16_t {
    MALFORMED           = 1,
    UNKNOWN_TYPE        = 2,
    INVALID_FIELD       = 3,
    UNKNOWN_SYMBOL      = 4,
    ORDER_NOT_FOUND     = 5,
    NOT_OWNER           = 6,
    QUEUE_FULL          = 7,
    UNKNOWN_GROUP       = 8,
    UNSUPPORTED_VERSION = 9,
    UNKNOWN_CLIENT      = 10,
    CLIENT_MISMATCH     = 11
};

// 20-byte TCP frame header
struct FrameHeader {
    uint32_t length{0};         // Payload bytes following header
    uint32_t req_id{0};         // Client-chosen request correlation id
    uint64_t client_id{0};      // Server-assigned client id (0 = unassigned)
    uint16_t callback_port{0};  // Push callback port (0 = none)
    MsgType  type{MsgType::BID};
    uint8_t  version{1};        // Protocol version = 1
};

constexpr size_t FRAME_HEADER_LEN = 20;

// Logical request payloads
struct BidRequest {
    uint16_t symbol_id{0};
    Price    price{0};
    Volume   volume{0};
};

struct AskRequest {
    uint16_t symbol_id{0};
    Price    price{0};
    Volume   volume{0};
};

struct CancelRequest {
    uint64_t order_id{0};
};

struct SubscribeRequest {
    uint16_t udp_port{0};
    uint32_t group_mask{0};
};

struct UnsubscribeRequest {
    uint32_t group_mask{0};
};

// Logical response structures
struct TradeRecord {
    uint64_t trade_id{0};
    Price    price{0};
    Volume   vol{0};
    uint64_t counterpart_id{0};
};

struct OrderAck {
    uint64_t order_id{0};
    OrderStatus status{OrderStatus::RESTING};
    Volume remaining_vol{0};
    std::vector<TradeRecord> trades;
};

struct CancelAck {
    uint64_t order_id{0};
    OrderStatus status{OrderStatus::CANCELLED};
};

struct TickerInfo {
    uint16_t symbol_id{0};
    char ticker[8]{0};
};

struct GroupInfo {
    uint8_t group_id{0};
    std::string name;
    std::vector<TickerInfo> tickers;
};

struct ListFeedResponse {
    std::vector<GroupInfo> groups;
};

struct SubscribeAck {
    uint32_t group_mask{0};
};

struct UnsubscribeAck {
    uint32_t group_mask{0};
};

struct ErrorResponse {
    ErrorCode code{ErrorCode::MALFORMED};
};

struct DisconnectMessage {
    uint8_t reason{0}; // 0 = idle
};

// 128-byte UDP Market Feed Datagram
struct Level2Quote {
    Price price{0};
    Volume vol{0};
};

struct UdpMarketFeedDatagram {
    uint64_t seq{0};
    uint64_t timestamp_us{0};
    uint32_t group_id{0};
    char ticker[8]{0};
    uint8_t n_bids{0};
    uint8_t n_asks{0};
    uint16_t reserved{0};
    std::array<Level2Quote, 3> bids{};
    std::array<Level2Quote, 3> asks{};
};

constexpr size_t UDP_DATAGRAM_LEN = 128;

} // namespace wire
