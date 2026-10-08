// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/scdb.h>

#include <consensus/amount.h>
#include <hash.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <algorithm>
#include <limits>
#include <optional>

namespace drivechain {

namespace {

/** Age of something proposed at `proposed_height`, counting the proposing block as one. */
int Age(int proposed_height, int height) { return height - proposed_height + 1; }

} // namespace

const Slot* SidechainDB::GetSlot(SidechainId id) const
{
    const auto it{m_slots.find(id)};
    return it == m_slots.end() ? nullptr : &it->second;
}

const Proposal* SidechainDB::GetProposal(const uint256& hash) const
{
    for (const Proposal& proposal : m_proposals) {
        if (proposal.hash == hash) return &proposal;
    }
    return nullptr;
}

const Proposal* SidechainDB::GetProposal(SidechainId slot, const uint256& hash) const
{
    for (const Proposal& proposal : m_proposals) {
        if (proposal.sidechain.slot == slot && proposal.hash == hash) return &proposal;
    }
    return nullptr;
}

std::vector<Bundle>::const_iterator SidechainDB::WeakestBundle(const std::vector<Bundle>& bundles)
{
    // The lowest score, and of those the oldest: the first in the list, which is oldest first.
    return std::min_element(bundles.begin(), bundles.end(), [](const Bundle& a, const Bundle& b) { return a.score < b.score; });
}

SidechainDB::PendingBundles SidechainDB::GetPendingBundles() const
{
    PendingBundles pending;
    for (const auto& [id, slot] : m_slots) pending.emplace_back(id, slot.bundles);
    return pending;
}

int SidechainDB::BlocksUntilActivation(const Proposal& proposal, bool slot_in_use, int height, const Consensus::DrivechainParams& params)
{
    const int period{slot_in_use ? params.replacement_period : params.activation_period};
    return period - Age(proposal.height, height);
}

int SidechainDB::Failures(const Proposal& proposal, int height)
{
    // The proposing block counts as an ack.
    return std::max(0, Age(proposal.height, height) - 1 - static_cast<int>(proposal.acks));
}

int SidechainDB::BlocksLeft(const Bundle& bundle, int height, const Consensus::DrivechainParams& params)
{
    return params.withdrawal_period - Age(bundle.height, height);
}

bool SidechainDB::UpvoteExpired(const Bundle& bundle, int height, const Consensus::DrivechainParams& params)
{
    // The blocks after the last upvote (or the proposal) and before this one did not upvote it.
    return height >= params.audit2_height && int64_t{height} - 1 - bundle.last_upvote >= params.upvote_expiry_blocks;
}

uint256 SidechainDB::GetHash() const
{
    return (HashWriter{} << *this).GetHash();
}

BlockUndo::SlotUndo& SidechainDB::SaveSlot(SidechainId id, BlockUndo& undo) const
{
    for (BlockUndo::SlotUndo& saved : undo.slots) {
        if (saved.id == id) return saved;
    }
    BlockUndo::SlotUndo& saved{undo.slots.emplace_back()};
    saved.id = id;
    if (const Slot* slot{GetSlot(id)}) {
        saved.existed = true;
        saved.activation_height = slot->activation_height;
        saved.has_ctip = slot->has_ctip;
        saved.ctip = slot->ctip;
    }
    return saved;
}

void SidechainDB::EraseBundle(Slot& slot, SidechainId id, size_t index, BlockUndo& undo)
{
    BlockUndo::BundleChange& change{undo.bundle_changes.emplace_back()};
    change.type = BlockUndo::BundleChange::Type::ERASE;
    change.id = id;
    change.index = static_cast<uint32_t>(index);
    change.bundle = slot.bundles[index];
    slot.bundles.erase(slot.bundles.begin() + index);
}

void SidechainDB::CloseBundle(SidechainId id, const uint256& hash, bool paid, int height, BlockUndo& undo)
{
    if (!m_closed.emplace(std::pair{id, hash}, ClosedBundle{paid, height}).second) return;
    undo.closed.push_back({id, hash, paid});
    if (!paid) m_failed_by_height.emplace(height, id, hash);
}

void SidechainDB::ForgetFailedBundles(int height, const Consensus::DrivechainParams& params, BlockUndo& undo)
{
    if (height < params.audit2_height) return;
    const int64_t last{int64_t{height} - std::max(1, params.withdrawal_period)};
    while (!m_failed_by_height.empty() && std::get<0>(*m_failed_by_height.begin()) <= last) {
        const auto& [failed_height, id, hash]{*m_failed_by_height.begin()};
        const auto closed{m_closed.find({id, hash})};
        undo.forgotten.emplace_back(closed->first, closed->second);
        m_closed.erase(closed);
        m_failed_by_height.erase(m_failed_by_height.begin());
    }
}

void SidechainDB::RemoveProposal(size_t index, BlockUndo& undo)
{
    undo.removed.emplace_back(static_cast<uint32_t>(index), m_proposals[index]);
    m_proposals.erase(m_proposals.begin() + index);
}

SidechainDB SidechainDB::Subset(const std::set<SidechainId>& slots) const
{
    SidechainDB subset;
    subset.m_block_hash = m_block_hash;
    for (const SidechainId id : slots) {
        if (const auto it{m_slots.find(id)}; it != m_slots.end()) subset.m_slots.emplace(id, it->second);
    }
    return subset;
}

SidechainDB::EscrowOutputs SidechainDB::GetEscrowOutputs() const
{
    EscrowOutputs escrow_outputs;
    for (const auto& [id, slot] : m_slots) {
        if (slot.has_ctip) escrow_outputs.emplace(slot.ctip.outpoint, id);
    }
    return escrow_outputs;
}

bool SidechainDB::ConnectTx(const CTransaction& tx, const Consensus::DrivechainParams& params, EscrowOutputs& escrow_outputs,
                            BlockUndo& undo, std::optional<Deposit>* deposit, std::string& reject_reason, int height)
{
    const auto invalid = [&](const char* reason) {
        reject_reason = reason;
        return false;
    };

    // The escrow output this transaction creates, if any.
    std::optional<SidechainId> out_slot;
    uint32_t burn_index{0};
    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        const auto slot{ParseEscrowScript(tx.vout[i].scriptPubKey)};
        if (!slot || *slot >= params.max_sidechains) continue;
        if (out_slot) return invalid("bad-dc-multiple-escrow-outputs");
        out_slot = *slot;
        burn_index = i;
    }

