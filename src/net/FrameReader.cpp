#include "net/FrameReader.hpp"
#include "codec/BinaryCodec.hpp"
#include "wire/ByteReaderWriter.hpp"
#include <stdexcept>

namespace net {

bool FrameReader::extractFrame(Frame& out_frame) {
    if (buf_.size() < wire::FRAME_HEADER_LEN) {
        return false;
    }

    wire::ByteReader header_reader(buf_.data(), wire::FRAME_HEADER_LEN);
    wire::FrameHeader hdr;
    if (!codec::BinaryCodec::decodeHeader(header_reader, hdr)) {
        return false;
    }

    if (hdr.version != 1) {
        throw std::runtime_error("FrameReader: unsupported protocol version");
    }

    if (hdr.length > MAX_FRAME_LEN) {
        throw std::runtime_error("FrameReader: frame length exceeds MAX_FRAME_LEN");
    }

    size_t total_frame_len = wire::FRAME_HEADER_LEN + hdr.length;
    if (buf_.size() < total_frame_len) {
        return false; // Incomplete frame
    }

    out_frame.header = hdr;
    out_frame.payload.assign(
        buf_.begin() + wire::FRAME_HEADER_LEN,
        buf_.begin() + total_frame_len
    );

    buf_.erase(buf_.begin(), buf_.begin() + total_frame_len);
    return true;
}

} // namespace net
