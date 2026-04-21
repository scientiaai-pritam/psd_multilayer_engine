#include "image_writer.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

bool write_png(const std::string& path, const RGBImage& img) {
    int result = stbi_write_png(
        path.c_str(),
        static_cast<int>(img.width),
        static_cast<int>(img.height),
        3,  // RGB channels
        img.pixels.data(),
        static_cast<int>(img.width * 3)  // stride
    );
    return result != 0;
}
