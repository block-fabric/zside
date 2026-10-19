// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/confirm.h>

#include <webui/chainconfig.h>
#include <webui/local.h>

#include <chainparams.h>
#include <drivechain/sidechain.h>
#include <interfaces/node.h>
#include <policy/feerate.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <logging.h>
#include <webui/rpcpolicy.h>

#include <algorithm>
#include <mutex>
#include <set>

namespace webui::confirm {

std::string Coins(CAmount amount)
{
    // Every decimal, as the page shows amounts: the same digits in the same places, row after row.
    const bool negative{amount < 0};
    const uint64_t abs{negative ? uint64_t(0) - static_cast<uint64_t>(amount) : static_cast<uint64_t>(amount)};
    return strprintf("%s%d.%08d %s", negative ? "-" : "", abs / COIN, abs % COIN, CURRENCY_UNIT);
}

std::string NetworkName()
{
    switch (Params().GetChainType()) {
    case ChainType::MAIN: return "Main network";
    case ChainType::TESTNET: return "Testnet";
    case ChainType::SIGNET: return "Signet";
    case ChainType::REGTEST: return "Regtest";
    default: return ChainTypeToString(Params().GetChainType());
    }
}

dialog::Row Text(std::string label, std::string value) { return {std::move(label), std::move(value), dialog::Row::Kind::TEXT}; }
dialog::Row Destination(std::string label, std::string value) { return {std::move(label), std::move(value), dialog::Row::Kind::ADDRESS}; }
dialog::Row Amount(std::string label, CAmount amount) { return {std::move(label), Coins(amount), dialog::Row::Kind::AMOUNT}; }
dialog::Row Total(std::string label, CAmount amount) { return {std::move(label), Coins(amount), dialog::Row::Kind::TOTAL}; }
dialog::Row Heading(std::string label, std::string value) { return {std::move(label), std::move(value), dialog::Row::Kind::HEADING}; }
dialog::Row Note(std::string text) { return {"", std::move(text), dialog::Row::Kind::NOTE}; }
dialog::Row Image(std::string label, std::string data_url) { return {std::move(label), std::move(data_url), dialog::Row::Kind::IMAGE}; }

std::vector<dialog::Row> WalletRows(const std::string& wallet)
{
    return {Text("Wallet", wallet.empty() ? std::string{"(the default wallet)"} : wallet), Text("Network", NetworkName())};
}

std::vector<std::string> ArgNames(interfaces::Node& node, const std::string& method)
{
    static std::mutex mutex;
    static std::map<std::string, std::vector<std::string>> names;
    {
        std::lock_guard lock{mutex};
        if (const auto it{names.find(method)}; it != names.end()) return it->second;
    }
    // The node's own description of its methods: [method, position, name, string?] for each parameter (and for the
    // fields of an options object, at its position: the first name at a position is the parameter's).
    UniValue map;
    try {
        UniValue args{UniValue::VARR};
        args.push_back("dump_all_command_conversions");
        map = node.executeRpc("help", args, "/");
    } catch (...) {
        return {};
    }
    std::lock_guard lock{mutex};
    if (map.isArray()) {
        std::map<std::string, std::vector<std::string>> fresh;
        for (const UniValue& entry : map.getValues()) {
            if (!entry.isArray() || entry.size() < 3 || !entry[0].isStr() || !entry[1].isNum() || !entry[2].isStr()) continue;
            const size_t pos{static_cast<size_t>(std::max(0, entry[1].getInt<int>()))};
            auto& list{fresh[entry[0].get_str()]};
            if (pos >= 64) continue;
            if (list.size() <= pos) list.resize(pos + 1);
            if (list[pos].empty()) list[pos] = entry[2].get_str().substr(0, entry[2].get_str().find('|'));
        }
        for (auto& [name, list] : fresh) names[name] = std::move(list);
    }
    // Known now, or never (an empty list, kept: the node is not asked again for it).
    return names[method];
}

UniValue CanonicalParams(interfaces::Node& node, const std::string& method, const UniValue& params)
{
    if (params.isNull()) return UniValue{UniValue::VARR};
    if (params.isArray()) return params;
    if (!params.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "params: an array or an object");
    const auto names{ArgNames(node, method)};
    if (names.empty()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: give its parameters in order (an array)", method));
    std::vector<UniValue> at;
    for (size_t i{0}; i < params.size(); ++i) {
        const std::string& key{params.getKeys()[i]};
        const auto it{std::ranges::find(names, key)};
        if (key.empty() || it == names.end()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s has no parameter named \"%s\": give its parameters in order (an array)", method, guard::OneLine(key, 60)));
        const size_t pos{static_cast<size_t>(it - names.begin())};
        if (at.size() <= pos) at.resize(pos + 1);
        at[pos] = params.getValues()[i];
    }
    UniValue out{UniValue::VARR};
    for (UniValue& v : at) out.push_back(std::move(v));
    return out;
}

UniValue Call::Arg(size_t index) const
{
    if (params.isArray()) return index < params.size() ? params[index] : UniValue{};
    if (params.isObject()) {
        const auto names{ArgNames(node, method)};
        if (index < names.size() && !names[index].empty()) return params.find_value(names[index]);
    }
    return UniValue{};
}

UniValue Call::Read(const std::string& m, const UniValue& args, bool in_wallet) const
{
    try {
        return node.executeRpc(m, args, in_wallet && !wallet.empty() ? WalletUri(wallet) : std::string{"/"});
    } catch (...) {
        return UniValue{};
    }
}

std::string ValueText(const UniValue& value)
{
    if (value.isStr()) return value.get_str();
    if (value.isNull()) return "(none)";
    return value.write();
}

std::vector<dialog::Row> ParamRows(const Call& call, const std::vector<std::string>& destinations, const std::vector<std::string>& amounts)
{
    // The names given are destinations or amounts; any other is told by its name (guard::RoleOf: an address, an owner, an
    // amount, a fee...), as the console does.
    const auto role{[&](const std::string& name) {
        if (std::ranges::find(destinations, name) != destinations.end()) return guard::ParamRole::DESTINATION;
        if (std::ranges::find(amounts, name) != amounts.end()) return guard::ParamRole::AMOUNT;
        return guard::RoleOf(name);
    }};
    return guard::ParamRows(call.method, call.params, ArgNames(call.node, call.method), role);
}

std::function<std::optional<Window::ConfirmDialog>(const Call&)> Generic(std::string title, std::string accept, std::string what,
                                                                         std::vector<std::string> destinations, std::vector<std::string> amounts,
                                                                         dialog::Level level)
{
    return [=](const Call& call) -> std::optional<Window::ConfirmDialog> {
        Window::ConfirmDialog dialog;
        dialog.title = title;
        dialog.message = what + "\n\nAccept only if you asked for it.";
        dialog.accept = accept;
        dialog.level = level;
        dialog.kind = "rpc:" + call.method;
        dialog.rows = call.wallet.empty() ? std::vector<dialog::Row>{Text("Network", NetworkName())} : WalletRows(call.wallet);
        dialog.rows.push_back(Text("Command", call.method));
        for (auto& row : ParamRows(call, destinations, amounts)) dialog.rows.push_back(std::move(row));
        return dialog;
    };
}

std::function<std::string(const Call&)> AfterDepth(std::string info_method, std::string depth_field)
{
    return [=](const Call& call) {
        const UniValue info{call.Read(info_method, UniValue{UniValue::VARR})};
        const UniValue& depth{info.isObject() ? info.find_value(depth_field) : NullUniValue};
        if (!depth.isNum()) return std::string{"once the reservation is deep enough"};
        const int64_t blocks{depth.getInt<int64_t>()};
        const int64_t seconds{(blocks + 1) * Params().GetConsensus().nPowTargetSpacing};
        const std::string about{seconds < 90 ? strprintf("about %d s", seconds) : strprintf("about %d min", (seconds + 30) / 60)};
        return strprintf("once the reservation is %d block%s deep (%s)", blocks, blocks == 1 ? "" : "s", about);
    };
}

bool SameFirstParam(const UniValue& first, const UniValue& second)
{
    return first.isArray() && second.isArray() && !first.empty() && !second.empty() && first[0].isStr() && second[0].isStr() && first[0].get_str() == second[0].get_str();
}

namespace {
std::optional<Window::ConfirmDialog> SignMessage(const Call& call)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Sign a message";
    dialog.message = "The wallet signs this message with the key of this address. Whoever gets the signature can show it as proof that "
                     "the holder of the address wrote the message.\n\nSign it only if you asked for it.";
    dialog.accept = "Sign";
    dialog.kind = "sign";
    dialog.rows = WalletRows(call.wallet);
    dialog.rows.push_back(Destination("Address", ValueText(call.Arg(0))));
    dialog.rows.push_back(Text("Message", ValueText(call.Arg(1))));
    return dialog;
}

#if WEBUI_SIDECHAIN
std::optional<Window::ConfirmDialog> Withdraw(const Call& call)
{
    // A withdrawal to the mainchain: the coins leave this wallet now, and are paid on the mainchain once a bundle with them
    // is voted through there.
    const UniValue to{call.Arg(0)};
    const UniValue amount_value{call.Arg(1)};
    const UniValue fee_value{call.Arg(2)};
    const auto coins{[](const UniValue& v) -> std::optional<CAmount> { return v.isNum() ? ParseMoney(v.getValStr()) : v.isStr() ? ParseMoney(v.get_str()) : std::nullopt; }};
    const auto amount{coins(amount_value)};
    const CAmount main_fee{fee_value.isNull() ? CAmount{10000} : coins(fee_value).value_or(-1)};
    if (!to.isStr() || !amount || *amount <= 0 || main_fee < 0 || !MoneyRange(*amount) || !MoneyRange(main_fee) || !MoneyRange(*amount + main_fee)) {
        throw HostError(BRIDGE_INVALID_REQUEST, "A withdrawal is [mainchain address, amount, mainchain fee?]");
    }
    Window::ConfirmDialog dialog;
    dialog.title = strprintf("Withdraw to %s", chain::MAINCHAIN_NAME);
    dialog.message = strprintf("The coins leave this wallet now. They are paid on %s once a withdrawal bundle with them is voted through by its "
                               "miners, which takes days. Until the withdrawal is in a bundle, it can be taken back (a refund).\n\n"
                               "Accept only if you asked for this withdrawal.", chain::MAINCHAIN_NAME);
    dialog.accept = "Withdraw";
    dialog.kind = "withdraw";
    dialog.rows = WalletRows(call.wallet);
    dialog.rows.push_back(Destination(strprintf("Pay on %s", chain::MAINCHAIN_NAME), to.get_str()));
    dialog.rows.push_back(Amount("Amount", *amount));
    dialog.rows.push_back(Amount(strprintf("Fee for %s miners", chain::MAINCHAIN_NAME), main_fee));
    dialog.rows.push_back(Total("Leaves this wallet", *amount + main_fee));
    return dialog;
}

std::optional<Window::ConfirmDialog> Refund(const Call& call)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Take back a withdrawal";
    dialog.message = "The withdrawal is taken back, as long as it is not in a withdrawal bundle: the block that includes the request pays its "
                     "coins, the fee for the mainchain included, to its refund address.\n\nAccept only if you asked for it.";
    dialog.accept = "Take back";
    dialog.kind = "refund";
    dialog.rows = WalletRows(call.wallet);
    dialog.rows.push_back(Destination("Withdrawal", strprintf("%s:%s", ValueText(call.Arg(0)), ValueText(call.Arg(1)))));
    return dialog;
}

