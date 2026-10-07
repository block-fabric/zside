// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/db.h>

#include <serialize.h>

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

/** What is stored for a block. */
struct BlockRecord {
    BlockUndo undo;
    //! The index entries the block added.
    std::vector<DepositKey> deposits;

    SERIALIZE_METHODS(BlockRecord, obj) { READWRITE(obj.undo, obj.deposits); }
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
    if (!m_db.Read(std::make_pair(DB_BLOCK, block_hash), record)) return false;
    undo = std::move(record.undo);
    return true;
}

bool Database::EraseBlockDeposits(const uint256& block_hash)
{
    BlockRecord record;
    if (!m_db.Read(std::make_pair(DB_BLOCK, block_hash), record)) return false;
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
    return true;
}

void Database::WipeState(const std::string& chainstate, const sidechain::DbStore& side_db)
{
    CDBBatch batch{m_db};
    batch.Erase(std::make_pair(DB_STATE, chainstate));
    batch.Erase(std::make_pair(DB_STATE_OLD, chainstate));
    side_db.Wipe(batch);
    m_db.WriteBatch(batch, /*fSync=*/true);
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
