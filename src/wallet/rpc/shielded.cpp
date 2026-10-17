// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <key_io.h>
#include <random.h>
#include <rpc/util.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <wallet/meltcast.h>
#include <wallet/rpc/util.h>
#include <wallet/shielded.h>
#include <wallet/wallet.h>
#include <zside/shielded.h>

#include <univalue.h>

#include <algorithm>

namespace wallet {
namespace {
//! The most transactions listshieldedtransactions returns at once (audit 7 E-ZS5).
constexpr int MAX_SHIELDED_TXS_PAGE{1000};

/**
 * The wallet of the request, its shielded account set up.
 * @param[in] rescan  scan again first the blocks the wallet scanned without the account, if any: for
 *                    what spends or plans with its notes. A method that only reads does not wait for
 *                    that (it may take minutes): the wallets' background thread does it (StartWallets),
 *                    and the method says what is known so far.
 */
std::shared_ptr<CWallet> ReadyWallet(const JSONRPCRequest& request, bool unlocked, bool rescan = true)
{
    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return nullptr;
    pwallet->BlockUntilSyncedToCurrentChain();
    if (unlocked) EnsureWalletIsUnlocked(*pwallet);
    {
        LOCK(pwallet->cs_wallet);
        // A locked wallet still reads what its keys see (those of a retired account included).
        if (!SetupShieldedAccount(*pwallet) && ShieldedViewingKeys(pwallet->m_shielded).empty()) {
            throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "The shielded account is not set up yet: unlock the wallet once (walletpassphrase)");
        }
    }
    // Set up after the wallet scanned blocks without it, the account has them scanned again first.
    if (rescan) ShieldedRescan(*pwallet);
    return pwallet;
}

OrchardMemo ParseMemo(const UniValue& value)
{
    const auto memo{MemoFromText(value.isNull() ? "" : value.get_str())};
    if (!memo) throw JSONRPCError(RPC_INVALID_PARAMETER, "A memo is at most 512 bytes of text");
    return *memo;
}

UniValue TxResult(const std::variant<CTransactionRef, std::string>& result)
{
    if (const auto* error{std::get_if<std::string>(&result)}) throw JSONRPCError(RPC_WALLET_ERROR, *error);
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", std::get<CTransactionRef>(result)->GetHash().GetHex());
    return obj;
}

const RPCResult TXID_RESULT{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::STR_HEX, "txid", "The transaction"}}};

UniValue JobToJSON(const CWallet& wallet, const MeltCastJob& job) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("id", job.id);
    obj.pushKV("kind", job.kind == MeltCastJob::MELT ? "melt" : "cast");
    obj.pushKV("amount", ValueFromAmount(job.amount));
    if (job.kind == MeltCastJob::MELT) obj.pushKV("coin", strprintf("%s:%d", job.coin.hash.GetHex(), job.coin.n));
    const int height{wallet.GetLastBlockHeight()};
    int at{std::max(job.due, height + 1)};
    if (job.kind == MeltCastJob::CAST) at = meltcast::NextCastWindow(job.bill, std::max(job.due, height));
    if (job.Sent()) {
        obj.pushKV("status", "sent");
        obj.pushKV("txid", job.txid.GetHex());
        const CWalletTx* wtx{wallet.GetWalletTx(Txid::FromUint256(job.txid))};
        obj.pushKV("confirmations", wtx ? wallet.GetTxDepthInMainChain(*wtx) : 0);
    } else {
        obj.pushKV("status", job.error.empty() ? "planned" : "waiting");
        obj.pushKV("height", at);
        if (!job.error.empty()) obj.pushKV("error", job.error);
    }
    return obj;
}

const RPCResult JOB_RESULT{RPCResult::Type::OBJ, "", "", {
    {RPCResult::Type::NUM, "id", "The step of the plan"},
    {RPCResult::Type::STR, "kind", "\"melt\" (a coin into the pool) or \"cast\" (a bill out of it)"},
    {RPCResult::Type::STR_AMOUNT, "amount", "The coin's value, or the bill's"},
    {RPCResult::Type::STR, "coin", /*optional=*/true, "Melt: the coin (txid:vout)"},
    {RPCResult::Type::STR, "status", "\"planned\", \"waiting\" (it tried, see error) or \"sent\""},
    {RPCResult::Type::NUM, "height", /*optional=*/true, "Not sent: the tip height from which it goes (a cast: its bill's next window)"},
    {RPCResult::Type::STR, "error", /*optional=*/true, "Why it did not go yet"},
    {RPCResult::Type::STR_HEX, "txid", /*optional=*/true, "Sent: the transaction"},
    {RPCResult::Type::NUM, "confirmations", /*optional=*/true, "Sent: its confirmations"},
}};

