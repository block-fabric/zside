// Copyright (c) 2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <kernel/chainparams.h>

#include <arith_uint256.h>

#include <chainparamsseeds.h>
#include <consensus/amount.h>
#include <consensus/merkle.h>
#include <consensus/params.h>
#include <crypto/hex_base.h>
#include <hash.h>
#include <kernel/messagestartchars.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/verify_flags.h>
#include <uint256.h>
#include <util/chaintype.h>
#include <util/log.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <map>
#include <span>
#include <utility>

using namespace util::hex_literals;

static CBlock CreateGenesisBlock(const char* pszTimestamp, const CScript& genesisOutputScript, uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    CMutableTransaction txNew;
    txNew.version = 1;
    txNew.vin.resize(1);
    txNew.vout.resize(1);
    txNew.vin[0].scriptSig = CScript() << 486604799 << CScriptNum(4) << std::vector<unsigned char>((const unsigned char*)pszTimestamp, (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
    txNew.vout[0].nValue = genesisReward;
    txNew.vout[0].scriptPubKey = genesisOutputScript;

    CBlock genesis;
    genesis.nTime    = nTime;
    genesis.nBits    = nBits;
    genesis.nNonce   = nNonce;
    genesis.nVersion = nVersion;
    genesis.vtx.push_back(MakeTransactionRef(std::move(txNew)));
    genesis.hashPrevBlock.SetNull();
    genesis.hashMerkleRoot = BlockMerkleRoot(genesis);
    return genesis;
}

/**
 * Build the genesis block. Note that the output of its generation
 * transaction cannot be spent since it did not originally exist in the
 * database.
 *
 * CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
 *   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
 *     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
 *     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
 *   vMerkleTree: 4a5e1e
 */
static CBlock CreateGenesisBlock(uint32_t nTime, uint32_t nNonce, uint32_t nBits, int32_t nVersion, const CAmount& genesisReward)
{
    const char* pszTimestamp = "The Times 03/Jan/2009 Chancellor on brink of second bailout for banks";
    const CScript genesisOutputScript = CScript() << "04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f"_hex << OP_CHECKSIG;
    return CreateGenesisBlock(pszTimestamp, genesisOutputScript, nTime, nNonce, nBits, nVersion, genesisReward);
}

void CChainParams::MakeSidechain(const SidechainIdentity& identity)
{
    consensus.sidechain.enabled = true;
    consensus.sidechain.slot = identity.slot;
    // A sidechain has no sidechains of its own.
    consensus.drivechain.max_sidechains = 0;
    // The mainchain makes no rule about how the blocks of a sidechain are signed.
    consensus.signet_blocks = false;
    consensus.signet_challenge.clear();

    // The chain is secured by the work of the mainchain. The proof of work of
    // its own blocks is kept as a formality that takes a couple of hashes, so
    // that the code inherited from the mainchain needs no changes.
    consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
    consensus.fPowNoRetargeting = true;
    consensus.fPowAllowMinDifficultyBlocks = false;
    consensus.enforce_BIP94 = false;
    consensus.nMinimumChainWork = uint256{};
    consensus.defaultAssumeValid = uint256{};

    pchMessageStart = identity.message_start;
    nDefaultPort = identity.default_port;
    bech32_hrp = identity.bech32_hrp;

    // The first block. It creates no coins: all coins come from the mainchain.
    genesis = CreateGenesisBlock(identity.genesis_message, CScript() << OP_RETURN, identity.genesis_time, /*nNonce=*/0, 0x207fffff, 1, 0);
    arith_uint256 target;
    target.SetCompact(genesis.nBits);
    while (UintToArith256(genesis.GetHash()) > target) ++genesis.nNonce;
    consensus.hashGenesisBlock = genesis.GetHash();

    vFixedSeeds.clear();
    vSeeds.clear();
    m_assumeutxo_data = {};
    chainTxData = ChainTxData{.nTime = identity.genesis_time, .tx_count = 1, .dTxRate = 0};
}

void CChainParams::ApplyDeploymentOptions(const DeploymentOptions& opts)
{
    for (const auto& [dep, height] : opts.activation_heights) {
        switch (dep) {
        case Consensus::BuriedDeployment::DEPLOYMENT_SEGWIT:
            consensus.SegwitHeight = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_HEIGHTINCB:
            consensus.BIP34Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_DERSIG:
            consensus.BIP66Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CLTV:
            consensus.BIP65Height = int{height};
            break;
        case Consensus::BuriedDeployment::DEPLOYMENT_CSV:
            consensus.CSVHeight = int{height};
            break;
        }
    }

    for (const auto& [deployment_pos, version_bits_params] : opts.version_bits_parameters) {
        consensus.vDeployments[deployment_pos].nStartTime = version_bits_params.start_time;
        consensus.vDeployments[deployment_pos].nTimeout = version_bits_params.timeout;
        consensus.vDeployments[deployment_pos].min_activation_height = version_bits_params.min_activation_height;
    }
}

/**
 * Chains main network.
 */
class CMainParams : public CChainParams {
public:
    CMainParams(const MainNetOptions& opts) {
        m_chain_type = ChainType::MAIN;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000; // ~146 days at one-minute blocks
        // New chain: every buried deployment is active from the first block.
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetSpacing = 60; // one-minute blocks
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing; // only sizes version bits periods; difficulty uses aserti3
        consensus.asert_half_life = 2 * 60 * 60; // two hours, i.e. 120 blocks
        consensus.asert_anchor_height = 2016; // blocks before it are mined at the proof of work limit
        // Blocks of up to six million weight units, four of them for transactions
        // and the rest for the coinbase with its drivechain messages and payouts.
        consensus.max_block_weight = 6'000'000;
        consensus.max_block_tx_weight = 4'000'000;
        consensus.coinbase_maturity = 360; // six hours
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false; // aserti3 is not subject to the timewarp attack
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xc7;
        pchMessageStart[1] = 0xe8;
        pchMessageStart[2] = 0xa9;
        pchMessageStart[3] = 0xd5;
        nDefaultPort = 9555;
        nPruneAfterHeight = 100000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // The genesis reward pays an unspendable output: there is no premine.
        genesis = CreateGenesisBlock("Chains 01/Oct/2026 One chain to mine them all: drivechains for everyone", CScript() << OP_RETURN, 1790879567, 1864237343, 0x1d00ffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000000942856f7f35f8af81e0bcade5750224d12b81dfb66cd348c52b7cb4a"});
        assert(genesis.hashMerkleRoot == uint256{"597434e3ce8c541d1b6b739a1f2477b8785cb4b803084ca628c06cd1c2c9b5af"});

        // No DNS or fixed seeds yet; peers are added with -addnode.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,28);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,88);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,156);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x88, 0xB2, 0x1E};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x88, 0xAD, 0xE4};

        bech32_hrp = "chn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1790879567,
            .tx_count = 1,
            .dTxRate  = 0,
        };

        // Bitcoin's values, to be regenerated with headerssync-params.py once the chain has history.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 649,
            .redownload_buffer_size = 15400,
        };

        // This chain is a sidechain of the Chains mainchain.
        MakeSidechain({.slot = 6, .genesis_message = "zSide: private payments with Zcash Orchard, on a sidechain", .genesis_time = 1791302400,
                       .message_start = {0x7a, 0x73, 0x64, 0x01}, .default_port = 9455, .bech32_hrp = "zs"});
        // Set when the slot activates on the mainchain: the height of the block that activated it.
        consensus.sidechain.main_activation_height = 0;
        consensus.sidechain.audit2_height = 0;
        // A tenth of the mainchain's withdrawal_min_score (64800); proposals within a day.
        consensus.sidechain.pending_min_score = 6480;
        consensus.sidechain.unproposed_expiry_blocks = 1440;
        consensus.coinbase_maturity = 0; // what a block pays (deposits, fees) can be spent in the next
        consensus.sidechain.zside.audit2_height = 0;
    }
};

/**
 * Chains public test network (testnet).
 */
class CTestNetParams : public CChainParams {
public:
    CTestNetParams(const TestNetOptions& opts) {
        m_chain_type = ChainType::TESTNET;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 210000; // ~146 days at one-minute blocks
        // New chain: every buried deployment is active from the first block.
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 0;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetSpacing = 60; // one-minute blocks
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing; // only sizes version bits periods; difficulty uses aserti3
        consensus.asert_half_life = 2 * 60 * 60; // two hours, i.e. 120 blocks
        consensus.asert_anchor_height = 2016; // blocks before it are mined at the proof of work limit
        // Blocks of up to six million weight units, four of them for transactions
        // and the rest for the coinbase with its drivechain messages and payouts.
        consensus.max_block_weight = 6'000'000;
        consensus.max_block_tx_weight = 4'000'000;
        consensus.coinbase_maturity = 360; // six hours
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = false; // aserti3 is not subject to the timewarp attack
        consensus.fPowNoRetargeting = false;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1512; // 75%;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(opts.dep_opts);

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        /**
         * The message start string is designed to be unlikely to occur in normal data.
         * The characters are rarely used upper ASCII, not valid as UTF-8, and produce
         * a large 32-bit integer with any alignment.
         */
        pchMessageStart[0] = 0xc7;
        pchMessageStart[1] = 0xe8;
        pchMessageStart[2] = 0xb4;
        pchMessageStart[3] = 0xf3;
        nDefaultPort = 19555;
        nPruneAfterHeight = 1000;
        m_assumed_blockchain_size = 1;
        m_assumed_chain_state_size = 1;

        // The genesis reward pays an unspendable output: there is no premine.
        genesis = CreateGenesisBlock("Chains testnet 01/Oct/2026 One chain to mine them all: drivechains for everyone", CScript() << OP_RETURN, 1790879566, 447901697, 0x1d00ffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"00000000b9f0e845fb2e059b3a648408ed9544d1b954f153d8a77f483a982894"});
        assert(genesis.hashMerkleRoot == uint256{"a4d0cd8aadbb7b5f90668570e675964d7bea62da9cfe9d18a92a7d06bcab398b"});

        // No DNS or fixed seeds yet; peers are added with -addnode.
        vFixedSeeds.clear();
        vSeeds.clear();

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        bech32_hrp = "tchn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1790879566,
            .tx_count = 1,
            .dTxRate  = 0,
        };

        // Bitcoin's values, to be regenerated with headerssync-params.py once the chain has history.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 649,
            .redownload_buffer_size = 15400,
        };

        // This chain is a sidechain of the Chains testnet.
        MakeSidechain({.slot = 6, .genesis_message = "zSide testnet", .genesis_time = 1791302400,
                       .message_start = {0x7a, 0x73, 0x64, 0x02}, .default_port = 19455, .bech32_hrp = "tzs"});
        // The mainchain of the test network votes on a withdrawal bundle within 600 blocks.
        consensus.sidechain.bundle_retry_delay = 20;
        // The test network ran without it until then; see SidechainParams.
        consensus.sidechain.single_bundle_height = 2300;
        // The height of the mainchain block that activated the slot; nothing is left out at 0.
        consensus.sidechain.main_activation_height = 4900;
        consensus.sidechain.audit2_height = 3597; // audits 2-4 on the test network: moved out while the fourth audit's fixes are made
        // A tenth of the testnet mainchain's withdrawal_min_score (300); proposals within an hour.
        consensus.sidechain.pending_min_score = 30;
        consensus.sidechain.unproposed_expiry_blocks = 60;
        consensus.coinbase_maturity = 0; // what a block pays (deposits, fees) can be spent in the next
        consensus.sidechain.zside.audit2_height = 3597; // audits 2-4 on the test network: moved out while the fourth audit's fixes are made
    }
};

