#pragma once
// Shared core types (contracts/core-interfaces.md section 1). No Qt, no InkView.
//
// Every network, clock and random dependency enters the core through an interface declared here, so each
// module is tested on the host without the device (constitution Principle II).

#include <cstddef>
#include <cstdint>
#include <string>

namespace pbo {

enum class ErrorClass { None, Network, Tls, Auth, Server, Protocol, Unsupported, Storage, Cancelled };

struct Error {
    ErrorClass cls = ErrorClass::None;
    // Stable cause key, for example "cert_expired", "cert_untrusted", "cert_host_mismatch", "auth_failed",
    // "server_unreachable", "no_network", "timeout", "starttls_unavailable", "protocol_error",
    // "refused_recipient", "too_large", "storage_full", "not_supported".
    std::string key;
    // Short English text. MUST NOT hold credentials, addresses, subjects, host names or paths (FR-041).
    std::string detail;

    bool ok() const { return cls == ErrorClass::None; }
};

inline Error make_error(ErrorClass cls, std::string key, std::string detail = std::string()) {
    Error e;
    e.cls = cls;
    e.key = std::move(key);
    e.detail = std::move(detail);
    return e;
}

template <class T>
struct Result {
    Error error;
    T value{};

    bool ok() const { return error.ok(); }
};

// Epoch seconds. Tests substitute a fake clock.
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t now() const = 0;
};

class Random {
public:
    virtual ~Random() = default;
    virtual void bytes(uint8_t* out, size_t n) = 0;
};

// Polled between commands so a running sync can be cancelled.
class Cancel {
public:
    virtual ~Cancel() = default;
    virtual bool requested() const = 0;
};

// Production implementations.
class SystemClock : public Clock {
public:
    int64_t now() const override;
};

class SystemRandom : public Random {
public:
    void bytes(uint8_t* out, size_t n) override;  // /dev/urandom; aborts the process if it cannot be read
};

}  // namespace pbo
