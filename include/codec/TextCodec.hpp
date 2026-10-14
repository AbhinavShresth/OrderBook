#pragma once

#include "wire/Messages.hpp"
#include <string>
#include <unordered_map>
#include <vector>
#include <cstdint>

namespace codec {

// Represents an HTTP request or response in text mode
struct TextHttpFrame {
    std::string method;
    std::string path;
    int status_code{0};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

class TextCodec {
public:
    // Encode logical messages to HTTP/1.1 + JSON bytes
    static std::string encodeBidRequest(uint32_t req_id, uint64_t client_id, uint16_t callback_port,
                                         const std::string& ticker, wire::Price price, wire::Volume vol);

    static std::string encodeCancelRequest(uint32_t req_id, uint64_t client_id, uint16_t callback_port,
                                           uint64_t order_id);

    static std::string encodeOrderAck(uint32_t req_id, uint64_t client_id,
                                      const wire::OrderAck& ack);

    static std::string encodeCancelAck(uint32_t req_id, uint64_t client_id,
                                       const wire::CancelAck& ack);

    static std::string encodeError(uint32_t req_id, uint64_t client_id,
                                   int http_status, const std::string& err_str, const std::string& msg);

    // Parses HTTP framing from stream buffer. Returns true if complete HTTP frame was extracted.
    static bool parseHttpFrame(std::string& stream_buf, TextHttpFrame& out_frame);
};

} // namespace codec
