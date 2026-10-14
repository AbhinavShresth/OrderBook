#include "wire/Messages.hpp"
#include "wire/ByteReaderWriter.hpp"
#include "codec/BinaryCodec.hpp"
#include "codec/TextCodec.hpp"
#include "core/Worker.hpp"
#include "net/Dispatcher.hpp"
#include "client/ClientSession.hpp"
#include "nlohmann/json.hpp"

#include <iostream>
#include <iomanip>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <thread>
#include <atomic>
#include <cmath>
#include <sstream>
#include <fstream>
#include <filesystem>

using namespace std::chrono;

void writeCsvSamples(std::ofstream& csv, const std::string& run_id, const std::string& section,
                     const std::string& metric, const std::string& codec, const std::string& scenario,
                     const std::vector<double>& values, const std::string& unit, const std::string& notes = "") {
    for (size_t i = 0; i < values.size(); ++i)
        csv << run_id << ',' << section << ',' << metric << ',' << codec << ',' << scenario << ','
            << i << ',' << std::setprecision(12) << values[i] << ',' << unit << ",\"" << notes << "\"\n";
}

bool decodeTextBid(std::string& input, wire::BidRequest& bid) {
    codec::TextHttpFrame frame;
    if (!codec::TextCodec::parseHttpFrame(input, frame)) return false;
    const auto json = nlohmann::json::parse(frame.body);
    bid.price = json.at("max_price").get<wire::Price>();
    bid.volume = json.at("volume").get<wire::Volume>();
    return true;
}

bool decodeTextAck(std::string& input, wire::OrderAck& ack) {
    codec::TextHttpFrame frame;
    if (!codec::TextCodec::parseHttpFrame(input, frame)) return false;
    const auto json = nlohmann::json::parse(frame.body);
    ack.order_id = json.at("order_id").get<uint64_t>();
    ack.remaining_vol = json.at("remaining_vol").get<wire::Volume>();
    const auto status = json.at("status").get<std::string>();
    ack.status = status == "FILLED" ? wire::OrderStatus::FILLED :
                 status == "PARTIAL_FILL" ? wire::OrderStatus::PARTIAL_FILL : wire::OrderStatus::RESTING;
    ack.trades.clear();
    for (const auto& trade : json.at("trades"))
        ack.trades.push_back({trade.at("trade_id").get<uint64_t>(),
                              trade.at("price").get<wire::Price>(),
                              trade.at("vol").get<wire::Volume>(),
                              trade.at("counterpart_id").get<uint64_t>()});
    return true;
}

std::string sizeRatio(size_t text_bytes, size_t binary_bytes) {
    std::ostringstream result;
    result << std::fixed << std::setprecision(2)
           << (static_cast<double>(text_bytes) / static_cast<double>(binary_bytes)) << "x";
    return result.str();
}

void printSection(int number, const std::string& title) {
    std::cout << "\n[" << number << "] " << title << "\n"
              << std::string(88, '-') << "\n";
}

