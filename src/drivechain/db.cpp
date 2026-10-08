// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <drivechain/db.h>

#include <serialize.h>
#include <sidechain/store.h>
#include <span.h>

#include <ios>
#include <memory>
#include <utility>

namespace drivechain {

namespace {

//! What a block needs to be taken back (BlockUndo), for the blocks a reorg can reach.
constexpr uint8_t DB_UNDO{'R'};
//! What a block did (BlockRecord), for every block.
constexpr uint8_t DB_EVENTS{'E'};
//! The sidechain database of a chainstate (StateRecord).
constexpr uint8_t DB_STATE{'s'};
//! An escrow change (Deposit), by DepositKey.
constexpr uint8_t DB_DEPOSIT{'D'};
//! Where an escrow change is (DepositKey), by sidechain and txid.
constexpr uint8_t DB_DEPOSIT_TXID{'T'};
constexpr uint8_t DB_FORMAT_VERSION{'v'};
//! The blocks that closed a bundle (std::vector<Closure>), by slot and bundle hash.
constexpr uint8_t DB_CLOSURE{'C'};
//! The sidechain database of a chainstate as an earlier version wrote it; erased with the state.
constexpr uint8_t DB_STATE_OLD{'S'};
//! The entries of the store of the state of this chain as a sidechain: 'K', the chainstate's name,
//! 0, then the key (sidechain/store.h).
constexpr uint8_t DB_SIDE_STORE{'K'};

//! Version of the snapshot of the sidechain database; a snapshot of another one is not read.
constexpr uint32_t STATE_VERSION{5};

//! Size of the batches Wipe erases with.
constexpr size_t WIPE_BATCH_BYTES{1 << 20};

/** Thrown on bytes left over after a record: it is of another format. */
template <typename Stream>
void ExpectEnd(Stream& s)
{
    if constexpr (requires { s.empty(); }) {
        if (!s.empty()) throw std::ios_base::failure("drivechain record of another format");
    }
}

/** The undo data of a block, read strictly: nothing may be left over. */
struct UndoRecord {
    BlockUndo& undo;

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        s >> undo;
        ExpectEnd(s);
    }
};

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

    friend bool operator==(const DepositKey&, const DepositKey&) = default;
};

/** Key of the position of an escrow change by its transaction. */
struct DepositTxidKey {
    SidechainId slot{0};
    uint256 txid;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_DEPOSIT_TXID);
        ser_writedata32be(s, slot);
        s << txid;
    }
};

/** Key of the closures of a bundle. */
struct ClosureKey {
    SidechainId slot{0};
    uint256 hash;

    template <typename Stream>
    void Serialize(Stream& s) const
    {
        ser_writedata8(s, DB_CLOSURE);
        ser_writedata32be(s, slot);
        s << hash;
    }
};

/** What is stored for every block. */
struct BlockRecord {
    BlockEvents events;
    //! The index entries the block added.
    std::vector<DepositKey> deposits;

    SERIALIZE_METHODS(BlockRecord, obj) { READWRITE(obj.events.closed, obj.events.proposed, obj.deposits, obj.events.pending); }
};

/** The snapshot of a sidechain database, with the version of its format and the parameters it was derived under. */
struct StateRecord {
    SidechainDB& scdb;
    const uint256& params_fingerprint;

    template <typename Stream>
    void Unserialize(Stream& s)
    {
        uint32_t version;
        s >> version;
        if (version != STATE_VERSION) throw std::ios_base::failure("unknown version of the sidechain database");
        uint256 fingerprint;
        s >> fingerprint;
        if (fingerprint != params_fingerprint) throw std::ios_base::failure("sidechain database derived under other drivechain parameters");
        s >> scdb;
        ExpectEnd(s);
    }
};

struct StateWriteRecord {
    const SidechainDB& scdb;
    const uint256& params_fingerprint;

    template <typename Stream>
    void Serialize(Stream& s) const { s << STATE_VERSION << params_fingerprint << scdb; }
};

/** What marks the format of the database: its version, and the parameters its data was derived under. */
struct FormatRecord {
    uint32_t version{0};
    uint256 params_fingerprint;

    SERIALIZE_METHODS(FormatRecord, obj) { READWRITE(obj.version, obj.params_fingerprint); }
};

/** A key as it is, whatever it holds: it reads all that is left. */
struct RawKey {
    std::vector<std::byte> bytes;

