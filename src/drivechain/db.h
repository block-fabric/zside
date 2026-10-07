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
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace drivechain {

/**
 * Storage for drivechain data that is not kept in memory:
 *
 *  - per block, what is needed to revert its changes to the sidechain database,
 *  - a snapshot of the sidechain database of each chainstate, taken whenever
 *    the chainstate is flushed,
 *  - an index of the escrow changes of each sidechain, which sidechain
 *    software reads to credit deposits.
 */
class Database
{
public:
    explicit Database(const DBParams& params);

    /** Store the undo data of a block that changed the escrow of the sidechains in `deposits` at `height`. */
    bool WriteBlock(const uint256& block_hash, int height, const BlockUndo& undo, const std::vector<Deposit>& deposits);
    bool ReadBlockUndo(const uint256& block_hash, BlockUndo& undo) const;
    /** Remove the escrow changes of a block that is no longer in the active chain from the index. */
    bool EraseBlockDeposits(const uint256& block_hash);

    /**
     * Store the sidechain database of the chainstate named `chainstate`, and with it, in the same
     * batch, the changes to the state of this chain as a sidechain (`side`, if given).
     */
    bool WriteState(const std::string& chainstate, const SidechainDB& scdb, const sidechain::DbStore* side_db = nullptr,
                    const std::map<sidechain::StoreBytes, std::optional<sidechain::StoreBytes>>* side_changes = nullptr);
    bool ReadState(const std::string& chainstate, SidechainDB& scdb) const;
    /** Erase the stored sidechain state, and any state of the format before it (to rebuild from blocks). */
    void WipeState(const std::string& chainstate, const sidechain::DbStore& side_db);
    /** Where the state of this chain as a sidechain, of the chainstate named `chainstate`, is stored. */
    std::unique_ptr<sidechain::DbStore> SideStore(const std::string& chainstate);

    /**
     * Escrow changes of a sidechain in chain order.
     *
     * @param[in] after  if set, the txid of the last change the caller knows about; only later ones are returned
     * @param[in] count  maximum number of changes to return, zero for no limit
     * @return nullopt if `after` is not a known escrow change of the sidechain
     */
    std::optional<std::vector<Deposit>> ListDeposits(SidechainId slot, const std::optional<uint256>& after, size_t count) const;
    /** Escrow changes of a sidechain made by the block of the active chain at `height`, in block order. */
    std::vector<Deposit> ListBlockDeposits(SidechainId slot, int height) const;

private:
    mutable CDBWrapper m_db;
};

} // namespace drivechain

#endif // BITCOIN_DRIVECHAIN_DB_H
