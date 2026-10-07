// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/store.h>

#include <hash.h>

#include <algorithm>
#include <memory>

namespace sidechain {

namespace {
bool StartsWith(std::span<const unsigned char> key, std::span<const unsigned char> prefix)
{
    return key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin());
}

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
    // Nothing before the prefix is under it.
    if (std::lexicographical_compare(at.begin(), at.end(), prefix.begin(), prefix.end())) at.assign(prefix.begin(), prefix.end());
    // The first change at or after `at`, and the first entry of the base: the smaller key wins, and a
    // change hides the base's entry of the same key.
    auto change{m_changes.lower_bound(at)};
    auto base{m_base->Next(at, prefix)};
    while (true) {
        if (change != m_changes.end() && !StartsWith(change->first, prefix)) change = m_changes.end();
        if (change == m_changes.end()) return base;
        if (base && base->first < change->first) return base;
        if (change->second) return std::make_pair(change->first, *change->second);
        // Erased here: look past it. The base is asked again only if this hid its entry.
        if (base && base->first == change->first) {
            StoreBytes past{change->first};
            past.push_back(0);
            base = m_base->Next(past, prefix);
        }
        ++change;
    }
}

void StoreOverlay::Note(std::span<const unsigned char> key)
{
    if (!m_journal) return;
    StoreBytes k(key.begin(), key.end());
    if (!m_noted.insert(k).second) return;
    m_undo.entries.emplace_back(std::move(k), Get(key));
}

void StoreOverlay::Set(std::span<const unsigned char> key, std::optional<StoreBytes> value)
{
    Note(key);
    // A map node, the key, the value: not exact, a bound to flush by. A value replaced no longer counts.
    const size_t size{value ? value->size() : 0};
    StoreBytes k(key.begin(), key.end());
    if (const auto it{m_changes.find(k)}; it != m_changes.end()) {
        m_bytes -= it->second ? it->second->size() : 0;
        it->second = std::move(value);
    } else {
        m_bytes += 96 + k.size();
        m_changes.emplace(std::move(k), std::move(value));
    }
    m_bytes += size;
}

void StoreOverlay::Put(std::span<const unsigned char> key, StoreBytes value) { Set(key, std::move(value)); }

void StoreOverlay::Erase(std::span<const unsigned char> key) { Set(key, std::nullopt); }

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