#else
std::string SlotText(const Call& call, int64_t slot)
{
    // The sidechain in the slot, by its title as the chain has it (text of the chain: shown as text).
    UniValue args{UniValue::VARR};
    args.push_back(slot);
    const UniValue sidechain{call.Read("getsidechain", args)};
    const UniValue& title{sidechain.find_value("title")};
    return title.isStr() ? strprintf("%s (slot %d)", guard::OneLine(title.get_str(), 80), slot) : strprintf("slot %d (no active sidechain)", slot);
}

std::optional<int64_t> SlotArg(const UniValue& value)
{
    if (value.isNum()) return value.getInt<int64_t>();
    if (value.isStr()) {
        const auto parsed{ToIntegral<int64_t>(value.get_str())};
        if (parsed) return *parsed;
    }
    return std::nullopt;
}

std::optional<Window::ConfirmDialog> Deposit(const Call& call)
{
    const auto slot{SlotArg(call.Arg(0))};
    const UniValue destination{call.Arg(1)};
    const UniValue amount_value{call.Arg(2)};
    if (!slot || !destination.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "A deposit is [slot, destination, amount]");
    const std::optional<CAmount> amount{amount_value.isNum() ? ParseMoney(amount_value.getValStr()) : amount_value.isStr() ? ParseMoney(amount_value.get_str()) : std::nullopt};
    if (!amount || *amount <= 0 || !MoneyRange(*amount)) throw HostError(BRIDGE_INVALID_REQUEST, "The amount of a deposit is a positive number of coins");
    Window::ConfirmDialog dialog;
    dialog.title = "Deposit to a sidechain";
    dialog.message = "The wallet pays this amount into the escrow of the sidechain, which credits it to the destination once it has seen the "
                     "deposit. Coins come back from a sidechain only by a withdrawal, which takes days.\n\nAccept only if you asked for this deposit.";
    dialog.accept = "Deposit";
    dialog.kind = "deposit";
    dialog.rows = WalletRows(call.wallet);
    dialog.rows.push_back(Text("Sidechain", SlotText(call, *slot)));
    dialog.rows.push_back(Destination("Deposit to", destination.get_str()));
    drivechain::DepositAddress parsed;
    switch (drivechain::ParseDepositAddress(destination.get_str(), parsed)) {
    case drivechain::DepositAddressKind::VALID:
        dialog.rows.push_back(Destination("Credited to", parsed.address));
        if (parsed.slot != static_cast<uint32_t>(*slot)) dialog.rows.push_back(Note(strprintf("This deposit address is for slot %u: the node refuses it.", parsed.slot)));
        break;
    case drivechain::DepositAddressKind::PLAIN:
        dialog.rows.push_back(Note("This destination is not a deposit address: it is not checked, and the sidechain may not understand it."));
        break;
    case drivechain::DepositAddressKind::INVALID:
        dialog.rows.push_back(Note("This deposit address has a typing error in it: the node refuses it."));
        break;
    }
    dialog.rows.push_back(Amount("Amount", *amount));
    // Made again in place of a deposit the mempool dropped: the two can never both be mined.
    if (const UniValue replaces{call.Arg(3)}; replaces.isStr()) {
        dialog.rows.push_back(Destination("Makes again", replaces.get_str()));
        dialog.rows.push_back(Note("In place of this deposit, which the mempool dropped: it spends a coin the dropped one spent, so that only one of the two can ever be mined."));
    }
    // The fee: what the wallet would pay at its fee rate for a deposit of the usual size (the escrow output spent and made
    // again, one coin of the wallet, its change, the destination), said as an estimate.
    const int64_t vsize{11 + 41 + 68 + 9 + 10 + 9 + 3 + static_cast<int64_t>(destination.get_str().size()) + 31};
    if (call.fee_for) {
        if (const auto fee{call.fee_for(vsize)}) {
            dialog.rows.push_back({"Network fee", "about " + Coins(*fee), dialog::Row::Kind::AMOUNT});
            dialog.rows.push_back({"Total", "about " + Coins(*amount + *fee), dialog::Row::Kind::TOTAL});
            return dialog;
        }
    }
    dialog.rows.push_back(Note("The wallet adds the network fee when it makes the transaction."));
    return dialog;
}