    // The escrow output this transaction spends, if any.
    std::optional<SidechainId> in_slot;
    for (const CTxIn& in : tx.vin) {
        const auto spent{escrow_outputs.find(in.prevout)};
        if (spent == escrow_outputs.end()) continue;
        if (in_slot) return invalid("bad-dc-multiple-escrow-inputs");
        in_slot = spent->second;
    }

    if (!out_slot && !in_slot) return true;
    // Coins leave the escrow only through a withdrawal, which returns the remainder to the escrow.
    if (!out_slot) return invalid("bad-dc-escrow-spend");

    const SidechainId id{*out_slot};
    const auto it{m_slots.find(id)};
    if (it == m_slots.end()) return invalid("bad-dc-inactive-sidechain");
    // A sidechain has a single escrow output: the new one has to replace the current one.
    if (in_slot && *in_slot != id) return invalid("bad-dc-escrow-mismatch");
    if (it->second.has_ctip && !in_slot) return invalid("bad-dc-escrow-unspent");

    SaveSlot(id, undo);
    Slot& slot{it->second};

    const CAmount old_amount{slot.has_ctip ? slot.ctip.amount : 0};
    const CAmount new_amount{tx.vout[burn_index].nValue};
    if (!MoneyRange(new_amount)) return invalid("bad-dc-escrow-amount");

