// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/sidechain.h>
#include <kernel/chain.h>
#include <kernel/types.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/fuzz/util/wallet.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <test/util/time.h>
#include <txmempool.h>
#include <wallet/wallet.h>

#include <limits>
#include <vector>

namespace wallet {
namespace {
const TestingSetup* g_setup;

void initialize_setup()
{
    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>();
    g_setup = testing_setup.get();
}

//! The removals from the mempool of chains of drivechain transactions of the wallet's (BMM requests
//! paid from the change of the one before, deposits spending the treasury output of the one before),
//! notified in any order, with abandons asked for and blocks connected in between: in the end every
//! one is abandoned, and the coins they spent are back. Nothing asserts on the way.
FUZZ_TARGET(wallet_drivechain_notifications, .init = initialize_setup)
{
    SeedRandomStateForTest(SeedRand::ZEROS);
    FuzzedDataProvider fuzzed_data_provider{buffer.data(), buffer.size()};
    FakeNodeClock clock{ConsumeTime(fuzzed_data_provider)};
    interfaces::Chain& chain{*g_setup->m_node.chain};
    FuzzedWallet fuzzed_wallet{
        chain,
        "fuzzed_wallet_a",
        "tprv8ZgxMBicQKsPd1QwsGgzfu2pcPYbBosZhJknqreRHgsWx32nNEhMjGQX2cgFL8n6wz9xdDYwLcs78N4nsCo32cxEX8RBtwGsEGgybLiQJfk",
    };
    CWallet& wallet{*fuzzed_wallet.wallet};
    const int tip_height{*Assert(chain.getHeight())};
    const uint256 tip_hash{chain.getBlockHash(tip_height)};

    struct Removal {
        CTransactionRef tx;
        MemPoolRemovalReason reason;
    };
    std::vector<Removal> removals;
    std::vector<COutPoint> coins;
    LIMITED_WHILE(fuzzed_data_provider.ConsumeBool(), 4) {
        // A confirmed coin of the wallet's.
        CMutableTransaction funding;
        funding.vin.emplace_back(COutPoint{Txid::FromUint256(uint256{static_cast<uint8_t>(coins.size() + 1)}), 0});
        funding.vout.emplace_back(50 * COIN, fuzzed_wallet.GetScriptPubKey(fuzzed_data_provider));
        const CTransactionRef funding_tx{MakeTransactionRef(funding)};
        WITH_LOCK(wallet.cs_wallet, Assert(wallet.AddToWallet(funding_tx, TxStateConfirmed{tip_hash, tip_height, /*index=*/1})));
        coins.emplace_back(funding_tx->GetHash(), 0);

        // The mempool drops a chain the same way: the requests one block made stale, or the deposits
        // another replaced.
        const bool deposit{fuzzed_data_provider.ConsumeBool()};
        const auto reason{fuzzed_data_provider.ConsumeBool() ? MemPoolRemovalReason::CONFLICT : MemPoolRemovalReason::REPLACED};
        COutPoint prev{coins.back()};
        CAmount value{50 * COIN};
        const int length{fuzzed_data_provider.ConsumeIntegralInRange<int>(1, 4)};
        for (int i{0}; i < length; ++i) {
            CMutableTransaction mtx;
            mtx.vin.emplace_back(prev);
            if (deposit && i > 0) mtx.vin.emplace_back(COutPoint{prev.hash, 0});
            drivechain::BmmRequest request;
            request.slot = static_cast<uint32_t>(i);
            request.side_block_hash = uint256{static_cast<uint8_t>(coins.size())};
            // For a block that is not the tip: stale.
            request.prev_main_block_hash = uint256{1};
            value -= COIN;
            mtx.vout.emplace_back(deposit ? COIN * (i + 1) : 0, deposit ? drivechain::EscrowScript(1) : drivechain::BmmRequestScript(request));
            mtx.vout.emplace_back(value, fuzzed_wallet.GetScriptPubKey(fuzzed_data_provider));
            const CTransactionRef tx{MakeTransactionRef(mtx)};
            WITH_LOCK(wallet.cs_wallet, Assert(wallet.AddToWallet(tx, TxStateInMempool{})));
            removals.push_back({tx, reason});
            prev = COutPoint{tx->GetHash(), 1};
        }
    }
    std::vector<CTransactionRef> txs;
    for (const Removal& removal : removals) txs.push_back(removal.tx);

    const auto connect_block{[&] {
        CBlock block;
        interfaces::BlockInfo info{tip_hash};
        info.height = tip_height;
        info.data = &block;
        info.chain_time_max = std::numeric_limits<unsigned int>::max();
        wallet.blockConnected(kernel::ChainstateRole{}, info);
    }};
    while (!removals.empty()) {
        // The next removal the mempool notifies, in any order.
        const size_t next{fuzzed_data_provider.ConsumeIntegralInRange<size_t>(0, removals.size() - 1)};
        const Removal removal{removals[next]};
        removals.erase(removals.begin() + next);
        wallet.transactionRemovedFromMempool(removal.tx, removal.reason);
        if (fuzzed_data_provider.ConsumeBool()) {
            CallOneOf(
                fuzzed_data_provider,
                [&] {
                    const CTransactionRef& tx{PickValue(fuzzed_data_provider, txs)};
                    LOCK(wallet.cs_wallet);
                    (void)wallet.AbandonTransaction(wallet.mapWallet.at(tx->GetHash()));
                },
                [&] { connect_block(); });
        }
    }
    connect_block();

    LOCK(wallet.cs_wallet);
    for (const CTransactionRef& tx : txs) {
        const CWalletTx& wtx{wallet.mapWallet.at(tx->GetHash())};
        Assert(!wtx.InMempool());
        Assert(wtx.isAbandoned());
    }
    for (const COutPoint& coin : coins) Assert(!wallet.IsSpent(coin));
}
} // namespace
} // namespace wallet
