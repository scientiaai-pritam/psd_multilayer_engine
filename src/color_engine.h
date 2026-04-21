#pragma once
#include "types.h"

// Convert a parsed PSD (N-channel) to an RGB image.
// Handles all color modes: RGB, CMYK, Multichannel, Lab.
RGBImage convert_to_rgb(const ParsedPSD& psd);
