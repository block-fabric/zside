// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_STORE_H
#define BITCOIN_SIDECHAIN_STORE_H

#include <primitives/transaction.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

class CDBBatch;
class CDBWrapper;

/**
 * The state of this chain as a sidechain, one entry per key, as Bitcoin keeps its coins.
 *
 * Every table of the state (withdrawals; a sidechain's names, tokens, nullifiers…) lives under a
 * key prefix of its own, one database entry per element. Nothing the size of the whole state is
 * copied, written or held in memory:
 *
 *  - the database holds every entry (DbStore);
 *  - the chainstate keeps, over it, the entries changed since the last flush, and a flush writes
 *    those alone (StoreOverlay, flushed with Flush());
 *  - connecting a block, building a block template, checking a transaction for the mempool:
 *    each works on an overlay of its own over that, kept or dropped after. An overlay that
 *    journals notes the earlier value of each key the first time it changes it: that list is
 *    the undo data of a block.
 *
 * Keys are bytes, compared as bytes: a table's keys are encoded so that their byte order is the
 * order the rules read the table in (big-endian integers, hashes as stored).
 */
namespace sidechain {

using StoreBytes = std::vector<unsigned char>;

/** The earlier values of the keys a block changed (nullopt: the key had no entry), in the order first changed. */
struct StoreUndo {
    std::vector<std::pair<StoreBytes, std::optional<StoreBytes>>> entries;

    bool empty() const { return entries.empty(); }
    template <typename Stream>
    void Serialize(Stream& s) const
    {
        WriteCompactSize(s, entries.size());
        for (const auto& [key, value] : entries) {
            s << key << value.has_value();
            if (value) s << *value;
        }
    }
    template <typename Stream>
    void Unserialize(Stream& s)
    {
        entries.clear();
        const uint64_t n{ReadCompactSize(s)};
        for (uint64_t i{0}; i < n; ++i) {
            StoreBytes key;
            bool has;
            s >> key >> has;
            std::optional<StoreBytes> value;
            if (has) s >> value.emplace();
            entries.emplace_back(std::move(key), std::move(value));
        }
    }
    friend bool operator==(const StoreUndo&, const StoreUndo&) = default;
};

/** An ordered map of keys to values, read only. */
class StoreView
{
public:
    virtual ~StoreView() = default;
    virtual std::optional<StoreBytes> Get(std::span<const unsigned char> key) const = 0;
    /** The first entry whose key is at least `from` and starts with `prefix`, if any. */
    virtual std::optional<std::pair<StoreBytes, StoreBytes>> Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const = 0;

    bool Contains(std::span<const unsigned char> key) const { return Get(key).has_value(); }
    /** Every entry under `prefix`, in key order, until `fn` returns false. */
    void ForEach(std::span<const unsigned char> prefix, const std::function<bool(const StoreBytes&, const StoreBytes&)>& fn) const;
};

/** No entries: the base of a state that has none yet, and of tests. */
class EmptyStore : public StoreView
{
public:
    std::optional<StoreBytes> Get(std::span<const unsigned char>) const override { return std::nullopt; }
    std::optional<std::pair<StoreBytes, StoreBytes>> Next(std::span<const unsigned char>, std::span<const unsigned char>) const override { return std::nullopt; }
};

/** The entries in a database, under a prefix of their own (which the keys given here leave out). */
class DbStore : public StoreView
{
public:
    DbStore(CDBWrapper& db, StoreBytes prefix) : m_db{db}, m_prefix{std::move(prefix)} {}
    std::optional<StoreBytes> Get(std::span<const unsigned char> key) const override;
    std::optional<std::pair<StoreBytes, StoreBytes>> Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const override;
    /** Put the changes in a batch. */
    void Write(CDBBatch& batch, const std::map<StoreBytes, std::optional<StoreBytes>>& changes) const;
    /** Erase every entry (a state started over). */
    void Wipe(CDBBatch& batch) const;
    CDBWrapper& Database() const { return m_db; }

private:
    StoreBytes Full(std::span<const unsigned char> key) const;
    CDBWrapper& m_db;
    StoreBytes m_prefix;
};

/** Changes over another view, kept apart from it until merged into it or dropped. */
class StoreOverlay : public StoreView
{
public:
    explicit StoreOverlay(const StoreView& base, bool journal = false) : m_base{&base}, m_journal{journal} {}
    StoreOverlay(const StoreOverlay&) = delete;
    StoreOverlay& operator=(const StoreOverlay&) = delete;

