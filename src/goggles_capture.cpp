#include "common.h"
#include "goggles_capture.h"
#include "usb_gadget.h"

#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <csignal>
#include <cerrno>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <linux/usb/functionfs.h>

// --- Android Open Accessory identity strings ---------------------------------
// The goggles check these before they agree to open the video path.
static GadgetIdentity aoa_identity() {
    return {VID, PID,
            "Google Inc.",
            "Android-powered device in accessory mode",
            "da64sxd1",
            "High speed configuration",
            "Android Accessory Interface"};
}

// --- signals -----------------------------------------------------------------
// The stop signals are the main thread's alone; Worker blocks them in every
// thread it starts, so they always land here.
static Flag g_stop{false};                               // SIGINT / SIGTERM
static void on_stop(int) { g_stop = true; }

// --- DJI DUML checksums (CRC-8/Maxim seed 0x77, CRC-16/X-25 seed 0x3692) -----
static const struct CrcTables {
    uint8_t  crc8[256]{};
    uint16_t crc16[256]{};
    CrcTables() {
        for (int i = 0; i < 256; i++) {
            uint32_t c8 = i, c16 = i;
            for (int k = 0; k < 8; k++) {
                c8  = (c8  & 1) ? (c8  >> 1) ^ 0x8C   : c8  >> 1;
                c16 = (c16 & 1) ? (c16 >> 1) ^ 0x8408 : c16 >> 1;
            }
            crc8[i]  = c8;
            crc16[i] = c16;
        }
    }
} TABLES;

uint8_t crc8(const uint8_t *d, size_t n) {
    uint8_t c = 0x77;
    for (size_t i = 0; i < n; i++) c = TABLES.crc8[c ^ d[i]];
    return c;
}

uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t c = 0x3692;
    for (size_t i = 0; i < n; i++) c = TABLES.crc16[(c ^ d[i]) & 0xFF] ^ (c >> 8);
    return c;
}

// --- DUML keep-alive commands ------------------------------------------------
// Pre-recorded inner packets from the official receiver. COMMANDS[5] ("APP")
// tells the goggles to start streaming; the rest are normal keep-alive chatter.
const std::vector<std::vector<uint8_t>> COMMANDS = {
    {0x55,0x10,0x04,0x56,0x02,0x88,0x54,0xad,0x40,0x00,0xe5,0x04,0x04,0x01,0x68,0x9c},
    {0x55,0x0e,0x04,0x66,0x02,0x28,0x55,0xad,0x40,0x00,0x51,0x06,0x7c,0x9c},
    {0x55,0x16,0x04,0xfc,0x02,0x48,0x56,0xad,0x40,0x00,0x4f,0x01,0x00,0x16,0x00,0x00,0xff,0xff,0xff,0xff,0x5c,0xe9},
    {0x55,0x1e,0x04,0x8a,0x02,0x01,0x63,0xad,0x40,0x02,0xeb,0x00,0xff,0x03,0x11,0x27,0x00,0x00,0x0a,0x00,0x03,0x00,0x08,0x00,0xd1,0x07,0x75,0x17,0x6f,0x3d},
    {0x55,0x0e,0x04,0x66,0x02,0x2d,0x7e,0x01,0x80,0x00,0x82,0x00,0xd2,0x72},
    {0x55,0x1b,0x04,0x75,0x02,0x3c,0x68,0xad,0x40,0x00,0x88,0x17,0x00,0x00,0x23,0x00,0x41,0x50,0x50,0x00,0x00,0x00,0x00,0x00,0x02,0x98,0xf0}, // APP
    {0x55,0x0e,0x04,0x66,0x02,0x01,0x6a,0xad,0x40,0x02,0xd0,0x04,0x20,0xa8},
    {0x55,0x0e,0x04,0x66,0x02,0x01,0x42,0x86,0x40,0x08,0x41,0x02,0xad,0xd2},
    {0x55,0x0d,0x04,0x33,0x02,0x0e,0x4b,0x86,0x00,0x00,0x00,0xc1,0x2a},
};

