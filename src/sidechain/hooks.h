// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_HOOKS_H
#define BITCOIN_SIDECHAIN_HOOKS_H

#include <arith_uint256.h>
#include <consensus/amount.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

class BlockValidationState;
class CBlockIndex;
class ChainstateManager;
class DataStream;
namespace Consensus {
struct Params;
struct SidechainParams;
} // namespace Consensus
namespace node {
struct NodeContext;
} // namespace node

/**
 * Extension points for sidechains whose blocks are not all merged mined: blocks of kinds of their
 * own, a chain order of their own, peg actions in some blocks only, a block producer of their own
 * and messages of their own.
 *
 * Everything here is off unless a sidechain installs hooks of its own (SetHooks) at startup. The
 * defaults are the template's behaviour exactly: every block is merged mined (BlockKind::BMM), takes
 * the peg actions, and the chain with the most work wins. A sidechain that wants more derives from
 * ChainHooks and overrides what it needs.
 */
namespace sidechain {

class Follower;
class Mainchain;
class State;
class StoreView;

/** The kinds of blocks a chain can have. */
enum class BlockKind : uint8_t {
    //! A block the mainchain commits to (blind merged mining), with Bitcoin's header: the template's.
    BMM = 0,
    //! A block of a kind of the sidechain's own: its header is checked by ChainHooks::CheckHeader,
    //! and it has no proof of work.
    PLUGIN = 1,
};

/** What another extension than that of the header the node has is (ChainHooks::CheckOtherExtension). */
enum class OtherExtension : uint8_t {
    //! Not valid: the block is a mutated one.
    INVALID,
    //! As valid as the node's (another valid signature of the same block): the same block.
    EQUIVALENT,
    //! The node cannot tell yet (the header it has was taken unchecked: ChainHooks::HeaderCheckable).
    UNKNOWN,
    //! Valid, and the extension the node has is not: the node takes this one in its place (a header
    //! taken before its signature could be checked, with a forged one).
    REPLACE,
};

/**
 * Where a block takes the peg actions (deposits, bundle outcomes, a new bundle, refunds, payouts):
 * as if it followed the mainchain up to `main_height`, with `nonce` in a bundle it starts.
 */
struct PegStep {
    //! The mainchain block up to which the block acts on what the mainchain did.
    int32_t main_height{0};
    //! The nonce of a bundle the block starts (PendingBundle::nonce): in the template, the block's parent.
    uint256 nonce;
    //! If set, a bundle the block starts takes only withdrawals made at this height or below.
    std::optional<int32_t> max_withdrawal_height;
    /**
     * If set, the height the peg actions count as at, in place of the block's: the lock time of a
     * bundle the block starts, and the height of a bundle failure it sees, from which the retry delay
     * counts. A block that takes the peg actions for another one (its nonce) sets it to that one's
     * height, so that every block taking them for the same one builds the same bundle.
     */
    std::optional<int32_t> peg_height;
    friend bool operator==(const PegStep&, const PegStep&) = default;
};

/**
 * The hooks, with the template's behaviour. Each is called where its comment says; the caller holds
 * cs_main wherever the code it replaces did.
 */
class ChainHooks
{
public:
    virtual ~ChainHooks();

    //
    // Blocks and headers
    //

