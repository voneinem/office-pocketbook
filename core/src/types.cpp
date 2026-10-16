#include "pbo/types.h"

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>

namespace pbo {

int64_t SystemClock::now() const {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

void SystemRandom::bytes(uint8_t* out, size_t n) {
    // A message id or a boundary built from weak random data would not be unique; failing hard is safer.
    const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    size_t got = 0;
    while (fd >= 0 && got < n) {
        const ssize_t r = ::read(fd, out + got, n - got);
        if (r <= 0) break;
        got += static_cast<size_t>(r);
    }
    if (fd >= 0) ::close(fd);
    if (got != n) std::abort();
}

}  // namespace pbo
