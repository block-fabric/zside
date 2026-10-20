// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/walletapi.h>
#include <webui/chainconfig.h>
#include <webui/local.h>

#include <addresstype.h>
#include <chainparams.h>
#include <coins.h>
#include <common/args.h>
#include <common/messages.h>
#include <common/signmessage.h>
#include <common/types.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <core_io.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <node/context.h>
#include <node/psbt.h>
#include <node/transaction.h>
#include <node/types.h>
#include <outputtype.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <psbt.h>
#include <pubkey.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <support/allocators/secure.h>
#include <support/cleanse.h>
#include <tinyformat.h>
#include <util/fees.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>
#include <wallet/receive.h>
#include <wallet/spend.h>
#include <wallet/types.h>
#include <wallet/wallet.h>
#include <wallet/walletutil.h>
#include <webui/bridge.h>
#include <webui/confirm.h>
#include <webui/guard.h>
#include <webui/rpcpolicy.h>

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <utility>

using wallet::AddressPurpose;
using wallet::CCoinControl;

namespace webui {

// --- The receive requests of chains-qt (recentrequeststablemodel.h, sendcoinsrecipient.h) ---

std::string EncodeReceiveRequest(const ReceiveRequest& r)
{
    DataStream ss{};
    // RecentRequestEntry: nVersion, id, date (as unsigned int seconds), then SendCoinsRecipient.
    ss << r.nVersion << r.id << static_cast<unsigned int>(r.time);
    ss << r.recipientVersion << r.address << r.label << r.amount << r.message << r.payment_request << r.authenticated_merchant;
    return ss.str();
}

std::optional<ReceiveRequest> DecodeReceiveRequest(const std::string& data)
{
    try {
        SpanReader ss{MakeByteSpan(data)};
        ReceiveRequest r;
        unsigned int time;
        ss >> r.nVersion >> r.id >> time;
        r.time = time;
        ss >> r.recipientVersion >> r.address >> r.label >> r.amount >> r.message >> r.payment_request >> r.authenticated_merchant;
        return r;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

namespace {

// --- Params ---

const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string Str(const UniValue& params, const std::string& key)
{
    const UniValue& v{Param(params, key)};
    if (!v.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return v.get_str();
}

std::string OptStr(const UniValue& params, const std::string& key, const std::string& fallback = "")
{
    const UniValue& v{Param(params, key)};
    return v.isStr() ? v.get_str() : fallback;
}

bool Bool(const UniValue& params, const std::string& key, bool fallback = false)
{
    const UniValue& v{Param(params, key)};
    return v.isBool() ? v.get_bool() : fallback;
}

int64_t Int(const UniValue& params, const std::string& key)
{
    const UniValue& v{Param(params, key)};
    if (!v.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a number is needed", key));
    return v.getInt<int64_t>();
}

std::optional<int64_t> OptInt(const UniValue& params, const std::string& key)
{
    const UniValue& v{Param(params, key)};
    if (v.isNull()) return std::nullopt;
    if (!v.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a number is needed", key));
    return v.getInt<int64_t>();
}

Txid TxidParam(const UniValue& params, const std::string& key = "txid")
{
    const auto txid{Txid::FromHex(Str(params, key))};
    if (!txid) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a transaction id is needed", key));
    return *txid;
}

SecureString Secure(const std::string& text)
{
    SecureString out;
    out.reserve(text.size());
    out.assign(text);
    return out;
}

/**
 * A passphrase of the params, straight into locked, wiped memory: no copy of it in a std::string.
 * The params themselves are wiped by the bridge after the call (CleanseSecrets).
 */
SecureString SecretParam(const UniValue& params, const std::string& key, bool required = true)
{
    const UniValue& v{Param(params, key)};
    if (v.isNull() && !required) return {};
    if (!v.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return Secure(v.get_str());
}

UniValue Warnings(const std::vector<bilingual_str>& warnings)
{
    UniValue out{UniValue::VARR};
    for (const auto& w : warnings) out.push_back(w.translated);
    return out;
}

std::string PurposeName(AddressPurpose purpose)
{
    switch (purpose) {
    case AddressPurpose::RECEIVE: return "receive";
    case AddressPurpose::SEND: return "send";
    case AddressPurpose::REFUND: return "refund";
    }
    return "unknown";
}

std::string FormatCoins(CAmount amount)
{
    return FormatMoney(amount) + " " + CURRENCY_UNIT;
}

std::string EncodePsbt(const PartiallySignedTransaction& psbtx)
{
    DataStream ss{};
    ss << psbtx;
    return EncodeBase64(ss.str());
}

PartiallySignedTransaction DecodePsbtParam(const UniValue& params)
{
    auto psbt{DecodeBase64PSBT(Str(params, "psbt"))};
    if (!psbt) throw HostError(strprintf("Unable to decode PSBT\n%s", util::ErrorString(psbt).original));
    return *psbt;
}

UniValue OutpointJson(const COutPoint& outpoint)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("txid", outpoint.hash.GetHex());
    out.pushKV("vout", static_cast<int64_t>(outpoint.n));
    return out;
}

COutPoint OutpointParam(const UniValue& v)
{
    if (!v.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "an outpoint is {txid, vout}");
    const auto txid{Txid::FromHex(Str(v, "txid"))};
    if (!txid) throw HostError(BRIDGE_INVALID_REQUEST, "txid: a transaction id is needed");
    const int64_t n{Int(v, "vout")};
    if (n < 0 || n > std::numeric_limits<uint32_t>::max()) throw HostError(BRIDGE_INVALID_REQUEST, "vout: out of range");
    return COutPoint{*txid, static_cast<uint32_t>(n)};
}

std::vector<COutPoint> OutpointsParam(const UniValue& params, const std::string& key)
{
    std::vector<COutPoint> out;
    const UniValue& list{Param(params, key)};
    if (list.isNull()) return out;
    if (!list.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a list of outpoints is needed", key));
    for (const UniValue& v : list.getValues()) out.push_back(OutpointParam(v));
    return out;
}

/** The coin control of the send page: selected inputs, a custom change address, the fee (as SendCoinsDialog::updateCoinControlState). */
CCoinControl MakeCoinControl(const UniValue& params)
{
    CCoinControl cc;
    for (const COutPoint& o : OutpointsParam(params, "selected")) cc.Select(o);
    if (const std::string change{OptStr(params, "changeAddress")}; !change.empty()) {
        const CTxDestination dest{DecodeDestination(change)};
        if (!IsValidDestination(dest)) throw HostError(WalletApi::SEND_REFUSED, strprintf("Warning: Invalid %s address (custom change address)", CLIENT_NAME));
        cc.destChange = dest;
    }
    if (const auto fee_rate{OptInt(params, "feeRate")}) {
        if (*fee_rate < 0) throw HostError(BRIDGE_INVALID_REQUEST, "feeRate: not negative");
        cc.m_feerate = CFeeRate(*fee_rate);
    }
    if (const auto target{OptInt(params, "confTarget")}) cc.m_confirm_target = static_cast<unsigned int>(std::max<int64_t>(1, *target));
    return cc;
}

/** A name of a wallet the page gives (audit 7 #14): one of the wallets folder, never a path out of it. */
std::string WalletName(const std::string& name)
{
    if (name.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "Enter a name for the wallet");
    if (name.size() > 200 || name == "." || name == ".." || name.find_first_of("/\\:") != std::string::npos || name.front() == ' ' || name.back() == ' ') {
        throw HostError(BRIDGE_INVALID_REQUEST, "A wallet name is a name, not a path: no '/', '\\', ':', and not \".\" or \"..\".");
    }
    size_t pos{0};
    while (pos < name.size()) {
        const auto cp{guard::NextCodePoint(name, pos)};
        if (!cp || guard::IsControlOrFormat(*cp)) throw HostError(BRIDGE_INVALID_REQUEST, "A wallet name has no control or invisible characters.");
    }
    return name;
}

// --- Transactions, as TransactionRecord and TransactionTableModel ---

enum class RecordType { Other, Generated, SendToAddress, SendToOther, RecvWithAddress, RecvFromOther };

const char* TypeName(RecordType type)
{
    switch (type) {
    case RecordType::Other: return "other";
    case RecordType::Generated: return "mined";
    case RecordType::SendToAddress: return "send";
    case RecordType::SendToOther: return "sendOther";
    case RecordType::RecvWithAddress: return "recv";
    case RecordType::RecvFromOther: return "recvOther";
    }
    return "other";
}

struct Record {
    Txid hash;
    int64_t time{0};
    RecordType type{RecordType::Other};
    std::string address;
    CAmount debit{0};
    CAmount credit{0};
    int idx{0};
    //! What the chain says the output pays, for an output of a coinbase (local::CoinbaseRoles); "" else.
    std::string role;
};

//! Number of confirmations recommended for accepting a transaction (TransactionRecord::RecommendedNumConfirmations).
constexpr int RECOMMENDED_CONFIRMATIONS{6};
//! The most addresses one wallet.owned call looks up (those of a page of the explorer).
constexpr size_t MAX_OWNED_ASKED{500};

/** TransactionRecord::decomposeTransaction. */
std::vector<Record> Decompose(const interfaces::WalletTx& wtx, const std::vector<std::string>& roles = {})
{
    std::vector<Record> parts;
    const int64_t nTime{wtx.time};
    const CAmount nCredit{wtx.credit};
    const CAmount nDebit{wtx.debit};
    const CAmount nNet{nCredit - nDebit};
    const Txid hash{wtx.tx->GetHash()};

    bool all_from_me{true};
    bool any_from_me{false};
    if (wtx.is_coinbase) {
        all_from_me = false;
    } else {
        for (const bool mine : wtx.txin_is_mine) {
            all_from_me = all_from_me && mine;
            if (mine) any_from_me = true;
        }
    }

    if (all_from_me || !any_from_me) {
        CAmount nTxFee{nDebit - wtx.tx->GetValueOut()};
        for (unsigned int i = 0; i < wtx.tx->vout.size(); i++) {
            const CTxOut& txout{wtx.tx->vout[i]};
            if (all_from_me) {
                // Change is only really possible if we're the sender.
                if (wtx.txout_is_change[i]) continue;
                Record sub{.hash = hash, .time = nTime};
                sub.idx = i;
                if (!std::get_if<CNoDestination>(&wtx.txout_address[i])) {
                    sub.type = RecordType::SendToAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                } else {
                    sub.type = RecordType::SendToOther;
                    sub.address = wtx.comment_to.value_or("");
                }
                CAmount nValue{txout.nValue};
                // Add fee to first output.
                if (nTxFee > 0) {
                    nValue += nTxFee;
                    nTxFee = 0;
                }
                sub.debit = -nValue;
                parts.push_back(sub);
            }
            if (wtx.txout_is_mine[i]) {
                Record sub{.hash = hash, .time = nTime};
                sub.idx = i;
                sub.credit = txout.nValue;
                if (wtx.txout_address_is_mine[i]) {
                    sub.type = RecordType::RecvWithAddress;
                    sub.address = EncodeDestination(wtx.txout_address[i]);
                } else {
                    sub.type = RecordType::RecvFromOther;
                    sub.address = wtx.from.value_or("");
                }
                if (wtx.is_coinbase) sub.type = RecordType::Generated;
                if (wtx.is_coinbase && i < roles.size()) sub.role = roles[i];
                parts.push_back(sub);
            }
        }
    } else {
        // Mixed debit transaction, can't break down payees.
        Record sub{.hash = hash, .time = nTime};
        sub.type = RecordType::Other;
        sub.debit = nNet;
        parts.push_back(sub);
    }
    return parts;
}

struct RecordStatus {
    std::string status;
    std::string sort_key;
    bool counts_for_balance{false};
    int64_t depth{0};
    int matures_in{0};
};

/** TransactionRecord::updateStatus. */
RecordStatus StatusOf(const Record& rec, const interfaces::WalletTxStatus& wtx)
{
    RecordStatus s;
    int typesort;
    switch (rec.type) {
    case RecordType::SendToAddress:
    case RecordType::SendToOther: typesort = 2; break;
    case RecordType::RecvWithAddress:
    case RecordType::RecvFromOther: typesort = 3; break;
    default: typesort = 9;
    }
    s.sort_key = strprintf("%010d-%01d-%010u-%03d-%d", wtx.block_height, wtx.is_coinbase ? 1 : 0, wtx.time_received, rec.idx, typesort);
    s.counts_for_balance = wtx.is_trusted && !(wtx.blocks_to_maturity > 0);
    s.depth = wtx.depth_in_main_chain;
    if (rec.type == RecordType::Generated) {
        if (wtx.blocks_to_maturity > 0) {
            s.status = "immature";
            if (wtx.is_in_main_chain) {
                s.matures_in = wtx.blocks_to_maturity;
            } else {
                s.status = "notaccepted";
            }
        } else {
            s.status = "confirmed";
        }
    } else if (s.depth < 0) {
        s.status = "conflicted";
    } else if (s.depth == 0) {
        s.status = wtx.is_abandoned ? "abandoned" : "unconfirmed";
    } else if (s.depth < RECOMMENDED_CONFIRMATIONS) {
        s.status = "confirming";
    } else {
        s.status = "confirmed";
    }
    return s;
}

/** As the private MakeWalletTx of the wallet interface, under the lock already held. */
interfaces::WalletTx MakeTx(const wallet::CWallet& w, const wallet::CWalletTx& wtx) EXCLUSIVE_LOCKS_REQUIRED(w.cs_wallet)
{
    interfaces::WalletTx result;
    result.tx = wtx.GetTx();
    for (const auto& txin : result.tx->vin) result.txin_is_mine.emplace_back(wallet::InputIsMine(w, txin));
    for (const auto& txout : result.tx->vout) {
        result.txout_is_mine.emplace_back(w.IsMine(txout));
        result.txout_is_change.push_back(wallet::OutputIsChange(w, txout));
        result.txout_address.emplace_back();
        result.txout_address_is_mine.emplace_back(ExtractDestination(txout.scriptPubKey, result.txout_address.back()) ? w.IsMine(result.txout_address.back()) : false);
    }
    result.credit = wallet::CachedTxGetCredit(w, wtx, /*avoid_reuse=*/true);
    result.debit = wallet::CachedTxGetDebit(w, wtx, /*avoid_reuse=*/true);
    result.change = wallet::CachedTxGetChange(w, wtx);
    result.time = wtx.GetTxTime();
    result.from = wtx.m_from;
    result.message = wtx.m_message;
    result.comment = wtx.m_comment;
    result.comment_to = wtx.m_comment_to;
    result.is_coinbase = wtx.IsCoinBase();
    return result;
}

/** As the private MakeWalletTxStatus of the wallet interface. */
interfaces::WalletTxStatus MakeStatus(const wallet::CWallet& w, const wallet::CWalletTx& wtx, std::set<Txid>& trusted_parents) EXCLUSIVE_LOCKS_REQUIRED(w.cs_wallet)
{
    interfaces::WalletTxStatus result;
    result.block_height = wtx.state<wallet::TxStateConfirmed>() ? wtx.state<wallet::TxStateConfirmed>()->confirmed_block_height :
                          wtx.state<wallet::TxStateBlockConflicted>() ? wtx.state<wallet::TxStateBlockConflicted>()->conflicting_block_height :
                                                                         std::numeric_limits<int>::max();
    result.blocks_to_maturity = w.GetTxBlocksToMaturity(wtx);
    result.depth_in_main_chain = w.GetTxDepthInMainChain(wtx);
    result.time_received = wtx.nTimeReceived;
    result.lock_time = wtx.GetTx()->nLockTime;
    result.is_trusted = wallet::CachedTxIsTrusted(w, wtx, trusted_parents);
    result.is_abandoned = wtx.isAbandoned();
    result.is_coinbase = wtx.IsCoinBase();
    result.is_in_main_chain = wtx.isConfirmed();
    return result;
}

std::map<std::string, std::string> LabelsOf(interfaces::Wallet& wallet)
{
    std::map<std::string, std::string> labels;
    for (const auto& a : wallet.getAddresses()) {
        if (!a.name.empty()) labels.emplace(EncodeDestination(a.dest), a.name);
    }
    return labels;
}

/** How the page brings the status of a record up to date by itself with each block (lib/wallet.ts recordStatus). */
const char* StateCode(const wallet::CWalletTx& wtx)
{
    if (wtx.state<wallet::TxStateConfirmed>()) return "c";
    if (wtx.state<wallet::TxStateBlockConflicted>()) return "x";
    return "u";
}

UniValue RecordJson(const Record& rec, const RecordStatus& s, const interfaces::WalletTxStatus& status, const char* state, const std::map<std::string, std::string>& labels)
{
    // An array per record, short as a long history is long: see the page (lib/wallet.ts) for the fields.
    UniValue r{UniValue::VARR};
    r.push_back(rec.hash.GetHex());
    r.push_back(rec.idx);
    r.push_back(TypeName(rec.type));
    r.push_back(rec.address);
    const auto label{labels.find(rec.address)};
    r.push_back(label == labels.end() ? "" : label->second);
    r.push_back(rec.time);
    r.push_back(rec.credit);
    r.push_back(rec.debit);
    r.push_back(s.status);
    r.push_back(s.depth);
    r.push_back(s.matures_in);
    r.push_back(s.counts_for_balance);
    r.push_back(s.sort_key);
    // What the status follows from: the height of the block (of the conflict), the state, and for a
    // transaction not in a block whether the wallet trusts it.
    r.push_back(status.block_height == std::numeric_limits<int>::max() ? -1 : status.block_height);
    r.push_back(state);
    r.push_back(status.is_coinbase);
    r.push_back(status.is_abandoned);
    r.push_back(status.is_trusted);
    // The role of an output of a coinbase, in the chain's words (local::CoinbaseRoles): "" mostly.
    r.push_back(rec.role);
    return r;
}

/** The records of a transaction of the wallet, under its lock. */
void AppendRecords(UniValue& records, const wallet::CWallet& w, const wallet::CWalletTx& wtx, std::set<Txid>& trusted_parents, const std::map<std::string, std::string>& labels) EXCLUSIVE_LOCKS_REQUIRED(w.cs_wallet)
{
    const interfaces::WalletTx tx{MakeTx(w, wtx)};
    const interfaces::WalletTxStatus status{MakeStatus(w, wtx, trusted_parents)};
    const char* state{StateCode(wtx)};
    std::vector<std::string> roles;
    if (const auto* confirmed{wtx.state<wallet::TxStateConfirmed>()}; confirmed && wtx.IsCoinBase()) roles = local::CoinbaseRoles(w.chain(), confirmed->confirmed_block_hash);
    for (const Record& rec : Decompose(tx, roles)) records.push_back(RecordJson(rec, StatusOf(rec, status), status, state, labels));
}

/** TransactionDesc::FormatTxStatus. */
std::string FormatTxStatus(const interfaces::WalletTxStatus& status, bool in_mempool)
{
    const int depth{status.depth_in_main_chain};
    if (depth < 0) return strprintf("conflicted with a transaction with %d confirmations", -depth);
    if (depth == 0) {
        std::string s{in_mempool ? "0/unconfirmed, in memory pool" : "0/unconfirmed, not in memory pool"};
        if (status.is_abandoned) s += ", abandoned";
        return s;
    }
    if (depth < 6) return strprintf("%d/unconfirmed", depth);
    return strprintf("%d confirmations", depth);
}

UniValue Row(const std::string& key)
{
    UniValue row{UniValue::VOBJ};
    row.pushKV("k", key);
    return row;
}

UniValue TextRow(const std::string& key, const std::string& text)
{
    UniValue row{Row(key)};
    row.pushKV("text", text);
    return row;
}

UniValue AmountRow(const std::string& key, CAmount sats, const std::string& note = "")
{
    UniValue row{Row(key)};
    row.pushKV("sats", sats);
    if (!note.empty()) row.pushKV("note", note);
    return row;
}

UniValue AddressRow(const std::string& key, const std::string& address, const std::string& label, bool own)
{
    UniValue row{Row(key)};
    row.pushKV("address", address);
    if (!label.empty()) row.pushKV("label", label);
    if (own) row.pushKV("own", true);
    return row;
}

std::string LabelOf(interfaces::Wallet& wallet, const CTxDestination& dest)
{
    std::string name;
    if (wallet.getAddress(dest, &name, /*purpose=*/nullptr)) return name;
    return "";
}

/** TransactionDesc::toHTML, as rows the page lays out. */
UniValue Describe(interfaces::Node& node, interfaces::Wallet& wallet, const Txid& txid, int idx, const std::string& rec_address)
{
    int num_blocks;
    interfaces::WalletTxStatus status;
    bool in_mempool;
    std::vector<std::string> messages;
    std::vector<std::string> payment_requests;
    const interfaces::WalletTx wtx{wallet.getWalletTxDetails(txid, status, messages, payment_requests, in_mempool, num_blocks)};
    if (!wtx.tx) throw HostError("No such transaction in this wallet");

    UniValue rows{UniValue::VARR};
    const int64_t nTime{wtx.time};
    const CAmount nCredit{wtx.credit};
    const CAmount nDebit{wtx.debit};
    const CAmount nNet{nCredit - nDebit};

    rows.push_back(TextRow("Status", FormatTxStatus(status, in_mempool)));
    {
        UniValue row{Row("Date")};
        row.pushKV("time", nTime);
        rows.push_back(std::move(row));
    }

    // From
    if (wtx.is_coinbase) {
        UniValue source{TextRow("Source", "Generated")};
        // What the output pays, in the chain's words (local::CoinbaseRoles): the page says it instead.
        if (wallet::CWallet* cw{wallet.wallet()}; cw && idx >= 0) {
            uint256 block;
            {
                LOCK(cw->cs_wallet);
                const auto it{cw->mapWallet.find(txid)};
                if (it != cw->mapWallet.end()) {
                    if (const auto* confirmed{it->second.state<wallet::TxStateConfirmed>()}) block = confirmed->confirmed_block_hash;
                }
            }
            if (!block.IsNull()) {
                const auto roles{local::CoinbaseRoles(cw->chain(), block)};
                if (static_cast<size_t>(idx) < roles.size() && !roles[idx].empty()) source.pushKV("role", roles[idx]);
            }
        }
        rows.push_back(std::move(source));
    } else if (wtx.from) {
        rows.push_back(TextRow("From", *wtx.from));
    } else if (nNet > 0) {
        const CTxDestination address{DecodeDestination(rec_address)};
        if (IsValidDestination(address)) {
            std::string name;
            if (wallet.getAddress(address, &name, /*purpose=*/nullptr)) {
                rows.push_back(TextRow("From", "unknown"));
                rows.push_back(AddressRow("To", rec_address, name, /*own=*/true));
            }
        }
    }

    // To
    if (wtx.comment_to) {
        rows.push_back(AddressRow("To", *wtx.comment_to, LabelOf(wallet, DecodeDestination(*wtx.comment_to)), false));
    }

    // Amount
    if (wtx.is_coinbase && nCredit == 0) {
        CAmount nUnmatured{0};
        for (const CTxOut& txout : wtx.tx->vout) nUnmatured += wallet.getCredit(txout);
        if (status.is_in_main_chain) {
            rows.push_back(AmountRow("Credit", nUnmatured, strprintf("matures in %d more block%s", status.blocks_to_maturity, status.blocks_to_maturity == 1 ? "" : "s")));
        } else {
            rows.push_back(TextRow("Credit", "(not accepted)"));
        }
    } else if (nNet > 0) {
        rows.push_back(AmountRow("Credit", nNet));
    } else {
        const bool all_from_me{std::all_of(wtx.txin_is_mine.begin(), wtx.txin_is_mine.end(), [](bool mine) { return mine; })};
        const bool all_to_me{std::all_of(wtx.txout_is_mine.begin(), wtx.txout_is_mine.end(), [](bool mine) { return mine; })};
        if (all_from_me) {
            auto mine{wtx.txout_is_mine.begin()};
            for (const CTxOut& txout : wtx.tx->vout) {
                const bool to_self{*(mine++)};
                // Ignore change.
                if (to_self && all_from_me) continue;
                if (!wtx.comment_to) {
                    CTxDestination address;
                    if (ExtractDestination(txout.scriptPubKey, address)) {
                        rows.push_back(AddressRow("To", EncodeDestination(address), LabelOf(wallet, address), to_self));
                    }
                }
                rows.push_back(AmountRow("Debit", -txout.nValue));
                if (to_self) rows.push_back(AmountRow("Credit", txout.nValue));
            }
            if (all_to_me) {
                // Payment to self.
                const CAmount nValue{nCredit - wtx.change};
                rows.push_back(AmountRow("Total debit", -nValue));
                rows.push_back(AmountRow("Total credit", nValue));
            }
            const CAmount nTxFee{nDebit - wtx.tx->GetValueOut()};
            if (nTxFee > 0) rows.push_back(AmountRow("Transaction fee", -nTxFee));
        } else {
            // Mixed debit transaction.
            auto mine{wtx.txin_is_mine.begin()};
            for (const CTxIn& txin : wtx.tx->vin) {
                if (*(mine++)) rows.push_back(AmountRow("Debit", -wallet.getDebit(txin)));
            }
            mine = wtx.txout_is_mine.begin();
            for (const CTxOut& txout : wtx.tx->vout) {
                if (*(mine++)) rows.push_back(AmountRow("Credit", wallet.getCredit(txout)));
            }
        }
    }
    {
        UniValue row{AmountRow("Net amount", nNet)};
        row.pushKV("plus", true);
        rows.push_back(std::move(row));
    }

    UniValue out{UniValue::VOBJ};
    out.pushKV("rows", std::move(rows));
    UniValue notes{UniValue::VARR};
    if (wtx.message) notes.push_back(TextRow("Message", *wtx.message));
    if (wtx.comment) notes.push_back(TextRow("Comment", *wtx.comment));
    out.pushKV("notes", std::move(notes));

    UniValue ids{UniValue::VARR};
    ids.push_back(TextRow("Transaction ID", txid.GetHex()));
    ids.push_back(TextRow("Transaction total size", strprintf("%u bytes", wtx.tx->ComputeTotalSize())));
    ids.push_back(TextRow("Transaction virtual size", strprintf("%d bytes", GetVirtualTransactionSize(*wtx.tx))));
    ids.push_back(TextRow("Output index", strprintf("%d", idx)));
    out.pushKV("ids", std::move(ids));

    UniValue msgs{UniValue::VARR};
    for (const std::string& m : messages) msgs.push_back(m);
    out.pushKV("messages", std::move(msgs));

    if (wtx.is_coinbase) {
        const int maturity{Params().GetConsensus().coinbase_maturity + 1};
        out.pushKV("coinbaseNote", strprintf("Generated coins must mature %d blocks before they can be spent. When you generated this block, it was broadcast to the network to be added to the block chain. If it fails to get into the chain, its state will change to \"not accepted\" and it won't be spendable. This may occasionally happen if another node generates a block within a few seconds of yours.", maturity));
    }

    // Debug view, when the node logs more than the default.
    if (node.getLogCategories() != BCLog::NONE) {
        UniValue debug{UniValue::VOBJ};
        UniValue amounts{UniValue::VARR};
        for (const CTxIn& txin : wtx.tx->vin) {
            if (wallet.txinIsMine(txin)) amounts.push_back(AmountRow("Debit", -wallet.getDebit(txin)));
        }
        for (const CTxOut& txout : wtx.tx->vout) {
            if (wallet.txoutIsMine(txout)) amounts.push_back(AmountRow("Credit", wallet.getCredit(txout)));
        }
        debug.pushKV("amounts", std::move(amounts));
        debug.pushKV("transaction", wtx.tx->ToString());
        UniValue inputs{UniValue::VARR};
        for (const CTxIn& txin : wtx.tx->vin) {
            if (auto prev{node.getUnspentOutput(txin.prevout)}) {
                UniValue input{UniValue::VOBJ};
                CTxDestination address;
                if (ExtractDestination(prev->out.scriptPubKey, address)) {
                    input.pushKV("address", EncodeDestination(address));
                    input.pushKV("label", LabelOf(wallet, address));
                }
                input.pushKV("sats", prev->out.nValue);
                input.pushKV("mine", wallet.txoutIsMine(prev->out));
                inputs.push_back(std::move(input));
            }
        }
        debug.pushKV("inputs", std::move(inputs));
        out.pushKV("debug", std::move(debug));
    }
    out.pushKV("txid", txid.GetHex());
    out.pushKV("coinbase", wtx.is_coinbase);
    return out;
}

// --- PSBTs, as PSBTOperationsDialog ---

size_t CouldSign(interfaces::Wallet* wallet, PartiallySignedTransaction psbtx)
{
    if (!wallet) return 0;
    size_t n_signed{0};
    bool complete;
    if (wallet->fillPSBT({.sign = false, .bip32_derivs = false}, &n_signed, psbtx, complete)) return 0;
    return n_signed;
}

/** What the PSBT dialog shows: the outputs, the fee, the status. */
UniValue PsbtState(interfaces::Wallet* wallet, const PartiallySignedTransaction& psbtx, bool complete)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("psbt", EncodePsbt(psbtx));
    out.pushKV("complete", complete);
    UniValue outputs{UniValue::VARR};
    CAmount total{0};
    for (const PSBTOutput& o : psbtx.outputs) {
        CTxDestination address;
        ExtractDestination(o.script, address);
        total += o.amount;
        UniValue output{UniValue::VOBJ};
        output.pushKV("address", IsValidDestination(address) ? EncodeDestination(address) : "");
        output.pushKV("sats", o.amount);
        output.pushKV("mine", wallet != nullptr && wallet->txoutIsMine(CTxOut(o.amount, o.script)));
        outputs.push_back(std::move(output));
    }
    out.pushKV("outputs", std::move(outputs));
    out.pushKV("total", total);
    const node::PSBTAnalysis analysis{node::AnalyzePSBT(psbtx)};
    if (analysis.fee) out.pushKV("fee", *analysis.fee);
    out.pushKV("unsignedInputs", static_cast<int64_t>(CountPSBTUnsignedInputs(psbtx)));
    out.pushKV("next", PSBTRoleName(analysis.next));

    // PSBTOperationsDialog::showTransactionStatus
    const size_t n_could_sign{CouldSign(wallet, psbtx)};
    std::string text;
    std::string level{"info"};
    switch (analysis.next) {
    case PSBTRole::UPDATER:
        text = "Transaction is missing some information about inputs.";
        level = "warning";
        break;
    case PSBTRole::SIGNER:
        text = "Transaction still needs signature(s).";
        if (!wallet) {
            text += " (But no wallet is loaded.)";
            level = "warning";
        } else if (wallet->privateKeysDisabled()) {
            text += " (But this wallet cannot sign transactions.)";
            level = "warning";
        } else if (n_could_sign < 1) {
            text += " (But this wallet does not have the right keys.)";
            level = "warning";
        }
        break;
    case PSBTRole::FINALIZER:
    case PSBTRole::EXTRACTOR:
        text = "Transaction is fully signed and ready for broadcast.";
        break;
    default:
        text = "Transaction status is unknown.";
        level = "error";
    }
    UniValue status{UniValue::VOBJ};
    status.pushKV("text", text);
    status.pushKV("level", level);
    out.pushKV("status", std::move(status));
    out.pushKV("canBroadcast", complete);
    return out;
}

/** A label of the page as a dialog of the host shows it: one line, cut (audit 7 #4). */
std::string ShownLabel(const std::string& label)
{
    return guard::OneLine(label, 100);
}

/** The rows of what an output pays: its destination first, whole; then its amount; then the label the page gave it. */
void OutputRows(std::vector<dialog::Row>& rows, interfaces::Wallet* wallet, const CTxOut& out, const std::string& label, const std::string& to = "Pay to")
{
    CTxDestination dest;
    if (out.scriptPubKey.IsUnspendable()) {
        rows.push_back(confirm::Destination("Data", ScriptToAsmStr(out.scriptPubKey)));
        if (out.nValue > 0) rows.push_back(confirm::Amount("Amount (burned)", out.nValue));
        return;
    }
    const bool mine{wallet && wallet->txoutIsMine(out)};
    if (ExtractDestination(out.scriptPubKey, dest)) {
        rows.push_back(confirm::Destination(mine ? "Back to this wallet" : to, EncodeDestination(dest)));
    } else {
        rows.push_back(confirm::Destination(mine ? "Back to this wallet (script)" : to + " (script)", HexStr(out.scriptPubKey)));
    }
    rows.push_back(confirm::Amount("Amount", out.nValue));
    if (!label.empty()) rows.push_back(confirm::Text("Label", "\u201c" + ShownLabel(label) + "\u201d"));
}

/**
 * The rows of a transaction the wallet made, from the transaction itself: each output (destination first, then amount, then
 * label), the fee, the total it pays. Only an output the wallet owns is its change, shown apart and not counted as paid: the
 * output the wallet put the change in (change_pos) to an address that is not the wallet's (a change address the page gave)
 * is a payment like any other, shown and counted (a hidden payment otherwise).
 */
std::vector<dialog::Row> PaymentRows(interfaces::Wallet& wallet, const CTransaction& tx, std::optional<unsigned int> change_pos, CAmount fee, const std::map<std::string, std::string>& labels)
{
    std::vector<dialog::Row> rows{confirm::WalletRows(wallet.getWalletName())};
    std::vector<size_t> paid;
    std::optional<size_t> change;
    for (size_t i{0}; i < tx.vout.size(); ++i) {
        if (change_pos && i == *change_pos && wallet.txoutIsMine(tx.vout[i])) change = i;
        else paid.push_back(i);
    }
    CAmount sum{0};
    for (size_t n{0}; n < paid.size(); ++n) {
        const CTxOut& out{tx.vout[paid[n]]};
        CTxDestination dest;
        const auto label{ExtractDestination(out.scriptPubKey, dest) ? labels.find(EncodeDestination(dest)) : labels.end()};
        // Of several, each named by its place on its destination's row (no row of its own: a part of Thunder's batch, 1500
        // recipients with their amounts and labels, fits in one question, dialog::MAX_ROWS).
        OutputRows(rows, &wallet, out, label == labels.end() ? std::string{} : label->second, paid.size() > 1 ? strprintf("Recipient %u of %u", n + 1, paid.size()) : "Pay to");
        sum += out.nValue;
    }
    rows.push_back(confirm::Amount("Network fee", fee));
    rows.push_back(confirm::Total("Total", sum + fee));
    if (change) {
        // The change, back to an address of this wallet: said, apart from what is paid.
        CTxDestination dest;
        rows.push_back(confirm::Destination("Change back to this wallet", ExtractDestination(tx.vout[*change].scriptPubKey, dest) ? EncodeDestination(dest) : HexStr(tx.vout[*change].scriptPubKey)));
        rows.push_back(confirm::Amount("Change", tx.vout[*change].nValue));
    }
    return rows;
}

/** The rows of a PSBT: what it spends of this wallet, each output (destination first), the fee when known, the id. */
std::vector<dialog::Row> PsbtRows(interfaces::Wallet* wallet, const PartiallySignedTransaction& psbtx)
{
    std::vector<dialog::Row> rows{wallet ? confirm::WalletRows(wallet->getWalletName()) : std::vector<dialog::Row>{confirm::Text("Network", confirm::NetworkName())}};
    CAmount mine_in{0};
    size_t mine_n{0}, unknown{0};
    for (const PSBTInput& input : psbtx.inputs) {
        CTxOut utxo;
        if (!input.GetUTXO(utxo)) {
            ++unknown;
            continue;
        }
        if (wallet && wallet->txoutIsMine(utxo)) {
            mine_in += utxo.nValue;
            ++mine_n;
        }
    }
    if (wallet) {
        if (mine_n) rows.push_back({"Spends of this wallet", strprintf("%s (%u of %u inputs)", confirm::Coins(mine_in), mine_n, psbtx.inputs.size()), dialog::Row::Kind::AMOUNT});
        else rows.push_back(confirm::Note("None of its inputs is a coin of this wallet that it knows."));
    }
    if (unknown) rows.push_back(confirm::Note(strprintf("%u of its inputs do not say what they spend.", unknown)));
    CAmount sum{0};
    for (size_t n{0}; n < psbtx.outputs.size(); ++n) {
        const PSBTOutput& o{psbtx.outputs[n]};
        if (psbtx.outputs.size() > 1) rows.push_back(confirm::Heading(strprintf("Output %u of %u", n + 1, psbtx.outputs.size())));
        OutputRows(rows, wallet, CTxOut(o.amount, o.script), {});
        sum += o.amount;
    }
    const node::PSBTAnalysis analysis{node::AnalyzePSBT(psbtx)};
    if (analysis.fee) {
        rows.push_back(confirm::Amount("Network fee", *analysis.fee));
        rows.push_back(confirm::Total("Total", sum + *analysis.fee));
    } else {
        rows.push_back(confirm::Total("Outputs in all", sum));
        rows.push_back(confirm::Note("The fee cannot be told: not every input says what it spends."));
    }
    if (const auto tx{psbtx.GetUnsignedTx()}) rows.push_back(confirm::Destination("Transaction", tx->GetHash().GetHex()));
    return rows;
}

} // namespace

struct WalletApi::Prepared {
    std::string wallet;
    //! Made, not signed: it is signed once the user accepted it on the desktop (wallet.send.commit).
    CTransactionRef tx;
    CAmount fee{0};
    std::optional<unsigned int> change_pos;
    struct Recipient {
        std::string address;
        std::string label;
        CAmount amount{0};
        std::string message;
    };
    std::vector<Recipient> recipients;
};

struct WalletApi::Bump {
    std::string wallet;
    Txid txid;
    CMutableTransaction mtx;
    CAmount old_fee{0};
    CAmount new_fee{0};
};

std::string WalletApi::ChosenFile(const UniValue& params, bool write) const
{
    // The page names a file only by what the user chose in a file chooser: page code (or content
    // shown in it) never decides where a wallet is written, nor which file is restored.
    const std::string file{Str(params, "file")};
    if (!m_may_use_file || !m_may_use_file(file, write)) {
        throw HostError(BRIDGE_INVALID_REQUEST, write ? "Only a file chosen by the user can be written" : "Only a file chosen by the user can be read");
    }
    return file;
}

interfaces::WalletLoader& WalletApi::Loader() const
{
    // A node without wallets (-disablewallet) has no wallet loader: the wallet methods answer with an
    // error then, they never reach the loader (Node::walletLoader asserts that there is one).
    interfaces::Node& node{m_node()};
    const node::NodeContext* context{node.context()};
    if (!context || !context->wallet_loader) throw HostError(WALLETS_DISABLED, "Wallets are disabled in this node (-disablewallet).");
    return node.walletLoader();
}

dialog::Answer WalletApi::Ask(const Window::ConfirmDialog& dialog) const
{
    // Every fact of a confirmation shown, none cut: more than a dialog shows is refused instead.
    if (dialog.rows.size() > dialog::MAX_ROWS) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("Too much for one question (%u facts; at most %u): make it in parts", dialog.rows.size(), dialog::MAX_ROWS));
    // Without a way to ask (a test that gives none), nothing is accepted.
    return m_ask ? m_ask(dialog) : dialog::Answer{};
}

std::unique_ptr<interfaces::Wallet> WalletApi::WalletNamed(const std::string& name) const
{
    for (auto& w : Loader().getWallets()) {
        if (w->getWalletName() == name) return std::move(w);
    }
    throw HostError(-18, strprintf("Requested wallet does not exist or is not loaded: %s", name));
}

UniValue WalletApi::WithKeys(const std::string& wallet_name, Window::ConfirmDialog dialog, bool always, bool fee_unknown, const std::function<UniValue()>& run)
{
    auto w{WalletNamed(wallet_name)};
    if (fee_unknown) {
        dialog.rows.push_back(confirm::Note(strprintf("Plus the network fee the wallet sets when it makes the transaction: at most %s.", confirm::Coins(w->getDefaultMaxTxFee()))));
    }
    // Wallets without private keys may look "encrypted" after an old bug: they need no unlocking.
    const bool locked{!w->privateKeysDisabled() && w->isCrypted() && w->isLocked()};
    if (!locked) {
        if (always && !Ask(dialog).accepted) throw HostError(BRIDGE_CANCELLED, strprintf("%s: declined.", dialog.title));
        return run();
    }
    // The passphrase is typed in the same dialog of the host, never in the page (audit 7 #1).
    dialog.passphrase = true;
    dialog.passphrase_label = strprintf("Passphrase of the wallet \"%s\"", guard::OneLine(wallet_name, 64));
    for (int attempt{0}; attempt < PASSPHRASE_TRIES; ++attempt) {
        dialog::Answer answer{Ask(dialog)};
        if (!answer.accepted) throw HostError(BRIDGE_CANCELLED, strprintf("%s: declined.", dialog.title));
        if (!w->isLocked()) return run();
        if (w->unlock(answer.passphrase)) {
            // Locked again as soon as the call is done, whatever happens in it.
            struct Relock {
                interfaces::Wallet& wallet;
                ~Relock() { wallet.lock(); }
            } relock{*w};
            return run();
        }
        dialog.error = answer.passphrase.find('\0') == SecureString::npos ?
            "The passphrase entered for the wallet decryption was incorrect. Try again." :
            "The passphrase entered for the wallet decryption is incorrect. It contains a null character (ie - a zero byte). If the passphrase was set with a "
            "version of this software prior to 25.0, please try again with only the characters up to — but not including — the first null character.";
    }
    throw HostError(PASSPHRASE_INCORRECT, "The passphrase entered for the wallet decryption was incorrect.");
}

std::optional<CAmount> WalletApi::FeeFor(const std::string& wallet_name, int64_t vsize)
{
    try {
        auto w{WalletNamed(wallet_name)};
        return w->getMinimumFee(static_cast<unsigned int>(std::clamp<int64_t>(vsize, 1, 1'000'000)), CCoinControl{}, /*returned_target=*/nullptr, /*reason=*/nullptr);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

WalletApi::WalletApi(NodeFn node, FileFn may_use_file, guard::AskFn ask) : m_node(std::move(node)), m_may_use_file(std::move(may_use_file)), m_ask(std::move(ask))
{
    // Find a loaded wallet by its name (the RPC methods of the page name wallets the same way).
    auto get_wallet{[this](const UniValue& params) -> std::unique_ptr<interfaces::Wallet> {
        const std::string name{Str(params, "wallet")};
        for (auto& w : Loader().getWallets()) {
            if (w->getWalletName() == name) return std::move(w);
        }
        throw HostError(-18, strprintf("Requested wallet does not exist or is not loaded: %s", name));
    }};
    auto find_wallet{[this](const UniValue& params) -> std::unique_ptr<interfaces::Wallet> {
        const UniValue name{Param(params, "wallet")};
        if (!name.isStr()) return nullptr;
        for (auto& w : Loader().getWallets()) {
            if (w->getWalletName() == name.get_str()) return std::move(w);
        }
        return nullptr;
    }};

    // --- Wallets of the node (WalletController) ---

    Add("wallets.list", [this](const UniValue&) {
        std::set<std::string> loaded;
        for (auto& w : Loader().getWallets()) loaded.insert(w->getWalletName());
        UniValue out{UniValue::VARR};
        for (const auto& [name, format] : Loader().listWalletDir()) {
            UniValue w{UniValue::VOBJ};
            w.pushKV("name", name);
            w.pushKV("format", format);
            w.pushKV("loaded", loaded.contains(name));
            w.pushKV("needsMigration", format == "bdb");
            out.push_back(std::move(w));
        }
        return out;
    });
    Add("wallets.signers", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        UniValue list{UniValue::VARR};
        try {
            for (const auto& signer : m_node().listExternalSigners()) {
                UniValue s{UniValue::VOBJ};
                s.pushKV("name", signer->getName());
                list.push_back(std::move(s));
            }
        } catch (const std::runtime_error& e) {
            out.pushKV("error", strprintf("Can't list signers: %s", e.what()));
        }
        if (list.size() > 1) {
            out.pushKV("error", "More than one external signer found. Please connect only one at a time.");
            list = UniValue{UniValue::VARR};
        }
        out.pushKV("signers", std::move(list));
#ifdef ENABLE_EXTERNAL_SIGNER
        out.pushKV("supported", true);
#else
        out.pushKV("supported", false);
#endif
        return out;
    });
    Add("wallets.create", [this](const UniValue& params) {
        const std::string name{WalletName(Str(params, "name"))};
        uint64_t flags{wallet::WALLET_FLAG_DESCRIPTORS};
        if (Bool(params, "disablePrivateKeys")) flags |= wallet::WALLET_FLAG_DISABLE_PRIVATE_KEYS;
        if (Bool(params, "blank")) flags |= wallet::WALLET_FLAG_BLANK_WALLET;
        if (Bool(params, "externalSigner")) flags |= wallet::WALLET_FLAG_EXTERNAL_SIGNER;
        const SecureString passphrase{SecretParam(params, "passphrase", /*required=*/false)};
        std::vector<bilingual_str> warnings;
        auto w{Loader().createWallet(name, passphrase, flags, warnings)};
        if (!w) throw HostError(util::ErrorString(w).translated);
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", (*w)->getWalletName());
        out.pushKV("warnings", Warnings(warnings));
        return out;
    });
    // A wallet of the wallets folder, by a name it lists: never a path the page makes up (audit 7 #14).
    auto listed_name{[this](const UniValue& params) {
        const std::string name{Str(params, "name")};
        for (const auto& [listed, format] : Loader().listWalletDir()) {
            if (listed == name) return name;
        }
        throw HostError(BRIDGE_INVALID_REQUEST, strprintf("There is no wallet \"%s\" in the wallets folder.", guard::OneLine(name, 64)));
    }};
    Add("wallets.open", [this, listed_name](const UniValue& params) {
        std::vector<bilingual_str> warnings;
        auto w{Loader().loadWallet(listed_name(params), warnings)};
        if (!w) throw HostError(util::ErrorString(w).translated);
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", (*w)->getWalletName());
        out.pushKV("warnings", Warnings(warnings));
        return out;
    });
    Add("wallets.close", [this](const UniValue& params) {
        const std::string name{Str(params, "name")};
        for (auto& w : Loader().getWallets()) {
            if (w->getWalletName() == name) {
                // As WalletController::removeWallet: the wallet goes, and is no longer loaded at start.
                w->remove();
                return UniValue{true};
            }
        }
        throw HostError(-18, strprintf("Requested wallet does not exist or is not loaded: %s", name));
    });
    Add("wallets.restore", [this](const UniValue& params) {
        const std::string name{WalletName(Str(params, "name"))};
        std::vector<bilingual_str> warnings;
        auto w{Loader().restoreWallet(fs::PathFromString(ChosenFile(params, /*write=*/false)), name, warnings, /*load_after_restore=*/true)};
        if (!w) throw HostError(util::ErrorString(w).translated);
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", (*w)->getWalletName());
        out.pushKV("warnings", Warnings(warnings));
        return out;
    });
    auto migrate{[this](const std::string& name, const SecureString& passphrase, bool load) {
        auto res{Loader().migrateWallet(name, passphrase, load)};
        if (!res) throw HostError(util::ErrorString(res).translated);
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", name);
        if (res->watchonly_wallet_name) out.pushKV("watchonlyName", *res->watchonly_wallet_name);
        if (res->solvables_wallet_name) out.pushKV("solvablesName", *res->solvables_wallet_name);
        out.pushKV("backup", fs::PathToString(res->backup_path));
        out.pushKV("loaded", load && res->wallet != nullptr);
        return out;
    }};
    Add("wallets.migrate", [migrate, listed_name](const UniValue& params) {
        return migrate(listed_name(params), SecretParam(params, "passphrase", /*required=*/false), Bool(params, "load", true));
    });
    Add("wallets.restoreMigrate", [this, migrate](const UniValue& params) {
        const std::string name{WalletName(Str(params, "name"))};
        std::vector<bilingual_str> warnings;
        auto restored{Loader().restoreWallet(fs::PathFromString(ChosenFile(params, /*write=*/false)), name, warnings, /*load_after_restore=*/false)};
        if (!restored) throw HostError(util::ErrorString(restored).translated);
        const SecureString passphrase{SecretParam(params, "passphrase", /*required=*/false)};
        if (passphrase.empty() && Loader().isEncrypted(name)) {
            // As MigrateWalletActivity::do_migrate: the passphrase of an encrypted wallet is asked for
            // now that it is restored; the page migrates it then (wallets.migrate).
            UniValue out{UniValue::VOBJ};
            out.pushKV("name", name);
            out.pushKV("needsPassphrase", true);
            out.pushKV("warnings", Warnings(warnings));
            return out;
        }
        UniValue out{migrate(name, passphrase, /*load=*/true)};
        out.pushKV("warnings", Warnings(warnings));
        return out;
    });
    Add("wallets.isEncrypted", [this](const UniValue& params) {
        return UniValue{Loader().isEncrypted(Str(params, "name"))};
    });

    // --- One wallet ---

    Add("wallet.info", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", w->getWalletName());
        out.pushKV("encrypted", w->isCrypted());
        out.pushKV("locked", w->isLocked());
        out.pushKV("privateKeysDisabled", w->privateKeysDisabled());
        out.pushKV("externalSigner", w->hasExternalSigner());
        // OptionsModel::hasSigner: the path of a signer script is set (the send button needs it).
        out.pushKV("signerConfigured", !gArgs.GetArg("-signer", "").empty());
        out.pushKV("hd", w->hdEnabled());
        out.pushKV("canGetAddresses", w->canGetAddresses());
        out.pushKV("taproot", w->taprootEnabled());
        out.pushKV("defaultAddressType", FormatOutputType(w->getDefaultAddressType()));
        out.pushKV("confirmTarget", static_cast<int64_t>(w->getConfirmTarget()));
        out.pushKV("requiredFeePerK", w->getRequiredFee(1000));
        out.pushKV("maxTxFee", w->getDefaultMaxTxFee());
        out.pushKV("multiwallet", Loader().getWallets().size() > 1);
        const interfaces::WalletBalances b{w->getBalances()};
        out.pushKV("balance", b.balance);
        out.pushKV("unconfirmed", b.unconfirmed_balance);
        out.pushKV("immature", b.immature_balance);
        return out;
    });
    Add("wallet.encrypt", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const SecureString pass{SecretParam(params, "passphrase")};
        if (pass.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "Cannot encrypt with an empty passphrase");
        if (w->isCrypted()) throw HostError("The wallet is encrypted already");
        if (!w->encryptWallet(pass)) throw HostError("Wallet encryption failed due to an internal error. Your wallet was not encrypted.");
        return UniValue{true};
    });
    Add("wallet.changePassphrase", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const SecureString old_pass{SecretParam(params, "old")};
        // Make sure the wallet is locked before attempting the change (as WalletModel::changePassphrase).
        w->lock();
        if (!w->changeWalletPassphrase(old_pass, SecretParam(params, "new"))) {
            throw HostError(WalletApi::PASSPHRASE_INCORRECT, old_pass.find('\0') == std::string::npos ?
                "The passphrase entered for the wallet decryption was incorrect." :
                "The old passphrase entered for the wallet decryption is incorrect. It contains a null character (ie - a zero byte). "
                "If the passphrase was set with a version of this software prior to 25.0, please try again with only the characters up to — but not including — the first null character.");
        }
        return UniValue{true};
    });
    Add("wallet.unlock", [this, get_wallet](const UniValue& params) {
        // Unlocked for a while (walletpassphrase), with the passphrase typed in the dialog of the host: the page never sees it
        // (audit 7 #1). Whatever is spent meanwhile is still confirmed on the desktop.
        auto w{get_wallet(params)};
        const int64_t seconds{Int(params, "seconds")};
        if (seconds < 1 || seconds > 100'000'000) throw HostError(BRIDGE_INVALID_REQUEST, "seconds: from 1 to 100000000");
        if (!w->isCrypted()) throw HostError("This wallet is not encrypted");
        Window::ConfirmDialog dialog;
        dialog.title = "Unlock the wallet for a while";
        dialog.message = "The keys of the wallet stay unlocked for this long, or until it is locked again. Payments, signatures and the like are "
                         "still asked here first.\n\nUnlock it only if you asked for it.";
        dialog.accept = "Unlock";
        dialog.kind = "unlock";
        dialog.rows = confirm::WalletRows(w->getWalletName());
        dialog.rows.push_back(confirm::Text("For", seconds % 60 == 0 ? strprintf("%d minute%s", seconds / 60, seconds == 60 ? "" : "s") : strprintf("%d seconds", seconds)));
        dialog.passphrase = true;
        dialog.passphrase_label = strprintf("Passphrase of the wallet \"%s\"", guard::OneLine(w->getWalletName(), 64));
        const std::string uri{WalletUri(w->getWalletName())};
        for (int attempt{0}; attempt < PASSPHRASE_TRIES; ++attempt) {
            dialog::Answer answer{Ask(dialog)};
            if (!answer.accepted) throw HostError(BRIDGE_CANCELLED, "The wallet was not unlocked: it was declined.");
            UniValue args{UniValue::VARR};
            args.push_back(std::string{answer.passphrase.begin(), answer.passphrase.end()});
            args.push_back(seconds);
            // Not left in freed memory.
            const auto wipe{[&] {
                std::string& text{const_cast<std::string&>(args[0].get_str())};
                memory_cleanse(text.data(), text.size());
            }};
            try {
                m_node().executeRpc("walletpassphrase", args, uri);
                wipe();
                return UniValue{true};
            } catch (const UniValue& error) {
                wipe();
                const UniValue& code{error.find_value("code")};
                if (!code.isNum() || code.getInt<int>() != PASSPHRASE_INCORRECT) {
                    const UniValue& message{error.find_value("message")};
                    throw HostError(message.isStr() ? message.get_str() : error.write());
                }
                dialog.error = "The passphrase entered for the wallet decryption was incorrect. Try again.";
            }
        }
        throw HostError(PASSPHRASE_INCORRECT, "The passphrase entered for the wallet decryption was incorrect.");
    });
    Add("wallet.lock", [get_wallet](const UniValue& params) {
        return UniValue{get_wallet(params)->lock()};
    });
    Add("wallet.backup", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const std::string file{ChosenFile(params, /*write=*/true)};
        if (!w->backupWallet(file)) throw HostError(strprintf("There was an error trying to save the wallet data to %s.", file));
        return UniValue{file};
    });
    Add("wallet.exportWatchOnly", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        if (w->privateKeysDisabled()) throw HostError("This wallet is watch-only already");
        auto res{w->exportWatchOnlyWallet(fs::PathFromString(ChosenFile(params, /*write=*/true)))};
        if (!res) throw HostError(util::ErrorString(res).translated);
        return UniValue{*res};
    });

    // --- Transactions ---

    Add("wallet.transactions", [get_wallet](const UniValue& params) {
        // The history (TransactionTableModel): all of it, or the records of some transactions only (the
        // ones an event said changed: the page keeps the rest, and brings their status up to date with
        // each block by itself), or the latest ones (the overview).
        auto w{get_wallet(params)};
        wallet::CWallet* cw{w->wallet()};
        if (!cw) throw HostError("No wallet behind this interface");
        const UniValue txids{Param(params, "txids")};
        if (!txids.isNull() && !txids.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, "txids: a list of transaction ids");
        const std::optional<int64_t> latest{OptInt(params, "latest")};
        const auto labels{LabelsOf(*w)};
        UniValue records{UniValue::VARR};
        UniValue out{UniValue::VOBJ};
        LOCK(cw->cs_wallet);
        std::set<Txid> trusted_parents;
        if (txids.isArray()) {
            UniValue gone{UniValue::VARR};
            for (const UniValue& id : txids.getValues()) {
                const auto txid{id.isStr() ? Txid::FromHex(id.get_str()) : std::nullopt};
                if (!txid) throw HostError(BRIDGE_INVALID_REQUEST, "txids: a list of transaction ids");
                const auto it{cw->mapWallet.find(*txid)};
                if (it == cw->mapWallet.end()) {
                    gone.push_back(txid->GetHex());
                    continue;
                }
                AppendRecords(records, *cw, it->second, trusted_parents, labels);
            }
            out.pushKV("gone", std::move(gone));
        } else if (latest) {
            // The newest transactions by their order in the wallet, enough of them for `latest` records;
            // the page sorts them by date.
            const int64_t wanted{std::clamp<int64_t>(*latest, 1, 1000)};
            int64_t txs{0};
            for (auto it{cw->wtxOrdered.rbegin()}; it != cw->wtxOrdered.rend() && (records.size() < static_cast<size_t>(wanted) || txs < wanted * 4); ++it, ++txs) {
                AppendRecords(records, *cw, *it->second, trusted_parents, labels);
            }
        } else {
            for (const auto& [txid, wtx] : cw->mapWallet) AppendRecords(records, *cw, wtx, trusted_parents, labels);
        }
        out.pushKV("height", cw->GetLastBlockHeight());
        out.pushKV("hash", cw->GetLastBlockHash().GetHex());
        // Blocks a coinbase needs on top of its own to be spent (GetTxBlocksToMaturity).
        out.pushKV("maturity", Params().GetConsensus().coinbase_maturity + 1);
        out.pushKV("records", std::move(records));
        return out;
    });
    Add("wallet.balanceHistory", [get_wallet](const UniValue& params) {
        // The balance after each transaction, oldest first, at most `points` of them (the chart of the
        // overview). The amounts are the ones the wallet keeps for each transaction: no records are made.
        auto w{get_wallet(params)};
        wallet::CWallet* cw{w->wallet()};
        if (!cw) throw HostError("No wallet behind this interface");
        const int64_t points{std::clamp<int64_t>(OptInt(params, "points").value_or(500), 2, 5000)};
        std::vector<std::pair<int64_t, CAmount>> changes;
        {
            LOCK(cw->cs_wallet);
            changes.reserve(cw->mapWallet.size());
            for (const auto& [txid, wtx] : cw->mapWallet) {
                // Not counted, as on the page: abandoned, conflicted, and coinbases that did not make it.
                if (wtx.isAbandoned()) continue;
                const int depth{cw->GetTxDepthInMainChain(wtx)};
                if (depth < 0) continue;
                if (wtx.IsCoinBase() && depth == 0) continue;
                CAmount net{0};
                if (wtx.IsCoinBase()) {
                    // Counted as the history shows it, mature or not (the wallet values an immature one at 0).
                    for (const CTxOut& txout : wtx.GetTx()->vout) {
                        if (cw->IsMine(txout)) net += txout.nValue;
                    }
                } else {
                    net = wallet::CachedTxGetCredit(*cw, wtx, /*avoid_reuse=*/true) - wallet::CachedTxGetDebit(*cw, wtx, /*avoid_reuse=*/true);
                }
                changes.emplace_back(wtx.GetTxTime(), net);
            }
        }
        std::stable_sort(changes.begin(), changes.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        std::vector<std::pair<int64_t, CAmount>> series;
        CAmount balance{0};
        for (const auto& [time, net] : changes) {
            balance += net;
            if (!series.empty() && series.back().first == time) series.back().second = balance;
            else series.emplace_back(time, balance);
        }
        UniValue out{UniValue::VARR};
        if (series.size() <= static_cast<size_t>(points)) {
            for (const auto& [time, b] : series) {
                UniValue p{UniValue::VARR};
                p.push_back(time);
                p.push_back(b);
                out.push_back(std::move(p));
            }
        } else {
            // The last balance of each of `points` stretches of time, and the very last one.
            const int64_t first{series.front().first}, last{series.back().first};
            const double step{double(last - first + 1) / points};
            size_t i{0};
            for (int64_t bucket{0}; bucket < points && i < series.size(); ++bucket) {
                const int64_t end{first + static_cast<int64_t>((bucket + 1) * step)};
                size_t j{i};
                while (j < series.size() && series[j].first < end) ++j;
                if (j == i) continue;
                UniValue p{UniValue::VARR};
                p.push_back(series[j - 1].first);
                p.push_back(series[j - 1].second);
                out.push_back(std::move(p));
                i = j;
            }
            if (out.empty() || out[out.size() - 1][0].getInt<int64_t>() != series.back().first) {
                UniValue p{UniValue::VARR};
                p.push_back(series.back().first);
                p.push_back(series.back().second);
                out.push_back(std::move(p));
            }
        }
        return out;
    });
    Add("wallet.transaction", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const auto idx{OptInt(params, "idx")};
        return Describe(m_node(), *w, TxidParam(params), static_cast<int>(idx.value_or(0)), OptStr(params, "address"));
    });
    Add("wallet.txActions", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const Txid txid{TxidParam(params)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("canAbandon", w->transactionCanBeAbandoned(txid));
        out.pushKV("canBump", w->transactionCanBeBumped(txid));
        const CTransactionRef tx{w->getTx(txid)};
        out.pushKV("hex", tx ? EncodeHexTx(*tx) : "");
        return out;
    });
    Add("wallet.abandon", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const Txid txid{TxidParam(params)};
        if (!w->transactionCanBeAbandoned(txid)) throw HostError("This transaction cannot be abandoned");
        return UniValue{w->abandonTransaction(txid)};
    });
    Add("wallet.bump.prepare", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        auto bump{std::make_shared<Bump>()};
        bump->wallet = w->getWalletName();
        bump->txid = TxidParam(params);
        CCoinControl coin_control;
        std::vector<bilingual_str> errors;
        CAmount old_fee, new_fee;
        if (!w->createBumpTransaction(bump->txid, coin_control, errors, old_fee, new_fee, bump->mtx)) {
            throw HostError(strprintf("Increasing transaction fee failed (%s)", errors.empty() ? "" : errors[0].translated));
        }
        bump->old_fee = old_fee;
        bump->new_fee = new_fee;
        const std::string token{NewToken()};
        {
            LOCK(m_mutex);
            m_bumps[token] = bump;
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("token", token);
        out.pushKV("oldFee", old_fee);
        out.pushKV("newFee", new_fee);
        out.pushKV("canSend", !w->privateKeysDisabled() || w->hasExternalSigner());
        return out;
    });
    // A prepared send or bump is used up by its question: answered (yes or no) or failed, it is gone. Only a question
    // carried out of this host unanswered (to the app that runs this daemon, webui/remotehost.h: not an exception of
    // the standard kind) leaves it prepared, for the same call again with the answer. One question per token at a time.
    const auto once{[this](const UniValue& params, bool bump, const std::function<UniValue()>& commit) -> UniValue {
        const std::string token{Str(params, "token")};
        {
            LOCK(m_mutex);
            if (!(bump ? m_bumps.contains(token) : m_prepared.contains(token))) {
                throw HostError(bump ? "This fee bump is no longer prepared: start again" : "This transaction is no longer prepared: send again");
            }
            if (!m_asking.insert(token).second) throw HostError(BRIDGE_INVALID_REQUEST, "Its confirmation is open already");
        }
        const auto done{[&](bool used_up) {
            LOCK(m_mutex);
            m_asking.erase(token);
            if (used_up) bump ? (void)m_bumps.erase(token) : (void)m_prepared.erase(token);
        }};
        try {
            UniValue result{commit()};
            done(true);
            return result;
        } catch (const std::exception&) {
            done(true);
            throw;
        } catch (...) {
            done(false);
            throw;
        }
    }};
    auto take_bump{[this](const UniValue& params, bool erase) {
        LOCK(m_mutex);
        const auto it{m_bumps.find(Str(params, "token"))};
        if (it == m_bumps.end()) throw HostError("This fee bump is no longer prepared: start again");
        auto bump{it->second};
        if (erase) m_bumps.erase(it);
        return bump;
    }};
    Add("wallet.bump.commit", [this, get_wallet, take_bump, once](const UniValue& params) {
        return once(params, /*bump=*/true, [&]() -> UniValue {
            auto bump{take_bump(params, false)};
            auto w{get_wallet(params)};
            if (w->getWalletName() != bump->wallet) throw HostError(BRIDGE_INVALID_REQUEST, "The fee bump is of another wallet");
            if (w->privateKeysDisabled() && !w->hasExternalSigner()) throw HostError("This wallet cannot sign: create an unsigned transaction instead");
            // What the replacement pays, from itself, and the fees before and after: confirmed on the desktop (audit 7 #1).
            Window::ConfirmDialog dialog;
            dialog.title = "Increase the fee";
            dialog.message = "The wallet replaces the transaction with one that pays a higher fee, to the same recipients.\n\nAccept only if you asked for it.";
            dialog.accept = "Increase the fee";
            dialog.kind = "bump";
            const CTransaction replacement{bump->mtx};
            // The output of the wallet's own (its change) is not a payment.
            std::optional<unsigned int> change;
            for (unsigned int i{0}; i < replacement.vout.size(); ++i) {
                if (w->txoutIsMine(replacement.vout[i])) change = i;
            }
            dialog.rows = PaymentRows(*w, replacement, change, bump->new_fee, LabelsOf(*w));
            dialog.rows.insert(dialog.rows.begin() + 2, confirm::Destination("Replaces", bump->txid.GetHex()));
            dialog.rows.insert(dialog.rows.begin() + 3, confirm::Amount("Fee before", bump->old_fee));
            return WithKeys(w->getWalletName(), dialog, /*always=*/true, /*fee_unknown=*/false, [&] {
                CMutableTransaction mtx{bump->mtx};
                if (!w->signBumpTransaction(mtx)) throw HostError("Can't sign transaction.");
                std::vector<bilingual_str> errors;
                Txid new_txid;
                if (!w->commitBumpTransaction(bump->txid, std::move(mtx), errors, new_txid)) {
                    throw HostError(strprintf("Could not commit transaction (%s)", errors.empty() ? "" : errors[0].translated));
                }
                UniValue out{UniValue::VOBJ};
                out.pushKV("txid", new_txid.GetHex());
                return out;
            });
        });
    });
    Add("wallet.bump.psbt", [get_wallet, take_bump](const UniValue& params) {
        auto bump{take_bump(params, true)};
        auto w{get_wallet(params)};
        PartiallySignedTransaction psbtx{bump->mtx};
        bool complete{false};
        const auto err{w->fillPSBT({.sign = false, .bip32_derivs = true}, nullptr, psbtx, complete)};
        if (err || complete) throw HostError("Can't draft transaction.");
        UniValue out{UniValue::VOBJ};
        out.pushKV("psbt", EncodePsbt(psbtx));
        return out;
    });

