// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <zside/shielded.h>

#include <crypto/common.h>
#include <hash.h>
#include <sync.h>
#include <zside/orchard.h>

#include <algorithm>
#include <cstring>

namespace zside {

namespace {
/** The data of a marker script: the bundle, after the tag and the version. */
std::optional<std::span<const uint8_t>> MarkerPayload(const CScript& script)
{
    if (script.size() < 2 || script[0] != OP_RETURN) return std::nullopt;
    CScript::const_iterator pc{script.begin() + 1};
    opcodetype opcode;
    if (!script.GetOp(pc, opcode) || opcode > OP_PUSHDATA4 || pc != script.end()) return std::nullopt;
    // The push is the rest of the script: where its data starts.
    size_t header{1};
    if (opcode == OP_PUSHDATA1) header = 2;
    if (opcode == OP_PUSHDATA2) header = 3;
    if (opcode == OP_PUSHDATA4) header = 5;
    std::span<const uint8_t> data{script.data() + 1 + header, script.size() - 1 - header};
    // Only the minimal push, as ShieldedMarkerScript writes it: the sighash stands for the marker by
    // the bundle's commitment, so another encoding of the same bundle would keep the signatures and
    // change the transaction's id, leaving its notes on a transaction that is never mined.
    const size_t n{data.size()};
    const opcodetype minimal{n < OP_PUSHDATA1 ? static_cast<opcodetype>(n) : n <= 0xff ? OP_PUSHDATA1 : n <= 0xffff ? OP_PUSHDATA2 : OP_PUSHDATA4};
    if (opcode != minimal) return std::nullopt;
    if (data.size() < sizeof(SHIELDED_TAG) + 1 || !std::equal(std::begin(SHIELDED_TAG), std::end(SHIELDED_TAG), data.begin())) return std::nullopt;
    if (data[sizeof(SHIELDED_TAG)] != SHIELDED_VERSION) return std::nullopt;
    return data.subspan(sizeof(SHIELDED_TAG) + 1);
}

/** Bundles whose proof and signatures were found valid, by the wtxid of their transaction. */
Mutex g_verified_mutex;
std::set<uint256> g_verified GUARDED_BY(g_verified_mutex);
constexpr size_t MAX_VERIFIED{50000};

bool Verified(const uint256& wtxid)
{
    LOCK(g_verified_mutex);
    return g_verified.contains(wtxid);
}

void SetVerified(const uint256& wtxid)
{
    LOCK(g_verified_mutex);
    if (g_verified.size() >= MAX_VERIFIED) g_verified.clear();
    g_verified.insert(wtxid);
}

uint256 HashFromBytes(const uint8_t* bytes)
{
    uint256 out;
    std::memcpy(out.data(), bytes, 32);
    return out;
}
} // namespace

CScript ShieldedMarkerScript(std::span<const uint8_t> bundle)
{
    std::vector<unsigned char> data(std::begin(SHIELDED_TAG), std::end(SHIELDED_TAG));
    data.push_back(SHIELDED_VERSION);
    data.insert(data.end(), bundle.begin(), bundle.end());
    return CScript() << OP_RETURN << data;
}

void Bundle::Free::operator()(ZoBundle* bundle) const { zo_bundle_free(bundle); }

std::optional<Bundle> Bundle::FromBytes(std::span<const uint8_t> bytes)
{
    ZoBundle* parsed{zo_bundle_parse(bytes.data(), bytes.size())};
    if (!parsed) return std::nullopt;
    Bundle bundle;
    bundle.m_bundle = std::shared_ptr<ZoBundle>{parsed, Free{}};
    return bundle;
}

std::optional<Bundle> Bundle::FromTx(const CTransaction& tx, std::string& error)
{
    error.clear();
    std::optional<Bundle> found;
    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        if (!IsShieldedMarker(tx.vout[i].scriptPubKey)) continue;
        if (found || !error.empty()) {
            error = "bad-zs-markers";
            return std::nullopt;
        }
        const auto payload{MarkerPayload(tx.vout[i].scriptPubKey)};
        if (payload) found = FromBytes(*payload);
        if (!found) {
            error = "bad-zs-bundle";
            continue;
        }
        found->marker_index = i;
    }
    if (!error.empty()) return std::nullopt;
    return found;
}

