// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_ZSIDE_SHIELDED_H
#define BITCOIN_ZSIDE_SHIELDED_H

#include <consensus/amount.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <serialize.h>
#include <sidechain/store.h>
#include <uint256.h>
#include <zside/marker.h>

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace Consensus {
struct Params;
struct SidechainParams;
} // namespace Consensus
struct ZoBundle;

/**
 * Private payments (zSide): the Orchard shielded pool of Zcash, on a sidechain.
 *
 *  - A transaction carries an Orchard **bundle** — encoded as in a Zcash v5 transaction —
 *    in a **marker** output: OP_RETURN, then a push of SHIELDED_TAG, a version byte and the
 *    bundle. A bundle spends notes of the pool (revealing their nullifiers, proving in zero
 *    knowledge that they are in the note commitment tree at the root it names, its anchor)
 *    and creates notes (their commitments join the tree).
 *  - Its **value balance** is what it takes out of the pool. Taken out, it counts as an input
 *    of the transaction: transparent outputs and the fee can come from the pool, and a
 *    transaction may have no transparent input at all. Put in (negative), the marker output
 *    carries it: the transaction pays it like any output, and it leaves circulation.
 *  - The anchor is the root of the tree at the end of an earlier block. A nullifier is
 *    revealed once: a note is spent once. The pool never holds less than nothing.
 *  - The bundle's signatures and proof are made over the **sighash** of the transaction:
 *    everything in it except their own bytes and the transparent signatures (the marker is
 *    taken with the commitment to the bundle's effects, ZIP 244, in place of the bundle).
 *    The transparent inputs sign the whole transaction, the bundle included.
 *    The sighash also commits to the chain: its genesis block, its slot and a branch id, so
 *    that a bundle signed for one chain (a test network, another sidechain of the same code)
 *    is invalid on any other.
 */
namespace zside {

/** The marker: OP_RETURN, then a push of SHIELDED_TAG (zside/marker.h), a version, and the bundle. */
inline constexpr uint8_t SHIELDED_VERSION{0};
/** Most actions one bundle may have (beyond that, policy and the block size limit before it). */
inline constexpr size_t MAX_ACTIONS{256};

/** The script of the marker of a bundle. */
CScript ShieldedMarkerScript(std::span<const uint8_t> bundle);

/** A bundle read from a transaction. */
class Bundle
{
public:
    /** The bundle of a transaction: none if it has no marker; `error` set if it has a bad one. */
    static std::optional<Bundle> FromTx(const CTransaction& tx, std::string& error);
    static std::optional<Bundle> FromBytes(std::span<const uint8_t> bytes);

    const ZoBundle* get() const { return m_bundle.get(); }
    size_t NumActions() const;
    /** What the bundle takes out of the pool (negative: what it puts in). */
    CAmount ValueBalance() const;
    uint256 Anchor() const;
    std::vector<uint256> Nullifiers() const;
    std::vector<uint256> Commitments() const;
    /** The commitment to its effects (ZIP 244), which the sighash takes in its place. */
    uint256 Commitment() const;
    /** The index of the marker output in its transaction. */
    uint32_t marker_index{0};

private:
    struct Free {
        void operator()(ZoBundle* bundle) const;
    };
    std::shared_ptr<ZoBundle> m_bundle;
};

/**
 * What a transaction takes out of the pool, read from its marker without checking the bundle
 * (the shielded rules do that): the value it adds to the inputs of the transaction. Zero if
 * it has no marker, or puts value in.
 */
CAmount ShieldedValueIn(const CTransaction& tx);

/** The chain the signatures of a bundle are bound to. */
struct SighashDomain {
    uint256 genesis;
    uint32_t slot{0};
};
/** Changed with anything the sighash commits to. */
inline constexpr uint32_t SIGHASH_BRANCH_ID{0x7a530001};
/** The domain of the sighash of the bundles of a chain. */
SighashDomain SighashDomainOf(const Consensus::SidechainParams& params, const uint256& genesis);
SighashDomain SighashDomainOf(const Consensus::Params& params);

/** The hash the bundle of a transaction signs, in `domain`. */
uint256 ShieldedSighash(const CTransaction& tx, uint32_t marker_index, const uint256& bundle_commitment, const SighashDomain& domain);

/**
 * Check what depends on the transaction alone: one well-formed marker at most, not in a
 * coinbase, the value put in carried by the marker, the proof and signatures valid (in the
 * given sighash domain).
 */
[[nodiscard]] bool CheckShieldedTx(const CTransaction& tx, std::string& reject_reason, const SighashDomain& domain);

/**
 * Whether `reject_reason` is one of CheckShieldedTx's, which looks at the transaction alone (its
 * marker, its bundle, the proof in the sighash domain of the chain): a block that fails so
 * fails whatever the record of the mainchain says (sidechain::RecordIndependent).
 */
bool TxOnlyReason(const std::string& reject_reason);

/**
 * Verify the proofs and signatures of the bundles of several transactions (a block's) in one batch,
 * and remember them valid: CheckShieldedTx then finds them so. False if one is invalid; nothing is
 * remembered then, and CheckShieldedTx finds which, one by one.
 */
bool VerifyBundles(std::span<const CTransactionRef> txs, const SighashDomain& domain);

/** The right edge of the note commitment tree: enough to append to it and compute its root. */
class Frontier
{
public:
    uint64_t Size() const { return m_size; }
    void Append(const uint256& cmx);
    uint256 Root() const;