// Fill in the sequence number + CRCs, then add the outer USB wrapper.
std::vector<uint8_t> build_command(const std::vector<uint8_t> &inner, uint16_t seq) {
    std::vector<uint8_t> p = inner;
    p[6] = seq & 0xFF;
    p[7] = seq >> 8;
    p[3] = crc8(p.data(), 3);
    const uint16_t c = crc16(p.data(), p.size() - 2);
    p[p.size() - 2] = c & 0xFF;
    p[p.size() - 1] = c >> 8;

    const uint16_t n = p.size();
    std::vector<uint8_t> out;
    out.reserve(8 + n);
    out = {0x55, 0xCC, 0x49, 0x57,
           static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>(n >> 8),
           static_cast<uint8_t>(seq & 0xFF), static_cast<uint8_t>(seq >> 8)};
    out.insert(out.end(), p.begin(), p.end());
    return out;
}

// --- EP0 events --------------------------------------------------------------
// FunctionFS answers every standard control request itself (the descriptors and
// identity strings come from configfs), so ep0 only tells us when the host has
// enabled the interface -- and hands us the odd vendor request addressed to it,
// which we stall.
//
// A no-data request is confirmed with a zero-length transfer in the data-stage
// direction; the same transfer in the opposite direction stalls it instead.
static void stall(const int fd, const bool host_to_device) {
    char x;
    if (host_to_device) (void)!write(fd, &x, 0); else (void)!read(fd, &x, 0);
}

static void event_loop(const int fd, Flag &enabled, const Flag &quit) {
    usb_functionfs_event ev[8];
    while (!quit) {
        pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, 200) <= 0) continue;

        const ssize_t n = read(fd, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            log(std::string("ep0 read failed: ") + strerror(errno));
            return;
        }

        for (ssize_t i = 0; i < n / static_cast<ssize_t>(sizeof(ev[0])); i++) {
            switch (ev[i].type) {
                case FUNCTIONFS_ENABLE:
                    if (!enabled.exchange(true)) log("USB: enabled (host configured us)");
                    break;
                case FUNCTIONFS_DISABLE:
                    if (enabled.exchange(false)) log("USB: disabled");
                    break;
                case FUNCTIONFS_UNBIND:
                    log("USB: unbind");
                    enabled = false;
                    break;
                case FUNCTIONFS_SETUP:
                    stall(fd, !(ev[i].u.setup.bRequestType & 0x80));
                    break;
                default:
                    break;                               // BIND / SUSPEND / RESUME
            }
        }
    }
}

// --- one session = one plug-in -----------------------------------------------
// What the two data-flow workers report back to the main thread.
struct Session {
    Flag over{false};                    // a worker ended it; main tears down and starts over
    Flag failed{false};                  // ...because of an unexpected I/O error
    Flag sink_closed{false};             // ...because nobody reads the video any more

    void end() { over = true; }

    void fail(const std::string &what) {
        log(what + ": " + strerror(errno));
        failed = true;
        over = true;
    }

    // An endpoint that went away is the normal end of a session -- the host
    // disabled the interface and a DISABLE event is on its way. Anything else
    // is a real error.
    void endpoint_error(const char *what) {
        if (errno == ESHUTDOWN || errno == ENODEV) end(); else fail(what);
    }
};

// Keep-alive: replay the DUML commands ~once a second for as long as the host
// keeps the interface enabled. Stop, and the goggles cut the video after ~11 s.
static void send_commands(const int fd, Session &s, const Flag &quit) {
    uint16_t seq = 0;
    while (!quit) {
        for (const auto &inner : COMMANDS) {
            if (quit) return;
            const std::vector<uint8_t> pkt = build_command(inner, seq++);
            // Blocks until the goggles drain the endpoint; Worker::stop() is
            // what ends it when they never do.
            if (write(fd, pkt.data(), pkt.size()) < 0 && errno != EINTR && errno != EAGAIN) {
                s.endpoint_error("command write");
                return;
            }
            usleep(20000);
        }
        usleep(1000000);
    }
}

// Offset of the first H.264 SPS (NAL type 7) at a start code, or -1.
// We start the stream here so a downstream decoder gets a clean first buffer.
ssize_t find_sps(const uint8_t *p, const size_t n) {
    for (size_t i = 0; i + 4 < n; i++)
        if (p[i] == 0 && p[i+1] == 0 && p[i+2] == 0 && p[i+3] == 1 && (p[i+4] & 0x1F) == 7)
            return i;
    return -1;
}

