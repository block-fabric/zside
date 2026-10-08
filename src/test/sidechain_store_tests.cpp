// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <dbwrapper.h>
#include <sidechain/store.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <map>

using namespace sidechain;

namespace {
StoreBytes B(std::initializer_list<unsigned char> bytes) { return StoreBytes(bytes); }

std::map<StoreBytes, StoreBytes> All(const StoreView& view)
{
    std::map<StoreBytes, StoreBytes> all;
    view.ForEach({}, [&](const StoreBytes& key, const StoreBytes& value) {
        all.emplace(key, value);
        return true;
    });
    return all;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_store_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(overlays_and_undo)
{
    // A database, the chainstate's overlay on it, a block's overlay on that: reads see through,
    // a dropped block changes nothing, a kept one is undone by its journal.
    CDBWrapper db{DBParams{.path = m_args.GetDataDirBase() / "store", .cache_bytes = 1 << 20, .memory_only = true}};
    DbStore base{db, B({'T', 0})};
    StoreOverlay cache{base};
    cache.Put(B({1, 1}), B({10}));
    cache.Put(B({1, 2}), B({20}));
    cache.Put(B({2, 1}), B({30}));
    {
        CDBBatch batch{db};
        base.Write(batch, cache.Changes());
        db.WriteBatch(batch);
        cache.Clear();
    }
    const std::map<StoreBytes, StoreBytes> before{All(cache)};
    BOOST_CHECK_EQUAL(before.size(), 3U);

    {
        StoreOverlay block{cache, /*journal=*/true};
        block.Erase(B({1, 1}));
        block.Put(B({1, 3}), B({40}));
        block.Put(B({1, 2}), B({21}));
        block.Put(B({1, 2}), B({22}));
        BOOST_CHECK(!block.Contains(B({1, 1})));
        // In key order, the base's entries and the block's merged, the erased one gone.
        std::vector<StoreBytes> keys;
        block.ForEach(B({1}), [&](const StoreBytes& key, const StoreBytes&) {
            keys.push_back(key);
            return true;
        });
        BOOST_CHECK((keys == std::vector<StoreBytes>{B({1, 2}), B({1, 3})}));
        // Dropped: nothing changed below.
    }
    BOOST_CHECK(All(cache) == before);

    StoreOverlay block{cache, /*journal=*/true};
    block.Erase(B({1, 1}));
    block.Put(B({1, 3}), B({40}));
    block.Put(B({1, 2}), B({21}));
    block.Put(B({1, 2}), B({22}));
    const StoreUndo undo{block.TakeUndo()};
    // Each key once, with the value before the block.
    BOOST_CHECK_EQUAL(undo.entries.size(), 3U);
    block.MergeInto(cache);
    BOOST_CHECK(*cache.Get(B({1, 2})) == B({22}));
    // Through serialization, as it is stored.
    DataStream stream{};
    stream << undo;
    StoreUndo read;
    stream >> read;
    BOOST_CHECK(read == undo);
    cache.Revert(read);
    BOOST_CHECK(All(cache) == before);
    BOOST_CHECK(StoreHash(cache) == StoreHash(base));
}

BOOST_AUTO_TEST_CASE(next_and_size)
{
    EmptyStore empty;
    StoreOverlay base{empty};
    base.Put(StoreBytes{1, 1}, {7});
    base.Put(StoreBytes{2, 1}, {8});
    base.Put(StoreBytes{2, 2}, {9});
    StoreOverlay top{static_cast<const StoreView&>(base)};
    top.Put(StoreBytes{0, 9}, {1});
    top.Erase(StoreBytes{2, 1});
    // From before the prefix: the first entry under it, not one of the changes before it.
    const auto first{top.Next(StoreBytes{0}, StoreBytes{2})};
    BOOST_REQUIRE(first);
    BOOST_CHECK(first->first == (StoreBytes{2, 2}));
    // A value written again counts once.
    const size_t size{top.Bytes()};
    for (int i{0}; i < 100; ++i) top.Put(StoreBytes{0, 9}, StoreBytes(10, 1));
    BOOST_CHECK_EQUAL(top.Bytes(), size + 9);
    top.Erase(StoreBytes{0, 9});
    BOOST_CHECK_EQUAL(top.Bytes(), size - 1);
}

BOOST_AUTO_TEST_CASE(keys_keep_order)
{
    // Encoded keys sort as the values do: integers big-endian, outpoints by txid then index.
    std::vector<uint32_t> numbers{0, 1, 255, 256, 65535, 1u << 24, 0xffffffff};
    for (size_t i{1}; i < numbers.size(); ++i) BOOST_CHECK(TableKey(1, numbers[i - 1]) < TableKey(1, numbers[i]));
    const Txid a{Txid::FromUint256(uint256{1})}, b{Txid::FromUint256(uint256{2})};
    std::vector<COutPoint> outpoints{{a, 0}, {a, 1}, {a, 256}, {b, 0}};
    std::sort(outpoints.begin(), outpoints.end());
    for (size_t i{1}; i < outpoints.size(); ++i) BOOST_CHECK(TableKey(1, outpoints[i - 1]) < TableKey(1, outpoints[i]));
    for (const COutPoint& o : outpoints) {
        const StoreBytes key{TableKey(1, o)};
        std::span<const unsigned char> rest{key};
        rest = rest.subspan(1);
        BOOST_CHECK(KeyCodec<COutPoint>::Decode(rest) == o);
        BOOST_CHECK(rest.empty());
    }
}

BOOST_AUTO_TEST_CASE(random_against_a_map)
{
    // Three layers, random changes, merges and flushes: every read matches a plain map.
    FastRandomContext rng{/*fDeterministic=*/true};
    CDBWrapper db{DBParams{.path = m_args.GetDataDirBase() / "store2", .cache_bytes = 1 << 20, .memory_only = true}};
    DbStore base{db, B({'T', 0})};
    StoreOverlay cache{base};
    std::map<StoreBytes, StoreBytes> model;
    for (int round{0}; round < 200; ++round) {
        StoreOverlay block{cache, /*journal=*/true};
        std::map<StoreBytes, StoreBytes> next{model};
        for (int i{0}; i < 20; ++i) {
            const StoreBytes key{static_cast<unsigned char>(rng.randrange(3)), static_cast<unsigned char>(rng.randrange(16))};
            if (rng.randbool()) {
                const StoreBytes value{static_cast<unsigned char>(rng.randrange(256))};
                block.Put(key, value);
                next[key] = value;
            } else {
                block.Erase(key);
                next.erase(key);
            }
        }
        BOOST_REQUIRE(All(block) == next);
        for (unsigned char t{0}; t < 3; ++t) {
            std::map<StoreBytes, StoreBytes> in_table;
            for (const auto& [k, v] : next) if (k[0] == t) in_table.emplace(k, v);
            std::map<StoreBytes, StoreBytes> read;
            block.ForEach(B({t}), [&](const StoreBytes& k, const StoreBytes& v) { read.emplace(k, v); return true; });
            BOOST_REQUIRE(read == in_table);
        }
        const StoreUndo undo{block.TakeUndo()};
        if (rng.randbool()) {
            // Dropped.
            continue;
        }
        block.MergeInto(cache);
        BOOST_REQUIRE(All(cache) == next);
        if (rng.randrange(4) == 0) {
            // Undone: exactly what was before the block.
            cache.Revert(undo);
            BOOST_REQUIRE(All(cache) == model);
        } else {
            model = next;
        }
        if (rng.randrange(5) == 0) {
            // Flushed: the database alone reads the same.
            CDBBatch batch{db};
            base.Write(batch, cache.Changes());
            db.WriteBatch(batch);
            cache.Clear();
            BOOST_REQUIRE(All(base) == model);
        }
        BOOST_REQUIRE(StoreHash(cache) == StoreHash(base) || !cache.Changes().empty());
    }
}

BOOST_AUTO_TEST_CASE(database_reads_in_order)
{
    // Reads from the database go on with the iterator of the read before when they can: whatever the
    // order they come in, they read what a fresh iterator would, and what was written since.
    FastRandomContext rng{/*fDeterministic=*/true};
    CDBWrapper db{DBParams{.path = m_args.GetDataDirBase() / "store3", .cache_bytes = 1 << 20, .memory_only = true}};
    // Entries of other stores around this one's.
    db.Write(std::make_pair(uint8_t{'S'}, uint8_t{1}), uint8_t{1});
    db.Write(std::make_pair(uint8_t{'U'}, uint8_t{1}), uint8_t{1});
    DbStore other{db, B({'T', 'a', 0})};
    DbStore store{db, B({'T', 0})};
    std::map<StoreBytes, StoreBytes> model;
    const auto expect{[&](std::span<const unsigned char> from, std::span<const unsigned char> prefix) -> std::optional<std::pair<StoreBytes, StoreBytes>> {
        for (auto it{model.lower_bound(StoreBytes(from.begin(), from.end()))}; it != model.end(); ++it) {
            if (it->first.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), it->first.begin())) return *it;
            return std::nullopt;
        }
        return std::nullopt;
    }};
    for (int round{0}; round < 50; ++round) {
        // Written: by this store, or another one of the database.
        std::map<StoreBytes, std::optional<StoreBytes>> changes;
        for (int i{0}; i < 30; ++i) {
            const StoreBytes key{static_cast<unsigned char>(rng.randrange(3)), static_cast<unsigned char>(rng.randrange(8))};
            if (rng.randbool()) {
                changes[key] = StoreBytes{static_cast<unsigned char>(rng.randrange(256))};
                model[key] = *changes[key];
            } else {
                changes[key] = std::nullopt;
                model.erase(key);
            }
        }
        CDBBatch batch{db};
        store.Write(batch, changes);
        other.Write(batch, {{B({1, 1}), B({9})}});
        db.WriteBatch(batch);
        store.Reset();
        // Read: in order, again, back, by jumps, under prefixes.
        for (int i{0}; i < 200; ++i) {
            StoreBytes from{static_cast<unsigned char>(rng.randrange(3))};
            if (rng.randbool()) from.push_back(static_cast<unsigned char>(rng.randrange(8)));
            if (rng.randbool()) from.push_back(0);
            const StoreBytes prefix{rng.randbool() ? StoreBytes{} : StoreBytes{from[0]}};
            BOOST_REQUIRE(store.Next(from, prefix) == expect(from, prefix));
        }
        BOOST_REQUIRE(All(store) == model);
        BOOST_REQUIRE(StoreHash(store) == StoreHash(StoreOverlay{store}));
    }
    // Wiped, in batches as small as one entry: nothing of it is left, and nothing of the others goes.
    store.Wipe(/*batch_bytes=*/1);
    BOOST_CHECK(All(store).empty());
    BOOST_CHECK_EQUAL(All(other).size(), 1U);
    BOOST_CHECK(db.Exists(std::make_pair(uint8_t{'S'}, uint8_t{1})) && db.Exists(std::make_pair(uint8_t{'U'}, uint8_t{1})));
}

