# psdthumb

High-performance C++ engine that generates pixel-accurate RGB thumbnails from Adobe Photoshop PSD files. Built for multichannel documents with custom spot colors (Pantone, white underprints, spot varnishes, textile separations).

## Features

- Parses PSD files in RGB, CMYK, Multichannel, and Lab color modes
- Reads spot color metadata from PSD resources (1067, 1077, 1045)
- ICC profile-aware color conversion via lcms2
- Subtractive ink compositing in XYZ space for multichannel documents
- Bilinear interpolation downscaling
- PNG output via stb_image_write

## Build

Requires CMake 3.20+, a C++17 compiler, and Git (for fetching dependencies).

```bash
cmake -B build -G "MSYS Makefiles"   # or "MinGW Makefiles", "Unix Makefiles"
cmake --build build --config Release
```

Dependencies (fetched automatically via FetchContent):
- [lcms2](https://github.com/mm2/Little-CMS) — color management
- [zlib](https://github.com/madler/zlib) — PSD ZIP decompression
- [stb](https://github.com/nothings/stb) — PNG encoding

## Usage

```bash
psdthumb [options] <input.psd>
```

Options:
| Flag | Description |
|------|-------------|
| `-o, --output <path>` | Output PNG path (default: replaces input extension with `.png`) |
| `-s, --size <N>` | Thumbnail max dimension in pixels (default: 512) |
| `-v, --verbose` | Print timing breakdown and channel info |

Examples:
```bash
psdthumb design.psd                        # -> design.png (512x512)
psdthumb -o thumb.png -s 256 design.psd    # -> thumb.png (256x256)
psdthumb -v -s 4724 design.psd             # full resolution, with timing
```

## Architecture

```
PSD file
  |
  v
[psd_parser] -- parses header, resources, image data
  |
  v
[color_engine] -- Lab->XYZ->sRGB conversion, multichannel compositing
  |
  v
[downscaler] -- bilinear interpolation to target size
  |
  v
[image_writer] -- PNG output
```

### Source files

| File | Purpose |
|------|---------|
| `src/types.h` | Shared data structures (`ParsedPSD`, `RGBImage`, `ChannelInfo`) |
| `src/binary_reader.h` | Big-endian binary reader for PSD format |
| `src/psd_parser.cpp` | PSD section parser (header, resources, image data, RLE/PackBits) |
| `src/color_engine.cpp` | Color conversion per mode (RGB, CMYK, Multichannel, Lab) |
| `src/downscaler.cpp` | Bilinear downscaling |
| `src/image_writer.cpp` | PNG write via stb_image_write |
| `src/main.cpp` | CLI entry point with argument parsing |

### Multichannel compositing

For multichannel (mode 7) documents, each channel represents a spot color ink with its Lab color from PSD metadata. The compositing uses a subtractive (multiply) model in XYZ space:

1. Start with D50 substrate white
2. For each ink, compute reflectance as `ink_XYZ / substrate_XYZ`
3. At coverage `a`, transmittance = `(1-a) + a * reflectance`
4. Inks combine by multiplying transmittances
5. Final XYZ is converted to sRGB via Bradford-adapted D50->D65 matrix

## License

Private project.
