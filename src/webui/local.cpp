// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of zSide (src/webui/app/src/chain/local.ts): the RPC methods of the shielded
// pool and the shielded wallet they call (src/rpc/shielded.cpp, src/wallet/rpc/shielded.cpp), and the one
// host method they need: the viewing key, shown only after the desktop asked (zside.viewingkey.export).

#include <webui/local.h>

#include <interfaces/node.h>
#include <tinyformat.h>
#include <uint256.h>
#include <univalue.h>
#include <util/moneystr.h>

namespace webui::local {

std::map<std::string, Bridge::Thread> RpcMethods()
{
    return {
        // The pool of the chain: read only.
        {"getshieldedinfo", Bridge::Thread::WORKER},
        // What the shielded account of the wallet holds and did: read only.
        {"getshieldedbalance", Bridge::Thread::WORKER},
        {"listshieldednotes", Bridge::Thread::WORKER},
        {"listshieldedtransactions", Bridge::Thread::WORKER},
        {"listmeltcast", Bridge::Thread::WORKER},
        // What a payment out of the pool would pay (the fee, what each recipient gets): read only, no keys. The
        // pages ask it first, show it, and give its fee to the payment (audit 7 E-ZS3).
        {"estimateshieldedfee", Bridge::Thread::WORKER},
        // An address of the account (a new one looks unrelated to the others); the first call of a locked
        // wallet whose account is not set up yet sets it up with the keys.
        {"getshieldedaddress", Bridge::Thread::WORKER},
        // Not exportshieldedviewingkey: the node answers it even with the wallet locked, and the key shows
        // every private payment of the account. The page asks for it through zside.viewingkey.export, which
        // the desktop confirms first (audit port-2 #3).
        // A viewing key watched in a wallet without keys: the blocks are scanned again, which takes long.
        {"importshieldedviewingkey", Bridge::Thread::LONG},
        // Payments out of the pool and into it, and the plans of melt & cast: confirmed on the desktop
        // (ConfirmedRpcMethods, below; on the long lane, as each proves a bundle). Cancelling a plan spends nothing.
        {"cancelmeltcast", Bridge::Thread::WORKER},
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // getshieldedaddress sets the account up from the keys the first time (the passphrase asked by the host).
    return {"getshieldedaddress"};
}

Window::ConfirmDialog ViewingKeyConfirmation(const std::string& wallet)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Show the viewing key";
    dialog.message = strprintf("Wallet: %s\n\n"
                               "zSide asks to show the viewing key of the shielded account of this wallet. Whoever has it sees "
                               "every payment the account receives and makes, past and future, with the amounts and the memos. "
                               "It cannot spend, and it cannot be taken back once given.\n\nShow it only if you asked for it.",
                               guard::OneLine(wallet, 64));
    dialog.accept = "Show the key";
    return dialog;
}

namespace {
/** An amount of a parameter (a number, or a string of one), in satoshis; nullopt for anything else. */
std::optional<CAmount> CoinsOf(const UniValue& v)
{
    std::optional<CAmount> amount;
    if (v.isNum()) amount = ParseMoney(v.getValStr());
    if (v.isStr()) amount = ParseMoney(v.get_str());
    if (!amount || *amount < 0 || !MoneyRange(*amount)) return std::nullopt;
    return amount;
}

/**
 * sendshielded [amounts, memo, fee]: each recipient whole, its amount, the fee and the total. The fee must be given (the
 * pages give the one estimateshieldedfee said): what the dialog shows is what is paid (audit 7 E-ZS3).
 */
std::optional<Window::ConfirmDialog> SendShielded(const confirm::Call& call)
{
    const UniValue amounts{call.Arg(0)};
    const UniValue memo{call.Arg(1)};
    const auto fee{CoinsOf(call.Arg(2))};
    if (!amounts.isObject() || amounts.empty() || !fee) {
        throw HostError(BRIDGE_INVALID_REQUEST, "A private payment is [{address: amount}, memo, fee] (the fee of estimateshieldedfee)");
    }
    Window::ConfirmDialog dialog;
    dialog.title = "Send privately";
    dialog.message = "The wallet pays out of its shielded account. A shielded recipient alone sees the amount and the memo; a "
                     "transparent address and its amount are public (not where the coins came from).\n\nAccept only if you asked for it.";
    dialog.accept = "Send";
    dialog.kind = "rpc:sendshielded";
    dialog.rows = confirm::WalletRows(call.wallet);
    CAmount total{*fee};
    for (const std::string& address : amounts.getKeys()) {
        const auto amount{CoinsOf(amounts[address])};
        if (!amount || *amount == 0 || !MoneyRange(total + *amount)) throw HostError(BRIDGE_INVALID_REQUEST, "An amount of the payment is not one");
        total += *amount;
        dialog.rows.push_back(confirm::Destination("To", address));
        dialog.rows.push_back(confirm::Amount("Amount", *amount));
    }
    if (memo.isStr() && !memo.get_str().empty()) dialog.rows.push_back(confirm::Text("Memo (to the shielded recipients)", memo.get_str()));
    dialog.rows.push_back(confirm::Amount("Fee", *fee));
    dialog.rows.push_back(confirm::Total("Total out of the shielded balance", total));
    return dialog;
}

/**
 * unshieldcoins [amount, address, subtractfeefromamount, fee]: the address (or a new one of the wallet), the amount, the fee,
 * and what the address gets when the fee comes out of it. An amount and the fee must be given (the pages give those of
 * estimateshieldedfee, also for "all of it"): what the dialog shows is what is paid (audit 7 E-ZS3).
 */
std::optional<Window::ConfirmDialog> UnshieldCoins(const confirm::Call& call)
{
    const auto amount{CoinsOf(call.Arg(0))};
    const UniValue to{call.Arg(1)};
    const UniValue subtract_value{call.Arg(2)};
    const auto fee{CoinsOf(call.Arg(3))};
    const bool subtract{subtract_value.isBool() && subtract_value.get_bool()};
    if (!amount || *amount == 0 || !fee || !(to.isNull() || to.isStr()) || !(subtract_value.isNull() || subtract_value.isBool()) || !MoneyRange(*amount + *fee)) {
        throw HostError(BRIDGE_INVALID_REQUEST, "An unshielding is [amount, address?, subtractfeefromamount?, fee] (the amount and the fee of estimateshieldedfee)");
    }
    if (subtract && *amount <= *fee) throw HostError(BRIDGE_INVALID_REQUEST, "The amount does not pay the fee");
    Window::ConfirmDialog dialog;
    dialog.title = "Unshield coins";
    dialog.message = "The wallet moves coins out of its shielded account to the address shown. The amount and the address will be "
                     "public; where the coins came from will not.\n\nAccept only if you asked for it.";
    dialog.accept = "Unshield";
    dialog.kind = "rpc:unshieldcoins";
    dialog.rows = confirm::WalletRows(call.wallet);
    dialog.rows.push_back(to.isStr() && !to.get_str().empty() ? confirm::Destination("To", to.get_str()) : confirm::Text("To", "A new transparent address of this wallet"));
    dialog.rows.push_back(confirm::Amount("Amount", *amount));
    dialog.rows.push_back(confirm::Amount("Fee", *fee));
    if (subtract) {
        dialog.rows.push_back(confirm::Total("The address gets (the fee taken out)", *amount - *fee));
    } else {
        dialog.rows.push_back(confirm::Total("Total out of the shielded balance", *amount + *fee));
    }
    return dialog;
}
} // namespace

std::map<std::string, confirm::Method> ConfirmedRpcMethods()
{
    // What the pages do that pays or signs: never the page's to call by itself. The host shows each call on the desktop, its
    // destinations whole first, then its amounts, then the rest of its parameters, with the passphrase of a locked wallet
    // typed there, and runs it with the keys unlocked for it alone (confirm.h, audit 7 #1).
    return {
        // A payment and an unshielding show their fee (given, as estimateshieldedfee said it) and what each recipient gets.
        {"sendshielded", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, SendShielded, /*fee_unknown=*/false, 3}},
        {"shieldcoins", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Shield coins", "Shield", "The wallet moves coins into its shielded account.", {"address"}, {"amount", "amounts"}), /*fee_unknown=*/true}},
        {"unshieldcoins", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, UnshieldCoins, /*fee_unknown=*/false, 1}},
        {"bumpshieldedfee", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Increase the fee", "Increase the fee", "The wallet replaces a shielded transaction with one that pays a higher fee.", {}, {"fee"}), /*fee_unknown=*/true}},
        {"meltcoins", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Melt coins", "Melt", "The wallet plans to shield these coins, each whole, at blocks it chooses.", {}, {"amount"}), /*fee_unknown=*/true}},
        {"castcoins", {confirm::Method::Kind::SPEND, Bridge::Thread::LONG, confirm::Generic("Cast coins", "Cast", "The wallet plans to pay out of its shielded account in bills, at the windows it chooses.", {}, {"amount"}), /*fee_unknown=*/true}},
    };
}

void RegisterHost(Bridge& bridge, NodeFn node, guard::ConfirmFn confirm, confirm::KeysFn keys)
{
    (void)keys;
    // zside.viewingkey.export {wallet}: the viewing key of the account of a wallet, after the user said yes on
    // the desktop (outside the page); a no answers BRIDGE_CANCELLED and the node is not asked.
    bridge.RegisterHost("zside.viewingkey.export", [node, confirm](const UniValue& params) -> UniValue {
        const UniValue& wallet{params.find_value("wallet")};
        if (!wallet.isStr() || wallet.get_str().size() > 1024) throw HostError(BRIDGE_INVALID_REQUEST, "wallet: the name of a wallet");
        if (!confirm || !confirm(ViewingKeyConfirmation(wallet.get_str()))) {
            throw HostError(BRIDGE_CANCELLED, "The viewing key was not shown: it was declined.");
        }
        return node().executeRpc("exportshieldedviewingkey", UniValue{UniValue::VARR}, WalletUri(wallet.get_str()));
    });
}

std::vector<std::string> CoinbaseRoles(interfaces::Chain& chain, const uint256& block)
{
    // For example (FastBlocks): {"staked", "bmm"} for a coinbase whose first output is the producer's share
    // and whose second is the fee pot paid to a BMM bidder.
    (void)chain;
    (void)block;
    return {};
}

} // namespace webui::local
