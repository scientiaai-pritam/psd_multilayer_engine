#include "psd_parser.h"
#include "color_engine.h"
#include "downscaler.h"
#include "image_writer.h"
#include <cstdio>
#include <string>
#include <cmath>
#include <algorithm>
#include <fstream>

struct Lab { double L, a, b; };
struct XYZ { double X, Y, Z; };
struct LinearRGB { double r, g, b; };

static XYZ lab_to_xyz(Lab lab) {
    constexpr double Xn = 0.9642, Yn = 1.0, Zn = 0.8249;
    auto finv = [](double t) -> double {
        constexpr double delta = 6.0 / 29.0;
        if (t > delta) return t * t * t;
        return 3.0 * delta * delta * (t - 4.0 / 29.0);
    };
    double L_scaled = (lab.L + 16.0) / 116.0;
    return { Xn * finv(L_scaled + lab.a / 500.0),
             Yn * finv(L_scaled),
             Zn * finv(L_scaled - lab.b / 200.0) };
}

static LinearRGB xyz_to_linear_srgb(XYZ xyz) {
    return {
         3.1338561 * xyz.X - 1.6168667 * xyz.Y - 0.4906146 * xyz.Z,
        -0.9787684 * xyz.X + 1.9161415 * xyz.Y + 0.0334540 * xyz.Z,
         0.0719453 * xyz.X - 0.2289914 * xyz.Y + 1.4052427 * xyz.Z
    };
}

static uint8_t srgb_gamma(double v) {
    if (v <= 0.0031308) v = 12.92 * v;
    else v = 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::clamp(v * 255.0, 0.0, 255.0));
}

static Lab psd_lab_to_float(int16_t L, int16_t a, int16_t b) {
    return { L / 100.0, a / 100.0, b / 100.0 };
}

int main(int argc, char* argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <file.psd>\n", argv[0]); return 1; }

    ParsedPSD psd;
    try { psd = parse_psd(argv[1]); }
    catch (const std::exception& e) { fprintf(stderr, "Error: %s\n", e.what()); return 1; }

    printf("Image: %ux%u, %u channels, mode=%u\n\n", psd.width, psd.height, psd.channel_count, psd.color_mode);

    // Dump channel info with Lab → sRGB conversion
    for (size_t ch = 0; ch < psd.channels.size(); ++ch) {
        const auto& info = psd.channels[ch];
        const auto& data = psd.pixel_data[ch];

        size_t nonzero = 0;
        double sum = 0;
        uint8_t max_val = 0;
        for (uint8_t v : data) {
            if (v > 0) nonzero++;
            sum += v;
            if (v > max_val) max_val = v;
        }
        double coverage = nonzero * 100.0 / data.size();
        double avg = sum / data.size();

        printf("Ch %2zu: id=%-3d name='%-20s' cov=%.1f%% avg=%.1f max=%3d",
               ch, info.channel_id, info.name.c_str(), coverage, avg, max_val);

        if (info.has_color && info.color_space == 7) {
            Lab lab = psd_lab_to_float(info.color_components[0],
                                        info.color_components[1],
                                        info.color_components[2]);
            XYZ xyz = lab_to_xyz(lab);
            LinearRGB rgb = xyz_to_linear_srgb(xyz);
            printf(" Lab(%.1f,%.1f,%.1f)->RGB(%d,%d,%d)",
                   lab.L, lab.a, lab.b,
                   srgb_gamma(rgb.r), srgb_gamma(rgb.g), srgb_gamma(rgb.b));
        }
        printf("\n");

        // Write individual channel render (spot color + coverage as brightness)
        if (info.has_color && info.color_space == 7 && coverage > 0.1) {
            Lab lab = psd_lab_to_float(info.color_components[0],
                                        info.color_components[1],
                                        info.color_components[2]);
            XYZ xyz = lab_to_xyz(lab);
            LinearRGB ink_rgb = xyz_to_linear_srgb(xyz);

            RGBImage img;
            img.width = psd.width;
            img.height = psd.height;
            img.pixels.resize((size_t)psd.width * psd.height * 3);

            size_t pixels = (size_t)psd.width * psd.height;
            for (size_t i = 0; i < pixels; ++i) {
                double t = data[i] / 255.0;
                // Show ink color scaled by coverage on white background
                double r = ink_rgb.r * t + 1.0 * (1.0 - t);
                double g = ink_rgb.g * t + 1.0 * (1.0 - t);
                double b = ink_rgb.b * t + 1.0 * (1.0 - t);
                img.pixels[i * 3 + 0] = srgb_gamma(r);
                img.pixels[i * 3 + 1] = srgb_gamma(g);
                img.pixels[i * 3 + 2] = srgb_gamma(b);
            }

            // Downscale to 512
            RGBImage thumb = downscale(img, 512, 512);
            char filename[256];
            snprintf(filename, sizeof(filename), "psd/debug_ch%02zu_%s.png", ch, info.name.c_str());
            write_png(filename, thumb);
            printf("  -> Wrote %s\n", filename);
        }
    }

    return 0;
}
