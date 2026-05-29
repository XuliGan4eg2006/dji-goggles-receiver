#include "goggles_capture.h"

#include <atomic>
#include <thread>
#include <string>
#include <cstdio>
#include <cstring>
#include <csignal>

#include <fcntl.h>
#include <unistd.h>
#include <glob.h>

// --- Android Open Accessory identity strings ---------------------------------
static const char *STRINGS[] = {
    nullptr,
    "Google Inc.",
    "Android-powered device in accessory mode",
    "da64sxd1",
    "High speed configuration",
    "Low speed configuration",
    "Android Accessory Interface",
};

static std::atomic<bool> g_stop{false};
static void on_signal(int) { g_stop = true; }

static void log(const std::string &s) {
    fprintf(stderr, "%s\n", s.c_str());
}

// --- DJI DUML checksums (CRC-8/Maxim seed 0x77, CRC-16/X-25 seed 0x3692) -----
static uint8_t  CRC8_TABLE[256];
static uint16_t CRC16_TABLE[256];

void make_tables() {
    for (int i = 0; i < 256; i++) {
        uint32_t c8 = i, c16 = i;
        for (int k = 0; k < 8; k++) {
            c8  = (c8  & 1) ? (c8  >> 1) ^ 0x8C   : c8  >> 1;
            c16 = (c16 & 1) ? (c16 >> 1) ^ 0x8408 : c16 >> 1;
        }
        CRC8_TABLE[i] = c8;
        CRC16_TABLE[i] = c16;
    }
}

uint8_t crc8(const uint8_t *d, size_t n) {
    uint8_t c = 0x77;
    for (size_t i = 0; i < n; i++) c = CRC8_TABLE[c ^ d[i]];
    return c;
}

uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t c = 0x3692;
    for (size_t i = 0; i < n; i++) c = CRC16_TABLE[(c ^ d[i]) & 0xFF] ^ (c >> 8);
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

    uint16_t n = p.size();
    std::vector<uint8_t> out = {0x55, 0xCC, 0x49, 0x57,
                                static_cast<uint8_t>(n & 0xFF), static_cast<uint8_t>(n >> 8),
                                static_cast<uint8_t>(seq & 0xFF), static_cast<uint8_t>(seq >> 8)};
    out.insert(out.end(), p.begin(), p.end());
    return out;
}

// --- USB descriptors -----------------------------------------
static void add_endpoint(std::vector<uint8_t> &v, const uint8_t addr, const uint16_t max_packet) {
    uint8_t d[] = {7, 5, addr, 0x02, static_cast<uint8_t>(max_packet & 0xFF), static_cast<uint8_t>(max_packet >> 8), 0};
    v.insert(v.end(), d, d + sizeof(d));
}

static std::vector<uint8_t> config_block() {
    std::vector<uint8_t> iface = {9, 4, 0, 0, 2, 0xFF, 0xFF, 0, 6};
    std::vector<uint8_t> eps;
    add_endpoint(eps, EP_IN, 0x0A00);
    add_endpoint(eps, EP_OUT, 0x0200);
    const uint16_t total = 9 + iface.size() + eps.size();
    std::vector<uint8_t> cfg = {9, 2, static_cast<uint8_t>(total & 0xFF), static_cast<uint8_t>(total >> 8), 1, 1, 4, 0xC0, 1};
    cfg.insert(cfg.end(), iface.begin(), iface.end());
    cfg.insert(cfg.end(), eps.begin(), eps.end());
    return cfg;
}

std::vector<uint8_t> build_descriptors() {
    std::vector<uint8_t> dev = {18, 1, 0x00, 0x02, 0, 0, 0, 64,
                                static_cast<uint8_t>(VID & 0xFF), static_cast<uint8_t>(VID >> 8),
                                static_cast<uint8_t>(PID & 0xFF), static_cast<uint8_t>(PID >> 8),
                                0x00, 0x02, 1, 2, 3, 1};
    std::vector<uint8_t> cfg = config_block();
    std::vector<uint8_t> out = {0, 0, 0, 0};            // 4-byte tag = 0
    out.insert(out.end(), cfg.begin(), cfg.end());      // full-speed config
    out.insert(out.end(), cfg.begin(), cfg.end());      // high-speed config
    out.insert(out.end(), dev.begin(), dev.end());
    return out;
}

