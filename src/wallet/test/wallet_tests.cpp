// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/wallet.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <thread>
#include <vector>

#include <addresstype.h>
#include <bech32.h>
#include <chainparams.h>
#include <util/strencodings.h>
#include <drivechain/sidechain.h>
#include <interfaces/chain.h>
#include <kernel/chainparams.h>
#include <key_io.h>
#include <node/blockstorage.h>
#include <node/types.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <policy/truc_policy.h>
#include <rpc/server.h>
#include <script/solver.h>
#include <test/util/common.h>
#include <test/util/logging.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <wallet/coincontrol.h>
#include <wallet/context.h>
#include <wallet/meltcast.h>
#include <wallet/receive.h>
#include <wallet/shielded.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>
#include <zside/shielded.h>
#include <zside/orchard.h>

#include <boost/test/unit_test.hpp>
#include <univalue.h>

using node::MAX_BLOCKFILE_SIZE;

namespace wallet {

// Ensure that fee levels defined in the wallet are at least as high
// as the default levels for node policy.
static_assert(DEFAULT_TRANSACTION_MINFEE >= DEFAULT_MIN_RELAY_TX_FEE, "wallet minimum fee is smaller than default relay fee");
static_assert(WALLET_INCREMENTAL_RELAY_FEE >= DEFAULT_INCREMENTAL_RELAY_FEE, "wallet incremental fee is smaller than default incremental relay fee");

BOOST_FIXTURE_TEST_SUITE(wallet_tests, WalletTestingSetup)

static CMutableTransaction TestSimpleSpend(const CTransaction& from, uint32_t index, const CKey& key, const CScript& pubkey)
{
    CMutableTransaction mtx;
    mtx.vout.emplace_back(from.vout[index].nValue - DEFAULT_TRANSACTION_MAXFEE, pubkey);
    mtx.vin.push_back({CTxIn{from.GetHash(), index}});
    FillableSigningProvider keystore;
    keystore.AddKey(key);
    std::map<COutPoint, Coin> coins;
    coins[mtx.vin[0].prevout].out = from.vout[index];
    std::map<int, bilingual_str> input_errors;
    BOOST_CHECK(SignTransaction(mtx, &keystore, coins, {.sighash_type = SIGHASH_ALL}, input_errors));
    return mtx;
}

static void AddKey(CWallet& wallet, const CKey& key)
{
    LOCK(wallet.cs_wallet);
    FlatSigningProvider provider;
    std::string error;
    auto descs = Parse("combo(" + EncodeSecret(key) + ")", provider, error, /* require_checksum=*/ false);
    assert(descs.size() == 1);
    auto& desc = descs.at(0);
    WalletDescriptor w_desc(std::move(desc), 0, 0, 1, 1);
    Assert(wallet.AddWalletDescriptor(w_desc, provider, "", false));
}

BOOST_FIXTURE_TEST_CASE(update_non_range_descriptor, TestingSetup)
{
    CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
    {
        LOCK(wallet.cs_wallet);
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        auto key{GenerateRandomKey()};
        auto desc_str{"combo(" + EncodeSecret(key) + ")"};
        FlatSigningProvider provider;
        std::string error;
        auto descs{Parse(desc_str, provider, error, /* require_checksum=*/ false)};
        auto& desc{descs.at(0)};
        WalletDescriptor w_desc{std::move(desc), 0, 0, 0, 0};
        BOOST_CHECK(wallet.AddWalletDescriptor(w_desc, provider, "", false));
        // Wallet should update the non-range descriptor successfully
        BOOST_CHECK(wallet.AddWalletDescriptor(w_desc, provider, "", false));
    }
}

BOOST_FIXTURE_TEST_CASE(scan_for_wallet_transactions, TestChain100Setup)
{
    // Cap last block file size, and mine new block in a new block file.
    CBlockIndex* oldTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());
    WITH_LOCK(::cs_main, m_node.chainman->m_blockman.GetBlockFileInfo(oldTip->GetBlockPos().nFile)->nSize = MAX_BLOCKFILE_SIZE);
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    CBlockIndex* newTip = WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Tip());

    // Verify ScanForWalletTransactions fails to read an unknown start block.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/{}, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK(result.last_scanned_block.IsNull());
        BOOST_CHECK(!result.last_scanned_height);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 0);
    }

    // Verify ScanForWalletTransactions picks up transactions in both the old
    // and new block files.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(newTip->nHeight, newTip->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        std::chrono::steady_clock::time_point fake_time;
        reserver.setNow([&] { fake_time += 60s; return fake_time; });
        reserver.reserve();

        {
            CBlockLocator locator;
            BOOST_CHECK(WalletBatch{wallet.GetDatabase()}.ReadBestBlock(locator));
            BOOST_REQUIRE(!locator.IsNull());
            BOOST_CHECK(locator.vHave.front() == newTip->GetBlockHash());
        }

        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*save_progress=*/true);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::SUCCESS);
        BOOST_CHECK(result.last_failed_block.IsNull());
        BOOST_CHECK_EQUAL(result.last_scanned_block, newTip->GetBlockHash());
        BOOST_CHECK_EQUAL(*result.last_scanned_height, newTip->nHeight);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 100 * COIN);

        {
            CBlockLocator locator;
            BOOST_CHECK(WalletBatch{wallet.GetDatabase()}.ReadBestBlock(locator));
            BOOST_REQUIRE(!locator.IsNull());
            BOOST_CHECK(locator.vHave.front() == newTip->GetBlockHash());
        }
    }

    // Prune the older block file.
    int file_number;
    {
        LOCK(cs_main);
        file_number = oldTip->GetBlockPos().nFile;
        Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file_number);
    }
    m_node.chainman->m_blockman.UnlinkPrunedFiles({file_number});

    // Verify ScanForWalletTransactions only picks transactions in the new block
    // file.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, oldTip->GetBlockHash());
        BOOST_CHECK_EQUAL(result.last_scanned_block, newTip->GetBlockHash());
        BOOST_CHECK_EQUAL(*result.last_scanned_height, newTip->nHeight);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 50 * COIN);
    }

    // Prune the remaining block file.
    {
        LOCK(cs_main);
        file_number = newTip->GetBlockPos().nFile;
        Assert(m_node.chainman)->m_blockman.PruneOneBlockFile(file_number);
    }
    m_node.chainman->m_blockman.UnlinkPrunedFiles({file_number});

    // Verify ScanForWalletTransactions scans no blocks.
    {
        CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
        {
            LOCK(wallet.cs_wallet);
            LOCK(Assert(m_node.chainman)->GetMutex());
            wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
            wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        }
        AddKey(wallet, coinbaseKey);
        WalletRescanReserver reserver(wallet);
        reserver.reserve();
        CWallet::ScanResult result = wallet.ScanForWalletTransactions(/*start_block=*/oldTip->GetBlockHash(), /*start_height=*/oldTip->nHeight, /*max_height=*/{}, reserver, /*save_progress=*/false);
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::FAILURE);
        BOOST_CHECK_EQUAL(result.last_failed_block, newTip->GetBlockHash());
        BOOST_CHECK(result.last_scanned_block.IsNull());
        BOOST_CHECK(!result.last_scanned_height);
        BOOST_CHECK_EQUAL(GetBalance(wallet).m_mine_immature, 0);
    }
}

BOOST_FIXTURE_TEST_CASE(scan_for_wallet_transactions_abort, TestChain100Setup)
{
    CWallet wallet(m_node.chain.get(), "", CreateMockableWalletDatabase());
    uint256 genesis_hash;
    {
        LOCK(wallet.cs_wallet);
        LOCK(Assert(m_node.chainman)->GetMutex());
        wallet.SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
        wallet.SetLastBlockProcessed(m_node.chainman->ActiveChain().Height(), m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        genesis_hash = m_node.chainman->ActiveChain().Genesis()->GetBlockHash();
    }

    // An abort requested while no rescan is held is stale and must
    // not cancel a later scan.
    wallet.AbortRescan();
    WalletRescanReserver reserver(wallet);
    BOOST_CHECK(reserver.reserve());
    BOOST_CHECK(!wallet.IsAbortingRescan());

    // An abort requested after the reservation but before the scan starts
    // (e.g. while importdescriptors is still deriving keys) must cancel the
    // scan.
    wallet.AbortRescan();
    CWallet::ScanResult result = wallet.ScanForWalletTransactions(genesis_hash, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false);
    BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::USER_ABORT);
    BOOST_CHECK(result.last_scanned_block.IsNull());
    BOOST_CHECK(!result.last_scanned_height);
    BOOST_CHECK(result.last_failed_block.IsNull());
}

//! Unloading a wallet aborts its rescan under way, which would otherwise hold the wallet until its end.
BOOST_FIXTURE_TEST_CASE(remove_wallet_aborts_rescan, TestChain100Setup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto wallet = TestCreateWallet(context);
    BOOST_REQUIRE(AddWallet(context, wallet));
    const uint256 genesis_hash{WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain().Genesis()->GetBlockHash())};
    {
        WalletRescanReserver reserver(*wallet);
        BOOST_REQUIRE(reserver.reserve());
        BOOST_CHECK(!wallet->IsAbortingRescan());
        BOOST_CHECK(RemoveWallet(context, wallet, /*load_on_start=*/std::nullopt));
        BOOST_CHECK(wallet->IsAbortingRescan());
        const CWallet::ScanResult result{wallet->ScanForWalletTransactions(genesis_hash, /*start_height=*/0, /*max_height=*/{}, reserver, /*save_progress=*/false)};
        BOOST_CHECK_EQUAL(result.status, CWallet::ScanResult::USER_ABORT);
        BOOST_CHECK(!result.last_scanned_height);
    }
    TestUnloadWallet(std::move(wallet));
}

// This test verifies that wallet settings can be added and removed
// concurrently, ensuring no race conditions occur during either process.
BOOST_FIXTURE_TEST_CASE(write_wallet_settings_concurrently, TestingSetup)
{
    auto chain = m_node.chain.get();
    const auto NUM_WALLETS{5};

    // Since we're counting the number of wallets, ensure we start without any.
    BOOST_REQUIRE(chain->getRwSetting("wallet").isNull());

    const auto& check_concurrent_wallet = [&](const auto& settings_function, int num_expected_wallets) {
        std::vector<std::thread> threads;
        threads.reserve(NUM_WALLETS);
        for (auto i{0}; i < NUM_WALLETS; ++i) threads.emplace_back(settings_function, i);
        for (auto& t : threads) t.join();

        auto wallets = chain->getRwSetting("wallet");
        BOOST_CHECK_EQUAL(wallets.getValues().size(), num_expected_wallets);
    };

    // Add NUM_WALLETS wallets concurrently, ensure we end up with NUM_WALLETS stored.
    check_concurrent_wallet([&chain](int i) {
        Assert(AddWalletSetting(*chain, strprintf("wallet_%d", i)));
    },
                            /*num_expected_wallets=*/NUM_WALLETS);

    // Remove NUM_WALLETS wallets concurrently, ensure we end up with 0 wallets.
    check_concurrent_wallet([&chain](int i) {
        Assert(RemoveWalletSetting(*chain, strprintf("wallet_%d", i)));
    },
                            /*num_expected_wallets=*/0);
}

static int64_t AddTx(ChainstateManager& chainman, CWallet& wallet, uint32_t lockTime, std::chrono::seconds mock_time, int64_t blockTime)
{
    CMutableTransaction tx;
    TxState state = TxStateInactive{};
    tx.nLockTime = lockTime;
    FakeNodeClock clock{mock_time};
    CBlockIndex* block = nullptr;
    if (blockTime > 0) {
        LOCK(cs_main);
        auto inserted = chainman.BlockIndex().emplace(std::piecewise_construct, std::make_tuple(GetRandHash()), std::make_tuple());
        assert(inserted.second);
        const uint256& hash = inserted.first->first;
        block = &inserted.first->second;
        block->nTime = blockTime;
        block->phashBlock = &hash;
        state = TxStateConfirmed{hash, block->nHeight, /*index=*/0};
    }
    return wallet.AddToWallet(MakeTransactionRef(tx), state, [&](CWalletTx& wtx, bool /* new_tx */) {
        // Assign wtx.m_state to simplify test and avoid the need to simulate
        // reorg events. Without this, AddToWallet asserts false when the same
        // transaction is confirmed in different blocks.
        wtx.m_state = state;
        return true;
    })->nTimeSmart;
}

