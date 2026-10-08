// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The store of the sidechain state in a database: apart from the rest of the store, which programs
// without a database (the transaction tool) use too.

#include <sidechain/store.h>

#include <dbwrapper.h>

#include <algorithm>
#include <memory>

namespace sidechain {

namespace {
bool StartsWith(std::span<const unsigned char> key, std::span<const unsigned char> prefix)
{
    return key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin());
}

/** The key of a database entry, read raw. */
struct RawKey {
    StoreBytes bytes;
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        s.read(MakeWritableByteSpan(bytes));
    }
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(MakeByteSpan(bytes));
    }
};

/** A value written to the database as it is, without a length in front. */
struct RawValue {
    StoreBytes bytes;
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        s.write(MakeByteSpan(bytes));
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        bytes.resize(s.size());
        s.read(MakeWritableByteSpan(bytes));
    }
};
} // namespace

//
// DbStore
//

/** The iterator of the last read in order, and what it found. */
struct DbStore::Cursor {
    std::unique_ptr<CDBIterator> it;
    //! The database has no entry from `from` (a full key) up to the iterator's: the read that put it there sought `from`.
    StoreBytes from;
    bool valid{false};
    //! The full key the iterator is at, if valid.
    StoreBytes key;

    void Load()
    {
        valid = it->Valid();
        RawKey raw;
        if (valid && it->GetKey(raw)) {
            key = std::move(raw.bytes);
        } else {
            valid = false;
        }
    }
};

DbStore::DbStore(CDBWrapper& db, StoreBytes prefix) : m_db{db}, m_prefix{std::move(prefix)} {}
DbStore::~DbStore() = default;

StoreBytes DbStore::Full(std::span<const unsigned char> key) const
{
    StoreBytes full{m_prefix};
    full.insert(full.end(), key.begin(), key.end());
    return full;
}

std::optional<StoreBytes> DbStore::Get(std::span<const unsigned char> key) const
{
    RawValue value;
    if (!m_db.Read(RawKey{Full(key)}, value)) return std::nullopt;
    return std::move(value.bytes);
}

std::optional<std::pair<StoreBytes, StoreBytes>> DbStore::Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const
{
    const StoreBytes start{Full(from)};
    const StoreBytes full_prefix{Full(prefix)};
    std::lock_guard lock{m_cursor_mutex};
    bool found{false};
    if (m_cursor && m_cursor->from <= start) {
        Cursor& cursor{*m_cursor};
        if (!cursor.valid || start <= cursor.key) {
            // Nothing between where it sought and where it is: the same answer.
            found = true;
        } else {
            StoreBytes after{cursor.key};
            after.push_back(0);
            if (start == after) {
                // The read in order: the entry after the last one.
                cursor.it->Next();
                cursor.from = start;
                cursor.Load();
                found = true;
            }
        }
    }
    if (!found) {
        if (!m_cursor) {
            m_cursor = std::make_unique<Cursor>();
            m_cursor->it.reset(m_db.NewIterator());
        }
        m_cursor->it->Seek(RawKey{start});
        m_cursor->from = start;
        m_cursor->Load();
    }
    const Cursor& cursor{*m_cursor};
    if (!cursor.valid || !StartsWith(cursor.key, full_prefix)) return std::nullopt;
    RawValue value;
    if (!cursor.it->GetValue(value)) return std::nullopt;
    return std::make_pair(StoreBytes(cursor.key.begin() + m_prefix.size(), cursor.key.end()), std::move(value.bytes));
}

void DbStore::Reset() const
{
    std::lock_guard lock{m_cursor_mutex};
    m_cursor.reset();
}

void DbStore::Write(CDBBatch& batch, const std::map<StoreBytes, std::optional<StoreBytes>>& changes) const
{
    for (const auto& [key, value] : changes) {
        if (value) {
            batch.Write(RawKey{Full(key)}, RawValue{*value});
        } else {
            batch.Erase(RawKey{Full(key)});
        }
    }
    // The batch is written next; the iterator would not see it.
    Reset();
}

void DbStore::Wipe(size_t batch_bytes) const
{
    Reset();
    // One batch per so many entries: a state of any size, never held whole in memory. An iterator sees
    // the database as it was when made, so erasing under it is no matter.
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    CDBBatch batch{m_db};
    for (it->Seek(RawKey{m_prefix}); it->Valid(); it->Next()) {
        RawKey key;
        if (!it->GetKey(key) || !StartsWith(key.bytes, m_prefix)) break;
        batch.Erase(key);
        if (batch.ApproximateSize() >= batch_bytes) {
            m_db.WriteBatch(batch);
            batch.Clear();
        }
    }
    m_db.WriteBatch(batch, /*fSync=*/true);
    Reset();
}

} // namespace sidechain
