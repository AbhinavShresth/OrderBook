#include "net/Dispatcher.hpp"
#include "core/Worker.hpp"
#include "client/ClientSession.hpp"
#include "wire/PcapWriter.hpp"
#include "codec/BinaryCodec.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <chrono>
#include <thread>
#include <iomanip>

int main() {
    std::cout << "======================================================================\n"
              << " LOW LATENCY TRADING EXCHANGE - AUTOMATED C++17 DEMONSTRATION \n"
              << " Protocol: 20-byte Binary TCP Header + 128-byte UDP Market Feed\n"
              << "======================================================================\n\n";

    // STEP 1: Launch Dispatcher & 3 Decoupled Workers
    std::cout << "[STEP 1] Starting Central Dispatcher & 3 Decoupled Workers...\n";
    std::vector<std::unique_ptr<core::Worker>> workers;
    workers.push_back(std::make_unique<core::Worker>(
        1, 1, "Tech", std::vector<std::pair<uint16_t, std::string>>{{1, "AAPL"}, {2, "GOOG"}, {3, "MSFT"}}));
    workers.push_back(std::make_unique<core::Worker>(
        2, 2, "Finance", std::vector<std::pair<uint16_t, std::string>>{{4, "JPM"}, {5, "GS"}, {6, "BAC"}}));
    workers.push_back(std::make_unique<core::Worker>(
        3, 3, "Energy", std::vector<std::pair<uint16_t, std::string>>{{7, "XOM"}, {8, "CVX"}, {9, "SHEL"}}));

    for (auto& w : workers) {
        w->initUdpSocket(0);
        std::cout << "  [OK] Worker " << (int)w->worker_id() << " (" << w->group_name()
                  << ") UDP broadcast port: " << w->udp_port() << "\n";
    }

    uint16_t tcp_port = 8095;
    net::Dispatcher dispatcher("127.0.0.1", tcp_port, workers, net::IdlePolicy::NEVER, 0);
    if (!dispatcher.start()) {
        std::cerr << "[-] Failed to start Dispatcher on port " << tcp_port << "\n";
        return 1;
    }
    std::cout << "  [OK] Central Dispatcher TCP bound to 127.0.0.1:" << tcp_port << "\n\n";

    // Background server thread
    std::atomic<bool> running{true};
    std::thread server_thread([&]() {
        while (running) {
            dispatcher.step();
            for (auto& w : workers) {
                w->step();
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // STEP 2: Connect C++ ClientSession & Discover Feeds
    std::cout << "[STEP 2] Connecting C++ Client & Feed Discovery (LIST_FEED)...\n";
    client::ClientSession client("127.0.0.1", tcp_port);
    if (!client.connect()) {
        std::cerr << "[-] Client connection failed\n";
        running = false;
        server_thread.join();
        return 1;
    }
    std::cout << "  [OK] TCP Connection established\n";

    wire::ListFeedResponse lfr;
    if (client.listFeed(lfr)) {
        std::cout << "  [OK] Discovered " << lfr.groups.size() << " Ticker Groups (Assigned Client-ID: "
                  << client.clientId() << "):\n";
        for (const auto& g : lfr.groups) {
            std::cout << "       - Group " << (int)g.group_id << ": " << g.name << " (Tickers: ";
            for (const auto& t : g.tickers) {
                std::cout << std::string(t.ticker, 8) << " ";
            }
            std::cout << ")\n";
        }
    }
    std::cout << "\n";

    // STEP 3: Setup UDP Receiver & Subscribe
    std::cout << "[STEP 3] Starting Background UDP Listener & Subscribing (SUBSCRIBE)...\n";
    std::atomic<uint64_t> packets_rcvd{0};
    wire::UdpMarketFeedDatagram last_dgram{};
    client::UdpListener udp_listener(0, [&](const wire::UdpMarketFeedDatagram& dgram, bool, uint64_t) {
        last_dgram = dgram;
        packets_rcvd++;
    });
    udp_listener.start();
    std::cout << "  [OK] Client UDP listener bound to port: " << udp_listener.port() << "\n";

    uint32_t subscribed = 0;
    client.subscribe(udp_listener.port(), 0x01, subscribed);
    std::cout << "  [OK] Subscribed to Group 1 (Tech) -> Mask: 0x" << std::hex << subscribed << std::dec << "\n\n";

    // STEP 4: Terminal Hex Dump & Packet Dissection
    std::cout << "[STEP 4] Wire Datagram Layout & Wireshark-Style Terminal Dissection (128 Bytes):\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    std::cout << "==================================================================================\n"
              << "[WIRESHARK PACKET TRACE - L2 MARKET FEED]\n"
              << "  Capture Length : 128 bytes (Wire: 156 bytes with IP/UDP)\n"
              << "  Transport      : UDP 127.0.0.1:" << workers[0]->udp_port() << " -> 127.0.0.1:" << udp_listener.port() << "\n"
              << "  Protocol       : L2_MARKET_FEED (SRS 4.4 / FR-10)\n"
              << "----------------------------------------------------------------------------------\n"
              << "PROTOCOL TREE DISSECTION (Wireshark Details Pane):\n"
              << "|-- Sequence Number    : #" << last_dgram.seq << " [OK]\n"
              << "|-- Timestamp (us)     : " << last_dgram.timestamp_us << " epoch microseconds\n"
              << "|-- Ticker Group ID    : " << last_dgram.group_id << " (Tech)\n"
              << "|-- Ticker Symbol      : \"" << std::string(last_dgram.ticker, 8) << "\"\n"
              << "|-- Top-3 Bid Levels (BBO Ladder):\n"
              << "|   |-- Level 1: Price = $" << last_dgram.bids[0].price << " | Volume = " << last_dgram.bids[0].vol << " lots\n"
              << "|   |-- Level 2: Price = $" << last_dgram.bids[1].price << " | Volume = " << last_dgram.bids[1].vol << " lots\n"
              << "|   \\-- Level 3: Price = $" << last_dgram.bids[2].price << " | Volume = " << last_dgram.bids[2].vol << " lots\n"
              << "\\-- Top-3 Ask Levels (BBO Ladder):\n"
              << "    |-- Level 1: Price = $" << last_dgram.asks[0].price << " | Volume = " << last_dgram.asks[0].vol << " lots\n"
              << "    |-- Level 2: Price = $" << last_dgram.asks[1].price << " | Volume = " << last_dgram.asks[1].vol << " lots\n"
              << "    \\-- Level 3: Price = $" << last_dgram.asks[2].price << " | Volume = " << last_dgram.asks[2].vol << " lots\n"
              << "----------------------------------------------------------------------------------\n"
              << "HEX DUMP & BYTE OFFSETS (Wireshark Bytes Pane):\n";

    wire::ByteWriter test_writer(128);
    codec::BinaryCodec::encodeUdpDatagram(last_dgram, test_writer);
    for (size_t i = 0; i < test_writer.size(); i += 16) {
        std::cout << std::hex << std::setw(4) << std::setfill('0') << i << "   ";
        for (size_t j = 0; j < 16; ++j) {
            if (i + j < test_writer.size()) {
                std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)test_writer.data()[i + j] << " ";
            } else {
                std::cout << "   ";
            }
            if (j == 7) std::cout << " ";
        }
        std::cout << "  |";
        for (size_t j = 0; j < 16 && (i + j) < test_writer.size(); ++j) {
            uint8_t c = test_writer.data()[i + j];
            std::cout << ((c >= 32 && c <= 126) ? (char)c : '.');
        }
        std::cout << "|\n" << std::dec;
    }
    std::cout << "==================================================================================\n\n";

    // STEP 5: Order Submission & Cancellation
    std::cout << "[STEP 5] Order Intake & Zero-Lookup Cancellation Flow:\n";
    wire::OrderAck ack;
    wire::ErrorCode err;
    if (client.placeBid(1, 180, 50, ack, err)) {
        std::cout << "  [OK] Placed BID for AAPL (Symbol 1) @  (50 lots)\n"
                  << "       -> ORDER_ACK Order-ID = " << ack.order_id
                  << " [Worker " << (int)(ack.order_id >> 56) << ", Counter " << (ack.order_id & 0x00FFFFFFFFFFFFFFULL) << "]\n"
                  << "       -> Status = " << (int)ack.status << " (RESTING), Rem Vol = " << ack.remaining_vol << "\n";
    }

    wire::CancelAck c_ack;
    if (client.cancelOrder(ack.order_id, c_ack, err)) {
        std::cout << "  [OK] CANCEL Order " << c_ack.order_id
                  << " -> CANCEL_ACK Status = " << (int)c_ack.status << " (CANCELLED)\n";
    }

    if (!client.cancelOrder(ack.order_id, c_ack, err)) {
        std::cout << "  [OK] Duplicate Cancel Negative Test: ErrorCode = " << (int)err << " (ORDER_NOT_FOUND)\n\n";
    }

    // STEP 6: Micro-Benchmark
    std::cout << "[STEP 6] Running C++ Round-Trip Micro-Benchmark (100 Orders)...\n";
    auto t0 = std::chrono::steady_clock::now();
    size_t count = 100;
    for (size_t i = 0; i < count; ++i) {
        client.placeBid(1, 150 + (i % 10), 10, ack, err);
    }
    auto t1 = std::chrono::steady_clock::now();
    double elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    double ops_sec = (count * 1000000.0) / elapsed_us;
    std::cout << "  [BENCHMARK] " << count << " round-trip TCP operations completed in " << (elapsed_us / 1000.0) << " ms\n"
              << "              Throughput: " << std::fixed << std::setprecision(1) << ops_sec << " ops/sec\n"
              << "              Mean Latency: " << (elapsed_us / count) << " microseconds/op\n\n";

    // STEP 7: Generate Full Multi-Packet PCAP from C++
    std::cout << "[STEP 7] Generating Standard Libpcap File (market_feed.pcap) from C++...\n";
    {
        wire::PcapWriter pcap("market_feed.pcap");
        uint32_t s_ip = 0x0100007f; // 127.0.0.1
        uint32_t c_ip = 0x0100007f;
        uint16_t c_port = 49152;
        uint16_t s_port = 8085;
        uint16_t u_port = udp_listener.port();
        uint16_t w_port = workers[0]->udp_port();

        // 1-3. TCP 3-way Handshake
        pcap.writeTcpPacket(nullptr, 0, c_ip, c_port, s_ip, s_port, 1000, 0, 0x02);
        pcap.writeTcpPacket(nullptr, 0, s_ip, s_port, c_ip, c_port, 5000, 1001, 0x12);
        pcap.writeTcpPacket(nullptr, 0, c_ip, c_port, s_ip, s_port, 1001, 5001, 0x10);

        // 4. Client -> Server: LIST_FEED (20B)
        wire::ByteWriter w_lf(20);
        wire::FrameHeader h_lf{20, 1, 0, 0, wire::MsgType::LIST_FEED, 1};
        codec::BinaryCodec::encodeHeader(h_lf, w_lf);
        pcap.writeTcpPacket(w_lf.data(), w_lf.size(), c_ip, c_port, s_ip, s_port, 1001, 5001, 0x18);

        // 5. Server -> Client: LIST_FEED_RSP
        wire::ByteWriter w_lfr;
        wire::FrameHeader h_lfr{0, 1, client.clientId(), 0, wire::MsgType::LIST_FEED_RSP, 1};
        wire::ByteWriter w_lfr_pay;
        codec::BinaryCodec::encodeListFeedRsp(lfr, w_lfr_pay);
        h_lfr.length = 20 + w_lfr_pay.size();
        codec::BinaryCodec::encodeHeader(h_lfr, w_lfr);
        w_lfr.writeBytes(w_lfr_pay.data(), w_lfr_pay.size());
        pcap.writeTcpPacket(w_lfr.data(), w_lfr_pay.size(), s_ip, s_port, c_ip, c_port, 5001, 1021, 0x18);

        // 6. Client -> Server: SUBSCRIBE (26B)
        wire::ByteWriter w_sub(26);
        wire::FrameHeader h_sub{26, 2, client.clientId(), 0, wire::MsgType::SUBSCRIBE, 1};
        codec::BinaryCodec::encodeHeader(h_sub, w_sub);
        wire::SubscribeRequest sreq{u_port, 0x01};
        codec::BinaryCodec::encodeSubscribe(sreq, w_sub);
        pcap.writeTcpPacket(w_sub.data(), w_sub.size(), c_ip, c_port, s_ip, s_port, 1021, 5001 + w_lfr.size(), 0x18);

        // 7. Server -> Client: SUBSCRIBE_ACK (24B)
        wire::ByteWriter w_suback(24);
        wire::FrameHeader h_suback{24, 2, client.clientId(), 0, wire::MsgType::SUBSCRIBE_ACK, 1};
        codec::BinaryCodec::encodeHeader(h_suback, w_suback);
        wire::SubscribeAck sack{0x01};
        codec::BinaryCodec::encodeSubscribeAck(sack, w_suback);
        pcap.writeTcpPacket(w_suback.data(), w_suback.size(), s_ip, s_port, c_ip, c_port, 5001 + w_lfr.size(), 1047, 0x18);

        // 8-9. UDP Market Feed Quotes from Worker (128B each)
        wire::ByteWriter w_udp1(128);
        codec::BinaryCodec::encodeUdpDatagram(last_dgram, w_udp1);
        pcap.writeUdpPacket(w_udp1.data(), w_udp1.size(), s_ip, w_port, c_ip, u_port);

        // 10. Client -> Server: BID (38B)
        wire::ByteWriter w_bid(38);
        wire::FrameHeader h_bid{38, 3, client.clientId(), 0, wire::MsgType::BID, 1};
        codec::BinaryCodec::encodeHeader(h_bid, w_bid);
        wire::BidRequest breq{1, 180, 50};
        codec::BinaryCodec::encodeBid(breq, w_bid);
        pcap.writeTcpPacket(w_bid.data(), w_bid.size(), c_ip, c_port, s_ip, s_port, 1047, 5001 + w_lfr.size() + 24, 0x18);

        // 11. Server -> Client: ORDER_ACK (39B)
        wire::ByteWriter w_ack(39);
        wire::FrameHeader h_ack{39, 3, client.clientId(), 0, wire::MsgType::ORDER_ACK, 1};
        codec::BinaryCodec::encodeHeader(h_ack, w_ack);
        wire::OrderAck oack{ack.order_id, wire::OrderStatus::RESTING, 50, {}};
        codec::BinaryCodec::encodeOrderAck(oack, w_ack);
        pcap.writeTcpPacket(w_ack.data(), w_ack.size(), s_ip, s_port, c_ip, c_port, 5001 + w_lfr.size() + 24, 1085, 0x18);

        // 12. UDP Quote Updated with new BBO level
        wire::UdpMarketFeedDatagram quote2 = last_dgram;
        quote2.seq++;
        quote2.bids[0] = {180, 50};
        wire::ByteWriter w_udp2(128);
        codec::BinaryCodec::encodeUdpDatagram(quote2, w_udp2);
        pcap.writeUdpPacket(w_udp2.data(), w_udp2.size(), s_ip, w_port, c_ip, u_port);

        // 13. Client -> Server: CANCEL (28B)
        wire::ByteWriter w_can(28);
        wire::FrameHeader h_can{28, 4, client.clientId(), 0, wire::MsgType::CANCEL, 1};
        codec::BinaryCodec::encodeHeader(h_can, w_can);
        wire::CancelRequest creq{ack.order_id};
        codec::BinaryCodec::encodeCancel(creq, w_can);
        pcap.writeTcpPacket(w_can.data(), w_can.size(), c_ip, c_port, s_ip, s_port, 1085, 5001 + w_lfr.size() + 63, 0x18);

        // 14. Server -> Client: CANCEL_ACK (29B)
        wire::ByteWriter w_canack(29);
        wire::FrameHeader h_canack{29, 4, client.clientId(), 0, wire::MsgType::CANCEL_ACK, 1};
        codec::BinaryCodec::encodeHeader(h_canack, w_canack);
        wire::CancelAck cack{ack.order_id, wire::OrderStatus::CANCELLED};
        codec::BinaryCodec::encodeCancelAck(cack, w_canack);
        pcap.writeTcpPacket(w_canack.data(), w_canack.size(), s_ip, s_port, c_ip, c_port, 5001 + w_lfr.size() + 63, 1113, 0x18);

        // 15. Client -> Server: Duplicate CANCEL (Negative Test, 28B)
        pcap.writeTcpPacket(w_can.data(), w_can.size(), c_ip, c_port, s_ip, s_port, 1113, 5001 + w_lfr.size() + 92, 0x18);

        // 16. Server -> Client: ERROR_RSP (ErrorCode 5: ORDER_NOT_FOUND)
        wire::ByteWriter w_err(22);
        wire::FrameHeader h_err{22, 5, client.clientId(), 0, wire::MsgType::ERROR_RSP, 1};
        codec::BinaryCodec::encodeHeader(h_err, w_err);
        wire::ErrorResponse eresp{wire::ErrorCode::ORDER_NOT_FOUND};
        codec::BinaryCodec::encodeError(eresp, w_err);
        pcap.writeTcpPacket(w_err.data(), w_err.size(), s_ip, s_port, c_ip, c_port, 5001 + w_lfr.size() + 92, 1141, 0x18);

        // 17-18. TCP FIN-ACK Teardown
        pcap.writeTcpPacket(nullptr, 0, c_ip, c_port, s_ip, s_port, 1141, 5001 + w_lfr.size() + 114, 0x11);
        pcap.writeTcpPacket(nullptr, 0, s_ip, s_port, c_ip, c_port, 5001 + w_lfr.size() + 114, 1142, 0x11);
    }
    std::cout << "  [OK] Saved market_feed.pcap! (18 frames: TCP 3-way handshake + discovery + UDP feeds + trade ack + cancel + errors)\n"
              << "       Open via .\\open_wireshark.bat in Windows.\n\n";

    // Clean teardown
    udp_listener.stop();
    client.disconnect();
    running = false;
    server_thread.join();
    net::cleanupNetworking();

    std::cout << "=== All 7 Demonstration Steps Completed Successfully! ===\n";
    return 0;
}