/**
 * Signet: test network with an additional consensus parameter (see BIP325).
 */
class SigNetParams : public CChainParams {
public:
    explicit SigNetParams(const SigNetOptions& options)
    {
        std::vector<uint8_t> bin;
        vFixedSeeds.clear();
        vSeeds.clear();

        if (!options.challenge) {
            // The default Chains signet: blocks are signed with a single key.
            bin = "5121022eb69a435e256daf541a170b29fdedbd5ef5f6efbd66f67cfec24402a2b1f35551ae"_hex_v_u8;
        } else {
            bin = *options.challenge;
            LogInfo("Signet with challenge %s", HexStr(bin));
        }
        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        if (options.seeds) {
            vSeeds = *options.seeds;
        }

        // Apart from the block signatures and the lower proof of work limit,
        // signet follows the rules of the main network.
        m_chain_type = ChainType::SIGNET;
        consensus.signet_blocks = true;
        consensus.signet_challenge.assign(bin.begin(), bin.end());
        consensus.nSubsidyHalvingInterval = 210000;
        consensus.BIP34Height = 1;
        consensus.BIP34Hash = uint256{};
        consensus.BIP65Height = 1;
        consensus.BIP66Height = 1;
        consensus.CSVHeight = 1;
        consensus.SegwitHeight = 1;
        consensus.nPowTargetSpacing = 60; // one-minute blocks
        consensus.nPowTargetTimespan = 2016 * consensus.nPowTargetSpacing; // only sizes version bits periods; difficulty uses aserti3
        consensus.asert_half_life = 2 * 60 * 60; // two hours, i.e. 120 blocks
        consensus.asert_anchor_height = 2016; // blocks before it are mined at the proof of work limit
        consensus.max_block_weight = 6'000'000;
        consensus.max_block_tx_weight = 4'000'000;
        consensus.coinbase_maturity = 360; // six hours
        consensus.fPowAllowMinDifficultyBlocks = false;
        consensus.enforce_BIP94 = false; // aserti3 is not subject to the timewarp attack
        consensus.fPowNoRetargeting = false;
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"00000377ae000000000000000000000000000000000000000000000000000000"};
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = Consensus::BIP9Deployment::NEVER_ACTIVE;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 1815; // 90%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 2016;

