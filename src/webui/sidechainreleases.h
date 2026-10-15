// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_SIDECHAINRELEASES_H
#define BITCOIN_WEBUI_SIDECHAINRELEASES_H

namespace webui {

/**
 * The releases of the sidechains that this app offers to download (Sidechain Nodes: Download), with the
 * SHA-256 of each archive, built into the program so that neither the page nor a web server chooses what
 * is installed. Fill it when the sidechains are packaged (dist/package.sh writes SHA256SUMS): one line per
 * sidechain and platform, an https address, the hash from SHA256SUMS. A sidechain without a line here can
 * still be downloaded from an address and a hash the user types, after the same confirmation of the desktop.
 *
 * Empty for now: the last published packages (20261005.3) predate the rules the testnet runs.
 */
struct SidechainReleaseEntry {
    const char* stem;
    const char* platform;
    const char* version;
    const char* url;
    const char* sha256;
};

inline constexpr SidechainReleaseEntry SIDECHAIN_RELEASES[] = {
    // {"thunder", "x86_64-linux-gnu", "2026xxxx", "https://blockfab.org/drivechains/2026xxxx/thunder-2026xxxx-x86_64-linux-gnu.tar.gz", "<sha256>"},
    {nullptr, nullptr, nullptr, nullptr, nullptr},
};

} // namespace webui

#endif // BITCOIN_WEBUI_SIDECHAINRELEASES_H
