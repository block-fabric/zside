// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <wallet/shielded.h>

#include <bech32.h>
#include <chainparams.h>
#include <hash.h>
#include <key_io.h>
#include <util/check.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <wallet/coincontrol.h>
#include <wallet/fees.h>
#include <wallet/scriptpubkeyman.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>
#include <wallet/walletdb.h>
#include <zside/shielded.h>

#include <algorithm>
#include <cstring>

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

std::optional<std::array<uint8_t, 32>> ShieldedSpendingKey(const CWallet& wallet)
{
    const auto* spkm{dynamic_cast<const DescriptorScriptPubKeyMan*>(wallet.GetScriptPubKeyMan(OutputType::BECH32, /*internal=*/false))};
    if (!spkm) return std::nullopt;
    std::string descriptor;
    if (!spkm->GetDescriptorString(descriptor, /*priv=*/true)) return std::nullopt;
    // The first extended private key of the descriptor (the account key, which is restored with it).
    for (const std::string prefix : {"xprv", "tprv"}) {
        const size_t at{descriptor.find(prefix)};
        if (at == std::string::npos) continue;
        size_t end{at};
        while (end < descriptor.size() && std::string_view{"123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"}.find(descriptor[end]) != std::string_view::npos) ++end;
        const CExtKey extkey{DecodeExtKey(descriptor.substr(at, end - at))};
        if (!extkey.key.IsValid()) return std::nullopt;
        unsigned char code[BIP32_EXTKEY_SIZE];
        extkey.Encode(code);
        HashWriter seed{TaggedHash("zSide/Orchard seed")};
        seed.write(std::as_bytes(std::span{code}));
        const uint256 seed_hash{seed.GetSHA256()};
        std::array<uint8_t, 32> sk;
        if (zo_spending_key_from_seed(seed_hash.data(), 32, COIN_TYPE, 0, sk.data()) != ZO_OK) return std::nullopt;
        return sk;
    }
    return std::nullopt;
}

