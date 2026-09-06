#pragma once

// Plumbing shared by the gadget and the capture runtime: logging, the RAII
// wrappers, the worker-thread mechanism the blocking FunctionFS endpoints
// force on us, and the goggles' outer frame format.

#include <atomic>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <csignal>

#include <pthread.h>
#include <unistd.h>

// --- logging -----------------------------------------------------------------
void log(const std::string &s);

// "a, b, c" -- for telling the user what a board actually has.
std::string comma_separated(const std::vector<std::string> &v);

// --- signals -----------------------------------------------------------------
using Flag = std::atomic<bool>;

// Deliberately without SA_RESTART: a signal has to make the blocking endpoint
// transfer return EINTR, otherwise the kernel restarts it and nothing changes.
void catch_signal(int sig, void (*handler)(int));

// Worker::stop() pokes its thread with SIGUSR1, which only works if the signal
// has a handler that does nothing rather than killing the process. Call this
// once before starting any Worker.
void install_worker_kick_handler();

// A FunctionFS endpoint transfer blocks until the host moves data, cannot be
// polled, and only returns early on a signal. So every blocking transfer runs
// in a Worker: a thread that ignores the stop signals (they belong to the main
// thread) and is poked with SIGUSR1 until it has actually left.
//
// The poking is a loop on purpose. A single signal that lands between the
// worker's quit check and its next transfer is silently consumed by the no-op
// handler, and the transfer then blocks with nobody left to wake it.
//
// The callable is passed a `const Flag &quit` it should check between transfers.
class Worker {
public:
    template <class Fn>
    explicit Worker(Fn fn) : thread_([this, fn] {
        sigset_t stop_signals;
        sigemptyset(&stop_signals);
        sigaddset(&stop_signals, SIGINT);
        sigaddset(&stop_signals, SIGTERM);
        pthread_sigmask(SIG_BLOCK, &stop_signals, nullptr);
        fn(quit_);
        done_ = true;
    }) {}

    ~Worker() { stop(); }
    Worker(const Worker &) = delete;
    Worker &operator=(const Worker &) = delete;

    void stop() {
        if (!thread_.joinable()) return;
        quit_ = true;
        while (!done_) {
            pthread_kill(thread_.native_handle(), SIGUSR1);
            usleep(10000);
        }
        thread_.join();
    }

private:
    Flag quit_{false}, done_{false};
    std::thread thread_;
};

// --- small RAII helpers ------------------------------------------------------
struct Fd {
    int fd = -1;
    Fd() = default;
    explicit Fd(const int f) : fd(f) {}
    ~Fd() { if (fd >= 0) close(fd); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    operator int() const { return fd; }
};

// Retries short writes and EINTR. Returns false with errno set otherwise.
bool write_all(int fd, const uint8_t *p, size_t n);

// --- the goggles' outer framing ----------------------------------------------
// Everything the goggles send is wrapped in 55 CC frames:
//   55 CC <channel> <..> <length lo> <length hi> <seq lo> <seq hi> <payload>
constexpr size_t FRAME_HEADER = 8;

struct Frame {
    uint8_t        channel = 0;
    const uint8_t *payload = nullptr;
    size_t         length  = 0;
};

// Next complete frame at or after pos, skipping garbage between frames.
// Returns false when the buffer holds no complete frame yet.
bool next_frame(const std::vector<uint8_t> &buf, size_t &pos, Frame &f);
