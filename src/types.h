#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct ParsedPSD {
    uint32_t width = 0;
    uint32_t height = 0;
    uint16_t channel_count = 0;
    uint16_t bit_depth = 0;      // 8, 16, or 32
    uint16_t color_mode = 0;     // 3=RGB, 4=CMYK, 7=Multichannel, 9=Lab

    struct ChannelInfo {
        int16_t channel_id = 0;       // 0,1,2... process, -1,-2... spot
        std::string name;
        uint16_t color_space = 0;     // 0=RGB, 2=CMYK, 7=Lab
        int16_t color_components[4] = {0, 0, 0, 0};
        bool has_color = false;
        uint16_t solidity = 100;      // Ink opacity 0-100% from resource 1077
    };
    std::vector<ChannelInfo> channels;

    // One buffer per channel, planar order
    // Size per buffer: width * height * (bit_depth / 8)
    std::vector<std::vector<uint8_t>> pixel_data;

    // Raw ICC profile bytes from resource 1039
    std::vector<uint8_t> icc_profile;
};

struct RGBImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> pixels;  // RGB interleaved, row-major
};