std::vector<uint8_t> string_descriptor(const int index) {
    std::vector<uint8_t> body;
    if (index == 0) {
        body = {static_cast<uint8_t>(LANGID & 0xFF), static_cast<uint8_t>(LANGID >> 8)};
    } else if (index >= 1 && index <= 6) {
        for (const char *p = STRINGS[index]; *p; p++) { body.push_back(*p); body.push_back(0); }
    } else {
        return {};
    }
    std::vector<uint8_t> d = {static_cast<uint8_t>(body.size() + 2), 3};
    d.insert(d.end(), body.begin(), body.end());
    return d;
}

// --- EP0 control handling ----------------------------------------------------
// gadgetfs delivers 12-byte events: 8-byte setup packet + 4-byte type.
static const int EVENT_SIZE = 12;
enum { CONNECT = 1, DISCONNECT = 2, SETUP = 3 };

// Confirm a no-data request: zero-length I/O in the data-stage direction.
static void ack(const int fd, const bool host_to_device) {
    char x;
    if (host_to_device) (void)!read(fd, &x, 0); else (void)!write(fd, &x, 0);
}
// Reject a request: zero-length I/O in the OPPOSITE direction to ack().
static void stall(const int fd, const bool host_to_device) {
    char x;
    if (host_to_device) (void)!write(fd, &x, 0); else (void)!read(fd, &x, 0);
}

static void handle_setup(const int fd, const uint8_t *s, std::atomic<bool> &configured) {
    const uint8_t request = s[1];
    const uint16_t value = s[2] | (s[3] << 8);
    const uint16_t length = s[6] | (s[7] << 8);
    const bool device_to_host = s[0] & 0x80;

    if (request == 6 && device_to_host) {               // GET_DESCRIPTOR
        if ((value >> 8) == 3) {                        // ...a string
            const std::vector<uint8_t> d = string_descriptor(value & 0xFF);
            if (d.empty()) {
                stall(fd, false); return;
            }
            const size_t n = (length && length < d.size()) ? length : d.size();
            (void)!write(fd, d.data(), n);
        } else {
            stall(fd, false);
        }
    } else if (request == 9) {                          // SET_CONFIGURATION
        if (value == 1 && !configured.exchange(true)) log("USB: configured");
        ack(fd, true);
    } else if (request == 11) {                         // SET_INTERFACE
        ack(fd, true);
    } else if (request == 10 && device_to_host) {       // GET_INTERFACE
        constexpr uint8_t zero = 0;
        (void)!write(fd, &zero, length ? 1 : 0);
    } else {
        stall(fd, device_to_host);
    }
}

static void control_loop(const int fd, std::atomic<bool> &configured) {
    uint8_t buf[EVENT_SIZE * 8];
    while (!g_stop) {
        const ssize_t n = read(fd, buf, sizeof(buf));
        if (n <= 0) {
            usleep(1000);
            continue;
        }

        for (ssize_t i = 0; i + EVENT_SIZE <= n; i += EVENT_SIZE) {
            const uint8_t *ev = buf + i;
            if (ev[8] == SETUP) {
                handle_setup(fd, ev, configured);
            }
            else if (ev[8] == DISCONNECT) {
                log("USB: disconnect");
                configured = false;
            }
        }
    }
}

// --- endpoints ---------------------------------------------------------------
static int open_endpoint(const char *path, const uint8_t addr) {
    const int fd = open(path, O_RDWR);
    if (fd < 0) return -1;
    std::vector<uint8_t> cfg = {1, 0, 0, 0};            // 4-byte tag = 1
    add_endpoint(cfg, addr, 64);                        // full speed
    add_endpoint(cfg, addr, 512);                       // high speed
    if (write(fd, cfg.data(), cfg.size()) < 0) {
        close(fd); return -1;
    }
    return fd;
}

