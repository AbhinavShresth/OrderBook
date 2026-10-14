#include "net/Dispatcher.hpp"
#include "core/Worker.hpp"
#include <iostream>
#include <vector>
#include <memory>
#include <csignal>
#include <atomic>
#if defined(_WIN32)
#include "mingw_thread.h"
#else
#include <thread>
#endif

static std::atomic<bool> g_running{true};

void signalHandler(int) {
    g_running = false;
}

int main(int argc, char* argv[]) {
    if (argc > 1 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        std::cout << "Usage: ./exchange_server [tcp_port] [idle_timeout_sec]\n"
                  << "  tcp_port:         Port to listen on (default: 8085)\n"
                  << "  idle_timeout_sec: Connection idle timeout in seconds (default: 1)\n";
        return 0;
    }
    uint16_t tcp_port = 8085;
    if (argc > 1) {
        tcp_port = static_cast<uint16_t>(std::stoi(argv[1]));
    }

    std::signal(SIGINT, signalHandler);

    std::cout << "========================================================\n"
              << " LOW LATENCY TRADING EXCHANGE (C++17 PROTOTYPE SERVER) \n"
              << " Protocol: 20-byte Binary TCP Header + 128-byte UDP Feed\n"
              << "========================================================\n";

    std::vector<std::unique_ptr<core::Worker>> workers;
    workers.push_back(std::make_unique<core::Worker>(
        1, 1, "Tech", std::vector<std::pair<uint16_t, std::string>>{{1, "AAPL"}, {2, "GOOG"}, {3, "MSFT"}}));
    workers.push_back(std::make_unique<core::Worker>(
        2, 2, "Finance", std::vector<std::pair<uint16_t, std::string>>{{4, "JPM"}, {5, "GS"}, {6, "BAC"}}));
    workers.push_back(std::make_unique<core::Worker>(
        3, 3, "Energy", std::vector<std::pair<uint16_t, std::string>>{{7, "XOM"}, {8, "CVX"}, {9, "SHEL"}}));

    for (auto& w : workers) {
        w->initUdpSocket(0);
        std::cout << "[+] Worker " << (int)w->worker_id() << " (" << w->group_name()
                  << ") UDP broadcast port: " << w->udp_port() << "\n";
    }

    net::Dispatcher dispatcher("0.0.0.0", tcp_port, workers, net::IdlePolicy::FIXED, 1);
    if (!dispatcher.start()) {
        std::cerr << "[-] Failed to start Dispatcher on port " << tcp_port << "\n";
        return 1;
    }

    std::cout << "[+] Central Dispatcher running on TCP port: " << dispatcher.port() << "\n";
    std::cout << "[*] Running single-threaded round-robin event loop. Press Ctrl+C to terminate.\n";

    while (g_running) {
        dispatcher.step();
        for (auto& w : workers) {
            w->step();
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }

    std::cout << "\nShutting down exchange server...\n";
    dispatcher.stop();
    net::cleanupNetworking();
    return 0;
}