// Read the goggles' stream, unwrap the frames, write channel 0x4A to sink.
// The read size is a multiple of the 512-byte high-speed max packet, which
// FunctionFS requires for OUT endpoints.
static void receive_video(const int fd, const int sink, Session &s, const Flag &quit) {
    std::vector<uint8_t> buf;
    uint8_t chunk[16384];
    size_t total = 0;
    bool started = false;                // only emit once we've seen the first SPS

    while (!quit) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            s.endpoint_error("video read");
            return;
        }
        buf.insert(buf.end(), chunk, chunk + n);

        size_t pos = 0;
        Frame f{};
        while (next_frame(buf, pos, f)) {
            if (f.channel != VIDEO_CHANNEL || f.length == 0) continue;

            if (!started) {                              // skip the partial leading NAL
                const ssize_t sps = find_sps(f.payload, f.length);
                if (sps < 0) continue;
                f.payload += sps;
                f.length  -= sps;
                started = true;
                log(">>> first SPS seen - clean H.264 stream starts here");
            }

            // SIGPIPE is ignored, so a consumer that went away shows up here as
            // EPIPE. EINTR only means Worker::stop() is asking us to leave.
            if (!write_all(sink, f.payload, f.length)) {
                if (errno != EINTR) {
                    log(std::string("output closed (") + strerror(errno) + ")");
                    s.sink_closed = true;
                }
                s.end();
                return;
            }
            total += f.length;
            if (total % (1 << 20) < f.length) log("  wrote " + std::to_string(total / 1024) + " KiB");
        }
        buf.erase(buf.begin(), buf.begin() + pos);
    }
}

// --- runtime -----------------------------------------------------------------
// Pick the controller to run on. Boards can have more than one -- an SoC with
// two OTG cores, or a dwc3 and a dwc2 on different ports -- and only the one
// wired to the port the goggles plug into will do, so say what we found.
static std::string pick_udc(const std::string &wanted) {
    const std::vector<std::string> udcs = FunctionFsGadget::list_udcs();
    if (udcs.empty()) {
        log( "no UDC under /sys/class/udc - is the port in device/peripheral mode?");
        return {};
    }
    if (!wanted.empty()) {
        for (const std::string &u : udcs)
            if (u == wanted) return u;
        log("no such UDC: " + wanted);
        log("this board has: " + comma_separated(udcs));
        return {};
    }
    if (udcs.size() > 1)
        log("this board has several UDCs (" + comma_separated(udcs) + "); using " + udcs.front()
            + " - pick another with --udc");
    return udcs.front();
}