std::optional<Window::ConfirmDialog> WithdrawalVote(const Call& call)
{
    const auto slot{SlotArg(call.Arg(0))};
    const UniValue vote{call.Arg(1)};
    if (!slot || !vote.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "A vote is [slot, vote, hash?]");
    Window::ConfirmDialog dialog;
    dialog.title = "Change a withdrawal vote";
    dialog.kind = "vote";
    dialog.accept = "Change the vote";
    dialog.rows = {Text("Network", NetworkName()), Text("Sidechain", SlotText(call, *slot))};
    const std::string type{vote.get_str()};
    if (type == "upvote") {
        dialog.rows.push_back(Text("Vote", "Upvote this bundle"));
        dialog.rows.push_back(Destination("Bundle", ValueText(call.Arg(2))));
        // What the bundle pays, as this node knows it.
        UniValue args{UniValue::VARR};
        args.push_back(*slot);
        const UniValue bundles{call.Read("listwithdrawalbundles", args)};
        if (bundles.isArray()) {
            for (const UniValue& bundle : bundles.getValues()) {
                if (!bundle.find_value("hash").isStr() || bundle.find_value("hash").get_str() != ValueText(call.Arg(2))) continue;
                for (const char* key : {"score", "blocksleft"}) {
                    if (bundle.find_value(key).isNum()) dialog.rows.push_back(Text(key == std::string{"score"} ? "Score now" : "Blocks left", bundle.find_value(key).getValStr()));
                }
                if (bundle.find_value("known").isBool() && !bundle.find_value("known").get_bool()) dialog.rows.push_back(Note("This node does not have the transaction of this bundle: it cannot check what it pays."));
                // What it pays, each destination whole before its amount (at most the first few; the count says the rest).
                const UniValue& payouts{bundle.find_value("payouts")};
                if (payouts.isArray()) {
                    CAmount total{0};
                    for (const UniValue& payout : payouts.getValues()) {
                        if (const auto amount{payout.find_value("amount").isNum() ? ParseMoney(payout.find_value("amount").getValStr()) : std::nullopt}) total += *amount;
                    }
                    dialog.rows.push_back(Total(strprintf("Pays in all (%u payouts)", payouts.size()), total));
                    for (size_t i{0}; i < payouts.size() && i < 10; ++i) {
                        const UniValue& payout{payouts[i]};
                        dialog.rows.push_back(Destination("Pays", payout.find_value("address").isStr() ? payout.find_value("address").get_str() : ValueText(payout.find_value("script"))));
                        dialog.rows.push_back({"Amount", ValueText(payout.find_value("amount")) + " " + CURRENCY_UNIT, dialog::Row::Kind::AMOUNT});
                    }
                    if (payouts.size() > 10) dialog.rows.push_back(Note(strprintf("… and %u more payouts.", payouts.size() - 10)));
                }
            }
        }
        dialog.rows.push_back(Note("Every other bundle of this sidechain is downvoted."));
        dialog.level = dialog::Level::DANGER;
    } else if (type == "downvote") {
        dialog.rows.push_back(Text("Vote", "Downvote every bundle of this sidechain"));
    } else if (type == "abstain") {
        dialog.rows.push_back(Text("Vote", "Abstain on the bundles of this sidechain"));
    } else if (type == "default") {
        dialog.rows.push_back(Text("Vote", "Follow the default vote"));
    } else {
        dialog.rows.push_back(Text("Vote", type));
    }
    dialog.message = "The blocks this node builds vote so from now on. The votes of miners decide which withdrawals of the sidechain "
                     "are paid out of its escrow: an upvote of a bundle that does not pay what the sidechain owes takes coins from "
                     "its users.\n\nChange it only if you asked for it.";
    return dialog;
}