int main(int argc, char* argv[]) {
    net::initNetworking();

    const std::filesystem::path output_dir = argc > 1 ? argv[1] : "benchmark_results";
    std::filesystem::create_directories(output_dir);
    const auto raw_csv_path = output_dir / "raw_measurements.csv";
    const bool write_csv_header = !std::filesystem::exists(raw_csv_path) || std::filesystem::file_size(raw_csv_path) == 0;
    std::ofstream raw_csv(raw_csv_path, std::ios::out | std::ios::app);
    if (!raw_csv) {
        std::cerr << "Cannot write benchmark CSV in " << output_dir << "\n";
        return 1;
    }
    const std::string run_id = std::to_string(duration_cast<microseconds>(
        system_clock::now().time_since_epoch()).count());
    if (write_csv_header)
        raw_csv << "run_id,section,metric,codec,scenario,sample_index,value,unit,notes\n";

    std::cout << "LOW LATENCY EXCHANGE | BENCHMARK REPORT\n"
              << "Protocol plan: Section 8 | Binary vs HTTP/1.1 + JSON baseline\n"
              << "================================================================================\n";

    // 1. Wire Bytes per Message Type
    printSection(1, "Wire size by message (bytes; measured encodings)");
    
    wire::ByteWriter bw;
    wire::FrameHeader hdr{18, 1, 100, 0, wire::MsgType::BID, 1};
    codec::BinaryCodec::encodeHeader(hdr, bw);
    wire::BidRequest bid{1, 180, 50};
    codec::BinaryCodec::encodeBid(bid, bw);
    size_t bin_bid_len = bw.size();

    bw.clear();
    wire::FrameHeader cancel_hdr{8, 2, 100, 0, wire::MsgType::CANCEL, 1};
    codec::BinaryCodec::encodeHeader(cancel_hdr, bw);
    wire::CancelRequest cancel{1042};
    codec::BinaryCodec::encodeCancel(cancel, bw);
    size_t bin_cancel_len = bw.size();

    bw.clear();
    wire::OrderAck ack0;
    ack0.order_id = 72057594037927937ULL;
    ack0.status = wire::OrderStatus::RESTING;
    ack0.remaining_vol = 50;
    wire::FrameHeader ack_hdr{19, 1, 100, 0, wire::MsgType::ORDER_ACK, 1};
    codec::BinaryCodec::encodeHeader(ack_hdr, bw);
    codec::BinaryCodec::encodeOrderAck(ack0, bw);
    size_t bin_ack0_len = bw.size();

    bw.clear();
    wire::OrderAck ack1 = ack0;
    ack1.trades.push_back({501, 180, 38, 1001});
    wire::FrameHeader ack1_hdr{51, 1, 100, 0, wire::MsgType::ORDER_ACK, 1};
    codec::BinaryCodec::encodeHeader(ack1_hdr, bw);
    codec::BinaryCodec::encodeOrderAck(ack1, bw);
    size_t bin_ack1_len = bw.size();

    bw.clear();
    wire::UdpMarketFeedDatagram dgram{};
    codec::BinaryCodec::encodeUdpDatagram(dgram, bw);
    size_t bin_udp_len = bw.size();

    std::string text_bid = codec::TextCodec::encodeBidRequest(1, 100, 9200, "AAPL", 180, 50);
    size_t text_bid_len = text_bid.size();

    std::string text_cancel = codec::TextCodec::encodeCancelRequest(2, 100, 9200, 1042);
    size_t text_cancel_len = text_cancel.size();

    std::string text_ack0 = codec::TextCodec::encodeOrderAck(1, 100, ack0);
    size_t text_ack0_len = text_ack0.size();

    std::string text_ack1 = codec::TextCodec::encodeOrderAck(1, 100, ack1);
    size_t text_ack1_len = text_ack1.size();

    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "binary", "BID", {static_cast<double>(bin_bid_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "text_http_json", "BID", {static_cast<double>(text_bid_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "binary", "CANCEL", {static_cast<double>(bin_cancel_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "text_http_json", "CANCEL", {static_cast<double>(text_cancel_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "binary", "ORDER_ACK_resting", {static_cast<double>(bin_ack0_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "text_http_json", "ORDER_ACK_resting", {static_cast<double>(text_ack0_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "binary", "ORDER_ACK_1_trade", {static_cast<double>(bin_ack1_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "text_http_json", "ORDER_ACK_1_trade", {static_cast<double>(text_ack1_len)}, "bytes");
    writeCsvSamples(raw_csv, run_id, "wire_size", "frame_bytes", "binary", "UDP_market_data", {static_cast<double>(bin_udp_len)}, "bytes");

    std::cout << std::left << std::setw(29) << "Message"
              << std::right << std::setw(13) << "Binary"
              << std::setw(17) << "Text HTTP/JSON"
              << std::setw(16) << "Text / Binary" << "\n"
              << std::string(75, '-') << "\n";
    auto sizeRow = [](const std::string& name, size_t binary, const std::string& text, const std::string& ratio) {
        std::cout << std::left << std::setw(29) << name << std::right << std::setw(13) << binary
                  << std::setw(17) << text << std::setw(16) << ratio << "\n";
    };
    sizeRow("BID request", bin_bid_len, std::to_string(text_bid_len), sizeRatio(text_bid_len, bin_bid_len));
    sizeRow("CANCEL request", bin_cancel_len, std::to_string(text_cancel_len), sizeRatio(text_cancel_len, bin_cancel_len));
    sizeRow("ORDER_ACK (resting)", bin_ack0_len, std::to_string(text_ack0_len), sizeRatio(text_ack0_len, bin_ack0_len));
    sizeRow("ORDER_ACK (1 trade)", bin_ack1_len, std::to_string(text_ack1_len), sizeRatio(text_ack1_len, bin_ack1_len));
    sizeRow("UDP market-data packet", bin_udp_len, "n/a", "binary only");

    // 2. Codec CPU Cost
    printSection(2, "Codec cost (encode + decode; nanoseconds per message)");
    const int WARMUP = 5000;
    const int ITERS = 100000;

    for (int i = 0; i < WARMUP; ++i) {
        wire::ByteWriter w;
        codec::BinaryCodec::encodeBid({1, 180, 50}, w);
        wire::ByteReader r(w.data(), w.size());
        wire::BidRequest br;
        codec::BinaryCodec::decodeBid(r, br);
    }

    auto t_start = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        wire::ByteWriter w(38);
        wire::FrameHeader fh{18, (uint32_t)i, 100, 0, wire::MsgType::BID, 1};
        codec::BinaryCodec::encodeHeader(fh, w);
        codec::BinaryCodec::encodeBid({1, 180, 50}, w);

        wire::ByteReader r(w.data(), w.size());
        wire::FrameHeader out_fh;
        wire::BidRequest out_br;
        codec::BinaryCodec::decodeHeader(r, out_fh);
        codec::BinaryCodec::decodeBid(r, out_br);
    }
    auto t_end = high_resolution_clock::now();
    double bin_bid_ns = duration_cast<nanoseconds>(t_end - t_start).count() / (double)ITERS;

    t_start = high_resolution_clock::now();
    for (int i = 0; i < ITERS; ++i) {
        wire::ByteWriter w(72);
        wire::FrameHeader fh{51, (uint32_t)i, 100, 0, wire::MsgType::ORDER_ACK, 1};
        codec::BinaryCodec::encodeHeader(fh, w);
        codec::BinaryCodec::encodeOrderAck(ack1, w);

        wire::ByteReader r(w.data(), w.size());
        wire::FrameHeader out_fh;
        wire::OrderAck out_ack;
        codec::BinaryCodec::decodeHeader(r, out_fh);
        codec::BinaryCodec::decodeOrderAck(r, out_ack);
    }
    t_end = high_resolution_clock::now();
    double bin_ack_ns = duration_cast<nanoseconds>(t_end - t_start).count() / (double)ITERS;

    t_start = high_resolution_clock::now();
    for (int i = 0; i < ITERS / 4; ++i) {
        std::string s = codec::TextCodec::encodeBidRequest(i, 100, 9200, "AAPL", 180, 50);
        wire::BidRequest decoded;
        decodeTextBid(s, decoded);
    }
    t_end = high_resolution_clock::now();
    double text_bid_ns = duration_cast<nanoseconds>(t_end - t_start).count() / (double)(ITERS / 4);

    t_start = high_resolution_clock::now();
    for (int i = 0; i < ITERS / 4; ++i) {
        std::string s = codec::TextCodec::encodeOrderAck(i, 100, ack1);
        wire::OrderAck decoded;
        decodeTextAck(s, decoded);
    }
    t_end = high_resolution_clock::now();
    double text_ack_ns = duration_cast<nanoseconds>(t_end - t_start).count() / (double)(ITERS / 4);

    // Batch samples retain variation while keeping timer overhead out of each operation.
    constexpr int CODEC_BATCHES = 100;
    constexpr int CODEC_OPS_PER_BATCH = 1000;
    auto collectCodecSamples = [&](const std::string& kind, const std::string& codec_name) {
        std::vector<double> samples;
        samples.reserve(CODEC_BATCHES);
        for (int batch = 0; batch < CODEC_BATCHES; ++batch) {
            auto begin = high_resolution_clock::now();
            for (int i = 0; i < CODEC_OPS_PER_BATCH; ++i) {
                if (kind == "BID" && codec_name == "binary") {
                    wire::ByteWriter w(38);
                    codec::BinaryCodec::encodeHeader({18, static_cast<uint32_t>(i), 100, 0, wire::MsgType::BID, 1}, w);
                    codec::BinaryCodec::encodeBid({1, 180, 50}, w);
                    wire::ByteReader r(w.data(), w.size());
                    wire::FrameHeader fh; wire::BidRequest req;
                    codec::BinaryCodec::decodeHeader(r, fh); codec::BinaryCodec::decodeBid(r, req);
                } else if (kind == "ORDER_ACK" && codec_name == "binary") {
                    wire::ByteWriter w(71);
                    codec::BinaryCodec::encodeHeader({51, static_cast<uint32_t>(i), 100, 0, wire::MsgType::ORDER_ACK, 1}, w);
                    codec::BinaryCodec::encodeOrderAck(ack1, w);
                    wire::ByteReader r(w.data(), w.size());
                    wire::FrameHeader fh; wire::OrderAck req;
                    codec::BinaryCodec::decodeHeader(r, fh); codec::BinaryCodec::decodeOrderAck(r, req);
                } else if (kind == "BID") {
                    auto s = codec::TextCodec::encodeBidRequest(i, 100, 9200, "AAPL", 180, 50);
                    wire::BidRequest decoded; decodeTextBid(s, decoded);
                } else {
                    auto s = codec::TextCodec::encodeOrderAck(i, 100, ack1);
                    wire::OrderAck decoded; decodeTextAck(s, decoded);
                }
            }
            const auto end = high_resolution_clock::now();
            samples.push_back(duration_cast<nanoseconds>(end - begin).count() /
                              static_cast<double>(CODEC_OPS_PER_BATCH));
        }
        return samples;
    };
    const auto bin_bid_codec_samples = collectCodecSamples("BID", "binary");
    const auto bin_ack_codec_samples = collectCodecSamples("ORDER_ACK", "binary");
    const auto text_bid_codec_samples = collectCodecSamples("BID", "text");
    const auto text_ack_codec_samples = collectCodecSamples("ORDER_ACK", "text");
    writeCsvSamples(raw_csv, run_id, "codec_cpu", "encode_decode_ns_per_message", "binary", "BID",
                    bin_bid_codec_samples, "ns", "100 batches; 1000 operations per batch");
    writeCsvSamples(raw_csv, run_id, "codec_cpu", "encode_decode_ns_per_message", "binary", "ORDER_ACK_1_trade",
                    bin_ack_codec_samples, "ns", "100 batches; 1000 operations per batch");
    writeCsvSamples(raw_csv, run_id, "codec_cpu", "encode_decode_ns_per_message", "text_http_json", "BID",
                    text_bid_codec_samples, "ns", "100 batches; 1000 operations per batch");
    writeCsvSamples(raw_csv, run_id, "codec_cpu", "encode_decode_ns_per_message", "text_http_json", "ORDER_ACK_1_trade",
                    text_ack_codec_samples, "ns", "100 batches; 1000 operations per batch");

    std::cout << std::left << std::setw(24) << "Message"
              << std::right << std::setw(16) << "Binary (ns)"
              << std::setw(16) << "Text (ns)"
              << std::setw(16) << "Ratio" << "\n"
              << std::string(72, '-') << "\n"
              << std::left << std::setw(24) << "BID request"
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(16) << bin_bid_ns << std::setw(16) << text_bid_ns
              << std::setw(15) << std::setprecision(2) << text_bid_ns / bin_bid_ns << "x\n"
              << std::left << std::setw(24) << "ORDER_ACK (1 trade)"
              << std::right << std::fixed << std::setprecision(1)
              << std::setw(16) << bin_ack_ns << std::setw(16) << text_ack_ns
              << std::setw(15) << std::setprecision(2) << text_ack_ns / bin_ack_ns << "x\n";

    // 3. Dispatch Latency
    printSection(3, "Dispatch latency (complete frame bytes -> SPSC queue push)");

    core::SpscQueue<core::WorkItem> test_in_queue(1024);
    std::vector<double> dispatch_latencies_us;
    dispatch_latencies_us.reserve(50000);

    wire::ByteWriter req_w;
    wire::BidRequest br{1, 180, 50};
    codec::BinaryCodec::encodeBid(br, req_w);

    for (int i = 0; i < 50000; ++i) {
        auto t_start_d = high_resolution_clock::now();
        wire::ByteReader r(req_w.data(), req_w.size());
        wire::BidRequest parsed_bid;
        codec::BinaryCodec::decodeBid(r, parsed_bid);

        core::WorkItem wi;
        wi.item_type = core::WorkItemType::ORDER;
        wi.client_id = 999;
        wi.req_id = i;
        wi.type = wire::MsgType::BID;
        wi.symbol_id = parsed_bid.symbol_id;
        wi.price = parsed_bid.price;
        wi.volume = parsed_bid.volume;
        test_in_queue.push(wi);
        auto t_end_d = high_resolution_clock::now();

        core::WorkItem drain;
        test_in_queue.pop(drain);

        double elapsed = duration_cast<nanoseconds>(t_end_d - t_start_d).count() / 1000.0;
        dispatch_latencies_us.push_back(elapsed);
    }

    writeCsvSamples(raw_csv, run_id, "dispatch_latency", "frame_bytes_to_queue_push_us", "binary", "in_process",
                    dispatch_latencies_us, "us", "50,000 samples; includes payload decode and WorkItem construction");
    std::cout << "Recorded 50,000 raw dispatch-latency samples to CSV.\n";

    // 4. Round-Trip Latency
    printSection(4, "Round-trip latency (stub worker, loopback TCP)");

    std::vector<std::unique_ptr<core::Worker>> bench_workers;
    bench_workers.push_back(std::make_unique<core::Worker>(
        1, 1, "Tech", std::vector<std::pair<uint16_t, std::string>>{{1, "AAPL"}}));

    net::Dispatcher bench_dispatcher("127.0.0.1", 0, bench_workers, net::IdlePolicy::NEVER, 0);
    bench_dispatcher.start();
    uint16_t b_port = bench_dispatcher.port();

    std::atomic<bool> b_srv_running{true};
    std::thread b_srv_thread([&]() {
        while (b_srv_running) {
            bench_dispatcher.step();
            for (auto& w : bench_workers) w->step();
        }
    });

    std::this_thread::sleep_for(milliseconds(20));

    client::ClientSession bench_client("127.0.0.1", b_port);
    bench_client.connect();

    std::vector<double> rtt_samples_bin;
    rtt_samples_bin.reserve(3000);
    for (int i = 0; i < 3000; ++i) {
        wire::OrderAck r_ack;
        wire::ErrorCode r_err;
        auto t0 = high_resolution_clock::now();
        bench_client.placeBid(1, 180, 50, r_ack, r_err);
        auto t1 = high_resolution_clock::now();
        rtt_samples_bin.push_back(duration_cast<nanoseconds>(t1 - t0).count() / 1000.0);
    }
    bench_client.disconnect();

    writeCsvSamples(raw_csv, run_id, "round_trip", "request_reply_us", "binary", "loopback",
                    rtt_samples_bin, "us", "3,000 individual order placement round trips");
    std::cout << "Recorded 3,000 binary loopback RTT samples to CSV. Text network RTT is not measured.\n";

    // 5. Peak Throughput
    printSection(5, "Peak throughput (single dispatcher, request/reply operations)");

    client::ClientSession tp_client("127.0.0.1", b_port);
    tp_client.connect();

    const int TP_BATCH = 5000;
    auto tp_t0 = high_resolution_clock::now();
    for (int i = 0; i < TP_BATCH; ++i) {
        wire::OrderAck r_ack;
        wire::ErrorCode r_err;
        tp_client.placeBid(1, 180, 50, r_ack, r_err);
    }
    auto tp_t1 = high_resolution_clock::now();
    double tp_sec = duration_cast<microseconds>(tp_t1 - tp_t0).count() / 1000000.0;
    double bin_msgs_sec = TP_BATCH / tp_sec;
    tp_client.disconnect();
    writeCsvSamples(raw_csv, run_id, "throughput", "operations_per_second", "binary", "loopback_single_client",
                    {bin_msgs_sec}, "ops_per_second", "one 5,000-order batch; single dispatcher");

    std::cout << std::left << std::setw(24) << "Codec / target" << std::right << std::setw(18) << "Ops / second"
              << std::setw(22) << "Measurement" << "\n" << std::string(64, '-') << "\n"
              << std::left << std::setw(24) << "Binary" << std::right << std::fixed << std::setprecision(1)
              << std::setw(18) << bin_msgs_sec << std::setw(22) << "measured" << "\n"
              << std::left << std::setw(24) << "Text HTTP/JSON" << std::right << std::setw(18) << "--"
              << std::setw(22) << "no text dispatcher" << "\n"
              << std::left << std::setw(24) << "SRS minimum" << std::right << std::setw(18) << "250"
              << std::setw(22) << (bin_msgs_sec >= 250.0 ? "binary: PASS" : "binary: FAIL") << "\n";

    b_srv_running = false;
    b_srv_thread.join();
    bench_dispatcher.stop();

    // LAN measurements require a second host; do not infer NIC costs from loopback.
    printSection(6, "Two-machine LAN run");
    std::cout << "Status: NOT RUN\n"
              << "Reason: this executable runs client and server on one host.\n"
              << "Next: run the same harness with the client and server on separate machines.\n";

    // 7. Idle Policy Comparison
    printSection(7, "Idle-policy sweep (never vs 100 ms / 1 s / 5 s)");
    std::cout << "Status: NOT RUN\n"
              << "Missing measurements: latency after idle, ORDER_UPDATE delivery, and connections per minute.\n";

    printSection(8, "Python analysis input");
    std::cout << "Raw samples are in the CSV; Python computes percentiles and writes the plots.\n";

    raw_csv.flush();
    if (!raw_csv) {
        std::cerr << "Failed while writing raw benchmark CSV.\n";
        return 1;
    }
    std::cout << "\nRaw samples written to: " << (output_dir / "raw_measurements.csv").string() << "\n"
              << "Run Python analysis with: python3 scripts/analyze_benchmarks.py \""
              << (output_dir / "raw_measurements.csv").string() << "\" --output-dir \""
              << output_dir.string() << "\"\n";
    net::cleanupNetworking();
    std::cout << "\nReport complete. Values marked NOT RUN are not measurements.\n";
    return 0;
}