    // --- Address book (AddressTableModel, EditAddressDialog) ---

    Add("wallet.addresses", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        UniValue out{UniValue::VARR};
        for (const auto& a : w->getAddresses()) {
            // Refund addresses are hidden, as in the Qt address book.
            if (a.purpose == AddressPurpose::REFUND) continue;
            UniValue e{UniValue::VOBJ};
            e.pushKV("address", EncodeDestination(a.dest));
            e.pushKV("label", a.name);
            e.pushKV("purpose", PurposeName(a.purpose));
            e.pushKV("mine", a.is_mine);
            out.push_back(std::move(e));
        }
        return out;
    });
    Add("wallet.owned", [get_wallet](const UniValue& params) {
        // Which of some addresses the explorer shows are the wallet's (it marks them "yours"): at most
        // MAX_OWNED_ASKED a call, those of the page shown only, each looked up in the wallet's own scripts (no
        // scan of the chain). Each one of the wallet: [address, label] (the label "" if it has none); the others
        // and what is no address are left out.
        auto w{get_wallet(params)};
        wallet::CWallet* cw{w->wallet()};
        if (!cw) throw HostError("No wallet behind this interface");
        const UniValue list{Param(params, "addresses")};
        if (!list.isArray() || list.size() > MAX_OWNED_ASKED) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("addresses: a list of %u addresses at most", MAX_OWNED_ASKED));
        const auto labels{LabelsOf(*w)};
        UniValue out{UniValue::VARR};
        LOCK(cw->cs_wallet);
        for (const UniValue& value : list.getValues()) {
            if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "addresses: a list of addresses");
            const CTxDestination dest{DecodeDestination(value.get_str())};
            if (!IsValidDestination(dest) || !cw->IsMine(dest)) continue;
            UniValue entry{UniValue::VARR};
            entry.push_back(value.get_str());
            const auto it{labels.find(EncodeDestination(dest))};
            entry.push_back(it == labels.end() ? std::string{} : it->second);
            out.push_back(std::move(entry));
        }
        return out;
    });
    Add("wallet.address", [get_wallet](const UniValue& params) {
        // An address of the explorer, from what the wallet knows of it (no scan of the chain: of an address that is not
        // the wallet's, only {mine: false}). Of one of the wallet: its label, what it received and what was spent of it,
        // its coins now (unspent, immature included), its transactions newest first: `count` (1 to 100) after `skip`,
        // each [txid, height (-1: not in a block), time, received, spent]; its coins likewise (`coinSkip`, `coinCount`),
        // each [txid, vout, amount, height, immature]. Conflicted and abandoned transactions are left out.
        auto w{get_wallet(params)};
        wallet::CWallet* cw{w->wallet()};
        if (!cw) throw HostError("No wallet behind this interface");
        const CTxDestination dest{DecodeDestination(Str(params, "address"))};
        if (!IsValidDestination(dest)) throw HostError(BRIDGE_INVALID_REQUEST, "address: an address of this chain is needed");
        const auto page{[&](const char* skip_key, const char* count_key) {
            const int64_t skip{OptInt(params, skip_key).value_or(0)};
            const int64_t count{OptInt(params, count_key).value_or(25)};
            if (skip < 0 || count < 1 || count > 100) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: 1 to 100, %s: 0 or more", count_key, skip_key));
            return std::pair<size_t, size_t>{static_cast<size_t>(skip), static_cast<size_t>(count)};
        }};
        const auto [skip, count]{page("skip", "count")};
        const auto [coin_skip, coin_count]{page("coinSkip", "coinCount")};
        std::string label;
        w->getAddress(dest, &label, /*purpose=*/nullptr);
        UniValue out{UniValue::VOBJ};
        LOCK(cw->cs_wallet);
        if (!cw->IsMine(dest)) {
            out.pushKV("mine", false);
            return out;
        }
        const CScript script{GetScriptForDestination(dest)};
        struct Row {
            const wallet::CWalletTx* wtx;
            int height;
            CAmount received{0}, spent{0};
        };
        struct Coin {
            COutPoint outpoint;
            CAmount amount;
            int height;
            bool immature;
        };
        std::vector<Row> rows;
        std::vector<Coin> coins;
        CAmount received{0}, spent{0}, balance{0}, immature{0};
        for (const auto& [txid, wtx] : cw->mapWallet) {
            if (wtx.isAbandoned() || cw->GetTxDepthInMainChain(wtx) < 0) continue;
            const auto* confirmed{wtx.state<wallet::TxStateConfirmed>()};
            Row row{&wtx, confirmed ? confirmed->confirmed_block_height : -1};
            const bool young{cw->IsTxImmatureCoinBase(wtx)};
            const CTransactionRef tx{wtx.GetTx()};
            for (uint32_t n{0}; n < tx->vout.size(); ++n) {
                const CTxOut& txout{tx->vout[n]};
                if (txout.scriptPubKey != script) continue;
                row.received += txout.nValue;
                if (!cw->IsSpent(COutPoint{txid, n})) {
                    coins.push_back({COutPoint{txid, n}, txout.nValue, row.height, young});
                    (young ? immature : balance) += txout.nValue;
                }
            }
            if (!wtx.IsCoinBase()) {
                for (const CTxIn& txin : tx->vin) {
                    const auto prev{cw->mapWallet.find(txin.prevout.hash)};
                    if (prev == cw->mapWallet.end()) continue;
                    const CTransactionRef prev_tx{prev->second.GetTx()};
                    if (txin.prevout.n >= prev_tx->vout.size()) continue;
                    const CTxOut& prevout{prev_tx->vout[txin.prevout.n]};
                    if (prevout.scriptPubKey == script) row.spent += prevout.nValue;
                }
            }
            if (row.received || row.spent) {
                received += row.received;
                spent += row.spent;
                rows.push_back(row);
            }
        }
        // Newest first: those not in a block yet, then by height, then by time.
        const auto newer{[](int a_height, int64_t a_time, int b_height, int64_t b_time) {
            if (a_height != b_height) return a_height < 0 || (b_height >= 0 && a_height > b_height);
            return a_time > b_time;
        }};
        std::sort(rows.begin(), rows.end(), [&](const Row& a, const Row& b) { return newer(a.height, a.wtx->GetTxTime(), b.height, b.wtx->GetTxTime()); });
        std::sort(coins.begin(), coins.end(), [&](const Coin& a, const Coin& b) { return a.height != b.height ? newer(a.height, 0, b.height, 0) : a.outpoint < b.outpoint; });
        out.pushKV("mine", true);
        out.pushKV("label", label);
        out.pushKV("received", received);
        out.pushKV("spent", spent);
        out.pushKV("balance", balance);
        out.pushKV("immature", immature);
        out.pushKV("txCount", static_cast<int64_t>(rows.size()));
        out.pushKV("coinCount", static_cast<int64_t>(coins.size()));
        UniValue history{UniValue::VARR};
        for (size_t i{skip}; i < rows.size() && i < skip + count; ++i) {
            UniValue r{UniValue::VARR};
            r.push_back(rows[i].wtx->GetHash().GetHex());
            r.push_back(rows[i].height);
            r.push_back(rows[i].wtx->GetTxTime());
            r.push_back(rows[i].received);
            r.push_back(rows[i].spent);
            history.push_back(std::move(r));
        }
        out.pushKV("history", std::move(history));
        UniValue held{UniValue::VARR};
        for (size_t i{coin_skip}; i < coins.size() && i < coin_skip + coin_count; ++i) {
            UniValue c{UniValue::VARR};
            c.push_back(coins[i].outpoint.hash.GetHex());
            c.push_back(static_cast<int64_t>(coins[i].outpoint.n));
            c.push_back(coins[i].amount);
            c.push_back(coins[i].height);
            c.push_back(coins[i].immature);
            held.push_back(std::move(c));
        }
        out.pushKV("coins", std::move(held));
        return out;
    });
    auto duplicate_warning{[](interfaces::Wallet& w, const std::string& address, bool sending) {
        std::string label;
        AddressPurpose purpose;
        w.getAddress(DecodeDestination(address), &label, &purpose);
        if (purpose == AddressPurpose::RECEIVE && sending) {
            return strprintf("Address \"%s\" already exists as a receiving address with label \"%s\" and so cannot be added as a sending address.", address, label);
        }
        return strprintf("The entered address \"%s\" is already in the address book with label \"%s\".", address, label);
    }};
    Add("wallet.addressBook.add", [get_wallet, duplicate_warning](const UniValue& params) {
        auto w{get_wallet(params)};
        const std::string address{Str(params, "address")};
        // One line (audit 7 #4): a label never holds line breaks to push the address out of sight.
        const std::string label{ShownLabel(OptStr(params, "label"))};
        if (!IsValidDestinationString(address)) throw HostError(WalletApi::SEND_REFUSED, strprintf("The entered address \"%s\" is not a valid %s address.", address, CLIENT_NAME));
        const CTxDestination dest{DecodeDestination(address)};
        if (w->getAddress(dest, /*name=*/nullptr, /*purpose=*/nullptr)) throw HostError(WalletApi::SEND_REFUSED, duplicate_warning(*w, address, true));
        w->setAddressBook(dest, label, AddressPurpose::SEND);
        return UniValue{address};
    });
    Add("wallet.addressBook.edit", [this, get_wallet, duplicate_warning](const UniValue& params) {
        auto w{get_wallet(params)};
        const std::string address{Str(params, "address")};
        const CTxDestination cur{DecodeDestination(address)};
        std::string old_label;
        AddressPurpose purpose;
        if (!w->getAddress(cur, &old_label, &purpose)) throw HostError("This address is not in the address book");
        if (purpose == AddressPurpose::REFUND) throw HostError("This address cannot be edited");
        const bool sending{purpose == AddressPurpose::SEND};
        const std::string label{Param(params, "label").isStr() ? ShownLabel(OptStr(params, "label")) : old_label};
        const std::string new_address{OptStr(params, "newAddress", address)};
        UniValue out{UniValue::VOBJ};
        if (new_address != address) {
            // Only sending addresses can change their address.
            if (!sending) throw HostError("The address of a receiving address cannot be changed");
            const CTxDestination dest{DecodeDestination(new_address)};
            if (std::get_if<CNoDestination>(&dest)) throw HostError(WalletApi::SEND_REFUSED, strprintf("The entered address \"%s\" is not a valid %s address.", new_address, CLIENT_NAME));
            if (w->getAddress(dest, nullptr, nullptr)) throw HostError(WalletApi::SEND_REFUSED, duplicate_warning(*w, new_address, true));
            // The label stays, the address behind it changes: what the user pays to by that name from now on. On the user's
            // word given to the desktop, old and new address whole (audit 7 #6).
            Window::ConfirmDialog dialog;
            dialog.title = "Change the address of a label";
            dialog.message = "Payments to this label go to the new address from now on.\n\nAccept only if you asked for this change.";
            dialog.accept = "Change the address";
            dialog.kind = "addressbook";
            dialog.level = dialog::Level::DANGER;
            dialog.rows = confirm::WalletRows(w->getWalletName());
            dialog.rows.push_back(confirm::Text("Label", "\u201c" + ShownLabel(label) + "\u201d"));
            dialog.rows.push_back(confirm::Destination("Old address", address));
            dialog.rows.push_back(confirm::Destination("New address", new_address));
            if (!Ask(dialog).accepted) throw HostError(BRIDGE_CANCELLED, "The address was not changed: it was declined.");
            w->delAddressBook(cur);
            w->setAddressBook(dest, label, purpose);
            out.pushKV("address", new_address);
            out.pushKV("changed", true);
            return out;
        }
        out.pushKV("address", address);
        if (label == old_label) {
            out.pushKV("changed", false);
            return out;
        }
        w->setAddressBook(cur, label, purpose);
        out.pushKV("changed", true);
        return out;
    });
    Add("wallet.addressBook.delete", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const CTxDestination dest{DecodeDestination(Str(params, "address"))};
        AddressPurpose purpose;
        if (!w->getAddress(dest, nullptr, &purpose)) throw HostError("This address is not in the address book");
        // Receiving addresses are never removed.
        if (purpose != AddressPurpose::SEND) throw HostError("Only sending addresses can be deleted");
        return UniValue{w->delAddressBook(dest)};
    });
    Add("wallet.newAddress", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const std::string label{ShownLabel(OptStr(params, "label"))};
        OutputType type{w->getDefaultAddressType()};
        if (const std::string t{OptStr(params, "type")}; !t.empty()) {
            const auto parsed{ParseOutputType(t)};
            if (!parsed) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("Unknown address type %s", t));
            type = *parsed;
        }
        // As AddressTableModel::addRow for a receiving address: unlock only when the keys are needed.
        if (auto dest{w->getNewDestination(type, label)}) return UniValue{EncodeDestination(*dest)};
        // The keys are needed to make more: the passphrase is asked in the dialog of the host (never by the page).
        Window::ConfirmDialog dialog;
        dialog.title = "Unlock the wallet";
        dialog.message = "The wallet needs its keys to make new addresses. Nothing is spent.";
        dialog.accept = "Unlock";
        dialog.kind = "keys:newaddress";
        dialog.level = dialog::Level::INFO;
        dialog.rows = confirm::WalletRows(w->getWalletName());
        return WithKeys(w->getWalletName(), dialog, /*always=*/false, /*fee_unknown=*/false, [&]() -> UniValue {
            if (auto dest{w->getNewDestination(type, label)}) return UniValue{EncodeDestination(*dest)};
            throw HostError(strprintf("Could not generate new %s address", FormatOutputType(type)));
        });
    });
    Add("wallet.displayAddress", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        try {
            auto res{w->displayAddress(DecodeDestination(Str(params, "address")))};
            if (!res) throw HostError(strprintf("Signer error: %s", util::ErrorString(res).translated));
        } catch (const std::runtime_error& e) {
            if (dynamic_cast<const HostError*>(&e)) throw;
            throw HostError(strprintf("Can't display address: %s", e.what()));
        }
        return UniValue{true};
    });

    // --- Receive requests (RecentRequestsTableModel) ---

    Add("wallet.requests", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        UniValue out{UniValue::VARR};
        for (const std::string& data : w->getAddressReceiveRequests()) {
            const auto r{DecodeReceiveRequest(data)};
            if (!r || r->id == 0) continue;
            UniValue e{UniValue::VOBJ};
            e.pushKV("id", r->id);
            e.pushKV("time", r->time);
            e.pushKV("address", r->address);
            e.pushKV("label", r->label);
            e.pushKV("amount", r->amount);
            e.pushKV("message", r->message);
            out.push_back(std::move(e));
        }
        return out;
    });
    Add("wallet.requests.add", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        ReceiveRequest r;
        r.address = Str(params, "address");
        r.label = OptStr(params, "label");
        r.amount = OptInt(params, "amount").value_or(0);
        r.message = OptStr(params, "message");
        if (r.amount < 0 || !MoneyRange(r.amount)) throw HostError(BRIDGE_INVALID_REQUEST, "amount: out of range");
        const CTxDestination dest{DecodeDestination(r.address)};
        if (!IsValidDestination(dest)) throw HostError(BRIDGE_INVALID_REQUEST, "address: not a valid address");
        int64_t max_id{0};
        for (const std::string& data : w->getAddressReceiveRequests()) {
            if (const auto old{DecodeReceiveRequest(data)}) max_id = std::max(max_id, old->id);
        }
        r.id = max_id + 1;
        r.time = GetTime();
        if (!w->setAddressReceiveRequest(dest, util::ToString(r.id), EncodeReceiveRequest(r))) throw HostError("The request could not be saved in the wallet");
        UniValue e{UniValue::VOBJ};
        e.pushKV("id", r.id);
        e.pushKV("time", r.time);
        e.pushKV("address", r.address);
        e.pushKV("label", r.label);
        e.pushKV("amount", r.amount);
        e.pushKV("message", r.message);
        return e;
    });
    Add("wallet.requests.remove", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const UniValue items{Param(params, "items")};
        if (!items.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, "items: a list of {address, id}");
        int64_t removed{0};
        for (const UniValue& item : items.getValues()) {
            if (w->setAddressReceiveRequest(DecodeDestination(Str(item, "address")), util::ToString(Int(item, "id")), "")) ++removed;
        }
        return UniValue{removed};
    });

    // --- Coin control (CoinControlDialog) ---

    Add("wallet.coins", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const auto labels{LabelsOf(*w)};
        UniValue out{UniValue::VARR};
        for (const auto& [dest, coins] : w->listCoins()) {
            UniValue group{UniValue::VOBJ};
            const std::string address{EncodeDestination(dest)};
            const auto label{labels.find(address)};
            group.pushKV("address", address);
            group.pushKV("label", label == labels.end() ? "" : label->second);
            UniValue list{UniValue::VARR};
            for (const auto& [outpoint, out_info] : coins) {
                UniValue c{OutpointJson(outpoint)};
                CTxDestination output_address;
                const std::string coin_address{ExtractDestination(out_info.txout.scriptPubKey, output_address) ? EncodeDestination(output_address) : ""};
                c.pushKV("address", coin_address);
                c.pushKV("change", coin_address != address);
                const auto coin_label{labels.find(coin_address)};
                c.pushKV("label", coin_label == labels.end() ? "" : coin_label->second);
                c.pushKV("sats", out_info.txout.nValue);
                c.pushKV("time", out_info.time);
                c.pushKV("depth", out_info.depth_in_main_chain);
                c.pushKV("locked", w->isLockedCoin(outpoint));
                list.push_back(std::move(c));
            }
            group.pushKV("coins", std::move(list));
            out.push_back(std::move(group));
        }
        return out;
    });
    Add("wallet.coins.lock", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const COutPoint outpoint{OutpointParam(params)};
        if (Bool(params, "lock", true)) return UniValue{w->lockCoin(outpoint, /*write_to_db=*/true)};
        return UniValue{w->unlockCoin(outpoint)};
    });
    Add("wallet.coinControl.summary", [this, get_wallet](const UniValue& params) {
        // CoinControlDialog::updateLabels: an estimate of the transaction made of the coins chosen.
        auto w{get_wallet(params)};
        CCoinControl cc{MakeCoinControl(params)};
        std::vector<CAmount> pay_amounts;
        CAmount nPayAmount{0};
        if (const UniValue amounts{Param(params, "amounts")}; amounts.isArray()) {
            for (const UniValue& a : amounts.getValues()) {
                pay_amounts.push_back(a.getInt<int64_t>());
                nPayAmount += a.getInt<int64_t>();
            }
        }
        const bool subtract_fee{Bool(params, "subtractFee")};
        CAmount nAmount{0}, nPayFee{0}, nAfterFee{0}, nChange{0};
        unsigned int nBytes{0}, nBytesInputs{0}, nQuantity{0};
        bool fWitness{false};
        const auto selected{cc.ListSelected()};
        UniValue spent{UniValue::VARR};
        size_t i{0};
        for (const auto& out : w->getCoins(selected)) {
            if (out.depth_in_main_chain < 0) continue;
            const COutPoint& outpt{selected[i++]};
            if (out.is_spent) {
                // Spent elsewhere meanwhile: no longer selected.
                cc.UnSelect(outpt);
                spent.push_back(OutpointJson(outpt));
                continue;
            }
            nQuantity++;
            nAmount += out.txout.nValue;
            CTxDestination address;
            int witnessversion{0};
            std::vector<unsigned char> witnessprogram;
            if (out.txout.scriptPubKey.IsWitnessProgram(witnessversion, witnessprogram)) {
                nBytesInputs += (32 + 4 + 1 + 4);
                if (witnessversion == 0) {
                    nBytesInputs += 107 / WITNESS_SCALE_FACTOR;
                } else if (witnessversion == 1) {
                    nBytesInputs += 66 / WITNESS_SCALE_FACTOR;
                } else {
                    throw HostError("Trying to spend future segwit version script");
                }
                fWitness = true;
            } else if (ExtractDestination(out.txout.scriptPubKey, address)) {
                CPubKey pubkey;
                const PKHash* pkhash{std::get_if<PKHash>(&address)};
                if (pkhash && w->getPubKey(out.txout.scriptPubKey, ToKeyID(*pkhash), pubkey)) {
                    nBytesInputs += (pubkey.IsCompressed() ? 148 : 180);
                } else {
                    nBytesInputs += 148;
                }
            } else {
                nBytesInputs += 148;
            }
        }
        if (nQuantity > 0) {
            nBytes = nBytesInputs + ((pay_amounts.size() > 0 ? pay_amounts.size() + 1 : 2) * 34) + 10;
            if (fWitness) {
                nBytes += 2;
                nBytes += nQuantity;
            }
            if (subtract_fee && nAmount - nPayAmount == 0) nBytes -= 34;
            nPayFee = w->getMinimumFee(nBytes, cc, /*returned_target=*/nullptr, /*reason=*/nullptr);
            if (nPayAmount > 0) {
                nChange = nAmount - nPayAmount;
                if (!subtract_fee) nChange -= nPayFee;
                if (nChange > 0) {
                    // Assumes a p2pkh script size.
                    CTxOut txout(nChange, CScript() << std::vector<unsigned char>(24, 0));
                    // Never create dust outputs; if we would, just add the dust to the fee.
                    if (IsDust(txout, m_node().getDustRelayFee())) {
                        nPayFee += nChange;
                        nChange = 0;
                        if (subtract_fee) nBytes -= 34;
                    }
                }
                if (nChange == 0 && !subtract_fee) nBytes -= 34;
            }
            nAfterFee = std::max<CAmount>(nAmount - nPayFee, 0);
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("quantity", static_cast<int64_t>(nQuantity));
        out.pushKV("amount", nAmount);
        out.pushKV("fee", nPayFee);
        out.pushKV("afterFee", nAfterFee);
        out.pushKV("bytes", static_cast<int64_t>(nBytes));
        out.pushKV("change", nChange);
        out.pushKV("changeEnabled", nPayAmount > 0);
        out.pushKV("insufficient", nChange < 0);
        out.pushKV("feeVary", nBytes != 0 ? double(nPayFee) / nBytes : 0.0);
        out.pushKV("spent", std::move(spent));
        return out;
    });
    Add("wallet.fee", [get_wallet](const UniValue& params) {
        // SendCoinsDialog::updateSmartFeeLabel: the fee rate of the confirmation target, without a custom fee.
        auto w{get_wallet(params)};
        CCoinControl cc{MakeCoinControl(params)};
        cc.m_feerate.reset();
        std::optional<int> returned_target;
        FeeReason reason;
        const CFeeRate rate{w->getMinimumFee(1000, cc, &returned_target, &reason)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("feePerK", rate.GetFeePerK());
        out.pushKV("fallback", reason == FeeReason::FALLBACK);
        out.pushKV("reason", common::StringForFeeReason(reason));
        if (returned_target) out.pushKV("target", *returned_target);
        out.pushKV("requiredFeePerK", w->getRequiredFee(1000));
        return out;
    });
    Add("wallet.available", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        CCoinControl cc{MakeCoinControl(params)};
        cc.m_allow_other_inputs = !cc.HasSelected();
        // As WalletModel::getAvailableBalance: the balance, or what the selected coins give.
        if (!cc.HasSelected()) return UniValue{w->getBalances().balance};
        return UniValue{w->getAvailableBalance(cc)};
    });
    Add("wallet.checkChange", [get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const std::string address{Str(params, "address")};
        const CTxDestination dest{DecodeDestination(address)};
        UniValue out{UniValue::VOBJ};
        out.pushKV("valid", IsValidDestination(dest));
        out.pushKV("mine", IsValidDestination(dest) && w->isSpendable(dest));
        out.pushKV("label", IsValidDestination(dest) ? LabelOf(*w, dest) : "");
        return out;
    });

    Add("send.isDust", [this](const UniValue& params) {
        // GUIUtil::isDust: an output of this amount to this address would be dust (the send form refuses it).
        const CTxDestination dest{DecodeDestination(Str(params, "address"))};
        if (!IsValidDestination(dest)) return UniValue{false};
        const CTxOut txout{Int(params, "amount"), GetScriptForDestination(dest)};
        return UniValue{IsDust(txout, m_node().getDustRelayFee())};
    });

    // --- Send (WalletModel::prepareTransaction, sendCoins; SendCoinsDialog) ---

    Add("wallet.send.prepare", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const UniValue list{Param(params, "recipients")};
        if (!list.isArray() || list.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "recipients: at least one");
        auto prepared{std::make_shared<Prepared>()};
        prepared->wallet = w->getWalletName();
        std::vector<wallet::CRecipient> vecSend;
        std::set<std::string> addresses;
        CAmount total{0};
        bool subtract{false};
        size_t data_outputs{0};
        for (const UniValue& r : list.getValues()) {
            if (Param(r, "data").isStr()) {
                // Data in the chain (a timestamp): an OP_RETURN output of no value, shown as such on the desktop.
                const auto bytes{TryParseHex<unsigned char>(Str(r, "data"))};
                if (!bytes || bytes->empty() || bytes->size() > 80) throw HostError(WalletApi::SEND_REFUSED, "data: the hexadecimal bytes of the data, at most 80");
                if (++data_outputs > 1) throw HostError(WalletApi::SEND_REFUSED, "One output of data at most");
                vecSend.push_back(wallet::CRecipient{CNoDestination{CScript() << OP_RETURN << *bytes}, 0, false});
                continue;
            }
            Prepared::Recipient rcp;
            rcp.address = Str(r, "address");
            // One line each (audit 7 #4): a label or a message never pushes what is paid out of sight.
            rcp.label = ShownLabel(OptStr(r, "label"));
            rcp.message = guard::OneLine(OptStr(r, "message"), 300);
            rcp.amount = OptInt(r, "amount").value_or(0);
            const bool sub{Bool(r, "subtractFee")};
            if (!IsValidDestinationString(rcp.address)) throw HostError(WalletApi::SEND_REFUSED, "The recipient address is not valid. Please recheck.");
            if (rcp.amount <= 0) throw HostError(WalletApi::SEND_REFUSED, "The amount to pay must be larger than 0.");
            // Every amount, and their sum, within the coins there can be (audit 7 #19).
            if (!MoneyRange(rcp.amount) || !MoneyRange(total + rcp.amount)) throw HostError(WalletApi::SEND_REFUSED, "The amount exceeds your balance.");
            if (sub) subtract = true;
            addresses.insert(rcp.address);
            vecSend.push_back(wallet::CRecipient{DecodeDestination(rcp.address), rcp.amount, sub});
            total += rcp.amount;
            prepared->recipients.push_back(rcp);
        }
        if (prepared->recipients.empty() && data_outputs == 0) throw HostError(BRIDGE_INVALID_REQUEST, "recipients: at least one");
        if (addresses.size() != prepared->recipients.size()) throw HostError(WalletApi::SEND_REFUSED, "Duplicate address found: addresses should only be used once each.");

        CCoinControl cc{MakeCoinControl(params)};
        // A change address of the page must be the wallet's own: the change would be a payment to someone else otherwise.
        if (!std::get_if<CNoDestination>(&cc.destChange) && !w->isSpendable(cc.destChange)) {
            throw HostError(WalletApi::SEND_REFUSED, "The custom change address is not an address of this wallet.");
        }
        // Future: a checkbox to choose this (as the Qt GUI says).
        cc.m_allow_other_inputs = !cc.HasSelected();
        const CAmount balance{cc.HasSelected() ? w->getAvailableBalance(cc) : w->getBalances().balance};
        if (total > balance) throw HostError(WalletApi::SEND_REFUSED, "The amount exceeds your balance.");

        // Made, not signed: the keys sign it once the user accepted it on the desktop (wallet.send.commit), with the
        // passphrase typed there. The page never gives one.
        CAmount fee;
        try {
            auto res{w->createTransaction(vecSend, cc, /*sign=*/false, /*change_pos=*/std::nullopt)};
            if (!res) throw HostError(WalletApi::SEND_REFUSED, strprintf("Transaction creation failed! %s", util::ErrorString(res).translated));
            prepared->tx = res->tx;
            prepared->change_pos = res->change_pos;
            fee = res->fee;
            if (subtract && prepared->tx) {
                // WalletModelTransaction::reassignAmounts: the amounts the outputs got, the fee taken off (the outputs of
                // the recipients are in their order, around the change; an output of data follows them).
                const int change_pos{res->change_pos ? static_cast<int>(*res->change_pos) : -1};
                int i{0};
                for (auto& rcp : prepared->recipients) {
                    if (i == change_pos) i++;
                    rcp.amount = prepared->tx->vout[i].nValue;
                    i++;
                }
            }
        } catch (const HostError&) {
            throw;
        } catch (const std::runtime_error& e) {
            throw HostError(strprintf("Transaction creation failed! %s", e.what()));
        }
        prepared->fee = fee;
        // Reject absurdly high fee (belt and suspenders: the wallet never makes one).
        if (fee > w->getDefaultMaxTxFee()) throw HostError(WalletApi::SEND_REFUSED, strprintf("A fee higher than %s is considered an absurdly high fee.", FormatCoins(w->getDefaultMaxTxFee())));

        const std::string token{NewToken()};
        {
            LOCK(m_mutex);
            // A few prepared sends at most: the oldest go.
            while (m_prepared.size() >= 16) m_prepared.erase(m_prepared.begin());
            m_prepared[token] = prepared;
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("token", token);
        out.pushKV("fee", fee);
        // The size once signed (the transaction is not signed yet): what the fee was set for.
        int64_t vsize{GetVirtualTransactionSize(*prepared->tx)};
        if (wallet::CWallet* cw{w->wallet()}) {
            LOCK(cw->cs_wallet);
            const wallet::TxSize signed_size{wallet::CalculateMaximumSignedTxSize(*prepared->tx, cw, &cc)};
            if (signed_size.vsize > 0) vsize = signed_size.vsize;
        }
        out.pushKV("vsize", vsize);
        CAmount amounts{0};
        UniValue rcps{UniValue::VARR};
        for (const auto& rcp : prepared->recipients) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("address", rcp.address);
            r.pushKV("label", rcp.label);
            r.pushKV("amount", rcp.amount);
            rcps.push_back(std::move(r));
            amounts += rcp.amount;
        }
        out.pushKV("recipients", std::move(rcps));
        out.pushKV("total", amounts + fee);
        out.pushKV("privateKeysDisabled", w->privateKeysDisabled());
        out.pushKV("externalSigner", w->hasExternalSigner());
        out.pushKV("txid", prepared->tx->GetHash().GetHex());
        return out;
    });
    auto take_prepared{[this](const UniValue& params, bool erase) {
        LOCK(m_mutex);
        const auto it{m_prepared.find(Str(params, "token"))};
        if (it == m_prepared.end()) throw HostError("This transaction is no longer prepared: send again");
        auto prepared{it->second};
        if (erase) m_prepared.erase(it);
        return prepared;
    }};
    Add("wallet.send.commit", [this, get_wallet, take_prepared, once](const UniValue& params) {
        return once(params, /*bump=*/false, [&]() -> UniValue {
            auto prepared{take_prepared(params, false)};
            auto w{get_wallet(params)};
            if (w->getWalletName() != prepared->wallet) throw HostError(BRIDGE_INVALID_REQUEST, "The transaction is of another wallet");
            if (w->privateKeysDisabled() && !w->hasExternalSigner()) throw HostError("This wallet cannot sign: create an unsigned transaction instead");
            // What the transaction pays, read from it (not from what the page asked): each destination whole, then its amount
            // and the label the page gave it, the fee, the total; confirmed on the desktop (audit 7 #1).
            std::map<std::string, std::string> labels;
            for (const auto& rcp : prepared->recipients) labels.emplace(rcp.address, rcp.label);
            Window::ConfirmDialog dialog;
            dialog.title = w->hasExternalSigner() ? "Send coins: sign on the device" : "Send coins";
            dialog.message = "Check each address in full: the coins cannot be taken back once sent.\n\nSend only if you asked for this payment.";
            dialog.accept = w->hasExternalSigner() ? "Sign on the device" : "Send";
            dialog.kind = "send";
            dialog.rows = PaymentRows(*w, *prepared->tx, prepared->change_pos, prepared->fee, labels);
            return WithKeys(w->getWalletName(), dialog, /*always=*/true, /*fee_unknown=*/false, [&] {
                CTransactionRef tx{prepared->tx};
                UniValue out{UniValue::VOBJ};
                CMutableTransaction mtx{*tx};
                if (w->hasExternalSigner()) {
                    // SendCoinsDialog::signWithExternalSigner: fill without signing first, then sign on the device.
                    PartiallySignedTransaction psbtx{mtx};
                    bool complete{false};
                    if (w->fillPSBT({.sign = false, .bip32_derivs = true}, nullptr, psbtx, complete)) throw HostError("Transaction creation failed!");
                    std::optional<common::PSBTError> err;
                    try {
                        err = w->fillPSBT({.sign = true, .bip32_derivs = true}, nullptr, psbtx, complete);
                    } catch (const std::runtime_error& e) {
                        throw HostError(strprintf("Sign failed: %s", e.what()));
                    }
                    if (err == common::PSBTError::EXTERNAL_SIGNER_NOT_FOUND) throw HostError("External signer not found");
                    if (err == common::PSBTError::EXTERNAL_SIGNER_FAILED) throw HostError("External signer failure");
                    if (err) throw HostError("Transaction creation failed!");
                    // fillPSBT does not always properly finalize.
                    complete = FinalizeAndExtractPSBT(psbtx, mtx);
                    if (!complete) {
                        // More signatures are needed (a multisig wallet): the PSBT goes to the user.
                        out.pushKV("complete", false);
                        out.pushKV("psbt", EncodePsbt(psbtx));
                        return out;
                    }
                } else if (!w->signBumpTransaction(mtx)) {
                    // The keys of the wallet sign each of its inputs.
                    throw HostError("Can't sign transaction.");
                }
                tx = MakeTransactionRef(mtx);
                std::vector<std::string> messages;
                for (const auto& rcp : prepared->recipients) {
                    // Message from a payment URI (chains:...?message=example).
                    if (!rcp.message.empty()) messages.push_back(rcp.message);
                }
                w->commitTransaction(tx, messages);
                // Addresses sent to go in the address book, with their label (WalletModel::sendCoins).
                for (const auto& rcp : prepared->recipients) {
                    const CTxDestination dest{DecodeDestination(rcp.address)};
                    std::string name;
                    if (!w->getAddress(dest, &name, /*purpose=*/nullptr)) {
                        w->setAddressBook(dest, rcp.label, AddressPurpose::SEND);
                    } else if (name != rcp.label) {
                        w->setAddressBook(dest, rcp.label, {});
                    }
                }
                out.pushKV("complete", true);
                out.pushKV("txid", tx->GetHash().GetHex());
                return out;
            });
        });
    });
    Add("wallet.send.psbt", [get_wallet, take_prepared](const UniValue& params) {
        auto prepared{take_prepared(params, true)};
        auto w{get_wallet(params)};
        CMutableTransaction mtx{*prepared->tx};
        PartiallySignedTransaction psbtx{mtx};
        bool complete{false};
        // Fill without signing.
        const auto err{w->fillPSBT({.sign = false, .bip32_derivs = true}, nullptr, psbtx, complete)};
        if (err) throw HostError(strprintf("Can't draft transaction: %s", common::PSBTErrorString(*err).translated));
        UniValue out{UniValue::VOBJ};
        out.pushKV("psbt", EncodePsbt(psbtx));
        return out;
    });
    Add("wallet.send.discard", [this](const UniValue& params) {
        LOCK(m_mutex);
        return UniValue{m_prepared.erase(Str(params, "token")) > 0};
    });

    // (Wallet RPC methods that pay, sign or need the keys: rpc.confirmed, confirm.cpp, through WithKeys.)

    // --- Messages (SignVerifyMessageDialog) ---

    Add("wallet.signMessage", [this, get_wallet](const UniValue& params) {
        auto w{get_wallet(params)};
        const CTxDestination destination{DecodeDestination(Str(params, "address"))};
        if (!IsValidDestination(destination)) throw HostError(-5, "The entered address is invalid. Please check the address and try again.");
        // The key of a P2WPKH address is looked up by the same hash as that of a P2PKH address.
        std::optional<PKHash> pkhash;
        if (const PKHash* legacy{std::get_if<PKHash>(&destination)}) {
            pkhash = *legacy;
        } else if (const WitnessV0KeyHash* witness{std::get_if<WitnessV0KeyHash>(&destination)}) {
            pkhash = PKHash{uint160{*witness}};
        }
        if (!pkhash) throw HostError(-3, "The entered address does not refer to a single key. Messages can be signed with P2PKH and P2WPKH addresses. Please check the address and try again.");
        // The message and the address, as the host has them: confirmed on the desktop (audit 7 #1).
        const std::string message{Str(params, "message")};
        Window::ConfirmDialog dialog;
        dialog.title = "Sign a message";
        dialog.message = "The wallet signs this message with the key of this address. Whoever gets the signature can show it as proof that the "
                         "holder of the address wrote the message.\n\nSign it only if you asked for it.";
        dialog.accept = "Sign";
        dialog.kind = "sign";
        dialog.rows = confirm::WalletRows(w->getWalletName());
        dialog.rows.push_back(confirm::Destination("Address", EncodeDestination(destination)));
        dialog.rows.push_back(confirm::Text("Message", message));
        return WithKeys(w->getWalletName(), dialog, /*always=*/true, /*fee_unknown=*/false, [&] {
            std::string signature;
            switch (w->signMessage(message, *pkhash, signature)) {
            case SigningResult::OK: break;
            case SigningResult::PRIVATE_KEY_NOT_AVAILABLE: throw HostError(-4, "Private key for the entered address is not available.");
            case SigningResult::SIGNING_FAILED: throw HostError("Message signing failed.");
            }
            UniValue out{UniValue::VOBJ};
            out.pushKV("signature", signature);
            return out;
        });
    });
    Add("message.verify", [](const UniValue& params) {
        const auto result{MessageVerify(Str(params, "address"), Str(params, "signature"), Str(params, "message"))};
        UniValue out{UniValue::VOBJ};
        std::string text, field;
        switch (result) {
        case MessageVerificationResult::OK: text = "Message verified."; break;
        case MessageVerificationResult::ERR_INVALID_ADDRESS: text = "The entered address is invalid. Please check the address and try again."; field = "address"; break;
        case MessageVerificationResult::ERR_ADDRESS_NO_KEY: text = "The entered address does not refer to a single key. Messages can be signed with P2PKH and P2WPKH addresses. Please check the address and try again."; field = "address"; break;
        case MessageVerificationResult::ERR_MALFORMED_SIGNATURE: text = "The signature could not be decoded. Please check the signature and try again."; field = "signature"; break;
        case MessageVerificationResult::ERR_PUBKEY_NOT_RECOVERED: text = "The signature did not match the message digest. Please check the signature and try again."; field = "signature"; break;
        case MessageVerificationResult::ERR_NOT_SIGNED: text = "Message verification failed."; break;
        }
        out.pushKV("ok", result == MessageVerificationResult::OK);
        out.pushKV("message", text);
        if (!field.empty()) out.pushKV("field", field);
        return out;
    });

    // --- PSBT (WalletFrame::gotoLoadPSBT, PSBTOperationsDialog) ---

    Add("psbt.decode", [](const UniValue& params) {
        std::vector<unsigned char> data;
        if (Param(params, "fileBase64").isStr()) {
            // The bytes of a file (base64 is only the transport here).
            auto bytes{DecodeBase64(Str(params, "fileBase64"))};
            if (!bytes) throw HostError(BRIDGE_INVALID_REQUEST, "fileBase64: not base64");
            if (bytes->size() >= static_cast<size_t>(MAX_FILE_SIZE_PSBT)) throw HostError("PSBT file must be smaller than 100 MiB");
            data = std::move(*bytes);
            // Some PSBT files hold a base64 string rather than binary data.
            std::string b64_str{data.begin(), data.end()};
            b64_str.erase(b64_str.find_last_not_of(" \t\n\r\f\v") + 1);
            if (auto b64_dec{DecodeBase64(b64_str)}) data = std::move(*b64_dec);
        } else {
            std::string raw{Str(params, "base64")};
            raw.erase(raw.find_last_not_of(" \t\n\r\f\v") + 1);
            raw.erase(0, raw.find_first_not_of(" \t\n\r\f\v"));
            auto result{DecodeBase64(raw)};
            if (!result) throw HostError("Unable to decode PSBT from clipboard (invalid base64)");
            data = std::move(*result);
        }
        auto psbt{DecodeRawPSBT(MakeByteSpan(data))};
        if (!psbt) throw HostError(strprintf("Unable to decode PSBT\n%s", util::ErrorString(psbt).original));
        UniValue out{UniValue::VOBJ};
        out.pushKV("psbt", EncodePsbt(*psbt));
        return out;
    });
    Add("wallet.psbt.open", [find_wallet](const UniValue& params) {
        PartiallySignedTransaction psbtx{DecodePsbtParam(params)};
        auto w{find_wallet(params)};
        // Make sure all existing signatures are fully combined before checking for completeness.
        bool complete{FinalizePSBT(psbtx)};
        bool can_sign{false};
        if (w) {
            size_t n_could_sign;
            const auto err{w->fillPSBT({.sign = false, .bip32_derivs = true}, &n_could_sign, psbtx, complete)};
            if (err) throw HostError(strprintf("Failed to load transaction: %s", common::PSBTErrorString(*err).translated));
            can_sign = !complete && !w->privateKeysDisabled() && n_could_sign > 0;
        }
        UniValue out{PsbtState(w.get(), psbtx, complete)};
        out.pushKV("canSign", can_sign);
        return out;
    });
    Add("wallet.psbt.sign", [this, get_wallet](const UniValue& params) {
        PartiallySignedTransaction psbtx{DecodePsbtParam(params)};
        auto w{get_wallet(params)};
        // What the PSBT spends of this wallet and pays, decoded here: confirmed on the desktop before the keys sign it (audit 7 #1).
        Window::ConfirmDialog dialog;
        dialog.title = "Sign a partially signed transaction";
        dialog.message = "The wallet signs the inputs of this transaction that are its own. Once it has every signature it needs, whoever has it "
                         "can broadcast it.\n\nSign it only if you asked for it.";
        dialog.accept = "Sign";
        dialog.kind = "psbt";
        dialog.rows = PsbtRows(w.get(), psbtx);
        const bool finalize{Bool(params, "finalize", true)};
        bool complete{false};
        size_t n_signed{0};
        // The signatures of each input before: an input signed without being finalized (a multisig) counts as signed too.
        const auto signatures{[](const PSBTInput& input) { return input.partial_sigs.size() + input.m_tap_script_sigs.size() + (input.m_tap_key_sig.empty() ? 0 : 1); }};
        std::vector<size_t> before;
        for (const PSBTInput& input : psbtx.inputs) before.push_back(signatures(input));
        WithKeys(w->getWalletName(), dialog, /*always=*/true, /*fee_unknown=*/false, [&] {
            const auto err{w->fillPSBT({.sign = true, .finalize = finalize, .bip32_derivs = true}, &n_signed, psbtx, complete)};
            if (err) throw HostError(strprintf("Failed to sign transaction: %s", common::PSBTErrorString(*err).translated));
            return UniValue{};
        });
        if (!finalize) {
            n_signed = 0;
            for (size_t i{0}; i < psbtx.inputs.size() && i < before.size(); ++i) {
                if (signatures(psbtx.inputs[i]) > before[i]) ++n_signed;
            }
        }
        UniValue out{PsbtState(w.get(), psbtx, complete)};
        UniValue status{UniValue::VOBJ};
        if (!complete && n_signed < 1) {
            status.pushKV("text", "Could not sign any more inputs.");
            status.pushKV("level", "warning");
        } else if (!complete) {
            status.pushKV("text", strprintf("Signed %d input%s, but more signatures are still required.", n_signed, n_signed == 1 ? "" : "s"));
            status.pushKV("level", "info");
        } else {
            status.pushKV("text", "Signed transaction successfully. Transaction is ready to broadcast.");
            status.pushKV("level", "success");
        }
        out.pushKV("status", std::move(status));
        out.pushKV("signed", static_cast<int64_t>(n_signed));
        out.pushKV("canSign", !complete && n_signed > 0);
        return out;
    });
    Add("psbt.broadcast", [this, find_wallet](const UniValue& params) {
        PartiallySignedTransaction psbtx{DecodePsbtParam(params)};
        CMutableTransaction mtx;
        if (!FinalizeAndExtractPSBT(psbtx, mtx)) throw HostError("Unknown error processing transaction.");
        const CTransactionRef tx{MakeTransactionRef(mtx)};
        // What it pays, decoded here: confirmed on the desktop before it goes to the network (audit 7 #1).
        auto w{find_wallet(params)};
        Window::ConfirmDialog dialog;
        dialog.title = "Broadcast a transaction";
        dialog.message = "The transaction goes to the network and cannot be taken back.\n\nBroadcast it only if you asked for it.";
        dialog.accept = "Broadcast";
        dialog.kind = "broadcast";
        dialog.rows = PsbtRows(w.get(), psbtx);
        if (!Ask(dialog).accepted) throw HostError(BRIDGE_CANCELLED, "The transaction was not broadcast: it was declined.");
        std::string err_string;
        const node::TransactionError error{m_node().broadcastTransaction(tx, node::DEFAULT_MAX_RAW_TX_FEE_RATE.GetFeePerK(), err_string)};
        if (error != node::TransactionError::OK) {
            throw HostError(strprintf("Transaction broadcast failed: %s%s", common::TransactionErrorString(error).translated, err_string.empty() ? "" : " (" + err_string + ")"));
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("txid", tx->GetHash().GetHex());
        return out;
    });
}