    template <typename Stream>
    void Serialize(Stream& s) const { s.write(std::span{bytes}); }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        s.read(std::span{bytes});
    }
};

} // namespace

Database::Database(const DBParams& params, const uint256& params_fingerprint) : m_db{params}, m_params_fingerprint{params_fingerprint}
{
    // A new database is in the current format (an older one, which has data, is not marked as such).
    if (m_db.IsEmpty()) WriteFormatVersion();
}

Database::Format Database::CheckFormat() const
{
    // The record of an older version (a version number alone) does not read as one.
    FormatRecord record;
    if (!m_db.Read(DB_FORMAT_VERSION, record) || record.version != FORMAT_VERSION) return Format::OTHER_VERSION;
    return record.params_fingerprint == m_params_fingerprint ? Format::CURRENT : Format::OTHER_PARAMS;
}

void Database::WriteFormatVersion()
{
    m_db.Write(DB_FORMAT_VERSION, FormatRecord{FORMAT_VERSION, m_params_fingerprint}, /*fSync=*/true);
}

void Database::Wipe()
{
    // One pass of one iterator, which reads the database as it was when it was made, erasing in
    // batches as it goes: a database that grew for long would not fit in memory as one batch, and
    // starting over from the first key after each batch would step over the tombstones of all the
    // keys erased before it, every time.
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    CDBBatch batch{m_db};
    for (it->SeekToFirst(); it->Valid(); it->Next()) {
        RawKey key;
        if (!it->GetKey(key)) break;
        batch.Erase(key);
        if (batch.ApproximateSize() >= WIPE_BATCH_BYTES) {
            m_db.WriteBatch(batch);
            batch.Clear();
        }
    }
    m_db.WriteBatch(batch, /*fSync=*/true);
}

bool Database::WriteBlock(const uint256& block_hash, int height, const BlockUndo& undo, const std::vector<Deposit>& deposits,
                          const SidechainDB& after, bool keep_undo)
{
    CDBBatch batch{m_db};
    BlockRecord record;
    record.events.closed = undo.closed;
    record.events.proposed = undo.proposed;
    for (const auto& [id, slot] : after.GetSlots()) {
        if (slot.bundles.empty()) continue;
        auto& bundles{record.events.pending.emplace_back(id, std::vector<std::pair<uint256, uint32_t>>{}).second};
        for (const Bundle& bundle : slot.bundles) bundles.emplace_back(bundle.hash, bundle.score);
    }
    for (const Deposit& deposit : deposits) {
        const DepositKey key{deposit.slot, static_cast<uint32_t>(height), deposit.tx_index};
        batch.Write(key, deposit);
        batch.Write(DepositTxidKey{deposit.slot, deposit.tx->GetHash().ToUint256()}, key);
        record.deposits.push_back(key);
    }
    batch.Write(std::make_pair(DB_EVENTS, block_hash), record);
    // A block closes a bundle once at most: its record replaces one left by the same block.
    for (const BlockUndo::Closed& closed : undo.closed) {
        const ClosureKey key{closed.id, closed.hash};
        std::vector<Closure> closures;
        m_db.Read(key, closures);
        std::erase_if(closures, [&](const Closure& c) { return c.block_hash == block_hash; });
        closures.push_back({block_hash, height, closed.paid});
        batch.Write(key, closures);
    }
    if (keep_undo) {
        batch.Write(std::make_pair(DB_UNDO, block_hash), undo);
    } else {
        batch.Erase(std::make_pair(DB_UNDO, block_hash));
    }
    m_db.WriteBatch(batch);
    return true;
}

bool Database::ReadBlockUndo(const uint256& block_hash, BlockUndo& undo) const
{
    BlockUndo read;
    UndoRecord record{read};
    if (!m_db.Read(std::make_pair(DB_UNDO, block_hash), record)) return false;
    undo = std::move(read);
    return true;
}

bool Database::HasBlockUndo(const uint256& block_hash) const
{
    return m_db.Exists(std::make_pair(DB_UNDO, block_hash));
}

void Database::EraseBlockUndo(const uint256& block_hash)
{
    m_db.Erase(std::make_pair(DB_UNDO, block_hash));
}

void Database::EraseBlockUndo(const std::vector<uint256>& block_hashes)
{
    CDBBatch batch{m_db};
    for (const uint256& block_hash : block_hashes) batch.Erase(std::make_pair(DB_UNDO, block_hash));
    m_db.WriteBatch(batch);
}

