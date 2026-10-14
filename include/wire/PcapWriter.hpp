#pragma once

#include "wire/Endian.hpp"
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <chrono>

namespace wire {

class PcapWriter {
public:
    explicit PcapWriter(const std::string& filename) {
        file_.open(filename, std::ios::binary | std::ios::trunc);
        if (file_.is_open()) {
            writeGlobalHeader();
        }
    }

    ~PcapWriter() {
        close();
    }

    bool isOpen() const { return file_.is_open(); }

    void close() {
        if (file_.is_open()) {
            file_.flush();
            file_.close();
        }
    }

    void writeUdpPacket(const void* payload, size_t len,
                        uint32_t src_ip_net, uint16_t src_port,
                        uint32_t dst_ip_net, uint16_t dst_port,
                        uint64_t ts_us = 0) {
        if (!file_.is_open()) return;
        if (ts_us == 0) {
            ts_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        uint32_t ts_sec = static_cast<uint32_t>(ts_us / 1000000ULL);
        uint32_t ts_usec = static_cast<uint32_t>(ts_us % 1000000ULL);

        std::vector<uint8_t> frame;
        frame.reserve(14 + 20 + 8 + len);

        // 1. Ethernet Header (14 bytes)
        uint8_t eth_dst[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
        uint8_t eth_src[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x02};
        uint16_t eth_type = hostToNetwork16(0x0800); // IPv4
        frame.insert(frame.end(), eth_dst, eth_dst + 6);
        frame.insert(frame.end(), eth_src, eth_src + 6);
        appendU16(frame, eth_type);

        // 2. IPv4 Header (20 bytes)
        size_t ip_start = frame.size();
        frame.push_back(0x45); // Ver 4, IHL 5
        frame.push_back(0x00); // DSCP/ECN
        uint16_t total_len = static_cast<uint16_t>(20 + 8 + len);
        appendU16(frame, hostToNetwork16(total_len));
        appendU16(frame, hostToNetwork16(12345)); // Identification
        appendU16(frame, 0); // Flags & Frag offset
        frame.push_back(64); // TTL
        frame.push_back(17); // Protocol UDP
        appendU16(frame, 0); // Checksum placeholder
        appendU32(frame, src_ip_net);
        appendU32(frame, dst_ip_net);

        uint16_t ip_cksum = calculateChecksum(&frame[ip_start], 20);
        std::memcpy(&frame[ip_start + 10], &ip_cksum, 2);

        // 3. UDP Header (8 bytes)
        appendU16(frame, hostToNetwork16(src_port));
        appendU16(frame, hostToNetwork16(dst_port));
        appendU16(frame, hostToNetwork16(static_cast<uint16_t>(8 + len)));
        appendU16(frame, 0); // Checksum optional

        // Payload
        const uint8_t* p = reinterpret_cast<const uint8_t*>(payload);
        frame.insert(frame.end(), p, p + len);

        // PCAP Record Header (16 bytes)
        uint32_t incl_len = static_cast<uint32_t>(frame.size());
        uint32_t orig_len = incl_len;
        file_.write(reinterpret_cast<const char*>(&ts_sec), 4);
        file_.write(reinterpret_cast<const char*>(&ts_usec), 4);
        file_.write(reinterpret_cast<const char*>(&incl_len), 4);
        file_.write(reinterpret_cast<const char*>(&orig_len), 4);
        file_.write(reinterpret_cast<const char*>(frame.data()), frame.size());
        file_.flush();
    }

    void writeTcpPacket(const void* payload, size_t len,
                        uint32_t src_ip_net, uint16_t src_port,
                        uint32_t dst_ip_net, uint16_t dst_port,
                        uint32_t seq, uint32_t ack, uint8_t flags,
                        uint64_t ts_us = 0) {
        if (!file_.is_open()) return;
        if (ts_us == 0) {
            ts_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        }

        uint32_t ts_sec = static_cast<uint32_t>(ts_us / 1000000ULL);
        uint32_t ts_usec = static_cast<uint32_t>(ts_us % 1000000ULL);

        std::vector<uint8_t> frame;
        frame.reserve(14 + 20 + 20 + len);

        // Ethernet
        uint8_t eth_dst[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x01};
        uint8_t eth_src[6] = {0x00, 0x00, 0x5e, 0x00, 0x53, 0x02};
        uint16_t eth_type = hostToNetwork16(0x0800);
        frame.insert(frame.end(), eth_dst, eth_dst + 6);
        frame.insert(frame.end(), eth_src, eth_src + 6);
        appendU16(frame, eth_type);

        // IPv4
        size_t ip_start = frame.size();
        frame.push_back(0x45);
        frame.push_back(0x00);
        uint16_t total_len = static_cast<uint16_t>(20 + 20 + len);
        appendU16(frame, hostToNetwork16(total_len));
        appendU16(frame, hostToNetwork16(54321));
        appendU16(frame, 0);
        frame.push_back(64);
        frame.push_back(6); // TCP
        appendU16(frame, 0);
        appendU32(frame, src_ip_net);
        appendU32(frame, dst_ip_net);

        uint16_t ip_cksum = calculateChecksum(&frame[ip_start], 20);
        std::memcpy(&frame[ip_start + 10], &ip_cksum, 2);

        // TCP Header (20 bytes)
        size_t tcp_start = frame.size();
        appendU16(frame, hostToNetwork16(src_port));
        appendU16(frame, hostToNetwork16(dst_port));
        appendU32(frame, hostToNetwork32(seq));
        appendU32(frame, hostToNetwork32(ack));
        frame.push_back(0x50); // Data offset (5 * 4 = 20B)
        frame.push_back(flags);
        appendU16(frame, hostToNetwork16(65535)); // Window
        appendU16(frame, 0); // Checksum placeholder
        appendU16(frame, 0); // Urgent pointer

        if (len > 0 && payload != nullptr) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(payload);
            frame.insert(frame.end(), p, p + len);
        }

        // Pseudo-header TCP checksum
        uint16_t tcp_len = static_cast<uint16_t>(20 + len);
        std::vector<uint8_t> pseudo;
        appendU32(pseudo, src_ip_net);
        appendU32(pseudo, dst_ip_net);
        pseudo.push_back(0);
        pseudo.push_back(6);
        appendU16(pseudo, hostToNetwork16(tcp_len));
        pseudo.insert(pseudo.end(), frame.begin() + tcp_start, frame.end());

        uint16_t tcp_cksum = calculateChecksum(pseudo.data(), pseudo.size());
        std::memcpy(&frame[tcp_start + 16], &tcp_cksum, 2);

        // PCAP Record Header (16 bytes)
        uint32_t incl_len = static_cast<uint32_t>(frame.size());
        uint32_t orig_len = incl_len;
        file_.write(reinterpret_cast<const char*>(&ts_sec), 4);
        file_.write(reinterpret_cast<const char*>(&ts_usec), 4);
        file_.write(reinterpret_cast<const char*>(&incl_len), 4);
        file_.write(reinterpret_cast<const char*>(&orig_len), 4);
        file_.write(reinterpret_cast<const char*>(frame.data()), frame.size());
        file_.flush();
    }

private:
    void writeGlobalHeader() {
        uint32_t magic = 0xa1b2c3d4;
        uint16_t v_maj = 2, v_min = 4;
        int32_t thiszone = 0;
        uint32_t sigfigs = 0, snaplen = 65535, network = 1;

        file_.write(reinterpret_cast<const char*>(&magic), 4);
        file_.write(reinterpret_cast<const char*>(&v_maj), 2);
        file_.write(reinterpret_cast<const char*>(&v_min), 2);
        file_.write(reinterpret_cast<const char*>(&thiszone), 4);
        file_.write(reinterpret_cast<const char*>(&sigfigs), 4);
        file_.write(reinterpret_cast<const char*>(&snaplen), 4);
        file_.write(reinterpret_cast<const char*>(&network), 4);
        file_.flush();
    }

    void appendU16(std::vector<uint8_t>& buf, uint16_t val) {
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
    }

    void appendU32(std::vector<uint8_t>& buf, uint32_t val) {
        buf.push_back(static_cast<uint8_t>(val & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 8) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 16) & 0xFF));
        buf.push_back(static_cast<uint8_t>((val >> 24) & 0xFF));
    }

    uint16_t calculateChecksum(const uint8_t* data, size_t length) {
        uint32_t sum = 0;
        for (size_t i = 0; i < length / 2; ++i) {
            uint16_t word = static_cast<uint16_t>(data[2 * i]) | (static_cast<uint16_t>(data[2 * i + 1]) << 8);
            sum += word;
        }
        if (length % 2 != 0) {
            sum += data[length - 1];
        }
        while (sum >> 16) {
            sum = (sum & 0xFFFF) + (sum >> 16);
        }
        return static_cast<uint16_t>(~sum);
    }

    std::ofstream file_;
};

} // namespace wire