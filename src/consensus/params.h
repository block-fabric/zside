// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CONSENSUS_PARAMS_H
#define BITCOIN_CONSENSUS_PARAMS_H

#include <consensus/consensus.h>
#include <script/verify_flags.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <map>
#include <vector>

namespace Consensus {

/**
 * A buried deployment is one where the height of the activation has been hardcoded into
 * the client implementation long after the consensus change has activated. See BIP 90.
 * Consensus changes for which the new rules are enforced from genesis are not listed here.
 */
enum BuriedDeployment : int16_t {
    // buried deployments get negative values to avoid overlap with DeploymentPos
    DEPLOYMENT_HEIGHTINCB = std::numeric_limits<int16_t>::min(),
    DEPLOYMENT_CLTV,
    DEPLOYMENT_DERSIG,
    DEPLOYMENT_CSV,
    // SCRIPT_VERIFY_WITNESS is enforced from genesis, but the check for downloading
    // missing witness data is not. BIP 147 also relies on hardcoded activation height.
    DEPLOYMENT_SEGWIT,
};
constexpr bool ValidDeployment(BuriedDeployment dep) { return dep <= DEPLOYMENT_SEGWIT; }

enum DeploymentPos : uint16_t {
    DEPLOYMENT_TESTDUMMY,
    // NOTE: Also add new deployments to VersionBitsDeploymentInfo in deploymentinfo.cpp
    // Removing an entry may require bumping MinBIP9WarningHeight.
    MAX_VERSION_BITS_DEPLOYMENTS
};
constexpr bool ValidDeployment(DeploymentPos dep) { return dep < MAX_VERSION_BITS_DEPLOYMENTS; }

/**
 * Struct for each individual consensus rule change using BIP9.
 */
struct BIP9Deployment {
    /** Bit position to select the particular bit in nVersion. */
    int bit{28};
    /** Start MedianTime for version bits miner confirmation. Can be a date in the past */
    int64_t nStartTime{NEVER_ACTIVE};
    /** Timeout/expiry MedianTime for the deployment attempt. */
    int64_t nTimeout{NEVER_ACTIVE};
    /** If lock in occurs, delay activation until at least this block
     *  height.  Note that activation will only occur on a retarget
     *  boundary.
     */
    int min_activation_height{0};
    /** Period of blocks to check signalling in (usually retarget period, ie params.DifficultyAdjustmentInterval()) */
    uint32_t period{2016};
    /**
     * Minimum blocks including miner confirmation of the total of 2016 blocks in a retargeting period,
     * which is also used for BIP9 deployments.
     * Examples: 1916 for 95%, 1512 for testchains.
     */
    uint32_t threshold{1916};

    /** Constant for nTimeout very far in the future. */
    static constexpr int64_t NO_TIMEOUT = std::numeric_limits<int64_t>::max();

    /** Special value for nStartTime indicating that the deployment is always active.
     *  This is useful for testing, as it means tests don't need to deal with the activation
     *  process (which takes at least 3 BIP9 intervals). Only tests that specifically test the
     *  behaviour during activation cannot use this. */
    static constexpr int64_t ALWAYS_ACTIVE = -1;

