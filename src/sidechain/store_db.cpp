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
    const std::unique_ptr<CDBIterator> it{m_db.NewIterator()};
    const StoreBytes start{Full(from)};
    const StoreBytes full_prefix{Full(prefix)};
    it->Seek(RawKey{start});
    if (!it->Valid()) return std::nullopt;
    RawKey key;
    if (!it->GetKey(key) || !StartsWith(key.bytes, full_prefix)) return std::nullopt;
    RawValue value;
    if (!it->GetValue(value)) return std::nullopt;
    return std::make_pair(StoreBytes(key.bytes.begin() + m_prefix.size(), key.bytes.end()), std::move(value.bytes));
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
}

void DbStore::Wipe(CDBBatch& batch) const
{
    ForEach({}, [&](const StoreBytes& key, const StoreBytes&) {
        batch.Erase(RawKey{Full(key)});
        return true;
    });
}

} // namespace sidechain