        ApplyDeploymentOptions(options.dep_opts);

        // message start is defined as the first 4 bytes of the sha256d of the block script
        HashWriter h{};
        h << consensus.signet_challenge;
        uint256 hash = h.GetHash();
        std::copy_n(hash.begin(), 4, pchMessageStart.begin());

        nDefaultPort = 39555;
        nPruneAfterHeight = 1000;

        // The genesis reward pays an unspendable output: there is no premine.
        genesis = CreateGenesisBlock("Chains signet 01/Oct/2026 One chain to mine them all: drivechains for everyone", CScript() << OP_RETURN, 1790879568, 9736156, 0x1e0377ae, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"000001ca9ff240bbe9e7883dee99e91d1096c95e10cee82979a6cd1f08364073"});
        assert(genesis.hashMerkleRoot == uint256{"661cbd2140e027a506eb6ced73964fdfe58c7becce7e66ae5a9710a9266057e7"});

        m_assumeutxo_data = {};

        chainTxData = ChainTxData{
            .nTime    = 1790879568,
            .tx_count = 1,
            .dTxRate  = 0,
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // As on Bitcoin, signet shares its address format with testnet.
        bech32_hrp = "tchn";

        fDefaultConsistencyChecks = false;
        m_is_mockable_chain = false;

        // Bitcoin's values, to be regenerated with headerssync-params.py once the chain has history.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 629,
            .redownload_buffer_size = 15885,
        };

        // This chain is a sidechain of the Chains signet. Its own blocks are not signed.
        MakeSidechain({.slot = 6, .genesis_message = "zSide signet", .genesis_time = 1791302400,
                       .message_start = {0x7a, 0x73, 0x64, 0x03}, .default_port = 39455, .bech32_hrp = "tzs"});
        consensus.sidechain.main_activation_height = 0;
        consensus.sidechain.audit2_height = 0;
        // The Chains signet has the rules of its main network.
        consensus.sidechain.pending_min_score = 6480;
        consensus.sidechain.unproposed_expiry_blocks = 1440;
        consensus.coinbase_maturity = 0; // what a block pays (deposits, fees) can be spent in the next
        consensus.sidechain.zside.audit2_height = 0;
    }
};

