#include "client/ClientSession.hpp"
#include "codec/BinaryCodec.hpp"
#include <iostream>
#include <iomanip>
#include <string>
#include <sstream>
#include <unordered_map>
#include <mutex>
#include <algorithm>

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Usage: ./exchange_client [host] [port]\n"
                  << "  host: Exchange server IP/hostname (default: 127.0.0.1)\n"
                  << "  port: Exchange server TCP port (default: 8085)\n";
        return 0;
    }
    std::string host = "127.0.0.1";
    uint16_t port = 8085;
    if (argc > 1) host = argv[1];
    if (argc > 2) port = static_cast<uint16_t>(std::stoi(argv[2]));

    std::cout << "========================================================\n"
              << " LOW LATENCY TRADING EXCHANGE (C++17 CLIENT CLI)        \n"
              << " Connecting to: " << host << ":" << port << "\n"
              << "========================================================\n";

    client::ClientSession session(host, port);
    session.setOrderUpdateCallback([](const wire::OrderAck& update) {
        const char* status = update.status == wire::OrderStatus::FILLED ? "FILLED" : "PARTIAL_FILL";
        std::cout << "\n[ORDER_UPDATE] Order-ID = " << update.order_id << ", Status = " << status
                  << ", Remaining Vol = " << update.remaining_vol;
        for (const auto& trade : update.trades)
            std::cout << " | Trade #" << trade.trade_id << " @ $" << trade.price
                      << " x " << trade.vol << " (counterparty " << trade.counterpart_id << ")";
        std::cout << "\nCommand > " << std::flush;
    });
    if (!session.connect()) {
        std::cerr << "[-] Connection failed to " << host << ":" << port << "\n";
        return 1;
    }

    std::cout << "[+] Connected! Requesting LIST_FEED...\n";
    wire::ListFeedResponse lfr;
    if (session.listFeed(lfr)) {
        std::cout << "[+] Discovered " << lfr.groups.size() << " ticker groups (Assigned Client-ID: "
                  << session.clientId() << "):\n";
        for (const auto& g : lfr.groups) {
            std::cout << "    - Group " << (int)g.group_id << ": " << g.name << " -> ";
            for (const auto& t : g.tickers) {
                std::cout << std::string(t.ticker, 8) << " (id: " << t.symbol_id << ") ";
            }
            std::cout << "\n";
        }
    }

    // Thread-safe snapshot storage and deduplication
    std::mutex quote_mtx;
    std::unordered_map<std::string, wire::UdpMarketFeedDatagram> latest_dgrams;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> last_bbo;

    client::UdpListener udp_listener(0, [&](const wire::UdpMarketFeedDatagram& dgram, bool loss, uint64_t) {
        std::string t(dgram.ticker, 8);
        size_t end = t.find_last_not_of(" \t\0");
        if (end != std::string::npos) t = t.substr(0, end + 1);

        {
            std::lock_guard<std::mutex> lock(quote_mtx);
            latest_dgrams[t] = dgram;
        }

        auto current_bbo = std::make_pair(dgram.bids[0].price, dgram.bids[0].vol);
        bool bbo_changed = (last_bbo[t] != current_bbo);
        if (bbo_changed || loss) {
            last_bbo[t] = current_bbo;
            std::cout << "\n[UDP QUOTE] Ticker: " << std::left << std::setw(6) << t
                      << " | Seq: #" << std::setw(4) << dgram.seq
                      << " | BBO: $" << dgram.bids[0].price << " (" << dgram.bids[0].vol << " lots) x $"
                      << dgram.asks[0].price << " (" << dgram.asks[0].vol << " lots)\n"
                      << "Command > " << std::flush;
        }
    });
    udp_listener.start();
    std::cout << "[+] Bound UDP listener on local port: " << udp_listener.port() << "\n";

    // Subscribe to group 1 (Tech)
    uint32_t subscribed = 0;
    session.subscribe(udp_listener.port(), 0x01, subscribed);
    std::cout << "[+] Subscribed to Group 1 (Mask: 0x" << std::hex << subscribed << std::dec << ")\n";

    std::cout << "\nCommands:\n"
              << "  snapshot [ticker]               (e.g.: snapshot, snapshot AAPL)\n"
              << "  depth [ticker]                  (e.g.: depth AAPL - shows ladder + hex layout)\n"
              << "  bid <symbol_id> <price> <vol>   (e.g.: bid 1 180 50)\n"
              << "  ask <symbol_id> <price> <vol>   (e.g.: ask 1 185 20)\n"
              << "  cancel <order_id>               (e.g.: cancel 72057594037927937)\n"
              << "  quit\n\n";

    std::string line;
    while (true) {
        std::cout << "Command > " << std::flush;
        if (!std::getline(std::cin, line)) break;
        if (line.empty()) continue;

        std::istringstream iss(line);
        std::string cmd;
        iss >> cmd;

        if (cmd == "quit" || cmd == "exit") {
            break;
        } else if (cmd == "bid") {
            uint16_t sym = 0;
            uint64_t price = 0, vol = 0;
            if (!(iss >> sym >> price >> vol)) {
                std::cout << "[-] Usage: bid <symbol_id> <price> <vol>\n";
                continue;
            }
            wire::OrderAck ack;
            wire::ErrorCode err;
            if (session.placeBid(sym, price, vol, ack, err)) {
                const char* status = ack.status == wire::OrderStatus::RESTING ? "RESTING" :
                                     ack.status == wire::OrderStatus::PARTIAL_FILL ? "PARTIAL_FILL" : "FILLED";
                std::cout << "[+] ORDER_ACK Received: Order-ID = " << ack.order_id
                          << ", Status = " << status << ", Remaining Vol = " << ack.remaining_vol << "\n";
                for (const auto& trade : ack.trades)
                    std::cout << "    Trade #" << trade.trade_id << " @ $" << trade.price
                              << " x " << trade.vol << " (counterparty " << trade.counterpart_id << ")\n";
            } else {
                std::cout << "[-] BID failed";
                if (err == wire::ErrorCode::MALFORMED) {
                    std::cout << ": no valid response (connection or protocol failure)\n";
                } else {
                    std::cout << ": protocol error " << (int)err << "\n";
                }
            }
        } else if (cmd == "ask") {
            uint16_t sym = 0;
            uint64_t price = 0, vol = 0;
            if (!(iss >> sym >> price >> vol)) {
                std::cout << "[-] Usage: ask <symbol_id> <price> <vol>\n";
                continue;
            }
            wire::OrderAck ack;
            wire::ErrorCode err;
            if (session.placeAsk(sym, price, vol, ack, err)) {
                const char* status = ack.status == wire::OrderStatus::RESTING ? "RESTING" :
                                     ack.status == wire::OrderStatus::PARTIAL_FILL ? "PARTIAL_FILL" : "FILLED";
                std::cout << "[+] ORDER_ACK Received: Order-ID = " << ack.order_id
                          << ", Status = " << status << ", Remaining Vol = " << ack.remaining_vol << "\n";
                for (const auto& trade : ack.trades)
                    std::cout << "    Trade #" << trade.trade_id << " @ $" << trade.price
                              << " x " << trade.vol << " (counterparty " << trade.counterpart_id << ")\n";
            } else {
                std::cout << "[-] ASK failed";
                if (err == wire::ErrorCode::MALFORMED) {
                    std::cout << ": no valid response (connection or protocol failure)\n";
                } else {
                    std::cout << ": protocol error " << (int)err << "\n";
                }
            }
        } else if (cmd == "cancel") {
            uint64_t oid = 0;
            if (!(iss >> oid)) {
                std::cout << "[-] Usage: cancel <order_id>\n";
                continue;
            }
            wire::CancelAck ack;
            wire::ErrorCode err;
            if (session.cancelOrder(oid, ack, err)) {
                std::cout << "[+] CANCEL_ACK Received: Order-ID = " << ack.order_id
                          << ", Status = CANCELLED\n";
            } else {
                std::cout << "[-] Cancel failed";
                if (err == wire::ErrorCode::ORDER_NOT_FOUND) {
                    std::cout << ": ORDER_NOT_FOUND\n";
                } else if (err == wire::ErrorCode::MALFORMED) {
                    std::cout << ": no valid response (connection or protocol failure)\n";
                } else {
                    std::cout << ": protocol error " << (int)err << "\n";
                }
            }
        } else if (cmd == "snapshot" || cmd == "depth" || cmd == "book") {
            std::lock_guard<std::mutex> lock(quote_mtx);
            std::string target_ticker = "";
            iss >> target_ticker;
            if (!target_ticker.empty()) {
                std::transform(target_ticker.begin(), target_ticker.end(), target_ticker.begin(), ::toupper);
            }

            if (latest_dgrams.empty()) {
                std::cout << "[*] No market data snapshots received yet. Ensure worker is publishing or submit an order to trigger one.\n";
            } else {
                bool found = false;
                for (const auto& kv : latest_dgrams) {
                    const auto& dg = kv.second;
                    std::string sym = kv.first;
                    if (!target_ticker.empty() && sym.find(target_ticker) == std::string::npos) {
                        continue;
                    }
                    found = true;
                    std::cout << "\n================================================================================\n"
                              << " [MARKET SNAPSHOT] Ticker: " << sym
                              << " | Group: " << dg.group_id
                              << " | Seq: #" << dg.seq
                              << " | Timestamp: " << dg.timestamp_us << " us\n"
                              << "--------------------------------------------------------------------------------\n"
                              << " TOP-3 BBO LADDER:\n"
                              << "   BID DEPTH (Buyers)                     ASK DEPTH (Sellers)\n"
                              << "   -----------------------------          -----------------------------\n";
                    for (int i = 0; i < 3; ++i) {
                        std::cout << "   L" << (i + 1) << ": ";
                        if (i < dg.n_bids && dg.bids[i].price > 0) {
                            std::cout << "$" << std::setw(8) << std::left << dg.bids[i].price
                                      << " (" << std::setw(5) << dg.bids[i].vol << " lots)    |   ";
                        } else {
                            std::cout << std::setw(17) << std::left << "EMPTY" << "    |   ";
                        }

                        std::cout << "L" << (i + 1) << ": ";
                        if (i < dg.n_asks && dg.asks[i].price > 0) {
                            std::cout << "$" << std::setw(8) << std::left << dg.asks[i].price
                                      << " (" << std::setw(5) << dg.asks[i].vol << " lots)\n";
                        } else {
                            std::cout << "EMPTY\n";
                        }
                    }
                    std::cout << "================================================================================\n";

                    // Also print Wireshark hex layout if requested
                    std::string sub_arg;
                    iss >> sub_arg;
                    if (sub_arg == "hex" || cmd == "depth") {
                        wire::ByteWriter hw(128);
                        codec::BinaryCodec::encodeUdpDatagram(dg, hw);
                        std::cout << " RAW 128-BYTE CACHELINE WIRE LAYOUT (Wireshark Bytes Pane):\n";
                        for (size_t r = 0; r < hw.size(); r += 16) {
                            std::cout << " " << std::hex << std::setw(4) << std::setfill('0') << r << "  ";
                            for (size_t c = 0; c < 16; ++c) {
                                if (r + c < hw.size()) {
                                    std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)hw.data()[r + c] << " ";
                                }
                                if (c == 7) std::cout << " ";
                            }
                            std::cout << " |";
                            for (size_t c = 0; c < 16 && (r + c) < hw.size(); ++c) {
                                uint8_t ch = hw.data()[r + c];
                                std::cout << ((ch >= 32 && ch <= 126) ? (char)ch : '.');
                            }
                            std::cout << "|\n" << std::dec << std::setfill(' ');
                        }
                        std::cout << "================================================================================\n";
                    }
                }
                if (!found && !target_ticker.empty()) {
                    std::cout << "[-] No snapshot found for ticker: " << target_ticker << "\n";
                }
            }
        } else {
            std::cout << "[-] Unknown command: '" << cmd << "'. Commands: 'snapshot', 'bid', 'ask', 'cancel', 'quit'.\n";
        }
    }

    std::cout << "[*] Exiting client session.\n";
    udp_listener.stop();
    session.disconnect();
    net::cleanupNetworking();
    return 0;
}
