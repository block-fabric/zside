// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/shielded.h>

#include <bech32.h>
#include <chain.h>
#include <interfaces/chain.h>
#include <chainparams.h>
#include <common/args.h>
#include <hash.h>
#include <key_io.h>
#include <policy/policy.h>
#include <support/cleanse.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/rpc/util.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <zside/shielded.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <set>

namespace wallet {

namespace {
std::string ShieldedHrp()
{
    switch (Params().GetChainType()) {
    case ChainType::MAIN: return "zo";
    case ChainType::REGTEST: return "rzo";
    default: return "tzo";
    }
}

/** ZIP 32 coin type of the account: the seed is of this chain already, so any fixed value does. */
constexpr uint32_t COIN_TYPE{1};

uint256 HashBytes(const uint8_t* bytes)
{
    uint256 out;
    std::memcpy(out.data(), bytes, 32);
    return out;
}

/** The bytes of a bundle's sighash marker placeholder: a bundle of `size` bytes, all zero. */
CScript PlaceholderMarker(size_t size)
{
    return zside::ShieldedMarkerScript(std::vector<uint8_t>(size, 0));
}

/** The serialized size of a transaction with these outputs and no transparent inputs. */
size_t ShieldedTxSize(const std::vector<CTxOut>& outputs)
{
    CMutableTransaction mtx;
    mtx.version = CTransaction::CURRENT_VERSION;
    mtx.vout = outputs;
    return GetSerializeSize(TX_WITH_WITNESS(CTransaction{mtx}));
}
} // namespace

std::string EncodeShieldedAddress(const OrchardAddress& address)
{
    std::vector<uint8_t> data;
    ConvertBits<8, 5, true>([&](uint8_t c) { data.push_back(c); }, address.begin(), address.end());
    return bech32::Encode(bech32::Encoding::BECH32M, ShieldedHrp(), data);
}

std::optional<OrchardAddress> DecodeShieldedAddress(const std::string& text)
{
    const auto decoded{bech32::Decode(text)};
    if (decoded.encoding != bech32::Encoding::BECH32M || decoded.hrp != ShieldedHrp()) return std::nullopt;
    std::vector<uint8_t> bytes;
    if (!ConvertBits<5, 8, false>([&](uint8_t c) { bytes.push_back(c); }, decoded.data.begin(), decoded.data.end())) return std::nullopt;
    if (bytes.size() != zside::ADDRESS_SIZE) return std::nullopt;
    OrchardAddress address;
    std::copy(bytes.begin(), bytes.end(), address.begin());
    if (!zo_address_valid(address.data())) return std::nullopt;
    return address;
}

std::string MemoText(const OrchardMemo& memo)
{
    // 0xF6 then zeros: no memo. Anything else up to 0xF4 is text.
    if (memo[0] >= 0xF5) return {};
    const auto end{std::find(memo.begin(), memo.end(), 0)};
    return std::string(memo.begin(), end);
}

std::optional<OrchardMemo> MemoFromText(const std::string& text)
{
    OrchardMemo memo{};
    if (text.empty()) {
        memo[0] = 0xF6;
        return memo;
    }
    if (text.size() > memo.size() || static_cast<uint8_t>(text[0]) > 0xF4) return std::nullopt;
    std::copy(text.begin(), text.end(), memo.begin());
    return memo;
}

std::optional<ShieldedKey> ShieldedSpendingKey(const CWallet& wallet, int version)
{
    const Consensus::Params& consensus{Params().GetConsensus()};
    return ShieldedSpendingKey(wallet, version, consensus.hashGenesisBlock, consensus.sidechain.slot);
}

std::optional<ShieldedKey> ShieldedSpendingKey(const CWallet& wallet, int version, const uint256& genesis, uint32_t slot)
{
    if (version != KEY_ANY_CHAIN && version != KEY_THIS_CHAIN) return std::nullopt;
    const auto* spkm{dynamic_cast<const DescriptorScriptPubKeyMan*>(wallet.GetScriptPubKeyMan(OutputType::BECH32, /*internal=*/false))};
    if (!spkm) return std::nullopt;
    // The descriptor with its private keys, and all made of it, wiped once read.
    const auto wipe{[](std::string& text) { memory_cleanse(text.data(), text.size()); }};
    std::string descriptor;
    std::optional<ShieldedKey> out;
    if (spkm->GetDescriptorString(descriptor, /*priv=*/true)) {
        // The first extended private key of the descriptor (the account key, which is restored with it).
        for (const std::string prefix : {"xprv", "tprv"}) {
            const size_t at{descriptor.find(prefix)};
            if (at == std::string::npos) continue;
            size_t end{at};
            while (end < descriptor.size() && std::string_view{"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"}.find(descriptor[end]) != std::string_view::npos) ++end;
            std::string encoded{descriptor.substr(at, end - at)};
            const CExtKey extkey{DecodeExtKey(encoded)};
            wipe(encoded);
            if (!extkey.key.IsValid()) break;
            std::array<unsigned char, BIP32_EXTKEY_SIZE> code;
            extkey.Encode(code.data());
            // Version 1: of the extended key alone, the same on every chain. Version 2: of the chain
            // too (its genesis block and slot), so that one seed makes unrelated keys on each.
            HashWriter seed{TaggedHash(version == KEY_ANY_CHAIN ? "zSide/Orchard seed" : "zSide/Orchard seed/chain")};
            seed.write(std::as_bytes(std::span{code}));
            if (version == KEY_THIS_CHAIN) seed << genesis << slot;
            memory_cleanse(code.data(), code.size());
            uint256 seed_hash{seed.GetSHA256()};
            ShieldedKey sk(32);
            const int result{zo_spending_key_from_seed(seed_hash.data(), 32, COIN_TYPE, 0, sk.data())};
            memory_cleanse(seed_hash.data(), seed_hash.size());
            if (result == ZO_OK) out = std::move(sk);
            break;
        }
    }
    wipe(descriptor);
    return out;
}

namespace {
std::optional<OrchardFvk> ViewingKeyOf(const CWallet& wallet, int version)
{
    const auto sk{ShieldedSpendingKey(wallet, version)};
    if (!sk) return std::nullopt;
    OrchardFvk fvk;
    if (zo_full_viewing_key(sk->data(), fvk.data()) != ZO_OK) return std::nullopt;
    return fvk;
}

/** The version of the keys of an account set up now: 2 (on regtest, -zsidekeyversion may say 1, for tests). */
int NewKeyVersion()
{
    if (Params().GetChainType() != ChainType::REGTEST) return KEY_THIS_CHAIN;
    return gArgs.GetIntArg("-zsidekeyversion", KEY_THIS_CHAIN) == KEY_ANY_CHAIN ? KEY_ANY_CHAIN : KEY_THIS_CHAIN;
}
} // namespace

std::vector<std::pair<const OrchardFvk*, bool>> ShieldedViewingKeys(const ShieldedAccount& account)
{
    std::vector<std::pair<const OrchardFvk*, bool>> keys;
    if (account.fvk) keys.emplace_back(&*account.fvk, false);
    if (account.legacy_fvk) keys.emplace_back(&*account.legacy_fvk, true);
    return keys;
}

int ShieldedAddressScope(const ShieldedAccount& account, const OrchardAddress& address)
{
    for (const auto& [fvk, legacy] : ShieldedViewingKeys(account)) {
        if (const int scope{zo_address_scope(fvk->data(), address.data())}) return scope;
    }
    return 0;
}

bool SetupShieldedAccount(CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    if (wallet.m_shielded.fvk) return true;
    // An account set up now has keys of this chain (version 2). The keys of version 1 of the same
    // descriptor are watched too: a copy of the wallet set up before (or the wallet whose descriptor
    // this one was restored from) may have been paid at them, and their notes are the account's.
    // An account set up before keeps its keys of version 1, and no others (WalletBatch::LoadWallet).
    const int version{NewKeyVersion()};
    const auto fvk{ViewingKeyOf(wallet, version)};
    if (!fvk) return false;
    std::optional<OrchardFvk> legacy;
    if (version != KEY_ANY_CHAIN) {
        legacy = ViewingKeyOf(wallet, KEY_ANY_CHAIN);
        if (!legacy) return false;
    }
    WalletBatch batch{wallet.GetDatabase()};
    // All or nothing: a viewing key kept without its version would be taken for one of version 1.
    if (!batch.TxnBegin()) return false;
    bool written{batch.WriteOrchardKeyVersion(version)};
    if (legacy) written = written && batch.WriteOrchardLegacyViewingKey(std::vector<unsigned char>(legacy->begin(), legacy->end()));
    written = written && batch.WriteOrchardViewingKey(std::vector<unsigned char>(fvk->begin(), fvk->end()));
    if (!written) {
        batch.TxnAbort();
        return false;
    }
    if (!batch.TxnCommit()) return false;
    wallet.m_shielded.fvk = fvk;
    wallet.m_shielded.key_version = version;
    wallet.m_shielded.legacy_fvk = legacy;
    // What the wallet keeps already may pay the account.
    for (const auto& [txid, wtx] : wallet.mapWallet) ShieldedTxAdded(wallet, wtx);
    // Blocks the wallet scanned without the account may have paid it alone, with nothing the wallet
    // kept then: those since the wallet's birth are scanned again.
    if (wallet.HaveChain() && wallet.GetLastBlockHeight() >= 0) {
        int from{0};
        const int64_t birth{wallet.GetBirthTime()};
        bool found{birth == std::numeric_limits<int64_t>::max()};
        if (!found) found = wallet.chain().findFirstBlockWithTimeAndHeight(birth - TIMESTAMP_WINDOW, 0, interfaces::FoundBlock().height(from));
        if (found && from <= wallet.GetLastBlockHeight()) {
            wallet.m_shielded.rescan_from = from;
            batch.WriteOrchardRescan(from);
            wallet.WalletLogPrintf("zSide: the shielded account is set up after the wallet scanned blocks without it: they are to be scanned again, from block %d\n", from);
        }
    }
    return true;
}

void ShieldedRescan(CWallet& wallet)
{
    const std::optional<int> from{WITH_LOCK(wallet.cs_wallet, return wallet.m_shielded.rescan_from)};
    if (!from) return;
    // Another rescan going on: the next call does it.
    WalletRescanReserver reserver{wallet};
    if (!reserver.reserve()) return;
    const std::optional<int> tip{wallet.chain().getHeight()};
    if (!tip) return;
    const int start{std::min(*from, *tip)};
    wallet.WalletLogPrintf("zSide: scanning again from block %d for the shielded account\n", start);
    // A scan that goes through clears the mark (ShieldedScanned).
    (void)wallet.ScanForWalletTransactions(wallet.chain().getBlockHash(start), start, /*max_height=*/{}, reserver, /*save_progress=*/false);
}

void ShieldedScanned(CWallet& wallet, int start_height)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!wallet.m_shielded.rescan_from || start_height > *wallet.m_shielded.rescan_from) return;
    wallet.m_shielded.rescan_from.reset();
    WalletBatch{wallet.GetDatabase()}.EraseOrchardRescan();
}

