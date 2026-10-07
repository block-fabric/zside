// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/miner.h>

#include <logging.h>
#include <streams.h>
#include <util/fs_helpers.h>

#include <algorithm>

namespace drivechain {

namespace {
//! Version of the file the state is kept in.
constexpr uint32_t MINER_STATE_VERSION{4};
} // namespace

bool MinerState::Load(const fs::path& path)
{
    LOCK(m_mutex);
    m_path = path;
    AutoFile file{fsbridge::fopen(path, "rb")};
    if (file.IsNull()) return true; // nothing saved yet
    try {
        uint32_t version;
        file >> version;
        if (version != MINER_STATE_VERSION && version != 3) throw std::ios_base::failure("unknown version");
        std::vector<std::pair<std::pair<SidechainId, uint256>, CMutableTransaction>> bundles;
        uint64_t bundle_count;
        uint8_t default_vote;
        if (version == 3) {
            // Acks were by proposal hash alone, in any slot: they are not carried over, since that
            // is the very thing that changed. Proposals of this node are acked again below.
            std::set<uint256> old_acks;
            file >> m_proposals >> old_acks >> m_votes >> default_vote >> m_follow >> bundle_count;
            m_acks.clear();
            for (const Sidechain& sidechain : m_proposals) m_acks.emplace(sidechain.slot, sidechain.GetHash());
            if (!old_acks.empty()) LogWarning("Acks of sidechain proposals are now by slot: %u acks were dropped, see acksidechain", old_acks.size());
        } else {
            file >> m_proposals >> m_acks >> m_votes >> default_vote >> m_follow >> m_latest_bundle >> bundle_count;
        }
        m_default_vote = default_vote <= static_cast<uint8_t>(Vote::Type::UPVOTE) ? static_cast<Vote::Type>(default_vote) : Vote::Type::ABSTAIN;
        m_bundles.clear();
        for (uint64_t i{0}; i < bundle_count; ++i) {
            SidechainId slot;
            CMutableTransaction tx;
            // A blind withdrawal has no inputs, and so no witness.
            file >> slot >> TX_NO_WITNESS(tx);
            m_bundles.emplace(std::make_pair(slot, tx.GetHash().ToUint256()), std::move(tx));
        }
    } catch (const std::exception& e) {
        LogError("Failed to read the drivechain miner state from %s: %s", fs::PathToString(path), e.what());
        m_proposals.clear();
        m_acks.clear();
        m_votes.clear();
        m_bundles.clear();
        m_latest_bundle.clear();
        m_default_vote = Vote::Type::ABSTAIN;
        m_follow = false;
        return false;
    }
    return true;
}

void MinerState::Save() const
{
    AssertLockHeld(m_mutex);
    if (m_path.empty()) return;
    const fs::path tmp_path{m_path + ".new"};
    AutoFile file{fsbridge::fopen(tmp_path, "wb")};
    if (file.IsNull()) {
        LogError("Failed to open %s to save the drivechain miner state", fs::PathToString(tmp_path));
        return;
    }
    try {
        file << MINER_STATE_VERSION << m_proposals << m_acks << m_votes << static_cast<uint8_t>(m_default_vote) << m_follow << m_latest_bundle << uint64_t{m_bundles.size()};
        for (const auto& [key, tx] : m_bundles) {
            file << key.first << TX_NO_WITNESS(tx);
        }
        if (!file.Commit()) throw std::runtime_error("commit failed");
        if (file.fclose() != 0) throw std::runtime_error("close failed");
        if (!RenameOver(tmp_path, m_path)) throw std::runtime_error("rename failed");
    } catch (const std::exception& e) {
        LogError("Failed to save the drivechain miner state to %s: %s", fs::PathToString(m_path), e.what());
        (void)file.fclose();
    }
}

void MinerState::AddProposal(const Sidechain& sidechain)
{
    LOCK(m_mutex);
    if (std::find(m_proposals.begin(), m_proposals.end(), sidechain) == m_proposals.end()) m_proposals.push_back(sidechain);
    // Whoever proposes a sidechain wants it activated.
    m_acks.emplace(sidechain.slot, sidechain.GetHash());
    Save();
}

bool MinerState::RemoveProposal(const uint256& hash)
{
    LOCK(m_mutex);
    const bool removed{std::erase_if(m_proposals, [&](const Sidechain& s) { return s.GetHash() == hash; }) > 0};
    if (removed) Save();
    return removed;
}

std::vector<Sidechain> MinerState::GetProposals() const
{
    LOCK(m_mutex);
    return m_proposals;
}

void MinerState::SetAck(SidechainId slot, const uint256& proposal_hash, bool ack)
{
    LOCK(m_mutex);
    if (ack) {
        m_acks.emplace(slot, proposal_hash);
    } else {
        m_acks.erase({slot, proposal_hash});
    }
    Save();
}

void MinerState::ClearAcks(const uint256& proposal_hash)
{
    LOCK(m_mutex);
    if (std::erase_if(m_acks, [&](const auto& ack) { return ack.second == proposal_hash; }) > 0) Save();
}

bool MinerState::IsAcked(SidechainId slot, const uint256& proposal_hash) const
{
    LOCK(m_mutex);
    return m_acks.contains({slot, proposal_hash});
}

std::set<std::pair<SidechainId, uint256>> MinerState::GetAcks() const
{
    LOCK(m_mutex);
    return m_acks;
}

std::optional<uint256> MinerState::AddBundle(SidechainId slot, const CMutableTransaction& blind_tx, std::string& error)
{
    if (!IsBlindWithdrawal(CTransaction{blind_tx})) {
        error = "a withdrawal bundle must be the blind form of a withdrawal: no inputs, output 0 OP_RETURN with the fee (8 bytes, big endian) and no value, then the payouts";
        return std::nullopt;
    }
    const uint256 hash{blind_tx.GetHash().ToUint256()};
    LOCK(m_mutex);
    m_bundles.emplace(std::make_pair(slot, hash), blind_tx);
    m_latest_bundle[slot] = hash;
    Save();
    return hash;
}

std::optional<CMutableTransaction> MinerState::GetBundle(SidechainId slot, const uint256& hash) const
{
    LOCK(m_mutex);
    const auto it{m_bundles.find({slot, hash})};
    if (it == m_bundles.end()) return std::nullopt;
    return it->second;
}

std::vector<std::pair<SidechainId, uint256>> MinerState::GetBundles() const
{
    LOCK(m_mutex);
    std::vector<std::pair<SidechainId, uint256>> bundles;
    for (const auto& [key, tx] : m_bundles) bundles.push_back(key);
    return bundles;
}

void MinerState::SetVote(SidechainId slot, const Vote& vote)
{
    LOCK(m_mutex);
    m_votes[slot] = vote;
    Save();
}

void MinerState::ClearVote(SidechainId slot)
{
    LOCK(m_mutex);
    if (m_votes.erase(slot) > 0) Save();
}

Vote MinerState::GetVote(SidechainId slot) const
{
    LOCK(m_mutex);
    const auto it{m_votes.find(slot)};
    if (it != m_votes.end()) return it->second;
    Vote vote;
    // Which bundle the default upvotes depends on the bundles that are pending: see ResolveVote.
    vote.type = m_default_vote;
    return vote;
}

Vote MinerState::ResolveVote(SidechainId slot, const std::vector<Bundle>& pending) const
{
    LOCK(m_mutex);
    const auto it{m_votes.find(slot)};
    if (it != m_votes.end()) return it->second;
    Vote vote;
    vote.type = m_default_vote;
    if (m_default_vote != Vote::Type::UPVOTE) return vote;
    // A sidechain node only hands over the bundle its own chain commits to, so
    // having it is the word of a node the operator runs that the bundle is right.
    // Only the last one handed: one handed before is a bundle its chain gave up
    // (after a reorg of the sidechain, say), and votes for it would count
    // against the bundle that replaced it.
    if (const auto latest{m_latest_bundle.find(slot)}; latest != m_latest_bundle.end()) {
        if (std::any_of(pending.begin(), pending.end(), [&](const Bundle& b) { return b.hash == latest->second; })) {
            vote.bundle = latest->second;
            return vote;
        }
        // Handed but not pending yet (it is proposed in the next block), or no longer pending.
        if (m_bundles.contains({slot, latest->second}) && !m_closed_seen.contains({slot, latest->second})) {
            vote.type = Vote::Type::ABSTAIN;
            return vote;
        }
    }
    if (!m_follow) {
        vote.type = Vote::Type::ABSTAIN;
        return vote;
    }
    // A bundle no sidechain node vouched for to this node, and the operator
    // chose to follow the miners who check it (BIP300 M4 "leading by 50"):
    // upvote the one that leads the next by LEADING_BY_50_MARGIN, so a miner
    // that does not run every sidechain still helps the withdrawals the others back.
    const Bundle* leader{nullptr};
    uint32_t second{0};
    for (const Bundle& bundle : pending) {
        if (!leader || bundle.score > leader->score) {
            if (leader) second = std::max(second, leader->score);
            leader = &bundle;
        } else {
            second = std::max(second, bundle.score);
        }
    }
    if (leader && leader->score >= second + LEADING_BY_50_MARGIN) {
        vote.bundle = leader->hash;
    } else {
        vote.type = Vote::Type::ABSTAIN;
    }
    return vote;
}

std::map<SidechainId, Vote> MinerState::GetVotes() const
{
    LOCK(m_mutex);
    return m_votes;
}

void MinerState::SetDefaultVote(Vote::Type type)
{
    LOCK(m_mutex);
    m_default_vote = type;
    Save();
}

void MinerState::SetFollow(bool follow)
{
    LOCK(m_mutex);
    m_follow = follow;
    Save();
}

bool MinerState::GetFollow() const
{
    LOCK(m_mutex);
    return m_follow;
}

Vote::Type MinerState::GetDefaultVote() const
{
    LOCK(m_mutex);
    return m_default_vote;
}

void MinerState::Prune(const SidechainDB& scdb, int height)
{
    LOCK(m_mutex);
    bool changed{false};
    // A sidechain that activated needs neither proposing nor acking any more; proposed again in its
    // slot it would replace itself, and fail the bundles pending there.
    const auto active{[&](SidechainId slot, const uint256& hash) {
        const Slot* state{scdb.GetSlot(slot)};
        return state && state->sidechain.GetHash() == hash;
    }};
    changed |= std::erase_if(m_proposals, [&](const Sidechain& s) { return active(s.slot, s.GetHash()); }) > 0;
    changed |= std::erase_if(m_acks, [&](const auto& ack) { return active(ack.first, ack.second); }) > 0;
    // Closed bundles go once they are closed deep enough that a reorg is unlikely to reopen them.
    std::erase_if(m_closed_seen, [&](const auto& entry) { return !scdb.IsClosed(entry.first.first, entry.first.second); });
    for (const auto& [key, tx] : m_bundles) {
        if (scdb.IsClosed(key.first, key.second)) m_closed_seen.try_emplace(key, height);
    }
    changed |= std::erase_if(m_bundles, [&](const auto& entry) {
        const auto seen{m_closed_seen.find(entry.first)};
        return seen != m_closed_seen.end() && height - seen->second >= PRUNE_DEPTH;
    }) > 0;
    std::erase_if(m_closed_seen, [&](const auto& entry) { return !m_bundles.contains(entry.first); });
    std::erase_if(m_latest_bundle, [&](const auto& entry) { return !m_bundles.contains({entry.first, entry.second}); });
    if (changed) Save();
}

BlockAdditions MinerState::CreateBlockAdditions(const SidechainDB& scdb, const Consensus::DrivechainParams& params,
                                                const uint256& prev_block_hash, const std::vector<CTransactionRef>& txs) const
{
    BlockAdditions additions;
    const auto add_message = [&](const CScript& script) { additions.coinbase_outputs.emplace_back(0, script); };

    // M1: one proposal per block.
    for (const Sidechain& sidechain : GetProposals()) {
        if (!sidechain.IsValid(params.max_sidechains)) continue;
        if (scdb.GetProposal(sidechain.slot, sidechain.GetHash())) continue;
        if (const Slot* slot{scdb.GetSlot(sidechain.slot)}; slot && slot->sidechain == sidechain) continue;
        add_message(ProposalScript(sidechain));
        break;
    }

    // M2: one ack per slot.
    std::set<SidechainId> acked_slots;
    for (const Proposal& proposal : scdb.GetProposals()) {
        const uint256& hash{proposal.hash};
        if (!IsAcked(proposal.sidechain.slot, hash)) continue;
        // Never ack the sidechain a slot already has: it would replace itself.
        if (const Slot* slot{scdb.GetSlot(proposal.sidechain.slot)}; slot && slot->sidechain.GetHash() == hash) continue;
        if (!acked_slots.insert(proposal.sidechain.slot).second) continue;
        add_message(AckScript(proposal.sidechain.slot, hash));
    }

    // M4: votes, one entry per active sidechain.
    std::vector<uint16_t> votes;
    bool any_vote{false};
    std::map<SidechainId, Vote> block_votes;
    for (const auto& [slot, bundles] : scdb.GetPendingBundles()) {
        const Vote vote{ResolveVote(slot, bundles)};
        uint16_t entry{VOTE_ABSTAIN};
        if (vote.type == Vote::Type::DOWNVOTE) {
            entry = VOTE_DOWNVOTE;
        } else if (vote.type == Vote::Type::UPVOTE) {
            for (size_t i{0}; i < bundles.size(); ++i) {
                if (bundles[i].hash == vote.bundle) entry = static_cast<uint16_t>(i);
            }
        }
        votes.push_back(entry);
        if (entry != VOTE_ABSTAIN) {
            any_vote = true;
            block_votes[slot] = vote;
        }
    }
    // A block without a vote message abstains on every sidechain.
    std::optional<CScript> vote_script;
    if (any_vote) vote_script = VoteScript(MakeVoteMessage(votes));

    // M3: bundles this node knows about and the chain does not.
    std::set<SidechainId> proposed_slots;
    for (const auto& [slot, hash] : GetBundles()) {
        const Slot* state{scdb.GetSlot(slot)};
        if (!state || proposed_slots.contains(slot)) continue;
        if (scdb.IsClosed(slot, hash)) continue;
        if (std::any_of(state->bundles.begin(), state->bundles.end(), [&](const Bundle& b) { return b.hash == hash; })) continue;
        if (state->bundles.size() >= params.max_pending_bundles) {
            // The block's votes count before its new bundles: the queue makes room only if its
            // weakest bundle is still weak enough after this node's own vote.
            std::vector<Bundle> after_votes{state->bundles};
            if (const auto vote{block_votes.find(slot)}; vote != block_votes.end() && vote->second.type != Vote::Type::ABSTAIN) {
                for (Bundle& bundle : after_votes) {
                    if (vote->second.type == Vote::Type::UPVOTE && bundle.hash == vote->second.bundle) {
                        ++bundle.score;
                    } else if (bundle.score > 0) {
                        --bundle.score;
                    }
                }
            }
            const auto weakest{SidechainDB::WeakestBundle(after_votes)};
            if (weakest == after_votes.end() || weakest->score > NEW_BUNDLE_SCORE) continue;
        }
        add_message(BundleScript(slot, hash));
        proposed_slots.insert(slot);
    }
    if (vote_script) add_message(*vote_script);

    // Follow the escrow outputs through the transactions of the block: a
    // withdrawal spends the escrow output as the deposits of the block leave it.
    SidechainDB after{scdb};
    SidechainDB::EscrowOutputs escrow_outputs{after.GetEscrowOutputs()};
    BlockUndo undo;
    std::string reject_reason;
    std::set<SidechainId> bmm_accepted;
    for (const CTransactionRef& tx : txs) {
        // M7: accept the BMM requests in the block.
        for (const BmmRequest& request : GetBmmRequests(*tx)) {
            if (request.slot >= params.max_sidechains || request.prev_main_block_hash != prev_block_hash) continue;
            if (!bmm_accepted.insert(request.slot).second) continue;
            add_message(BmmAcceptScript(request.slot, request.side_block_hash));
        }
        // The mempool only holds transactions that pass this, withdrawals of bundles with the work score included.
        (void)after.ConnectTx(*tx, params, escrow_outputs, undo, nullptr, reject_reason);
    }

    // M6: pay out the bundles that have the work score.
    for (const auto& [slot, state] : scdb.GetSlots()) {
        for (const Bundle& bundle : state.bundles) {
            if (bundle.score < static_cast<uint32_t>(params.withdrawal_min_score)) continue;
            const auto blind_tx{GetBundle(slot, bundle.hash)};
            if (!blind_tx) continue;
            const Slot* current{after.GetSlot(slot)};
            if (!current || !current->has_ctip) continue;

            CAmount fee{0};
            auto mtx{CompleteWithdrawal(CTransaction{*blind_tx}, slot, current->ctip, &fee)};
            if (!mtx) continue;
            const CTransactionRef tx{MakeTransactionRef(std::move(*mtx))};
            // A withdrawal the block already has from the mempool fails here, the bundle being paid.
            if (!after.ConnectTx(*tx, params, escrow_outputs, undo, nullptr, reject_reason)) continue;
            additions.withdrawals.push_back(tx);
            additions.withdrawal_fees.push_back(fee);
            // One withdrawal per sidechain per block: the next one needs the new escrow output.
            break;
        }
    }

    return additions;
}

} // namespace drivechain
