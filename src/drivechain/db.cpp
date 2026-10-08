// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/db.h>

#include <serialize.h>
#include <sidechain/state.h>

#include <memory>
#include <utility>

namespace drivechain {

namespace {

constexpr uint8_t DB_BLOCK{'U'};
//! The sidechain database, without the sidechain state (now in its store): a node with the
//! earlier format, under 'S', rebuilds the state from its blocks.
constexpr uint8_t DB_STATE_OLD{'S'};
constexpr uint8_t DB_STATE{'s'};
//! The entries of the store of the sidechain state: 'T', the chainstate's name, 0, then the key.
constexpr uint8_t DB_SIDE_STORE{'T'};
constexpr uint8_t DB_DEPOSIT{'D'};

/** Key of an escrow change; big endian so that the database orders the changes of a sidechain by position in the chain. */
struct DepositKey {
    SidechainId slot{0};
    uint32_t height{0};
    uint32_t tx_index{0};

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_DEPOSIT);
        ser_writedata32be(s, slot);
        ser_writedata32be(s, height);
        ser_writedata32be(s, tx_index);
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        if (ser_readdata8(s) != DB_DEPOSIT) throw std::ios_base::failure("not a deposit key");
        slot = ser_readdata32be(s);
        height = ser_readdata32be(s);
        tx_index = ser_readdata32be(s);
    }
};

/** Thrown on bytes left over after a record: it is of another format. */
template <typename Stream>
void ExpectEnd(Stream& s)
{
    if constexpr (requires { s.empty(); }) {
        if (!s.empty()) throw std::ios_base::failure("drivechain record of another format");
    }
}

/** What is stored for a block. */
struct BlockRecord {
    BlockUndo undo;
    //! The index entries the block added.
    std::vector<DepositKey> deposits;

    template <typename Stream>
    void Serialize(Stream& s) const { s << undo << deposits; }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> undo >> deposits;
        ExpectEnd(s);
    }
};

/**
 * A record written before the sidechain state had a store (sidechain/store.h): the change of a
 * block to the state was a structure of its own, which BlockUndo::side cannot be read from. Read
 * only for what comes before it, when it is the empty structure that a chain which is no
 * sidechain wrote (`side_empty`).
 */
struct LegacyBlockRecord {
    BlockUndo undo;
    std::vector<DepositKey> deposits;
    bool side_empty{false};

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> undo.prev_block_hash >> undo.slots >> undo.proposals_added >> undo.acked >> undo.removed >> undo.closed >> undo.last_votes;
        int32_t main_height, last_failure_height;
        bool has_bundle;
        s >> main_height >> has_bundle;
        if (has_bundle) {
            sidechain::PendingBundle bundle;
            s >> bundle;
        }
        std::vector<COutPoint> added;
        std::vector<sidechain::Withdrawal> removed;
        std::vector<CTxOut> paid, paid_tx;
        uint64_t queued, queued_tx;
        s >> last_failure_height >> added >> removed >> paid >> queued >> paid_tx >> queued_tx;
        s >> deposits;
        ExpectEnd(s);
        side_empty = main_height == -1 && !has_bundle && last_failure_height == -1 && added.empty() && removed.empty() &&
                     paid.empty() && queued == 0 && paid_tx.empty() && queued_tx == 0;
    }
};

} // namespace

Database::Database(const DBParams& params) : m_db{params} {}

bool Database::WriteBlock(const uint256& block_hash, int height, const BlockUndo& undo, const std::vector<Deposit>& deposits)
{
    CDBBatch batch{m_db};
    BlockRecord record;
    record.undo = undo;
    for (const Deposit& deposit : deposits) {
        const DepositKey key{deposit.slot, static_cast<uint32_t>(height), deposit.tx_index};
        batch.Write(key, deposit);
        record.deposits.push_back(key);
    }
    batch.Write(std::make_pair(DB_BLOCK, block_hash), record);
    m_db.WriteBatch(batch);
    return true;
}