    /** Whether this chain has extended blocks (BLOCK_EXTENSION_BIT): read once, when the hooks are installed. */
    virtual bool BlockExtensions() const { return false; }
    /** The kind of a block, from its header. */
    virtual BlockKind Kind(const CBlockHeader& header) const;
    /**
     * Check the header of a block of a kind of the sidechain's own, in place of the proof of work and
     * merged mining rules (ContextualCheckBlockHeader). A header that may become valid once the node
     * knows more is BLOCK_TIME_FUTURE, with a reason that IsWaitReason accepts. Called with cs_main held.
     */
    virtual bool CheckHeader(const CBlockHeader& header, const CBlockIndex& prev, const ChainstateManager& chainman, BlockValidationState& state) const;
    /**
     * Whether the node can check every rule of this header now (CheckHeader). A chain whose headers
     * carry a signature the node can check only once it has connected the chain below (the signer is
     * known from that chain's state) says no: the header is then taken *unchecked* (CheckHeader lets
     * it through), within the budget of the peer that sent it (UncheckedLimits), and its signature is
     * checked when its block is connected. Unchecked headers, and every header on top of one, belong
     * to the peers that sent them (ChainstateManager::m_unchecked): they are not written to the block
     * index database, their blocks are stored apart from the block files until connected, and both go
     * when no peer holds them any more. See doc/sidechain.md, "Headers the node
     * cannot check yet". By default every header can be checked. Called with cs_main held.
     */
    virtual bool HeaderCheckable(const CBlockHeader& header, const CBlockIndex& prev) const { return true; }
    struct UncheckedBudget {
        //! The unchecked headers an outbound (or manual) peer may hold.
        size_t per_peer{2000};
        //! The bytes of the stored blocks of the unchecked headers an outbound peer holds.
        size_t block_bytes_per_peer{size_t{32} << 20};
        //! An inbound peer's: much smaller (the honest peer a node syncs from is usually one it chose,
        //! and anyone can open inbound connections). The bytes: enough for a block of the largest size.
        size_t per_inbound_peer{500};
        size_t block_bytes_per_inbound_peer{size_t{8} << 20};
        //! How many inbound peers may hold unchecked headers at once: a further one is refused
        //! (unchecked-budget, not punished) until one of them holds none; none is dropped for it.
        size_t inbound_holders{8};
    };
    /**
     * The budget of unchecked headers (HeaderCheckable). Not consensus. Worst case, node-wide: the
     * outbound peers' budgets (19 such peers at most: 11 automatic ones, full relay, block relay,
     * feelers, address fetches and the extra ones all sharing CConnman::semOutbound, and 8 manual
     * ones, semAddnode) and inbound_holders inbound peers' (see doc/sidechain.md, "Headers the node
     * cannot check yet").
     */
    virtual UncheckedBudget UncheckedLimits() const { return {}; }
    /** Context free checks of a whole block, after Bitcoin's (CheckBlock). */
    virtual bool CheckBlock(const CBlock& block, BlockValidationState& state) const;
    /** What an extended header's hashSections must be for these sections (not empty). */
    virtual uint256 SectionsHash(std::span<const unsigned char> sections) const;
    /**
     * A header or block (or compact block) for an extended header the node has, `index`, with another
     * extension: the block hash covers neither signature. By default INVALID: for a block, a mutated
     * block (AcceptBlock, "bad-blk-extension"), which Bitcoin punishes; for a header, nothing. A chain
     * whose signatures a signer can make twice for one block says EQUIVALENT when the other one is as
     * valid: the block is then taken with the extension the node has (it keeps that one), the peer is
     * not punished. REPLACE: valid, and the one the node has is not (taken unchecked, forged): the
     * node takes this one in its place (Chainstate::ReplaceBlockExtension). UNKNOWN: the node cannot
     * tell yet; the block is the same block, taken with the header's extension, which is checked when
     * it is connected -- unless that one failed already (ExtensionFailure), then this one replaces it.
     * Called with cs_main held.
     */
    virtual OtherExtension CheckOtherExtension(const CBlockHeader& header, const CBlockIndex& index) const;
    /**
     * Whether a block that failed for this reason failed for its extension alone (a signature): if the
     * node took its header unchecked (HeaderCheckable), the failure is that of the copy it has, not of
     * the block. It is no fault of the peers (BLOCK_TIME_FUTURE, "extension-failed"), in memory only,
     * taken back by any other extension, and the node asks the peers that have the block for its
     * header again. By default none.
     */
    virtual bool ExtensionFailure(const std::string& reject_reason) const { return false; }
    /**
     * How far ahead of the node's clock a block's time may be, in seconds (ContextualCheckBlockHeader,
     * "time-too-new", a header that may come later). By default Bitcoin's MAX_FUTURE_BLOCK_TIME. A chain
     * whose blocks come every few seconds may want less: far-ahead times push the median time past
     * ahead, and with it the time every next block must have.
     */
    virtual int64_t MaxFutureBlockTime() const;

