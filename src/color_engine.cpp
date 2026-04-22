#include "color_engine.h"
#include <lcms2.h>
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <memory>

// --- Color Math Utilities ---

struct Lab { double L, a, b; };
struct XYZ { double X, Y, Z; };
struct LinearRGB { double r, g, b; };

static XYZ lab_to_xyz(Lab lab) {
    // D50 illuminant reference values
    constexpr double Xn = 0.9642;
    constexpr double Yn = 1.0000;
    constexpr double Zn = 0.8249;

    auto finv = [](double t) -> double {
        constexpr double delta = 6.0 / 29.0;
        if (t > delta)
            return t * t * t;
        return 3.0 * delta * delta * (t - 4.0 / 29.0);
    };

    double L_scaled = (lab.L + 16.0) / 116.0;
    return {
        Xn * finv(L_scaled + lab.a / 500.0),
        Yn * finv(L_scaled),
        Zn * finv(L_scaled - lab.b / 200.0)
    };
}

static LinearRGB xyz_to_linear_srgb(XYZ xyz) {
    // sRGB XYZ→linear RGB matrix (D50 adapted, since our Lab→XYZ uses D50)
    // Actually, we need to go from D50 XYZ to D65 linear sRGB.
    // Standard Bradford-adapted matrix:
    return {
         3.1338561 * xyz.X - 1.6168667 * xyz.Y - 0.4906146 * xyz.Z,
        -0.9787684 * xyz.X + 1.9161415 * xyz.Y + 0.0334540 * xyz.Z,
         0.0719453 * xyz.X - 0.2289914 * xyz.Y + 1.4052427 * xyz.Z
    };
}

static uint8_t srgb_gamma(double v) {
    if (v <= 0.0031308)
        v = 12.92 * v;
    else
        v = 1.055 * std::pow(v, 1.0 / 2.4) - 0.055;
    return static_cast<uint8_t>(std::clamp(v * 255.0, 0.0, 255.0));
}

static Lab psd_lab_to_float(int16_t L, int16_t a, int16_t b) {
    // PSD Lab encoding: L = 0...10000, a = -12800...12700, b = -12800...12700
    return {
        L / 100.0,      // 0..100
        a / 100.0,      // -128..127
        b / 100.0       // -128..127
    };
}

// --- lcms2 Helper ---

struct LcmsTransform {
    cmsHTRANSFORM handle = nullptr;
    explicit LcmsTransform(cmsHTRANSFORM h) : handle(h) {}
    ~LcmsTransform() { if (handle) cmsDeleteTransform(handle); }
    LcmsTransform(const LcmsTransform&) = delete;
    LcmsTransform& operator=(const LcmsTransform&) = delete;
};

