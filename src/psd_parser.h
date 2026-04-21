#pragma once
#include "types.h"
#include <string>
#include <stdexcept>

struct PSDError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct PSDHeader {
    uint16_t version = 0;
    uint16_t channel_count = 0;
    uint32_t height = 0;
    uint32_t width = 0;
    uint16_t depth = 0;
    uint16_t color_mode = 0;
};

// Parse a PSD file, extracting merged composite image data + metadata.
ParsedPSD parse_psd(const std::string& filepath);