// Simple test to verify assignment of CWalletTx::nSmartTime value. Could be
// expanded to cover more corner cases of smart time logic.
BOOST_AUTO_TEST_CASE(ComputeTimeSmart)
{
    // New transaction should use clock time if lower than block time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 1, 100s, 120), 100);

    // Test that updating existing transaction does not change smart time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 1, 200s, 220), 100);

    // New transaction should use clock time if there's no block time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 2, 300s, 0), 300);

    // New transaction should use block time if lower than clock time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 3, 420s, 400), 400);

    // New transaction should use latest entry time if higher than
    // min(block time, clock time).
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 4, 500s, 390), 400);

    // If there are future entries, new transaction should use time of the
    // newest entry that is no more than 300 seconds ahead of the clock time.
    BOOST_CHECK_EQUAL(AddTx(*m_node.chainman, m_wallet, 5, 50s, 600), 300);
}

void TestLoadWallet(const std::string& name, DatabaseFormat format, std::function<void(std::shared_ptr<CWallet>)> f)
{
    node::NodeContext node;
    auto chain{interfaces::MakeChain(node)};
    DatabaseOptions options;
    options.require_format = format;
    DatabaseStatus status;
    bilingual_str error;
    std::vector<bilingual_str> warnings;
    auto database{MakeWalletDatabase(name, options, status, error)};
    auto wallet{std::make_shared<CWallet>(chain.get(), "", std::move(database))};
    BOOST_CHECK_EQUAL(wallet->PopulateWalletFromDB(error, warnings), DBErrors::LOAD_OK);
    WITH_LOCK(wallet->cs_wallet, f(wallet));
}

BOOST_FIXTURE_TEST_CASE(LoadReceiveRequests, TestingSetup)
{
    for (DatabaseFormat format : DATABASE_FORMATS) {
        const std::string name{strprintf("receive-requests-%i", format)};
        TestLoadWallet(name, format, [](std::shared_ptr<CWallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
            BOOST_CHECK(!wallet->IsAddressPreviouslySpent(PKHash()));
            WalletBatch batch{wallet->GetDatabase()};
            BOOST_CHECK(batch.WriteAddressPreviouslySpent(PKHash(), true));
            BOOST_CHECK(batch.WriteAddressPreviouslySpent(ScriptHash(), true));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, PKHash(), "0", "val_rr00"));
            BOOST_CHECK(wallet->EraseAddressReceiveRequest(batch, PKHash(), "0"));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, PKHash(), "1", "val_rr10"));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, PKHash(), "1", "val_rr11"));
            BOOST_CHECK(wallet->SetAddressReceiveRequest(batch, ScriptHash(), "2", "val_rr20"));
        });
        TestLoadWallet(name, format, [](std::shared_ptr<CWallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
            BOOST_CHECK(wallet->IsAddressPreviouslySpent(PKHash()));
            BOOST_CHECK(wallet->IsAddressPreviouslySpent(ScriptHash()));
            auto requests = wallet->GetAddressReceiveRequests();
            auto erequests = {"val_rr11", "val_rr20"};
            BOOST_CHECK_EQUAL_COLLECTIONS(requests.begin(), requests.end(), std::begin(erequests), std::end(erequests));
            RunWithinTxn(wallet->GetDatabase(), /*process_desc=*/"test", [](WalletBatch& batch){
                BOOST_CHECK(batch.WriteAddressPreviouslySpent(PKHash(), false));
                BOOST_CHECK(batch.EraseAddressData(ScriptHash()));
                return true;
            });
        });
        TestLoadWallet(name, format, [](std::shared_ptr<CWallet> wallet) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
            BOOST_CHECK(!wallet->IsAddressPreviouslySpent(PKHash()));
            BOOST_CHECK(!wallet->IsAddressPreviouslySpent(ScriptHash()));
            auto requests = wallet->GetAddressReceiveRequests();
            auto erequests = {"val_rr11"};
            BOOST_CHECK_EQUAL_COLLECTIONS(requests.begin(), requests.end(), std::begin(erequests), std::end(erequests));
        });
    }
}

class ListCoinsTestingSetup : public TestChain100Setup
{
public:
    ListCoinsTestingSetup()
    {
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        wallet = CreateSyncedWallet(*m_node.chain, WITH_LOCK(Assert(m_node.chainman)->GetMutex(), return m_node.chainman->ActiveChain()), coinbaseKey);
    }

    ~ListCoinsTestingSetup()
    {
        wallet.reset();
    }

    CWalletTx& AddTx(CRecipient recipient)
    {
        CTransactionRef tx;
        CCoinControl dummy;
        {
            auto res = CreateTransaction(*wallet, {recipient}, /*change_pos=*/std::nullopt, dummy);
            BOOST_CHECK(res);
            tx = res->tx;
        }
        wallet->CommitTransaction(tx);
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            blocktx = CMutableTransaction(*wallet->mapWallet.at(tx->GetHash()).GetTx());
        }
        CreateAndProcessBlock({CMutableTransaction(blocktx)}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

        LOCK(wallet->cs_wallet);
        LOCK(Assert(m_node.chainman)->GetMutex());
        wallet->SetLastBlockProcessed(wallet->GetLastBlockHeight() + 1, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
        auto it = wallet->mapWallet.find(tx->GetHash());
        BOOST_CHECK(it != wallet->mapWallet.end());
        it->second.m_state = TxStateConfirmed{m_node.chainman->ActiveChain().Tip()->GetBlockHash(), m_node.chainman->ActiveChain().Height(), /*index=*/1};
        return it->second;
    }

    std::unique_ptr<CWallet> wallet;
};

BOOST_FIXTURE_TEST_CASE(ListCoinsTest, ListCoinsTestingSetup)
{
    std::string coinbaseAddress = coinbaseKey.GetPubKey().GetID().ToString();

    // Confirm ListCoins initially returns 1 coin grouped under coinbaseKey
    // address.
    std::map<CTxDestination, std::vector<COutput>> list;
    {
        LOCK(wallet->cs_wallet);
        list = ListCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 1U);

    // Check initial balance from one mature coinbase transaction.
    BOOST_CHECK_EQUAL(50 * COIN, WITH_LOCK(wallet->cs_wallet, return AvailableCoins(*wallet).GetTotalAmount()));

    // Add a transaction creating a change address, and confirm ListCoins still
    // returns the coin associated with the change address underneath the
    // coinbaseKey pubkey, even though the change address has a different
    // pubkey.
    AddTx(CRecipient{PubKeyDestination{{}}, 1 * COIN, /*subtract_fee=*/false});
    {
        LOCK(wallet->cs_wallet);
        list = ListCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2U);

    // Lock both coins. Confirm number of available coins drops to 0.
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet).Size(), 2U);
    }
    for (const auto& group : list) {
        for (const auto& coin : group.second) {
            LOCK(wallet->cs_wallet);
            wallet->LockCoin(coin.outpoint, /*persist=*/false);
        }
    }
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet).Size(), 0U);
    }
    // Confirm ListCoins still returns same result as before, despite coins
    // being locked.
    {
        LOCK(wallet->cs_wallet);
        list = ListCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(list.size(), 1U);
    BOOST_CHECK_EQUAL(std::get<PKHash>(list.begin()->first).ToString(), coinbaseAddress);
    BOOST_CHECK_EQUAL(list.begin()->second.size(), 2U);
}

void TestCoinsResult(ListCoinsTest& context, OutputType out_type, CAmount amount,
                     std::map<OutputType, size_t>& expected_coins_sizes)
{
    LOCK(context.wallet->cs_wallet);
    util::Result<CTxDestination> dest = Assert(context.wallet->GetNewDestination(out_type, ""));
    CWalletTx& wtx = context.AddTx(CRecipient{*dest, amount, /*fSubtractFeeFromAmount=*/true});
    CoinFilterParams filter;
    filter.skip_locked = false;
    CoinsResult available_coins = AvailableCoins(*context.wallet, nullptr, std::nullopt, filter);
    // Lock outputs so they are not spent in follow-up transactions
    for (uint32_t i = 0; i < wtx.GetTx()->vout.size(); i++) context.wallet->LockCoin({wtx.GetHash(), i}, /*persist=*/false);
    for (const auto& [type, size] : expected_coins_sizes) BOOST_CHECK_EQUAL(size, available_coins.coins[type].size());
}

BOOST_FIXTURE_TEST_CASE(BasicOutputTypesTest, ListCoinsTest)
{
    std::map<OutputType, size_t> expected_coins_sizes;
    for (const auto& out_type : OUTPUT_TYPES) { expected_coins_sizes[out_type] = 0U; }

    // Verify our wallet has one usable coinbase UTXO before starting
    // This UTXO is a P2PK, so it should show up in the Other bucket
    expected_coins_sizes[OutputType::UNKNOWN] = 1U;
    CoinsResult available_coins = WITH_LOCK(wallet->cs_wallet, return AvailableCoins(*wallet));
    BOOST_CHECK_EQUAL(available_coins.Size(), expected_coins_sizes[OutputType::UNKNOWN]);
    BOOST_CHECK_EQUAL(available_coins.coins[OutputType::UNKNOWN].size(), expected_coins_sizes[OutputType::UNKNOWN]);

    // We will create a self transfer for each of the OutputTypes and
    // verify it is put in the correct bucket after running GetAvailablecoins
    //
    // For each OutputType, We expect 2 UTXOs in our wallet following the self transfer:
    //   1. One UTXO as the recipient
    //   2. One UTXO from the change, due to payment address matching logic

    for (const auto& out_type : OUTPUT_TYPES) {
        if (out_type == OutputType::UNKNOWN) continue;
        expected_coins_sizes[out_type] = 2U;
        TestCoinsResult(*this, out_type, 1 * COIN, expected_coins_sizes);
    }
}

BOOST_FIXTURE_TEST_CASE(wallet_disableprivkeys, TestChain100Setup)
{
    const std::shared_ptr<CWallet> wallet = std::make_shared<CWallet>(m_node.chain.get(), "", CreateMockableWalletDatabase());
    LOCK(wallet->cs_wallet);
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    wallet->SetWalletFlag(WALLET_FLAG_DISABLE_PRIVATE_KEYS);
    BOOST_CHECK(!wallet->GetNewDestination(OutputType::BECH32, ""));
}

// Explicit calculation which is used to test the wallet constant
// We get the same virtual size due to rounding(weight/4) for both use_max_sig values
static size_t CalculateNestedKeyhashInputSize(bool use_max_sig)
{
    // Generate ephemeral valid pubkey
    CKey key = GenerateRandomKey();
    CPubKey pubkey = key.GetPubKey();

    // Generate pubkey hash
    uint160 key_hash(Hash160(pubkey));

    // Create inner-script to enter into keystore. Key hash can't be 0...
    CScript inner_script = CScript() << OP_0 << std::vector<unsigned char>(key_hash.begin(), key_hash.end());

    // Create outer P2SH script for the output
    uint160 script_id(Hash160(inner_script));
    CScript script_pubkey = CScript() << OP_HASH160 << std::vector<unsigned char>(script_id.begin(), script_id.end()) << OP_EQUAL;

    // Add inner-script to key store and key to watchonly
    FillableSigningProvider keystore;
    keystore.AddCScript(inner_script);
    keystore.AddKeyPubKey(key, pubkey);

    // Fill in dummy signatures for fee calculation.
    SignatureData sig_data;

    if (!ProduceSignature(keystore, use_max_sig ? DUMMY_MAXIMUM_SIGNATURE_CREATOR : DUMMY_SIGNATURE_CREATOR, script_pubkey, sig_data)) {
        // We're hand-feeding it correct arguments; shouldn't happen
        assert(false);
    }

    CTxIn tx_in;
    UpdateInput(tx_in, sig_data);
    return (size_t)GetVirtualTransactionInputSize(tx_in);
}

BOOST_FIXTURE_TEST_CASE(dummy_input_size_test, TestChain100Setup)
{
    BOOST_CHECK_EQUAL(CalculateNestedKeyhashInputSize(false), DUMMY_NESTED_P2WPKH_INPUT_SIZE);
    BOOST_CHECK_EQUAL(CalculateNestedKeyhashInputSize(true), DUMMY_NESTED_P2WPKH_INPUT_SIZE);
}

bool malformed_descriptor(std::ios_base::failure e)
{
    std::string s(e.what());
    return s.find("Missing checksum") != std::string::npos;
}

