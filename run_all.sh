#!/usr/bin/env bash
set -e

echo "=== Building Low Latency OrderBook (C++17 Linux) ==="
cmake -B build -S .
cmake --build build

echo ""
echo "=== Running Doctest Test Suite (Phases 1-8) ==="
./build/test_suite

echo ""
echo "=== All Tests Passed Successfully! ==="
echo "To run the exchange server:  ./build/exchange_server 8085"
echo "To run the client CLI:       ./build/exchange_client 127.0.0.1 8085"
echo "To run automated demo:       ./build/demo_automated"
echo "To run benchmarks (Sec 8):   ./build/benchmark_suite"