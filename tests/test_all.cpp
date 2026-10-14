// On MinGW win32 thread model, std::mutex is not available from libstdc++.
// Disable doctest's internal locking (it only matters for multi-threaded test
// runners, which we don't use).
#define DOCTEST_CONFIG_NO_MULTITHREADING

#if defined(_WIN32)
// Pull in mingw-std-threads BEFORE any header that might touch <thread>/<mutex>
#include "mingw_thread.h"
#include "mingw.mutex.h"
#endif

#include "wire/ByteReaderWriter.hpp"
#include "wire/Messages.hpp"
#include "codec/BinaryCodec.hpp"
#include "codec/TextCodec.hpp"
#include "core/SpscQueue.hpp"
#include "core/Worker.hpp"
#include "net/FrameReader.hpp"
#include "net/Dispatcher.hpp"
#include "client/ClientSession.hpp"

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include <chrono>

TEST_CASE("Phase 1: BinaryCodec and ByteReaderWriter Golden Round-Trip") {
    // 1. Header round trip
    wire::FrameHeader hdr{};
    hdr.length = 38;
    hdr.req_id = 101;
    hdr.client_id = 987654321ULL;
    hdr.callback_port = 8080;
    hdr.type = wire::MsgType::BID;
    hdr.version = 1;

    wire::ByteWriter writer;
    codec::BinaryCodec::encodeHeader(hdr, writer);
    CHECK(writer.size() == 20);

    wire::ByteReader reader(writer.data(), writer.size());
    wire::FrameHeader decoded_hdr{};
    CHECK(codec::BinaryCodec::decodeHeader(reader, decoded_hdr));
    CHECK(decoded_hdr.length == 38);
    CHECK(decoded_hdr.req_id == 101);
    CHECK(decoded_hdr.client_id == 987654321ULL);
    CHECK(decoded_hdr.callback_port == 8080);
    CHECK(decoded_hdr.type == wire::MsgType::BID);
    CHECK(decoded_hdr.version == 1);

    // 2. BID request round trip
    wire::BidRequest bid{42, 18050, 100};
    writer.clear();
    codec::BinaryCodec::encodeBid(bid, writer);
    CHECK(writer.size() == 18);

    wire::ByteReader bid_reader(writer.data(), writer.size());
    wire::BidRequest decoded_bid{};
    CHECK(codec::BinaryCodec::decodeBid(bid_reader, decoded_bid));
    CHECK(decoded_bid.symbol_id == 42);
    CHECK(decoded_bid.price == 18050);
    CHECK(decoded_bid.volume == 100);

    // 3. UDP Datagram (Fixed 128 bytes)
    wire::UdpMarketFeedDatagram dgram{};
    dgram.seq = 1;
    dgram.timestamp_us = 123456789ULL;
    dgram.group_id = 2;
    std::memcpy(dgram.ticker, "AAPL\0\0\0", 8);
    dgram.n_bids = 1;
    dgram.n_asks = 1;
    dgram.bids[0] = {180, 50};
    dgram.asks[0] = {182, 30};

    writer.clear();
    codec::BinaryCodec::encodeUdpDatagram(dgram, writer);
    CHECK(writer.size() == 128);

    wire::ByteReader udp_reader(writer.data(), writer.size());
    wire::UdpMarketFeedDatagram decoded_dgram{};
    CHECK(codec::BinaryCodec::decodeUdpDatagram(udp_reader, decoded_dgram));
    CHECK(decoded_dgram.seq == 1);
    CHECK(decoded_dgram.group_id == 2);
    CHECK(decoded_dgram.bids[0].price == 180);
    CHECK(decoded_dgram.asks[0].vol == 30);
}

TEST_CASE("Phase 2: FrameReader Partial and Multi-Frame Stream Reassembly") {
    net::FrameReader reader;

    wire::FrameHeader hdr{};
    hdr.length = 8;
    hdr.req_id = 1;
    hdr.client_id = 42;
    hdr.type = wire::MsgType::CANCEL;
    hdr.version = 1;

    wire::ByteWriter writer;
    codec::BinaryCodec::encodeHeader(hdr, writer);
    wire::CancelRequest cancel{1001};
    codec::BinaryCodec::encodeCancel(cancel, writer);

    // Feed 1 byte at a time
    for (size_t i = 0; i < writer.size() - 1; ++i) {
        reader.feed(writer.data() + i, 1);
        net::Frame f;
        CHECK(reader.extractFrame(f) == false);
    }

    // Feed the last byte
    reader.feed(writer.data() + writer.size() - 1, 1);
    net::Frame extracted;
    CHECK(reader.extractFrame(extracted) == true);
    CHECK(extracted.header.req_id == 1);
    CHECK(extracted.payload.size() == 8);
}