BOOST_AUTO_TEST_CASE(hash_with_entries)
{
    EmptyStore empty;
    StoreOverlay store{empty};
    for (unsigned char i{0}; i < 10; ++i) store.Put(B({static_cast<unsigned char>(i % 3), i}), B({i}));
    size_t entries{0};
    const uint256 hash{StoreHash(store, {}, [&](const StoreBytes&, const StoreBytes&) { ++entries; })};
    BOOST_CHECK(hash == StoreHash(store));
    BOOST_CHECK_EQUAL(entries, 10U);
}

BOOST_AUTO_TEST_CASE(table_ids_and_short_keys)
{
    // The tables of this build take a byte each.
    BOOST_CHECK(DuplicateTableIds().empty());
    {
        // One more under a byte already taken is a mistake, told until it goes; a copy is the same table.
        const Table<uint32_t, uint32_t> clash{'w'};
        const TableId copy{TableId{'z'}};
        BOOST_CHECK(DuplicateTableIds() == std::vector<uint8_t>{'w'});
    }
    BOOST_CHECK(DuplicateTableIds().empty());
    // A key shorter than its format is refused, not read past its end.
    const StoreBytes short_key(10, 1);
    std::span<const unsigned char> in{short_key};
    BOOST_CHECK_THROW(KeyCodec<uint256>::Decode(in), std::ios_base::failure);
    BOOST_CHECK_THROW(KeyCodec<COutPoint>::Decode(in), std::ios_base::failure);
    BOOST_CHECK_THROW(KeyCodec<uint64_t>::Decode(in = std::span<const unsigned char>{short_key}.first(7)), std::ios_base::failure);
    const StoreBytes empty_key;
    in = empty_key;
    BOOST_CHECK_THROW(KeyCodec<uint8_t>::Decode(in), std::ios_base::failure);
    // A table read under a key that is not of its format throws rather than reading past the key.
    EmptyStore none;
    StoreOverlay store{none};
    store.Put(B({'k', 1, 2}), EncodeValue(uint32_t{7}));
    const Table<uint32_t, uint32_t> table{'k'};
    BOOST_CHECK_THROW(table.ForEach(store, [](const uint32_t&, const uint32_t&) { return true; }), std::ios_base::failure);
}

BOOST_AUTO_TEST_SUITE_END()