bool SetupShieldedAccount(CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    if (wallet.m_shielded.fvk) return true;
    const auto sk{ShieldedSpendingKey(wallet)};
    if (!sk) return false;
    OrchardFvk fvk;
    if (zo_full_viewing_key(sk->data(), fvk.data()) != ZO_OK) return false;
    WalletBatch batch{wallet.GetDatabase()};
    batch.WriteOrchardViewingKey(std::vector<unsigned char>(fvk.begin(), fvk.end()));
    wallet.m_shielded.fvk = fvk;
    // What the wallet keeps already may pay the account.
    for (const auto& [txid, wtx] : wallet.mapWallet) ShieldedTxAdded(wallet, wtx);
    return true;
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
    if (!account.fvk) return;
    const uint8_t* fvk{account.fvk->data()};
    for (uint32_t i{0}; i < bundle->NumActions(); ++i) {
        ShieldedNote note;
        if (zo_bundle_decrypt(bundle->get(), i, fvk, note.note.data(), note.memo.data(), &note.internal)) {
            note.txid = txid;
            note.action = i;
            note.value = static_cast<CAmount>(zo_note_value(note.note.data()));
            zo_note_nullifier(note.note.data(), fvk, note.nullifier.data());
            zo_bundle_cmx(bundle->get(), i, note.cmx.data());
            account.notes.emplace(note.nullifier, note);
        }
        // What the wallet paid, to others or itself (recovered with its outgoing key).
        ShieldedPayment payment;
        OrchardNoteBytes sent;
        if (zo_bundle_recover(bundle->get(), i, fvk, sent.data(), payment.memo.data())) {
            payment.txid = txid;
            payment.action = i;
            std::copy(sent.begin(), sent.begin() + zside::ADDRESS_SIZE, payment.recipient.begin());
            payment.value = static_cast<CAmount>(zo_note_value(sent.data()));
            account.payments.emplace(std::make_pair(txid, i), payment);
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
    const uint8_t* fvk{wallet.m_shielded.fvk->data()};
    for (size_t i{0}; i < bundle->NumActions(); ++i) {
        OrchardNoteBytes note;
        OrchardMemo memo;
        bool internal;
        if (zo_bundle_decrypt(bundle->get(), i, fvk, note.data(), memo.data(), &internal)) return true;
        if (zo_bundle_recover(bundle->get(), i, fvk, note.data(), memo.data())) return true;
    }
    return false;
}

std::vector<NoteStatus> ShieldedNotes(const CWallet& wallet)
{
    AssertLockHeld(wallet.cs_wallet);
    std::vector<NoteStatus> out;
    for (const auto& [nf, note] : wallet.m_shielded.notes) {
        const auto it{wallet.mapWallet.find(note.txid)};
        if (it == wallet.mapWallet.end()) continue;
        NoteStatus status;
        status.note = &note;
        status.confirmations = wallet.GetTxDepthInMainChain(it->second);
        // A note whose transaction was abandoned, or lost to a conflict, is not there.
        if (it->second.isAbandoned() || status.confirmations < 0) continue;
        const auto range{wallet.m_shielded.spends.equal_range(nf)};
        for (auto spend{range.first}; spend != range.second; ++spend) {
            const auto stx{wallet.mapWallet.find(spend->second)};
            if (stx == wallet.mapWallet.end() || stx->second.isAbandoned() || wallet.GetTxDepthInMainChain(stx->second) < 0) continue;
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

namespace {
/** Prove and sign a built bundle for a transaction whose marker (at `marker_index`) is a placeholder of its size, and put it in. */
std::optional<std::string> FinishBundle(CMutableTransaction& mtx, uint32_t marker_index, ZoUnsigned* unsigned_bundle)
{
    uint256 commitment;
    if (zo_unsigned_commitment(unsigned_bundle, commitment.data()) != ZO_OK) {
        zo_unsigned_free(unsigned_bundle);
        return "Cannot commit to the bundle";
    }
    const uint256 sighash{zside::ShieldedSighash(CTransaction{mtx}, marker_index, commitment)};
    uint8_t* encoded{nullptr};
    size_t len{0};
    const int result{zo_unsigned_finish(unsigned_bundle, sighash.data(), &encoded, &len)};
    if (result != ZO_OK) return strprintf("Cannot prove and sign the bundle (error %d)", result);
    mtx.vout[marker_index].scriptPubKey = zside::ShieldedMarkerScript(std::span{encoded, len});
    zo_buffer_free(encoded, len);
    return std::nullopt;
}

std::optional<std::string> Commit(CWallet& wallet, const CTransactionRef& tx)
{
    wallet.CommitTransaction(tx);
    if (!wallet.chain().isInMempool(tx->GetHash())) {
        wallet.AbandonTransaction(tx->GetHash());
        return "The transaction was not accepted into the mempool (see the debug log for why)";
    }
    return std::nullopt;
}
} // namespace

std::variant<CTransactionRef, std::string> SendShielded(CWallet& wallet, const std::vector<ShieldedRecipient>& recipients, std::optional<CAmount> fixed_fee, bool subtract_fee)
{
    LOCK(wallet.cs_wallet);
    if (!SetupShieldedAccount(wallet)) return std::string{"The wallet is locked, or has no private keys"};
    const auto sk{ShieldedSpendingKey(wallet)};
    if (!sk) return std::string{"The wallet is locked"};
    const uint8_t* fvk{wallet.m_shielded.fvk->data()};

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

    // The notes to spend: in blocks, not spent, largest first, until they pay for it all and the fee.
    std::vector<const ShieldedNote*> available;
    for (const NoteStatus& s : ShieldedNotes(wallet)) {
        if (!s.spent_by && s.confirmations > 0 && s.note->value > 0) available.push_back(s.note);
    }
    std::sort(available.begin(), available.end(), [](const auto* a, const auto* b) { return a->value > b->value; });
    const MinimumFeeRateResult fee_rate{GetMinimumFeeRate(wallet, CCoinControl{})};
    const auto fee_for{[&](size_t spends) -> CAmount {
        if (fixed_fee) return *fixed_fee;
        std::vector<CTxOut> outputs{transparent};
        outputs.emplace_back(0, PlaceholderMarker(BundleSize(ActionCount(spends, shielded_outputs + 1))));
        return GetMinimumFee(fee_rate, ShieldedTxSize(outputs));
    }};
    std::vector<const ShieldedNote*> spends;
    CAmount spent{0};
    for (const ShieldedNote* note : available) {
        if (spent >= needed(fee_for(spends.size()))) break;
        spends.push_back(note);
        spent += note->value;
    }
    const CAmount fee{fee_for(spends.size())};
    if (spent < needed(fee)) {
        const ShieldedBalance balance{GetShieldedBalance(wallet)};
        if (balance.pending > 0 && balance.available < needed(fee)) {
            return strprintf("Not enough shielded funds in blocks yet: %s available, %s waiting for a block; %s needed with the fee", FormatMoney(balance.available), FormatMoney(balance.pending), FormatMoney(needed(fee)));
        }
        return strprintf("Not enough shielded funds: %s available, %s needed with the fee", FormatMoney(balance.available), FormatMoney(needed(fee)));
    }
    if (subtract_fee) {
        if (transparent[0].nValue <= fee) return strprintf("The amount does not pay the fee of %s", FormatMoney(fee));
        transparent[0].nValue -= fee;
    }
    const CAmount change{spent - needed(fee)};

    // Where the notes are in the tree, all against the same root.
    std::vector<uint256> cmxs;
    for (const ShieldedNote* note : spends) cmxs.push_back(note->cmx);
    const auto witnesses{wallet.chain().getOrchardWitnesses(cmxs)};
    if (!witnesses) return std::string{"The node cannot find the notes in its tree yet (is -orchardindex on, and synced?)"};

    ZoBuilder* builder{zo_builder_new(witnesses->anchor.data())};
    if (!builder) return std::string{"Cannot start a bundle"};
    for (size_t i{0}; i < spends.size(); ++i) {
        std::vector<uint8_t> path;
        for (const uint256& node : witnesses->paths[i].path) path.insert(path.end(), node.begin(), node.end());
        const int result{zo_builder_add_spend(builder, sk->data(), spends[i]->note.data(), static_cast<uint32_t>(witnesses->paths[i].position), path.data())};
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
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    if (!unsigned_bundle) return std::string{"Cannot build the bundle"};
    // What comes out of the pool pays the transparent outputs and the fee, exactly.
    const CAmount out_of_pool{zo_unsigned_value_balance(unsigned_bundle)};
    CAmount transparent_out{0};
    for (const CTxOut& out : transparent) transparent_out += out.nValue;
    if (out_of_pool != transparent_out + fee) {
        zo_unsigned_free(unsigned_bundle);
        return std::string{"The bundle does not balance"};
    }

    CMutableTransaction mtx;
    mtx.version = CTransaction::CURRENT_VERSION;
    mtx.vout = transparent;
    mtx.vout.emplace_back(0, PlaceholderMarker(BundleSize(zo_unsigned_num_actions(unsigned_bundle))));
    if (const auto error{FinishBundle(mtx, mtx.vout.size() - 1, unsigned_bundle)}) return *error;
    const CTransactionRef tx{MakeTransactionRef(std::move(mtx))};
    if (const auto error{Commit(wallet, tx)}) return *error;
    return tx;
}

std::variant<CTransactionRef, std::string> ShieldCoins(CWallet& wallet, CAmount amount, const OrchardAddress& to, const OrchardMemo& memo)
{
    LOCK(wallet.cs_wallet);
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
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    if (!unsigned_bundle) return std::string{"Cannot build the bundle"};

    // The wallet funds the marker, which carries what goes into the pool, like any payment; the
    // placeholder has the size of the bundle, so the fee is right.
    const CScript placeholder{PlaceholderMarker(BundleSize(zo_unsigned_num_actions(unsigned_bundle)))};
    CCoinControl coin_control;
    auto created{CreateTransaction(wallet, {{CNoDestination{placeholder}, amount, /*fSubtractFeeFromAmount=*/false}}, /*change_pos=*/std::nullopt, coin_control, /*sign=*/false)};
    if (!created) {
        zo_unsigned_free(unsigned_bundle);
        return util::ErrorString(created).original;
    }
    CMutableTransaction mtx{*created->tx};
    uint32_t marker_index{0};
    while (marker_index < mtx.vout.size() && mtx.vout[marker_index].scriptPubKey != placeholder) ++marker_index;
    if (marker_index == mtx.vout.size()) {
        zo_unsigned_free(unsigned_bundle);
        return std::string{"The marker went missing"};
    }
    if (const auto error{FinishBundle(mtx, marker_index, unsigned_bundle)}) return *error;
    // The transparent inputs sign last, the bundle included.
    if (!wallet.SignTransaction(mtx)) return std::string{"Cannot sign the transaction (is the wallet unlocked?)"};
    const CTransactionRef tx{MakeTransactionRef(std::move(mtx))};
    if (const auto error{Commit(wallet, tx)}) return *error;
    return tx;
}

std::variant<CTransactionRef, std::string> ShieldCoin(CWallet& wallet, const COutPoint& coin, const OrchardAddress& to)
{
    LOCK(wallet.cs_wallet);
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
    CMutableTransaction mtx{*created->tx};
    if (mtx.vin.size() != 1 || mtx.vout.size() != 1 || mtx.vout[0].scriptPubKey != placeholder) return std::string{"The coin cannot be shielded alone (too small to pay its fee?)"};
    const CAmount into_pool{mtx.vout[0].nValue};

    const uint256 empty{zside::EmptyRoot(zside::TREE_DEPTH)};
    ZoBuilder* builder{zo_builder_new(empty.data())};
    if (!builder) return std::string{"Cannot start a bundle"};
    if (zo_builder_add_output(builder, wallet.m_shielded.fvk->data(), to.data(), static_cast<uint64_t>(into_pool), MemoFromText("")->data()) != ZO_OK) {
        zo_builder_free(builder);
        return std::string{"Cannot pay the shielded address"};
    }
    ZoUnsigned* unsigned_bundle{zo_builder_build(builder)};
    if (!unsigned_bundle) return std::string{"Cannot build the bundle"};
    if (const auto error{FinishBundle(mtx, 0, unsigned_bundle)}) return *error;
    if (!wallet.SignTransaction(mtx)) return std::string{"Cannot sign the transaction (is the wallet unlocked?)"};
    const CTransactionRef tx{MakeTransactionRef(std::move(mtx))};
    if (const auto error{Commit(wallet, tx)}) return *error;
    return tx;
}

} // namespace wallet