std::vector<Txid> ShieldedConflicts(const CWallet& wallet, const CTransaction& tx)
{
    AssertLockHeld(wallet.cs_wallet);
    std::vector<Txid> out;
    if (wallet.m_shielded.spends.empty() || !zside::HasShieldedMarker(tx)) return out;
    std::string error;
    const auto bundle{zside::Bundle::FromTx(tx, error)};
    if (!bundle) return out;
    for (const uint256& nf : bundle->Nullifiers()) {
        const auto range{wallet.m_shielded.spends.equal_range(nf)};
        for (auto it{range.first}; it != range.second; ++it) {
            if (it->second != tx.GetHash() && std::find(out.begin(), out.end(), it->second) == out.end()) out.push_back(it->second);
        }
    }
    return out;
}

std::optional<OrchardAddress> ShieldedAddress(const CWallet& wallet, uint64_t index, bool internal)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!wallet.m_shielded.fvk) return std::nullopt;
    OrchardAddress address;
    if (zo_address(wallet.m_shielded.fvk->data(), index, internal, address.data()) != ZO_OK) return std::nullopt;
    return address;
}

void ShieldedTxAdded(CWallet& wallet, const CWalletTx& wtx)
{
    AssertLockHeld(wallet.cs_wallet);
    const CTransaction& tx{*wtx.GetTx()};
    if (!zside::HasShieldedMarker(tx)) return;
    std::string error;
    const auto bundle{zside::Bundle::FromTx(tx, error)};
    if (!bundle) return;
    ShieldedAccount& account{wallet.m_shielded};
    const Txid txid{tx.GetHash()};
    for (const uint256& nf : bundle->Nullifiers()) {
        const auto range{account.spends.equal_range(nf)};
        if (std::none_of(range.first, range.second, [&](const auto& e) { return e.second == txid; })) account.spends.emplace(nf, txid);
    }
    const auto keys{ShieldedViewingKeys(account)};
    for (uint32_t i{0}; i < bundle->NumActions(); ++i) {
        for (const auto& [key, legacy] : keys) {
            const uint8_t* fvk{key->data()};
            ShieldedNote note;
            if (!zo_bundle_decrypt(bundle->get(), i, fvk, note.note.data(), note.memo.data(), &note.internal)) continue;
            note.legacy = legacy;
            note.value = static_cast<CAmount>(zo_note_value(note.note.data()));
            zo_note_nullifier(note.note.data(), fvk, note.nullifier.data());
            zo_bundle_cmx(bundle->get(), i, note.cmx.data());
            // The same note in another transaction (its bundle again): one note, carried by both.
            const uint256 nullifier{note.nullifier};
            auto& carriers{account.notes.try_emplace(nullifier, std::move(note)).first->second.carriers};
            const std::pair<Txid, uint32_t> carrier{txid, i};
            if (std::find(carriers.begin(), carriers.end(), carrier) == carriers.end()) carriers.push_back(carrier);
            break;
        }
        // What the wallet paid, to others or itself (recovered with its outgoing key).
        for (const auto& [key, legacy] : keys) {
            ShieldedPayment payment;
            OrchardNoteBytes sent;
            if (!zo_bundle_recover(bundle->get(), i, key->data(), sent.data(), payment.memo.data())) continue;
            payment.txid = txid;
            payment.action = i;
            std::copy(sent.begin(), sent.begin() + zside::ADDRESS_SIZE, payment.recipient.begin());
            payment.value = static_cast<CAmount>(zo_note_value(sent.data()));
            account.payments.emplace(std::make_pair(txid, i), payment);
            break;
        }
    }
}

