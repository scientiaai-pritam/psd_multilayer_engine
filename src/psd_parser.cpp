#include "psd_parser.h"
#include "binary_reader.h"
#include <zlib.h>
#include <vector>
#include <cstring>
#include <algorithm>

// --- File Header ---

static PSDHeader parse_header(BinaryReader& r) {
    // Verify signature
    uint8_t sig[4];
    r.read_bytes(sig, 4);
    if (sig[0] != '8' || sig[1] != 'B' || sig[2] != 'P' || sig[3] != 'S') {
        throw PSDError("Not a PSD file: invalid signature");
    }

    PSDHeader h;
    h.version = r.read_u16();
    if (h.version != 1) {
        throw PSDError("Unsupported PSD version: " + std::to_string(h.version) +
                       " (only version 1/PSD supported)");
    }

    r.skip(6); // reserved

    h.channel_count = r.read_u16();
    h.height = r.read_u32();
    h.width = r.read_u32();
    h.depth = r.read_u16();
    h.color_mode = r.read_u16();

    // Validate
    if (h.channel_count < 1 || h.channel_count > 56) {
        throw PSDError("Invalid channel count: " + std::to_string(h.channel_count));
    }
    if (h.depth != 8 && h.depth != 16 && h.depth != 32) {
        throw PSDError("Unsupported bit depth: " + std::to_string(h.depth));
    }
    if (h.color_mode != 3 && h.color_mode != 4 && h.color_mode != 7 && h.color_mode != 9) {
        throw PSDError("Unsupported color mode: " + std::to_string(h.color_mode));
    }

    return h;
}

// --- Image Resources ---

struct SpotColorEntry {
    int32_t channel_id = 0;
    uint16_t color_space = 0;
    int16_t components[4] = {0, 0, 0, 0};
    uint16_t solidity = 100;  // opacity 0-100% from resource 1077
};

struct ResourceData {
    std::vector<uint8_t> icc_profile;
    std::vector<std::string> channel_names;
    std::vector<SpotColorEntry> spot_colors;
    std::vector<SpotColorEntry> display_info;
};

static std::vector<SpotColorEntry> parse_resource_1067(const uint8_t* data, size_t len) {
    // Resource 1067: Alternate Spot Colors
    // 2 bytes version, 2 bytes count, then per entry: 4 bytes channel ID + 10 bytes Color
    if (len < 4) return {};

    uint16_t version = (uint16_t(data[0]) << 8) | data[1];
    uint16_t count   = (uint16_t(data[2]) << 8) | data[3];
    (void)version;

    std::vector<SpotColorEntry> entries;
    size_t offset = 4;
    for (uint16_t i = 0; i < count && offset + 14 <= len; ++i) {
        SpotColorEntry e;
        e.channel_id = (int32_t(
            (uint32_t(data[offset]) << 24) | (uint32_t(data[offset+1]) << 16) |
            (uint32_t(data[offset+2]) << 8) | data[offset+3]));
        offset += 4;

        e.color_space = (uint16_t(data[offset]) << 8) | data[offset+1];
        offset += 2;

        for (int j = 0; j < 4; ++j) {
            e.components[j] = (int16_t((uint16_t(data[offset]) << 8) | data[offset+1]));
            offset += 2;
        }
        entries.push_back(e);
    }
    return entries;
}

static std::vector<SpotColorEntry> parse_resource_1077(const uint8_t* data, size_t len) {
    // Resource 1077: DisplayInfo
    // Format: 2 bytes version + 2 bytes (flags, not entry count) + N entries of 13 bytes each
    // Per entry: 2 bytes color space + 8 bytes components + 2 bytes solidity(%) + 1 byte kind
    // Entry count is derived from resource size: (len - 4) / 13
    if (len < 4) return {};

    // Derive entry count from data size
    size_t count = (len - 4) / 13;

    std::vector<SpotColorEntry> entries;
    size_t offset = 4;
    for (size_t i = 0; i < count && offset + 13 <= len; ++i) {
        SpotColorEntry e;
        e.channel_id = static_cast<int32_t>(i); // DisplayInfo is indexed sequentially
        e.color_space = (uint16_t(data[offset]) << 8) | data[offset+1];
        offset += 2;
        for (int j = 0; j < 4; ++j) {
            e.components[j] = (int16_t((uint16_t(data[offset]) << 8) | data[offset+1]));
            offset += 2;
        }
        e.solidity = (uint16_t(data[offset]) << 8) | data[offset+1]; // opacity 0-100
        offset += 2;
        offset += 1; // kind byte
        entries.push_back(e);
    }
    return entries;
}

