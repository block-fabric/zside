// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_ICONTINT_H
#define BITCOIN_WEBUI_ICONTINT_H

#include <cstddef>
#include <span>
#include <string>

namespace webui {

/** How the icon of the app is tinted on a network (NetworkStyle of chains-qt): hue shift, saturation reduction. */
struct IconTint {
    int hue_shift{0};
    int saturation_reduction{0};
    bool None() const { return hue_shift == 0 && saturation_reduction == 0; }
};

/** The tint of a network: "main" none, "test" and "testnet4" 70/30, "signet" 35/15, "regtest" 160/30; the hue of the chain (chain::ICON_HUE) added. */
IconTint TintOf(const std::string& network);

/**
 * Tint pixels of 8-bit RGBA (or RGB with `channels` 3), as NetworkStyle does: the hue turned, the
 * saturation reduced, the lightness and the alpha kept (HSL with Qt's ranges: hue 0-359, the rest 0-255).
 */
void TintPixels(std::span<unsigned char> pixels, size_t channels, size_t width, size_t height, size_t rowstride, IconTint tint);

} // namespace webui

#endif // BITCOIN_WEBUI_ICONTINT_H