bool IsShieldedMine(const CWallet& wallet, const CTransaction& tx)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!wallet.m_shielded.fvk || !zside::HasShieldedMarker(tx)) return false;
    std::string error;
    const auto bundle{zside::Bundle::FromTx(tx, error)};
    if (!bundle) return false;
    for (const uint256& nf : bundle->Nullifiers()) {
        if (wallet.m_shielded.notes.contains(nf)) return true;
    }
    for (const auto& [key, legacy] : ShieldedViewingKeys(wallet.m_shielded)) {
        const uint8_t* fvk{key->data()};
        for (size_t i{0}; i < bundle->NumActions(); ++i) {
            OrchardNoteBytes note;
            OrchardMemo memo;
            bool internal;
            if (zo_bundle_decrypt(bundle->get(), i, fvk, note.data(), memo.data(), &internal)) return true;
            if (zo_bundle_recover(bundle->get(), i, fvk, note.data(), memo.data())) return true;
        }
    }
    return false;
}

std::vector<NoteStatus> ShieldedNotes(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    std::vector<NoteStatus> out;
    for (const auto& [nf, note] : wallet.m_shielded.notes) {
        // A transaction that was abandoned, or lost to a conflict (in a block, or replaced in the
        // mempool), does not carry it: another one may (the one a block took).
        std::optional<NoteStatus> best;
        for (const auto& [txid, action] : note.carriers) {
            const auto it{wallet.mapWallet.find(txid)};
            if (it == wallet.mapWallet.end() || it->second.isAbandoned()) continue;
            const int depth{wallet.GetTxDepthInMainChain(it->second)};
            if (depth < 0 || (depth == 0 && it->second.isMempoolConflicted())) continue;
            if (best && best->confirmations >= depth) continue;
            best.emplace();
            best->txid = txid;
            best->action = action;
            best->confirmations = depth;
        }
        if (!best) continue;
        NoteStatus status{*best};
        status.note = &note;
        const auto range{wallet.m_shielded.spends.equal_range(nf)};
        for (auto spend{range.first}; spend != range.second; ++spend) {
            const auto stx{wallet.mapWallet.find(spend->second)};
            if (stx == wallet.mapWallet.end() || stx->second.isAbandoned()) continue;
            const int depth{wallet.GetTxDepthInMainChain(stx->second)};
            if (depth < 0 || (depth == 0 && stx->second.isMempoolConflicted())) continue;
            status.spent_by = spend->second;
            break;
        }
        out.push_back(status);
    }
    return out;
}

ShieldedBalance GetShieldedBalance(const CWallet& wallet)
{
    ShieldedBalance balance;
    for (const NoteStatus& s : ShieldedNotes(wallet)) {
        if (s.spent_by) continue;
        ++balance.notes;
        (s.confirmations > 0 ? balance.available : balance.pending) += s.note->value;
    }
    return balance;
}

size_t ActionCount(size_t spends, size_t outputs)
{
    return std::max<size_t>({spends, outputs, 2});
}

size_t BundleSize(size_t actions)
{
    const size_t proof{2720 + 2272 * actions};
    const auto compact{[](size_t n) -> size_t { return n < 0xfd ? 1 : n <= 0xffff ? 3 : 5; }};
    return compact(actions) + actions * 820 + 1 + 8 + 32 + compact(proof) + proof + actions * 64 + 64;
}