size_t Bundle::NumActions() const { return zo_bundle_num_actions(get()); }
CAmount Bundle::ValueBalance() const { return zo_bundle_value_balance(get()); }

uint256 Bundle::Anchor() const
{
    uint8_t out[32];
    zo_bundle_anchor(get(), out);
    return HashFromBytes(out);
}

std::vector<uint256> Bundle::Nullifiers() const
{
    std::vector<uint256> out(NumActions());
    for (size_t i{0}; i < out.size(); ++i) zo_bundle_nullifier(get(), i, out[i].data());
    return out;
}

std::vector<uint256> Bundle::Commitments() const
{
    std::vector<uint256> out(NumActions());
    for (size_t i{0}; i < out.size(); ++i) zo_bundle_cmx(get(), i, out[i].data());
    return out;
}

uint256 Bundle::Commitment() const
{
    uint256 out;
    zo_bundle_commitment(get(), out.data());
    return out;
}

CAmount ShieldedValueIn(const CTransaction& tx)
{
    for (const CTxOut& out : tx.vout) {
        const auto payload{MarkerPayload(out.scriptPubKey)};
        if (!payload) continue;
        // The value balance follows the actions and the flags: read it without the rest.
        std::span<const uint8_t> p{*payload};
        if (p.empty()) return 0;
        uint64_t n{p[0]};
        size_t at{1};
        if (p[0] == 0xfd) {
            if (p.size() < 3) return 0;
            n = p[1] | (uint64_t{p[2]} << 8);
            at = 3;
        } else if (p[0] > 0xfd) {
            return 0;
        }
        const uint64_t offset{at + n * 820 + 1};
        if (n > MAX_ACTIONS || p.size() < offset + 8) return 0;
        const int64_t balance{static_cast<int64_t>(ReadLE64(p.data() + offset))};
        return balance > 0 && MoneyRange(balance) ? balance : 0;
    }
    return 0;
}

uint256 ShieldedSighash(const CTransaction& tx, uint32_t marker_index, const uint256& bundle_commitment)
{
    HashWriter h{TaggedHash("zSide/Orchard sighash")};
    h << tx.version;
    WriteCompactSize(h, tx.vin.size());
    for (const CTxIn& in : tx.vin) h << in.prevout << in.nSequence;
    WriteCompactSize(h, tx.vout.size());
    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        h << tx.vout[i].nValue;
        if (i == marker_index) {
            // The bundle as its effects: not its proof and signatures, which sign this.
            h << ShieldedMarkerScript(std::span{bundle_commitment.data(), 32});
        } else {
            h << tx.vout[i].scriptPubKey;
        }
    }
    h << tx.nLockTime;
    return h.GetSHA256();
}

bool CheckShieldedTx(const CTransaction& tx, std::string& reject_reason)
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    std::string error;
    const auto bundle{Bundle::FromTx(tx, error)};
    if (!error.empty()) return invalid(error.c_str());
    if (!bundle) {
        // Without a bundle, a transaction has transparent inputs, as ever.
        if (tx.vin.empty()) return invalid("bad-txns-vin-empty");
        return true;
    }
    if (tx.IsCoinBase()) return invalid("bad-zs-coinbase");
    if (bundle->NumActions() > MAX_ACTIONS) return invalid("bad-zs-too-many-actions");
    const CAmount balance{bundle->ValueBalance()};
    if (balance < -MAX_MONEY || balance > MAX_MONEY) return invalid("bad-zs-value-balance");
    // What goes into the pool, the marker carries; it carries nothing else.
    if (tx.vout[bundle->marker_index].nValue != std::max<CAmount>(0, -balance)) return invalid("bad-zs-marker-value");
    if (Verified(tx.GetWitnessHash().ToUint256())) return true;
    const uint256 sighash{ShieldedSighash(tx, bundle->marker_index, bundle->Commitment())};
    const ZoBundle* bundles[1]{bundle->get()};
    if (!zo_bundles_verify(bundles, sighash.data(), 1)) return invalid("bad-zs-proof");
    SetVerified(tx.GetWitnessHash().ToUint256());
    return true;
}

//
// The tree.
//