BOOST_FIXTURE_TEST_CASE(wallet_descriptor_test, BasicTestingSetup)
{
    std::vector<unsigned char> malformed_record;
    VectorWriter vw{malformed_record, 0};
    vw << std::string("notadescriptor");
    vw << uint64_t{0};
    vw << int32_t{0};
    vw << int32_t{0};
    vw << int32_t{1};

    SpanReader vr{malformed_record};
    std::optional<WalletDescriptor> w_desc;
    BOOST_CHECK_EXCEPTION(w_desc.emplace(WalletDescriptor::FromStream(deserialize, vr)), std::ios_base::failure, malformed_descriptor);
}

//! Test CWallet::CreateNew() and its behavior handling potential race
//! conditions if it's called the same time an incoming transaction shows up in
//! the mempool or a new block.
//!
//! It isn't possible to verify there aren't race condition in every case, so
//! this test just checks two specific cases and ensures that timing of
//! notifications in these cases doesn't prevent the wallet from detecting
//! transactions.
//!
//! In the first case, block and mempool transactions are created before the
//! wallet is loaded, but notifications about these transactions are delayed
//! until after it is loaded. The notifications are superfluous in this case, so
//! the test verifies the transactions are detected before they arrive.
//!
//! In the second case, block and mempool transactions are created after the
//! wallet rescan and notifications are immediately synced, to verify the wallet
//! must already have a handler in place for them, and there's no gap after
//! rescanning where new transactions in new blocks could be lost.
BOOST_FIXTURE_TEST_CASE(CreateWallet, TestChain100Setup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    // Create new wallet with known key and unload it.
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto wallet = TestCreateWallet(context);
    CKey key = GenerateRandomKey();
    AddKey(*wallet, key);
    TestUnloadWallet(std::move(wallet));


    // Add log hook to detect AddToWallet events from rescans, blockConnected,
    // and transactionAddedToMempool notifications
    int addtx_count = 0;
    DebugLogHelper addtx_counter("[default wallet] AddToWallet", [&](const std::string* s) {
        if (s) ++addtx_count;
        return false;
    });


    bool rescan_completed = false;
    DebugLogHelper rescan_check("[default wallet] Rescan completed", [&](const std::string* s) {
        if (s) rescan_completed = true;
        return false;
    });


    // Block the queue to prevent the wallet receiving blockConnected and
    // transactionAddedToMempool notifications, and create block and mempool
    // transactions paying to the wallet
    std::promise<void> promise;
    m_node.validation_signals->CallFunctionInValidationInterfaceQueue([&promise] {
        promise.get_future().wait();
    });
    std::string error;
    m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto block_tx = TestSimpleSpend(*m_coinbase_txns[0], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    m_coinbase_txns.push_back(CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto mempool_tx = TestSimpleSpend(*m_coinbase_txns[1], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    BOOST_CHECK(m_node.chain->broadcastTransaction(MakeTransactionRef(mempool_tx), DEFAULT_TRANSACTION_MAXFEE, node::TxBroadcast::MEMPOOL_NO_BROADCAST, error));


    // Reload wallet and make sure new transactions are detected despite events
    // being blocked
    // Loading will also ask for current mempool transactions
    wallet = TestLoadWallet(context);
    BOOST_CHECK(rescan_completed);
    // AddToWallet events for block_tx and mempool_tx (x2)
    BOOST_CHECK_EQUAL(addtx_count, 3);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->mapWallet.contains(block_tx.GetHash()));
        BOOST_CHECK(wallet->mapWallet.contains(mempool_tx.GetHash()));
    }


    // Unblock notification queue and make sure stale blockConnected and
    // transactionAddedToMempool events are processed
    promise.set_value();
    m_node.validation_signals->SyncWithValidationInterfaceQueue();
    // AddToWallet events for block_tx and mempool_tx events are counted a
    // second time as the notification queue is processed
    BOOST_CHECK_EQUAL(addtx_count, 5);


    TestUnloadWallet(std::move(wallet));


    // Load wallet again, this time creating new block and mempool transactions
    // paying to the wallet as the wallet finishes loading and syncing the
    // queue so the events have to be handled immediately. Releasing the wallet
    // lock during the sync is a little artificial but is needed to avoid a
    // deadlock during the sync and simulates a new block notification happening
    // as soon as possible.
    addtx_count = 0;
    auto handler = HandleLoadWallet(context, [&](std::unique_ptr<interfaces::Wallet> wallet) {
            BOOST_CHECK(rescan_completed);
            m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
            block_tx = TestSimpleSpend(*m_coinbase_txns[2], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
            m_coinbase_txns.push_back(CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
            mempool_tx = TestSimpleSpend(*m_coinbase_txns[3], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
            BOOST_CHECK(m_node.chain->broadcastTransaction(MakeTransactionRef(mempool_tx), DEFAULT_TRANSACTION_MAXFEE, node::TxBroadcast::MEMPOOL_NO_BROADCAST, error));
            m_node.validation_signals->SyncWithValidationInterfaceQueue();
        });
    wallet = TestLoadWallet(context);
    // Since mempool transactions are requested at the end of loading, there will
    // be 2 additional AddToWallet calls, one from the previous test, and a duplicate for mempool_tx
    BOOST_CHECK_EQUAL(addtx_count, 2 + 2);
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->mapWallet.contains(block_tx.GetHash()));
        BOOST_CHECK(wallet->mapWallet.contains(mempool_tx.GetHash()));
    }


    TestUnloadWallet(std::move(wallet));
}

//! A load handler is called without the wallets' lock, and once disconnected it is never called
//! again: disconnecting waits for a call in flight (its owner may be deleted right after).
BOOST_FIXTURE_TEST_CASE(load_wallet_handler_disconnect, TestingSetup)
{
    WalletContext context;
    context.args = &m_args;
    auto wallet = std::make_shared<CWallet>(m_node.chain.get(), "", CreateMockableWalletDatabase());

    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> released{release.get_future().share()};
    std::atomic<int> calls{0};
    std::atomic<int> other_calls{0};
    auto handler = HandleLoadWallet(context, [&](std::unique_ptr<interfaces::Wallet>) {
        GetWallets(context); // the wallets' lock is free
        if (++calls == 1) {
            entered.set_value();
            released.wait();
        }
    });
    auto other = HandleLoadWallet(context, [&](std::unique_ptr<interfaces::Wallet>) { ++other_calls; });

    std::thread notifier{[&] { NotifyWalletLoaded(context, wallet); }};
    entered.get_future().wait();
    std::atomic<bool> disconnected{false};
    std::thread remover{[&] { handler->disconnect(); disconnected = true; }};
    std::this_thread::sleep_for(std::chrono::milliseconds{200});
    BOOST_CHECK(!disconnected); // waits for the call in flight
    release.set_value();
    remover.join();
    notifier.join();
    BOOST_CHECK(disconnected);
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK_EQUAL(other_calls, 1);

    NotifyWalletLoaded(context, wallet);
    BOOST_CHECK_EQUAL(calls, 1);
    BOOST_CHECK_EQUAL(other_calls, 2);
    handler.reset(); // destroying a disconnected handler is harmless
    other.reset();
    NotifyWalletLoaded(context, wallet);
    BOOST_CHECK_EQUAL(other_calls, 2);
}

BOOST_FIXTURE_TEST_CASE(CreateWalletWithoutChain, BasicTestingSetup)
{
    WalletContext context;
    context.args = &m_args;
    auto wallet = TestCreateWallet(context);
    BOOST_CHECK(wallet);
    WaitForDeleteWallet(std::move(wallet));
}

BOOST_FIXTURE_TEST_CASE(RemoveTxs, TestChain100Setup)
{
    m_args.ForceSetArg("-unsafesqlitesync", "1");
    WalletContext context;
    context.args = &m_args;
    context.chain = m_node.chain.get();
    auto wallet = TestCreateWallet(context);
    CKey key = GenerateRandomKey();
    AddKey(*wallet, key);

    m_coinbase_txns.push_back(CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey())).vtx[0]);
    auto block_tx = TestSimpleSpend(*m_coinbase_txns[0], 0, coinbaseKey, GetScriptForRawPubKey(key.GetPubKey()));
    CreateAndProcessBlock({block_tx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

    m_node.validation_signals->SyncWithValidationInterfaceQueue();

    {
        auto block_hash = block_tx.GetHash();
        auto prev_tx = m_coinbase_txns[0];

        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->HasWalletSpend(prev_tx));
        BOOST_CHECK(wallet->mapWallet.contains(block_hash));

        std::vector<Txid> vHashIn{ block_hash };
        BOOST_CHECK(wallet->RemoveTxs(vHashIn));

        BOOST_CHECK(!wallet->HasWalletSpend(prev_tx));
        BOOST_CHECK(!wallet->mapWallet.contains(block_hash));
    }

    TestUnloadWallet(std::move(wallet));
}

BOOST_FIXTURE_TEST_CASE(drivechain_parents_not_selected, ListCoinsTestingSetup)
{
    // The mempool takes a transaction spending an unconfirmed output of a BMM request or a treasury
    // transaction only if it is a small one of their kind (dc-unconfirmed-parent): the wallet does
    // not select those outputs for anything else.
    const auto request_recipient{[](uint8_t n) {
        drivechain::BmmRequest request;
        request.slot = 1;
        request.side_block_hash = uint256{n};
        request.prev_main_block_hash = uint256{2};
        return CRecipient{CNoDestination{drivechain::BmmRequestScript(request)}, 0, /*fSubtractFeeFromAmount=*/false};
    }};
    CTransactionRef request;
    {
        CCoinControl coin_control;
        auto res{CreateTransaction(*wallet, {request_recipient(1)}, /*change_pos=*/1, coin_control)};
        BOOST_REQUIRE(res);
        request = res->tx;
    }
    BOOST_CHECK(IsDrivechainParent(*request));
    {
        CMutableTransaction deposit;
        deposit.vout.emplace_back(COIN, drivechain::EscrowScript(1));
        BOOST_CHECK(IsDrivechainParent(CTransaction{deposit}));
        BOOST_CHECK(!IsDrivechainParent(CTransaction{CMutableTransaction{}}));
    }
    // In the mempool (no sidechain is active on the test chain, which would let it in), it spends
    // the only coin of the wallet: its change is all there is.
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->AddToWallet(request, TxStateInMempool{}));
        // (With coin control: unconfirmed coins are checked against its transaction version.)
        const CCoinControl coin_control;
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet, &coin_control).Size(), 0U);
        CoinFilterParams listed;
        listed.skip_drivechain_parents = false;
        const CoinsResult coins{AvailableCoins(*wallet, &coin_control, std::nullopt, listed)};
        BOOST_REQUIRE_EQUAL(coins.Size(), 1U);
        BOOST_CHECK(coins.All().at(0).outpoint == COutPoint(request->GetHash(), 1));
        BOOST_CHECK_EQUAL(ListCoins(*wallet).size(), 1U);
    }
    // A payment cannot use it.
    {
        CCoinControl coin_control;
        const CTxDestination dest{PKHash{coinbaseKey.GetPubKey()}};
        BOOST_CHECK(!CreateTransaction(*wallet, {CRecipient{dest, COIN, /*fSubtractFeeFromAmount=*/false}}, /*change_pos=*/std::nullopt, coin_control));
    }
    // Another request can, made small enough.
    {
        CCoinControl coin_control;
        coin_control.m_allow_drivechain_parents = true;
        coin_control.m_max_tx_weight = TRUC_CHILD_MAX_WEIGHT;
        auto res{CreateTransaction(*wallet, {request_recipient(3)}, /*change_pos=*/1, coin_control)};
        BOOST_REQUIRE(res);
        BOOST_CHECK(res->tx->vin.at(0).prevout == COutPoint(request->GetHash(), 1));
        BOOST_CHECK_LE(GetTransactionWeight(*res->tx), TRUC_CHILD_MAX_WEIGHT);
    }
    // Nothing else can when the outputs of BMM requests are left out (a deposit): it would leave the
    // mempool with the request if that is not in the next block.
    {
        LOCK(wallet->cs_wallet);
        CCoinControl coin_control;
        coin_control.m_allow_drivechain_parents = true;
        coin_control.m_allow_bmm_request_parents = false;
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet, &coin_control).Size(), 0U);
        BOOST_CHECK(IsBmmRequest(*request));
    }
    // Once confirmed, it is a coin like any other.
    {
        LOCK(wallet->cs_wallet);
        LOCK(Assert(m_node.chainman)->GetMutex());
        wallet->mapWallet.at(request->GetHash()).m_state = TxStateConfirmed{m_node.chainman->ActiveChain().Tip()->GetBlockHash(), m_node.chainman->ActiveChain().Height(), /*index=*/1};
        BOOST_CHECK_EQUAL(AvailableCoins(*wallet).Size(), 1U);
    }
}