    std::optional<StoreBytes> Get(std::span<const unsigned char> key) const override;
    std::optional<std::pair<StoreBytes, StoreBytes>> Next(std::span<const unsigned char> from, std::span<const unsigned char> prefix) const override;

    void Put(std::span<const unsigned char> key, StoreBytes value);
    void Erase(std::span<const unsigned char> key);
    /** Put back the values an undo list holds. */
    void Revert(const StoreUndo& undo);

    /** The earlier values of what changed since the journal was started or taken. */
    StoreUndo TakeUndo();
    const std::map<StoreBytes, std::optional<StoreBytes>>& Changes() const { return m_changes; }
    /** Apply the changes to `parent` (which must be this overlay's base) and forget them. */
    void MergeInto(StoreOverlay& parent);
    void Clear()
    {
        m_changes.clear();
        m_undo.entries.clear();
        m_noted.clear();
        m_bytes = 0;
    }
    const StoreView& Base() const { return *m_base; }
    /** Roughly the memory the changes take. */
    size_t Bytes() const { return m_bytes; }

private:
    void Note(std::span<const unsigned char> key);
    void Set(std::span<const unsigned char> key, std::optional<StoreBytes> value);

    const StoreView* m_base;
    //! Value or erasure of each key changed here.
    std::map<StoreBytes, std::optional<StoreBytes>> m_changes;
    bool m_journal;
    StoreUndo m_undo;
    std::set<StoreBytes> m_noted;
    size_t m_bytes{0};
};

/** A hash over every entry under `prefix`, in key order: the same for the same state, whatever stores it. */
uint256 StoreHash(const StoreView& view, std::span<const unsigned char> prefix = {});

//
// Keys. A table is a byte; then the key, encoded so that byte order is the order of the key.
//

/** Appends the encoding of a key to `out`. Specialised per key type. */
template <typename K>
struct KeyCodec;

template <>
struct KeyCodec<uint32_t> {
    static void Encode(StoreBytes& out, uint32_t v)
    {
        for (int i{3}; i >= 0; --i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
    static uint32_t Decode(std::span<const unsigned char>& in)
    {
        uint32_t v{0};
        for (int i{0}; i < 4; ++i) v = (v << 8) | in[i];
        in = in.subspan(4);
        return v;
    }
};

template <>
struct KeyCodec<uint64_t> {
    static void Encode(StoreBytes& out, uint64_t v)
    {
        for (int i{7}; i >= 0; --i) out.push_back(static_cast<unsigned char>(v >> (8 * i)));
    }
    static uint64_t Decode(std::span<const unsigned char>& in)
    {
        uint64_t v{0};
        for (int i{0}; i < 8; ++i) v = (v << 8) | in[i];
        in = in.subspan(8);
        return v;
    }
};

template <>
struct KeyCodec<uint8_t> {
    static void Encode(StoreBytes& out, uint8_t v) { out.push_back(v); }
    static uint8_t Decode(std::span<const unsigned char>& in)
    {
        const uint8_t v{in[0]};
        in = in.subspan(1);
        return v;
    }
};

/** Hashes as base_blob compares them: their bytes in order. */
template <>
struct KeyCodec<uint256> {
    static void Encode(StoreBytes& out, const uint256& v) { out.insert(out.end(), v.begin(), v.end()); }
    static uint256 Decode(std::span<const unsigned char>& in)
    {
        uint256 v;
        std::copy(in.begin(), in.begin() + 32, v.begin());
        in = in.subspan(32);
        return v;
    }
};

template <>
struct KeyCodec<uint160> {
    static void Encode(StoreBytes& out, const uint160& v) { out.insert(out.end(), v.begin(), v.end()); }
    static uint160 Decode(std::span<const unsigned char>& in)
    {
        uint160 v;
        std::copy(in.begin(), in.begin() + 20, v.begin());
        in = in.subspan(20);
        return v;
    }
};

/** As COutPoint compares: the txid's bytes, then the index. */
template <>
struct KeyCodec<COutPoint> {
    static void Encode(StoreBytes& out, const COutPoint& v)
    {
        KeyCodec<uint256>::Encode(out, v.hash.ToUint256());
        KeyCodec<uint32_t>::Encode(out, v.n);
    }
    static COutPoint Decode(std::span<const unsigned char>& in)
    {
        const uint256 hash{KeyCodec<uint256>::Decode(in)};
        return COutPoint{Txid::FromUint256(hash), KeyCodec<uint32_t>::Decode(in)};
    }
};

template <typename K>
StoreBytes TableKey(uint8_t table, const K& key)
{
    StoreBytes out{table};
    KeyCodec<K>::Encode(out, key);
    return out;
}

template <typename V>
StoreBytes EncodeValue(const V& value)
{
    DataStream s{};
    s << value;
    return StoreBytes(UCharCast(s.data()), UCharCast(s.data()) + s.size());
}

template <typename V>
V DecodeValue(std::span<const unsigned char> bytes)
{
    SpanReader s{bytes};
    V value;
    s >> value;
    return value;
}

/**
 * A table of the state: keys of type K, values of type V, under one table byte, read from a
 * view and written to an overlay.
 */
template <typename K, typename V>
class Table
{
public:
    explicit Table(uint8_t id) : m_id{id} {}
    uint8_t Id() const { return m_id; }

    std::optional<V> Get(const StoreView& view, const K& key) const
    {
        const auto bytes{view.Get(TableKey(m_id, key))};
        if (!bytes) return std::nullopt;
        return DecodeValue<V>(*bytes);
    }
    bool Contains(const StoreView& view, const K& key) const { return view.Contains(TableKey(m_id, key)); }
    void Put(StoreOverlay& overlay, const K& key, const V& value) const { overlay.Put(TableKey(m_id, key), EncodeValue(value)); }
    void Erase(StoreOverlay& overlay, const K& key) const { overlay.Erase(TableKey(m_id, key)); }
    /** Every entry from `from` on (all of them if not given), in key order, until `fn` returns false. */
    void ForEach(const StoreView& view, const std::function<bool(const K&, const V&)>& fn, const std::optional<K>& from = std::nullopt) const
    {
        const StoreBytes prefix{m_id};
        StoreBytes at{from ? TableKey(m_id, *from) : prefix};
        while (const auto entry{view.Next(at, prefix)}) {
            std::span<const unsigned char> rest{entry->first};
            rest = rest.subspan(1);
            const K key{KeyCodec<K>::Decode(rest)};
            if (!fn(key, DecodeValue<V>(entry->second))) return;
            at = entry->first;
            at.push_back(0);
        }
    }
    /** Every entry whose key starts with the encoding of `head` (a leading part of the key). */
    template <typename H>
    void ForEachWith(const StoreView& view, const H& head, const std::function<bool(const K&, const V&)>& fn) const
    {
        const StoreBytes prefix{TableKey(m_id, head)};
        StoreBytes at{prefix};
        while (const auto entry{view.Next(at, prefix)}) {
            std::span<const unsigned char> rest{entry->first};
            rest = rest.subspan(1);
            const K key{KeyCodec<K>::Decode(rest)};
            if (!fn(key, DecodeValue<V>(entry->second))) return;
            at = entry->first;
            at.push_back(0);
        }
    }

private:
    uint8_t m_id;
};

/** A single value of the state (a height, a counter), under a table byte of its own. */
template <typename V>
class Cell
{
public:
    Cell(uint8_t id, V fallback) : m_id{id}, m_fallback{std::move(fallback)} {}
    V Get(const StoreView& view) const
    {
        const auto bytes{view.Get(StoreBytes{m_id})};
        return bytes ? DecodeValue<V>(*bytes) : m_fallback;
    }
    void Put(StoreOverlay& overlay, const V& value) const
    {
        // The fallback is stored as no entry, so that a state at its start has none.
        if (value == m_fallback) {
            overlay.Erase(StoreBytes{m_id});
        } else {
            overlay.Put(StoreBytes{m_id}, EncodeValue(value));
        }
    }

private:
    uint8_t m_id;
    V m_fallback;
};

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_STORE_H