uint256 EmptyRoot(unsigned level)
{
    static const std::array<uint256, TREE_DEPTH + 1> roots{[] {
        std::array<uint256, TREE_DEPTH + 1> r;
        for (unsigned l{0}; l <= TREE_DEPTH; ++l) zo_tree_empty_root(l, r[l].data());
        return r;
    }()};
    return roots.at(level);
}

uint256 Combine(unsigned level, const uint256& left, const uint256& right)
{
    uint256 out;
    // Nodes come from the tree itself, which holds valid ones only.
    Assert(zo_tree_combine(level, left.data(), right.data(), out.data()));
    return out;
}

void Frontier::Append(const uint256& cmx)
{
    if (m_ommers.size() != TREE_DEPTH) m_ommers.resize(TREE_DEPTH);
    uint256 node{cmx};
    unsigned level{0};
    while (level < TREE_DEPTH && (m_size >> level) & 1) {
        node = Combine(level, m_ommers[level], node);
        ++level;
    }
    if (level < TREE_DEPTH) m_ommers[level] = node;
    ++m_size;
}

uint256 Frontier::Root() const
{
    uint256 node{EmptyRoot(0)};
    for (unsigned level{0}; level < TREE_DEPTH; ++level) {
        if ((m_size >> level) & 1) {
            node = Combine(level, m_ommers[level], node);
        } else {
            node = Combine(level, node, EmptyRoot(level));
        }
    }
    return node;
}

//
// The state.
//

ShieldedState::ShieldedState()
{
    // The empty tree is an anchor from the start (a bundle that spends nothing names it).
    m_roots.insert(m_tree.Root());
}

bool ShieldedState::Check(const CTransaction& tx, const Bundle& bundle, std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    if (!m_roots.contains(bundle.Anchor())) return invalid("bad-zs-anchor");
    std::set<uint256> seen;
    for (const uint256& nf : bundle.Nullifiers()) {
        if (m_nullifiers.contains(nf) || !seen.insert(nf).second) return invalid("bad-zs-double-spend");
    }
    if (m_tree.Size() + bundle.NumActions() > (uint64_t{1} << TREE_DEPTH)) return invalid("bad-zs-tree-full");
    // The pool never holds less than nothing: a flaw in the proofs could not take out more than went in.
    if (m_pool - bundle.ValueBalance() < 0) return invalid("bad-zs-pool-negative");
    return true;
}

bool ShieldedState::CheckTx(const CTransaction& tx, std::string& reject_reason) const
{
    // The cheap checks against the state first (anchor, nullifiers, pool): a bundle that fails them
    // costs no proof verification -- one spent already, signed again, say.
    std::string error;
    if (const auto bundle{Bundle::FromTx(tx, error)}; bundle && !Check(tx, *bundle, reject_reason)) return false;
    return CheckShieldedTx(tx, reject_reason);
}

bool ShieldedState::ApplyTx(const CTransaction& tx, ShieldedUndo& undo, std::string& reject_reason)
{
    if (!CheckShieldedTx(tx, reject_reason)) return false;
    std::string error;
    const auto bundle{Bundle::FromTx(tx, error)};
    if (!bundle) return true;
    if (!Check(tx, *bundle, reject_reason)) return false;
    if (!undo.tree) undo.tree = m_tree;
    if (!undo.pool) undo.pool = m_pool;
    for (const uint256& nf : bundle->Nullifiers()) {
        m_nullifiers.insert(nf);
        undo.nullifiers.push_back(nf);
    }
    for (const uint256& cmx : bundle->Commitments()) m_tree.Append(cmx);
    m_pool -= bundle->ValueBalance();
    return true;
}

void ShieldedState::EndBlock(ShieldedUndo& undo)
{
    if (!undo.tree) return;
    const uint256 root{m_tree.Root()};
    if (m_roots.insert(root).second) undo.root = root;
}

void ShieldedState::Revert(const ShieldedUndo& undo)
{
    if (undo.root) m_roots.erase(*undo.root);
    for (const uint256& nf : undo.nullifiers) m_nullifiers.erase(nf);
    if (undo.tree) m_tree = *undo.tree;
    if (undo.pool) m_pool = *undo.pool;
}

} // namespace zside