UniValue JobsToJSON(const CWallet& wallet, const std::variant<std::vector<MeltCastJob>, std::string>& planned) EXCLUSIVE_LOCKS_REQUIRED(wallet.cs_wallet)
{
    if (const auto* error{std::get_if<std::string>(&planned)}) throw JSONRPCError(RPC_WALLET_ERROR, *error);
    UniValue result(UniValue::VARR);
    for (const MeltCastJob& job : std::get<std::vector<MeltCastJob>>(planned)) result.push_back(JobToJSON(wallet, job));
    return result;
}

} // namespace

RPCMethod getshieldedaddress()
{
    return RPCMethod{
        "getshieldedaddress",
        "An address of the wallet's shielded account, to be paid at privately. The default one is always the same;\n"
        "a new one looks unrelated to it and to every other, yet pays the same account.",
        {
            {"new", RPCArg::Type::BOOL, RPCArg::Default{false}, "A new address instead of the default one"},
        },
        RPCResult{RPCResult::Type::STR, "", "The address"},
        RPCExamples{HelpExampleCli("getshieldedaddress", "") + HelpExampleCli("getshieldedaddress", "true")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    LOCK(pwallet->cs_wallet);
    // Not one of an account that may be of another chain (set up before the chain was kept with its keys): checked once unlocked.
    if (!ShieldedAccountOfThisChain(pwallet->m_shielded)) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "The shielded account is to be checked against this chain first: unlock the wallet once (walletpassphrase)");
    }
    // Diversified addresses: any index gives an address of the account; random ones cannot be linked.
    const uint64_t index{!request.params[0].isNull() && request.params[0].get_bool() ? FastRandomContext{}.rand64() : 0};
    const auto address{ShieldedAddress(*pwallet, index)};
    if (!address) throw JSONRPCError(RPC_WALLET_ERROR, "Cannot make an address");
    return EncodeShieldedAddress(*address);
},
    };
}

RPCMethod getshieldedbalance()
{
    return RPCMethod{
        "getshieldedbalance",
        "What the wallet's shielded account holds.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_AMOUNT, "available", "In notes in blocks"},
            {RPCResult::Type::STR_AMOUNT, "spendable", strprintf("The part of available in notes %d blocks deep at least: what a payment can spend now "
                                                                 "(a payment spends only notes that deep, so that its anchor does not name a note's block)", ANCHOR_CONFIRMATIONS)},
            {RPCResult::Type::STR_AMOUNT, "pending", "In notes waiting for a block"},
            {RPCResult::Type::NUM, "notes", "How many notes"},
            {RPCResult::Type::STR_AMOUNT, "watchonly", /*optional=*/true, "In notes of an account the wallet set up for another chain whose keys it cannot make (not in the above): watched, not spendable"},
            {RPCResult::Type::NUM, "rescanning_from", /*optional=*/true, "The account was set up after the wallet scanned blocks without it: those from this height "
                                                                         "are being scanned again, in the background, and what they paid it is not counted yet"},
        }},
        RPCExamples{HelpExampleCli("getshieldedbalance", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    LOCK(pwallet->cs_wallet);
    const ShieldedBalance balance{GetShieldedBalance(*pwallet)};
    UniValue result(UniValue::VOBJ);
    result.pushKV("available", ValueFromAmount(balance.available));
    result.pushKV("spendable", ValueFromAmount(balance.spendable));
    result.pushKV("pending", ValueFromAmount(balance.pending));
    result.pushKV("notes", balance.notes);
    if (balance.watch_only > 0) result.pushKV("watchonly", ValueFromAmount(balance.watch_only));
    if (pwallet->m_shielded.rescan_from) result.pushKV("rescanning_from", *pwallet->m_shielded.rescan_from);
    return result;
},
    };
}

