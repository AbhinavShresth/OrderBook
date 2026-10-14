#pragma once

#include "wire/Messages.hpp"
#include <cstdint>
#if defined(_WIN32)
#include <winsock2.h>
#else
#include <netinet/in.h>
#endif

namespace core {

enum class WorkItemType : uint8_t {
    ORDER,
    CANCEL,
    SUBSCRIBE_CONTROL,
    UNSUBSCRIBE_CONTROL
};

struct WorkItem {
    WorkItemType item_type{WorkItemType::ORDER};
    uint64_t client_id{0};    // Opaque reply token, echoed back
    uint32_t req_id{0};
    wire::MsgType type{wire::MsgType::BID};
    uint16_t symbol_id{0};
    wire::Price price{0};
    wire::Volume volume{0};
    uint64_t order_id{0};

    // Subscription control fields (D29, Section 6)
    sockaddr_in sub_endpoint{};
    uint32_t group_mask{0};
};

struct WorkResult {
    uint64_t client_id{0};
    uint32_t req_id{0};
    wire::MsgType type{wire::MsgType::ORDER_ACK};
    bool is_error{false};
    wire::ErrorCode error_code{wire::ErrorCode::MALFORMED};

    wire::OrderAck order_ack{};
    wire::CancelAck cancel_ack{};
    wire::SubscribeAck sub_ack{};
    wire::UnsubscribeAck unsub_ack{};
};

} // namespace core
