#include "codec/TextCodec.hpp"
#include "nlohmann/json.hpp"
#include <sstream>
#include <algorithm>

namespace codec {

std::string TextCodec::encodeBidRequest(uint32_t req_id, uint64_t client_id, uint16_t callback_port,
                                        const std::string& ticker, wire::Price price, wire::Volume vol) {
    nlohmann::json j;
    j["ticker"] = ticker;
    j["type"] = "LIMIT";
    j["max_price"] = price;
    j["volume"] = vol;
    std::string body = j.dump();

    std::ostringstream oss;
    oss << "POST /order/bid HTTP/1.1\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Req-ID: " << req_id << "\r\n"
        << "Client-ID: " << client_id << "\r\n"
        << "Callback-Port: " << callback_port << "\r\n\r\n"
        << body;
    return oss.str();
}

std::string TextCodec::encodeCancelRequest(uint32_t req_id, uint64_t client_id, uint16_t callback_port,
                                          uint64_t order_id) {
    std::ostringstream oss;
    oss << "DELETE /order/cancel HTTP/1.1\r\n"
        << "Content-Length: 0\r\n"
        << "Order-ID: " << order_id << "\r\n"
        << "Req-ID: " << req_id << "\r\n"
        << "Client-ID: " << client_id << "\r\n"
        << "Callback-Port: " << callback_port << "\r\n\r\n";
    return oss.str();
}

std::string TextCodec::encodeOrderAck(uint32_t req_id, uint64_t client_id,
                                     const wire::OrderAck& ack) {
    nlohmann::json j;
    j["order_id"] = ack.order_id;
    switch (ack.status) {
        case wire::OrderStatus::RESTING: j["status"] = "RESTING"; break;
        case wire::OrderStatus::PARTIAL_FILL: j["status"] = "PARTIAL_FILL"; break;
        case wire::OrderStatus::FILLED: j["status"] = "FILLED"; break;
        case wire::OrderStatus::CANCELLED: j["status"] = "CANCELLED"; break;
    }
    j["remaining_vol"] = ack.remaining_vol;
    nlohmann::json trades_arr = nlohmann::json::array();
    for (const auto& t : ack.trades) {
        nlohmann::json tj;
        tj["trade_id"] = t.trade_id;
        tj["price"] = t.price;
        tj["vol"] = t.vol;
        tj["counterpart_id"] = t.counterpart_id;
        trades_arr.push_back(tj);
    }
    j["trades"] = trades_arr;
    std::string body = j.dump();

    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Req-ID: " << req_id << "\r\n"
        << "Client-ID: " << client_id << "\r\n\r\n"
        << body;
    return oss.str();
}

std::string TextCodec::encodeCancelAck(uint32_t req_id, uint64_t client_id,
                                      const wire::CancelAck& ack) {
    nlohmann::json j;
    j["order_id"] = ack.order_id;
    j["status"] = "CANCELLED";
    std::string body = j.dump();

    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Req-ID: " << req_id << "\r\n"
        << "Client-ID: " << client_id << "\r\n\r\n"
        << body;
    return oss.str();
}

std::string TextCodec::encodeError(uint32_t req_id, uint64_t client_id,
                                  int http_status, const std::string& err_str, const std::string& msg) {
    nlohmann::json j;
    j["error"] = err_str;
    j["message"] = msg;
    std::string body = j.dump();

    std::ostringstream oss;
    oss << "HTTP/1.1 " << http_status << " Error\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Req-ID: " << req_id << "\r\n"
        << "Client-ID: " << client_id << "\r\n\r\n"
        << body;
    return oss.str();
}

bool TextCodec::parseHttpFrame(std::string& stream_buf, TextHttpFrame& out_frame) {
    size_t header_end = stream_buf.find("\r\n\r\n");
    size_t delim_len = 4;
    if (header_end == std::string::npos) {
        header_end = stream_buf.find("\n\n");
        delim_len = 2;
    }
    if (header_end == std::string::npos) {
        return false;
    }

    std::string header_part = stream_buf.substr(0, header_end);
    std::istringstream iss(header_part);
    std::string start_line;
    if (!std::getline(iss, start_line)) {
        return false;
    }
    if (!start_line.empty() && start_line.back() == '\r') {
        start_line.pop_back();
    }

    // Parse start line
    std::istringstream sl_iss(start_line);
    std::string p1, p2, p3;
    sl_iss >> p1 >> p2 >> p3;

    out_frame.headers.clear();
    if (p1.rfind("HTTP/", 0) == 0) {
        // Response: HTTP/1.1 200 OK
        out_frame.status_code = std::stoi(p2);
    } else {
        // Request: POST /order/bid HTTP/1.1
        out_frame.method = p1;
        out_frame.path = p2;
    }

    std::string line;
    size_t content_length = 0;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string k = line.substr(0, colon);
            std::string v = line.substr(colon + 1);
            while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(v.begin());
            std::string k_lower = k;
            std::transform(k_lower.begin(), k_lower.end(), k_lower.begin(), ::tolower);
            out_frame.headers[k_lower] = v;
            if (k_lower == "content-length") {
                content_length = std::stoul(v);
            }
        }
    }

    size_t total_msg_len = header_end + delim_len + content_length;
    if (stream_buf.size() < total_msg_len) {
        return false; // Wait for body data
    }

    out_frame.body = stream_buf.substr(header_end + delim_len, content_length);
    stream_buf.erase(0, total_msg_len);
    return true;
}

} // namespace codec
