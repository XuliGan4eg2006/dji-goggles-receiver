#pragma once

#include <vector>
#include <cstdint>
#include <cstddef>
#include <sys/types.h>

// --- USB identity ----------
constexpr uint16_t VID = 0x18D1, PID = 0x2D01;
constexpr uint8_t  EP_IN = 0x81, EP_OUT = 0x02;   // IN = commands, OUT = video
constexpr uint16_t LANGID = 0x0409;
constexpr uint8_t  VIDEO_CHANNEL = 0x4A;

// --- DJI DUML checksums (CRC-8/Maxim seed 0x77, CRC-16/X-25 seed 0x3692) -----
void     make_tables();
uint8_t  crc8(const uint8_t *d, size_t n);
uint16_t crc16(const uint8_t *d, size_t n);

// --- DUML keep-alive commands ------------------------------------------------
extern const std::vector<std::vector<uint8_t>> COMMANDS;

std::vector<uint8_t> build_command(const std::vector<uint8_t> &inner, uint16_t seq);

// --- USB descriptors -----------------------------------------
std::vector<uint8_t> build_descriptors();
std::vector<uint8_t> string_descriptor(int index);

// --- Runtime ------------------------------------------------------------
ssize_t find_sps(const uint8_t *p, size_t n);
int run_capture(const char *out_path);