std::optional<size_t> MaxShieldedSpends(const std::vector<CTxOut>& transparent, size_t shielded_outputs)
{
    const auto fits{[&](size_t n_spends) {
        std::vector<CTxOut> outputs{transparent};
        outputs.emplace_back(0, PlaceholderMarker(BundleSize(ActionCount(n_spends, shielded_outputs))));
        return ShieldedTxSize(outputs) * WITNESS_SCALE_FACTOR <= static_cast<size_t>(MAX_STANDARD_TX_WEIGHT);
    }};
    if (!fits(0)) return std::nullopt;
    size_t n{0};
    while (n < zside::MAX_ACTIONS && fits(n + 1)) ++n;
    return n;
}

namespace {
struct UnsignedFree {
    void operator()(ZoUnsigned* bundle) const { zo_unsigned_free(bundle); }
};
using UnsignedBundle = std::unique_ptr<ZoUnsigned, UnsignedFree>;

/**
 * A payment out of, or into, the shielded account, made in three steps so that proving (seconds)
 * holds no lock: under the wallet's lock the notes and coins are picked and reserved, and the bundle
 * built (Prepare*); without it the bundle is proved and signed; under the lock again what it spends
 * is checked still unspent, and it is committed (ProveAndCommit).
 */
struct PendingPayment {
    CMutableTransaction mtx;
    //! Its marker, a placeholder of the bundle's size until the bundle is proved.
    uint32_t marker_index{0};
    UnsignedBundle bundle;
    uint256 sighash;
    //! The notes it spends (by nullifier), reserved meanwhile (ShieldedAccount::in_flight).
    std::vector<uint256> nullifiers;
    //! The wallet's coins it spends; those it locked meanwhile (not those locked before).
    std::vector<COutPoint> coins;
    std::vector<COutPoint> locked;
    //! Its transparent inputs sign once the bundle is in (they sign it too).
    bool sign{false};
    std::optional<Txid> replaces;
};
using Prepared = std::variant<PendingPayment, std::string>;

/** The sighash the bundle of `p` signs; false if the bundle cannot commit to its effects. */
bool SetSighash(const CWallet& wallet, PendingPayment& p) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    uint256 commitment;
    if (zo_unsigned_commitment(p.bundle.get(), commitment.data()) != ZO_OK) return false;
    // Signed for the next block of the node's chain (zside/shielded.h): the one the mempool checks it
    // for. The wallet may not have seen the node's tip yet, and the domain changes at audit2_height.
    const std::optional<int> tip{wallet.chain().getHeight()};
    const int height{(tip ? *tip : wallet.GetLastBlockHeight()) + 1};
    p.sighash = zside::ShieldedSighash(CTransaction{p.mtx}, p.marker_index, commitment, zside::SighashDomainAt(Params().GetConsensus(), height));
    return true;
}

/** Hold what `p` spends from other payments while it is proved: its notes, and its coins (not locked yet). */
void Reserve(CWallet& wallet, PendingPayment& p) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    for (const uint256& nf : p.nullifiers) wallet.m_shielded.in_flight.insert(nf);
    for (const COutPoint& coin : p.coins) {
        if (wallet.IsLockedCoin(coin)) continue;
        wallet.LockCoin(coin, /*persist=*/false);
        p.locked.push_back(coin);
    }
}

/** Whether a transaction of the wallet may still be mined: not abandoned, nor conflicted. */
bool Active(const CWallet& wallet, const Txid& txid) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    const CWalletTx* wtx{wallet.GetWalletTx(txid)};
    if (!wtx || wtx->isAbandoned()) return false;
    const int depth{wallet.GetTxDepthInMainChain(*wtx)};
    return depth > 0 || (depth == 0 && !wtx->isMempoolConflicted());
}

/** Commit a proved payment, if what it spends is still unspent. Its reservations are released first. */
std::variant<CTransactionRef, std::string> Commit(CWallet& wallet, PendingPayment& p, int proved) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    for (const uint256& nf : p.nullifiers) wallet.m_shielded.in_flight.erase(nf);
    for (const COutPoint& coin : p.locked) wallet.UnlockCoin(coin);
    if (proved != ZO_OK) return strprintf("Cannot prove and sign the bundle (error %d)", proved);
    // While it was proved, the wallet may have taken in a block or a transaction that spends the same:
    // a payment made meanwhile without the wallet's reservations (another wallet with the same keys,
    // or the one it replaces mined).
    if (p.replaces) {
        const CWalletTx* old{wallet.GetWalletTx(*p.replaces)};
        if (!old || old->m_replaced_by_txid || old->isAbandoned() || wallet.GetTxDepthInMainChain(*old) != 0 || old->isMempoolConflicted()) {
            return std::string{"The transaction it replaces is no longer waiting for a block"};
        }
    }
    for (const uint256& nf : p.nullifiers) {
        const auto range{wallet.m_shielded.spends.equal_range(nf)};
        for (auto it{range.first}; it != range.second; ++it) {
            if (it->second != p.replaces && Active(wallet, it->second)) return std::string{"A note it spends was spent meanwhile (try again)"};
        }
    }
    for (const COutPoint& coin : p.coins) {
        if (wallet.IsSpent(coin)) return std::string{"A coin it spends was spent meanwhile (try again)"};
    }
    // The transparent inputs sign last, the bundle included.
    if (p.sign && !wallet.SignTransaction(p.mtx)) return std::string{"Cannot sign the transaction (is the wallet unlocked?)"};
    const CTransactionRef tx{MakeTransactionRef(std::move(p.mtx))};
    wallet.CommitTransaction(tx, p.replaces);
    return tx;
}

/** What a payment committed under the wallet's lock comes to once the lock is released (RefusedCommit). */
std::variant<CTransactionRef, std::string> Settle(CWallet& wallet, std::variant<CTransactionRef, std::string> result) EXCLUSIVE_LOCKS_REQUIRED(!wallet.cs_wallet)
{
    if (const auto* tx{std::get_if<CTransactionRef>(&result)}) {
        if (auto error{RefusedCommit(wallet, *tx)}) return std::move(*error);
    }
    return result;
}