RPCMethod listshieldednotes()
{
    return RPCMethod{
        "listshieldednotes",
        "The notes of the wallet's shielded account.",
        {
            {"include_spent", RPCArg::Type::BOOL, RPCArg::Default{false}, "Also the notes spent"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", "The transaction that made it"},
            {RPCResult::Type::NUM, "action", "Its action in the bundle"},
            {RPCResult::Type::STR_AMOUNT, "amount", ""},
            {RPCResult::Type::STR, "memo", "Its memo, as text"},
            {RPCResult::Type::BOOL, "change", "Whether it is change"},
            {RPCResult::Type::NUM, "confirmations", ""},
            {RPCResult::Type::STR_HEX, "spent_by", /*optional=*/true, "The transaction that spends it"},
            {RPCResult::Type::BOOL, "retired", /*optional=*/true, "Of an account the wallet set up for another chain (true if so)"},
            {RPCResult::Type::BOOL, "watchonly", /*optional=*/true, "The wallet cannot make its key: it cannot be spent (true if so)"},
            {RPCResult::Type::BOOL, "viewonly", /*optional=*/true, "Of a viewing key imported (importshieldedviewingkey) (true if so)"},
        }}}},
        RPCExamples{HelpExampleCli("listshieldednotes", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    const bool include_spent{!request.params[0].isNull() && request.params[0].get_bool()};
    LOCK(pwallet->cs_wallet);
    UniValue result(UniValue::VARR);
    for (const NoteStatus& s : ShieldedNotes(*pwallet)) {
        if (s.spent_by && !include_spent) continue;
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", s.txid.GetHex());
        obj.pushKV("action", s.action);
        obj.pushKV("amount", ValueFromAmount(s.note->value));
        obj.pushKV("memo", MemoText(s.note->memo));
        obj.pushKV("change", s.note->internal);
        obj.pushKV("confirmations", s.confirmations);
        if (s.spent_by) obj.pushKV("spent_by", s.spent_by->GetHex());
        if (s.note->retired >= 0 && pwallet->m_shielded.retired.at(s.note->retired).version == KEY_VIEW_ONLY) {
            obj.pushKV("viewonly", true);
        } else if (s.note->retired >= 0) {
            obj.pushKV("retired", true);
        }
        if (!ShieldedNoteSpendable(pwallet->m_shielded, *s.note)) obj.pushKV("watchonly", true);
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod shieldcoins()
{
    return RPCMethod{
        "shieldcoins",
        "Move transparent coins of the wallet into a shielded address: the wallet's own, unless another is given." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A shielded address (default: the wallet's)"},
            {"memo", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A memo, at most 512 bytes, that only the recipient reads"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("shieldcoins", "10")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/true)};
    if (!pwallet) return UniValue::VNULL;
    const CAmount amount{AmountFromValue(request.params[0])};
    OrchardAddress to;
    if (request.params[1].isNull()) {
        LOCK(pwallet->cs_wallet);
        to = *ShieldedAddress(*pwallet, 0);
    } else {
        const auto address{DecodeShieldedAddress(request.params[1].get_str())};
        if (!address) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not a shielded address");
        to = *address;
    }
    return TxResult(ShieldCoins(*pwallet, amount, to, ParseMemo(request.params[2])));
},
    };
}

RPCMethod sendshielded()
{
    return RPCMethod{
        "sendshielded",
        "Pay out of the wallet's shielded account, privately: to shielded addresses (with a memo each can read), or to\n"
        "transparent ones (which unshields: the amount and the address are then public, not where they came from).\n"
        "The change goes back to the shielded account. The transaction has no transparent inputs.\n"
        "It spends only notes 3 blocks deep at least, and leaves what casts planned (castcoins) hold back." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The addresses and how much each",
            {
                {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A shielded or transparent address, and the amount"},
            }},
            {"memo", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A memo for the shielded recipients, at most 512 bytes"},
            {"fee", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The fee (default: at the wallet's fee rate); refused above -maxtxfee"},
        },
        TXID_RESULT,
        RPCExamples{HelpExampleCli("sendshielded", "'{\"zo1...\": 1.5}' \"for the coffee\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/true)};
    if (!pwallet) return UniValue::VNULL;
    const OrchardMemo memo{ParseMemo(request.params[1])};
    std::vector<ShieldedRecipient> recipients;
    for (const std::string& key : request.params[0].getKeys()) {
        ShieldedRecipient r;
        r.amount = AmountFromValue(request.params[0][key]);
        r.memo = memo;
        if (const auto shielded{DecodeShieldedAddress(key)}) {
            r.shielded = *shielded;
        } else {
            const CTxDestination dest{DecodeDestination(key)};
            if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not an address: " + key);
            r.script = GetScriptForDestination(dest);
        }
        recipients.push_back(std::move(r));
    }
    if (recipients.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "No recipients");
    const std::optional<CAmount> fee{request.params[2].isNull() ? std::nullopt : std::optional{AmountFromValue(request.params[2])}};
    const CAmount hold_back{WITH_LOCK(pwallet->cs_wallet, return CastReserved(*pwallet))};
    return TxResult(SendShielded(*pwallet, recipients, fee, /*subtract_fee=*/false, hold_back));
},
    };
}

RPCMethod estimateshieldedfee()
{
    return RPCMethod{
        "estimateshieldedfee",
        "What sendshielded, or unshieldcoins, would pay now: the fee, the notes it spends, and what each recipient gets.\n"
        "Nothing is made, and the wallet may be locked. Give the fee returned to sendshielded or unshieldcoins (fee),\n"
        "and the fee shown is the fee paid.",
        {
            {"amounts", RPCArg::Type::OBJ_USER_KEYS, RPCArg::Optional::NO, "The addresses and how much each",
            {
                {"address", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "A shielded or transparent address, and the amount (\"all\": one transparent address, as unshieldcoins all)"},
            }},
            {"subtractfeefromamount", RPCArg::Type::BOOL, RPCArg::Default{false}, "Take the fee out of the amount (one transparent recipient; always with \"all\")"},
            {"fee", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The fee (default: at the wallet's fee rate)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_AMOUNT, "fee", "The fee"},
            {RPCResult::Type::NUM, "notes", "How many notes it spends"},
            {RPCResult::Type::OBJ_DYN, "amounts", "What each address gets (the fee taken out where asked)", {
                {RPCResult::Type::STR_AMOUNT, "address", ""},
            }},
            {RPCResult::Type::STR_AMOUNT, "total", "What leaves the shielded balance: the amounts and the fee"},
        }},
        RPCExamples{HelpExampleCli("estimateshieldedfee", "'{\"zo1...\": 1.5}'") + HelpExampleCli("estimateshieldedfee", "'{\"zs1...\": \"all\"}'")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    bool subtract{!request.params[1].isNull() && request.params[1].get_bool()};
    const std::optional<CAmount> fee{request.params[2].isNull() ? std::nullopt : std::optional{AmountFromValue(request.params[2])}};
    const CAmount hold_back{WITH_LOCK(pwallet->cs_wallet, return CastReserved(*pwallet))};
    std::vector<ShieldedRecipient> recipients;
    const auto keys{request.params[0].getKeys()};
    for (const std::string& key : keys) {
        ShieldedRecipient r;
        const UniValue& value{request.params[0][key]};
        if (value.isStr() && value.get_str() == "all") {
            if (keys.size() != 1 || DecodeShieldedAddress(key)) throw JSONRPCError(RPC_INVALID_PARAMETER, "\"all\" is for one transparent address alone");
            r.amount = WITH_LOCK(pwallet->cs_wallet, return GetShieldedBalance(*pwallet).spendable) - hold_back;
            if (r.amount <= 0) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, "Nothing in the shielded account that can be spent yet");
            subtract = true;
        } else {
            r.amount = AmountFromValue(value);
        }
        if (const auto shielded{DecodeShieldedAddress(key)}) {
            r.shielded = *shielded;
        } else {
            const CTxDestination dest{DecodeDestination(key)};
            if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not an address: " + key);
            r.script = GetScriptForDestination(dest);
        }
        recipients.push_back(std::move(r));
    }
    if (recipients.empty()) throw JSONRPCError(RPC_INVALID_PARAMETER, "No recipients");
    const auto result{EstimateShielded(*pwallet, recipients, fee, subtract, hold_back)};
    if (const auto* error{std::get_if<std::string>(&result)}) throw JSONRPCError(RPC_WALLET_ERROR, *error);
    const ShieldedEstimate& estimate{std::get<ShieldedEstimate>(result)};
    UniValue amounts(UniValue::VOBJ);
    CAmount total{estimate.fee};
    for (size_t i{0}; i < keys.size(); ++i) {
        amounts.pushKV(keys[i], ValueFromAmount(estimate.amounts[i]));
        total += estimate.amounts[i];
    }
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("fee", ValueFromAmount(estimate.fee));
    obj.pushKV("notes", static_cast<uint64_t>(estimate.notes));
    obj.pushKV("amounts", std::move(amounts));
    obj.pushKV("total", ValueFromAmount(total));
    return obj;
},
    };
}

RPCMethod bumpshieldedfee()
{
    return RPCMethod{
        "bumpshieldedfee",
        "Pay the same again with a higher fee: a transaction out of the wallet's shielded account (with no transparent\n"
        "inputs) that waits for a block is replaced by one that spends the same notes (and more if they do not cover\n"
        "the fee), pays the same recipients, and gives back less change. For one with transparent inputs, see bumpfee." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"txid", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The transaction"},
            {"fee", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The new fee (default: the least a replacement pays, or the wallet's fee rate if more)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "txid", "The new transaction"},
            {RPCResult::Type::STR_AMOUNT, "origfee", "The fee of the one it replaces"},
            {RPCResult::Type::STR_AMOUNT, "fee", "Its fee"},
        }},
        RPCExamples{HelpExampleCli("bumpshieldedfee", "\"txid\"") + HelpExampleCli("bumpshieldedfee", "\"txid\" 0.001")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/true)};
    if (!pwallet) return UniValue::VNULL;
    const Txid txid{Txid::FromUint256(ParseHashV(request.params[0], "txid"))};
    const std::optional<CAmount> fee{request.params[1].isNull() ? std::nullopt : std::optional{AmountFromValue(request.params[1])}};
    const auto fee_of{[](const CTransaction& tx) {
        std::string error;
        const auto bundle{zside::Bundle::FromTx(tx, error)};
        CAmount out{bundle ? bundle->ValueBalance() : 0};
        for (uint32_t i{0}; i < tx.vout.size(); ++i) {
            if (!bundle || i != bundle->marker_index) out -= tx.vout[i].nValue;
        }
        return out;
    }};
    CAmount old_fee{0};
    {
        LOCK(pwallet->cs_wallet);
        if (const CWalletTx* wtx{pwallet->GetWalletTx(txid)}) old_fee = fee_of(*wtx->GetTx());
    }
    const auto result{BumpShieldedFee(*pwallet, txid, fee)};
    if (const auto* error{std::get_if<std::string>(&result)}) throw JSONRPCError(RPC_WALLET_ERROR, *error);
    const CTransactionRef& tx{std::get<CTransactionRef>(result)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", tx->GetHash().GetHex());
    obj.pushKV("origfee", ValueFromAmount(old_fee));
    obj.pushKV("fee", ValueFromAmount(fee_of(*tx)));
    return obj;
},
    };
}

RPCMethod unshieldcoins()
{
    return RPCMethod{
        "unshieldcoins",
        "Move coins out of the wallet's shielded account to a transparent address: a new one of the wallet, unless\n"
        "another is given. The amount and the address are then public; where the coins came from is not.\n"
        "For amounts nobody can link to what went in, see castcoins." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much, or \"all\" for the whole shielded balance that can be spent (notes 3 blocks deep at least), less what casts planned hold back"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "A transparent address (default: a new one of the wallet)"},
            {"subtractfeefromamount", RPCArg::Type::BOOL, RPCArg::Default{false}, "Take the fee out of the amount (always with \"all\")"},
            {"fee", RPCArg::Type::AMOUNT, RPCArg::Optional::OMITTED, "The fee (default: at the wallet's fee rate; see estimateshieldedfee); refused above -maxtxfee"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::STR_HEX, "txid", "The transaction"},
            {RPCResult::Type::STR, "address", "Where the coins went"},
            {RPCResult::Type::STR_AMOUNT, "amount", "What the address receives"},
        }},
        RPCExamples{HelpExampleCli("unshieldcoins", "2.5") + HelpExampleCli("unshieldcoins", "all")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/true)};
    if (!pwallet) return UniValue::VNULL;
    const bool all{request.params[0].isStr() && request.params[0].get_str() == "all"};
    bool subtract{all || (!request.params[2].isNull() && request.params[2].get_bool())};
    CAmount amount;
    const CAmount hold_back{WITH_LOCK(pwallet->cs_wallet, return CastReserved(*pwallet))};
    if (all) {
        LOCK(pwallet->cs_wallet);
        amount = GetShieldedBalance(*pwallet).spendable - hold_back;
        if (amount <= 0) throw JSONRPCError(RPC_WALLET_INSUFFICIENT_FUNDS, strprintf("Nothing in the shielded account that can be spent yet (notes %d blocks deep, less what casts planned hold back)", ANCHOR_CONFIRMATIONS));
    } else {
        amount = AmountFromValue(request.params[0]);
    }
    CTxDestination dest;
    if (request.params[1].isNull()) {
        LOCK(pwallet->cs_wallet);
        auto fresh{pwallet->GetNewDestination(OutputType::BECH32, "unshielded")};
        if (!fresh) throw JSONRPCError(RPC_WALLET_KEYPOOL_RAN_OUT, util::ErrorString(fresh).original);
        dest = *fresh;
    } else {
        if (DecodeShieldedAddress(request.params[1].get_str())) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "A shielded address: to pay it, use sendshielded");
        dest = DecodeDestination(request.params[1].get_str());
        if (!IsValidDestination(dest)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not an address");
    }
    ShieldedRecipient r;
    r.script = GetScriptForDestination(dest);
    r.amount = amount;
    const std::optional<CAmount> fee{request.params[3].isNull() ? std::nullopt : std::optional{AmountFromValue(request.params[3])}};
    const auto result{SendShielded(*pwallet, {r}, fee, subtract, hold_back)};
    if (const auto* error{std::get_if<std::string>(&result)}) throw JSONRPCError(RPC_WALLET_ERROR, *error);
    const CTransactionRef& tx{std::get<CTransactionRef>(result)};
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", tx->GetHash().GetHex());
    obj.pushKV("address", EncodeDestination(dest));
    for (const CTxOut& out : tx->vout) {
        if (out.scriptPubKey == r.script) obj.pushKV("amount", ValueFromAmount(out.nValue));
    }
    return obj;
},
    };
}

RPCMethod listshieldedtransactions()
{
    return RPCMethod{
        "listshieldedtransactions",
        "The transactions of the wallet's shielded account, newest first: what each received, spent and paid.",
        {
            {"count", RPCArg::Type::NUM, RPCArg::Default{50}, strprintf("How many, at most %d (more is read as %d)", MAX_SHIELDED_TXS_PAGE, MAX_SHIELDED_TXS_PAGE)},
            {"skip", RPCArg::Type::NUM, RPCArg::Default{0}, "How many of the newest to skip (pages)"},
        },
        RPCResult{RPCResult::Type::ARR, "", "", {{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "txid", ""},
            {RPCResult::Type::NUM_TIME, "time", "When the wallet saw it"},
            {RPCResult::Type::NUM, "confirmations", ""},
            {RPCResult::Type::STR_AMOUNT, "received", "In notes to the account (change included)"},
            {RPCResult::Type::STR_AMOUNT, "spent", "In notes of the account it spent"},
            {RPCResult::Type::STR_AMOUNT, "shielded", "Put into the pool from transparent coins of the wallet"},
            {RPCResult::Type::STR_AMOUNT, "unshielded", "Taken out of the pool, to transparent outputs and the fee"},
            {RPCResult::Type::ARR, "memos", "The memos it brought the account", {{RPCResult::Type::STR, "", ""}}},
            {RPCResult::Type::ARR, "payments", "What it paid to shielded addresses", {{RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR, "address", ""},
                {RPCResult::Type::STR_AMOUNT, "amount", ""},
                {RPCResult::Type::STR, "memo", ""},
                {RPCResult::Type::BOOL, "to_self", "To the account itself"},
            }}}},
        }}}},
        RPCExamples{HelpExampleCli("listshieldedtransactions", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    // A page at most: each one is read again (its bundle parsed) under the wallet's lock.
    const int count{std::clamp(request.params[0].isNull() ? 50 : request.params[0].getInt<int>(), 0, MAX_SHIELDED_TXS_PAGE)};
    const int skip{request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
    if (skip < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "skip must not be negative");
    LOCK(pwallet->cs_wallet);
    struct Entry {
        const CWalletTx* wtx;
        CAmount received{0}, spent{0};
        std::vector<std::string> memos;
    };
    std::map<Txid, Entry> entries;
    for (const auto& [nf, note] : pwallet->m_shielded.notes) {
        // Each transaction that carries it received it (one of them, at most, is mined).
        for (const auto& [carrier, action] : note.carriers) {
            const auto it{pwallet->mapWallet.find(carrier)};
            if (it == pwallet->mapWallet.end()) continue;
            Entry& e{entries.try_emplace(carrier, Entry{&it->second}).first->second};
            e.received += note.value;
            if (!note.internal) {
                if (const std::string text{MemoText(note.memo)}; !text.empty()) e.memos.push_back(text);
            }
        }
        const auto range{pwallet->m_shielded.spends.equal_range(nf)};
        for (auto spend{range.first}; spend != range.second; ++spend) {
            const auto stx{pwallet->mapWallet.find(spend->second)};
            if (stx == pwallet->mapWallet.end()) continue;
            entries.try_emplace(spend->second, Entry{&stx->second}).first->second.spent += note.value;
        }
    }
    for (const auto& [key, payment] : pwallet->m_shielded.payments) {
        if (const auto it{pwallet->mapWallet.find(key.first)}; it != pwallet->mapWallet.end()) entries.try_emplace(key.first, Entry{&it->second});
    }
    std::vector<const std::pair<const Txid, Entry>*> sorted;
    for (const auto& entry : entries) sorted.push_back(&entry);
    std::sort(sorted.begin(), sorted.end(), [](const auto* a, const auto* b) { return a->second.wtx->GetTxTime() > b->second.wtx->GetTxTime(); });
    UniValue result(UniValue::VARR);
    for (size_t i{static_cast<size_t>(skip)}; i < sorted.size(); ++i) {
        const auto* entry{sorted[i]};
        if (result.size() >= static_cast<size_t>(count)) break;
        const auto& [txid, e] = *entry;
        const CTransaction& tx{*e.wtx->GetTx()};
        std::string error;
        const auto bundle{zside::Bundle::FromTx(tx, error)};
        const CAmount balance{bundle ? bundle->ValueBalance() : 0};
        UniValue obj(UniValue::VOBJ);
        obj.pushKV("txid", txid.GetHex());
        obj.pushKV("time", e.wtx->GetTxTime());
        obj.pushKV("confirmations", pwallet->GetTxDepthInMainChain(*e.wtx));
        obj.pushKV("received", ValueFromAmount(e.received));
        obj.pushKV("spent", ValueFromAmount(e.spent));
        obj.pushKV("shielded", ValueFromAmount(balance < 0 && pwallet->IsFromMe(tx) ? -balance : 0));
        obj.pushKV("unshielded", ValueFromAmount(balance > 0 && e.spent > 0 ? balance : 0));
        UniValue memos(UniValue::VARR);
        for (const std::string& memo : e.memos) memos.push_back(memo);
        obj.pushKV("memos", std::move(memos));
        UniValue payments(UniValue::VARR);
        for (auto it{pwallet->m_shielded.payments.lower_bound({txid, 0})}; it != pwallet->m_shielded.payments.end() && it->first.first == txid; ++it) {
            const ShieldedPayment& p{it->second};
            // Change is not a payment.
            const int scope{ShieldedAddressScope(pwallet->m_shielded, p.recipient)};
            if (scope == 2) continue;
            UniValue pay(UniValue::VOBJ);
            pay.pushKV("address", EncodeShieldedAddress(p.recipient));
            pay.pushKV("amount", ValueFromAmount(p.value));
            pay.pushKV("memo", MemoText(p.memo));
            pay.pushKV("to_self", scope == 1);
            payments.push_back(std::move(pay));
        }
        obj.pushKV("payments", std::move(payments));
        result.push_back(std::move(obj));
    }
    return result;
},
    };
}

