#pragma once
// T124: runs an operation in a forked child and kills it with SIGKILL at a seeded pseudo-random moment, like a battery pull or a crash. The child
// reports what it has COMMITTED through a pipe (one line per step, written after the commit), so the parent knows the lower bound of what must
// have survived. Deterministic: the delay comes from the iteration number.

#include <signal.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "mutate.h"

namespace pbtest {

class KillRun {
public:
    // Forks, runs `child` (which gets the write end of the report pipe), waits until the child has reported `wait_for_lines` lines, lets it run for
    // `extra_us` more microseconds and kills it. Returns the lines reported before the kill.
    static std::vector<std::string> run(const std::function<void(int report_fd)>& child, int wait_for_lines, useconds_t extra_us) {
        int fds[2];
        if (::pipe(fds) != 0) return {};
        const pid_t pid = ::fork();
        if (pid < 0) return {};
        if (pid == 0) {   // the child
            ::close(fds[0]);
            child(fds[1]);
            ::_exit(0);   // no destructors, no atexit: it only ends here if it finished before being killed
        }
        ::close(fds[1]);
        std::string seen;
        int lines = 0;
        char buf[512];
        while (lines < wait_for_lines) {
            const ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n <= 0) break;   // the child ended before reporting enough
            seen.append(buf, static_cast<size_t>(n));
            lines = 0;
            for (char c : seen)
                if (c == '\n') ++lines;
        }
        ::usleep(extra_us);
        ::kill(pid, SIGKILL);
        int status = 0;
        ::waitpid(pid, &status, 0);
        // what the child reported before it died (the pipe holds the rest)
        // the child is dead and its end of the pipe is closed: read() returns the rest and then 0
        for (;;) {
            const ssize_t n = ::read(fds[0], buf, sizeof buf);
            if (n <= 0) break;
            seen.append(buf, static_cast<size_t>(n));
        }
        ::close(fds[0]);
        std::vector<std::string> out;
        size_t pos = 0;
        while (pos < seen.size()) {
            const size_t e = seen.find('\n', pos);
            if (e == std::string::npos) break;   // a half line was cut by the kill
            out.push_back(seen.substr(pos, e - pos));
            pos = e + 1;
        }
        return out;
    }

    static void report(int fd, const std::string& line) {
        const std::string l = line + "\n";
        const ssize_t n = ::write(fd, l.data(), l.size());
        (void)n;
    }

    // microseconds to let the child run, 0 .. max_us, from the iteration number
    static useconds_t delay_for(int iteration, useconds_t max_us) { return static_cast<useconds_t>(Prng(0x9E3779B97F4A7C15ull + static_cast<uint64_t>(iteration) * 7919).below(max_us + 1)); }

    static int iterations(int fallback) {
        const char* v = std::getenv("PBO_KILL_ITERATIONS");
        return v && std::atoi(v) > 0 ? std::atoi(v) : fallback;
    }

};

}  // namespace pbtest