std::optional<Window::ConfirmDialog> DefaultVote(const Call& call)
{
    const UniValue vote{call.Arg(0)};
    Window::ConfirmDialog dialog;
    dialog.title = "Change the default withdrawal vote";
    dialog.kind = "vote";
    dialog.accept = "Change the vote";
    const std::string type{ValueText(vote)};
    dialog.rows = {Text("Network", NetworkName()), Text("Default vote", type)};
    if (type == "follow") dialog.rows.push_back(Note("Follow also upvotes, where no sidechain node of yours handed a bundle, the one that leads by 50 votes: it trusts the miners who check bundles."));
    if (type == "upvote") dialog.rows.push_back(Note("Upvote votes for the bundle a sidechain node of yours handed to this node, and abstains where it was handed none."));
    dialog.message = "The blocks this node builds vote so on the withdrawals of every sidechain it has no vote of its own for.\n\n"
                     "Change it only if you asked for it.";
    dialog.level = type == "follow" ? dialog::Level::DANGER : dialog::Level::WARNING;
    return dialog;
}

/** The proposal of a hash, as this node knows it (pending in the chain, or queued by this node). */
UniValue ProposalOf(const Call& call, const std::string& hash)
{
    const UniValue list{call.Read("listsidechainproposals", UniValue{UniValue::VARR})};
    for (const char* group : {"pending", "queued"}) {
        const UniValue& proposals{list.find_value(group)};
        if (!proposals.isArray()) continue;
        for (const UniValue& p : proposals.getValues()) {
            for (const char* key : {"proposalhash", "hash"}) {
                if (p.find_value(key).isStr() && p.find_value(key).get_str() == hash) return p;
            }
        }
    }
    return UniValue{};
}

void ProposalRows(std::vector<dialog::Row>& rows, const UniValue& proposal)
{
    if (!proposal.isObject()) {
        rows.push_back(Note("This node knows no proposal with this hash."));
        return;
    }
    if (proposal.find_value("slot").isNum()) rows.push_back(Text("Slot", proposal.find_value("slot").getValStr()));
    if (proposal.find_value("title").isStr()) rows.push_back(Text("Title", guard::OneLine(proposal.find_value("title").get_str(), 120)));
    if (proposal.find_value("description").isStr() && !proposal.find_value("description").get_str().empty()) rows.push_back(Text("Description", proposal.find_value("description").get_str()));
    if (proposal.find_value("replacement").isTrue()) rows.push_back(Note("The slot holds a sidechain already: this proposal replaces it if it activates, and fails its pending withdrawals."));
}

std::optional<Window::ConfirmDialog> Ack(const Call& call)
{
    const std::string hash{ValueText(call.Arg(0))};
    const UniValue ack{call.Arg(1)};
    // Stopping only when the call says so in so many words (false).
    const bool on{!(ack.isBool() && !ack.get_bool())};
    Window::ConfirmDialog dialog;
    dialog.title = on ? "Ack a sidechain proposal" : "Stop acking a sidechain proposal";
    dialog.kind = "ack";
    dialog.accept = on ? "Ack" : "Stop acking";
    dialog.rows = {Text("Network", NetworkName()), Destination("Proposal", hash)};
    ProposalRows(dialog.rows, ProposalOf(call, hash));
    if (!call.Arg(2).isNull()) dialog.rows.push_back(Text("In slot", ValueText(call.Arg(2))));
    dialog.message = on ? "The blocks this node builds ack this proposal from now on: with enough acks of miners, the sidechain activates in its slot."
                        : "The blocks this node builds no longer ack this proposal.";
    dialog.message += "\n\nChange it only if you asked for it.";
    return dialog;
}

std::optional<Window::ConfirmDialog> Propose(const Call& call)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Propose a sidechain";
    dialog.kind = "propose";
    dialog.accept = "Propose";
    dialog.rows = {Text("Network", NetworkName())};
    const auto slot{SlotArg(call.Arg(0))};
    dialog.rows.push_back(Text("Slot", slot ? SlotText(call, *slot) : ValueText(call.Arg(0))));
    dialog.rows.push_back(Text("Title", guard::OneLine(ValueText(call.Arg(1)), 120)));
    if (!call.Arg(2).isNull()) dialog.rows.push_back(Text("Description", ValueText(call.Arg(2))));
    if (!call.Arg(3).isNull()) dialog.rows.push_back(Destination("Release hash", ValueText(call.Arg(3))));
    if (!call.Arg(4).isNull()) dialog.rows.push_back(Destination("Commit hash", ValueText(call.Arg(4))));
    dialog.message = "The next block this node builds proposes this sidechain, and the blocks after ack it. A proposal for a slot that holds a "
                     "sidechain replaces that one if it activates.\n\nPropose it only if you asked for it.";
    return dialog;
}

std::optional<Window::ConfirmDialog> RemoveProposal(const Call& call)
{
    const std::string hash{ValueText(call.Arg(0))};
    Window::ConfirmDialog dialog;
    dialog.title = "Stop proposing a sidechain";
    dialog.kind = "propose";
    dialog.accept = "Stop proposing";
    dialog.rows = {Text("Network", NetworkName()), Destination("Proposal", hash)};
    ProposalRows(dialog.rows, ProposalOf(call, hash));
    dialog.message = "The blocks this node builds no longer propose nor ack this sidechain.\n\nChange it only if you asked for it.";
    return dialog;
}

std::optional<Window::ConfirmDialog> Generate(const Call& call)
{
    // Turning the miner off needs no question.
    // Only when the call says so in so many words (false): a missing, null or other value is asked about.
    if (const UniValue generate{call.Arg(0)}; generate.isBool() && !generate.get_bool()) return std::nullopt;
    Window::ConfirmDialog dialog;
    dialog.title = "Mine with this computer";
    dialog.kind = "mine";
    dialog.accept = "Mine";
    dialog.rows = {Text("Network", NetworkName()), Destination("Rewards to", ValueText(call.Arg(1)))};
    const UniValue threads{call.Arg(2)};
    dialog.rows.push_back(Text("Threads", threads.isNull() ? std::string{"1"} : threads.getValStr() == "-1" ? std::string{"one per processor core"} : ValueText(threads)));
    dialog.message = "The processor of this computer searches for blocks until mining is turned off, which uses it fully. The rewards of the "
                     "blocks found go to the address above.\n\nMine only if you asked for it.";
    return dialog;
}