RPCMethod meltcoins()
{
    return RPCMethod{
        "meltcoins",
        "Melt: plan to shield every transparent coin of the wallet (in blocks, not locked), each one alone and whole,\n"
        "at a random height over the next blocks, one coin a block at most, so that no two of them go in one transaction\n"
        "or one block. Their timing can still hint that they are one wallet's, on a quiet chain: use a large spread.\n"
        "The coins are locked until then. The wallet carries out the plan as the blocks come: it must be running, and unlocked." +
        HELP_REQUIRING_PASSPHRASE,
        {
            {"spread", RPCArg::Type::NUM, RPCArg::Default{36}, strprintf("Over how many blocks the coins go (at most %d; more if there are more coins)", meltcast::MAX_SPREAD)},
        },
        RPCResult{RPCResult::Type::ARR, "", "The steps planned", {JOB_RESULT}},
        RPCExamples{HelpExampleCli("meltcoins", "") + HelpExampleCli("meltcoins", "144")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/true)};
    if (!pwallet) return UniValue::VNULL;
    const auto planned{ScheduleMelt(*pwallet, request.params[0].isNull() ? 36 : request.params[0].getInt<int>())};
    LOCK(pwallet->cs_wallet);
    return JobsToJSON(*pwallet, planned);
},
    };
}