BOOST_AUTO_TEST_CASE(shielded_spends_capped)
{
    // zSide: a payment out of the shielded account spends no more notes than a transaction the network
    // relays holds (each is an action of about 3 kB).
    const CTxOut transparent{COIN, CScript() << OP_TRUE};
    const auto max{MaxShieldedSpends({transparent}, 1)};
    BOOST_REQUIRE(max);
    BOOST_CHECK_GT(*max, 20U);
    BOOST_CHECK_LT(*max, 40U);
    const auto weight{[&](size_t spends) {
        CMutableTransaction mtx;
        mtx.vout = {transparent, CTxOut{0, CScript() << OP_RETURN << std::vector<unsigned char>(BundleSize(ActionCount(spends, 1)), 0)}};
        return GetTransactionWeight(CTransaction{mtx});
    }};
    BOOST_CHECK_LE(weight(*max), MAX_STANDARD_TX_WEIGHT);
    BOOST_CHECK_GT(weight(*max + 1), MAX_STANDARD_TX_WEIGHT);
    // Actions pair a spend with an output: outputs fewer than the notes cost nothing more. Too many
    // outputs alone, and no note can be spent.
    BOOST_CHECK_EQUAL(*MaxShieldedSpends({transparent}, 10), *max);
    BOOST_CHECK(!MaxShieldedSpends({}, 40));
}

BOOST_FIXTURE_TEST_CASE(shielded_commit_a_block_took, ListCoinsTestingSetup)
{
    // zSide: a shielded payment checks the mempool took it once the wallet's lock is released. A block
    // may have taken it from the mempool since the commit (the in-process miner, or a block that came
    // in): the payment went, and is neither abandoned nor reported as a failure.
    auto handler{m_node.chain->handleNotifications({wallet.get(), [](CWallet*) {}})};
    wallet->SetBroadcastTransactions(true);
    const CScript coinbase_script{GetScriptForRawPubKey(coinbaseKey.GetPubKey())};
    const auto make{[&] {
        CCoinControl coin_control;
        auto res{CreateTransaction(*wallet, {CRecipient{PKHash{coinbaseKey.GetPubKey()}, COIN, /*fSubtractFeeFromAmount=*/false}}, /*change_pos=*/std::nullopt, coin_control)};
        BOOST_REQUIRE(res);
        return res->tx;
    }};

    const CTransactionRef mined{make()};
    wallet->CommitTransaction(mined);
    BOOST_CHECK(m_node.chain->isInMempool(mined->GetHash()));
    // The block takes it before the check: no longer in the mempool, and the wallet may not have
    // heard of the block yet (the check waits for it).
    CreateAndProcessBlock({CMutableTransaction{*mined}}, coinbase_script);
    BOOST_CHECK(!m_node.chain->isInMempool(mined->GetHash()));
    BOOST_CHECK(!RefusedCommit(*wallet, mined));
    {
        LOCK(wallet->cs_wallet);
        const CWalletTx* wtx{wallet->GetWalletTx(mined->GetHash())};
        BOOST_REQUIRE(wtx);
        BOOST_CHECK(!wtx->isAbandoned());
        BOOST_CHECK_EQUAL(wallet->GetTxDepthInMainChain(*wtx), 1);
    }

    // Still in the mempool: kept, of course.
    const CTransactionRef waiting{make()};
    wallet->CommitTransaction(waiting);
    BOOST_CHECK(!RefusedCommit(*wallet, waiting));
    BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return wallet->GetWalletTx(waiting->GetHash())->isAbandoned()));
    CreateAndProcessBlock({CMutableTransaction{*waiting}}, coinbase_script);

    // One the mempool refused (here: not final yet) is abandoned, its coins free again, and the
    // error says so.
    const CTransactionRef refused{[&] {
        CCoinControl coin_control;
        auto res{CreateTransaction(*wallet, {CRecipient{PKHash{coinbaseKey.GetPubKey()}, COIN, /*fSubtractFeeFromAmount=*/false}}, /*change_pos=*/std::nullopt, coin_control, /*sign=*/false)};
        BOOST_REQUIRE(res);
        CMutableTransaction mtx{*res->tx};
        mtx.nLockTime = WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height()) + 100;
        for (CTxIn& in : mtx.vin) in.nSequence = CTxIn::MAX_SEQUENCE_NONFINAL;
        BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->SignTransaction(mtx)));
        return MakeTransactionRef(std::move(mtx));
    }()};
    wallet->CommitTransaction(refused);
    BOOST_CHECK(!m_node.chain->isInMempool(refused->GetHash()));
    const auto error{RefusedCommit(*wallet, refused)};
    BOOST_REQUIRE(error);
    BOOST_CHECK(error->find("not accepted into the mempool") != std::string::npos);
    {
        LOCK(wallet->cs_wallet);
        const CWalletTx* wtx{wallet->GetWalletTx(refused->GetHash())};
        BOOST_REQUIRE(wtx);
        BOOST_CHECK(wtx->isAbandoned());
        for (const CTxIn& in : refused->vin) BOOST_CHECK(!wallet->IsSpent(in.prevout));
    }

    // A wallet that does not broadcast (-walletbroadcast=0) never hands it to the mempool: kept for
    // the user to send, neither abandoned nor reported as a failure.
    wallet->SetBroadcastTransactions(false);
    const CTransactionRef kept{make()};
    wallet->CommitTransaction(kept);
    BOOST_CHECK(!m_node.chain->isInMempool(kept->GetHash()));
    BOOST_CHECK(!RefusedCommit(*wallet, kept));
    {
        LOCK(wallet->cs_wallet);
        const CWalletTx* wtx{wallet->GetWalletTx(kept->GetHash())};
        BOOST_REQUIRE(wtx);
        BOOST_CHECK(!wtx->isAbandoned());
        for (const CTxIn& in : kept->vin) BOOST_CHECK(wallet->IsSpent(in.prevout));
    }
}

/** zSide: a viewing key and the default address of a spending key made of `seed_byte`. */
static std::pair<OrchardFvk, OrchardAddress> ShieldedKeyOf(uint8_t seed_byte)
{
    std::array<uint8_t, 32> seed{};
    seed.fill(seed_byte);
    std::array<uint8_t, 32> sk;
    BOOST_REQUIRE_EQUAL(zo_spending_key_from_seed(seed.data(), seed.size(), 1, 0, sk.data()), ZO_OK);
    OrchardFvk fvk;
    OrchardAddress address;
    BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk.data(), fvk.data()), ZO_OK);
    BOOST_REQUIRE_EQUAL(zo_address(fvk.data(), 0, false, address.data()), ZO_OK);
    return {fvk, address};
}

/** zSide: a transaction that pays `value` into the pool to `address` (a bundle of two actions, proved), with one transparent input. */
static CMutableTransaction ShieldingTxTo(const OrchardFvk& ovk, const OrchardAddress& address, CAmount value)
{
    const uint256 empty{zside::EmptyRoot(zside::TREE_DEPTH)};
    ZoBuilder* builder{zo_builder_new(empty.data())};
    BOOST_REQUIRE(builder);
    BOOST_REQUIRE_EQUAL(zo_builder_add_output(builder, ovk.data(), address.data(), static_cast<uint64_t>(value), MemoFromText("")->data()), ZO_OK);
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    BOOST_REQUIRE(unsigned_bundle);
    CMutableTransaction mtx;
    mtx.version = 2;
    mtx.vin.emplace_back(COutPoint{Txid::FromUint256(uint256::ONE), 0});
    mtx.vout.emplace_back(value, zside::ShieldedMarkerScript(std::vector<uint8_t>(BundleSize(zo_unsigned_num_actions(unsigned_bundle)), 0)));
    uint256 commitment;
    BOOST_REQUIRE_EQUAL(zo_unsigned_commitment(unsigned_bundle, commitment.data()), ZO_OK);
    const uint256 sighash{zside::ShieldedSighash(CTransaction{mtx}, 0, commitment, zside::SighashDomainOf(Params().GetConsensus()))};
    uint8_t* bytes{nullptr};
    size_t len{0};
    BOOST_REQUIRE_EQUAL(zo_unsigned_finish(unsigned_bundle, sighash.data(), &bytes, &len), ZO_OK);
    mtx.vout[0].scriptPubKey = zside::ShieldedMarkerScript(std::span{bytes, len});
    zo_buffer_free(bytes, len);
    return mtx;
}

BOOST_FIXTURE_TEST_CASE(shielded_note_carried_by_another_tx, TestChain100Setup)
{
    // zSide: the bundle of a transaction can go out in another one: proved again by its sender, or its
    // transparent signatures malleated (the bundle does not sign them). The note is the same; the
    // wallet keeps it while any transaction that carries it is not abandoned nor conflicted -- the
    // one mined, typically, not the one it saw first.
    CWallet wallet{m_node.chain.get(), "", CreateMockableWalletDatabase()};
    const auto [fvk, address]{ShieldedKeyOf(3)};
    int height;
    uint256 tip;
    {
        LOCK2(wallet.cs_wallet, ::cs_main);
        height = m_node.chainman->ActiveChain().Height();
        tip = m_node.chainman->ActiveChain().Tip()->GetBlockHash();
        wallet.SetLastBlockProcessed(height, tip);
        wallet.m_shielded.fvk = fvk;
    }
    const CMutableTransaction seen{ShieldingTxTo(fvk, address, 5 * COIN)};
    CMutableTransaction malleated{seen};
    malleated.vin[0].scriptSig = CScript() << OP_TRUE;
    const CTransactionRef first{MakeTransactionRef(seen)};
    const CTransactionRef mined{MakeTransactionRef(malleated)};
    BOOST_REQUIRE(first->GetHash() != mined->GetHash());

    LOCK(wallet.cs_wallet);
    BOOST_REQUIRE(wallet.AddToWallet(first, TxStateInMempool{}));
    {
        const auto notes{ShieldedNotes(wallet)};
        BOOST_REQUIRE_EQUAL(notes.size(), 1U);
        BOOST_CHECK(notes[0].txid == first->GetHash());
        BOOST_CHECK_EQUAL(GetShieldedBalance(wallet).pending, 5 * COIN);
    }
    // The other is mined; the first conflicts with it (it spends the same coin).
    BOOST_REQUIRE(wallet.AddToWallet(mined, TxStateConfirmed{tip, height, /*index=*/1}));
    wallet.mapWallet.at(first->GetHash()).m_state = TxStateBlockConflicted{tip, height};
    const auto check_mined{[&] {
        const auto notes{ShieldedNotes(wallet)};
        BOOST_REQUIRE_EQUAL(notes.size(), 1U);
        BOOST_CHECK(notes[0].txid == mined->GetHash());
        BOOST_CHECK_EQUAL(notes[0].confirmations, 1);
        BOOST_CHECK(!notes[0].spent_by);
        const ShieldedBalance balance{GetShieldedBalance(wallet)};
        BOOST_CHECK_EQUAL(balance.available, 5 * COIN);
        BOOST_CHECK_EQUAL(balance.pending, 0);
        BOOST_CHECK_EQUAL(balance.notes, 1);
    }};
    check_mined();

    // Taken in again, in any order and more than once (a rescan, a load): the same.
    wallet.m_shielded.notes.clear();
    ShieldedTxAdded(wallet, wallet.mapWallet.at(mined->GetHash()));
    ShieldedTxAdded(wallet, wallet.mapWallet.at(first->GetHash()));
    ShieldedTxAdded(wallet, wallet.mapWallet.at(mined->GetHash()));
    BOOST_REQUIRE_EQUAL(wallet.m_shielded.notes.size(), 1U);
    BOOST_CHECK_EQUAL(wallet.m_shielded.notes.begin()->second.carriers.size(), 2U);
    check_mined();

    // Neither carries it any more: gone.
    wallet.mapWallet.at(mined->GetHash()).m_state = TxStateInactive{/*abandoned=*/true};
    BOOST_CHECK(ShieldedNotes(wallet).empty());
    BOOST_CHECK_EQUAL(GetShieldedBalance(wallet).notes, 0);
}