    SERIALIZE_METHODS(Frontier, obj) { READWRITE(obj.m_size, obj.m_ommers); }
    friend bool operator==(const Frontier&, const Frontier&) = default;

private:
    uint64_t m_size{0};
    //! For each level, the left node waiting for its right sibling, when the size has that bit set.
    std::vector<uint256> m_ommers;
};

/** The root of an empty subtree of the given height (0: an empty leaf). */
uint256 EmptyRoot(unsigned level);
/** The parent of two nodes at `level`. */
uint256 Combine(unsigned level, const uint256& left, const uint256& right);

/**
 * The shielded pool, as the chain leaves it, on the store of the sidechain state (sidechain/store.h):
 * the nullifiers of spent notes and the anchors one entry each, so that neither is ever read, copied
 * or written whole. Tables: 0x10 nullifiers, 0x11 anchors (roots the tree had at the end of a
 * block); single values: 0x12 the tree's frontier, 0x13 what the pool holds, 0x14 and 0x15 how many
 * nullifiers and anchors there are. The empty tree's root is an anchor from the start without an entry.
 */
class ShieldedState
{
public:
    /** Read only. */
    explicit ShieldedState(const sidechain::StoreView& view) : m_view{&view}, m_overlay{nullptr} {}
    /** Read and write. */
    explicit ShieldedState(sidechain::StoreOverlay& overlay) : m_view{&overlay}, m_overlay{&overlay} {}

    /** Check a transaction (CheckShieldedTx, in `domain`) and against the pool, and apply it. A transaction that fails changes nothing. */
    [[nodiscard]] bool ApplyTx(const CTransaction& tx, std::string& reject_reason, const SighashDomain& domain);
    /**
     * Whether the bundle of a transaction fits the pool now (its anchor, its nullifiers, the pool's
     * balance), its value balance in range: the cheap part of ApplyTx, without the proof
     * (CheckShieldedTx), which the mempool verifies last.
     */
    [[nodiscard]] bool CheckState(const CTransaction& tx, std::string& reject_reason) const;
    /** End a block: its final root becomes an anchor later bundles may use. */
    void EndBlock();

    Frontier Tree() const;
    bool IsSpent(const uint256& nullifier) const;
    bool IsAnchor(const uint256& root) const;
    /** How many roots are anchors, the empty tree's included. */
    uint64_t AnchorCount() const;
    /** What the pool holds. */
    CAmount Pool() const;
    uint64_t NullifierCount() const;

private:
    [[nodiscard]] bool Check(const CTransaction& tx, const Bundle& bundle, std::string& reject_reason) const;

    const sidechain::StoreView* m_view;
    sidechain::StoreOverlay* m_overlay;
};

} // namespace zside

#endif // BITCOIN_ZSIDE_SHIELDED_H