// --- the two data flows ------------------------------------------------------
// Keep-alive: replay the DUML commands ~once a second, forever.
static void send_commands(const int fd) {
    uint16_t seq = 0;
    while (!g_stop) {
        for (const auto &inner : COMMANDS) {
            std::vector<uint8_t> pkt = build_command(inner, seq++);
            (void)!write(fd, pkt.data(), pkt.size());
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

// Read the goggles' stream, unwrap 55 CC frames, write channel 0x4A to sink.
static void receive_video(const int fd, FILE *sink) {
    std::vector<uint8_t> buf;
    uint8_t chunk[16384];
    size_t saved = 0;
    bool started = false;   // only emit once we've seen the first SPS

    while (!g_stop) {
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            if (g_stop) break; usleep(1000); continue;
        }
        buf.insert(buf.end(), chunk, chunk + n);

        size_t pos = 0;
        while (buf.size() - pos >= 8) {
            if (buf[pos] != 0x55 || buf[pos + 1] != 0xCC) {
                pos++;
                continue;
            }
            const uint16_t length = buf[pos + 4] | (buf[pos + 5] << 8);

            if (buf.size() - pos < static_cast<size_t>(8 + length)) break;

            const uint8_t channel = buf[pos + 2];
            if (channel == VIDEO_CHANNEL && length > 0) {
                const uint8_t *payload = &buf[pos + 8];
                size_t plen = length;
                if (!started) {                          // skip the partial leading NAL
                    const ssize_t sps = find_sps(payload, plen);
                    if (sps < 0) { pos += 8 + length; continue; }
                    payload += sps;
                    plen -= sps;
                    started = true;
                    log(">>> first SPS seen - clean H.264 stream starts here");
                }
                fwrite(payload, 1, plen, sink);
                fflush(sink);                            // low latency when piped
                saved += plen;
                if (saved % (1 << 20) < plen) log("  wrote " + std::to_string(saved / 1024) + " KiB");
            }
            pos += 8 + length;
        }
        buf.erase(buf.begin(), buf.begin() + pos);
    }
}

// --- runtime -----------------------------------------------------------------
int run_capture(const char *out_path) {
    glob_t g;
    if (glob("/dev/gadget/*.usb", 0, nullptr, &g) != 0 || g.gl_pathc == 0) {
        log("no UDC under /dev/gadget - is gadgetfs mounted?");
        return 1;
    }
    const std::string udc = g.gl_pathv[0];
    globfree(&g);
    log("using UDC: " + udc);

    int udc_fd = open(udc.c_str(), O_RDWR | O_NONBLOCK);
    if (udc_fd < 0) {
        perror("open udc");
        return 1;
    }

    const std::vector<uint8_t> desc = build_descriptors();
    if (write(udc_fd, desc.data(), desc.size()) < 0) {
        perror("write descriptors");
        return 1;
    }

    log("descriptors written; plug in the goggles");

    std::atomic<bool> configured{false};
    std::thread control(control_loop, udc_fd, std::ref(configured));

    while (!configured && !g_stop) {
        usleep(100000);
    }

    if (g_stop) {
        control.join(); return 0;
    }

    usleep(200000);                                     // the real receiver waits ~200 ms
    const int in_fd = open_endpoint("/dev/gadget/ep1in", EP_IN);
    const int out_fd = open_endpoint("/dev/gadget/ep2out", EP_OUT);
    if (in_fd < 0 || out_fd < 0) {
        log("failed to open endpoints");
        return 1;
    }

    FILE *sink = (strcmp(out_path, "stdout") == 0) ? stdout : fopen(out_path, "wb");
    if (!sink) {
        perror("open output"); return 1;
    }
    log(std::string("streaming video -> ") + (sink == stdout ? "stdout" : out_path));

    std::thread sender(send_commands, in_fd);
    receive_video(out_fd, sink);

    g_stop = true;
    close(out_fd);                                      // unblock the reader if needed
    sender.join();
    control.join();
    if (sink != stdout) {
        fclose(sink);
    }

    log("stopped");
    return 0;
}

// --- main --------------------------------------------------------------------
int main(int argc, char **argv) {
    make_tables();

    if (geteuid() != 0) {
        fprintf(stderr, "must run as root\n"); return 1;
    }

    const char *out_path = (argc > 1) ? argv[1] : "./goggles_feed.h264";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    return run_capture(out_path);
}
