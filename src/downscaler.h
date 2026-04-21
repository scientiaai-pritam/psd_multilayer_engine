#pragma once
#include "types.h"

// Downscale an RGB image to target dimensions using bilinear interpolation.
RGBImage downscale(const RGBImage& src, uint32_t target_width, uint32_t target_height);