RPCMethod castcoins()
{
    return RPCMethod{
        "castcoins",
        strprintf("Cast: plan to unshield an amount as standard bills, %s CHN times a power of two, each to a new transparent\n"
        "address of the wallet, at the heights every wallet uses for that bill (a window of %d blocks, once a period).\n"
        "What is below the smallest bill stays in the pool. The fees come out of the pool too: %s a bill, the same in every\n"
        "wallet. A bill spends notes %d blocks deep at least (not before; a melt just made waits). A wallet whose notes are\n"
        "fragmented spends more than one note for a bill, which shows; and bills of one cast can still be summed up on a\n"
        "quiet chain: use a larger spread.\n"
        "The wallet carries out the plan as the blocks come: it must be running, and unlocked, then.",
        FormatMoney(meltcast::BILL_UNIT), meltcast::WINDOW, FormatMoney(meltcast::CastFee()), ANCHOR_CONFIRMATIONS),
        {
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "How much"},
            {"spread", RPCArg::Type::NUM, RPCArg::Default{1}, strprintf("Each bill goes at one of its next spread + 1 windows, at random (spread at most %d)", meltcast::MAX_SPREAD)},
        },
        RPCResult{RPCResult::Type::ARR, "", "The steps planned, a bill each", {JOB_RESULT}},
        RPCExamples{HelpExampleCli("castcoins", "1.5")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false)};
    if (!pwallet) return UniValue::VNULL;
    const auto planned{ScheduleCast(*pwallet, AmountFromValue(request.params[0]), request.params[1].isNull() ? 1 : request.params[1].getInt<int>())};
    LOCK(pwallet->cs_wallet);
    return JobsToJSON(*pwallet, planned);
},
    };
}