    //
    // Chain order
    //

    /**
     * Whether ChainWork is the chain order of the sidechain's own rather than proof of work. If so,
     * header synchronisation does not ask for a minimum of work (headers presync), which would mean nothing.
     */
    virtual bool CustomChainOrder() const { return false; }
    /**
     * The nChainWork of a block with this header on top of `prev` (none for the genesis block): the
     * key by which the chain with the most of it is the best one. It must grow along every chain.
     * Used only if CustomChainOrder; otherwise, and by default, the work of the parent plus that of the header.
     */
    virtual arith_uint256 ChainWork(const CBlockIndex* prev, const CBlockHeader& header) const;
    // (A ChainWork that reads the record of the mainchain is computed anew for the blocks taken back
    // when their commitment comes back, perhaps at another height: Chainstate::ReconsiderRecordFailure.)

    //
    // Headers that wait for the node to know more (the record of the mainchain, say)
    //

    /**
     * Whether a header for a block marked failed against the record of the mainchain, or on top of
     * one, is no reason to punish the peer that sent it (AcceptBlockHeader: BLOCK_TIME_FUTURE,
     * "record-failed"): the peer's record may simply be ahead of this node's, and its chain valid by
     * it. On for every sidechain: a node a little behind on the record would otherwise cut itself off
     * from the peers that are not. A chain that wants Bitcoin's punishment turns it off.
     */
    virtual bool PardonRecordFailures() const { return true; }

    /**
     * Whether a block that fails for `reject_reason` (a reason of the sidechain's own rules) fails
     * whatever the record of the mainchain says: then it is not noted as failed against the record,
     * and is not tried again when the record changes, nor pardoned. In addition to the template's own
     * (sidechain::RecordIndependent). By default none.
     */
    virtual bool RecordIndependent(const std::string& reject_reason) const { return false; }

    /** Whether a header turned away for `reject_reason` may become valid later, and should wait. */
    virtual bool IsWaitReason(const std::string& reject_reason) const;
    /**
     * Whether a waiting header can be tried again, given the record: if so, a key that orders the
     * headers to try (lowest first, so that each comes after its parent). By default the height of
     * the mainchain block that committed to it.
     */
    virtual std::optional<int64_t> WaitingReady(const CBlockHeader& header, const Mainchain& record) const;

    //
    // Mainchain commitments (the Follower)
    //

    /**
     * The hash the mainchain has to commit to for the block to be valid, if it needs one; by
     * default the block's own. A block that needs none is valid whatever commitments come and go.
     */
    virtual std::optional<uint256> CommitmentOf(const CBlockIndex& index) const;
    /** The blocks whose validity rests on a commitment to `commitment`; by default the block of that hash. */
    virtual std::vector<uint256> BlocksCommittedBy(const uint256& commitment) const;

    //
    // Peg actions (sidechain/state.cpp)
    //