/** Prove and sign a prepared payment without the wallet's lock, then commit it under the lock (and Settle). */
std::variant<CTransactionRef, std::string> ProveAndCommit(CWallet& wallet, Prepared prepared) EXCLUSIVE_LOCKS_REQUIRED(!wallet.cs_wallet)
{
    if (auto* error{std::get_if<std::string>(&prepared)}) return std::move(*error);
    PendingPayment& p{std::get<PendingPayment>(prepared)};
    uint8_t* encoded{nullptr};
    size_t len{0};
    // Consumes the bundle, proved or not.
    const int proved{zo_unsigned_finish(p.bundle.release(), p.sighash.data(), &encoded, &len)};
    if (proved == ZO_OK) {
        p.mtx.vout[p.marker_index].scriptPubKey = zside::ShieldedMarkerScript(std::span{encoded, len});
        zo_buffer_free(encoded, len);
    }
    std::variant<CTransactionRef, std::string> result;
    {
        LOCK(wallet.cs_wallet);
        result = Commit(wallet, p, proved);
    }
    return Settle(wallet, std::move(result));
}
} // namespace

std::optional<std::string> RefusedCommit(CWallet& wallet, const CTransactionRef& tx)
{
    AssertLockNotHeld(wallet.cs_wallet);
    // A wallet that does not broadcast (-walletbroadcast=0) never hands it to the mempool: it is
    // kept for the user to send, not abandoned for not being there.
    if (!wallet.GetBroadcastTransactions()) return std::nullopt;
    // A block may have taken it from the mempool since the commit: it went, and is kept.
    if (!CommittedTransactionRefused(wallet, tx)) return std::nullopt;
    wallet.AbandonTransaction(tx->GetHash());
    return "The transaction was not accepted into the mempool (see the debug log for why)";
}