RPCMethod listmeltcast()
{
    return RPCMethod{
        "listmeltcast",
        "The melt/cast plan of the wallet: what is still to go, and what went.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "", {JOB_RESULT}},
        RPCExamples{HelpExampleCli("listmeltcast", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    LOCK(pwallet->cs_wallet);
    UniValue result(UniValue::VARR);
    for (const auto& [id, job] : pwallet->m_shielded.jobs) result.push_back(JobToJSON(*pwallet, job));
    return result;
},
    };
}

RPCMethod cancelmeltcast()
{
    return RPCMethod{
        "cancelmeltcast",
        "Drop steps of the melt/cast plan that have not gone yet (a melted coin is unlocked).",
        {
            {"id", RPCArg::Type::STR, RPCArg::Optional::NO, "The step, or \"all\" for every step not gone yet"},
        },
        RPCResult{RPCResult::Type::NUM, "", "How many steps were dropped"},
        RPCExamples{HelpExampleCli("cancelmeltcast", "3") + HelpExampleCli("cancelmeltcast", "all")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false)};
    if (!pwallet) return UniValue::VNULL;
    std::vector<uint64_t> ids;
    const std::string which{request.params[0].isNum() ? request.params[0].getValStr() : request.params[0].get_str()};
    if (which == "all") {
        LOCK(pwallet->cs_wallet);
        for (const auto& [id, job] : pwallet->m_shielded.jobs) if (!job.Sent()) ids.push_back(id);
    } else {
        const auto id{ToIntegral<uint64_t>(which)};
        if (!id) throw JSONRPCError(RPC_INVALID_PARAMETER, "id must be a step number or \"all\"");
        ids.push_back(*id);
    }
    int dropped{0};
    for (const uint64_t id : ids) dropped += CancelMeltCast(*pwallet, id);
    if (ids.size() == 1 && which != "all" && dropped == 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "No such step not gone yet");
    return dropped;
},
    };
}

