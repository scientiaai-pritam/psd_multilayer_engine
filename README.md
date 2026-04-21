# psdthumb

High-performance C++ engine that generates RGB thumbnails from Adobe Photoshop PSD files. Handles multichannel documents with custom spot colors (Pantone, white underprints, spot varnishes, textile separations).

## Features

- Parses PSD files in RGB, CMYK, Multichannel, and Lab color modes
- Reads spot color metadata from PSD resources (1039, 1045, 1067, 1077)
- ICC profile-aware CMYK conversion via lcms2
- Multichannel compositing: Lab→XYZ→sRGB with alpha-blend in XYZ space
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

| Flag | Description |
|------|-------------|
| `-o, --output <path>` | Output PNG path (default: replaces input extension with `.png`) |
| `-s, --size <N>` | Thumbnail max dimension in pixels (default: 512) |
| `-v, --verbose` | Print timing breakdown and channel info |

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
[psd_parser] -- header, resources (ICC, spot colors, channel names), image data
  |
  v
[color_engine] -- per-mode conversion (RGB, CMYK, Multichannel, Lab)
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
| `src/psd_parser.cpp` | PSD section parser — header, resources, image data (Raw/RLE/ZIP) |
| `src/color_engine.cpp` | Color conversion per mode with Lab→XYZ→sRGB pipeline |
| `src/downscaler.cpp` | Bilinear downscaling |
| `src/image_writer.cpp` | PNG write via stb_image_write |
| `src/main.cpp` | CLI entry point with argument parsing |

### Multichannel compositing

For multichannel (mode 7) documents, each channel represents a spot color ink:

1. Spot color metadata extracted from PSD resources (1067 Alternate Spot Colors, 1077 DisplayInfo, 1045 Unicode Alpha Names)
2. Each ink's Lab color is converted to XYZ via D50 illuminant
3. Pixel values are inverted (0 = full ink coverage, 255 = no ink)
4. Channels are composited using alpha-blend in XYZ space onto D50 substrate white
5. Final XYZ is converted to sRGB via Bradford-adapted D50→D65 matrix

### Supported PSD resources

| Resource ID | Name | Usage |
|-------------|------|-------|
| 1039 | ICC Profile | CMYK color management via lcms2 |
| 1045 | Unicode Alpha Names | Channel name labels |
| 1067 | Alternate Spot Colors | Ink Lab colors + channel IDs |
| 1077 | DisplayInfo | Fallback ink colors for channels missing from 1067 |

## Tested files

| File | Dimensions | Channels | Mode | Timing |
|------|-----------|----------|------|--------|
| `1197~OK~7368.psd` | 3149x6298 | 4ch | RGB | ~440ms |
| `1273-...7769.psd` | 4724x7560 | 16ch | Multichannel | ~30s |
| `3404-...7834.psd` | 4724x3780 | 12ch | Multichannel | ~12s |
| `5041-...7768.psd` | 4724x3780 | 15ch | Multichannel | ~14s |
| `5054-...7770.psd` | 4724x3780 | 13ch | Multichannel | ~13s |
| `psdd_03_06_2022_90.psd` | 3000x2000 | 3ch | RGB | ~130ms |

RGB files are fast. Multichannel color conversion is the bottleneck (per-pixel Lab→XYZ for each channel).

## License

Private project.