BOOST_FIXTURE_TEST_CASE(shielded_keys_of_the_chain, ListCoinsTestingSetup)
{
    // zSide: an account set up now has keys of version 2, bound to the chain (its genesis block and
    // slot): one seed makes unrelated keys on each chain. It watches the keys of version 1 of the same
    // descriptor too, the same on every chain, so that what was paid to them is found.
    LOCK(wallet->cs_wallet);
    BOOST_REQUIRE(SetupShieldedAccount(*wallet));
    const ShieldedAccount& account{wallet->m_shielded};
    BOOST_CHECK_EQUAL(account.key_version, KEY_THIS_CHAIN);
    BOOST_REQUIRE(account.fvk);
    BOOST_REQUIRE(account.legacy_fvk);
    const auto fvk_of{[](const std::optional<ShieldedKey>& sk) {
        BOOST_REQUIRE(sk);
        OrchardFvk fvk;
        BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk->data(), fvk.data()), ZO_OK);
        return fvk;
    }};
    const Consensus::Params& consensus{Params().GetConsensus()};
    const uint256& genesis{consensus.hashGenesisBlock};
    const uint32_t slot{consensus.sidechain.slot};
    BOOST_CHECK(fvk_of(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN)) == *account.fvk);
    BOOST_CHECK(fvk_of(ShieldedSpendingKey(*wallet, KEY_ANY_CHAIN)) == *account.legacy_fvk);
    // Another genesis block, another slot: other keys of version 2; version 1 is the same everywhere.
    const std::set<OrchardFvk> keys{*account.fvk, *account.legacy_fvk,
                                    fvk_of(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN, uint256::ONE, slot)),
                                    fvk_of(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN, genesis, slot + 1))};
    BOOST_CHECK_EQUAL(keys.size(), 4U);
    BOOST_CHECK(fvk_of(ShieldedSpendingKey(*wallet, KEY_ANY_CHAIN, uint256::ONE, slot + 1)) == *account.legacy_fvk);
    BOOST_CHECK(!ShieldedSpendingKey(*wallet, 3));

    // Its addresses are of version 2; one of version 1 is its own too, and what it was paid is found.
    OrchardAddress legacy_address;
    BOOST_REQUIRE_EQUAL(zo_address(account.legacy_fvk->data(), 0, false, legacy_address.data()), ZO_OK);
    BOOST_CHECK(*ShieldedAddress(*wallet, 0) != legacy_address);
    BOOST_CHECK_EQUAL(ShieldedAddressScope(account, *ShieldedAddress(*wallet, 0)), 1);
    BOOST_CHECK_EQUAL(ShieldedAddressScope(account, legacy_address), 1);
    BOOST_CHECK_EQUAL(ShieldedAddressScope(account, *ShieldedAddress(*wallet, 0, /*internal=*/true)), 2);
    BOOST_CHECK_EQUAL(ShieldedAddressScope(account, ShieldedKeyOf(4).second), 0);
    const CTransactionRef paid{MakeTransactionRef(ShieldingTxTo(ShieldedKeyOf(4).first, legacy_address, COIN))};
    BOOST_CHECK(IsShieldedMine(*wallet, *paid));
    BOOST_REQUIRE(wallet->AddToWallet(paid, TxStateInMempool{}));
    const auto notes{ShieldedNotes(*wallet)};
    BOOST_REQUIRE_EQUAL(notes.size(), 1U);
    BOOST_CHECK(notes[0].note->legacy);
    BOOST_CHECK_EQUAL(notes[0].note->value, COIN);
    // One to another key is not.
    BOOST_CHECK(!IsShieldedMine(*wallet, CTransaction{ShieldingTxTo(ShieldedKeyOf(4).first, ShieldedKeyOf(4).second, COIN)}));
}

BOOST_FIXTURE_TEST_CASE(meltcast_stops_between_steps, ListCoinsTestingSetup)
{
    // zSide: each step of a melt/cast plan proves for seconds; shutting down, or unloading the wallet,
    // waits for the one under way at most, not for every step due.
    AddTx(CRecipient{PKHash{coinbaseKey.GetPubKey()}, 10 * COIN, /*fSubtractFeeFromAmount=*/false});
    const auto planned{ScheduleMelt(*wallet, /*spread=*/0)};
    BOOST_REQUIRE(std::holds_alternative<std::vector<MeltCastJob>>(planned));
    BOOST_REQUIRE_GE(std::get<std::vector<MeltCastJob>>(planned).size(), 2U);
    // Due at the next block.
    CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    int height;
    {
        LOCK2(wallet->cs_wallet, ::cs_main);
        height = m_node.chainman->ActiveChain().Height();
        wallet->SetLastBlockProcessed(height, m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    }
    const auto tried{[&] {
        LOCK(wallet->cs_wallet);
        return std::count_if(wallet->m_shielded.jobs.begin(), wallet->m_shielded.jobs.end(), [&](const auto& j) { return j.second.tried == height; });
    }};
    // Interrupted from the start: no step is tried.
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, [] { return true; }), 0);
    BOOST_CHECK_EQUAL(tried(), 0);
    // Interrupted after the first step: that one alone is tried (made, or failed: either way it went).
    int asked{0};
    RunMeltCast(*wallet, [&] { return ++asked > 1; });
    BOOST_CHECK_EQUAL(asked, 2);
    BOOST_CHECK_EQUAL(tried(), 1);
}

/** zSide: a block more, the wallet told of it (the wallets of these tests are not attached to the chain); its height. */
static int NextBlock(TestChain100Setup& setup, CWallet& wallet, const CKey& key)
{
    setup.CreateAndProcessBlock({}, GetScriptForRawPubKey(key.GetPubKey()));
    LOCK2(wallet.cs_wallet, ::cs_main);
    const int height{setup.m_node.chainman->ActiveChain().Height()};
    wallet.SetLastBlockProcessed(height, setup.m_node.chainman->ActiveChain().Tip()->GetBlockHash());
    return height;
}

BOOST_FIXTURE_TEST_CASE(meltcast_locked_spent_cancelled, ListCoinsTestingSetup)
{
    // zSide: a melt waits while the wallet is locked; one whose coin was spent some other way is dropped;
    // one cancelled while it was being made went, and is not kept.
    using namespace meltcast;
    AddTx(CRecipient{PKHash{coinbaseKey.GetPubKey()}, 10 * COIN, /*fSubtractFeeFromAmount=*/false});
    const auto planned{ScheduleMelt(*wallet, /*spread=*/0)};
    BOOST_REQUIRE(std::holds_alternative<std::vector<MeltCastJob>>(planned));
    std::vector<MeltCastJob> melts{std::get<std::vector<MeltCastJob>>(planned)};
    BOOST_REQUIRE_GE(melts.size(), 2U);
    // Two of them, the first due first (steps go in the order of their ids).
    for (size_t i{2}; i < melts.size(); ++i) BOOST_REQUIRE(CancelMeltCast(*wallet, melts[i].id));
    melts.resize(2);
    BOOST_REQUIRE(melts[0].id < melts[1].id);
    const auto job{[&](uint64_t id) -> std::optional<MeltCastJob> {
        LOCK(wallet->cs_wallet);
        const auto it{wallet->m_shielded.jobs.find(id)};
        if (it == wallet->m_shielded.jobs.end()) return std::nullopt;
        return it->second;
    }};

    // Locked: each step due waits, and says why; nothing is made.
    const SecureString passphrase{"melt"};
    BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
    BOOST_REQUIRE(wallet->IsLocked());
    int height{NextBlock(*this, *wallet, coinbaseKey)};
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, {}), 0);
    for (const MeltCastJob& melt : melts) {
        BOOST_CHECK_EQUAL(job(melt.id)->error, "Waiting for the wallet to be unlocked");
        BOOST_CHECK_EQUAL(job(melt.id)->tried, height);
        BOOST_CHECK(!job(melt.id)->Sent());
        BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(melt.coin)));
    }
    // Tried once a block: not again at the same height.
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, [] { BOOST_ERROR("no step is due"); return true; }), 0);
    // Still locked at the next block: the same.
    height = NextBlock(*this, *wallet, coinbaseKey);
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, {}), 0);
    BOOST_CHECK_EQUAL(job(melts[0].id)->error, "Waiting for the wallet to be unlocked");
    BOOST_CHECK_EQUAL(job(melts[0].id)->tried, height);
    BOOST_REQUIRE(wallet->Unlock(passphrase));

    // The first coin, unlocked and spent by another payment: its step is dropped when it comes.
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->UnlockCoin(melts[0].coin));
    }
    CCoinControl coin_control;
    coin_control.m_allow_other_inputs = false;
    coin_control.Select(melts[0].coin);
    const auto spend{CreateTransaction(*wallet, {CRecipient{PKHash{coinbaseKey.GetPubKey()}, COIN, /*fSubtractFeeFromAmount=*/false}}, /*change_pos=*/std::nullopt, coin_control)};
    BOOST_REQUIRE(spend);
    BOOST_REQUIRE_EQUAL(spend->tx->vin.size(), 1U);
    BOOST_REQUIRE(spend->tx->vin[0].prevout == melts[0].coin);
    wallet->CommitTransaction(spend->tx);
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->IsSpent(melts[0].coin)));
    height = NextBlock(*this, *wallet, coinbaseKey);
    int asked{0};
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, [&] { return ++asked > 1; }), 0);
    BOOST_CHECK(!job(melts[0].id));
    // (The second waited for the next call.)
    BOOST_CHECK_EQUAL(job(melts[1].id)->tried, height - 1);

    // The second, cancelled while it was being made (when the wallet takes in its transaction): it went;
    // the step is not kept, and its coin is no longer locked.
    std::optional<Txid> made;
    btcsignals::scoped_connection cancel{wallet->NotifyTransactionChanged.connect([&](const Txid& txid, ChangeType status) {
        if (status != CT_NEW) return;
        made = txid;
        BOOST_CHECK(CancelMeltCast(*wallet, melts[1].id));
    })};
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, {}), 1);
    cancel.disconnect();
    BOOST_REQUIRE(made);
    BOOST_CHECK(!job(melts[1].id));
    LOCK(wallet->cs_wallet);
    BOOST_CHECK(!wallet->IsLockedCoin(melts[1].coin));
    const CWalletTx* wtx{wallet->GetWalletTx(*made)};
    BOOST_REQUIRE(wtx);
    BOOST_CHECK(!wtx->isAbandoned());
    BOOST_REQUIRE_EQUAL(wtx->GetTx()->vin.size(), 1U);
    BOOST_CHECK(wtx->GetTx()->vin[0].prevout == melts[1].coin);
    BOOST_CHECK(zside::HasShieldedMarker(*wtx->GetTx()));
    BOOST_CHECK(wallet->m_shielded.jobs.empty());
}