static std::unique_ptr<LcmsTransform> make_cmyk_to_rgb_transform(const uint8_t* icc_data, size_t icc_len) {
    cmsHPROFILE hInput = nullptr;

    if (icc_data && icc_len > 0) {
        hInput = cmsOpenProfileFromMem(icc_data, icc_len);
    }

    if (!hInput) {
        // No embedded ICC profile — build a fallback CMYK profile from first principles.
        // Uses a simple Neugebauer-like model with standard SWOP ink colors
        // converted through Lab→XYZ→sRGB.
        hInput = cmsCreateProfilePlaceholder(NULL);
        cmsSetProfileVersion(hInput, 4.3);
        cmsSetDeviceClass(hInput, cmsSigOutputClass);
        cmsSetColorSpace(hInput, cmsSigCmykData);
        cmsSetPCS(hInput, cmsSigLabData);

        // Build AToB0 pipeline: CMYK → Lab using a 9x9x9x9 CLUT
        cmsPipeline* pipe = cmsPipelineAlloc(NULL, 4, 3);

        uint32_t dims[4] = {9, 9, 9, 9};
        cmsStage* clut = cmsStageAllocCLut16bitGranular(NULL, dims, 3, 0, NULL);

        // Fill CLUT: map CMYK grid points to Lab
        cmsUInt16Number* clutData = (cmsUInt16Number*)cmsStageData(clut);
        size_t total_entries = 1;
        for (int d = 0; d < 4; ++d) total_entries *= dims[d];

        // Standard SWOP process ink colors (approximate Lab values at 100%)
        // Cyan:    Lab(55, -37, -50)
        // Magenta: Lab(48, 74, -3)
        // Yellow:  Lab(89, -5, 94)
        // Black:   Lab(0, 0, 0)
        const double ink_Lab[5][3] = {
            {55.0, -37.0, -50.0},  // Cyan
            {48.0,  74.0,  -3.0},  // Magenta
            {89.0,  -5.0,  94.0},  // Yellow
            { 0.0,   0.0,   0.0},  // Black
            {100.0,  0.0,   0.0},  // Paper white
        };

        size_t idx = 0;
        for (size_t ci = 0; ci < dims[0]; ++ci) {
            for (size_t mi = 0; mi < dims[1]; ++mi) {
                for (size_t yi = 0; yi < dims[2]; ++yi) {
                    for (size_t ki = 0; ki < dims[3]; ++ki) {
                        double c = ci / (double)(dims[0] - 1);
                        double m = mi / (double)(dims[1] - 1);
                        double y_ = yi / (double)(dims[2] - 1);
                        double k = ki / (double)(dims[3] - 1);

                        // Simple subtractive mixing: start with paper white,
                        // subtract each ink's absorption contribution
                        // Result in Lab, then convert to 16-bit for CLUT
                        double L = ink_Lab[4][0];  // paper white
                        double a = ink_Lab[4][1];
                        double b = ink_Lab[4][2];

                        // Each ink reduces the reflected light
                        // Using simplified Murray-Davies: paper * (1-ink)
                        // For each ink, blend toward that ink's color by coverage
                        double remain = 1.0;

                        auto apply_ink = [&](double coverage, const double ink_lab[3]) {
                            // Ink darkens and shifts color proportionally
                            L = L * (1.0 - coverage) + ink_lab[0] * coverage * 0.6 + L * coverage * 0.4;
                            a = a * (1.0 - coverage) + ink_lab[1] * coverage;
                            b = b * (1.0 - coverage) + ink_lab[2] * coverage;
                        };

                        apply_ink(k, ink_Lab[3]);  // black first
                        apply_ink(c, ink_Lab[0]);
                        apply_ink(m, ink_Lab[1]);
                        apply_ink(y_, ink_Lab[2]);

                        // Encode as 16-bit Lab for CLUT
                        // Lab in CLUT: L = 0..65280 (0..255*256), a/b = 0..65280 offset at 32768
                        clutData[idx++] = (uint16_t)std::clamp(L / 100.0 * 65535.0, 0.0, 65535.0);
                        clutData[idx++] = (uint16_t)std::clamp((128.0 + a) / 255.0 * 65535.0, 0.0, 65535.0);
                        clutData[idx++] = (uint16_t)std::clamp((128.0 + b) / 255.0 * 65535.0, 0.0, 65535.0);
                    }
                }
            }
        }

        cmsPipelineInsertStage(pipe, cmsAT_END, clut);
        cmsWriteTag(hInput, cmsSigAToB0Tag, pipe);
        cmsPipelineFree(pipe);

        fprintf(stderr, "Warning: No embedded ICC profile, using built-in SWOP fallback\n");
    }

    cmsHPROFILE hOutput = cmsCreate_sRGBProfile();

    cmsHTRANSFORM xform = cmsCreateTransform(
        hInput, TYPE_CMYK_8,
        hOutput, TYPE_RGB_8,
        INTENT_RELATIVE_COLORIMETRIC, 0
    );

    cmsCloseProfile(hInput);
    cmsCloseProfile(hOutput);

    if (!xform) return nullptr;
    return std::make_unique<LcmsTransform>(xform);
}

// --- Per-Mode Conversion ---

static RGBImage convert_rgb(const ParsedPSD& psd) {
    RGBImage img;
    img.width = psd.width;
    img.height = psd.height;
    img.pixels.resize((size_t)psd.width * psd.height * 3);

    // Direct RGB — channels 0,1,2 = R,G,B
    const uint8_t* r = psd.pixel_data[0].data();
    const uint8_t* g = psd.pixel_data[1].data();
    const uint8_t* b = psd.pixel_data[2].data();

    size_t pixel_count = (size_t)psd.width * psd.height;
    for (size_t i = 0; i < pixel_count; ++i) {
        img.pixels[i * 3 + 0] = r[i];
        img.pixels[i * 3 + 1] = g[i];
        img.pixels[i * 3 + 2] = b[i];
    }

    // If there are extra spot channels (channels > 2), ignore them for RGB mode
    // Spot channels in RGB mode are rare and complex to composite correctly

    return img;
}