    /** Special value for nStartTime indicating that the deployment is never active.
     *  This is useful for integrating the code changes for a new feature
     *  prior to deploying it on some or all networks. */
    static constexpr int64_t NEVER_ACTIVE = -2;
};

/**
 * Parameters of a chain that is a sidechain of the mainchain: its blocks are
 * blind merged mined, and its coins come from and return to the mainchain.
 */
struct SidechainParams {
    /** Whether this chain is a sidechain. If not, none of the sidechain rules apply. */
    bool enabled{false};
    /** The slot the mainchain gave to this sidechain. */
    uint32_t slot{0};
    /** Smallest amount, in the smallest unit, that can be withdrawn to the mainchain. */
    int64_t min_withdrawal{10000};
    /** Most withdrawals that one withdrawal bundle pays out. */
    uint32_t max_bundle_withdrawals{1000};
    /** Number of blocks to wait after a withdrawal bundle failed before the next one can be made. */
    int bundle_retry_delay{144};
};

/**
 * Parameters of the drivechain (sidechain escrow and blind merged mining) rules.
 * All periods and thresholds are counted in blocks.
 */
struct DrivechainParams {
    /** Number of sidechain slots. Slots at or above this number are not subject to any drivechain rule. */
    uint32_t max_sidechains{256};
    /** Age at which a proposal for an empty slot activates. */
    int activation_period{7200};
    /** A proposal is rejected once it has gone this many blocks without an ack. */
    int activation_max_failures{3599};
    /** Age at which a proposal for a slot that is already in use activates, replacing the sidechain in it. */
    int replacement_period{64800};
    /** Number of blocks a withdrawal bundle has to collect its work score. */
    int withdrawal_period{129600};
    /** Work score a withdrawal bundle needs before it can be paid out. */
    int withdrawal_min_score{64800};
    /** Maximum number of pending withdrawal bundles per sidechain. */
    uint32_t max_pending_bundles{64};
};

/**
 * Parameters that influence chain consensus.
 */
struct Params {
    uint256 hashGenesisBlock;
    int nSubsidyHalvingInterval;
    /**
     * Hashes of blocks that
     * - are known to be consensus valid, and
     * - buried in the chain, and
     * - fail if the default script verify flags are applied.
     */
    std::map<uint256, script_verify_flags> script_flag_exceptions;
    /** Block height and hash at which BIP34 becomes active */
    int BIP34Height;
    uint256 BIP34Hash;
    /** Block height at which BIP65 becomes active */
    int BIP65Height;
    /** Block height at which BIP66 becomes active */
    int BIP66Height;
    /** Block height at which CSV (BIP68, BIP112 and BIP113) becomes active */
    int CSVHeight;
    /** Block height at which Segwit (BIP141, BIP143 and BIP147) becomes active.
     * Note that segwit v0 script rules are enforced on all blocks except the
     * BIP 16 exception blocks. */
    int SegwitHeight;
    /** Don't warn about unknown BIP 9 activations below this height.
     * This prevents us from warning about the CSV, segwit and taproot activations. */
    int MinBIP9WarningHeight;
    std::array<BIP9Deployment,MAX_VERSION_BITS_DEPLOYMENTS> vDeployments;
    /** Proof of work parameters */
    uint256 powLimit;
    bool fPowAllowMinDifficultyBlocks;
    /**
      * Enforce BIP94 timewarp attack mitigation. Where minimum difficulty blocks are allowed this also enforces
      * the block storm mitigation.
      */
    bool enforce_BIP94;
    bool fPowNoRetargeting;
    int64_t nPowTargetSpacing;
    int64_t nPowTargetTimespan;
    /**
     * Half-life, in seconds, of the aserti3 difficulty algorithm, which retargets
     * every block. Zero disables it in favour of the periodic nPowTargetTimespan
     * retarget.
     */
    int64_t asert_half_life{0};
    /**
     * Height of the aserti3 anchor block. Blocks below it are mined at the
     * proof of work limit; the anchor's own target is calibrated from the time
     * those blocks took, and aserti3 retargets every block after it.
     */
    int asert_anchor_height{0};
    DrivechainParams drivechain;
    SidechainParams sidechain;
    /** Maximum weight of a block. */
    uint32_t max_block_weight{MAX_BLOCK_WEIGHT};
    /**
     * Maximum weight of the transactions of a block other than the coinbase.
     * Where this is below max_block_weight, the difference is set aside for
     * the coinbase, which carries the drivechain messages and the payouts of
     * mining pools.
     */
    uint32_t max_block_tx_weight{MAX_BLOCK_WEIGHT};
    /** Number of blocks before the outputs of a coinbase can be spent. */
    int coinbase_maturity{COINBASE_MATURITY};
    /** Upper bound for the serialized size of a block; a sanity limit, not a consensus rule. */
    uint32_t MaxBlockSerializedSize() const { return std::max<uint32_t>(MAX_BLOCK_SERIALIZED_SIZE, max_block_weight); }
    std::chrono::seconds PowTargetSpacing() const
    {
        return std::chrono::seconds{nPowTargetSpacing};
    }
    int64_t DifficultyAdjustmentInterval() const { return nPowTargetTimespan / nPowTargetSpacing; }
    /** The best chain should have at least this much work */
    uint256 nMinimumChainWork;
    /** By default assume that the signatures in ancestors of this block are valid */
    uint256 defaultAssumeValid;

    /**
     * If true, witness commitments contain a payload equal to a Bitcoin Script solution
     * to the signet challenge. See BIP325.
     */
    bool signet_blocks{false};
    std::vector<uint8_t> signet_challenge;

    int DeploymentHeight(BuriedDeployment dep) const
    {
        switch (dep) {
        case DEPLOYMENT_HEIGHTINCB:
            return BIP34Height;
        case DEPLOYMENT_CLTV:
            return BIP65Height;
        case DEPLOYMENT_DERSIG:
            return BIP66Height;
        case DEPLOYMENT_CSV:
            return CSVHeight;
        case DEPLOYMENT_SEGWIT:
            return SegwitHeight;
        } // no default case, so the compiler can warn about missing cases
        return std::numeric_limits<int>::max();
    }
};

} // namespace Consensus

#endif // BITCOIN_CONSENSUS_PARAMS_H
