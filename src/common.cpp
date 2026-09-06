#include "common.h"

#include <cstdio>
#include <cerrno>

void log(const std::string &s) {
    fprintf(stderr, "%s\n", s.c_str());
}

std::string comma_separated(const std::vector<std::string> &v) {
    std::string s;
    for (const std::string &x : v) s += (s.empty() ? "" : ", ") + x;
    return s;
}

void catch_signal(const int sig, void (*handler)(int)) {
    struct sigaction sa {};
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, nullptr);
}

static void on_kick(int) {}                              // just interrupt the syscall

void install_worker_kick_handler() {
    catch_signal(SIGUSR1, on_kick);
}

bool write_all(const int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        const ssize_t w = write(fd, p, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        p += w;
        n -= w;
    }
    return true;
}

bool next_frame(const std::vector<uint8_t> &buf, size_t &pos, Frame &f) {
    while (buf.size() - pos >= FRAME_HEADER) {
        if (buf[pos] != 0x55 || buf[pos + 1] != 0xCC) { pos++; continue; }
        const size_t length = buf[pos + 4] | (buf[pos + 5] << 8);
        if (buf.size() - pos < FRAME_HEADER + length) return false;
        f = {buf[pos + 2], buf.data() + pos + FRAME_HEADER, length};
        pos += FRAME_HEADER + length;
        return true;
    }
    return false;
}