static std::vector<std::string> parse_resource_1045(const uint8_t* data, size_t len) {
    // Resource 1045: Unicode Alpha Names
    // Repeated: 4-byte length (UTF-16 code units), then UTF-16 chars + null terminator
    std::vector<std::string> names;
    size_t offset = 0;
    while (offset + 4 <= len) {
        uint32_t char_count = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset+1]) << 16) |
                              (uint32_t(data[offset+2]) << 8) | data[offset+3];
        offset += 4;
        if (char_count == 0 || offset + char_count * 2 > len) break;

        std::string name;
        for (uint32_t i = 0; i < char_count; ++i) {
            uint16_t ch = (uint16_t(data[offset]) << 8) | data[offset+1];
            offset += 2;
            if (ch == 0) break;
            if (ch < 0x80) name += static_cast<char>(ch);
            else if (ch < 0x800) {
                name += static_cast<char>(0xC0 | (ch >> 6));
                name += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                name += static_cast<char>(0xE0 | (ch >> 12));
                name += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                name += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        names.push_back(name);
    }
    return names;
}

static ResourceData parse_image_resources(BinaryReader& r) {
    ResourceData result;
    uint32_t section_len = r.read_u32();
    size_t section_end = r.position() + section_len;

    while (r.position() < section_end) {
        // Verify resource block signature
        uint8_t sig[4];
        r.read_bytes(sig, 4);
        if (sig[0] != '8' || sig[1] != 'B' || sig[2] != 'I' || sig[3] != 'M') {
            throw PSDError("Invalid image resource signature");
        }

        uint16_t resource_id = r.read_u16();
        r.read_pascal_string();  // name (usually empty)
        uint32_t data_len = r.read_u32();

        // Only extract resources we care about
        if (resource_id == 1039 && data_len > 0) {
            // ICC Profile
            result.icc_profile = r.read_vec(data_len);
        } else if (resource_id == 1045 && data_len > 0) {
            // Unicode Alpha Names
            auto raw = r.read_vec(data_len);
            result.channel_names = parse_resource_1045(raw.data(), raw.size());
        } else if (resource_id == 1067 && data_len > 0) {
            // Alternate Spot Colors
            auto raw = r.read_vec(data_len);
            result.spot_colors = parse_resource_1067(raw.data(), raw.size());
        } else if (resource_id == 1077 && data_len > 0) {
            // DisplayInfo
            auto raw = r.read_vec(data_len);
            result.display_info = parse_resource_1077(raw.data(), raw.size());
        } else {
            // Skip unneeded resources
            r.skip(data_len);
        }

        // Resource data is padded to even size
        if (data_len % 2 != 0) r.skip(1);
    }

    // Ensure we're at the end of the section
    if (r.position() != section_end) {
        r.seek(section_end);
    }

    return result;
}

// --- Image Data Decompression ---

static std::vector<uint8_t> decompress_raw(BinaryReader& r, size_t byte_count) {
    return r.read_vec(byte_count);
}

static std::vector<uint8_t> decompress_rle(BinaryReader& r,
                                            uint32_t width, uint32_t height,
                                            uint16_t channel_count,
                                            uint16_t bytes_per_sample) {
    // RLE: first read byte counts for each scanline of each channel
    size_t total_scanlines = (size_t)height * channel_count;
    std::vector<uint16_t> scanline_bytecounts(total_scanlines);
    for (size_t i = 0; i < total_scanlines; ++i) {
        scanline_bytecounts[i] = r.read_u16();
    }

    size_t row_bytes = (size_t)width * bytes_per_sample;
    std::vector<uint8_t> result((size_t)width * height * bytes_per_sample * channel_count);
    size_t out_offset = 0;

    for (size_t sl = 0; sl < total_scanlines; ++sl) {
        // Read the compressed scanline data
        std::vector<uint8_t> compressed = r.read_vec(scanline_bytecounts[sl]);

        // PackBits decompression
        size_t in_pos = 0;
        size_t row_written = 0;
        while (in_pos < compressed.size() && row_written < row_bytes) {
            int8_t n = static_cast<int8_t>(compressed[in_pos++]);
            if (n >= 0) {
                // Copy n+1 bytes literally
                int count = n + 1;
                for (int i = 0; i < count && row_written < row_bytes; ++i) {
                    result[out_offset++] = compressed[in_pos++];
                    row_written++;
                }
            } else if (n > -128) {
                // Repeat next byte (-n+1) times
                int count = -n + 1;
                uint8_t val = compressed[in_pos++];
                for (int i = 0; i < count && row_written < row_bytes; ++i) {
                    result[out_offset++] = val;
                    row_written++;
                }
            }
            // n == -128: no-op
        }
    }

    return result;
}