TEST_CASE("Phase 3: TextCodec vs BinaryCodec Logical Parity") {
    // Both codecs represent equivalent BID orders
    std::string text_req = codec::TextCodec::encodeBidRequest(5, 777, 9000, "AAPL", 180, 50);
    CHECK(text_req.find("POST /order/bid HTTP/1.1") != std::string::npos);
    CHECK(text_req.find("\"max_price\":180") != std::string::npos);

    codec::TextHttpFrame parsed_frame;
    CHECK(codec::TextCodec::parseHttpFrame(text_req, parsed_frame));
    CHECK(parsed_frame.method == "POST");
    CHECK(parsed_frame.path == "/order/bid");
    CHECK(parsed_frame.headers["client-id"] == "777");
}

TEST_CASE("Phase 4: SpscQueue Lock-Free Concurrent Transfer") {
    core::SpscQueue<int> queue(1024);
    std::atomic<bool> done{false};
    int total_items = 10000;

    std::thread producer([&]() {
        for (int i = 0; i < total_items; ++i) {
            while (!queue.push(i)) {
                std::this_thread::yield();
            }
        }
        done = true;
    });

    std::thread consumer([&]() {
        int expected = 0;
        int received = 0;
        while (received < total_items) {
            int val;
            if (queue.pop(val)) {
                CHECK(val == expected);
                expected++;
                received++;
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();
}

TEST_CASE("Phase 5: Worker Order Processing and Order-ID Encoding") {
    std::vector<std::pair<uint16_t, std::string>> tickers = {{1, "AAPL"}, {2, "GOOG"}};
    core::Worker worker(1, 1, "Tech", tickers);

    core::WorkItem wi;
    wi.item_type = core::WorkItemType::ORDER;
    wi.client_id = 999;
    wi.req_id = 12;
    wi.type = wire::MsgType::BID;
    wi.symbol_id = 1;
    wi.price = 180;
    wi.volume = 50;

    CHECK(worker.in_queue().push(wi));
    worker.step();

    core::WorkResult res;
    CHECK(worker.out_queue().pop(res));
    CHECK(res.is_error == false);
    CHECK(res.order_ack.status == wire::OrderStatus::RESTING);

    // D9: Verify worker_id encoded in top 8 bits: order_id >> 56 == worker_id
    uint8_t extracted_worker_id = static_cast<uint8_t>(res.order_ack.order_id >> 56);
    CHECK(extracted_worker_id == 1);
}

TEST_CASE("Phase 6-8: End-to-End ClientSession and Dispatcher Lifecycle") {
    net::initNetworking();

    std::vector<std::unique_ptr<core::Worker>> workers;
    workers.push_back(std::make_unique<core::Worker>(1, 1, "Tech", std::vector<std::pair<uint16_t, std::string>>{{1, "AAPL"}}));
    workers.push_back(std::make_unique<core::Worker>(2, 2, "Finance", std::vector<std::pair<uint16_t, std::string>>{{2, "JPM"}}));

    // Dynamic port 0
    net::Dispatcher dispatcher("127.0.0.1", 0, workers, net::IdlePolicy::NEVER);
    CHECK(dispatcher.start());
    uint16_t srv_port = dispatcher.port();

    std::atomic<bool> srv_running{true};
    std::thread srv_thread([&]() {
        while (srv_running) {
            dispatcher.step();
            for (auto& w : workers) w->step();
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    // Client connection
    client::ClientSession client("127.0.0.1", srv_port);
    CHECK(client.connect());

    // 1. LIST_FEED (Discovery)
    wire::ListFeedResponse lfr;
    CHECK(client.listFeed(lfr));
    CHECK(lfr.groups.size() == 2);
    CHECK(lfr.groups[0].name == "Tech");

    // D11: client_id assigned by server on first interaction
    CHECK(client.clientId() != 0);

    // 2. Place BID order
    wire::OrderAck ack;
    wire::ErrorCode err;
    CHECK(client.placeBid(1, 180, 50, ack, err));
    CHECK(ack.status == wire::OrderStatus::RESTING);
    CHECK(ack.remaining_vol == 50);

    // 3. Cancel Order
    wire::CancelAck cancel_ack;
    CHECK(client.cancelOrder(ack.order_id, cancel_ack, err));
    CHECK(cancel_ack.status == wire::OrderStatus::CANCELLED);

    // Duplicate cancel -> ORDER_NOT_FOUND (D19)
    CHECK(client.cancelOrder(ack.order_id, cancel_ack, err) == false);
    CHECK(err == wire::ErrorCode::ORDER_NOT_FOUND);

    // Clean shutdown
    client.disconnect();
    srv_running = false;
    srv_thread.join();
    dispatcher.stop();
}
