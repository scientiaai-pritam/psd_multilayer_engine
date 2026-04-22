#include "psd_parser.h"
#include "color_engine.h"
#include "downscaler.h"
#include "image_writer.h"
#include <cstdio>
#include <string>
#include <chrono>

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s [options] <input.psd>\n", prog);
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o, --output <path>   Output file path (default: input.png)\n");
    fprintf(stderr, "  -s, --size <N>        Thumbnail size (default: 512)\n");
    fprintf(stderr, "  -f, --fullsize        Output at native PSD resolution (no downscale)\n");
    fprintf(stderr, "  -v, --verbose         Print timing and channel info\n");
}

int main(int argc, char* argv[]) {
    std::string input_path;
    std::string output_path;
    uint32_t thumb_size = 512;
    bool verbose = false;
    bool fullsize = false;

    // Simple argument parsing
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "-o" || arg == "--output") && i + 1 < argc) {
            output_path = argv[++i];
        } else if ((arg == "-s" || arg == "--size") && i + 1 < argc) {
            thumb_size = static_cast<uint32_t>(std::stoi(argv[++i]));
        } else if (arg == "-f" || arg == "--fullsize") {
            fullsize = true;
        } else if (arg == "-v" || arg == "--verbose") {
            verbose = true;
        } else if (arg[0] != '-') {
            input_path = arg;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }

    if (input_path.empty()) {
        print_usage(argv[0]);
        return 1;
    }

    if (output_path.empty()) {
        // Default: replace extension with .png
        output_path = input_path;
        auto dot = output_path.rfind('.');
        if (dot != std::string::npos) output_path.erase(dot);
        output_path += ".png";
    }

    auto t0 = std::chrono::high_resolution_clock::now();

    // Stage 1: Parse PSD
    ParsedPSD psd;
    try {
        psd = parse_psd(input_path);
    } catch (const PSDError& e) {
        fprintf(stderr, "Parse error: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    auto t1 = std::chrono::high_resolution_clock::now();

    if (verbose) {
        printf("PSD: %ux%u, %u channels, depth=%u, mode=%u\n",
               psd.width, psd.height, psd.channel_count, psd.bit_depth, psd.color_mode);
        printf("ICC profile: %zu bytes\n", psd.icc_profile.size());
        for (size_t i = 0; i < psd.channels.size(); ++i) {
            const auto& ch = psd.channels[i];
            printf("  Channel %zd: id=%d name='%s' colorspace=%u has_color=%d solidity=%u%%\n",
                   i, ch.channel_id, ch.name.c_str(), ch.color_space, ch.has_color, ch.solidity);
        }
    }

    // Stage 2: Color convert to RGB
    RGBImage rgb = convert_to_rgb(psd);
    auto t2 = std::chrono::high_resolution_clock::now();

    // Stage 3: Downscale (or keep full size)
    RGBImage thumb;
    if (fullsize) {
        thumb = std::move(rgb);
    } else {
        thumb = downscale(rgb, thumb_size, thumb_size);
    }
    auto t3 = std::chrono::high_resolution_clock::now();

    // Stage 4: Write PNG
    if (!write_png(output_path, thumb)) {
        fprintf(stderr, "Failed to write: %s\n", output_path.c_str());
        return 1;
    }
    auto t4 = std::chrono::high_resolution_clock::now();

    if (verbose) {
        double parse_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double color_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
        double scale_ms = std::chrono::duration<double, std::milli>(t3 - t2).count();
        double write_ms = std::chrono::duration<double, std::milli>(t4 - t3).count();
        double total_ms = std::chrono::duration<double, std::milli>(t4 - t0).count();
        printf("Timing: parse=%.1fms color=%.1fms scale=%.1fms write=%.1fms total=%.1fms\n",
               parse_ms, color_ms, scale_ms, write_ms, total_ms);
    }

    printf("Written: %s (%ux%u)\n", output_path.c_str(), thumb.width, thumb.height);
    return 0;
}