    std::string destination;
    uint256 paid_bundle;
    if (new_amount > old_amount) {
        // Deposit (M5). The output after the treasury output names the destination on the sidechain.
        std::optional<std::string> dest;
        if (burn_index + 1 < tx.vout.size()) dest = ParseDestinationScript(tx.vout[burn_index + 1].scriptPubKey);
        if (!dest || dest->empty() || dest->size() > MAX_DEPOSIT_DESTINATION_SIZE || *dest == WITHDRAWAL_RETURN_DEST) {
            return invalid("bad-dc-deposit-destination");
        }
        destination = *dest;
    } else if (new_amount < old_amount) {
        // Withdrawal (M6): the treasury output is the only input, the new
        // treasury output is output 0, the payouts follow, and whatever else
        // left the treasury is the fee for mainchain miners.
        if (tx.vin.size() != 1) return invalid("bad-dc-withdrawal-inputs");
        if (tx.vout.size() < 2 || burn_index != 0) return invalid("bad-dc-withdrawal-outputs");

        const auto blind_hash{BlindWithdrawalHash(tx, old_amount)};
        if (!blind_hash) return invalid("bad-dc-withdrawal-amount");
        const auto bundle{std::find_if(slot.bundles.begin(), slot.bundles.end(), [&](const Bundle& b) { return b.hash == *blind_hash; })};
        if (bundle == slot.bundles.end()) return invalid("bad-dc-withdrawal-unknown");
        if (bundle->score < static_cast<uint32_t>(params.withdrawal_min_score)) return invalid("bad-dc-withdrawal-score");

        EraseBundle(slot, id, bundle - slot.bundles.begin(), undo);
        CloseBundle(id, *blind_hash, /*paid=*/true, height, undo);
        if (height >= params.single_payout_height) {
            // The other bundles of the sidechain are copies holding the same withdrawals: they fail.
            for (const Bundle& other : slot.bundles) CloseBundle(id, other.hash, /*paid=*/false, height, undo);
            for (size_t i{slot.bundles.size()}; i-- > 0;) EraseBundle(slot, id, i, undo);
        }
        destination = WITHDRAWAL_RETURN_DEST;
        paid_bundle = *blind_hash;
    } else {
        return invalid("bad-dc-escrow-amount");
    }

    if (slot.has_ctip) escrow_outputs.erase(slot.ctip.outpoint);
    slot.has_ctip = true;
    slot.ctip.outpoint = COutPoint{tx.GetHash(), burn_index};
    slot.ctip.amount = new_amount;
    escrow_outputs.emplace(slot.ctip.outpoint, id);

    if (deposit) {
        deposit->emplace();
        (*deposit)->slot = id;
        (*deposit)->destination = destination;
        (*deposit)->burn_index = burn_index;
        (*deposit)->amount = new_amount > old_amount ? new_amount - old_amount : 0;
        (*deposit)->total = new_amount;
        (*deposit)->bundle = paid_bundle;
    }
    return true;
}