BOOST_FIXTURE_TEST_CASE(meltcast_failed_step_tried_again, ListCoinsTestingSetup)
{
    // zSide: a step that cannot go keeps the reason, and is tried again: a cast at its bill's next
    // window, at most once a block.
    using namespace meltcast;
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
    }
    // A note of 1 for the account, waiting for a block: a cast may count on it, and cannot spend it yet.
    const CTransactionRef paid{MakeTransactionRef(ShieldingTxTo(ShieldedKeyOf(8).first, *WITH_LOCK(wallet->cs_wallet, return ShieldedAddress(*wallet, 0)), COIN))};
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->AddToWallet(paid, TxStateInMempool{})));
    const auto cast{ScheduleCast(*wallet, BillAmount(1), /*spread=*/0)};
    BOOST_REQUIRE(std::holds_alternative<std::vector<MeltCastJob>>(cast));
    BOOST_REQUIRE_EQUAL(std::get<std::vector<MeltCastJob>>(cast).size(), 1U);
    const MeltCastJob bill{std::get<std::vector<MeltCastJob>>(cast)[0]};
    BOOST_REQUIRE_EQUAL(bill.bill, 1);
    const auto job{[&] { return WITH_LOCK(wallet->cs_wallet, return wallet->m_shielded.jobs.at(bill.id)); }};
    // Up to its window (on regtest a period of 12 blocks): nothing due.
    int height{WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHeight())};
    while (height + 1 < bill.due) {
        height = NextBlock(*this, *wallet, coinbaseKey);
        BOOST_CHECK_EQUAL(RunMeltCast(*wallet, [] { BOOST_ERROR("no step is due"); return true; }), 0);
    }
    BOOST_CHECK_EQUAL(job().tried, -1);
    BOOST_CHECK(job().error.empty());
    // In its window: tried, and the reason kept.
    height = NextBlock(*this, *wallet, coinbaseKey);
    BOOST_REQUIRE(InCastWindow(bill.bill, height));
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, {}), 0);
    int tries{1};
    BOOST_CHECK_EQUAL(job().tried, height);
    BOOST_CHECK_MESSAGE(job().error.starts_with("Not enough shielded funds in blocks yet: 0.00 available, 1.00 waiting for a block"), job().error);
    BOOST_CHECK(!job().Sent());
    // Once a block.
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, [] { BOOST_ERROR("no step is due"); return true; }), 0);
    // Out of its window: not tried, the reason kept.
    while (InCastWindow(bill.bill, height + 1)) {
        height = NextBlock(*this, *wallet, coinbaseKey);
        BOOST_CHECK_EQUAL(RunMeltCast(*wallet, {}), 0);
        BOOST_CHECK_EQUAL(job().tried, height);
        ++tries;
    }
    height = NextBlock(*this, *wallet, coinbaseKey);
    BOOST_CHECK_EQUAL(RunMeltCast(*wallet, [] { BOOST_ERROR("no step is due"); return true; }), 0);
    BOOST_CHECK_EQUAL(job().tried, height - 1);
    BOOST_CHECK(!job().error.empty());
    // Each try took a new address of the wallet for the bill.
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return wallet->ListAddrBookAddresses(CWallet::AddrBookFilter{.m_op_label = std::string{"cast"}}).size()), static_cast<size_t>(tries));
}

BOOST_FIXTURE_TEST_CASE(shielded_keys_of_any_descriptor_of_the_wallet, ListCoinsTestingSetup)
{
    // zSide: the account's keys are those of the descriptor it was made from, active or not: encryptwallet
    // makes new descriptors active, and one may be imported active. With none of the wallet's, it cannot spend.
    ShieldedRecipient recipient;
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
        const OrchardFvk fvk{*wallet->m_shielded.fvk};
        const OrchardFvk legacy{*wallet->m_shielded.legacy_fvk};
        const auto fvk_of{[](const std::optional<ShieldedKey>& sk) {
            BOOST_REQUIRE(sk);
            OrchardFvk out;
            BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk->data(), out.data()), ZO_OK);
            return out;
        }};
        BOOST_CHECK(fvk_of(AccountSpendingKey(*wallet, KEY_THIS_CHAIN, fvk)) == fvk);
        BOOST_CHECK(fvk_of(AccountSpendingKey(*wallet, KEY_ANY_CHAIN, legacy)) == legacy);
        // New descriptors, of a new seed, active.
        wallet->SetupDescriptorScriptPubKeyMans();
        BOOST_CHECK(fvk_of(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN)) != fvk);
        BOOST_CHECK(fvk_of(AccountSpendingKey(*wallet, KEY_THIS_CHAIN, fvk)) == fvk);
        BOOST_CHECK(fvk_of(AccountSpendingKey(*wallet, KEY_ANY_CHAIN, legacy)) == legacy);
        // Of the other version, or of no version: none.
        BOOST_CHECK(!AccountSpendingKey(*wallet, KEY_ANY_CHAIN, fvk));
        BOOST_CHECK(!AccountSpendingKey(*wallet, 3, fvk));
        // A viewing key no descriptor of the wallet makes: none, and a payment says so.
        const OrchardFvk foreign{ShieldedKeyOf(9).first};
        BOOST_CHECK(!AccountSpendingKey(*wallet, KEY_THIS_CHAIN, foreign));
        wallet->m_shielded.fvk = foreign;
        recipient.script = GetScriptForRawPubKey(coinbaseKey.GetPubKey());
        recipient.amount = COIN;
    }
    const auto result{SendShielded(*wallet, {recipient})};
    BOOST_CHECK_EQUAL(std::get<std::string>(result), "No descriptor of the wallet holds the keys of its shielded account: import the one the account was made from to spend its notes");
}

BOOST_FIXTURE_TEST_CASE(shielded_account_records_on_load, TestingSetup)
{
    // zSide: the account a wallet loads: its viewing key, and the version of its keys (none written: 1,
    // set up before version 2); the key of version 1 it also watches, with keys of version 2 only. Keys
    // of a version this release does not know: the wallet is too new.
    const OrchardFvk fvk{ShieldedKeyOf(10).first};
    const OrchardFvk legacy{ShieldedKeyOf(11).first};
    const auto load{[&](std::optional<int> version) {
        std::unique_ptr<WalletDatabase> database{CreateMockableWalletDatabase()};
        {
            WalletBatch batch{*database};
            BOOST_REQUIRE(batch.WriteOrchardViewingKey(std::vector<unsigned char>(fvk.begin(), fvk.end())));
            BOOST_REQUIRE(batch.WriteOrchardLegacyViewingKey(std::vector<unsigned char>(legacy.begin(), legacy.end())));
            if (version) BOOST_REQUIRE(batch.WriteOrchardKeyVersion(*version));
        }
        auto wallet{std::make_shared<CWallet>(m_node.chain.get(), "", std::move(database))};
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        const DBErrors result{wallet->PopulateWalletFromDB(error, warnings)};
        return std::make_pair(result, wallet);
    }};
    for (const std::optional<int> version : {std::optional<int>{}, std::optional<int>{KEY_ANY_CHAIN}}) {
        const auto [result, wallet]{load(version)};
        BOOST_CHECK_EQUAL(result, DBErrors::LOAD_OK);
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->m_shielded.fvk == fvk);
        BOOST_CHECK_EQUAL(wallet->m_shielded.key_version, KEY_ANY_CHAIN);
        BOOST_CHECK(!wallet->m_shielded.legacy_fvk);
    }
    {
        const auto [result, wallet]{load(KEY_THIS_CHAIN)};
        BOOST_CHECK_EQUAL(result, DBErrors::LOAD_OK);
        LOCK(wallet->cs_wallet);
        BOOST_CHECK_EQUAL(wallet->m_shielded.key_version, KEY_THIS_CHAIN);
        BOOST_CHECK(wallet->m_shielded.legacy_fvk == legacy);
    }
    for (const int version : {0, 3, -1}) {
        BOOST_CHECK_EQUAL(load(version).first, DBErrors::TOO_NEW);
    }
}

/** zSide: the viewing key of a spending key. */
static OrchardFvk FvkOf(const std::optional<ShieldedKey>& sk)
{
    BOOST_REQUIRE(sk);
    OrchardFvk fvk;
    BOOST_REQUIRE_EQUAL(zo_full_viewing_key(sk->data(), fvk.data()), ZO_OK);
    return fvk;
}

/** zSide: the default address of a viewing key. */
static OrchardAddress AddressOf(const OrchardFvk& fvk)
{
    OrchardAddress address;
    BOOST_REQUIRE_EQUAL(zo_address(fvk.data(), 0, false, address.data()), ZO_OK);
    return address;
}

BOOST_FIXTURE_TEST_CASE(shielded_account_of_another_chain_is_retired, ListCoinsTestingSetup)
{
    // zSide: an account whose keys were made for another chain (a wallet kept across a reset of the
    // network, or loaded with another slot) cannot be spent from here with the keys of this chain. It is
    // retired: watched still, its notes spent with its keys made again for its chain, nothing paid into
    // it again; a new account of this chain is set up.
    AddTx(CRecipient{PKHash{coinbaseKey.GetPubKey()}, 10 * COIN, /*fSubtractFeeFromAmount=*/false});
    const KeyChain here{ThisKeyChain()};
    const KeyChain there{here.genesis, here.slot + 1};
    OrchardFvk ours, theirs;
    OrchardAddress old_address;
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
        BOOST_CHECK(wallet->m_shielded.chain == here);
        BOOST_CHECK(ShieldedAccountOfThisChain(wallet->m_shielded));
        ours = *wallet->m_shielded.fvk;
        // The account as the wallet set it up on the other chain.
        theirs = FvkOf(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN, there.genesis, there.slot));
        BOOST_REQUIRE(theirs != ours);
        wallet->m_shielded.fvk = theirs;
        wallet->m_shielded.chain = there;
        BOOST_CHECK(!ShieldedAccountOfThisChain(wallet->m_shielded));
        old_address = AddressOf(theirs);
        // Paid there (or here, before the fix, from its old address).
        const CTransactionRef paid{MakeTransactionRef(ShieldingTxTo(ShieldedKeyOf(4).first, old_address, COIN))};
        BOOST_REQUIRE(wallet->AddToWallet(paid, TxStateInMempool{}));
        BOOST_REQUIRE_EQUAL(ShieldedNotes(*wallet).size(), 1U);
        BOOST_CHECK_EQUAL(ShieldedNotes(*wallet)[0].note->retired, -1);

        // Set up: retired, a new account of this chain.
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
        const ShieldedAccount& account{wallet->m_shielded};
        BOOST_CHECK(account.fvk == ours);
        BOOST_CHECK(account.chain == here);
        BOOST_REQUIRE_EQUAL(account.retired.size(), 2U);
        BOOST_CHECK(account.retired[0].fvk == theirs);
        BOOST_CHECK(account.retired[0].chain == there);
        BOOST_CHECK(account.retired[0].Spendable());
        // Its key of version 1, the same as the new account's: watched once, as the account's.
        BOOST_CHECK(account.retired[1].fvk == *account.legacy_fvk);
        BOOST_CHECK_EQUAL(ShieldedViewingKeys(account).size(), 3U);
        // Its note is still the wallet's, spent with its keys made for its chain.
        const auto notes{ShieldedNotes(*wallet)};
        BOOST_REQUIRE_EQUAL(notes.size(), 1U);
        BOOST_CHECK_EQUAL(notes[0].note->retired, 0);
        BOOST_CHECK(ShieldedNoteSpendable(account, *notes[0].note));
        BOOST_CHECK_EQUAL(GetShieldedBalance(*wallet).pending, COIN);
        BOOST_CHECK_EQUAL(GetShieldedBalance(*wallet).watch_only, 0);
        BOOST_CHECK(FvkOf(AccountSpendingKey(*wallet, KEY_THIS_CHAIN, theirs, there)) == theirs);
        BOOST_CHECK(!AccountSpendingKey(*wallet, KEY_THIS_CHAIN, theirs));
        // Its address is its own, and not to be paid any more; the new one is.
        BOOST_CHECK_EQUAL(ShieldedAddressScope(account, old_address), 1);
        BOOST_CHECK(*ShieldedAddress(*wallet, 0) != old_address);
        BOOST_CHECK(UnspendableOwnAddress(*wallet, old_address)->starts_with("The address is of a shielded account the wallet set up for another chain"));
        BOOST_CHECK(!UnspendableOwnAddress(*wallet, *ShieldedAddress(*wallet, 0)));
        BOOST_CHECK(!UnspendableOwnAddress(*wallet, ShieldedKeyOf(4).second));
        // Again: nothing more.
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
        BOOST_CHECK_EQUAL(wallet->m_shielded.retired.size(), 2U);
    }
    // Shielding into it is refused: shieldcoins, the Qt Shield button (shieldcoins) and a melt (ShieldCoin).
    const auto shielded{ShieldCoins(*wallet, COIN, old_address, *MemoFromText(""))};
    BOOST_CHECK(std::get<std::string>(shielded).starts_with("The address is of a shielded account the wallet set up for another chain"));
    const COutPoint coin{WITH_LOCK(wallet->cs_wallet, return AvailableCoins(*wallet).All().at(0).outpoint)};
    const auto melted{ShieldCoin(*wallet, coin, old_address)};
    BOOST_CHECK(std::get<std::string>(melted).starts_with("The address is of a shielded account the wallet set up for another chain"));
    // Paying it from the account too.
    ShieldedRecipient to_old;
    to_old.shielded = old_address;
    to_old.amount = COIN / 10;
    to_old.memo = *MemoFromText("");
    BOOST_CHECK(std::get<std::string>(SendShielded(*wallet, {to_old})).starts_with("The address is of a shielded account the wallet set up for another chain"));
}

