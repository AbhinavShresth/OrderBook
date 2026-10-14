#pragma once

#include <cstdint>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace wire {

inline uint16_t hostToNetwork16(uint16_t val) {
    return htons(val);
}

inline uint16_t networkToHost16(uint16_t val) {
    return ntohs(val);
}

inline uint32_t hostToNetwork32(uint32_t val) {
    return htonl(val);
}

inline uint32_t networkToHost32(uint32_t val) {
    return ntohl(val);
}

inline uint64_t hostToNetwork64(uint64_t val) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return val;
#else
    return (((uint64_t)htonl((uint32_t)(val & 0xFFFFFFFF))) << 32) |
           ((uint64_t)htonl((uint32_t)(val >> 32)));
#endif
}

inline uint64_t networkToHost64(uint64_t val) {
    return hostToNetwork64(val);
}

} // namespace wire
