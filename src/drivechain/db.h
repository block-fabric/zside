// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_DRIVECHAIN_DB_H
#define BITCOIN_DRIVECHAIN_DB_H

#include <dbwrapper.h>
#include <drivechain/scdb.h>
#include <drivechain/sidechain.h>
#include <sidechain/store.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace drivechain {

/** What a block did that sidechain software follows: kept for every block of the active chain. */
struct BlockEvents {
    //! Bundles the block closed: paid out, or failed.
    std::vector<BlockUndo::Closed> closed;
    //! Bundles the block proposed (M3) that became pending.
    std::vector<std::pair<SidechainId, uint256>> proposed;
    //! The bundles pending after the block, with their score after it, in vote order, per sidechain that has any.
    std::vector<std::pair<SidechainId, std::vector<std::pair<uint256, uint32_t>>>> pending;
};

/** A block that closed a bundle. */
struct Closure {
    uint256 block_hash;
    int32_t height{0};
    bool paid{false};

    SERIALIZE_METHODS(Closure, obj) { READWRITE(obj.block_hash, obj.height, obj.paid); }
};

/**
 * Storage for drivechain data that is not kept in memory:
 *
 *  - per block, what is needed to revert its changes to the sidechain database, for the
 *    blocks a reorg can take back (see DRIVECHAIN_UNDO_DEPTH): older ones are erased,
 *  - per block, what it did that sidechain software follows (BlockEvents), for good,
 *  - a snapshot of the sidechain database of each chainstate, taken whenever
 *    the chainstate is flushed,
 *  - an index of the escrow changes of each sidechain, which sidechain
 *    software reads to credit deposits,
 *  - an index of the blocks that closed each bundle, for good: a bundle the sidechain database
 *    forgot (see SidechainDB::ForgetFailedBundles) is still known to have failed,
 *  - on a chain that is itself a sidechain, the store of its state (SideStore), one entry per key.
 */
class Database
{
public:
    /**
     * @param[in] params_fingerprint  ParamsFingerprint() of the drivechain parameters of the chain: all
     *                                of the data is derived under them
     */
    Database(const DBParams& params, const uint256& params_fingerprint);

    /** Version of the way the data is laid out; a database of another version is wiped and built anew from the blocks. */
    static constexpr uint32_t FORMAT_VERSION{8};
    enum class Format {
        CURRENT,
        //! Laid out another way (by an older version of this software), or not marked at all.
        OTHER_VERSION,
        //! Derived under other drivechain parameters (another activation height, say).
        OTHER_PARAMS,
    };
    Format CheckFormat() const;
    bool IsCurrentFormat() const { return CheckFormat() == Format::CURRENT; }
    /** Mark the database as being of the current format, derived under the current parameters. */
    void WriteFormatVersion();
    /** Erase everything (the stores of the sidechain state too), a batch at a time. */
    void Wipe();

    /**
     * Store what a block at `height` did: its undo data, unless `keep_undo` is false, its events,
     * and the escrow changes it made (`deposits`). `after` is the sidechain database the block left,
     * whose pending bundles go with the events.
     */
    bool WriteBlock(const uint256& block_hash, int height, const BlockUndo& undo, const std::vector<Deposit>& deposits,
                    const SidechainDB& after, bool keep_undo = true);
    bool ReadBlockUndo(const uint256& block_hash, BlockUndo& undo) const;
    bool HasBlockUndo(const uint256& block_hash) const;
    /** Erase the undo data of a block (not its events), once no reorg can take it back. */
    void EraseBlockUndo(const uint256& block_hash);
    /** The same for several blocks, in one batch. */
    void EraseBlockUndo(const std::vector<uint256>& block_hashes);
    /** The blocks that have undo data. */
    std::vector<uint256> ListUndoBlocks() const;
    bool ReadBlockEvents(const uint256& block_hash, BlockEvents& events) const;
    /**
     * Remove the escrow changes of a block from the index. Not needed for correctness (readers skip
     * the records of blocks out of the active chain): it only frees the records of stale blocks.
     */
    bool EraseBlockDeposits(const uint256& block_hash);

    /**
     * The last closure of a bundle by a block that `in_active_chain` accepts (the blocks of the active
     * chain); nullopt if there is none. Records of blocks that left the chain are kept, and skipped here.
     */
    std::optional<Closure> FindClosure(SidechainId slot, const uint256& bundle_hash, const std::function<bool(const uint256&)>& in_active_chain) const;

    /**
     * Store the sidechain database of the chainstate named `chainstate`, and with it, in the same
     * batch, the changes to the state of this chain as a sidechain (`side`, if given).
     */
    bool WriteState(const std::string& chainstate, const SidechainDB& scdb, const sidechain::DbStore* side_db = nullptr,
                    const std::map<sidechain::StoreBytes, std::optional<sidechain::StoreBytes>>* side_changes = nullptr);
    /**
     * Read it; false, with `scdb` empty, if there is none or it cannot be read (in an unknown format, say),
     * or if it was derived under other drivechain parameters.
     */
    bool ReadState(const std::string& chainstate, SidechainDB& scdb) const;
    /** Erase the stored sidechain state, and any state of the format before it (to rebuild from blocks). */
    void WipeState(const std::string& chainstate, const sidechain::DbStore& side_db);
    /** Where the state of this chain as a sidechain, of the chainstate named `chainstate`, is stored. */
    std::unique_ptr<sidechain::DbStore> SideStore(const std::string& chainstate);

    /**
     * Escrow changes of a sidechain in chain order, of the blocks `in_active_chain` accepts. Records
     * of other blocks (blocks that left the chain keep theirs) are skipped, and do not count towards `count`.
     *
     * @param[in] after  if set, the txid of the last change the caller knows about; only later ones are returned
     * @param[in] count  maximum number of changes to return, zero for no limit
     * @return nullopt if `after` is not an escrow change of the sidechain in an accepted block
     */
    std::optional<std::vector<Deposit>> ListDeposits(SidechainId slot, const std::optional<uint256>& after, size_t count,
                                                     const std::function<bool(const uint256&)>& in_active_chain) const;
    /** Escrow changes of a sidechain made by the block `block_hash` at `height`, in block order. */
    std::vector<Deposit> ListBlockDeposits(SidechainId slot, int height, const uint256& block_hash) const;

private:
    mutable CDBWrapper m_db;
    const uint256 m_params_fingerprint;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DB_H
