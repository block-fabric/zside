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
constexpr uint32_t MINER_STATE_VERSION{2};
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
        if (version != MINER_STATE_VERSION) throw std::ios_base::failure("unknown version");
        std::vector<std::pair<std::pair<SidechainId, uint256>, CMutableTransaction>> bundles;
        uint64_t bundle_count;
        uint8_t default_vote;
        file >> m_proposals >> m_acks >> m_votes >> default_vote >> bundle_count;
        m_default_vote = default_vote == static_cast<uint8_t>(Vote::Type::DOWNVOTE) ? Vote::Type::DOWNVOTE : Vote::Type::ABSTAIN;
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
        m_default_vote = Vote::Type::ABSTAIN;
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
        file << MINER_STATE_VERSION << m_proposals << m_acks << m_votes << static_cast<uint8_t>(m_default_vote) << uint64_t{m_bundles.size()};
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
    m_acks.insert(sidechain.GetHash());
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

void MinerState::SetAck(const uint256& proposal_hash, bool ack)
{
    LOCK(m_mutex);
    if (ack) {
        m_acks.insert(proposal_hash);
    } else {
        m_acks.erase(proposal_hash);
    }
    Save();
}

bool MinerState::IsAcked(const uint256& proposal_hash) const
{
    LOCK(m_mutex);
    return m_acks.contains(proposal_hash);
}

std::set<uint256> MinerState::GetAcks() const
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
    if (m_bundles.emplace(std::make_pair(slot, hash), blind_tx).second) Save();
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
    // There is no bundle to name by default, so the default can only be to abstain or to downvote.
    vote.type = m_default_vote == Vote::Type::DOWNVOTE ? Vote::Type::DOWNVOTE : Vote::Type::ABSTAIN;
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

Vote::Type MinerState::GetDefaultVote() const
{
    LOCK(m_mutex);
    return m_default_vote;
}

void MinerState::Prune(const SidechainDB& scdb)
{
    LOCK(m_mutex);
    if (std::erase_if(m_bundles, [&](const auto& entry) { return scdb.IsClosed(entry.first.first, entry.first.second); }) > 0) Save();
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
        const uint256 hash{proposal.sidechain.GetHash()};
        if (!IsAcked(hash)) continue;
        if (!acked_slots.insert(proposal.sidechain.slot).second) continue;
        add_message(AckScript(proposal.sidechain.slot, hash));
    }

    // M3: bundles this node knows about and the chain does not.
    std::set<SidechainId> proposed_slots;
    for (const auto& [slot, hash] : GetBundles()) {
        const Slot* state{scdb.GetSlot(slot)};
        if (!state || proposed_slots.contains(slot)) continue;
        if (scdb.IsClosed(slot, hash)) continue;
        if (std::any_of(state->bundles.begin(), state->bundles.end(), [&](const Bundle& b) { return b.hash == hash; })) continue;
        if (state->bundles.size() >= params.max_pending_bundles) continue;
        add_message(BundleScript(slot, hash));
        proposed_slots.insert(slot);
    }

    // M4: votes, one entry per active sidechain.
    std::vector<uint16_t> votes;
    bool any_vote{false};
    for (const auto& [slot, bundles] : scdb.GetPendingBundles()) {
        const Vote vote{GetVote(slot)};
        uint16_t entry{VOTE_ABSTAIN};
        if (vote.type == Vote::Type::DOWNVOTE) {
            entry = VOTE_DOWNVOTE;
        } else if (vote.type == Vote::Type::UPVOTE) {
            for (size_t i{0}; i < bundles.size(); ++i) {
                if (bundles[i].hash == vote.bundle) entry = static_cast<uint16_t>(i);
            }
        }
        votes.push_back(entry);
        if (entry != VOTE_ABSTAIN) any_vote = true;
    }
    // A block without a vote message abstains on every sidechain.
    if (any_vote) add_message(VoteScript(MakeVoteMessage(votes)));

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
        // The mempool only holds transactions that pass this.
        (void)after.ConnectTx(*tx, params, escrow_outputs, /*allow_withdrawal=*/false, undo, nullptr, reject_reason);
    }

    // M6: pay out the bundles that have the work score.
    for (const auto& [slot, state] : scdb.GetSlots()) {
        for (const Bundle& bundle : state.bundles) {
            if (bundle.score < static_cast<uint32_t>(params.withdrawal_min_score)) continue;
            const auto blind_tx{GetBundle(slot, bundle.hash)};
            if (!blind_tx) continue;
            const Slot* current{after.GetSlot(slot)};
            if (!current || !current->has_ctip) continue;

            const auto fee{ParseWithdrawalFeeScript(blind_tx->vout[0].scriptPubKey)};
            CAmount paid_out{0};
            for (const CTxOut& out : blind_tx->vout) paid_out += out.nValue;
            if (!fee || paid_out + *fee > current->ctip.amount) continue;

            // The treasury output is the only input, and the new one takes the place of the fee output.
            CMutableTransaction mtx{*blind_tx};
            mtx.vin.assign(1, CTxIn{current->ctip.outpoint});
            mtx.vout[0] = CTxOut{current->ctip.amount - paid_out - *fee, EscrowScript(slot)};
            const CTransactionRef tx{MakeTransactionRef(std::move(mtx))};
            if (!after.ConnectTx(*tx, params, escrow_outputs, /*allow_withdrawal=*/true, undo, nullptr, reject_reason)) continue;
            additions.withdrawals.push_back(tx);
            additions.withdrawal_fees.push_back(*fee);
            // One withdrawal per sidechain per block: the next one needs the new escrow output.
            break;
        }
    }

    return additions;
}

} // namespace drivechain
