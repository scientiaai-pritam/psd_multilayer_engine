# PSD Thumbnail Engine — Design Specification

**Date:** 2026-04-21
**Status:** Approved for implementation
**Target:** High-performance 512x512 thumbnail generator for multichannel PSD/PSB files with spot colors
**Formats supported:** PSD (version 1) and PSB/Large Document Format (version 2)

---

## Problem Statement

Generate pixel-accurate 512x512 RGB thumbnails from Adobe Photoshop PSD files that use multichannel setups and custom spot colors (Pantone, white underprints, spot varnishes). Must handle 100K+ files. Current Python approach (psd-tools) takes 1-2 minutes per file with inaccurate colors.

**Why existing tools fail:**
- No C/C++ PSD parser supports multichannel mode (color mode 7)
- psd_sdk: RGB/Grayscale only, dormant since 2023
- PhotoshopAPI: explicitly excludes multichannel and Lab modes
- psd-tools (Python): slow pure-Python color math, grayscale fallback for spot channels
- ImageMagick: slow, high memory, high failure rate on complex documents

**Approach:** Build a targeted C++ parser that reads only what's needed (header, image resources, merged image data), then convert N-channel pixel data to sRGB using lcms2.

---

## Architecture

### 4-Stage Pipeline

```
PSD File → [PSD Parser] → [Color Engine] → [Downscaler] → [Output]
              bytes →        N-ch →          any size →     PNG/JPEG
              structured      sRGB             512×512       file
              channel data    pixels
```

**Key principle:** The parser knows nothing about color. The color engine knows nothing about PSD. They communicate through a plain struct.

### Shared Data Types

```cpp
struct ParsedPSD {
    uint32_t width;              // uint32 for PSB (up to 300,000)
    uint32_t height;             // uint32 for PSB (up to 300,000)
    uint16_t channel_count;
    uint16_t bit_depth;           // 8, 16, or 32
    uint16_t color_mode;          // 3=RGB, 4=CMYK, 7=Multichannel, 9=Lab

    struct ChannelInfo {
        int16_t channel_id;       // 0,1,2... for process, -1,-2... for spot
        std::string name;
        uint16_t color_space;     // 0=RGB, 2=CMYK, 7=Lab
        int16_t color_components[4];
        bool has_color;
    };
    std::vector<ChannelInfo> channels;

    std::vector<std::vector<uint8_t>> pixel_data;  // one buffer per channel
    std::vector<uint8_t> icc_profile;               // raw ICC bytes from resource 1039
};

struct RGBImage {
    uint32_t width;
    uint32_t height;
    std::vector<uint8_t> pixels;  // RGB interleaved, row-major
};
```

---

## Stage 1: PSD Parser

Reads 3 of the 5 PSD sections. Skips Color Mode Data (empty for multichannel) and Layer/Mask Info (we use the merged composite).

### Read Sequence

```
Offset 0:   File Header (26 bytes, fixed)
            → width, height, channels, depth, color_mode
            → supported modes: 3 (RGB), 4 (CMYK), 7 (Multichannel), 9 (Lab)

Offset 26:  Color Mode Data
            → read 4-byte length, skip length bytes

Next:       Image Resources
            → read 4-byte section length
            → walk resource blocks, extract:
                Resource 1039 → ICC profile (raw bytes)
                Resource 1045 → channel names (Unicode strings)
                Resource 1067 → spot color Lab values + channel IDs
                Resource 1077 → DisplayInfo (fallback color data)
            → skip all other resources

Last:       Image Data Section
            → read 2-byte compression type
            → decompress each channel in planar order

Note: PSB files use 8-byte length fields instead of 4-byte for the
Layer/Mask Info and Image Data sections. The parser checks the version
field from the header to determine which size to use.
```

### Decompression

| Type | Code | Method |
|------|------|--------|
| Raw | 0 | Read directly |
| RLE (PackBits) | 1 | Read 2-byte byte counts per scanline, then PackBits decode |
| ZIP | 2 | zlib inflate |
| ZIP+prediction | 3 | zlib inflate + horizontal differencing reversal per row |

### Channel Color Metadata Resolution

1. Resource 1067 (Alternate Spot Colors) — primary, maps channel ID → Lab
2. Resource 1077 (DisplayInfo) — fallback
3. If color_mode=4 (CMYK) and no metadata: assume C,M,Y,K
4. No metadata: grayscale fallback

### Error Handling

- Corrupt header → fail fast with clear error
- Missing resource 1067/1077 → degrade to grayscale fallback for unknown channels
- Truncated pixel data → fail with partial info
- Unknown compression → fail with clear message

---

## Stage 2: Color Engine

### Channel Classification

```
color_mode == 3 (RGB):    channels 0,1,2 = R,G,B; 3+ = spot
color_mode == 4 (CMYK):   channels 0,1,2,3 = C,M,Y,K; 4+ = spot
color_mode == 7 (Multi):  all channels = inks, identified via resource 1067
color_mode == 9 (Lab):    channels 0,1,2 = L,a,b; 3+ = spot
```

### Per-Mode Pipeline

**RGB documents:** Composite 3 RGB bands directly. Overlay any spot channels via alpha-over.

**CMYK documents:** Convert CMYK→RGB using embedded ICC profile (or Fogra39 fallback) via lcms2. Overlay spot channels.

**Lab documents:** Convert L,a,b → XYZ (standard formula), then XYZ→sRGB via lcms2.

**Multichannel documents (primary target):**