namespace {
/**
 * SendShielded, spending the notes of `spend_first` (by nullifier) before any other: those of a
 * transaction this one replaces, which it so conflicts with.
 * @param[in] replaced_fee  the fee of the transaction it replaces: it pays that, and the incremental
 *                          relay fee of its own size (BIP 125 rules 3 and 4), at least
 */
Prepared PrepareSend(CWallet& wallet, const std::vector<ShieldedRecipient>& recipients, std::optional<CAmount> fixed_fee, bool subtract_fee,
                     const std::set<uint256>& spend_first, std::optional<Txid> replaces, std::optional<CAmount> replaced_fee = std::nullopt) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!SetupShieldedAccount(wallet)) return std::string{"The wallet is locked, or has no private keys"};
    const ShieldedAccount& account{wallet.m_shielded};
    const auto sk{ShieldedSpendingKey(wallet, account.key_version)};
    // The key of version 1 too, for the notes paid to it (ShieldedAccount::legacy_fvk).
    const auto legacy_sk{account.legacy_fvk ? ShieldedSpendingKey(wallet, KEY_ANY_CHAIN) : std::nullopt};
    if (!sk || (account.legacy_fvk && !legacy_sk)) return std::string{"The wallet is locked"};
    const uint8_t* fvk{account.fvk->data()};
    // The keys come from the wallet's active descriptor: if another was made active since the account
    // was set up, they are not the account's, and could spend none of its notes.
    const auto matches{[](const ShieldedKey& key, const OrchardFvk& expected) {
        OrchardFvk derived;
        const bool same{zo_full_viewing_key(key.data(), derived.data()) == ZO_OK && derived == expected};
        memory_cleanse(derived.data(), derived.size());
        return same;
    }};
    if (!matches(*sk, *account.fvk) || (account.legacy_fvk && !matches(*legacy_sk, *account.legacy_fvk))) {
        return std::string{"The active descriptor of the wallet is not the one the shielded account was made from: make that one active again to spend the account's notes"};
    }

    CAmount to_pay{0};
    size_t shielded_outputs{0};
    std::vector<CTxOut> transparent;
    for (const ShieldedRecipient& r : recipients) {
        if (r.amount <= 0 || !MoneyRange(r.amount)) return std::string{"Invalid amount"};
        to_pay += r.amount;
        if (!MoneyRange(to_pay)) return std::string{"Invalid amount"};
        if (r.shielded) {
            ++shielded_outputs;
        } else {
            transparent.emplace_back(r.amount, r.script);
        }
    }
    if (subtract_fee && (transparent.size() != 1 || recipients.size() != 1)) return std::string{"The fee can be taken out of the amount of one transparent recipient alone"};
    // What the notes must cover: the amounts, and the fee unless it comes out of the amount.
    const auto needed{[&](CAmount fee) { return subtract_fee ? to_pay : to_pay + fee; }};

    // The notes to spend: those it must first; then in blocks, not spent, those deep enough to be
    // spent at an anchor a reorg of the last blocks leaves (ANCHOR_CONFIRMATIONS) first, largest first,
    // until they pay for it all and the fee. Notes a payment being proved spends are left to it.
    struct Candidate {
        const ShieldedNote* note;
        int confirmations;
    };
    std::vector<Candidate> available, spends;
    for (const NoteStatus& s : ShieldedNotes(wallet)) {
        const bool in_flight{wallet.m_shielded.in_flight.contains(s.note->nullifier)};
        if (spend_first.contains(s.note->nullifier)) {
            if (s.confirmations <= 0) return std::string{"A note it spends is not in a block"};
            if (in_flight) return std::string{"Another payment that spends its notes is being made"};
            spends.push_back({s.note, s.confirmations});
        } else if (!s.spent_by && !in_flight && s.confirmations > 0 && s.note->value > 0) {
            available.push_back({s.note, s.confirmations});
        }
    }
    if (spends.size() != spend_first.size()) return std::string{"The notes it spends are not all the wallet's"};
    std::sort(available.begin(), available.end(), [](const Candidate& a, const Candidate& b) {
        const bool deep_a{a.confirmations >= ANCHOR_CONFIRMATIONS}, deep_b{b.confirmations >= ANCHOR_CONFIRMATIONS};
        if (deep_a != deep_b) return deep_a;
        return a.note->value > b.note->value;
    });
    const MinimumFeeRateResult fee_rate{GetMinimumFeeRate(wallet, CCoinControl{})};
    // Its size with so many notes (and change): its virtual size, without transparent inputs.
    const auto size_for{[&](size_t n_spends) {
        std::vector<CTxOut> outputs{transparent};
        outputs.emplace_back(0, PlaceholderMarker(BundleSize(ActionCount(n_spends, shielded_outputs + 1))));
        return ShieldedTxSize(outputs);
    }};
    // A replacement pays what the one it replaces paid, and the relay of its own size: of the new
    // transaction, which is larger if it spends more notes.
    const auto lowest_for{[&](size_t n_spends) -> CAmount {
        return *replaced_fee + wallet.chain().relayIncrementalFee().GetFee(size_for(n_spends));
    }};
    const auto fee_for{[&](size_t n_spends) -> CAmount {
        if (fixed_fee) return *fixed_fee;
        const CAmount fee{GetMinimumFee(fee_rate, size_for(n_spends))};
        return replaced_fee ? std::max(fee, lowest_for(n_spends)) : fee;
    }};
    // Each note spent is an action of about 3 kB: the transaction must stay of a size the network
    // relays, which caps the notes one payment spends.
    const std::optional<size_t> max_spends{MaxShieldedSpends(transparent, shielded_outputs + 1)};
    const auto fits{[&](size_t n_spends) { return max_spends && n_spends <= *max_spends; }};
    if (!fits(spends.size())) return std::string{"Too many recipients, or notes to spend, for one transaction the network relays"};
    CAmount spent{0};
    for (const Candidate& c : spends) spent += c.note->value;
    bool capped{false};
    for (const Candidate& c : available) {
        if (spent >= needed(fee_for(spends.size()))) break;
        if (!fits(spends.size() + 1)) {
            capped = true;
            break;
        }
        spends.push_back(c);
        spent += c.note->value;
    }
    const CAmount fee{fee_for(spends.size())};
    if (replaced_fee) {
        if (fee < lowest_for(spends.size())) {
            return strprintf("The new fee must be at least %s (the old fee, %s, and what a replacement of its size adds)", FormatMoney(lowest_for(spends.size())), FormatMoney(*replaced_fee));
        }
        if (!MoneyRange(fee) || fee > wallet.m_default_max_tx_fee) return strprintf("The fee of %s is more than the wallet pays (-maxtxfee)", FormatMoney(fee));
    }
    if (spent < needed(fee) && capped) {
        return strprintf("It would take more than %u notes, too many for one transaction the network relays: %s at most can be paid at once now. "
                         "Pay less, or join the notes first (pay part of the balance to the account's own address)", spends.size(), FormatMoney(std::max<CAmount>(0, subtract_fee ? spent : spent - fee)));
    }
    if (spent < needed(fee)) {
        const ShieldedBalance balance{GetShieldedBalance(wallet)};
        if (balance.pending > 0 && balance.available < needed(fee)) {
            return strprintf("Not enough shielded funds in blocks yet: %s available, %s waiting for a block; %s needed with the fee", FormatMoney(balance.available), FormatMoney(balance.pending), FormatMoney(needed(fee)));
        }
        if (!wallet.m_shielded.in_flight.empty()) {
            return strprintf("Not enough shielded funds: %s available, part of it held by a payment being made; %s needed with the fee", FormatMoney(balance.available), FormatMoney(needed(fee)));
        }
        return strprintf("Not enough shielded funds: %s available, %s needed with the fee", FormatMoney(balance.available), FormatMoney(needed(fee)));
    }
    if (subtract_fee) {
        if (transparent[0].nValue <= fee) return strprintf("The amount does not pay the fee of %s", FormatMoney(fee));
        transparent[0].nValue -= fee;
    }
    const CAmount change{spent - needed(fee)};

    // Where the notes are in the tree, all against the same root: the tree as the block with
    // ANCHOR_CONFIRMATIONS left it, or the block of the newest note if it has fewer. An anchor at the
    // tip would go with a reorg of one block, and the spend with it, for nothing.
    int depth{ANCHOR_CONFIRMATIONS};
    for (const Candidate& c : spends) depth = std::min(depth, c.confirmations);
    const int anchor_height{wallet.GetLastBlockHeight() - depth + 1};
    std::vector<uint256> cmxs;
    for (const Candidate& c : spends) cmxs.push_back(c.note->cmx);
    auto witnesses{wallet.chain().getOrchardWitnesses(cmxs, anchor_height)};
    // The node may be at another tip than the wallet for a moment: the tree of its tip then.
    if (!witnesses) witnesses = wallet.chain().getOrchardWitnesses(cmxs, std::nullopt);
    if (!witnesses) return std::string{"The node cannot find the notes in its tree yet: its -orchardindex is off, or not caught up with the last blocks (try again in a moment)"};

    ZoBuilder* builder{zo_builder_new(witnesses->anchor.data())};
    if (!builder) return std::string{"Cannot start a bundle"};
    for (size_t i{0}; i < spends.size(); ++i) {
        std::vector<uint8_t> path;
        for (const uint256& node : witnesses->paths[i].path) path.insert(path.end(), node.begin(), node.end());
        const ShieldedKey& key{spends[i].note->legacy ? *legacy_sk : *sk};
        const int result{zo_builder_add_spend(builder, key.data(), spends[i].note->note.data(), static_cast<uint32_t>(witnesses->paths[i].position), path.data())};
        if (result != ZO_OK) {
            zo_builder_free(builder);
            return strprintf("Cannot spend a note (error %d)", result);
        }
    }
    for (const ShieldedRecipient& r : recipients) {
        if (r.shielded && zo_builder_add_output(builder, fvk, r.shielded->data(), static_cast<uint64_t>(r.amount), r.memo.data()) != ZO_OK) {
            zo_builder_free(builder);
            return std::string{"Cannot pay a shielded address"};
        }
    }
    if (change > 0) {
        const auto change_address{ShieldedAddress(wallet, 0, /*internal=*/true)};
        if (zo_builder_add_output(builder, fvk, change_address->data(), static_cast<uint64_t>(change), MemoFromText("")->data()) != ZO_OK) {
            zo_builder_free(builder);
            return std::string{"Cannot make the change"};
        }
    }
    PendingPayment p;
    p.bundle.reset(zo_builder_build(builder));
    if (!p.bundle) return std::string{"Cannot build the bundle"};
    // What comes out of the pool pays the transparent outputs and the fee, exactly.
    const CAmount out_of_pool{zo_unsigned_value_balance(p.bundle.get())};
    CAmount transparent_out{0};
    for (const CTxOut& out : transparent) transparent_out += out.nValue;
    if (out_of_pool != transparent_out + fee) return std::string{"The bundle does not balance"};

    p.mtx.version = CTransaction::CURRENT_VERSION;
    p.mtx.vout = transparent;
    p.mtx.vout.emplace_back(0, PlaceholderMarker(BundleSize(zo_unsigned_num_actions(p.bundle.get()))));
    p.marker_index = p.mtx.vout.size() - 1;
    if (!SetSighash(wallet, p)) return std::string{"Cannot commit to the bundle"};
    for (const Candidate& c : spends) p.nullifiers.push_back(c.note->nullifier);
    p.replaces = replaces;
    Reserve(wallet, p);
    return p;
}
} // namespace

