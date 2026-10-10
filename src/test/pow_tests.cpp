// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chain.h>
#include <chainparams.h>
#include <pow.h>
#include <test/util/random.h>
#include <test/util/common.h>
#include <test/util/inherited_params.h>
#include <test/util/setup_common.h>
#include <util/chaintype.h>

#include <boost/test/unit_test.hpp>

#include <cmath>
#include <vector>

BOOST_FIXTURE_TEST_SUITE(pow_tests, BasicTestingSetup)

/**
 * Bitcoin's periodic retarget, which the main chain replaces with aserti3. It
 * is still used when asert_half_life is zero, so keep exercising it with the
 * parameters its test vectors were made for.
 */
static Consensus::Params PeriodicRetargetParams(const ArgsManager& args)
{
    Consensus::Params params{WithInheritedRules(CreateChainParams(args, ChainType::MAIN)->GetConsensus())};
    params.asert_half_life = 0;
    params.nPowTargetSpacing = 10 * 60;
    params.nPowTargetTimespan = 14 * 24 * 60 * 60;
    return params;
}

/* Test calculation of next difficulty target with no constraints applying */
BOOST_AUTO_TEST_CASE(get_next_work)
{
    const Consensus::Params params{PeriodicRetargetParams(*m_node.args)};
    int64_t nLastRetargetTime = 1261130161; // Block #30240
    CBlockIndex pindexLast;
    pindexLast.nHeight = 32255;
    pindexLast.nTime = 1262152739;  // Block #32255
    pindexLast.nBits = 0x1d00ffff;

    // Here (and below): expected_nbits is calculated in
    // CalculateNextWorkRequired(); redoing the calculation here would be just
    // reimplementing the same code that is written in pow.cpp. Rather than
    // copy that code, we just hardcode the expected result.
    unsigned int expected_nbits = 0x1d00d86aU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the upper bound for next work */
BOOST_AUTO_TEST_CASE(get_next_work_pow_limit)
{
    const Consensus::Params params{PeriodicRetargetParams(*m_node.args)};
    int64_t nLastRetargetTime = 1231006505; // Block #0
    CBlockIndex pindexLast;
    pindexLast.nHeight = 2015;
    pindexLast.nTime = 1233061996;  // Block #2015
    pindexLast.nBits = 0x1d00ffff;
    unsigned int expected_nbits = 0x1d00ffffU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
}

/* Test the constraint on the lower bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_lower_limit_actual)
{
    const Consensus::Params params{PeriodicRetargetParams(*m_node.args)};
    int64_t nLastRetargetTime = 1279008237; // Block #66528
    CBlockIndex pindexLast;
    pindexLast.nHeight = 68543;
    pindexLast.nTime = 1279297671;  // Block #68543
    pindexLast.nBits = 0x1c05a3f4;
    unsigned int expected_nbits = 0x1c0168fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that reducing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits-1;
    BOOST_CHECK(!PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

/* Test the constraint on the upper bound for actual time taken */
BOOST_AUTO_TEST_CASE(get_next_work_upper_limit_actual)
{
    const Consensus::Params params{PeriodicRetargetParams(*m_node.args)};
    int64_t nLastRetargetTime = 1263163443; // NOTE: Not an actual block time
    CBlockIndex pindexLast;
    pindexLast.nHeight = 46367;
    pindexLast.nTime = 1269211443;  // Block #46367
    pindexLast.nBits = 0x1c387f6f;
    unsigned int expected_nbits = 0x1d00e1fdU;
    BOOST_CHECK_EQUAL(CalculateNextWorkRequired(&pindexLast, nLastRetargetTime, params), expected_nbits);
    BOOST_CHECK(PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, expected_nbits));
    // Test that increasing nbits further would not be a PermittedDifficultyTransition.
    unsigned int invalid_nbits = expected_nbits+1;
    BOOST_CHECK(!PermittedDifficultyTransition(params, pindexLast.nHeight+1, pindexLast.nBits, invalid_nbits));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_negative_target)
{
    const auto consensus = WithInheritedRules(CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus());
    uint256 hash;
    unsigned int nBits;
    nBits = UintToArith256(consensus.powLimit).GetCompact(true);
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_overflow_target)
{
    const auto consensus = WithInheritedRules(CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus());
    uint256 hash;
    unsigned int nBits{~0x00800000U};
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_too_easy_target)
{
    const auto consensus = WithInheritedRules(CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus());
    uint256 hash;
    unsigned int nBits;
    arith_uint256 nBits_arith = UintToArith256(consensus.powLimit);
    nBits_arith *= 2;
    nBits = nBits_arith.GetCompact();
    hash = uint256{1};
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_biger_hash_than_target)
{
    const auto consensus = WithInheritedRules(CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus());
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith = UintToArith256(consensus.powLimit);
    nBits = hash_arith.GetCompact();
    hash_arith *= 2; // hash > nBits
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(CheckProofOfWork_test_zero_target)
{
    const auto consensus = WithInheritedRules(CreateChainParams(*m_node.args, ChainType::MAIN)->GetConsensus());
    uint256 hash;
    unsigned int nBits;
    arith_uint256 hash_arith{0};
    nBits = hash_arith.GetCompact();
    hash = ArithToUint256(hash_arith);
    BOOST_CHECK(!CheckProofOfWork(hash, nBits, consensus));
}

BOOST_AUTO_TEST_CASE(GetBlockProofEquivalentTime_test)
{
    const auto chainParams = CreateChainParams(*m_node.args, ChainType::MAIN);
    std::vector<CBlockIndex> blocks(10000);
    for (int i = 0; i < 10000; i++) {
        blocks[i].pprev = i ? &blocks[i - 1] : nullptr;
        blocks[i].nHeight = i;
        blocks[i].nTime = 1269211443 + i * chainParams->GetConsensus().nPowTargetSpacing;
        blocks[i].nBits = 0x207fffff; /* target 0x7fffff000... */
        blocks[i].nChainWork = i ? blocks[i - 1].nChainWork + GetBlockProof(blocks[i - 1]) : arith_uint256(0);
    }

    for (int j = 0; j < 1000; j++) {
        CBlockIndex *p1 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p2 = &blocks[m_rng.randrange(10000)];
        CBlockIndex *p3 = &blocks[m_rng.randrange(10000)];

        int64_t tdiff = GetBlockProofEquivalentTime(*p1, *p2, *p3, chainParams->GetConsensus());
        BOOST_CHECK_EQUAL(tdiff, p1->GetBlockTime() - p2->GetBlockTime());
    }
}

void sanity_check_chainparams(const ArgsManager& args, ChainType chain_type)
{
    const auto chainParams = CreateChainParams(args, chain_type);
    const auto consensus = chainParams->GetConsensus();

    // hash genesis is correct
    BOOST_CHECK_EQUAL(consensus.hashGenesisBlock, chainParams->GenesisBlock().GetHash());

    // target timespan is an even multiple of spacing
    BOOST_CHECK_EQUAL(consensus.nPowTargetTimespan % consensus.nPowTargetSpacing, 0);

    // genesis nBits is positive, doesn't overflow and is lower than powLimit
    arith_uint256 pow_compact;
    bool neg, over;
    pow_compact.SetCompact(chainParams->GenesisBlock().nBits, &neg, &over);
    BOOST_CHECK(!neg);
    BOOST_CHECK(pow_compact != 0);
    BOOST_CHECK(!over);
    BOOST_CHECK(UintToArith256(consensus.powLimit) >= pow_compact);

    // check max target * 4*nPowTargetTimespan doesn't overflow -- see pow.cpp:CalculateNextWorkRequired()
    if (!consensus.fPowNoRetargeting) {
        arith_uint256 targ_max{UintToArith256(uint256{"ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
        targ_max /= consensus.nPowTargetTimespan*4;
        BOOST_CHECK(UintToArith256(consensus.powLimit) < targ_max);
    }
}

BOOST_AUTO_TEST_CASE(ChainParams_MAIN_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::MAIN);
}

BOOST_AUTO_TEST_CASE(ChainParams_REGTEST_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::REGTEST);
}

BOOST_AUTO_TEST_CASE(ChainParams_TESTNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::TESTNET);
}

BOOST_AUTO_TEST_CASE(ChainParams_SIGNET_sanity)
{
    sanity_check_chainparams(*m_node.args, ChainType::SIGNET);
}

/* aserti3: a block exactly on schedule leaves the target unchanged. */
BOOST_AUTO_TEST_CASE(asert_on_schedule)
{
    const arith_uint256 pow_limit{UintToArith256(uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    const arith_uint256 ref{pow_limit >> 8};
    const int64_t spacing{60};
    const int64_t half_life{2 * 60 * 60};
    for (const int64_t height_diff : {0, 1, 119, 120, 100000}) {
        BOOST_CHECK(CalculateASERT(ref, spacing, spacing * (height_diff + 1), height_diff, pow_limit, half_life) == ref);
    }
}

/* aserti3: each half-life behind (ahead of) schedule doubles (halves) the target exactly. */
BOOST_AUTO_TEST_CASE(asert_half_life)
{
    const arith_uint256 pow_limit{UintToArith256(uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    const arith_uint256 ref{pow_limit >> 8};
    const int64_t spacing{60};
    const int64_t half_life{2 * 60 * 60};
    const int64_t height_diff{1000};
    const int64_t on_schedule{spacing * (height_diff + 1)};
    BOOST_CHECK(CalculateASERT(ref, spacing, on_schedule + half_life, height_diff, pow_limit, half_life) == ref * 2);
    BOOST_CHECK(CalculateASERT(ref, spacing, on_schedule + 3 * half_life, height_diff, pow_limit, half_life) == ref * 8);
    BOOST_CHECK(CalculateASERT(ref, spacing, on_schedule - half_life, height_diff, pow_limit, half_life) == ref / 2);
    BOOST_CHECK(CalculateASERT(ref, spacing, on_schedule - 4 * half_life, height_diff, pow_limit, half_life) == ref / 16);
}

/* aserti3: fractional exponents follow 2^x to within the 0.013% error of the cubic approximation. */
BOOST_AUTO_TEST_CASE(asert_fractional)
{
    const arith_uint256 pow_limit{UintToArith256(uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    const arith_uint256 ref{arith_uint256{1} << 200};
    const int64_t spacing{60};
    const int64_t half_life{2 * 60 * 60};
    const int64_t height_diff{10};
    const int64_t on_schedule{spacing * (height_diff + 1)};
    for (const int64_t offset : {-7000, -3600, -1800, -61, -1, 1, 59, 1800, 3600, 5400, 7199}) {
        const arith_uint256 next{CalculateASERT(ref, spacing, on_schedule + offset, height_diff, pow_limit, half_life)};
        const double expected{std::pow(2.0, static_cast<double>(offset) / half_life)};
        const double actual{next.getdouble() / ref.getdouble()};
        BOOST_CHECK_CLOSE(actual, expected, 0.02); // percent
        // A later parent timestamp never makes the next block harder.
        BOOST_CHECK(CalculateASERT(ref, spacing, on_schedule + offset + 1, height_diff, pow_limit, half_life) >= next);
    }
}

/* aserti3: results are clamped to [1, pow_limit]. */
BOOST_AUTO_TEST_CASE(asert_clamp)
{
    const arith_uint256 pow_limit{UintToArith256(uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    const int64_t spacing{60};
    const int64_t half_life{2 * 60 * 60};
    // Far behind schedule: never easier than the limit, including when the shift overflows 256 bits.
    BOOST_CHECK(CalculateASERT(pow_limit, spacing, spacing + half_life, 0, pow_limit, half_life) == pow_limit);
    BOOST_CHECK(CalculateASERT(pow_limit >> 4, spacing, spacing + 300 * half_life, 0, pow_limit, half_life) == pow_limit);
    // Far ahead of schedule: never zero.
    BOOST_CHECK(CalculateASERT(arith_uint256{2}, spacing, 0, 1000000, pow_limit, half_life) == arith_uint256{1});
}

/* aserti3 on the main chain parameters: bootstrap period, calibrated anchor, steady state, and response to hash rate changes. */
BOOST_AUTO_TEST_CASE(asert_chain)
{
    const auto chain_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const Consensus::Params params{WithInheritedRules(chain_params->GetConsensus())};
    BOOST_REQUIRE(params.asert_half_life > 0);
    const int anchor_height{params.asert_anchor_height};
    BOOST_REQUIRE(anchor_height >= 3);
    const uint32_t limit_bits{UintToArith256(params.powLimit).GetCompact()};
    const int64_t spacing{params.nPowTargetSpacing};
    const auto target_of = [](const CBlockIndex& index) {
        arith_uint256 target;
        target.SetCompact(index.nBits);
        return target;
    };

    std::vector<CBlockIndex> blocks(anchor_height + 2000);
    const auto add_block = [&](int height, int64_t time) {
        CBlockHeader header;
        header.nTime = time;
        blocks[height].pprev = &blocks[height - 1];
        blocks[height].nHeight = height;
        blocks[height].nTime = time;
        blocks[height].nBits = GetNextWorkRequired(&blocks[height - 1], &header, params);
        blocks[height].BuildSkip();
    };

    // Genesis long before the launch of the network.
    blocks[0].nHeight = 0;
    blocks[0].nTime = 1790879567;
    blocks[0].nBits = limit_bits;

    // Bootstrap blocks are mined at the limit however fast they arrive: here sixteen times too fast.
    int64_t time{blocks[0].nTime + 90 * 24 * 60 * 60};
    add_block(1, time);
    for (int h{2}; h < anchor_height; ++h) {
        if (h % 4 == 0) time += spacing / 4;
        add_block(h, time);
    }
    for (int h{1}; h < anchor_height; ++h) BOOST_REQUIRE_EQUAL(blocks[h].nBits, limit_bits);

    // The anchor is calibrated to the observed rate, and the launch delay after the genesis block is not part of it.
    add_block(anchor_height, time += spacing);
    BOOST_CHECK_CLOSE(target_of(blocks[anchor_height]).getdouble() / target_of(blocks[0]).getdouble(), 1.0 / 16, 0.5);

    // On-schedule blocks after the anchor keep its target.
    const uint32_t anchor_bits{blocks[anchor_height].nBits};
    int h{anchor_height + 1};
    for (; h <= anchor_height + 300; ++h) add_block(h, time += spacing);
    BOOST_CHECK_EQUAL(blocks[h - 1].nBits, anchor_bits);

    // Blocks arriving twice as fast raise the difficulty...
    for (; h <= anchor_height + 1000; ++h) add_block(h, time += spacing / 2);
    BOOST_CHECK(target_of(blocks[h - 1]) < target_of(blocks[anchor_height]) / 4);

    // ...and a stall lowers it again, by a factor of two per half-life.
    add_block(h, time += spacing);
    add_block(h + 1, time += params.asert_half_life + spacing);
    add_block(h + 2, time += spacing);
    BOOST_CHECK_CLOSE(target_of(blocks[h + 2]).getdouble() / target_of(blocks[h + 1]).getdouble(), 2.0, 0.1);
}

/* aserti3: slow bootstrap blocks leave the anchor at the proof of work limit. */
BOOST_AUTO_TEST_CASE(asert_slow_bootstrap)
{
    const auto chain_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    const Consensus::Params params{WithInheritedRules(chain_params->GetConsensus())};
    const int anchor_height{params.asert_anchor_height};
    const uint32_t limit_bits{UintToArith256(params.powLimit).GetCompact()};

    std::vector<CBlockIndex> blocks(anchor_height + 1);
    blocks[0].nTime = 1790879567;
    blocks[0].nBits = limit_bits;
    for (int h{1}; h <= anchor_height; ++h) {
        CBlockHeader header;
        blocks[h].pprev = &blocks[h - 1];
        blocks[h].nHeight = h;
        blocks[h].nTime = blocks[h - 1].nTime + 3 * params.nPowTargetSpacing;
        header.nTime = blocks[h].nTime;
        blocks[h].nBits = GetNextWorkRequired(&blocks[h - 1], &header, params);
        blocks[h].BuildSkip();
    }
    BOOST_CHECK_EQUAL(blocks[anchor_height].nBits, limit_bits);
}

/* aserti3: more than sixteen half-lives behind schedule, a small target is shifted left whole, without reaching the limit. */
BOOST_AUTO_TEST_CASE(asert_large_shift)
{
    const arith_uint256 pow_limit{UintToArith256(uint256{"00000000ffffffffffffffffffffffffffffffffffffffffffffffffffffffff"})};
    const int64_t spacing{60}, half_life{7200};
    // 20 half-lives: 2^20 times the target, exactly (the fractional part is zero).
    BOOST_CHECK(CalculateASERT(arith_uint256{1}, spacing, spacing + 20 * half_life, 0, pow_limit, half_life) == arith_uint256{1} << 20);
    BOOST_CHECK(CalculateASERT(arith_uint256{3}, spacing, spacing + 17 * half_life, 0, pow_limit, half_life) == arith_uint256{3} << 17);
    // Half a half-life more: within the error of the cubic approximation of the square root of two.
    const arith_uint256 half{CalculateASERT(arith_uint256{1} << 10, spacing, spacing + 20 * half_life + half_life / 2, 0, pow_limit, half_life)};
    BOOST_CHECK(half > (arith_uint256{1} << 30) * 14140 / 10000);
    BOOST_CHECK(half < (arith_uint256{1} << 30) * 14145 / 10000);
    // Shifted past 256 bits: the limit.
    BOOST_CHECK(CalculateASERT(pow_limit >> 8, spacing, spacing + 300 * half_life, 0, pow_limit, half_life) == pow_limit);
}

/* aserti3 on test networks: a block more than two target spacings after its parent may be mined at the limit; others follow aserti3. */
BOOST_AUTO_TEST_CASE(asert_min_difficulty_blocks)
{
    const auto chain_params{CreateChainParams(*m_node.args, ChainType::TESTNET)};
    Consensus::Params params{chain_params->GetConsensus()};
    if (params.sidechain.enabled) {
        // The blocks of a sidechain carry no work of their own (see CChainParams::MakeSidechain): the
        // rule is checked with the test network's parameters as the mainchain has them.
        BOOST_REQUIRE(!params.fPowAllowMinDifficultyBlocks && params.fPowNoRetargeting);
        params.fPowAllowMinDifficultyBlocks = true;
        params.fPowNoRetargeting = false;
    }
    BOOST_REQUIRE(params.fPowAllowMinDifficultyBlocks);
    BOOST_REQUIRE(params.asert_half_life > 0);
    const int anchor_height{params.asert_anchor_height};
    const uint32_t limit_bits{UintToArith256(params.powLimit).GetCompact()};
    const int64_t spacing{params.nPowTargetSpacing};

    std::vector<CBlockIndex> blocks(anchor_height + 10);
    blocks[0].nTime = 1790879567;
    blocks[0].nBits = limit_bits;
    // Bootstrap blocks four times faster than the target: the anchor is harder than the limit.
    for (int h{1}; h <= anchor_height + 1; ++h) {
        CBlockHeader header;
        blocks[h].pprev = &blocks[h - 1];
        blocks[h].nHeight = h;
        blocks[h].nTime = blocks[h - 1].nTime + (h == 1 ? 1000 : spacing / 4);
        header.nTime = blocks[h].nTime;
        blocks[h].nBits = GetNextWorkRequired(&blocks[h - 1], &header, params);
        blocks[h].BuildSkip();
    }
    const CBlockIndex& last{blocks[anchor_height + 1]};
    BOOST_REQUIRE(last.nBits != limit_bits);
    CBlockHeader header;
    // Exactly two spacings after its parent: aserti3.
    header.nTime = last.nTime + 2 * spacing;
    const uint32_t asert_bits{GetNextWorkRequired(&last, &header, params)};
    BOOST_CHECK(asert_bits != limit_bits);
    // One second more: the limit.
    header.nTime = last.nTime + 2 * spacing + 1;
    BOOST_CHECK_EQUAL(GetNextWorkRequired(&last, &header, params), limit_bits);

    // Not on the main chain.
    const auto main_params{CreateChainParams(*m_node.args, ChainType::MAIN)};
    BOOST_REQUIRE(!main_params->GetConsensus().fPowAllowMinDifficultyBlocks);
    BOOST_CHECK(GetNextWorkRequired(&last, &header, main_params->GetConsensus()) != limit_bits);
}

BOOST_AUTO_TEST_SUITE_END()