bool Database::ReadBlockUndo(const uint256& block_hash, BlockUndo& undo) const
{
    BlockRecord record;
    if (m_db.Read(std::make_pair(DB_BLOCK, block_hash), record)) {
        undo = std::move(record.undo);
        return true;
    }
    // Of the format before: usable only if it held no change to a sidechain state, which could not be taken back from it.
    LegacyBlockRecord legacy;
    if (!m_db.Read(std::make_pair(DB_BLOCK, block_hash), legacy) || !legacy.side_empty) return false;
    undo = std::move(legacy.undo);
    return true;
}

bool Database::EraseBlockDeposits(const uint256& block_hash)
{
    BlockRecord record;
    if (!m_db.Read(std::make_pair(DB_BLOCK, block_hash), record)) {
        LegacyBlockRecord legacy;
        if (!m_db.Read(std::make_pair(DB_BLOCK, block_hash), legacy)) return false;
        record.deposits = std::move(legacy.deposits);
    }
    if (record.deposits.empty()) return true;
    CDBBatch batch{m_db};
    for (const DepositKey& key : record.deposits) batch.Erase(key);
    m_db.WriteBatch(batch);
    return true;
}

bool Database::WriteState(const std::string& chainstate, const SidechainDB& scdb, const sidechain::DbStore* side_db,
                          const std::map<sidechain::StoreBytes, std::optional<sidechain::StoreBytes>>* side_changes)
{
    CDBBatch batch{m_db};
    batch.Write(std::make_pair(DB_STATE, chainstate), scdb);
    if (side_db && side_changes) side_db->Write(batch, *side_changes);
    m_db.WriteBatch(batch, /*fSync=*/true);
    if (side_db) side_db->Reset();
    return true;
}

void Database::WipeState(const std::string& chainstate, const sidechain::DbStore& side_db)
{
    // The database of the chainstate goes first: without it, a node that stops before the store is
    // empty finds no state at its next start, and wipes the store again before deriving the state.
    CDBBatch batch{m_db};
    batch.Erase(std::make_pair(DB_STATE, chainstate));
    batch.Erase(std::make_pair(DB_STATE_OLD, chainstate));
    m_db.WriteBatch(batch, /*fSync=*/true);
    side_db.Wipe();
}

std::unique_ptr<sidechain::DbStore> Database::SideStore(const std::string& chainstate)
{
    sidechain::StoreBytes prefix{DB_SIDE_STORE};
    prefix.insert(prefix.end(), chainstate.begin(), chainstate.end());
    prefix.push_back(0);
    return std::make_unique<sidechain::DbStore>(m_db, std::move(prefix));
}

bool Database::ReadState(const std::string& chainstate, SidechainDB& scdb) const
{
    return m_db.Read(std::make_pair(DB_STATE, chainstate), scdb);
}

std::vector<Deposit> Database::ListBlockDeposits(SidechainId slot, int height) const
{
    std::vector<Deposit> deposits;
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(DepositKey{slot, static_cast<uint32_t>(height), 0}); it->Valid(); it->Next()) {
        DepositKey key;
        if (!it->GetKey(key) || key.slot != slot || key.height != static_cast<uint32_t>(height)) break;
        Deposit deposit;
        if (!it->GetValue(deposit)) break;
        deposits.push_back(std::move(deposit));
    }
    return deposits;
}

std::optional<std::vector<Deposit>> Database::ListDeposits(SidechainId slot, const std::optional<uint256>& after, size_t count) const
{
    std::vector<Deposit> deposits;
    bool found{!after};
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    for (it->Seek(DepositKey{slot, 0, 0}); it->Valid(); it->Next()) {
        DepositKey key;
        if (!it->GetKey(key) || key.slot != slot) break;
        Deposit deposit;
        if (!it->GetValue(deposit)) break;
        if (!found) {
            found = deposit.tx->GetHash().ToUint256() == *after;
            continue;
        }
        deposits.push_back(std::move(deposit));
        if (count != 0 && deposits.size() >= count) break;
    }
    if (!found) return std::nullopt;
    return deposits;
}

} // namespace drivechain