static RGBImage convert_cmyk(const ParsedPSD& psd) {
    RGBImage img;
    img.width = psd.width;
    img.height = psd.height;
    img.pixels.resize((size_t)psd.width * psd.height * 3);

    auto xform = make_cmyk_to_rgb_transform(
        psd.icc_profile.empty() ? nullptr : psd.icc_profile.data(),
        psd.icc_profile.size()
    );

    if (!xform || !xform->handle) {
        // Last resort: subtractive CMYK→RGB with undercolor removal
        size_t pixel_count = (size_t)psd.width * psd.height;
        const uint8_t* c = psd.pixel_data[0].data();
        const uint8_t* m = psd.pixel_data[1].data();
        const uint8_t* y = psd.pixel_data[2].data();
        const uint8_t* k = psd.pixel_data[3].data();
        for (size_t i = 0; i < pixel_count; ++i) {
            double cf = c[i] / 255.0, mf = m[i] / 255.0, yf = y[i] / 255.0, kf = k[i] / 255.0;
            img.pixels[i * 3 + 0] = srgb_gamma(std::clamp((1.0 - cf) * (1.0 - kf), 0.0, 1.0));
            img.pixels[i * 3 + 1] = srgb_gamma(std::clamp((1.0 - mf) * (1.0 - kf), 0.0, 1.0));
            img.pixels[i * 3 + 2] = srgb_gamma(std::clamp((1.0 - yf) * (1.0 - kf), 0.0, 1.0));
        }
        return img;
    }

    // Use lcms2 for CMYK→RGB
    size_t pixel_count = (size_t)psd.width * psd.height;

    // Build interleaved CMYK buffer
    std::vector<uint8_t> cmyk_buf(pixel_count * 4);
    const uint8_t* c = psd.pixel_data[0].data();
    const uint8_t* m = psd.pixel_data[1].data();
    const uint8_t* y = psd.pixel_data[2].data();
    const uint8_t* k = psd.pixel_data[3].data();
    for (size_t i = 0; i < pixel_count; ++i) {
        cmyk_buf[i * 4 + 0] = c[i];
        cmyk_buf[i * 4 + 1] = m[i];
        cmyk_buf[i * 4 + 2] = y[i];
        cmyk_buf[i * 4 + 3] = k[i];
    }

    cmsDoTransform(xform->handle, cmyk_buf.data(), img.pixels.data(), pixel_count);

    return img;
}

static RGBImage convert_lab(const ParsedPSD& psd) {
    RGBImage img;
    img.width = psd.width;
    img.height = psd.height;
    img.pixels.resize((size_t)psd.width * psd.height * 3);

    size_t pixel_count = (size_t)psd.width * psd.height;
    const uint8_t* L_data = psd.pixel_data[0].data();
    const uint8_t* a_data = psd.pixel_data[1].data();
    const uint8_t* b_data = psd.pixel_data[2].data();

    for (size_t i = 0; i < pixel_count; ++i) {
        // PSD stores Lab as 8-bit where L=0..255 maps to 0..100
        // and a,b = 0..255 maps to -128..127
        double L = (L_data[i] / 255.0) * 100.0;
        double a = ((a_data[i] / 255.0) * 255.0) - 128.0;
        double b = ((b_data[i] / 255.0) * 255.0) - 128.0;

        XYZ xyz = lab_to_xyz({L, a, b});
        LinearRGB rgb = xyz_to_linear_srgb(xyz);

        img.pixels[i * 3 + 0] = srgb_gamma(rgb.r);
        img.pixels[i * 3 + 1] = srgb_gamma(rgb.g);
        img.pixels[i * 3 + 2] = srgb_gamma(rgb.b);
    }

    return img;
}

// --- Multichannel Ink Info (pre-computed per channel) ---

struct InkInfo {
    XYZ ink_xyz;       // pre-computed solid ink color in XYZ (from Lab)
    bool has_data;
};