BOOST_FIXTURE_TEST_CASE(shielded_account_without_its_chain_checked_once_unlocked, ListCoinsTestingSetup)
{
    // zSide: an account set up before the chain was kept with its keys is checked once the wallet can
    // make its keys: one of this chain gets its chain kept; one of another chain is retired, its chain
    // found (another slot) and its notes spent; one whose chain is not found is retired watch-only.
    const KeyChain here{ThisKeyChain()};
    const auto unchecked{[&](const OrchardFvk& fvk) EXCLUSIVE_LOCKS_REQUIRED(wallet->cs_wallet) {
        wallet->m_shielded.fvk = fvk;
        wallet->m_shielded.key_version = KEY_THIS_CHAIN;
        wallet->m_shielded.chain.reset();
        wallet->m_shielded.retired.clear();
        BOOST_CHECK(!ShieldedAccountOfThisChain(wallet->m_shielded));
    }};
    LOCK(wallet->cs_wallet);
    const OrchardFvk ours{FvkOf(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN))};

    // Of this chain: kept, and its chain written.
    unchecked(ours);
    BOOST_REQUIRE(SetupShieldedAccount(*wallet));
    BOOST_CHECK(wallet->m_shielded.fvk == ours);
    BOOST_CHECK(wallet->m_shielded.chain == here);
    BOOST_CHECK(wallet->m_shielded.retired.empty());
    BOOST_CHECK(ShieldedAccountOfThisChain(wallet->m_shielded));

    // Of another slot: retired, its chain found.
    const KeyChain there{here.genesis, (here.slot + 7) % 256};
    const OrchardFvk other_slot{FvkOf(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN, there.genesis, there.slot))};
    unchecked(other_slot);
    BOOST_CHECK(FindKeyChain(*wallet, other_slot) == there);
    BOOST_REQUIRE(wallet->AddToWallet(MakeTransactionRef(ShieldingTxTo(ShieldedKeyOf(4).first, AddressOf(other_slot), COIN)), TxStateInMempool{}));
    BOOST_REQUIRE(SetupShieldedAccount(*wallet));
    BOOST_CHECK(wallet->m_shielded.fvk == ours);
    BOOST_REQUIRE_GE(wallet->m_shielded.retired.size(), 1U);
    BOOST_CHECK(wallet->m_shielded.retired[0].fvk == other_slot);
    BOOST_CHECK(wallet->m_shielded.retired[0].chain == there);
    BOOST_CHECK_EQUAL(GetShieldedBalance(*wallet).pending, COIN);
    BOOST_CHECK_EQUAL(GetShieldedBalance(*wallet).watch_only, 0);

    // Of no chain the wallet finds (or of a descriptor it lacks): retired, watched only.
    const auto [foreign, foreign_address]{ShieldedKeyOf(12)};
    unchecked(foreign);
    BOOST_CHECK(!FindKeyChain(*wallet, foreign));
    BOOST_REQUIRE(wallet->AddToWallet(MakeTransactionRef(ShieldingTxTo(ShieldedKeyOf(4).first, foreign_address, 2 * COIN)), TxStateInMempool{}));
    BOOST_REQUIRE(SetupShieldedAccount(*wallet));
    BOOST_CHECK(wallet->m_shielded.fvk == ours);
    BOOST_REQUIRE_GE(wallet->m_shielded.retired.size(), 1U);
    BOOST_CHECK(wallet->m_shielded.retired[0].fvk == foreign);
    BOOST_CHECK(!wallet->m_shielded.retired[0].chain);
    BOOST_CHECK(!wallet->m_shielded.retired[0].Spendable());
    const ShieldedBalance balance{GetShieldedBalance(*wallet)};
    BOOST_CHECK_EQUAL(balance.watch_only, 2 * COIN);
    BOOST_CHECK_EQUAL(balance.notes, 0);
    BOOST_CHECK(UnspendableOwnAddress(*wallet, foreign_address));
}

BOOST_FIXTURE_TEST_CASE(shielded_account_checked_when_unlocked, ListCoinsTestingSetup)
{
    // zSide: a locked wallet cannot tell whether an account set up before the chain was kept is of this
    // chain: it hands out none of its addresses until unlocked; one known to be of another chain is
    // retired at once, without the keys, and the new one set up once unlocked.
    const SecureString passphrase{"check"};
    OrchardFvk theirs;
    {
        LOCK(wallet->cs_wallet);
        theirs = FvkOf(ShieldedSpendingKey(*wallet, KEY_THIS_CHAIN, ThisKeyChain().genesis, ThisKeyChain().slot + 3));
    }
    BOOST_REQUIRE(wallet->EncryptWallet(passphrase));
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(wallet->IsLocked());
        wallet->m_shielded = ShieldedAccount{};
        wallet->m_shielded.fvk = theirs;
        wallet->m_shielded.key_version = KEY_THIS_CHAIN;
        BOOST_CHECK(!SetupShieldedAccount(*wallet));
        BOOST_CHECK(wallet->m_shielded.fvk == theirs);
        BOOST_CHECK(!ShieldedAccountOfThisChain(wallet->m_shielded));
    }
    // Unlocking checks it (the descriptor that made it is no longer active: encryptwallet made new ones).
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    {
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(ShieldedAccountOfThisChain(wallet->m_shielded));
        BOOST_CHECK(wallet->m_shielded.fvk != theirs);
        BOOST_REQUIRE(!wallet->m_shielded.retired.empty());
        BOOST_CHECK(wallet->m_shielded.retired[0].fvk == theirs);
        BOOST_CHECK(wallet->m_shielded.retired[0].chain == (KeyChain{ThisKeyChain().genesis, ThisKeyChain().slot + 3}));
    }
    BOOST_REQUIRE(wallet->Lock());
    {
        // Of a chain known to be another: retired while locked; no account until unlocked.
        LOCK(wallet->cs_wallet);
        const OrchardFvk current{*wallet->m_shielded.fvk};
        wallet->m_shielded.chain = KeyChain{uint256::ONE, 0};
        BOOST_CHECK(!SetupShieldedAccount(*wallet));
        BOOST_CHECK(!wallet->m_shielded.fvk);
        BOOST_CHECK(std::any_of(wallet->m_shielded.retired.begin(), wallet->m_shielded.retired.end(), [&](const RetiredKey& r) { return r.fvk == current && r.chain == KeyChain{uint256::ONE, 0}; }));
    }
    BOOST_REQUIRE(wallet->Unlock(passphrase));
    LOCK(wallet->cs_wallet);
    BOOST_CHECK(ShieldedAccountOfThisChain(wallet->m_shielded));
}

BOOST_FIXTURE_TEST_CASE(shielded_account_not_paid_without_its_keys, ListCoinsTestingSetup)
{
    // zSide: the wallet does not shield into an address of its account whose keys none of its descriptors make.
    AddTx(CRecipient{PKHash{coinbaseKey.GetPubKey()}, 10 * COIN, /*fSubtractFeeFromAmount=*/false});
    const auto [foreign, foreign_address]{ShieldedKeyOf(13)};
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
        BOOST_CHECK(!UnspendableOwnAddress(*wallet, *ShieldedAddress(*wallet, 0)));
        wallet->m_shielded.fvk = foreign;
        BOOST_CHECK(UnspendableOwnAddress(*wallet, foreign_address));
    }
    const std::string error{"No descriptor of the wallet holds the keys of this address of its shielded account: what it is paid could not be spent"};
    BOOST_CHECK_EQUAL(std::get<std::string>(ShieldCoins(*wallet, COIN, foreign_address, *MemoFromText(""))), error);
    const COutPoint coin{WITH_LOCK(wallet->cs_wallet, return AvailableCoins(*wallet).All().at(0).outpoint)};
    BOOST_CHECK_EQUAL(std::get<std::string>(ShieldCoin(*wallet, coin, foreign_address)), error);
    // Another's address is paid as before (only the wallet's own unspendable keys are refused).
    LOCK(wallet->cs_wallet);
    BOOST_CHECK(!UnspendableOwnAddress(*wallet, ShieldedKeyOf(14).second));
}

BOOST_FIXTURE_TEST_CASE(shielded_account_chain_records_on_load, TestingSetup)
{
    // zSide: the chain of the account's keys and the retired keys are kept; an account kept for another
    // chain is retired on load, without its keys.
    const OrchardFvk fvk{ShieldedKeyOf(15).first};
    const OrchardFvk old{ShieldedKeyOf(16).first};
    const KeyChain here{ThisKeyChain()};
    const auto load{[&](const KeyChain& chain) {
        std::unique_ptr<WalletDatabase> database{CreateMockableWalletDatabase()};
        {
            WalletBatch batch{*database};
            BOOST_REQUIRE(batch.WriteOrchardViewingKey(std::vector<unsigned char>(fvk.begin(), fvk.end())));
            BOOST_REQUIRE(batch.WriteOrchardKeyVersion(KEY_THIS_CHAIN));
            BOOST_REQUIRE(batch.WriteOrchardKeyChain(chain.genesis, chain.slot));
            BOOST_REQUIRE(batch.WriteOrchardRetired(std::vector<unsigned char>(old.begin(), old.end()), KEY_THIS_CHAIN, std::make_pair(uint256::ONE, uint32_t{9})));
        }
        auto wallet{std::make_shared<CWallet>(m_node.chain.get(), "", std::move(database))};
        bilingual_str error;
        std::vector<bilingual_str> warnings;
        BOOST_CHECK_EQUAL(wallet->PopulateWalletFromDB(error, warnings), DBErrors::LOAD_OK);
        return wallet;
    }};
    {
        const auto wallet{load(here)};
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(wallet->m_shielded.fvk == fvk);
        BOOST_CHECK(wallet->m_shielded.chain == here);
        BOOST_REQUIRE_EQUAL(wallet->m_shielded.retired.size(), 1U);
        BOOST_CHECK(wallet->m_shielded.retired[0].fvk == old);
        BOOST_CHECK(wallet->m_shielded.retired[0].chain == (KeyChain{uint256::ONE, 9}));
        BOOST_CHECK(ShieldedAccountOfThisChain(wallet->m_shielded));
    }
    {
        const auto wallet{load({uint256::ONE, here.slot})};
        LOCK(wallet->cs_wallet);
        BOOST_CHECK(!ShieldedAccountOfThisChain(wallet->m_shielded));
        // No keys: retired, and no new account yet.
        BOOST_CHECK(!SetupShieldedAccount(*wallet));
        BOOST_CHECK(!wallet->m_shielded.fvk);
        BOOST_REQUIRE_EQUAL(wallet->m_shielded.retired.size(), 2U);
        BOOST_CHECK(wallet->m_shielded.retired[1].fvk == fvk);
        BOOST_CHECK(wallet->m_shielded.retired[1].chain == (KeyChain{uint256::ONE, here.slot}));
    }
}

BOOST_AUTO_TEST_CASE(shielded_keys_of_the_test_network_before_its_reset)
{
    // zSide: the first block of a sidechain, made again from its text and time: that of the test network
    // now, and the one it had before its reset of 2026-10-08, whose keys a wallet kept across it has.
    const auto test{CreateChainParams(ArgsManager{}, ChainType::TESTNET)};
    BOOST_CHECK(SidechainGenesisHash("zSide testnet", 1791513282) == test->GetConsensus().hashGenesisBlock);
    BOOST_CHECK(SidechainGenesisHash("zSide testnet", 1791302400) == uint256{"44cf5414f37f1adda63098ed74fbb189fe7a8224bea6a521d407a88be4e129db"});
    const auto signet{CreateChainParams(ArgsManager{}, ChainType::SIGNET)};
    BOOST_CHECK(SidechainGenesisHash("zSide signet", 1791302400) == signet->GetConsensus().hashGenesisBlock);
}

