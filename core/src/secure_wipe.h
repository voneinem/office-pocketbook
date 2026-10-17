#pragma once
// Overwrites memory that held a secret so the compiler cannot drop the writes (a plain memset before a free may
// be removed). Not a guarantee against every copy (std::string may have reallocated), only best effort.
#include <cstddef>
#include <string>

namespace pbo {

inline void secure_wipe(void* p, size_t n) {
    volatile unsigned char* v = static_cast<volatile unsigned char*>(p);
    while (n--) *v++ = 0;
}

inline void secure_wipe(std::string& s) {
    if (!s.empty()) secure_wipe(&s[0], s.size());
}

}  // namespace pbo
