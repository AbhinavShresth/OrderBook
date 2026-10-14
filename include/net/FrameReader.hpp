#pragma once

#include "wire/Messages.hpp"
#include <vector>
#include <cstdint>
#include <cstddef>

namespace net {

constexpr size_t MAX_FRAME_LEN = 64 * 1024; // 64 KiB maximum frame payload size (D20, NFR-4)

struct Frame {
    wire::FrameHeader header;
    std::vector<uint8_t> payload;
};

class FrameReader {
public:
    FrameReader() = default;

    // Feed raw bytes read from the socket
    void feed(const uint8_t* data, size_t len) {
        buf_.insert(buf_.end(), data, data + len);
    }

    // Extract next complete frame if available. Returns true if frame was read.
    // Throws std::runtime_error if protocol is violated (e.g., bad version, frame too large).
    bool extractFrame(Frame& out_frame);

    size_t bufferedBytes() const { return buf_.size(); }
    void clear() { buf_.clear(); }

private:
    std::vector<uint8_t> buf_;
};

} // namespace net
