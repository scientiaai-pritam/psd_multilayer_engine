#pragma once
#include "types.h"
#include <string>

// Write an RGB image to a PNG file.
bool write_png(const std::string& path, const RGBImage& img);
