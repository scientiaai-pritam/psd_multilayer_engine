#include "downscaler.h"
#include <algorithm>
#include <cmath>

RGBImage downscale(const RGBImage& src, uint32_t target_width, uint32_t target_height) {
    if (src.width == 0 || src.height == 0 || src.pixels.empty()) return {};

    // If already at target size, return a copy
    if (src.width == target_width && src.height == target_height) {
        return src;
    }

    RGBImage result;
    result.width = target_width;
    result.height = target_height;
    result.pixels.resize((size_t)target_width * target_height * 3);

    double x_ratio = static_cast<double>(src.width) / target_width;
    double y_ratio = static_cast<double>(src.height) / target_height;

    for (uint32_t dy = 0; dy < target_height; ++dy) {
        for (uint32_t dx = 0; dx < target_width; ++dx) {
            // Map destination pixel to source coordinates
            double sx = (dx + 0.5) * x_ratio - 0.5;
            double sy = (dy + 0.5) * y_ratio - 0.5;

            // Clamp to source bounds
            int x0 = std::max(0, static_cast<int>(std::floor(sx)));
            int y0 = std::max(0, static_cast<int>(std::floor(sy)));
            int x1 = std::min(static_cast<int>(src.width) - 1, x0 + 1);
            int y1 = std::min(static_cast<int>(src.height) - 1, y0 + 1);

            // Bilinear weights
            double xf = sx - x0;
            double yf = sy - y0;
            xf = std::clamp(xf, 0.0, 1.0);
            yf = std::clamp(yf, 0.0, 1.0);

            // Sample 4 source pixels
            auto sample = [&](int x, int y) -> const uint8_t* {
                return &src.pixels[(y * src.width + x) * 3];
            };

            const uint8_t* p00 = sample(x0, y0);
            const uint8_t* p10 = sample(x1, y0);
            const uint8_t* p01 = sample(x0, y1);
            const uint8_t* p11 = sample(x1, y1);

            double w00 = (1.0 - xf) * (1.0 - yf);
            double w10 = xf * (1.0 - yf);
            double w01 = (1.0 - xf) * yf;
            double w11 = xf * yf;

            size_t out_idx = (dy * target_width + dx) * 3;
            for (int c = 0; c < 3; ++c) {
                result.pixels[out_idx + c] = static_cast<uint8_t>(std::clamp(
                    p00[c] * w00 + p10[c] * w10 + p01[c] * w01 + p11[c] * w11,
                    0.0, 255.0
                ));
            }
        }
    }

    return result;
}