/**
 * Regression test: intended for private networks only. Has minimal difficulty to ensure that
 * blocks can be found instantly.
 */
class CRegTestParams : public CChainParams
{
public:
    explicit CRegTestParams(const RegTestOptions& opts)
    {
        m_chain_type = ChainType::REGTEST;
        consensus.signet_blocks = false;
        consensus.signet_challenge.clear();
        consensus.nSubsidyHalvingInterval = 150;
        // Short drivechain periods so that tests can run through them.
        consensus.drivechain.activation_period = 20;
        consensus.drivechain.activation_max_failures = 9;
        consensus.drivechain.replacement_period = 40;
        consensus.drivechain.withdrawal_period = 60;
        consensus.drivechain.withdrawal_min_score = 30;
        consensus.drivechain.upvote_expiry_blocks = 20;
        consensus.drivechain.unvoted_forget_blocks = 20;
        for (const auto& [name, value] : opts.drivechain_params) {
            auto& dc{consensus.drivechain};
            if (name == "max_sidechains") {
                dc.max_sidechains = value;
            } else if (name == "activation_period") {
                dc.activation_period = value;
            } else if (name == "activation_max_failures") {
                dc.activation_max_failures = value;
            } else if (name == "replacement_period") {
                dc.replacement_period = value;
            } else if (name == "withdrawal_period") {
                dc.withdrawal_period = value;
            } else if (name == "withdrawal_min_score") {
                dc.withdrawal_min_score = value;
            } else if (name == "max_pending_bundles") {
                dc.max_pending_bundles = value;
            } else if (name == "single_payout_height") {
                dc.single_payout_height = value;
            } else if (name == "idle_expiry_height") {
                dc.idle_expiry_height = value;
            } else if (name == "idle_expiry_blocks") {
                dc.idle_expiry_blocks = value;
            } else if (name == "audit2_height") {
                dc.audit2_height = value;
            } else if (name == "upvote_expiry_blocks") {
                dc.upvote_expiry_blocks = value;
            } else if (name == "unvoted_forget_blocks") {
                dc.unvoted_forget_blocks = value;
            } else {
                throw std::runtime_error(strprintf("Invalid name (%s) for -testdrivechainparam=name@value.", name));
            }
        }
        consensus.BIP34Height = 1; // Always active unless overridden
        consensus.BIP34Hash = uint256();
        consensus.BIP65Height = 1;  // Always active unless overridden
        consensus.BIP66Height = 1;  // Always active unless overridden
        consensus.CSVHeight = 1;    // Always active unless overridden
        consensus.SegwitHeight = 0; // Always active unless overridden
        consensus.MinBIP9WarningHeight = 0;
        consensus.powLimit = uint256{"7fffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"};
        consensus.nPowTargetTimespan = 24 * 60 * 60; // one day
        consensus.nPowTargetSpacing = 10 * 60;
        consensus.fPowAllowMinDifficultyBlocks = true;
        consensus.enforce_BIP94 = opts.enforce_bip94;
        consensus.fPowNoRetargeting = true;

        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].bit = 28;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nStartTime = 0;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].nTimeout = Consensus::BIP9Deployment::NO_TIMEOUT;
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].min_activation_height = 0; // No activation delay
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].threshold = 108; // 75%
        consensus.vDeployments[Consensus::DEPLOYMENT_TESTDUMMY].period = 144; // Faster than normal for regtest (144 instead of 2016)

        consensus.nMinimumChainWork = uint256{};
        consensus.defaultAssumeValid = uint256{};

        pchMessageStart[0] = 0x7a;
        pchMessageStart[1] = 0x73;
        pchMessageStart[2] = 0x64;
        pchMessageStart[3] = 0x04;
        nDefaultPort = 29455;
        nPruneAfterHeight = opts.fastprune ? 100 : 1000;

        // On request this chain is a sidechain, with what tests need to be quick.
        if (opts.sidechain_slot) {
            consensus.sidechain.enabled = true;
            consensus.sidechain.slot = *opts.sidechain_slot;
            consensus.sidechain.bundle_retry_delay = 5;
            consensus.sidechain.audit2_height = 0;
            // A tenth of the regtest mainchain's withdrawal_min_score (30).
            consensus.sidechain.pending_min_score = 3;
            consensus.sidechain.unproposed_expiry_blocks = 20;
            // A sidechain has no sidechains of its own.
            consensus.drivechain.max_sidechains = 0;
            consensus.coinbase_maturity = 0;
            consensus.sidechain.zside.audit2_height = 0;
        }
        m_assumed_blockchain_size = 0;
        m_assumed_chain_state_size = 0;

        ApplyDeploymentOptions(opts.dep_opts);

        genesis = CreateGenesisBlock(1296688602, 2, 0x207fffff, 1, 50 * COIN);
        consensus.hashGenesisBlock = genesis.GetHash();
        assert(consensus.hashGenesisBlock == uint256{"0f9188f13cb7b2c71f2a335e3a4fc328bf5beb436012afca590b1a11466e2206"});
        assert(genesis.hashMerkleRoot == uint256{"4a5e1e4baab89f3a32518a88c31bc87f618f76673e2cc77ab2127b7afdeda33b"});

        vFixedSeeds.clear(); //!< Regtest mode doesn't have any fixed seeds.
        vSeeds.clear();
        vSeeds.emplace_back("dummySeed.invalid.");

        fDefaultConsistencyChecks = true;
        m_is_mockable_chain = true;

        m_assumeutxo_data = {
            {   // For use by unit tests
                .height = 110,
                .hash_serialized = AssumeutxoHash{uint256{"86e9a1205b418b16dde3a18a78c730e30137e28466bda5dbf6b33ab8fc05447c"}},
                .m_chain_tx_count = 111,
                .blockhash = uint256{"135eec25a6fb277884e5824e7aa7d052c4868161c99a5122170b5266f86c273d"},
            },
            {
                // For use by fuzz target src/test/fuzz/utxo_snapshot.cpp
                .height = 200,
                .hash_serialized = AssumeutxoHash{uint256{"17dcc016d188d16068907cdeb38b75691a118d43053b8cd6a25969419381d13a"}},
                .m_chain_tx_count = 201,
                .blockhash = uint256{"385901ccbd69dff6bbd00065d01fb8a9e464dede7cfe0372443884f9b1dcf6b9"},
            },
            {
                // For use by test/functional/feature_assumeutxo.py and test/functional/tool_bitcoin_chainstate.py
                .height = 299,
                .hash_serialized = AssumeutxoHash{uint256{"106b2c56233e378a824cf0d5ff2be42ed32c72f1605c9be288d00942908a40ac"}},
                .m_chain_tx_count = 334,
                .blockhash = uint256{"0c552ced4721c249a389eb9b08cb8da261cd46f0e7b5f9d064d48f3113406853"},
            },
        };

        chainTxData = ChainTxData{
            .nTime = 0,
            .tx_count = 0,
            .dTxRate = 0.001, // Set a non-zero rate to make it testable
        };

        base58Prefixes[PUBKEY_ADDRESS] = std::vector<unsigned char>(1,111);
        base58Prefixes[SCRIPT_ADDRESS] = std::vector<unsigned char>(1,196);
        base58Prefixes[SECRET_KEY] =     std::vector<unsigned char>(1,239);
        base58Prefixes[EXT_PUBLIC_KEY] = {0x04, 0x35, 0x87, 0xCF};
        base58Prefixes[EXT_SECRET_KEY] = {0x04, 0x35, 0x83, 0x94};

        // Its own, so that an address of another chain is not taken for one of this chain.
        bech32_hrp = "rsc";

        // Copied from Testnet4.
        m_headers_sync_params = HeadersSyncParams{
            .commitment_period = 275,
            .redownload_buffer_size = 7017, // 7017/275 = ~25.5 commitments
        };
    }
};

