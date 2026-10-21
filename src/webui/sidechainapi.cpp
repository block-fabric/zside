// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/sidechainapi.h>

#include <webui/bridge.h>
#include <webui/chainconfig.h>

#include <chainparams.h>
#include <core_io.h>
#include <drivechain/sidechain.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/context.h>
#include <sidechain/follower.h>
#include <sidechain/mainclient.h>
#include <tinyformat.h>
#include <util/fs.h>
#include <util/moneystr.h>
#include <util/string.h>

namespace webui {
namespace {
const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string ParamStr(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return value.get_str();
}

const std::vector<std::string> METHODS{"peg.connection", "peg.call", "peg.deposit", "bmm.set", "bmm.request"};

/** An amount of coins of the mainchain the page gives as text: positive. */
CAmount ParseOffer(const UniValue& params, const std::string& key, const std::string& what)
{
    const UniValue& value{Param(params, key)};
    const std::string text{value.isStr() ? util::TrimString(value.get_str()) : value.isNum() ? value.getValStr() : std::string{}};
    const auto amount{ParseMoney(text)};
    if (!amount || *amount <= 0) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s is a positive number of coins, like 0.0001", what));
    return *amount;
}

/** An address of this chain the page gives (the fees of merged mining). */
std::string ParseOwnAddress(const UniValue& params)
{
    const std::string address{util::TrimString(ParamStr(params, "address"))};
    if (!IsValidDestinationString(address)) throw HostError(BRIDGE_INVALID_REQUEST, "This is not an address of this chain.");
    return address;
}
} // namespace

const std::set<std::string>& SidechainApi::MainchainMethods()
{
    // What the pages read of the mainchain: nothing here changes it, nor spends.
    static const std::set<std::string> methods{
        "getbestblockhash", "getblockchaininfo", "getblockcount", "getdrivechaininfo", "getsidechain",
        "getsidechainevents", "listactivesidechains", "listsidechaindeposits", "listwithdrawalbundles",
        // Its wallet: what it holds, and an address of it for a withdrawal to come to.
        "getbalances", "getnewaddress", "getwalletinfo"};
    return methods;
}

const std::set<std::string>& SidechainApi::MainchainWalletMethods()
{
    static const std::set<std::string> methods{"getbalances", "getnewaddress", "getwalletinfo"};
    return methods;
}

SidechainApi::SidechainApi(NodeFn node, guard::ConfirmFn confirm) : m_node(std::move(node)), m_confirm(std::move(confirm)) {}

void SidechainApi::Register(Bridge& bridge)
{
    for (const std::string& method : METHODS) {
        // Calls to the mainchain node go over the network, and may wait for it: apart from the quick calls.
        bridge.RegisterHost(method, [this, method](const UniValue& params) { return Call(method, params); },
                            method == "peg.connection" ? Bridge::Thread::WORKER : Bridge::Thread::LONG);
        // (bmm.* and peg.deposit wait for a dialog of the desktop: on the long lane too.)
    }
}

UniValue SidechainApi::Call(const std::string& method, const UniValue& params)
{
    // What the page gives is checked first; then the node, its follower and its connection.
    const auto follower{[&]() -> const sidechain::Follower& {
        node::NodeContext* context{m_node().context()};
        if (!context || !context->follower) throw HostError("This chain is not running as a sidechain.");
        return *context->follower;
    }};
    const uint32_t slot{Params().GetConsensus().sidechain.slot};
    const auto call_main{[&](const std::string& m, const UniValue& p, bool wallet) {
        const sidechain::Follower& f{follower()};
        try {
            return f.Client().Call(m, p, wallet);
        } catch (const sidechain::MainClientError& e) {
            throw HostError(e.what());
        }
    }};

    if (method == "peg.connection") {
        const sidechain::Follower& f{follower()};
        const auto status{f.GetStatus()};
        const auto& options{f.Client().GetOptions()};
        UniValue out{UniValue::VOBJ};
        out.pushKV("connected", status.connected);
        if (!status.connected) out.pushKV("error", status.error);
        out.pushKV("node", strprintf("%s:%u", options.host, options.port));
        out.pushKV("slot", static_cast<int64_t>(slot));
        out.pushKV("via", "json-rpc");
        out.pushKV("walletSet", options.wallet_set);
        out.pushKV("wallet", options.wallet);
        out.pushKV("auth", options.credentials.empty() ? "cookie" : "password");
        if (options.credentials.empty()) out.pushKV("cookieFile", fs::PathToString(options.cookie_file));
        return out;
    }
    if (method == "peg.call") {
        const std::string name{ParamStr(params, "method")};
        if (!MainchainMethods().contains(name)) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s is not a method the pages call on the mainchain node", name));
        const UniValue& args{Param(params, "params")};
        if (!args.isNull() && !args.isArray()) throw HostError(BRIDGE_INVALID_REQUEST, "params: an array is needed");
        return call_main(name, args.isNull() ? UniValue{UniValue::VARR} : args, MainchainWalletMethods().contains(name));
    }
    if (method == "peg.deposit") {
        // A deposit address of this sidechain: its slot, and an address of this chain to credit.
        const std::string destination{util::TrimString(ParamStr(params, "destination"))};
        drivechain::DepositAddress deposit_address;
        if (drivechain::ParseDepositAddress(destination, deposit_address) != drivechain::DepositAddressKind::VALID) {
            throw HostError(BRIDGE_INVALID_REQUEST, "This is not a deposit address: get one with \"New deposit address\".");
        }
        if (deposit_address.slot != slot) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("This deposit address is for the sidechain in slot %u, not this one (slot %u).", deposit_address.slot, slot));
        if (!IsValidDestinationString(deposit_address.address)) throw HostError(BRIDGE_INVALID_REQUEST, "The deposit address does not name an address of this chain.");
        const std::string amount_text{util::TrimString(ParamStr(params, "amount"))};
        const auto amount{ParseMoney(amount_text)};
        if (!amount || *amount <= 0) throw HostError(BRIDGE_INVALID_REQUEST, "The amount is a positive number of coins, like 1.5");
        // The wallet of the mainchain node pays: on the user's word, given to the desktop, the amount first.
        const auto& options{follower().Client().GetOptions()};
        Window::ConfirmDialog dialog;
        dialog.title = strprintf("Deposit from the wallet of %s", chain::MAINCHAIN_NAME);
        dialog.message = strprintf("Amount: %s coins of %s, and the fee of the transaction\n\n"
                                   "The wallet of the %s node at %s:%u pays it to this sidechain (slot %u), to be credited to:\n%s\n\n"
                                   "Accept only if you asked for this deposit.",
                                   FormatMoney(*amount), chain::MAINCHAIN_NAME, chain::MAINCHAIN_NAME, guard::OneLine(options.host, 64), options.port, slot, deposit_address.address);
        dialog.accept = "Deposit";
        if (!m_confirm || !m_confirm(dialog)) throw HostError(BRIDGE_CANCELLED, "The deposit was declined.");
        UniValue args{UniValue::VARR};
        args.push_back(static_cast<int64_t>(slot));
        args.push_back(destination);
        args.push_back(FormatMoney(*amount));
        return call_main("createsidechaindeposit", args, /*wallet=*/true);
    }
    // Merged mining: what the wallet of the mainchain node offers its miners, checked here, then confirmed on the desktop.
    const auto run_own{[&](const std::string& m, const UniValue& args) {
        try {
            return m_node().executeRpc(m, args, "/");
        } catch (const UniValue& error) {
            const UniValue& message{error.find_value("message")};
            throw HostError(message.isStr() ? message.get_str() : error.write());
        }
    }};
    if (method == "bmm.set") {
        const UniValue& mine{Param(params, "mine")};
        if (!mine.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "mine: true or false");
        UniValue args{UniValue::VARR};
        args.push_back(mine.get_bool());
        if (!mine.get_bool()) return run_own("setbmm", args);
        const std::string address{ParseOwnAddress(params)};
        const bool always{Param(params, "always").isBool() && Param(params, "always").get_bool()};
        Window::ConfirmDialog dialog;
        dialog.title = "Turn merged mining on";
        if (always) {
            const CAmount offer{ParseOffer(params, "amount", "The offer")};
            if (offer > MAX_BMM_OFFER) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The offer is at most %s for a block.", FormatMoney(MAX_BMM_OFFER)));
            dialog.message = strprintf("Offer: %s coins of %s for every block of %s, with or without fees for it\n\n"
                                       "The wallet of the %s node pays this to its miners for each block of this chain they mine, "
                                       "from now on, until merged mining is turned off. The fees of the blocks go to:\n%s",
                                       FormatMoney(offer), chain::MAINCHAIN_NAME, chain::MAINCHAIN_NAME, chain::MAINCHAIN_NAME, address);
            args.push_back(address);
            args.push_back(true);
            args.push_back(ValueFromAmount(offer));
        } else {
            dialog.message = strprintf("Offer: 99%% of the fees of each block of this chain\n\n"
                                       "The wallet of the %s node pays this to its miners for each block of this chain they mine, "
                                       "from now on, until merged mining is turned off. The fees of the blocks go to:\n%s",
                                       chain::MAINCHAIN_NAME, address);
            args.push_back(address);
        }
        dialog.accept = "Mine";
        if (!m_confirm || !m_confirm(dialog)) throw HostError(BRIDGE_CANCELLED, "Merged mining was not turned on: it was declined.");
        return run_own("setbmm", args);
    }
    if (method == "bmm.request") {
        const std::string address{ParseOwnAddress(params)};
        const CAmount offer{ParseOffer(params, "amount", "The offer")};
        if (offer > MAX_BMM_OFFER) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The offer is at most %s for a block.", FormatMoney(MAX_BMM_OFFER)));
        Window::ConfirmDialog dialog;
        dialog.title = "Mine one block of this chain";
        dialog.message = strprintf("Offer: %s coins of %s, for one block\n\n"
                                   "The wallet of the %s node offers this to its miners to commit to a block of this chain. "
                                   "The fees of the block go to:\n%s",
                                   FormatMoney(offer), chain::MAINCHAIN_NAME, chain::MAINCHAIN_NAME, address);
        dialog.accept = "Offer";
        if (!m_confirm || !m_confirm(dialog)) throw HostError(BRIDGE_CANCELLED, "No block was asked for: it was declined.");
        UniValue args{UniValue::VARR};
        args.push_back(address);
        args.push_back(ValueFromAmount(offer));
        return run_own("requestbmmblock", args);
    }
    throw HostError(BRIDGE_METHOD_NOT_FOUND, "No method " + method);
}

} // namespace webui