static RGBImage convert_multichannel(const ParsedPSD& psd) {
    RGBImage img;
    img.width = psd.width;
    img.height = psd.height;
    img.pixels.resize((size_t)psd.width * psd.height * 3);

    size_t pixel_count = (size_t)psd.width * psd.height;

    // D50 paper white substrate
    constexpr double paper_X = 0.9642;
    constexpr double paper_Y = 1.0000;
    constexpr double paper_Z = 0.8249;

    // Ink strength: >1.0 applies ink more aggressively (darker), <1.0 lighter.
    // Original alpha-blend at 1.0 was slightly too light vs Photoshop.
    constexpr double ink_strength = 1.3;

    // Pre-compute ink XYZ values outside pixel loop
    std::vector<InkInfo> inks(psd.channels.size());
    for (size_t ch = 0; ch < psd.channels.size(); ++ch) {
        inks[ch].has_data = false;
        const auto& info = psd.channels[ch];
        if (info.has_color && info.color_space == 7) {
            Lab solid = psd_lab_to_float(info.color_components[0],
                                          info.color_components[1],
                                          info.color_components[2]);
            inks[ch].ink_xyz = lab_to_xyz(solid);
            inks[ch].has_data = true;
        }
    }

    // Detect white underprint by NAME only.
    // Lab-based detection causes false positives (e.g. Zari = Lab(100,0,0) is gold, not white).
    int white_channel = -1;
    for (size_t ch = 0; ch < psd.channels.size(); ++ch) {
        const auto& name = psd.channels[ch].name;
        if (name.find("White") != std::string::npos ||
            name.find("white") != std::string::npos) {
            white_channel = static_cast<int>(ch);
            break;
        }
    }

    // Composite all channels in XYZ space
    for (size_t i = 0; i < pixel_count; ++i) {
        double X = paper_X, Y = paper_Y, Z = paper_Z;

        // Apply white underprint first
        if (white_channel >= 0 && white_channel < (int)psd.pixel_data.size()) {
            double white_tint = psd.pixel_data[white_channel][i] / 255.0;
            X = X * (1.0 - white_tint) + paper_X * white_tint;
            Y = Y * (1.0 - white_tint) + paper_Y * white_tint;
            Z = Z * (1.0 - white_tint) + paper_Z * white_tint;
        }

        // Composite each ink channel
        for (size_t ch = 0; ch < psd.channels.size(); ++ch) {
            if (static_cast<int>(ch) == white_channel) continue;

            // Skip channels with 0% solidity only if they have no pixel data
            // 0% solidity in PSD means "use ink at full strength" for display purposes

            // Invert: 0=full ink, 255=no ink
            double tint = 1.0 - (psd.pixel_data[ch][i] / 255.0);
            if (tint < 0.001) continue;

            if (inks[ch].has_data) {
                // Alpha-blend in XYZ with adjustable strength
                double t = std::min(tint * ink_strength, 1.0);
                X = X * (1.0 - t) + inks[ch].ink_xyz.X * t;
                Y = Y * (1.0 - t) + inks[ch].ink_xyz.Y * t;
                Z = Z * (1.0 - t) + inks[ch].ink_xyz.Z * t;
            } else {
                // No color metadata — grayscale fallback
                double gray = tint;
                X = X * (1.0 - gray * 0.5);
                Y = Y * (1.0 - gray * 0.5);
                Z = Z * (1.0 - gray * 0.5);
            }
        }

        // Convert XYZ to sRGB
        LinearRGB rgb = xyz_to_linear_srgb({X, Y, Z});
        img.pixels[i * 3 + 0] = srgb_gamma(rgb.r);
        img.pixels[i * 3 + 1] = srgb_gamma(rgb.g);
        img.pixels[i * 3 + 2] = srgb_gamma(rgb.b);
    }

    return img;
}

// --- Main Entry Point ---

RGBImage convert_to_rgb(const ParsedPSD& psd) {
    if (psd.pixel_data.empty()) return {};

    switch (psd.color_mode) {
        case 3: return convert_rgb(psd);
        case 4: return convert_cmyk(psd);
        case 7: return convert_multichannel(psd);
        case 9: return convert_lab(psd);
        default:
            fprintf(stderr, "Unsupported color mode: %d\n", psd.color_mode);
            return {};
    }
}
