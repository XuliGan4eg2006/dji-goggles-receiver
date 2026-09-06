#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <cstddef>
#include <sys/types.h>

// --- USB identity ------------------------------------------------------------
constexpr uint16_t VID = 0x18D1, PID = 0x2D01;          // Google, AOA accessory
constexpr uint8_t  VIDEO_CHANNEL = 0x4A;

// --- DJI DUML checksums (CRC-8/Maxim seed 0x77, CRC-16/X-25 seed 0x3692) -----
uint8_t  crc8(const uint8_t *d, size_t n);
uint16_t crc16(const uint8_t *d, size_t n);

// --- DUML keep-alive commands ------------------------------------------------
extern const std::vector<std::vector<uint8_t>> COMMANDS;

std::vector<uint8_t> build_command(const std::vector<uint8_t> &inner, uint16_t seq);

// --- Runtime -----------------------------------------------------------------
// How the gadget should be brought up. An empty ffs_path means "compose it
// ourselves"; otherwise we attach to a FunctionFS instance someone else set up
// (an init script, an existing multi-function gadget).
struct Options {
    std::string udc;                        // empty => the board's only UDC
    std::string gadget_name = "goggles";    // configfs gadget + ffs instance name
    std::string mount_point;                // empty => /dev/ffs-<gadget_name>
    std::string ffs_path;                   // attach mode
    std::string gadget_dir;                 // attach mode: gadget to bind, optional
};

ssize_t find_sps(const uint8_t *p, size_t n);
// out_path "stdout" pipes the stream. Returns the process exit status.
int run_capture(const std::string &out_path, const Options &opt);
