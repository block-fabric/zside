// Copyright (c) 2020-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/context.h>

namespace wallet {
WalletContext::WalletContext() = default;
WalletContext::~WalletContext()
{
    // Stopped by UnloadWallets normally; never left running past the context it uses.
    meltcast_interrupt();
    if (meltcast_thread.joinable()) meltcast_thread.join();
}
} // namespace wallet