bool SidechainDB::ConnectBlock(const CBlock& block, int height, const Consensus::DrivechainParams& params,
                               BlockUndo& undo, std::vector<Deposit>* deposits, std::string& reject_reason,
                               const SideContext* side)
{
    const auto invalid = [&](const char* reason) {
        reject_reason = reason;
        return false;
    };

    // The undo data is complete at every point of failure as well: a block that fails is taken
    // back with what it has (see Chainstate::ConnectBlock), not by keeping a copy of the database.
    undo = BlockUndo{};
    undo.prev_block_hash = m_block_hash;
    undo.last_votes = m_last_votes;

    if (side) {
        if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) {
            reject_reason = "bad-dc-no-coinbase";
            return false;
        }
        sidechain::State state{side->store};
        if (!state.ConnectBlock(block, height, side->params, side->mainchain, side->minted, reject_reason)) {
            side->failed = true;
            return false;
        }
        undo.side = side->store.TakeUndo();
    }

    if (block.vtx.empty() || !block.vtx[0]->IsCoinBase()) return invalid("bad-dc-no-coinbase");
    const uint256 block_hash{block.GetHash()};

    //
    // Read the messages in the coinbase. Messages for slots this software does
    // not know about, and messages that do not parse, are ignored so that later
    // versions can give them a meaning.
    //
    std::vector<std::pair<Sidechain, uint256>> new_proposals;
    std::map<SidechainId, uint256> acks;
    std::map<SidechainId, uint256> new_bundles;
    std::optional<VoteMessage> votes;
    std::map<SidechainId, uint256> bmm_accepts;

    for (const CTxOut& out : block.vtx[0]->vout) {
        const CScript& script{out.scriptPubKey};

        if (const auto slot{ParseEscrowScript(script)}; slot && *slot < params.max_sidechains) {
            // A coinbase cannot be a deposit: it cannot spend the previous escrow output.
            return invalid("bad-dc-coinbase-escrow");
        }
        if (script.empty() || script[0] != OP_RETURN) continue;

        if (const auto proposal{ParseProposalScript(script)}) {
            if (!proposal->IsValid(params.max_sidechains)) continue;
            // A block makes a handful of proposals at most; the rest are not looked at, so that a
            // coinbase full of them costs nobody anything.
            if (new_proposals.size() >= MAX_PROPOSALS_PER_BLOCK) continue;
            const uint256 hash{proposal->GetHash()};
            for (const auto& [other, other_hash] : new_proposals) {
                if (other.slot == proposal->slot && other_hash == hash) return invalid("bad-dc-duplicate-proposal");
            }
            new_proposals.emplace_back(*proposal, hash);
        } else if (const auto ack{ParseAckScript(script)}) {
            if (ack->first >= params.max_sidechains) continue;
            // A block can support one proposal per slot.
            if (!acks.emplace(ack->first, ack->second).second) return invalid("bad-dc-multiple-acks");
        } else if (const auto bundle{ParseBundleScript(script)}) {
            if (bundle->first >= params.max_sidechains) continue;
            if (!new_bundles.emplace(bundle->first, bundle->second).second) return invalid("bad-dc-multiple-bundles");
        } else if (const auto parsed_votes{ParseVoteScript(script)}) {
            if (votes) return invalid("bad-dc-multiple-votes");
            votes = *parsed_votes;
        } else if (const auto accept{ParseBmmAcceptScript(script)}) {
            if (accept->first >= params.max_sidechains) continue;
            if (!IsActive(accept->first)) return invalid("bad-dc-bmm-inactive-sidechain");
            if (!bmm_accepts.emplace(accept->first, accept->second).second) return invalid("bad-dc-multiple-bmm-accepts");
        }
    }

    // Votes refer to the sidechains and bundles as they were before this block.
    const PendingBundles votable{GetPendingBundles()};

    //
    // Transactions: deposits (M5), withdrawals (M6) and BMM requests (M8).
    //
    EscrowOutputs escrow_outputs{GetEscrowOutputs()};
    std::set<SidechainId> bmm_requested;

    for (size_t tx_index{1}; tx_index < block.vtx.size(); ++tx_index) {
        const CTransaction& tx{*block.vtx[tx_index]};

        for (const BmmRequest& request : GetBmmRequests(tx)) {
            if (request.slot >= params.max_sidechains) continue;
            // A request is only good for one block: the one mined on top of the block it names.
            if (request.prev_main_block_hash != block.hashPrevBlock) return invalid("bad-dc-bmm-prev-block");
            const auto accept{bmm_accepts.find(request.slot)};
            if (accept == bmm_accepts.end() || accept->second != request.side_block_hash) return invalid("bad-dc-bmm-not-accepted");
            if (!bmm_requested.insert(request.slot).second) return invalid("bad-dc-multiple-bmm-requests");
        }

        std::optional<Deposit> deposit;
        if (!ConnectTx(tx, params, escrow_outputs, undo, &deposit, reject_reason, height)) return false;
        if (deposit && deposits) {
            deposit->tx = block.vtx[tx_index];
            deposit->tx_index = static_cast<uint32_t>(tx_index);
            deposit->block_hash = block_hash;
            deposits->push_back(std::move(*deposit));
        }
    }

    //
    // Sidechain proposals (M1) and acks (M2).
    //
    for (const auto& [sidechain, hash] : new_proposals) {
        // Proposing again what is already proposed would reset its acks: such a proposal is ignored.
        if (GetProposal(sidechain.slot, hash)) continue;
        // Proposing the sidechain a slot already has would replace it with itself, failing the
        // bundles pending there; miners that acked it once would ack it again. It is ignored.
        if (const Slot* slot{GetSlot(sidechain.slot)}; slot && slot->sidechain.GetHash() == hash) continue;
        Proposal proposal;
        proposal.sidechain = sidechain;
        proposal.hash = hash;
        proposal.height = height;
        m_proposals.push_back(std::move(proposal));
        ++undo.proposals_added;
    }

    for (const auto& [ack_slot, hash] : acks) {
        const auto proposal{std::find_if(m_proposals.begin(), m_proposals.end(), [&](const Proposal& p) { return p.sidechain.slot == ack_slot && p.hash == hash; })};
        // An ack of nothing that is proposed is ignored.
        if (proposal == m_proposals.end()) continue;
        // The block that proposes a sidechain already counts as an ack of it.
        if (proposal->height == height) continue;
        ++proposal->acks;
        undo.acked.emplace_back(ack_slot, hash);
    }

    // Proposals that ran out of time.
    for (size_t i{0}; i < m_proposals.size();) {
        if (BlocksUntilActivation(m_proposals[i], IsActive(m_proposals[i].sidechain.slot), height, params) < 0) {
            RemoveProposal(i, undo);
        } else {
            ++i;
        }
    }
    // Proposals that too many blocks did not ack.
    for (size_t i{0}; i < m_proposals.size();) {
        if (Failures(m_proposals[i], height) >= params.activation_max_failures) {
            RemoveProposal(i, undo);
        } else {
            ++i;
        }
    }
    // Proposals that made it.
    for (size_t i{0}; i < m_proposals.size();) {
        const SidechainId id{m_proposals[i].sidechain.slot};
        if (BlocksUntilActivation(m_proposals[i], IsActive(id), height, params) != 0) {
            ++i;
            continue;
        }
        BlockUndo::SlotUndo& saved{SaveSlot(id, undo)};
        Slot& slot{m_slots[id]};
        // Only the first activation in the block replaces the sidechain the slot had before it.
        if (saved.existed && !saved.sidechain) saved.sidechain = slot.sidechain;
        // A sidechain that replaces another one takes over its escrow, but not its pending
        // withdrawals: they fail, so that the software of the old sidechain gives them back.
        for (const Bundle& bundle : slot.bundles) CloseBundle(id, bundle.hash, /*paid=*/false, height, undo);
        for (size_t b{slot.bundles.size()}; b-- > 0;) EraseBundle(slot, id, b, undo);
        slot.sidechain = m_proposals[i].sidechain;
        slot.activation_height = height;
        RemoveProposal(i, undo);
    }

    //
    // Withdrawal bundles: expiry, votes (M4) and new bundles (M3).
    //
    for (auto& [id, slot] : m_slots) {
        for (size_t i{0}; i < slot.bundles.size();) {
            const Bundle& bundle{slot.bundles[i]};
            // Blocks left before this one was connected.
            const int blocks_left{BlocksLeft(bundle, height, params) + 1};
            // Idle: nobody vouches for it (see DrivechainParams::idle_expiry_blocks).
            const bool idle{height >= params.idle_expiry_height && bundle.score == 0 && Age(bundle.height, height) >= params.idle_expiry_blocks};
            const bool expired{blocks_left <= 0 || idle || UpvoteExpired(bundle, height, params) ||
                               params.withdrawal_min_score - static_cast<int64_t>(bundle.score) > blocks_left};
            if (!expired) {
                ++i;
                continue;
            }
            CloseBundle(id, bundle.hash, /*paid=*/false, height, undo);
            EraseBundle(slot, id, i, undo);
        }
    }

    // The vote (M4) of this block, per sidechain. A block without one abstains everywhere.
    std::map<SidechainId, Vote> block_votes;
    if (votes) {
        switch (votes->form) {
        case VoteForm::REPEAT_PREVIOUS:
            block_votes = m_last_votes;
            break;
        case VoteForm::LEADING_BY_50:
            for (const auto& [id, bundles] : votable) {
                const Bundle* leader{nullptr};
                uint32_t second{0};
                for (const Bundle& bundle : bundles) {
                    if (!leader || bundle.score > leader->score) {
                        if (leader) second = std::max(second, leader->score);
                        leader = &bundle;
                    } else {
                        second = std::max(second, bundle.score);
                    }
                }
                if (!leader || leader->score < second + LEADING_BY_50_MARGIN) continue;
                Vote vote;
                vote.type = Vote::Type::UPVOTE;
                vote.bundle = leader->hash;
                block_votes[id] = vote;
            }
            break;
        case VoteForm::ONE_BYTE:
        case VoteForm::TWO_BYTES:
            if (votes->votes.size() != votable.size()) return invalid("bad-dc-votes-size");
            // The two byte form is only for votes that do not fit in one.
            if (votes->form == VoteForm::TWO_BYTES && std::all_of(votes->votes.begin(), votes->votes.end(), [](uint16_t v) { return v <= VOTE_MAX_ONE_BYTE_INDEX; })) {
                return invalid("bad-dc-votes-form");
            }
            for (size_t i{0}; i < votes->votes.size(); ++i) {
                const uint16_t entry{votes->votes[i]};
                const auto& [id, bundles]{votable[i]};
                if (entry == VOTE_ABSTAIN) continue;
                Vote vote;
                if (entry == VOTE_DOWNVOTE) {
                    vote.type = Vote::Type::DOWNVOTE;
                } else {
                    if (entry >= bundles.size()) return invalid("bad-dc-votes-index");
                    vote.type = Vote::Type::UPVOTE;
                    vote.bundle = bundles[entry].hash;
                }
                block_votes[id] = vote;
            }
            break;
        }
    }

    // Upvoting a bundle downvotes the other bundles of its sidechain; a downvote downvotes them all.
    m_last_votes.clear();
    for (const auto& [id, vote] : block_votes) {
        const auto it{m_slots.find(id)};
        if (it == m_slots.end() || it->second.bundles.empty()) continue;
        Slot& slot{it->second};
        if (vote.type == Vote::Type::UPVOTE) {
            // A repeated upvote of a bundle that is no longer pending casts no vote.
            const bool pending{std::any_of(slot.bundles.begin(), slot.bundles.end(), [&](const Bundle& b) { return b.hash == vote.bundle; })};
            if (!pending) continue;
        } else if (vote.type != Vote::Type::DOWNVOTE) {
            continue;
        }
        BlockUndo::BundleChange& change{undo.bundle_changes.emplace_back()};
        change.type = BlockUndo::BundleChange::Type::VOTE;
        change.id = id;
        for (size_t i{0}; i < slot.bundles.size(); ++i) {
            Bundle& bundle{slot.bundles[i]};
            if (vote.type == Vote::Type::UPVOTE && bundle.hash == vote.bundle) {
                change.upvoted = static_cast<uint32_t>(i);
                change.last_upvote = bundle.last_upvote;
                change.saturated = bundle.score == std::numeric_limits<uint32_t>::max();
                if (!change.saturated) ++bundle.score;
                bundle.last_upvote = height;
            } else if (bundle.score > 0) {
                --bundle.score;
            } else {
                change.at_zero.push_back(static_cast<uint32_t>(i));
            }
        }
        m_last_votes[id] = vote;
    }

    for (const auto& [id, hash] : new_bundles) {
        const auto it{m_slots.find(id)};
        if (it == m_slots.end()) continue;
        Slot& slot{it->second};
        if (std::any_of(slot.bundles.begin(), slot.bundles.end(), [&](const Bundle& b) { return b.hash == hash; })) return invalid("bad-dc-bundle-known");
        if (IsClosed(id, hash)) return invalid("bad-dc-bundle-closed");
        if (slot.bundles.size() >= params.max_pending_bundles) {
            // A full queue makes room by failing its weakest bundle, if that one has no more than the
            // vote a new bundle starts with: otherwise a miner with a few blocks could fill it with
            // bundles nobody votes for, which live for the whole withdrawal period, and so keep every
            // honest bundle out. A bundle the miners vote for is never pushed out.
            const auto weakest{WeakestBundle(slot.bundles)};
            if (weakest == slot.bundles.end() || weakest->score > NEW_BUNDLE_SCORE) return invalid("bad-dc-too-many-bundles");
            CloseBundle(id, weakest->hash, /*paid=*/false, height, undo);
            EraseBundle(slot, id, weakest - slot.bundles.begin(), undo);
        }
        Bundle bundle;
        bundle.hash = hash;
        bundle.height = height;
        bundle.score = NEW_BUNDLE_SCORE;
        bundle.last_upvote = height;
        slot.bundles.push_back(bundle);
        BlockUndo::BundleChange& change{undo.bundle_changes.emplace_back()};
        change.type = BlockUndo::BundleChange::Type::APPEND;
        change.id = id;
        undo.proposed.emplace_back(id, hash);
    }

    ForgetFailedBundles(height, params, undo);

    m_block_hash = block_hash;
    return true;
}