static std::vector<uint8_t> decompress_zip(BinaryReader& r, size_t compressed_size, size_t expected_size) {
    // Minimal zlib inflate using the low-level API
    // For now, read raw bytes and use a simple inflate approach
    std::vector<uint8_t> compressed = r.read_vec(compressed_size);

    // Allocate output buffer
    std::vector<uint8_t> output(expected_size);

    // Use zlib's uncompress
    uLongf dest_len = expected_size;
    int ret = uncompress(output.data(), &dest_len, compressed.data(), compressed_size);
    if (ret != Z_OK) {
        throw PSDError("ZIP decompression failed: error " + std::to_string(ret));
    }

    return output;
}

static void undo_prediction(std::vector<uint8_t>& data, uint32_t width, uint32_t height,
                             uint16_t channel_count, uint16_t bytes_per_sample) {
    // ZIP with prediction: horizontal differencing per row, per channel
    size_t row_bytes = (size_t)width * bytes_per_sample;
    for (size_t ch = 0; ch < channel_count; ++ch) {
        for (size_t row = 0; row < height; ++row) {
            size_t row_start = (ch * height + row) * row_bytes;
            if (bytes_per_sample == 1) {
                for (size_t x = 1; x < width; ++x) {
                    data[row_start + x] += data[row_start + x - 1];
                }
            } else if (bytes_per_sample == 2) {
                for (size_t x = 1; x < width; ++x) {
                    size_t idx = row_start + x * 2;
                    // Big-endian 16-bit: add previous to current
                    int16_t prev = (int16_t((uint16_t(data[idx-2]) << 8)) | data[idx-1]);
                    int16_t curr = (int16_t((uint16_t(data[idx]) << 8)) | data[idx+1]);
                    int16_t sum = prev + curr;
                    data[idx]   = (sum >> 8) & 0xFF;
                    data[idx+1] = sum & 0xFF;
                }
            }
        }
    }
}

static std::vector<std::vector<uint8_t>> parse_image_data(BinaryReader& r,
                                                           const PSDHeader& header) {
    uint16_t compression = r.read_u16();
    size_t pixels_per_channel = (size_t)header.width * header.height;
    uint16_t bytes_per_sample = header.depth / 8;
    size_t channel_byte_count = pixels_per_channel * bytes_per_sample;

    std::vector<uint8_t> raw_data;

    switch (compression) {
        case 0: // Raw
            raw_data = decompress_raw(r, channel_byte_count * header.channel_count);
            break;

        case 1: // RLE
            raw_data = decompress_rle(r, header.width, header.height,
                                      header.channel_count, bytes_per_sample);
            break;

        case 2: // ZIP without prediction
            {
                // For PSD, the entire image data section is compressed as one block
                // We need to figure out how many bytes remain
                // Actually, for compression 2 and 3, the data length is the rest of the section
                // This is computed from the section length which we don't have here.
                // Workaround: read to end of file
                size_t pos = r.position();
                r.skip(0); // just to get position
                // Read all remaining data
                std::vector<uint8_t> all_remaining;
                all_remaining.reserve(channel_byte_count * header.channel_count);
                // Read in chunks
                const size_t chunk = 65536;
                while (true) {
                    uint8_t buf[chunk];
                    r.read_bytes(buf, std::min(chunk, channel_byte_count * header.channel_count - all_remaining.size()));
                    size_t got = std::min(chunk, channel_byte_count * header.channel_count - all_remaining.size());
                    all_remaining.insert(all_remaining.end(), buf, buf + got);
                    if (all_remaining.size() >= channel_byte_count * header.channel_count) break;
                }
                raw_data = all_remaining;

                uLongf dest_len = channel_byte_count * header.channel_count;
                std::vector<uint8_t> inflated(dest_len);
                int ret = uncompress(inflated.data(), &dest_len,
                                     all_remaining.data(), all_remaining.size());
                if (ret != Z_OK) {
                    throw PSDError("ZIP decompression failed");
                }
                raw_data = inflated;
            }
            break;

        case 3: // ZIP with prediction
            {
                // Same as case 2, then undo prediction
                size_t pos = r.position();
                std::vector<uint8_t> all_remaining;
                all_remaining.reserve(channel_byte_count * header.channel_count * 2);
                const size_t chunk = 65536;
                while (true) {
                    uint8_t buf[chunk];
                    size_t remaining = channel_byte_count * header.channel_count * 2 - all_remaining.size();
                    if (remaining == 0) break;
                    size_t to_read = std::min(chunk, remaining);
                    try {
                        r.read_bytes(buf, to_read);
                    } catch (...) {
                        break;
                    }
                    all_remaining.insert(all_remaining.end(), buf, buf + to_read);
                }

                uLongf dest_len = channel_byte_count * header.channel_count;
                std::vector<uint8_t> inflated(dest_len);
                int ret = uncompress(inflated.data(), &dest_len,
                                     all_remaining.data(), all_remaining.size());
                if (ret != Z_OK) {
                    throw PSDError("ZIP+prediction decompression failed");
                }
                raw_data = inflated;
                undo_prediction(raw_data, header.width, header.height,
                               header.channel_count, bytes_per_sample);
            }
            break;

        default:
            throw PSDError("Unsupported compression: " + std::to_string(compression));
    }

    // Split planar data into per-channel vectors
    std::vector<std::vector<uint8_t>> channels;
    channels.reserve(header.channel_count);
    for (uint16_t ch = 0; ch < header.channel_count; ++ch) {
        size_t offset = (size_t)ch * channel_byte_count;
        channels.emplace_back(
            raw_data.begin() + offset,
            raw_data.begin() + offset + channel_byte_count
        );
    }

    return channels;
}