WalletApi::~WalletApi() = default;

void WalletApi::Add(const std::string& name, Method method)
{
    m_methods[name] = std::move(method);
}

std::string WalletApi::NewToken()
{
    LOCK(m_mutex);
    return strprintf("t%d", m_next_token++);
}

std::vector<std::string> WalletApi::Methods() const
{
    std::vector<std::string> out;
    for (const auto& [name, fn] : m_methods) out.push_back(name);
    return out;
}

UniValue WalletApi::Call(const std::string& method, const UniValue& params)
{
    const auto it{m_methods.find(method)};
    if (it == m_methods.end()) throw HostError(BRIDGE_METHOD_NOT_FOUND, strprintf("No host method %s", method));
    try {
        return it->second(params);
    } catch (const HostError&) {
        throw;
    } catch (const UniValue& e) {
        throw HostError(e.isObject() && e.find_value("message").isStr() ? e.find_value("message").get_str() : e.write());
    } catch (const std::exception& e) {
        throw HostError(e.what());
    }
}

void WalletApi::Register(Bridge& bridge)
{
    // Loading, restoring, migrating and backing up a wallet can take minutes (a rescan, a large file), and what asks the
    // user on the desktop waits for the answer: apart from the quick calls of the pages.
    static const std::set<std::string> SLOW{"wallets.create", "wallets.open", "wallets.restore", "wallets.migrate", "wallets.restoreMigrate",
                                            "wallet.backup", "wallet.exportWatchOnly", "wallet.encrypt", "wallets.signers",
                                            "wallet.displayAddress", "wallet.send.commit", "wallet.bump.commit", "wallet.signMessage",
                                            "wallet.psbt.sign", "psbt.broadcast", "wallet.unlock", "wallet.newAddress", "wallet.addressBook.edit"};
    for (const auto& [name, fn] : m_methods) {
        bridge.RegisterHost(name, [this, name](const UniValue& params) { return Call(name, params); }, SLOW.contains(name) ? Bridge::Thread::LONG : Bridge::Thread::WORKER);
    }
}

} // namespace webui