std::variant<CTransactionRef, std::string> SendShielded(CWallet& wallet, const std::vector<ShieldedRecipient>& recipients, std::optional<CAmount> fixed_fee, bool subtract_fee)
{
    // The index caught up with the tip first, the wallet's lock not held: a notification in the queue
    // may wait for that lock (getOrchardWitnesses does not wait, under it).
    AssertLockNotHeld(wallet.cs_wallet);
    wallet.chain().waitForOrchardIndex();
    Prepared prepared{WITH_LOCK(wallet.cs_wallet, return PrepareSend(wallet, recipients, fixed_fee, subtract_fee, /*spend_first=*/{}, /*replaces=*/std::nullopt))};
    return ProveAndCommit(wallet, std::move(prepared));
}

namespace {
Prepared PrepareBump(CWallet& wallet, const Txid& txid, std::optional<CAmount> new_fee) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!SetupShieldedAccount(wallet)) return std::string{"The wallet is locked, or has no private keys"};
    const CWalletTx* wtx{wallet.GetWalletTx(txid)};
    if (!wtx) return std::string{"The wallet has no such transaction"};
    if (wtx->m_replaced_by_txid) return strprintf("It was replaced already, by %s", wtx->m_replaced_by_txid->GetHex());
    if (wtx->isAbandoned() || wallet.GetTxDepthInMainChain(*wtx) != 0 || wtx->isMempoolConflicted()) return std::string{"It is not waiting for a block"};
    const CTransaction& tx{*wtx->GetTx()};
    // One of the shielded account alone: a transaction with transparent inputs is bumped with bumpfee.
    if (!tx.vin.empty()) return std::string{"It has transparent inputs: not one of the shielded account alone (see bumpfee)"};
    std::string error;
    const auto bundle{zside::Bundle::FromTx(tx, error)};
    if (!bundle) return std::string{"It has no shielded bundle"};

    // The notes it spends: the same are spent again, so that the new one replaces it.
    std::set<uint256> notes;
    for (const uint256& nf : bundle->Nullifiers()) {
        if (wallet.m_shielded.notes.contains(nf)) notes.insert(nf);
    }
    if (notes.empty()) return std::string{"It spends no note of the account"};

    // Whom it paid: its transparent outputs, and the shielded payments it made (its change aside).
    std::vector<ShieldedRecipient> recipients;
    CAmount transparent_out{0};
    for (uint32_t i{0}; i < tx.vout.size(); ++i) {
        if (i == bundle->marker_index) continue;
        ShieldedRecipient r;
        r.script = tx.vout[i].scriptPubKey;
        r.amount = tx.vout[i].nValue;
        transparent_out += r.amount;
        recipients.push_back(std::move(r));
    }
    for (auto it{wallet.m_shielded.payments.lower_bound({txid, 0})}; it != wallet.m_shielded.payments.end() && it->first.first == txid; ++it) {
        const ShieldedPayment& payment{it->second};
        // Change is to an internal address of the account.
        if (ShieldedAddressScope(wallet.m_shielded, payment.recipient) == 2) continue;
        ShieldedRecipient r;
        r.shielded = payment.recipient;
        r.amount = payment.value;
        r.memo = payment.memo;
        recipients.push_back(std::move(r));
    }
    if (recipients.empty()) return std::string{"It pays no one but the account's change"};

    // The fee it paid. The new one pays at least what the mempool takes as a replacement (BIP 125
    // rules 3 and 4), at the size of the new transaction: larger than the old one if the fee takes
    // more notes. By default, that or the wallet's fee rate, whichever is more.
    const CAmount old_fee{bundle->ValueBalance() - transparent_out};
    return PrepareSend(wallet, recipients, new_fee, /*subtract_fee=*/false, notes, txid, old_fee);
}
} // namespace

std::variant<CTransactionRef, std::string> BumpShieldedFee(CWallet& wallet, const Txid& txid, std::optional<CAmount> new_fee)
{
    AssertLockNotHeld(wallet.cs_wallet);
    wallet.chain().waitForOrchardIndex();
    Prepared prepared{WITH_LOCK(wallet.cs_wallet, return PrepareBump(wallet, txid, new_fee))};
    auto result{ProveAndCommit(wallet, std::move(prepared))};
    // Replaced once the new one went (to the mempool, or a block): one refused replaces nothing.
    if (const auto* bumped{std::get_if<CTransactionRef>(&result)}) {
        LOCK(wallet.cs_wallet);
        wallet.MarkReplaced(txid, (*bumped)->GetHash());
    }
    return result;
}