// --- Main Parse Function ---

ParsedPSD parse_psd(const std::string& filepath) {
    BinaryReader reader(filepath);

    // Section 1: File Header
    PSDHeader header = parse_header(reader);

    // Section 2: Color Mode Data (skip — empty for multichannel/RGB/CMYK/Lab)
    uint32_t color_mode_len = reader.read_u32();
    reader.skip(color_mode_len);

    // Section 3: Image Resources
    ResourceData resources = parse_image_resources(reader);

    // Section 4: Layer and Mask Information (skip — we use merged composite)
    uint32_t layer_mask_len = reader.read_u32();
    reader.skip(layer_mask_len);

    // Section 5: Image Data
    auto pixel_data = parse_image_data(reader, header);

    // Assemble result
    ParsedPSD result;
    result.width = header.width;
    result.height = header.height;
    result.channel_count = header.channel_count;
    result.bit_depth = header.depth;
    result.color_mode = header.color_mode;
    result.pixel_data = std::move(pixel_data);
    result.icc_profile = std::move(resources.icc_profile);

    // Build channel info from metadata
    result.channels.resize(header.channel_count);

    // First, try resource 1067 (spot colors)
    // Resource 1067 entries are in sequential order matching pixel data channels.
    // Entry index 0 = pixel channel 0, entry 1 = pixel channel 1, etc.
    // The channel_id field is Photoshop's internal ID, NOT a pixel data index.
    for (size_t i = 0; i < resources.spot_colors.size() && i < result.channels.size(); ++i) {
        const auto& sc = resources.spot_colors[i];
        result.channels[i].channel_id = sc.channel_id;
        result.channels[i].color_space = sc.color_space;
        for (int j = 0; j < 4; ++j) {
            result.channels[i].color_components[j] = sc.components[j];
        }
        result.channels[i].has_color = true;
    }

    // Apply DisplayInfo (resource 1077) for solidity and fallback color
    // DisplayInfo is indexed sequentially matching pixel data channel order
    for (size_t i = 0; i < resources.display_info.size() && i < result.channels.size(); ++i) {
        const auto& di = resources.display_info[i];
        // Always apply solidity from DisplayInfo
        result.channels[i].solidity = di.solidity;
        if (!result.channels[i].has_color) {
            result.channels[i].color_space = di.color_space;
            for (int j = 0; j < 4; ++j) {
                result.channels[i].color_components[j] = di.components[j];
            }
            result.channels[i].has_color = true;
        }
    }

    // Apply channel names from resource 1045
    for (size_t i = 0; i < resources.channel_names.size() && i < result.channels.size(); ++i) {
        result.channels[i].name = resources.channel_names[i];
    }

    // For CMYK documents without spot color metadata, assume standard CMYK channels
    if (header.color_mode == 4 && resources.spot_colors.empty()) {
        const char* cmyk_names[] = {"Cyan", "Magenta", "Yellow", "Black"};
        for (int i = 0; i < 4 && i < (int)header.channel_count; ++i) {
            result.channels[i].channel_id = i;
            result.channels[i].color_space = 2; // CMYK
            result.channels[i].has_color = true;
            if (result.channels[i].name.empty()) {
                result.channels[i].name = cmyk_names[i];
            }
        }
    }

    // Fill in remaining channel IDs
    for (size_t i = 0; i < result.channels.size(); ++i) {
        if (result.channels[i].channel_id == 0 && !result.channels[i].has_color) {
            result.channels[i].channel_id = static_cast<int16_t>(i);
        }
    }

    return result;
}