static Fd open_sink(const std::string &path) {
    if (path == "stdout") return Fd(dup(STDOUT_FILENO));
    return Fd(open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
}

// Sessions that fail back to back without ever streaming: give up and let the
// supervisor restart us, rather than logging the same failure forever.
static constexpr int MAX_CONSECUTIVE_FAILURES = 5;

int run_capture(const std::string &out_path, const Options &opt) {
    // Attaching to a FunctionFS that somebody else binds needs no controller.
    const bool needs_udc = opt.ffs_path.empty() || !opt.gadget_dir.empty();
    std::string udc;
    if (needs_udc && (udc = pick_udc(opt.udc)).empty()) return 1;

    FunctionFsGadget gadget(opt.gadget_name, udc);
    if (!opt.mount_point.empty()) gadget.set_mount_point(opt.mount_point);

    const GadgetIdentity id = aoa_identity();
    const bool up = opt.ffs_path.empty() ? gadget.setup(id)
                                         : gadget.attach(opt.ffs_path, opt.gadget_dir, id);
    if (!up) return 1;

    const Fd sink = open_sink(out_path);
    if (sink < 0) { perror(("open " + out_path).c_str()); return 1; }

    Flag enabled{false};
    Worker events([&](const Flag &quit) { event_loop(gadget.ep0(), enabled, quit); });
    log("descriptors written; plug in the goggles");

    // Each pass is one plug-in: FunctionFS invalidates the endpoint files when
    // the host disables the interface, so they are reopened per session. The
    // main thread never touches an endpoint itself; it only watches the flags
    // and stops the workers, so a stop signal always has somewhere to land.
    int failures = 0;
    while (!g_stop) {
        if (!enabled) { usleep(100000); continue; }
        usleep(200000);                                  // the real receiver waits ~200 ms

        Session s;
        const auto in  = Fd(gadget.open_endpoint("ep1"));  // commands out to the goggles
        const auto out = Fd(gadget.open_endpoint("ep2"));  // video in from the goggles
        if (in < 0 || out < 0) {
            s.fail("open endpoints");
        } else {
            log("streaming video -> " + out_path);
            Worker sender  ([&](const Flag &quit) { send_commands(in, s, quit); });
            Worker receiver([&](const Flag &quit) { receive_video(out, sink, s, quit); });
            while (!g_stop && enabled && !s.over) usleep(50000);
        }                                                // workers stopped here, before the fds close

        if (s.sink_closed) return 1;
        failures = s.failed ? failures + 1 : 0;
        if (failures >= MAX_CONSECUTIVE_FAILURES) {
            log("giving up after " + std::to_string(failures) + " failed sessions");
            return 1;
        }
        if (s.failed) usleep(500000);
    }

    log("stopped");
    return 0;
}

// --- main --------------------------------------------------------------------
static void usage() {
    fprintf(stderr,
        "usage: goggles_capture [OUTPUT|stdout] [options]\n"
        "\n"
        "  OUTPUT            file to write the H.264 elementary stream to\n"
        "                    (default ./goggles_feed.h264; 'stdout' pipes it)\n"
        "\n"
        "Controller\n"
        "  --udc NAME        device controller to bind; default is the board's\n"
        "                    only one. --list-udc shows what this board has.\n"
        "  --list-udc        list the device controllers, then exit\n"
        "\n"
        "Self-composed gadget (default)\n"
        "  --gadget NAME     configfs gadget + FunctionFS instance name\n"
        "                    (default 'goggles')\n"
        "  --mount PATH      where to mount FunctionFS (default /dev/ffs-<NAME>)\n"
        "  --cleanup         remove a gadget left behind by a killed run, then exit\n"
        "\n"
        "Attach to a gadget composed elsewhere\n"
        "  --ffs PATH        FunctionFS is already mounted here; leave configfs alone\n"
        "  --gadget-dir DIR  with --ffs: bind and unbind this configfs gadget\n"
        "                    around the run. Omit it to leave the UDC alone too.\n");
}

int main(const int argc, char **argv) {
    std::string out_path = "./goggles_feed.h264";
    Options opt;
    bool cleanup = false, list_udc = false;

    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        const bool has_value = i + 1 < argc;
        if      (a == "--cleanup")                 cleanup   = true;
        else if (a == "--list-udc")                list_udc  = true;
        else if (a == "--udc"        && has_value) opt.udc         = argv[++i];
        else if (a == "--gadget"     && has_value) opt.gadget_name = argv[++i];
        else if (a == "--mount"      && has_value) opt.mount_point = argv[++i];
        else if (a == "--ffs"        && has_value) opt.ffs_path    = argv[++i];
        else if (a == "--gadget-dir" && has_value) opt.gadget_dir  = argv[++i];
        else if (a == "-h" || a == "--help")     { usage(); return 0; }
        else if (!a.empty() && a[0] == '-')      { usage(); return 1; }
        else out_path = a;
    }

    if (list_udc) {
        const std::vector<std::string> udcs = FunctionFsGadget::list_udcs();
        if (udcs.empty()) { log( "no UDC under /sys/class/udc - is the port in device/peripheral mode?"); return 1; }
        for (const std::string &u : udcs) printf("%s\n", u.c_str());
        return 0;
    }

    if (!opt.ffs_path.empty() && (!opt.mount_point.empty() || cleanup)) {
        log("--ffs attaches to an existing gadget; --mount and --cleanup do not apply to it");
        return 1;
    }
    if (!opt.gadget_dir.empty() && opt.ffs_path.empty()) {
        log("--gadget-dir only means something together with --ffs");
        return 1;
    }
    if (geteuid() != 0) {
        log("must run as root");
        return 1;
    }

    if (cleanup) {
        // The stale gadget is removed either way; the controller is only
        // needed to hand it back to its previous owner.
        const std::string udc = pick_udc(opt.udc);
        if (udc.empty() && !opt.udc.empty()) return 1;
        return FunctionFsGadget::cleanup_stale(opt.gadget_name, opt.mount_point, udc) ? 0 : 1;
    }

    catch_signal(SIGINT,  on_stop);
    catch_signal(SIGTERM, on_stop);
    install_worker_kick_handler();
    signal(SIGPIPE, SIG_IGN);                            // downstream pipe may close first
    return run_capture(out_path, opt);
}