```
For each pixel:
  1. Start with substrate white (D50 XYZ: 0.9642, 1.0, 0.8249)
  2. For each channel with color metadata:
     a. Normalize pixel value to 0.0-1.0 tint
     b. Get channel's Lab color (from resource 1067)
     c. Scale Lab by tint: Lab_tinted = Lab_solid * tint
     d. Convert Lab_tinted → XYZ
     e. Accumulate XYZ
  3. Channels without metadata: grayscale overlay (L=pixel_value, a=0, b=0)
  4. Convert final XYZ → sRGB via lcms2
  5. Clamp 0-255, apply sRGB gamma
```

### Special Channel Handling

**White underprint:** Detected by name "White"/"white" or Lab near (100,0,0). Blends substrate toward white before compositing other inks.

**Spot varnish:** Detected by name "Varnish"/"UV". Post-composite saturation boost proportional to value. Can be skipped for speed.

**Overprint:** Simplified additive XYZ compositing. No full Kubelka-Munk — unnecessary for thumbnails.

### lcms2 Usage

```cpp
// CMYK documents: embedded profile → sRGB
cmsHTRANSFORM cmyk_to_rgb = cmsCreateTransform(
    embedded_profile, TYPE_CMYK_8,
    srgb_profile,     TYPE_RGB_8,
    INTENT_RELATIVE_COLORIMETRIC, 0);

// Multichannel: manual XYZ compositing, then XYZ → sRGB via lcms2
cmsHTRANSFORM xyz_to_rgb = cmsCreateTransform(
    xyz_profile,  TYPE_XYZ_DBL,
    srgb_profile, TYPE_RGB_8,
    INTENT_RELATIVE_COLORIMETRIC, 0);
```

---

## Stage 3: Downscaler

Bilinear interpolation from source dimensions to target (default 512x512).

For each output pixel: map to source coordinates, sample 4 nearest source pixels with bilinear weights, write interpolated RGB.

**Why bilinear:** Clean results at 512x512. Faster than bicubic/Lanczos. Good enough for thumbnails.

---

## Stage 4: Output

### Phase 1
- PNG output via `stb_image_write.h`
- Single file output

### Phase 2 (future)
- JPEG output option
- Batch directory processing
- Configurable quality/compression

---

## CLI Interface

```
psdthumb [options] <input.psd>

Options:
  -o, --output <path>     Output file path (default: input name + .png)
  -s, --size <N>          Thumbnail size (default: 512)
  -f, --format <png|jpg>  Output format (default: png)
  -j, --jobs <N>          Parallel workers (default: 1)
  -v, --verbose           Print timing and channel info
  -q, --quality <1-100>   JPEG quality (default: 90)
  --batch <dir>           Process all PSD/PSB files in directory
```

---

## Dependencies

| Library | Purpose | License | Integration |
|---------|---------|---------|-------------|
| lcms2 | N-channel color management → RGB | MIT | Static link, C library |
| zlib | ZIP decompression for PSD image data | zlib license | Static link |
| stb_image_write.h | PNG/JPEG output | Public Domain | Single header include |

**Total: 3 external dependencies, all permissively licensed.**

---

## Project Structure

```
engine/
├── CMakeLists.txt
├── src/
│   ├── main.cpp              CLI entry point, argument parsing
│   ├── psd_parser.h/cpp      PSD binary parser
│   ├── color_engine.h/cpp    N-channel → RGB conversion
│   ├── downscaler.h/cpp      Bilinear downscale
│   ├── image_writer.h/cpp    PNG/JPEG output via stb
│   └── types.h               ParsedPSD, RGBImage, shared types
├── vendor/
│   ├── lcms2/
│   ├── zlib/
│   └── stb_image_write.h
└── tests/
    ├── test_parser.cpp
    ├── test_color.cpp
    └── test_downscale.cpp
```

---

## Performance Targets

| Metric | Target |
|--------|--------|
| Parse 10-channel 8-bit 5000x5000 PSD | < 200ms |
| Color convert 10-channel → RGB | < 100ms |
| Downscale to 512x512 | < 10ms |
| **Total per file** | **< 500ms** |
| 100K files (sequential) | ~14 hours |
| 100K files (8 workers) | ~2 hours |

---

## PSD Format Reference (from Adobe Spec)

### File Header (26 bytes)

| Offset | Size | Field |
|--------|------|-------|
| 0 | 4 | Signature: '8BPS' |
| 4 | 2 | Version: 1 (PSD) or 2 (PSB) |
| 6 | 6 | Reserved: zero |
| 12 | 2 | Channel count (1-56) |
| 14 | 4 | Height (1-30000, PSB: 300000) |
| 18 | 4 | Width (1-30000, PSB: 300000) |
| 20 | 2 | Depth: 1, 8, 16, or 32 |
| 22 | 2 | Color mode: 0=Bitmap, 1=Gray, 2=Indexed, 3=RGB, 4=CMYK, 7=Multichannel, 8=Duotone, 9=Lab |

### Resource 1067 — Alternate Spot Colors

```
2 bytes: version (=1)
2 bytes: channel count
For each channel:
  4 bytes: channel ID
  2 bytes: color space (7=Lab)
  8 bytes: 4 x 2-byte color components
```

### PSD Color Structure (10 bytes)

```
2 bytes: color space (0=RGB, 2=CMYK, 7=Lab)
8 bytes: 4 x 2-byte components

Lab encoding: L = 0...10000, a = -12800...12700, b = -12800...12700
CMYK encoding: 0 = 100% ink
```

### Image Data Compression

| Code | Method |
|------|--------|
| 0 | Raw |
| 1 | RLE (PackBits per scanline) |
| 2 | ZIP without prediction |
| 3 | ZIP with prediction |