std::unique_ptr<const CChainParams> CChainParams::SigNet(const SigNetOptions& options)
{
    return std::make_unique<const SigNetParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::RegTest(const RegTestOptions& options)
{
    return std::make_unique<const CRegTestParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::Main(const MainNetOptions& options)
{
    return std::make_unique<const CMainParams>(options);
}

std::unique_ptr<const CChainParams> CChainParams::TestNet(const TestNetOptions& options)
{
    return std::make_unique<const CTestNetParams>(options);
}


std::vector<int> CChainParams::GetAvailableSnapshotHeights() const
{
    std::vector<int> heights;
    heights.reserve(m_assumeutxo_data.size());

    for (const auto& data : m_assumeutxo_data) {
        heights.emplace_back(data.height);
    }
    return heights;
}

std::optional<ChainType> GetNetworkForMagic(const MessageStartChars& message)
{
    const auto mainnet_msg = CChainParams::Main()->MessageStart();
    const auto testnet_msg = CChainParams::TestNet()->MessageStart();
    const auto regtest_msg = CChainParams::RegTest()->MessageStart();
    const auto signet_msg = CChainParams::SigNet()->MessageStart();

    if (std::ranges::equal(message, mainnet_msg)) {
        return ChainType::MAIN;
    } else if (std::ranges::equal(message, testnet_msg)) {
        return ChainType::TESTNET;
    } else if (std::ranges::equal(message, regtest_msg)) {
        return ChainType::REGTEST;
    } else if (std::ranges::equal(message, signet_msg)) {
        return ChainType::SIGNET;
    }
    return std::nullopt;
}
