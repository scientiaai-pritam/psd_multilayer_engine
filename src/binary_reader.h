#pragma once
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>
#include <stdexcept>

// Reads big-endian binary data from a stream.
// PSD files are big-endian on all platforms.
class BinaryReader {
public:
    explicit BinaryReader(const std::string& path)
        : file_(path, std::ios::binary)
    {
        if (!file_) throw std::runtime_error("Cannot open file: " + path);
    }

    void read_bytes(void* buf, size_t count) {
        file_.read(reinterpret_cast<char*>(buf), count);
        if (!file_) throw std::runtime_error("Unexpected end of file");
    }

    uint8_t read_u8() {
        uint8_t v; read_bytes(&v, 1); return v;
    }

    uint16_t read_u16() {
        uint8_t b[2]; read_bytes(b, 2);
        return (uint16_t(b[0]) << 8) | b[1];
    }

    uint32_t read_u32() {
        uint8_t b[4]; read_bytes(b, 4);
        return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) |
               (uint32_t(b[2]) << 8)  | b[3];
    }

    int16_t read_i16() { return static_cast<int16_t>(read_u16()); }
    int32_t read_i32() { return static_cast<int32_t>(read_u32()); }

    std::string read_pascal_string() {
        uint8_t len = read_u8();
        std::string s(len, '\0');
        if (len > 0) read_bytes(s.data(), len);
        // Pascal strings are padded to even size (including the length byte)
        size_t total = 1 + len;
        if (total % 2 != 0) skip(1);
        return s;
    }

    std::string read_unicode_string() {
        uint32_t char_count = read_u32();
        std::string result;
        result.reserve(char_count);
        for (uint32_t i = 0; i < char_count; ++i) {
            uint16_t ch = read_u16();
            if (ch == 0) break;
            // Simple UTF-16 to UTF-8 for BMP characters
            if (ch < 0x80) {
                result += static_cast<char>(ch);
            } else if (ch < 0x800) {
                result += static_cast<char>(0xC0 | (ch >> 6));
                result += static_cast<char>(0x80 | (ch & 0x3F));
            } else {
                result += static_cast<char>(0xE0 | (ch >> 12));
                result += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
                result += static_cast<char>(0x80 | (ch & 0x3F));
            }
        }
        // Read the trailing null
        return result;
    }

    void skip(size_t count) {
        file_.seekg(count, std::ios::cur);
        if (!file_) throw std::runtime_error("Seek failed");
    }

    size_t position() {
        file_.clear();
        return static_cast<size_t>(file_.tellg());
    }

    void seek(size_t pos) {
        file_.clear();
        file_.seekg(pos);
        if (!file_) throw std::runtime_error("Seek failed");
    }

    bool eof() const {
        return file_.eof();
    }

    std::vector<uint8_t> read_vec(size_t count) {
        std::vector<uint8_t> v(count);
        read_bytes(v.data(), count);
        return v;
    }

private:
    std::ifstream file_;
};
