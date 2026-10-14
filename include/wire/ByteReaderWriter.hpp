#pragma once

#include "wire/Endian.hpp"
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <stdexcept>

namespace wire {

class ByteWriter {
public:
    explicit ByteWriter(size_t reserve_bytes = 128) {
        buf_.reserve(reserve_bytes);
    }

    void writeU8(uint8_t val) {
        buf_.push_back(val);
    }

    void writeU16(uint16_t val) {
        uint16_t be = hostToNetwork16(val);
        appendBytes(reinterpret_cast<const uint8_t*>(&be), 2);
    }

    void writeU32(uint32_t val) {
        uint32_t be = hostToNetwork32(val);
        appendBytes(reinterpret_cast<const uint8_t*>(&be), 4);
    }

    void writeU64(uint64_t val) {
        uint64_t be = hostToNetwork64(val);
        appendBytes(reinterpret_cast<const uint8_t*>(&be), 8);
    }

    void writeBytes(const void* src, size_t len) {
        appendBytes(reinterpret_cast<const uint8_t*>(src), len);
    }

    void writeStringPadded(const std::string& str, size_t fixed_len) {
        size_t written = 0;
        for (; written < str.size() && written < fixed_len; ++written) {
            buf_.push_back(static_cast<uint8_t>(str[written]));
        }
        for (; written < fixed_len; ++written) {
            buf_.push_back(0);
        }
    }

    const std::vector<uint8_t>& buffer() const { return buf_; }
    std::vector<uint8_t>& buffer() { return buf_; }
    const uint8_t* data() const { return buf_.data(); }
    size_t size() const { return buf_.size(); }

    void clear() { buf_.clear(); }

private:
    void appendBytes(const uint8_t* p, size_t n) {
        buf_.insert(buf_.end(), p, p + n);
    }

    std::vector<uint8_t> buf_;
};

class ByteReader {
public:
    ByteReader(const uint8_t* data, size_t len)
        : data_(data), len_(len), offset_(0) {}

    size_t remaining() const {
        return (offset_ <= len_) ? (len_ - offset_) : 0;
    }

    size_t offset() const { return offset_; }

    uint8_t readU8() {
        checkBounds(1);
        return data_[offset_++];
    }

    uint16_t readU16() {
        checkBounds(2);
        uint16_t be;
        std::memcpy(&be, data_ + offset_, 2);
        offset_ += 2;
        return networkToHost16(be);
    }

    uint32_t readU32() {
        checkBounds(4);
        uint32_t be;
        std::memcpy(&be, data_ + offset_, 4);
        offset_ += 4;
        return networkToHost32(be);
    }

    uint64_t readU64() {
        checkBounds(8);
        uint64_t be;
        std::memcpy(&be, data_ + offset_, 8);
        offset_ += 8;
        return networkToHost64(be);
    }

    void readBytes(void* dst, size_t n) {
        checkBounds(n);
        std::memcpy(dst, data_ + offset_, n);
        offset_ += n;
    }

    std::string readString(size_t n) {
        checkBounds(n);
        std::string s(reinterpret_cast<const char*>(data_ + offset_), n);
        offset_ += n;
        return s;
    }

private:
    void checkBounds(size_t needed) const {
        if (offset_ + needed > len_) {
            throw std::runtime_error("ByteReader: bounds check failed");
        }
    }

    const uint8_t* data_;
    size_t len_;
    size_t offset_;
};

} // namespace wire
