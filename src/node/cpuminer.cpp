// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/cpuminer.h>

#include <arith_uint256.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <crypto/sha256.h>
#include <interfaces/mining.h>
#include <logging.h>
#include <node/context.h>
#include <primitives/block.h>
#include <streams.h>
#include <uint256.h>
#include <util/signalinterrupt.h>
#include <util/threadnames.h>
#include <validation.h>

#include <chrono>
#include <memory>

namespace node {
namespace {
//! Number of hashes between checks of whether the work is still current.
constexpr uint32_t HASHES_PER_CHECK{1 << 16};
//! How long to search on one block template before building a new one.
constexpr auto TEMPLATE_LIFETIME{20s};
//! Minimum time between two blocks found by a thread, which matters where blocks take no work.
//! It is waited out before a block is built, not after: what goes in a block has to follow
//! the block before it. Sidechains ask for a commitment in the block after the one they see,
//! and need a moment to see it.
constexpr auto MIN_BLOCK_INTERVAL{2500ms};
} // namespace

CpuMiner::CpuMiner(NodeContext& node) : m_node{node}, m_template_lifetime{TEMPLATE_LIFETIME} {}

bool CpuMiner::Start(int threads, const CScript& coinbase_output_script, std::string& error)
{
    if (threads < 1) {
        error = "The number of threads must be at least 1";
        return false;
    }
    if (!m_node.mining || !m_node.chainman) {
        error = "The node is not ready to mine";
        return false;
    }
    if (m_node.chainman->GetConsensus().sidechain.enabled) {
        error = "The blocks of a sidechain are mined by the miners of the mainchain; use setbmm instead";
        return false;
    }
    if (m_node.chainman->GetConsensus().signet_blocks) {
        error = "Blocks of a signet need a signature; use contrib/signet/miner instead";
        return false;
    }
    Stop();
    LOCK(m_mutex);
    m_stop = false;
    m_hashes = 0;
    m_sample_hashes = 0;
    m_hashes_per_second = 0;
    m_sample_time = SteadyClock::now();
    m_coinbase_output_script = coinbase_output_script;
    for (int i{0}; i < threads; ++i) {
        m_threads.emplace_back(&CpuMiner::Run, this, i, threads, coinbase_output_script);
    }
    LogInfo("CPU miner started with %d threads", threads);
    return true;
}

void CpuMiner::Stop()
{
    std::vector<std::thread> threads;
    {
        LOCK(m_mutex);
        m_stop = true;
        threads.swap(m_threads);
    }
    for (auto& thread : threads) thread.join();
    if (!threads.empty()) LogInfo("CPU miner stopped");
}

CpuMiner::Stats CpuMiner::GetStats()
{
    LOCK(m_mutex);
    Stats stats;
    stats.running = !m_threads.empty();
    stats.threads = m_threads.size();
    stats.hashes = m_hashes;
    stats.blocks_found = m_blocks_found;
    stats.blocks_rejected = m_blocks_rejected;
    stats.templates = m_templates;
    stats.coinbase_output_script = m_coinbase_output_script;
    if (stats.running) {
        const auto now{SteadyClock::now()};
        const std::chrono::duration<double> elapsed{now - m_sample_time};
        if (elapsed >= 1s) {
            m_hashes_per_second = (stats.hashes - m_sample_hashes) / elapsed.count();
            m_sample_time = now;
            m_sample_hashes = stats.hashes;
        }
        stats.hashes_per_second = m_hashes_per_second;
    }
    return stats;
}

void CpuMiner::Run(int thread, int threads, CScript coinbase_output_script)
{
    util::ThreadRename(strprintf("miner.%i", thread));
    ChainstateManager& chainman{*m_node.chainman};
    const auto stopped{[&] { return m_stop.load() || static_cast<bool>(chainman.m_interrupt); }};
    auto last_block_time{SteadyClock::now() - MIN_BLOCK_INTERVAL};

    while (!stopped()) {
        while (!stopped() && SteadyClock::now() - last_block_time < MIN_BLOCK_INTERVAL) std::this_thread::sleep_for(50ms);
        if (stopped()) break;
        std::unique_ptr<interfaces::BlockTemplate> block_template;
        try {
            block_template = m_node.mining->createNewBlock({.coinbase_output_script = coinbase_output_script}, /*cooldown=*/false);
        } catch (const std::exception& e) {
            LogWarning("CPU miner: cannot create a block template: %s", e.what());
        }
        if (!block_template) {
            std::this_thread::sleep_for(1s);
            continue;
        }
        CBlock block{block_template->getBlock()};
        block.hashMerkleRoot = BlockMerkleRoot(block);

        // The first 64 bytes of the header do not depend on the nonce.
        DataStream stream;
        stream << static_cast<const CBlockHeader&>(block);
        unsigned char header[80];
        Assert(stream.size() == sizeof(header));
        std::copy(stream.begin(), stream.end(), reinterpret_cast<std::byte*>(header));
        CSHA256 midstate;
        midstate.Write(header, 64);

        bool negative, overflow;
        arith_uint256 target;
        target.SetCompact(block.nBits, &negative, &overflow);
        if (negative || overflow || target == 0) return;
        if (m_max_target && target > *m_max_target) target = *m_max_target;
        ++m_templates;

        const auto template_time{SteadyClock::now()};
        // Threads work on the same template, so each takes its share of the nonces.
        uint64_t nonce{static_cast<uint64_t>(thread)};
        bool found{false};
        uint256 hash;
        while (!found && nonce <= std::numeric_limits<uint32_t>::max()) {
            uint32_t count{0};
            for (; count < HASHES_PER_CHECK && nonce <= std::numeric_limits<uint32_t>::max(); ++count, nonce += threads) {
                WriteLE32(header + 76, static_cast<uint32_t>(nonce));
                unsigned char first[CSHA256::OUTPUT_SIZE];
                CSHA256{midstate}.Write(header + 64, 16).Finalize(first);
                CSHA256{}.Write(first, sizeof(first)).Finalize(hash.begin());
                if (UintToArith256(hash) <= target) {
                    found = true;
                    ++count;
                    break;
                }
            }
            m_hashes += count;
            if (found || stopped()) break;
            if (SteadyClock::now() - template_time > m_template_lifetime) break;
            if (WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()) != block.hashPrevBlock) break;
        }
        if (!found) continue;

        block.nNonce = static_cast<uint32_t>(nonce);
        last_block_time = SteadyClock::now();

        bool new_block{false};
        const auto shared_block{std::make_shared<const CBlock>(std::move(block))};
        if (chainman.ProcessNewBlock(shared_block, /*force_processing=*/true, /*min_pow_checked=*/true, &new_block) && new_block) {
            ++m_blocks_found;
            LogInfo("CPU miner found block %s", hash.ToString());
        } else {
            ++m_blocks_rejected;
        }
    }
}

} // namespace node
