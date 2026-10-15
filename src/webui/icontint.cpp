// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/icontint.h>
#include <webui/chainconfig.h>

#include <algorithm>
#include <cmath>

namespace webui {

IconTint TintOf(const std::string& network)
{
    // The hue of the chain (chain.json, brand.iconHue: one icon for every chain of the family, told
    // apart by its colour), then that of the network.
    IconTint tint{};
    if (network == "test" || network == "testnet4") tint = {70, 30};
    if (network == "signet") tint = {35, 15};
    if (network == "regtest") tint = {160, 30};
    tint.hue_shift = ((tint.hue_shift + chain::ICON_HUE) % 360 + 360) % 360;
    return tint;
}

namespace {
/** RGB (0-255) to HSL with Qt's ranges (hue 0-359, -1 for a grey; saturation and lightness 0-255). */
void ToHsl(int r, int g, int b, int& h, int& s, int& l)
{
    const double rf{r / 255.0}, gf{g / 255.0}, bf{b / 255.0};
    const double max{std::max({rf, gf, bf})}, min{std::min({rf, gf, bf})};
    const double light{(max + min) / 2};
    const double delta{max - min};
    l = static_cast<int>(std::lround(light * 255));
    if (delta < 1e-9) {
        h = -1;
        s = 0;
        return;
    }
    const double sat{light <= 0.5 ? delta / (max + min) : delta / (2 - max - min)};
    double hue;
    if (max == rf) hue = (gf - bf) / delta + (gf < bf ? 6 : 0);
    else if (max == gf) hue = (bf - rf) / delta + 2;
    else hue = (rf - gf) / delta + 4;
    h = static_cast<int>(std::lround(hue * 60)) % 360;
    s = static_cast<int>(std::lround(sat * 255));
}

double HueToRgb(double p, double q, double t)
{
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 1.0 / 2) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

void FromHsl(int h, int s, int l, int& r, int& g, int& b)
{
    const double sf{s / 255.0}, lf{l / 255.0};
    if (h < 0 || sf <= 0) {
        r = g = b = static_cast<int>(std::lround(lf * 255));
        return;
    }
    const double hf{(h % 360) / 360.0};
    const double q{lf < 0.5 ? lf * (1 + sf) : lf + sf - lf * sf};
    const double p{2 * lf - q};
    r = static_cast<int>(std::lround(HueToRgb(p, q, hf + 1.0 / 3) * 255));
    g = static_cast<int>(std::lround(HueToRgb(p, q, hf) * 255));
    b = static_cast<int>(std::lround(HueToRgb(p, q, hf - 1.0 / 3) * 255));
}
} // namespace

void TintPixels(std::span<unsigned char> pixels, size_t channels, size_t width, size_t height, size_t rowstride, IconTint tint)
{
    if (tint.None() || channels < 3) return;
    for (size_t y{0}; y < height; ++y) {
        for (size_t x{0}; x < width; ++x) {
            const size_t at{y * rowstride + x * channels};
            if (at + 2 >= pixels.size()) return;
            int h, s, l;
            ToHsl(pixels[at], pixels[at + 1], pixels[at + 2], h, s, l);
            // As NetworkStyle: the colour turned on the circle, the saturation reduced (greys stay grey).
            if (h >= 0) h = (h + tint.hue_shift) % 360;
            if (s > tint.saturation_reduction) s -= tint.saturation_reduction;
            int r, g, b;
            FromHsl(h, s, l, r, g, b);
            pixels[at] = static_cast<unsigned char>(std::clamp(r, 0, 255));
            pixels[at + 1] = static_cast<unsigned char>(std::clamp(g, 0, 255));
            pixels[at + 2] = static_cast<unsigned char>(std::clamp(b, 0, 255));
        }
    }
}

} // namespace webui
