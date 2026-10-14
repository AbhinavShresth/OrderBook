@echo off
setlocal
set WIRESHARK_EXE=C:\Program Files\Wireshark\Wireshark.exe

if not exist "%WIRESHARK_EXE%" (
    echo [ERROR] Wireshark was not found at "%WIRESHARK_EXE%".
    echo Please install Wireshark or update the path in this script.
    pause
    exit /b 1
)

set PCAP_FILE=%~dp0market_feed.pcap
set LUA_SCRIPT=%~dp0wireshark\market_feed.lua

if not exist "%PCAP_FILE%" (
    echo [INFO] Generating market_feed.pcap via C++ demo_automated binary...
    wsl -e bash -c "cd /mnt/c/Users/LENOVO/OneDrive/Desktop/OrderBook && cmake -B build -S . -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && cmake --build build --target demo_automated > /dev/null 2>&1 && ./build/demo_automated"
)

echo [OK] Opening Wireshark with Market Feed Dissector...
start "" "%WIRESHARK_EXE%" -X lua_script:"%LUA_SCRIPT%" "%PCAP_FILE%"
