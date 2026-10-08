// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <zside/shielded.h>

#include <util/check.h>

#include <consensus/params.h>
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

/**
 * Bundles whose proof and signatures were found valid, by the wtxid of their transaction and the
 * sighash they were verified against: one valid in a domain is not taken as valid in another.
 */
Mutex g_verified_mutex;
std::set<uint256> g_verified GUARDED_BY(g_verified_mutex);
constexpr size_t MAX_VERIFIED{50000};

uint256 VerifiedKey(const CTransaction& tx, const uint256& sighash)
{
    return (HashWriter{} << tx.GetWitnessHash() << sighash).GetSHA256();
}

bool Verified(const uint256& key)
{
    LOCK(g_verified_mutex);
    return g_verified.contains(key);
}

void SetVerified(const uint256& key)
{
    LOCK(g_verified_mutex);
    if (g_verified.size() >= MAX_VERIFIED) g_verified.clear();
    g_verified.insert(key);
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

std::optional<SighashDomain> SighashDomainAt(const Consensus::SidechainParams& params, const uint256& genesis, int height)
{
    if (height < params.zside.audit2_height) return std::nullopt;
    return SighashDomain{.genesis = genesis, .slot = params.slot};
}

std::optional<SighashDomain> SighashDomainAt(const Consensus::Params& params, int height)
{
    return SighashDomainAt(params.sidechain, params.hashGenesisBlock, height);
}

uint256 ShieldedSighash(const CTransaction& tx, uint32_t marker_index, const uint256& bundle_commitment, const std::optional<SighashDomain>& domain)
{
    // In a domain: a tag of its own (no sighash of one is one of the other), then the chain.
    HashWriter h{TaggedHash(domain ? "zSide/Orchard sighash/chain" : "zSide/Orchard sighash")};
    if (domain) h << SIGHASH_BRANCH_ID << domain->genesis << domain->slot;
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

bool CheckShieldedTx(const CTransaction& tx, std::string& reject_reason, const std::optional<SighashDomain>& domain)
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
    const uint256 sighash{ShieldedSighash(tx, bundle->marker_index, bundle->Commitment(), domain)};
    const uint256 key{VerifiedKey(tx, sighash)};
    if (Verified(key)) return true;
    const ZoBundle* bundles[1]{bundle->get()};
    if (!zo_bundles_verify(bundles, sighash.data(), 1)) return invalid("bad-zs-proof");
    SetVerified(key);
    return true;
}

bool VerifyBundles(std::span<const CTransactionRef> txs, const std::optional<SighashDomain>& domain)
{
    std::vector<Bundle> bundles;
    std::vector<const ZoBundle*> list;
    std::vector<uint8_t> sighashes;
    std::vector<uint256> keys;
    for (const CTransactionRef& tx : txs) {
        if (!HasShieldedMarker(*tx)) continue;
        std::string error;
        auto bundle{Bundle::FromTx(*tx, error)};
        // Those CheckShieldedTx refuses before the proof are left to it.
        if (!bundle || bundle->NumActions() > MAX_ACTIONS) continue;
        const uint256 sighash{ShieldedSighash(*tx, bundle->marker_index, bundle->Commitment(), domain)};
        const uint256 key{VerifiedKey(*tx, sighash)};
        if (Verified(key)) continue;
        list.push_back(bundle->get());
        bundles.push_back(std::move(*bundle));
        sighashes.insert(sighashes.end(), sighash.begin(), sighash.end());
        keys.push_back(key);
    }
    // One alone costs the same either way.
    if (list.size() < 2) return true;
    if (!zo_bundles_verify(list.data(), sighashes.data(), list.size())) return false;
    for (const uint256& key : keys) SetVerified(key);
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

namespace {
const sidechain::Table<uint256, bool> NULLIFIERS{0x10};
const sidechain::Table<uint256, bool> ANCHORS{0x11};
const sidechain::Cell<Frontier> TREE{0x12, Frontier{}};
const sidechain::Cell<CAmount> POOL{0x13, 0};
const sidechain::Cell<uint64_t> NULLIFIER_COUNT{0x14, 0};
const sidechain::Cell<uint64_t> ANCHOR_COUNT{0x15, 0};

const uint256& EmptyTreeRoot()
{
    static const uint256 root{Frontier{}.Root()};
    return root;
}
} // namespace

Frontier ShieldedState::Tree() const { return TREE.Get(*m_view); }
bool ShieldedState::IsSpent(const uint256& nullifier) const { return NULLIFIERS.Contains(*m_view, nullifier); }
bool ShieldedState::IsAnchor(const uint256& root) const { return root == EmptyTreeRoot() || ANCHORS.Contains(*m_view, root); }
uint64_t ShieldedState::AnchorCount() const { return 1 + ANCHOR_COUNT.Get(*m_view); }
CAmount ShieldedState::Pool() const { return POOL.Get(*m_view); }
uint64_t ShieldedState::NullifierCount() const { return NULLIFIER_COUNT.Get(*m_view); }

bool ShieldedState::Check(const CTransaction& tx, const Bundle& bundle, std::string& reject_reason) const
{
    const auto invalid{[&](const char* reason) {
        reject_reason = reason;
        return false;
    }};
    // In range before anything is computed with it: the pool's balance below is defined only so
    // (CheckShieldedTx refuses any other, but may come after: the mempool checks the proof last).
    const CAmount balance{bundle.ValueBalance()};
    if (balance < -MAX_MONEY || balance > MAX_MONEY) return invalid("bad-zs-value-balance");
    if (!IsAnchor(bundle.Anchor())) return invalid("bad-zs-anchor");
    std::set<uint256> seen;
    for (const uint256& nf : bundle.Nullifiers()) {
        if (IsSpent(nf) || !seen.insert(nf).second) return invalid("bad-zs-double-spend");
    }
    if (Tree().Size() + bundle.NumActions() > (uint64_t{1} << TREE_DEPTH)) return invalid("bad-zs-tree-full");
    // The pool never holds less than nothing: a flaw in the proofs could not take out more than went in.
    if (Pool() - balance < 0) return invalid("bad-zs-pool-negative");
    return true;
}

bool ShieldedState::CheckState(const CTransaction& tx, std::string& reject_reason) const
{
    // The cheap checks against the state (anchor, nullifiers, pool): a bundle that fails them costs
    // no proof verification -- one spent already, signed again, say.
    std::string error;
    const auto bundle{Bundle::FromTx(tx, error)};
    if (!error.empty()) {
        reject_reason = error;
        return false;
    }
    return !bundle || Check(tx, *bundle, reject_reason);
}

bool ShieldedState::ApplyTx(const CTransaction& tx, std::string& reject_reason, const std::optional<SighashDomain>& domain)
{
    if (!CheckShieldedTx(tx, reject_reason, domain)) return false;
    std::string error;
    const auto bundle{Bundle::FromTx(tx, error)};
    if (!bundle) return true;
    if (!Check(tx, *bundle, reject_reason)) return false;
    sidechain::StoreOverlay& out{*Assert(m_overlay)};
    uint64_t spent{NullifierCount()};
    for (const uint256& nf : bundle->Nullifiers()) {
        NULLIFIERS.Put(out, nf, true);
        ++spent;
    }
    NULLIFIER_COUNT.Put(out, spent);
    Frontier tree{Tree()};
    for (const uint256& cmx : bundle->Commitments()) tree.Append(cmx);
    TREE.Put(out, tree);
    POOL.Put(out, Pool() - bundle->ValueBalance());
    return true;
}

void ShieldedState::EndBlock()
{
    // A root the tree had before is an anchor already: only a new one is added.
    const uint256 root{Tree().Root()};
    if (IsAnchor(root)) return;
    sidechain::StoreOverlay& out{*Assert(m_overlay)};
    ANCHORS.Put(out, root, true);
    ANCHOR_COUNT.Put(out, ANCHOR_COUNT.Get(*m_view) + 1);
}

} // namespace zside