    /**
     * Whether `block` takes the peg actions, and where. False, with the reason, if it cannot be
     * known or the block is invalid. By default every block does, as committed: up to the mainchain
     * block before the one that committed to it, with its parent as the bundle nonce ("bmm-unknown"
     * if no mainchain block on record committed to it).
     */
    virtual bool GetPegStep(const CBlock& block, const Mainchain& mainchain, std::optional<PegStep>& step, std::string& reject_reason) const;
    /** Whether refund requests are valid only in blocks that take the peg actions. */
    virtual bool RefundsOnlyInPegSteps() const { return false; }
    /**
     * Rules of the sidechain's own for a transaction that is not a coinbase, after the template's
     * (State::ApplyTx): outputs of kinds of its own, say. `state` is the transaction's own overlay,
     * so a transaction that breaks a rule changes nothing; what it adds to `payouts` is paid as
     * refunds are. The mempool asks it too, on an overlay it drops, with the height of the next block
     * and `peg_step` false. By default nothing: every transaction keeps them.
     */
    virtual bool ApplyTx(State& state, const CTransaction& tx, int height, bool peg_step, std::vector<CTxOut>& payouts, std::string& reject_reason) const;
    /**
     * Whether ApplyTx may refuse `tx` (it has outputs of the sidechain's own kinds): the mempool checks
     * such transactions again after every change of its tip, and drops those the next block could no
     * longer take (the state moved under them). By default none.
     */
    virtual bool HasTxRules(const CTransaction& tx) const { return false; }
    /**
     * Peg actions of the sidechain's own, first in a block that takes them: what it adds to
     * `owed_tx` is paid by the coinbase as refunds are. Block assembly calls it too, with the block
     * as far as it is known then: its parent, version, time and sections (SidechainAssembly), and a
     * coinbase whose only output pays the coinbase script, with no value; no other transactions.
     */
    virtual bool BeforePegStep(State& state, const CBlock& block, int height, const PegStep& step, const Consensus::SidechainParams& params,
                               std::vector<CTxOut>& owed_tx, std::string& reject_reason) const;
    /**
     * Last in every block, after the payouts: what the sidechain's own rules note of the block.
     * `minted` is what the coinbase pays out (its outputs 1 to n: deposits, refunds and the like).
     * Not called by block assembly.
     */
    virtual bool AfterBlock(State& state, const CBlock& block, int height, const std::optional<PegStep>& step, CAmount minted,
                            const Consensus::SidechainParams& params, std::string& reject_reason) const;

    //
    // Block production and services
    //

    /**
     * Outputs of the sidechain's own that a block assembled with the template's peg actions (a merged
     * mined block) puts in its coinbase, after the payouts and the bundle commitment: proofs its rules
     * take, say. `view`: the state with the block as assembled so far; `prev` its parent and `time`
     * its time. The rules check them as any other output of the coinbase. By default none. Called
     * with cs_main held, by block assembly only.
     */
    virtual std::vector<CTxOut> MergeMinedOutputs(const StoreView& view, const CBlockIndex& prev, int64_t time) const;

    /**
     * Called by the Follower each round, once the record is up to date, in place of its own merged
     * mining: true if the sidechain's own producer did what it does, false to let the Follower mine.
     */
    virtual bool FollowerRound(Follower& follower) const;
    /** Start the services of the sidechain's own (a block producer, say), after the Follower started. */
    virtual void StartServices(node::NodeContext& node) const;
    /** Stop them, before the Follower stops. */
    virtual void StopServices(node::NodeContext& node) const;

    //
    // P2P
    //

    /**
     * A message of a type Bitcoin does not know, from `peer`: true if it was handled. Set
     * `misbehaving` to say why the peer should be punished.
     */
    virtual bool ProcessMessage(int64_t peer, const std::string& msg_type, DataStream& recv, std::string& misbehaving) const;
};

/** The hooks this chain runs with: the template's unless a sidechain installed its own. */
const ChainHooks& Hooks();
/**
 * Install hooks, or the template's again with nullptr. At startup, before any block is read and
 * before other threads run: it also turns extended blocks on or off (SetBlockExtensions).
 * @return the hooks that were installed before
 */
std::unique_ptr<ChainHooks> SetHooks(std::unique_ptr<ChainHooks> hooks);

/** Whether a header has the proof of work it needs: none for a block of a kind of the sidechain's own. */
bool CheckHeaderWork(const CBlockHeader& header, const Consensus::Params& params);
/** Whether an extended block's sections are those its header commits to (always so for others). */
bool CheckSections(const CBlock& block);
/**
 * Whether a header carries the extension of the block index of its hash (always so for a header that
 * is not extended, as is the index then). The block hash does not cover the extension's signature:
 * a block for a header the node has with another one is a mutated block (AcceptBlock).
 */
bool SameExtension(const CBlockHeader& header, const CBlockIndex& index);

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_HOOKS_H