namespace {
Prepared PrepareShieldCoins(CWallet& wallet, CAmount amount, const OrchardAddress& to, const OrchardMemo& memo) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    if (!SetupShieldedAccount(wallet)) return std::string{"The wallet is locked, or has no private keys"};
    if (amount <= 0 || !MoneyRange(amount)) return std::string{"Invalid amount"};
    // A bundle that spends nothing: its anchor is the empty tree.
    const uint256 empty{zside::EmptyRoot(zside::TREE_DEPTH)};
    ZoBuilder* builder{zo_builder_new(empty.data())};
    if (!builder) return std::string{"Cannot start a bundle"};
    if (zo_builder_add_output(builder, wallet.m_shielded.fvk->data(), to.data(), static_cast<uint64_t>(amount), memo.data()) != ZO_OK) {
        zo_builder_free(builder);
        return std::string{"Cannot pay the shielded address"};
    }
    PendingPayment p;
    p.bundle.reset(zo_builder_build(builder));
    if (!p.bundle) return std::string{"Cannot build the bundle"};

    // The wallet funds the marker, which carries what goes into the pool, like any payment; the
    // placeholder has the size of the bundle, so the fee is right. Coins a payment being proved
    // spends are locked meanwhile: not picked again.
    const CScript placeholder{PlaceholderMarker(BundleSize(zo_unsigned_num_actions(p.bundle.get())))};
    CCoinControl coin_control;
    auto created{CreateTransaction(wallet, {{CNoDestination{placeholder}, amount, /*fSubtractFeeFromAmount=*/false}}, /*change_pos=*/std::nullopt, coin_control, /*sign=*/false)};
    if (!created) return util::ErrorString(created).original;
    p.mtx = CMutableTransaction{*created->tx};
    while (p.marker_index < p.mtx.vout.size() && p.mtx.vout[p.marker_index].scriptPubKey != placeholder) ++p.marker_index;
    if (p.marker_index == p.mtx.vout.size()) return std::string{"The marker went missing"};
    if (!SetSighash(wallet, p)) return std::string{"Cannot commit to the bundle"};
    for (const CTxIn& in : p.mtx.vin) p.coins.push_back(in.prevout);
    p.sign = true;
    Reserve(wallet, p);
    return p;
}

Prepared PrepareShieldCoin(CWallet& wallet, const COutPoint& coin, const OrchardAddress& to) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    if (wallet.IsLocked()) return std::string{"The wallet is locked"};
    if (!SetupShieldedAccount(wallet)) return std::string{"The wallet has no private keys"};
    const CWalletTx* wtx{wallet.GetWalletTx(coin.hash)};
    if (!wtx || coin.n >= wtx->GetTx()->vout.size()) return std::string{"The wallet does not have this coin"};
    if (wallet.IsSpent(coin)) return std::string{"The coin is spent"};
    const CAmount value{wtx->GetTx()->vout[coin.n].nValue};

    // The fee first: the coin alone pays the marker, the fee taken out of it. One output makes a
    // bundle of two actions, whose size the placeholder has.
    const CScript placeholder{PlaceholderMarker(BundleSize(ActionCount(0, 1)))};
    CCoinControl coin_control;
    coin_control.m_allow_other_inputs = false;
    coin_control.Select(coin);
    // A coin waiting to be melted is locked, so that nothing else spends it: this spends it.
    const bool was_locked{wallet.IsLockedCoin(coin)};
    if (was_locked) wallet.UnlockCoin(coin);
    auto created{CreateTransaction(wallet, {{CNoDestination{placeholder}, value, /*fSubtractFeeFromAmount=*/true}}, /*change_pos=*/std::nullopt, coin_control, /*sign=*/false)};
    if (was_locked) wallet.LockCoin(coin, /*persist=*/true);
    if (!created) return util::ErrorString(created).original;
    PendingPayment p;
    p.mtx = CMutableTransaction{*created->tx};
    if (p.mtx.vin.size() != 1 || p.mtx.vout.size() != 1 || p.mtx.vout[0].scriptPubKey != placeholder) return std::string{"The coin cannot be shielded alone (too small to pay its fee?)"};
    const CAmount into_pool{p.mtx.vout[0].nValue};

    const uint256 empty{zside::EmptyRoot(zside::TREE_DEPTH)};
    ZoBuilder* builder{zo_builder_new(empty.data())};
    if (!builder) return std::string{"Cannot start a bundle"};
    if (zo_builder_add_output(builder, wallet.m_shielded.fvk->data(), to.data(), static_cast<uint64_t>(into_pool), MemoFromText("")->data()) != ZO_OK) {
        zo_builder_free(builder);
        return std::string{"Cannot pay the shielded address"};
    }
    p.bundle.reset(zo_builder_build(builder));
    if (!p.bundle) return std::string{"Cannot build the bundle"};
    p.marker_index = 0;
    if (!SetSighash(wallet, p)) return std::string{"Cannot commit to the bundle"};
    p.coins.push_back(coin);
    p.sign = true;
    Reserve(wallet, p);
    return p;
}
} // namespace

std::variant<CTransactionRef, std::string> ShieldCoins(CWallet& wallet, CAmount amount, const OrchardAddress& to, const OrchardMemo& memo)
{
    AssertLockNotHeld(wallet.cs_wallet);
    Prepared prepared{WITH_LOCK(wallet.cs_wallet, return PrepareShieldCoins(wallet, amount, to, memo))};
    return ProveAndCommit(wallet, std::move(prepared));
}

std::variant<CTransactionRef, std::string> ShieldCoin(CWallet& wallet, const COutPoint& coin, const OrchardAddress& to)
{
    AssertLockNotHeld(wallet.cs_wallet);
    Prepared prepared{WITH_LOCK(wallet.cs_wallet, return PrepareShieldCoin(wallet, coin, to))};
    return ProveAndCommit(wallet, std::move(prepared));
}

} // namespace wallet