void SidechainDB::DisconnectBlock(const BlockUndo& undo)
{
    // The sidechain state is reverted by whoever holds its store, with undo.side.
    for (const auto& [key, closed] : undo.forgotten) {
        m_closed.emplace(key, closed);
        if (!closed.paid) m_failed_by_height.emplace(closed.height, key.first, key.second);
    }
    // The bundles, from the last change back; the slots they belong to still exist.
    for (auto it{undo.bundle_changes.rbegin()}; it != undo.bundle_changes.rend(); ++it) {
        const BlockUndo::BundleChange& change{*it};
        std::vector<Bundle>& bundles{m_slots.at(change.id).bundles};
        switch (change.type) {
        case BlockUndo::BundleChange::Type::ERASE:
            bundles.insert(bundles.begin() + change.index, change.bundle);
            break;
        case BlockUndo::BundleChange::Type::APPEND:
            bundles.pop_back();
            break;
        case BlockUndo::BundleChange::Type::VOTE: {
            auto zero{change.at_zero.begin()};
            for (size_t i{0}; i < bundles.size(); ++i) {
                Bundle& bundle{bundles[i]};
                if (i == change.upvoted) {
                    if (!change.saturated) --bundle.score;
                    bundle.last_upvote = change.last_upvote;
                } else if (zero != change.at_zero.end() && *zero == i) {
                    ++zero;
                } else {
                    ++bundle.score;
                }
            }
            break;
        }
        }
    }
    for (const BlockUndo::SlotUndo& saved : undo.slots) {
        if (!saved.existed) {
            m_slots.erase(saved.id);
            continue;
        }
        Slot& slot{m_slots.at(saved.id)};
        slot.activation_height = saved.activation_height;
        slot.has_ctip = saved.has_ctip;
        slot.ctip = saved.ctip;
        if (saved.sidechain) slot.sidechain = *saved.sidechain;
    }
    for (const BlockUndo::Closed& closed : undo.closed) {
        const auto it{m_closed.find({closed.id, closed.hash})};
        if (it == m_closed.end()) continue;
        if (!it->second.paid) m_failed_by_height.erase({it->second.height, closed.id, closed.hash});
        m_closed.erase(it);
    }
    for (auto it{undo.removed.rbegin()}; it != undo.removed.rend(); ++it) {
        m_proposals.insert(m_proposals.begin() + it->first, it->second);
    }
    for (const auto& [slot, hash] : undo.acked) {
        for (Proposal& proposal : m_proposals) {
            if (proposal.sidechain.slot == slot && proposal.hash == hash) {
                --proposal.acks;
                break;
            }
        }
    }
    m_proposals.resize(m_proposals.size() - undo.proposals_added);
    m_last_votes = undo.last_votes;
    m_block_hash = undo.prev_block_hash;
}

} // namespace drivechain