bool Database::ReadBlockEvents(const uint256& block_hash, BlockEvents& events) const
{
    BlockRecord record;
    if (!m_db.Read(std::make_pair(DB_EVENTS, block_hash), record)) return false;
    events = std::move(record.events);
    return true;
}

bool Database::EraseBlockDeposits(const uint256& block_hash)
{
    BlockRecord record;
    if (!m_db.Read(std::make_pair(DB_EVENTS, block_hash), record)) return false;
    if (record.deposits.empty()) return true;
    CDBBatch batch{m_db};
    for (const DepositKey& key : record.deposits) {
        Deposit deposit;
        if (m_db.Read(key, deposit)) {
            // The txid entry goes with it, unless it is the entry of the transaction in another block.
            const DepositTxidKey txid_key{deposit.slot, deposit.tx->GetHash().ToUint256()};
            DepositKey indexed;
            if (m_db.Read(txid_key, indexed) && indexed == key) batch.Erase(txid_key);
        }
        batch.Erase(key);
    }
    m_db.WriteBatch(batch);
    return true;
}

std::optional<Closure> Database::FindClosure(SidechainId slot, const uint256& bundle_hash, const std::function<bool(const uint256&)>& in_active_chain) const
{
    std::vector<Closure> closures;
    if (!m_db.Read(ClosureKey{slot, bundle_hash}, closures)) return std::nullopt;
    std::optional<Closure> last;
    for (const Closure& closure : closures) {
        if ((!last || closure.height > last->height) && in_active_chain(closure.block_hash)) last = closure;
    }
    return last;
}

bool Database::WriteState(const std::string& chainstate, const SidechainDB& scdb, const sidechain::DbStore* side_db,
                          const std::map<sidechain::StoreBytes, std::optional<sidechain::StoreBytes>>* side_changes)
{
    CDBBatch batch{m_db};
    batch.Write(std::make_pair(DB_STATE, chainstate), StateWriteRecord{scdb, m_params_fingerprint});
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
    SidechainDB read;
    StateRecord record{read, m_params_fingerprint};
    if (!m_db.Read(std::make_pair(DB_STATE, chainstate), record)) {
        // Whatever was read before the failure is not left behind.
        scdb = SidechainDB{};
        return false;
    }
    scdb = std::move(read);
    return true;
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

std::optional<std::vector<Deposit>> Database::ListDeposits(SidechainId slot, const std::optional<uint256>& after, size_t count,
                                                           const std::function<bool(const uint256&)>& in_active_chain) const
{
    std::vector<Deposit> deposits;
    DepositKey start{slot, 0, 0};
    if (after) {
        // Straight to the change the caller knows, by its txid: not through all those before it.
        Deposit known;
        const bool indexed{m_db.Read(DepositTxidKey{slot, *after}, start) && m_db.Read(start, known) &&
                           known.slot == slot && known.tx->GetHash().ToUint256() == *after};
        if (!indexed || !in_active_chain(known.block_hash)) {
            // The txid entry names the record of a block that left the chain, or the entry went with
            // such a record (a crash after a reorg can leave either). Going on from there would skip,
            // or repeat, changes: the record of the transaction in the active chain is looked for
            // instead, through the sidechain's records (an `after` that is no change at all costs that).
            std::optional<DepositKey> found;
            const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
            for (it->Seek(DepositKey{slot, 0, 0}); it->Valid(); it->Next()) {
                DepositKey key;
                if (!it->GetKey(key) || key.slot != slot) break;
                Deposit deposit;
                if (!it->GetValue(deposit)) break;
                if (deposit.tx->GetHash().ToUint256() == *after && in_active_chain(deposit.block_hash)) found = key;
            }
            if (!found) return std::nullopt;
            start = *found;
        }
    }
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    it->Seek(start);
    if (after && it->Valid()) it->Next();
    for (; it->Valid(); it->Next()) {
        DepositKey key;
        if (!it->GetKey(key) || key.slot != slot) break;
        Deposit deposit;
        if (!it->GetValue(deposit)) break;
        if (!in_active_chain(deposit.block_hash)) continue;
        deposits.push_back(std::move(deposit));
        if (count != 0 && deposits.size() >= count) break;
    }
    return deposits;
}

} // namespace drivechain