#endif // WEBUI_SIDECHAIN

std::map<std::string, Method> KitMethods()
{
    using enum Method::Kind;
    std::map<std::string, Method> methods{
        // Signing a message with a key of the wallet (proofs of funds sign one message with each of their addresses).
        {"signmessage", {SPEND, Bridge::Thread::LONG, SignMessage, false, 2, /*batch=*/true}},
#if WEBUI_SIDECHAIN
        // The peg of a sidechain: a withdrawal burns the coins of the wallet (the network fee set by the wallet), a refund
        // takes one back.
        {"createwithdrawal", {SPEND, Bridge::Thread::LONG, Withdraw, /*fee_unknown=*/true, 2}},
        {"refundwithdrawal", {SPEND, Bridge::Thread::LONG, Refund, /*fee_unknown=*/true, 2}},
#else
        // Drivechains: a deposit pays the wallet; votes, acks and proposals change what the blocks of this node say.
        {"createsidechaindeposit", {SPEND, Bridge::Thread::LONG, Deposit, false, 3}},
        {"setwithdrawalvote", {NODE, Bridge::Thread::LONG, WithdrawalVote, false, 2}},
        {"setdefaultwithdrawalvote", {NODE, Bridge::Thread::LONG, DefaultVote, false, 1}},
        {"acksidechain", {NODE, Bridge::Thread::LONG, Ack, false, 1}},
        {"createsidechainproposal", {NODE, Bridge::Thread::LONG, Propose, false, 2}},
        {"removesidechainproposal", {NODE, Bridge::Thread::LONG, RemoveProposal, false, 1}},
        // Mining with the processor: it loads the computer (and the rewards go to an address the page gives).
        {"setgenerate", {NODE, Bridge::Thread::LONG, Generate, false, 1}},
#endif
    };
    // The chain's own (local.cpp): what its pages do that pays, signs or changes the node, with its describers.
    for (auto& [name, method] : local::ConfirmedRpcMethods()) methods.emplace(name, std::move(method));
    return methods;
}

std::optional<Window::ConfirmDialog> Keys(const Call& call)
{
    Window::ConfirmDialog dialog;
    dialog.title = "Unlock the wallet";
    dialog.kind = "keys:" + call.method;
    dialog.message = "The app needs the keys of this wallet for this command. It reads with them or makes a key: it spends nothing.";
    dialog.accept = "Unlock";
    dialog.level = dialog::Level::INFO;
    dialog.rows = WalletRows(call.wallet);
    dialog.rows.push_back(Text("Command", call.method));
    for (auto& row : ParamRows(call)) dialog.rows.push_back(std::move(row));
    return dialog;
}

UniValue RunRpc(interfaces::Node& node, const std::string& method, const UniValue& params, const std::string& wallet)
{
    try {
        return node.executeRpc(method, params, wallet.empty() ? std::string{"/"} : WalletUri(wallet));
    } catch (const UniValue& error) {
        const UniValue& code{error.find_value("code")};
        const UniValue& message{error.find_value("message")};
        throw HostError(code.isNum() ? code.getInt<int>() : BRIDGE_HOST_ERROR, message.isStr() ? message.get_str() : error.write());
    }
}
} // namespace

void Authorizations::Add(Entry entry)
{
    std::lock_guard lock{m_mutex};
    // One for the same second step: the newer replaces it.
    std::erase_if(m_entries, [&](const Entry& e) { return e.wallet == entry.wallet && e.method == entry.method && e.params == entry.params; });
    m_entries.push_back(std::move(entry));
    if (m_entries.size() > MAX) m_entries.erase(m_entries.begin(), m_entries.begin() + (m_entries.size() - MAX));
}

std::optional<Authorizations::Entry> Authorizations::Take(const std::string& wallet, const std::string& method, const std::string& params, std::chrono::steady_clock::time_point now)
{
    std::lock_guard lock{m_mutex};
    std::erase_if(m_entries, [&](const Entry& e) { return e.expires <= now; });
    const auto it{std::ranges::find_if(m_entries, [&](const Entry& e) { return e.wallet == wallet && e.method == method && e.params == params; })};
    if (it == m_entries.end()) return std::nullopt;
    Entry out{std::move(*it)};
    m_entries.erase(it);
    return out;
}

void Authorizations::GiveBack(Entry entry, std::chrono::steady_clock::time_point now)
{
    if (entry.expires <= now) return;
    Add(std::move(entry));
}

size_t Authorizations::Size() const
{
    std::lock_guard lock{m_mutex};
    return m_entries.size();
}

const std::map<std::string, Method>& Methods()
{
    static const std::map<std::string, Method> methods{KitMethods()};
    return methods;
}

ConfirmedRpc::ConfirmedRpc(NodeFn node, guard::AskFn ask, KeysFn keys, FeeFn fee)
    : m_node(std::move(node)), m_ask(std::move(ask)), m_keys(std::move(keys)), m_fee(std::move(fee)) {}

void ConfirmedRpc::Register(Bridge& bridge)
{
    // Each waits for the user's answer on the desktop: on the long lane.
    bridge.RegisterHost("rpc.confirmed", [this](const UniValue& params) { return Call(params); }, Bridge::Thread::LONG);
    bridge.RegisterHost("rpc.confirmedMethods", [](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        UniValue confirmed{UniValue::VARR};
        for (const auto& [name, method] : Methods()) confirmed.push_back(name);
        out.pushKV("confirmed", std::move(confirmed));
        UniValue keyed{UniValue::VARR};
        for (const std::string& name : PageRpcMethods()) {
            if (KeyedWalletRpc(name)) keyed.push_back(name);
        }
        out.pushKV("keyed", std::move(keyed));
        return out;
    });
}