RPCMethod exportshieldedviewingkey()
{
    return RPCMethod{
        "exportshieldedviewingkey",
        "The full viewing key of the wallet's shielded account. Whoever holds it sees every note the account receives\n"
        "and every payment it makes, with the amounts and the memos, for good: it cannot be taken back. It cannot spend.\n"
        "Give it only to someone who is to see all of that (an auditor, a watch-only wallet of your own).",
        {},
        RPCResult{RPCResult::Type::STR, "", "The viewing key (zview1…, tzview1… on the test networks)"},
        RPCExamples{HelpExampleCli("exportshieldedviewingkey", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const auto pwallet{ReadyWallet(request, /*unlocked=*/false, /*rescan=*/false)};
    if (!pwallet) return UniValue::VNULL;
    LOCK(pwallet->cs_wallet);
    if (!ShieldedAccountOfThisChain(pwallet->m_shielded)) {
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, "The shielded account is to be checked against this chain first: unlock the wallet once (walletpassphrase)");
    }
    return EncodeShieldedViewingKey(*pwallet->m_shielded.fvk);
},
    };
}

RPCMethod importshieldedviewingkey()
{
    return RPCMethod{
        "importshieldedviewingkey",
        "Watch a full viewing key (exportshieldedviewingkey): what its account receives and pays shows in this wallet,\n"
        "watch-only (getshieldedbalance, listshieldednotes, listshieldedtransactions). Only into a wallet without private\n"
        "keys, so that it is not mixed with an account the wallet spends from. The blocks from the height given are\n"
        "scanned again for it, which may take a while.",
        {
            {"viewingkey", RPCArg::Type::STR, RPCArg::Optional::NO, "The viewing key"},
            {"height", RPCArg::Type::NUM, RPCArg::Default{0}, "Scan the blocks again from this height (the account's first payment is not before)"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {
            {RPCResult::Type::NUM, "scanned_from", "The height the blocks were scanned again from"},
        }},
        RPCExamples{HelpExampleCli("importshieldedviewingkey", "\"zview1...\"") + HelpExampleCli("importshieldedviewingkey", "\"zview1...\" 1200")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    const std::shared_ptr<CWallet> pwallet{GetWalletForJSONRPCRequest(request)};
    if (!pwallet) return UniValue::VNULL;
    const auto fvk{DecodeShieldedViewingKey(request.params[0].get_str())};
    if (!fvk) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Not a viewing key of this network");
    const int height{request.params[1].isNull() ? 0 : request.params[1].getInt<int>()};
    if (height < 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The height is negative");
    pwallet->BlockUntilSyncedToCurrentChain();
    {
        LOCK(pwallet->cs_wallet);
        if (const auto error{ImportShieldedViewingKey(*pwallet, *fvk, height)}) throw JSONRPCError(RPC_WALLET_ERROR, *error);
    }
    ShieldedRescan(*pwallet);
    UniValue result(UniValue::VOBJ);
    result.pushKV("scanned_from", height);
    return result;
},
    };
}

} // namespace wallet
