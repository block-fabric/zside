// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/store.h>

#include <dbwrapper.h>
#include <hash.h>

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

void StoreView::ForEach(std::span<const unsigned char> prefix, const std::function<bool(const StoreBytes&, const StoreBytes&)>& fn) const
{
    StoreBytes at(prefix.begin(), prefix.end());
    while (const auto entry{Next(at, prefix)}) {
        if (!fn(entry->first, entry->second)) return;
        at = entry->first;
        at.push_back(0);
    }
}

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

//
// StoreOverlay
//

std::optional<StoreBytes> StoreOverlay::Get(std::span<const unsigned char> key) const
{
    if (const auto it{m_changes.find(StoreBytes(key.begin(), key.end()))}; it != m_changes.end()) return it->second;
    return m_base->Get(key);
}

std::optional<std::pair<StoreBytes, StoreBytes>> StoreOverlay::Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const
{
    StoreBytes at(from.begin(), from.end());
    while (true) {
        // The first change at or after `at`, and the first entry of the base: the smaller key wins,
        // and a change hides the base's entry of the same key.
        auto change{m_changes.lower_bound(at)};
        if (change != m_changes.end() && !StartsWith(change->first, prefix)) change = m_changes.end();
        auto base{m_base->Next(at, prefix)};
        if (change == m_changes.end()) {
            return base;
        }
        if (base && base->first < change->first) return base;
        if (change->second) return std::make_pair(change->first, *change->second);
        // Erased here: look past it.
        at = change->first;
        at.push_back(0);
    }
}

void StoreOverlay::Note(std::span<const unsigned char> key)
{
    if (!m_journal) return;
    StoreBytes k(key.begin(), key.end());
    if (!m_noted.insert(k).second) return;
    m_undo.entries.emplace_back(std::move(k), Get(key));
}

void StoreOverlay::Put(std::span<const unsigned char> key, StoreBytes value)
{
    Note(key);
    const size_t size{value.size()};
    const auto [it, added]{m_changes.insert_or_assign(StoreBytes(key.begin(), key.end()), std::move(value))};
    // A map node, the key, the value: not exact, a bound to flush by.
    if (added) m_bytes += 96 + key.size();
    m_bytes += size;
}

void StoreOverlay::Erase(std::span<const unsigned char> key)
{
    Note(key);
    const auto [it, added]{m_changes.insert_or_assign(StoreBytes(key.begin(), key.end()), std::nullopt)};
    if (added) m_bytes += 96 + key.size();
}

void StoreOverlay::Revert(const StoreUndo& undo)
{
    // Each key once, with the value it had before the block: the order does not matter.
    for (const auto& [key, value] : undo.entries) {
        if (value) {
            Put(key, *value);
        } else {
            Erase(key);
        }
    }
}

StoreUndo StoreOverlay::TakeUndo()
{
    StoreUndo undo{std::move(m_undo)};
    m_undo.entries.clear();
    m_noted.clear();
    return undo;
}

void StoreOverlay::MergeInto(StoreOverlay& parent)
{
    assert(&parent == m_base);
    for (auto& [key, value] : m_changes) {
        if (value) {
            parent.Put(key, std::move(*value));
        } else {
            parent.Erase(key);
        }
    }
    Clear();
}

uint256 StoreHash(const StoreView& view, std::span<const unsigned char> prefix)
{
    HashWriter hasher{};
    view.ForEach(prefix, [&](const StoreBytes& key, const StoreBytes& value) {
        hasher << key << value;
        return true;
    });
    return hasher.GetHash();
}

} // namespace sidechain