UniValue ConfirmedRpc::Call(const UniValue& params)
{
    const UniValue& wallet_value{params.find_value("wallet")};
    if (!wallet_value.isNull() && !wallet_value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "wallet: the name of a wallet");
    const std::string wallet{wallet_value.isStr() ? wallet_value.get_str() : std::string{}};
    const UniValue& list{params.find_value("calls")};
    const bool batch{!list.isNull()};
    std::vector<std::pair<std::string, UniValue>> calls;
    if (batch) {
        if (!list.isArray() || list.empty() || list.size() > MAX_CALLS) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("calls: 1 to %u calls of one method", MAX_CALLS));
        for (const UniValue& c : list.getValues()) {
            if (!c.find_value("method").isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "calls: {method, params}");
            calls.emplace_back(c.find_value("method").get_str(), c.find_value("params").isNull() ? UniValue{UniValue::VARR} : c.find_value("params"));
        }
    } else {
        if (!params.find_value("method").isStr()) throw HostError(BRIDGE_INVALID_REQUEST, "method: the name of a method");
        calls.emplace_back(params.find_value("method").get_str(), params.find_value("params").isNull() ? UniValue{UniValue::VARR} : params.find_value("params"));
    }
    const std::string name{calls.front().first};
    const UniValue& then{params.find_value("then")};
    const bool only{params.find_value("authorizedOnly").isTrue()};
    if (batch && (!then.isNull() || only)) throw HostError(BRIDGE_INVALID_REQUEST, "calls: one at a time with then or authorizedOnly");
    if (!then.isNull() && only) throw HostError(BRIDGE_INVALID_REQUEST, "then or authorizedOnly, not both");
    // Reads with the keys of several methods may go together (what a locked wallet holds: its assets and their history).
    const auto keyed_read{[this](const std::string& m) { return !AllMethods().contains(m) && KeyedWalletRpc(m); }};
    std::string names{name};
    for (const auto& [m, p] : calls) {
        if (m != name && !(keyed_read(m) && keyed_read(name))) throw HostError(BRIDGE_INVALID_REQUEST, "calls: all of one method, or reads with the keys");
        if (names.find(m) == std::string::npos && names.size() < 400) names += ", " + m;
        if (!p.isArray() && !p.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "params: an array or an object");
    }
    Method method;
    if (const auto it{AllMethods().find(name)}; it != AllMethods().end()) {
        method = it->second;
    } else if (KeyedWalletRpc(name)) {
        method = {Method::Kind::KEYS, Bridge::Thread::LONG, Keys, false, 0, /*batch=*/true};
    } else {
        throw HostError(BRIDGE_METHOD_NOT_FOUND, strprintf("%s is not a method the pages call through the host", name));
    }
    if (method.kind == Method::Kind::NODE && !wallet.empty()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s is a method of the node, not of a wallet", name));
    if (method.kind != Method::Kind::NODE && wallet.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "wallet: the wallet is needed");
    // Several calls in one question only for what moves no coins (signatures of a proof, reads with the keys): a spend is one
    // call, one question.
    if (batch && !method.batch) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: one call at a time", name));

    interfaces::Node& node{m_node()};
    // The parameters as the RPC server takes them: what is described is what runs.
    for (auto& [m, p] : calls) {
        p = CanonicalParams(node, m, p);
        for (size_t i{0}; i < method.required; ++i) {
            if (i >= p.size() || p[i].isNull()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: parameter %u is missing", name, i + 1));
        }
    }
    // A first step with its second, in one question.
    if (!then.isNull()) return TwoSteps(node, wallet, name, method, calls.front().second, then);
    // A second step the user accepted with its first: without a question, when it is exactly that one.
    if (method.kind == Method::Kind::SPEND && !batch) {
        if (auto done{Authorized(node, wallet, name, method, calls.front().second, only)}) return std::move(*done);
    }
    if (only) throw HostError(BRIDGE_AUTH_NEEDED, strprintf("%s needs your confirmation", name));
    // The question: each call described by the host. For several, the rows of the wallet once, then every row of every call,
    // none left out; more than a dialog shows is refused, never cut.
    std::optional<Window::ConfirmDialog> dialog;
    const auto wallet_rows{wallet.empty() ? std::vector<dialog::Row>{} : WalletRows(wallet)};
    // Several reads with the keys: the passphrase of a locked wallet is asked once for the command (it spends nothing).
    if (method.kind == Method::Kind::KEYS && calls.size() > 1) {
        dialog = Keys(confirm::Call{node, wallet, names, UniValue{UniValue::VARR}});
        dialog->rows.push_back(Text("Calls", strprintf("%u", calls.size())));
    }
    const bool keys_batch{dialog.has_value()};
    for (size_t n{0}; n < calls.size() && !keys_batch; ++n) {
        confirm::Call call{node, wallet, calls[n].first, calls[n].second};
        if (m_fee && !wallet.empty()) call.fee_for = [&](int64_t vsize) { return m_fee(wallet, vsize); };
        auto one{method.describe(call)};
        if (!one) continue;
        if (!dialog) {
            dialog = std::move(one);
            if (calls.size() > 1) dialog->rows.insert(dialog->rows.begin() + std::min(wallet_rows.size(), dialog->rows.size()), Heading(strprintf("1 of %u", calls.size())));
            continue;
        }
        dialog->rows.push_back(Heading(strprintf("%u of %u", n + 1, calls.size())));
        for (size_t i{0}; i < one->rows.size(); ++i) {
            // The rows of the wallet, the same for every call (one wallet): said once, above.
            if (i < wallet_rows.size() && one->rows[i].label == wallet_rows[i].label && one->rows[i].value == wallet_rows[i].value) continue;
            dialog->rows.push_back(std::move(one->rows[i]));
        }
    }
    if (dialog && dialog->rows.size() > dialog::MAX_ROWS) {
        throw HostError(BRIDGE_INVALID_REQUEST, strprintf("Too much for one question (%u facts; at most %u): ask for fewer at once", dialog->rows.size(), dialog::MAX_ROWS));
    }
    if (dialog && calls.size() > 1) dialog->message = strprintf("%u at once.\n\n%s", calls.size(), dialog->message);

    const auto run{[&]() -> UniValue {
        if (!batch) return RunRpc(node, name, calls.front().second, wallet);
        UniValue out{UniValue::VARR};
        for (const auto& [m, p] : calls) {
            UniValue answer{UniValue::VOBJ};
            try {
                answer.pushKV("result", RunRpc(node, m, p, wallet));
            } catch (const HostError& e) {
                answer.pushKV("error", ErrorObject(e.code, e.what()));
            }
            out.push_back(std::move(answer));
        }
        return out;
    }};
    if (method.kind == Method::Kind::NODE) {
        if (dialog && (!m_ask || !m_ask(*dialog).accepted)) throw HostError(BRIDGE_CANCELLED, strprintf("%s was not run: it was declined.", name));
        return run();
    }
    if (!m_keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
    if (!dialog) dialog = Keys(confirm::Call{node, wallet, name, calls.front().second});
    return m_keys(wallet, *dialog, method.kind == Method::Kind::SPEND, method.fee_unknown, run);
}

namespace {
//! Whether the wallet's keys are locked (getwalletinfo: unlocked_until 0); unknown is not locked (the call will say).
bool WalletLocked(interfaces::Node& node, const std::string& wallet)
{
    const UniValue info{confirm::Call{node, wallet, "getwalletinfo", UniValue{UniValue::VARR}}.Read("getwalletinfo", UniValue{UniValue::VARR}, true)};
    const UniValue& until{info.isObject() ? info.find_value("unlocked_until") : NullUniValue};
    return until.isNum() && until.getInt<int64_t>() == 0;
}

//! The rows of a describer's question without those of the wallet (said once, above).
void AppendRows(std::vector<dialog::Row>& rows, std::vector<dialog::Row> more, const std::vector<dialog::Row>& wallet_rows)
{
    for (size_t i{0}; i < more.size(); ++i) {
        if (i < wallet_rows.size() && more[i].label == wallet_rows[i].label && more[i].value == wallet_rows[i].value) continue;
        rows.push_back(std::move(more[i]));
    }
}

//! The transaction of a result: the result itself (a txid), or its "txid".
std::string TxidOf(const UniValue& result)
{
    const UniValue& txid{result.isObject() ? result.find_value("txid") : result};
    return txid.isStr() && txid.get_str().size() == 64 && IsHex(txid.get_str()) ? txid.get_str() : std::string{};
}
} // namespace

UniValue ConfirmedRpc::TwoSteps(interfaces::Node& node, const std::string& wallet, const std::string& name, const Method& method, UniValue params, const UniValue& then)
{
    if (!method.reveal || method.kind != Method::Kind::SPEND) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s has no second step", name));
    const Reveal& reveal{*method.reveal};
    if (!then.isObject() || !then.find_value("method").isStr() || then.find_value("method").get_str() != reveal.method) {
        throw HostError(BRIDGE_INVALID_REQUEST, strprintf("then: {method: \"%s\", params}", reveal.method));
    }
    const auto it{AllMethods().find(reveal.method)};
    if (it == AllMethods().end() || it->second.kind != Method::Kind::SPEND) throw HostError(BRIDGE_METHOD_NOT_FOUND, strprintf("%s is not a method the pages call through the host", reveal.method));
    const Method& second{it->second};
    const UniValue& raw{then.find_value("params")};
    if (!raw.isNull() && !raw.isArray() && !raw.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "then.params: an array or an object");
    UniValue second_params{CanonicalParams(node, reveal.method, raw.isNull() ? UniValue{UniValue::VARR} : raw)};
    for (size_t i{0}; i < second.required; ++i) {
        if (i >= second_params.size() || second_params[i].isNull()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: parameter %u is missing", reveal.method, i + 1));
    }
    if (!reveal.matches || !reveal.matches(params, second_params)) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s is not the second step of this %s", reveal.method, name));
    if (!m_keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");

    confirm::Call first_call{node, wallet, name, params};
    confirm::Call second_call{node, wallet, reveal.method, second_params};
    if (m_fee) {
        first_call.fee_for = [&](int64_t vsize) { return m_fee(wallet, vsize); };
        second_call.fee_for = first_call.fee_for;
    }
    auto first_dialog{method.describe(first_call)};
    auto second_dialog{second.describe(second_call)};
    if (!first_dialog || !second_dialog) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: nothing to confirm", name));

    // One question for both: the wallet, each step with all it does, the fees (about, and the most the second may pay).
    const auto wallet_rows{WalletRows(wallet)};
    Window::ConfirmDialog dialog;
    dialog.title = reveal.title;
    dialog.accept = reveal.accept;
    dialog.kind = "two:" + name;
    dialog.level = std::max(first_dialog->level, second_dialog->level);
    dialog.rows = wallet_rows;
    dialog.rows.push_back(Heading("Step 1 of 2", reveal.first_label));
    AppendRows(dialog.rows, std::move(first_dialog->rows), wallet_rows);
    dialog.rows.push_back(Heading("Step 2 of 2", reveal.second_label + " (by itself)"));
    AppendRows(dialog.rows, std::move(second_dialog->rows), wallet_rows);
    const int64_t second_vsize{reveal.second_vsize ? std::max<int64_t>(1, reveal.second_vsize(second_params)) : 300};
    const std::optional<CAmount> first_fee{m_fee ? m_fee(wallet, reveal.first_vsize) : std::nullopt};
    const std::optional<CAmount> second_fee{m_fee ? m_fee(wallet, second_vsize) : std::nullopt};
    const std::optional<CAmount> cap{second_fee ? std::optional<CAmount>{*second_fee * FEE_CAP_FACTOR} : std::nullopt};
    dialog.rows.push_back(Heading("Network fees"));
    dialog.rows.push_back(first_fee ? Amount("Step 1, about", *first_fee) : Text("Step 1", "set by the wallet"));
    if (second_fee) {
        dialog.rows.push_back(Amount("Step 2, about", *second_fee));
        dialog.rows.push_back(Amount("Step 2, at most", *cap));
        if (first_fee) dialog.rows.push_back(Total("Fees, about", *first_fee + *second_fee));
    } else {
        dialog.rows.push_back(Text("Step 2", "set by the wallet: the app asks again for it"));
    }
    const bool locked{WalletLocked(node, wallet)};
    if (locked) dialog.rows.push_back(Note("The wallet is locked: step 2 asks for its passphrase again, to sign it."));
    const std::string when{reveal.when ? reveal.when(first_call) : std::string{"once the first is deep enough"}};
    dialog.message = strprintf("%s\n\nStep 2 is sent by the app without asking again, %s: exactly as shown, from this wallet, within an hour, "
                               "and for a network fee of at most the one shown. Anything else, and the app asks again.\n\nAccept only if you asked for it.",
                               reveal.what, when);

    const UniValue result{m_keys(wallet, dialog, /*always=*/true, /*fee_unknown=*/false, [&] { return RunRpc(node, name, params, wallet); })};
    const std::string txid{TxidOf(result)};
    if (txid.empty() || !cap) return result;
    m_authorizations.Add({wallet, reveal.method, second_params.write(), txid, *cap, second_vsize, m_now() + Authorizations::EXPIRY});
    LogInfo("webui: %s accepted with its second step (%s), which runs without a question once, within %d s", name, reveal.method, Authorizations::EXPIRY.count());
    if (!result.isObject()) return result;
    UniValue out{result};
    out.pushKV("authorized", static_cast<int64_t>(Authorizations::EXPIRY.count()));
    return out;
}

std::optional<UniValue> ConfirmedRpc::Authorized(interfaces::Node& node, const std::string& wallet, const std::string& name, const Method& method, const UniValue& params, bool only)
{
    const auto now{m_now()};
    auto entry{m_authorizations.Take(wallet, name, params.write(), now)};
    if (!entry) return std::nullopt;
    // The first step's transaction still in this wallet, neither replaced nor abandoned: else it is asked again (and the
    // authorization goes: it was for that transaction).
    UniValue args{UniValue::VARR};
    args.push_back(entry->commit_txid);
    const UniValue tx{confirm::Call{node, wallet, "gettransaction", args}.Read("gettransaction", args, true)};
    const UniValue& depth{tx.isObject() ? tx.find_value("confirmations") : NullUniValue};
    bool commit_abandoned{tx.isObject() && tx.find_value("abandoned").isTrue()};
    if (tx.isObject() && tx.find_value("details").isArray()) {
        for (const UniValue& d : tx.find_value("details").getValues()) commit_abandoned |= d.find_value("abandoned").isTrue();
    }
    if (!depth.isNum() || depth.getInt<int64_t>() < 0 || commit_abandoned) {
        LogInfo("webui: the authorization of %s is dropped: its first step %s is not in the wallet any more", name, entry->commit_txid);
        return std::nullopt;
    }
    // Sent already: the method never runs again under the authorization (a new transaction could pay twice while the
    // first may still confirm). While it waits in the mempool, nothing; out of it without a block, that very transaction
    // is broadcast again, a few times at most; in a block, conflicted, abandoned or gone, the authorization is used up.
    if (!entry->reveal_txid.empty()) {
        UniValue reveal{UniValue::VARR};
        reveal.push_back(entry->reveal_txid);
        if (confirm::Call{node, wallet, "getmempoolentry", reveal}.Read("getmempoolentry", reveal).isObject()) {
            m_authorizations.GiveBack(std::move(*entry), now);
            return std::nullopt;
        }
        const UniValue sent{confirm::Call{node, wallet, "gettransaction", reveal}.Read("gettransaction", reveal, true)};
        const UniValue& sent_depth{sent.isObject() ? sent.find_value("confirmations") : NullUniValue};
        const UniValue& hex{sent.isObject() ? sent.find_value("hex") : NullUniValue};
        // Abandoned: said by each entry of its details (gettransaction), or at the top.
        bool abandoned{sent.isObject() && sent.find_value("abandoned").isTrue()};
        if (sent.isObject() && sent.find_value("details").isArray()) {
            for (const UniValue& d : sent.find_value("details").getValues()) abandoned |= d.find_value("abandoned").isTrue();
        }
        if (!sent_depth.isNum() || sent_depth.getInt<int64_t>() != 0 || abandoned || !hex.isStr() ||
            entry->rebroadcasts >= Authorizations::MAX_REBROADCASTS) {
            LogInfo("webui: the authorization of %s is used up (its transaction %s is in a block, conflicted, abandoned or sent again enough)", name, entry->reveal_txid);
            return std::nullopt;
        }
        ++entry->rebroadcasts;
        UniValue raw{UniValue::VARR};
        raw.push_back(hex);
        try {
            const UniValue txid{RunRpc(node, "sendrawtransaction", raw, std::string{})};
            LogInfo("webui: %s: its transaction %s broadcast again as accepted (%d of %d)", name, entry->reveal_txid, entry->rebroadcasts, Authorizations::MAX_REBROADCASTS);
            UniValue result{UniValue::VOBJ};
            result.pushKV("txid", txid.isStr() ? txid.get_str() : entry->reveal_txid);
            result.pushKV("rebroadcast", true);
            m_authorizations.GiveBack(std::move(*entry), now);
            return result;
        } catch (const HostError& e) {
            // Not taken back (stale at this tip, its inputs spent): used up; the call is asked as any other.
            LogInfo("webui: the authorization of %s is used up: its transaction %s is not taken back (%s)", name, entry->reveal_txid, e.what());
            return std::nullopt;
        }
    }
    // At most the fee the user was shown, at the wallet's fee rate now.
    const std::optional<CAmount> fee{m_fee ? m_fee(wallet, entry->vsize) : std::nullopt};
    if (!fee || *fee > entry->fee_cap) {
        LogInfo("webui: %s is asked again: its network fee would be more than the most the user accepted", name);
        m_authorizations.GiveBack(std::move(*entry), now);
        return std::nullopt;
    }
    if (!m_keys) throw HostError(-18, "Wallets are disabled in this node (-disablewallet).");
    // A locked wallet needs its passphrase: asked (the question of this call, saying it was accepted), unless the page
    // asked for no question.
    if (only && WalletLocked(node, wallet)) {
        m_authorizations.GiveBack(std::move(*entry), now);
        throw HostError(BRIDGE_AUTH_NEEDED, strprintf("%s needs the passphrase of the wallet", name));
    }
    confirm::Call call{node, wallet, name, params};
    if (m_fee) call.fee_for = [&](int64_t vsize) { return m_fee(wallet, vsize); };
    auto dialog{method.describe(call)};
    if (!dialog) dialog = Keys(call);
    dialog->message = "You accepted this with its first step. The wallet is locked: its passphrase signs it.\n\n" + dialog->message;
    try {
        UniValue result{m_keys(wallet, *dialog, /*always=*/false, /*fee_unknown=*/false, [&] { return RunRpc(node, name, params, wallet); })};
        LogInfo("webui: %s ran as accepted with its first step %s", name, entry->commit_txid);
        // Kept for a send again of this very call, should its transaction leave the mempool without a block.
        if (const std::string txid{TxidOf(result)}; !txid.empty()) {
            entry->reveal_txid = txid;
            m_authorizations.GiveBack(std::move(*entry), now);
        }
        return result;
    } catch (...) {
        // Nothing was made: the authorization stays, for the call again.
        m_authorizations.GiveBack(std::move(*entry), now);
        throw;
    }
}

} // namespace webui::confirm
