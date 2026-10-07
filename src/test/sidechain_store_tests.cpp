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

BOOST_AUTO_TEST_SUITE_END()
