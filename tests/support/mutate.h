#pragma once
// Deterministic mutations of a byte string for the "never crash" tests: truncation at an offset, seeded byte flips and
// insertions, line-ending changes. A fixed seed keeps every run identical (no flaky tests).

#include <cstdint>
#include <string>
#include <vector>

namespace pbtest {

// xorshift: tiny, deterministic, good enough to pick offsets
struct Prng {
    uint64_t s;
    explicit Prng(uint64_t seed) : s(seed ? seed : 0x9E3779B97F4A7C15ull) {}
    uint64_t next() {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return s;
    }
    size_t below(size_t n) { return n ? static_cast<size_t>(next() % n) : 0; }
};

inline std::string truncated(const std::string& data, size_t at) { return data.substr(0, at < data.size() ? at : data.size()); }

// `flips` bytes replaced by random values at random places
inline std::string flipped(const std::string& data, uint64_t seed, int flips) {
    std::string out = data;
    if (out.empty()) return out;
    Prng r(seed);
    for (int i = 0; i < flips; ++i) out[r.below(out.size())] = static_cast<char>(r.next() & 0xFF);
    return out;
}

// `count` random bytes inserted at random places
inline std::string with_insertions(const std::string& data, uint64_t seed, int count) {
    std::string out = data;
    Prng r(seed);
    for (int i = 0; i < count; ++i) out.insert(r.below(out.size() + 1), 1, static_cast<char>(r.next() & 0xFF));
    return out;
}

inline std::string with_line_ends(const std::string& data, const std::string& end) {
    std::string out;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i] == '\r' && i + 1 < data.size() && data[i + 1] == '\n') continue;
        if (data[i] == '\n') out += end;
        else out += data[i];
    }
    return out;
}

// Offsets at which to truncate: every offset for small inputs, else about `samples` evenly spread ones plus the edges.
inline std::vector<size_t> truncation_points(size_t size, size_t every_below = 3000, size_t samples = 200) {
    std::vector<size_t> pts;
    if (size <= every_below) {
        for (size_t i = 0; i <= size; ++i) pts.push_back(i);
        return pts;
    }
    for (size_t i = 0; i < samples; ++i) pts.push_back(size * i / samples);
    for (size_t e : {size_t(1), size_t(2), size_t(3), size - 3, size - 2, size - 1, size}) pts.push_back(e);
    return pts;
}

}  // namespace pbtest