BOOST_AUTO_TEST_CASE(shielded_addresses_and_memos)
{
    // zSide: a shielded address is bech32m with a prefix of its network; one of another network, another
    // encoding, another length or no valid address at all is none.
    const OrchardAddress address{ShieldedKeyOf(6).second};
    const auto encode_on{[&](ChainType chain) {
        SelectParams(chain);
        return EncodeShieldedAddress(address);
    }};
    const std::string main{encode_on(ChainType::MAIN)};
    const std::string test{encode_on(ChainType::TESTNET)};
    const std::string signet{encode_on(ChainType::SIGNET)};
    const std::string regtest{encode_on(ChainType::REGTEST)};
    BOOST_CHECK(main.starts_with("zo1"));
    BOOST_CHECK(test.starts_with("tzo1"));
    BOOST_CHECK_EQUAL(signet, test);
    BOOST_CHECK(regtest.starts_with("rzo1"));
    for (const auto& [chain, text] : std::vector<std::pair<ChainType, std::string>>{{ChainType::MAIN, main}, {ChainType::TESTNET, test}, {ChainType::REGTEST, regtest}}) {
        SelectParams(chain);
        BOOST_CHECK(DecodeShieldedAddress(text) == address);
        for (const std::string& other : {main, test, regtest}) {
            if (other != text) BOOST_CHECK(!DecodeShieldedAddress(other));
        }
    }
    SelectParams(ChainType::MAIN);
    std::vector<uint8_t> data;
    ConvertBits<8, 5, true>([&](uint8_t c) { data.push_back(c); }, address.begin(), address.end());
    BOOST_CHECK(!DecodeShieldedAddress(bech32::Encode(bech32::Encoding::BECH32, "zo", data)));
    std::vector<uint8_t> shorter;
    ConvertBits<8, 5, true>([&](uint8_t c) { shorter.push_back(c); }, address.begin(), address.end() - 1);
    BOOST_CHECK(!DecodeShieldedAddress(bech32::Encode(bech32::Encoding::BECH32M, "zo", shorter)));
    // 43 bytes that are no address: the transmission key is no point of the curve.
    OrchardAddress invalid{address};
    std::fill(invalid.begin() + 11, invalid.end(), 0xff);
    BOOST_CHECK(!zo_address_valid(invalid.data()));
    BOOST_CHECK(!DecodeShieldedAddress(EncodeShieldedAddress(invalid)));
    BOOST_CHECK(!DecodeShieldedAddress(main.substr(0, main.size() - 1) + (main.back() == 'q' ? 'p' : 'q')));
    BOOST_CHECK(!DecodeShieldedAddress(""));

    // Memos: none (0xF6, then zeros), text up to 512 bytes, nothing else.
    const OrchardMemo none{*MemoFromText("")};
    BOOST_CHECK_EQUAL(none[0], 0xF6);
    BOOST_CHECK(std::all_of(none.begin() + 1, none.end(), [](uint8_t b) { return b == 0; }));
    BOOST_CHECK_EQUAL(MemoText(none), "");
    BOOST_CHECK_EQUAL(MemoText(*MemoFromText("thanks")), "thanks");
    const std::string longest(512, 'x');
    BOOST_CHECK_EQUAL(MemoText(*MemoFromText(longest)), longest);
    BOOST_CHECK(!MemoFromText(longest + "x"));
    BOOST_CHECK(!MemoFromText("\xF5 not text"));
    BOOST_CHECK(MemoFromText("\xF4"));
    OrchardMemo arbitrary{};
    arbitrary[0] = 0xFF;
    BOOST_CHECK_EQUAL(MemoText(arbitrary), "");
}

BOOST_AUTO_TEST_CASE(meltcast_windows_and_bills)
{
    // zSide: on the main network a bill's window opens once a day (144 blocks), at bill * 7 into the
    // period, for WINDOW blocks.
    using namespace meltcast;
    BOOST_CHECK_EQUAL(CastPeriod(), 144);
    for (int h{0}; h < WINDOW; ++h) BOOST_CHECK(InCastWindow(0, h));
    BOOST_CHECK(!InCastWindow(0, WINDOW));
    BOOST_CHECK(InCastWindow(0, 144));
    // Below the offset: the window of the period before (heights are never negative, the arithmetic is).
    BOOST_CHECK(!InCastWindow(3, 0));
    BOOST_CHECK(!InCastWindow(0, -1));
    BOOST_CHECK(InCastWindow(0, -144));
    BOOST_CHECK(InCastWindow(3, 21) && InCastWindow(3, 23) && !InCastWindow(3, 24) && !InCastWindow(3, 20));
    BOOST_CHECK_EQUAL(NextCastWindow(3, 0), 21);
    BOOST_CHECK_EQUAL(NextCastWindow(3, 22), 22);
    BOOST_CHECK_EQUAL(NextCastWindow(3, 24), 21 + 144);
    BOOST_CHECK_EQUAL(NextCastWindow(20, 143), 140 + 144);
    // Every bill, from every height of a period: the next window is the first height in one.
    for (int bill{0}; bill < NUM_BILLS; ++bill) {
        for (int from{1000}; from < 1000 + 144; ++from) {
            const int next{NextCastWindow(bill, from)};
            BOOST_CHECK(next >= from && next - from < 144);
            BOOST_CHECK(InCastWindow(bill, next));
            for (int h{from}; h < next; ++h) BOOST_CHECK(!InCastWindow(bill, h));
        }
    }

    // The binary digits of an amount in bills, the largest first; what is less than a bill stays.
    BOOST_CHECK_EQUAL(BillAmount(0), BILL_UNIT);
    BOOST_CHECK_EQUAL(BillAmount(20), BILL_UNIT << 20);
    BOOST_CHECK(Bills(0).empty());
    BOOST_CHECK(Bills(BILL_UNIT - 1).empty());
    BOOST_CHECK(Bills(5'990'000) == std::vector<int>({2, 0}));
    BOOST_CHECK(Bills(BILL_UNIT * 64) == std::vector<int>({6}));
    std::vector<int> all;
    for (int k{NUM_BILLS - 1}; k >= 0; --k) all.push_back(k);
    BOOST_CHECK(Bills(BillAmount(NUM_BILLS) - 1) == all);

    // A step of the plan as the wallet keeps it: all but the height it last tried at.
    MeltCastJob job;
    job.id = 7;
    job.kind = MeltCastJob::CAST;
    job.coin = COutPoint{Txid::FromUint256(uint256::ONE), 3};
    job.amount = BillAmount(4);
    job.bill = 4;
    job.due = 1234;
    job.created = 99;
    job.error = "Waiting";
    job.tried = 1230;
    BOOST_CHECK(!job.Sent());
    job.txid = uint256::ONE;
    BOOST_CHECK(job.Sent());
    DataStream s{};
    s << job;
    MeltCastJob read;
    s >> read;
    BOOST_CHECK(s.empty());
    BOOST_CHECK_EQUAL(read.id, 7U);
    BOOST_CHECK_EQUAL(read.kind, MeltCastJob::CAST);
    BOOST_CHECK(read.coin == job.coin);
    BOOST_CHECK_EQUAL(read.amount, job.amount);
    BOOST_CHECK_EQUAL(read.bill, 4);
    BOOST_CHECK_EQUAL(read.due, 1234);
    BOOST_CHECK_EQUAL(read.created, 99);
    BOOST_CHECK(read.txid == uint256::ONE);
    BOOST_CHECK_EQUAL(read.error, "Waiting");
    BOOST_CHECK_EQUAL(read.tried, -1);
}

BOOST_FIXTURE_TEST_CASE(meltcast_plan_refusals_and_cancelling, ListCoinsTestingSetup)
{
    // zSide: what a melt or a cast refuses to plan, what the plan holds back, and dropping steps.
    using namespace meltcast;
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleMelt(*wallet, -1)), "spread must not be negative");
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleCast(*wallet, BILL_UNIT, -1)), "spread must not be negative");
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleCast(*wallet, BILL_UNIT, 0)), "The wallet has no shielded account yet");
    {
        LOCK(wallet->cs_wallet);
        BOOST_REQUIRE(SetupShieldedAccount(*wallet));
    }
    // (Up to the largest amount below the 22nd bill: what is less than the smallest bill stays shielded.)
    const std::string range{"The amount must be between 0.01 and 20971.51999999"};
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleCast(*wallet, 0, 0)), range);
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleCast(*wallet, -BILL_UNIT, 0)), range);
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleCast(*wallet, BillAmount(NUM_BILLS), 0)), range);
    BOOST_CHECK_EQUAL(std::get<std::string>(ScheduleCast(*wallet, BILL_UNIT - 1, 0)), "The smallest bill is 0.01");
    const std::string empty{std::get<std::string>(ScheduleCast(*wallet, BILL_UNIT, 0))};
    BOOST_CHECK_MESSAGE(empty.starts_with("Not enough in the shielded account: ") && empty.ends_with(", 0.00 free (casts planned already hold back 0.00)"), empty);

    // A note of 1 for the account, waiting for a block: a cast may count on it.
    const CTransactionRef paid{MakeTransactionRef(ShieldingTxTo(ShieldedKeyOf(5).first, *WITH_LOCK(wallet->cs_wallet, return ShieldedAddress(*wallet, 0)), COIN))};
    BOOST_REQUIRE(WITH_LOCK(wallet->cs_wallet, return wallet->AddToWallet(paid, TxStateInMempool{})));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(wallet->cs_wallet, return GetShieldedBalance(*wallet).pending), COIN);
    const auto cast{ScheduleCast(*wallet, 5'990'000, 0)};
    BOOST_REQUIRE(std::holds_alternative<std::vector<MeltCastJob>>(cast));
    const auto bills{std::get<std::vector<MeltCastJob>>(cast)};
    BOOST_REQUIRE_EQUAL(bills.size(), 2U);
    const int tip{WITH_LOCK(wallet->cs_wallet, return wallet->GetLastBlockHeight())};
    for (const MeltCastJob& job : bills) {
        BOOST_CHECK_EQUAL(job.kind, MeltCastJob::CAST);
        BOOST_CHECK_EQUAL(job.amount, BillAmount(job.bill));
        BOOST_CHECK(InCastWindow(job.bill, job.due));
        BOOST_CHECK_EQUAL(job.due, NextCastWindow(job.bill, tip + 1));
    }
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return CastReserved(*wallet)), 5 * BILL_UNIT);
    // More than is free with what the plan holds back.
    const std::string held{std::get<std::string>(ScheduleCast(*wallet, 99 * BILL_UNIT, 0))};
    BOOST_CHECK_MESSAGE(held.ends_with("(casts planned already hold back 0.05)"), held);

    // Melt: every coin in a block, each locked until it goes.
    const auto melt{ScheduleMelt(*wallet, 0)};
    BOOST_REQUIRE(std::holds_alternative<std::vector<MeltCastJob>>(melt));
    const auto melts{std::get<std::vector<MeltCastJob>>(melt)};
    BOOST_REQUIRE(!melts.empty());
    for (const MeltCastJob& job : melts) {
        BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(job.coin)));
        BOOST_CHECK_EQUAL(job.due, tip + 1);
    }
    // Nothing left to melt: all locked.
    BOOST_CHECK(std::get<std::string>(ScheduleMelt(*wallet, 0)).starts_with("No transparent coin to melt"));
    // Melts hold nothing of the shielded account back.
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return CastReserved(*wallet)), 5 * BILL_UNIT);

    // Cancelling: a step not there, a melt (its coin unlocked), a cast (no longer held back).
    BOOST_CHECK(!CancelMeltCast(*wallet, 999));
    BOOST_CHECK(CancelMeltCast(*wallet, melts[0].id));
    BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(melts[0].coin)));
    BOOST_CHECK(!CancelMeltCast(*wallet, melts[0].id));
    BOOST_CHECK(CancelMeltCast(*wallet, bills[0].id));
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return CastReserved(*wallet)), 5 * BILL_UNIT - bills[0].amount);
    // A step sent stays, while its transaction may still be mined: in the mempool or a block.
    const auto mark_sent{[&](uint64_t id, const uint256& txid) {
        LOCK(wallet->cs_wallet);
        wallet->m_shielded.jobs.at(id).txid = txid;
    }};
    mark_sent(bills[1].id, paid->GetHash().ToUint256());
    BOOST_CHECK(!CancelMeltCast(*wallet, bills[1].id));
    BOOST_CHECK_EQUAL(WITH_LOCK(wallet->cs_wallet, return CastReserved(*wallet)), 0);
    // Abandoned, it will never be: the step can go.
    {
        LOCK(wallet->cs_wallet);
        wallet->mapWallet.at(paid->GetHash()).m_state = TxStateInactive{/*abandoned=*/true};
    }
    BOOST_CHECK(CancelMeltCast(*wallet, bills[1].id));
    // One whose transaction the wallet no longer has, too.
    if (melts.size() > 1) {
        mark_sent(melts[1].id, uint256::ONE);
        BOOST_CHECK(CancelMeltCast(*wallet, melts[1].id));
        BOOST_CHECK(!WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(melts[1].coin)));
    }
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
