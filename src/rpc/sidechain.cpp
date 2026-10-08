// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <cctype>
#include <chain.h>
#include <common/args.h>
#include <common/settings.h>
#include <core_io.h>
#include <key_io.h>
#include <node/context.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <streams.h>
#include <util/strencodings.h>
#include <sidechain/follower.h>
#include <sidechain/mainchain.h>
#include <sidechain/state.h>
#include <univalue.h>
#include <validation.h>

using node::NodeContext;
using sidechain::Follower;

namespace {

Follower& EnsureFollower(const NodeContext& node)
{
    if (!node.follower) throw JSONRPCError(RPC_MISC_ERROR, "This chain is not a sidechain");
    return *node.follower;
}

UniValue WithdrawalToJSON(const sidechain::Withdrawal& withdrawal, const sidechain::State& side)
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("txid", withdrawal.outpoint.hash.GetHex());
    obj.pushKV("vout", withdrawal.outpoint.n);
    obj.pushKV("amount", ValueFromAmount(withdrawal.amount));
    obj.pushKV("mainchainfee", ValueFromAmount(withdrawal.main_fee));
    obj.pushKV("mainchainscript", HexStr(withdrawal.main_script));
    obj.pushKV("refundaddress", EncodeDestination(WitnessV0KeyHash{withdrawal.refund_keyhash}));
    obj.pushKV("height", withdrawal.height);
    obj.pushKV("status", side.InBundle(withdrawal.outpoint) ? "bundled" : "waiting");
    return obj;
}

RPCMethod getmainchaininfo()
{
    return RPCMethod{
        "getmainchaininfo",
        "Returns how this node follows the mainchain, of which this chain is a sidechain.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::NUM, "slot", "The slot of this sidechain on the mainchain"},
            {RPCResult::Type::BOOL, "connected", "Whether the mainchain node answered the last time it was asked"},
            {RPCResult::Type::STR, "error", /*optional=*/true, "Why the mainchain node could not be asked"},
            {RPCResult::Type::STR, "node", "Address of the mainchain node"},
            {RPCResult::Type::NUM, "height", "Height of the last mainchain block on record, -1 if there is none"},
            {RPCResult::Type::STR_HEX, "bestblockhash", /*optional=*/true, "Hash of the last mainchain block on record"},
            {RPCResult::Type::NUM, "tipheight", "Height of the last mainchain block that the tip of this chain has acted on"},
        }},
        RPCExamples{HelpExampleCli("getmainchaininfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    Follower& follower{EnsureFollower(node)};
    ChainstateManager& chainman{EnsureChainman(node)};
    const auto status{follower.GetStatus()};
    UniValue result(UniValue::VOBJ);
    result.pushKV("slot", chainman.GetConsensus().sidechain.slot);
    result.pushKV("connected", status.connected);
    if (!status.connected) result.pushKV("error", status.error);
    result.pushKV("node", strprintf("%s:%u", follower.Client().GetOptions().host, follower.Client().GetOptions().port));
    result.pushKV("height", chainman.m_mainchain->Height());
    if (chainman.m_mainchain->Height() >= 0) result.pushKV("bestblockhash", chainman.m_mainchain->TipHash().GetHex());
    result.pushKV("tipheight", WITH_LOCK(::cs_main, return chainman.ActiveChainstate().SideState().MainHeight()));
    return result;
},
    };
}

RPCMethod syncmainchain()
{
    return RPCMethod{
        "syncmainchain",
        "Ask the mainchain node for its new blocks now, instead of waiting for the next time this node does so by itself,\n"
        "and act on them: connect the blocks of this chain that the mainchain committed to.",
        {
            {"allowdeepreorg", RPCArg::Type::BOOL, RPCArg::Default{false}, "Follow the mainchain node even if it dropped more blocks with commitments\n"
             "to this chain than a reorg of this chain can take back. A node refuses that otherwise, as it would take the word of a wrong\n"
             "mainchain node for it: only for a mainchain that really did reorganise so deep."},
        },
        RPCResult{RPCResult::Type::NUM, "", "Height of the last mainchain block on record"},
        RPCExamples{HelpExampleCli("syncmainchain", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    std::string error;
    const bool allow_deep{!request.params[0].isNull() && request.params[0].get_bool()};
    if (allow_deep) EnsureFollower(node).AllowDeepReorg();
    const bool synced{EnsureFollower(node).Sync(error)};
    if (allow_deep) EnsureFollower(node).AllowDeepReorg(false);
    if (!synced) throw JSONRPCError(RPC_MISC_ERROR, error);
    return EnsureChainman(node).m_mainchain->Height();
},
    };
}

RPCMethod createbmmblock()
{
    return RPCMethod{
        "createbmmblock",
        "Build the next block of this chain and keep it. The block becomes part of the chain when the next block of the\n"
        "mainchain commits to its hash (blind merged mining); use createbmmrequest on the mainchain node to ask for that,\n"
        "or setbmm on this node to have all of it done continuously.",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address that receives the transaction fees of the block"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "hash", "The hash of the block, for the mainchain to commit to"},
            {RPCResult::Type::STR_HEX, "mainchainblockhash", "The mainchain block the block was built on; only the mainchain block after it can commit to the block"},
            {RPCResult::Type::NUM, "mainchainheight", "The height of that mainchain block"},
            {RPCResult::Type::NUM, "transactions", "Number of transactions in the block, the coinbase included"},
        }},
        RPCExamples{HelpExampleCli("createbmmblock", "\"address\"")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    const CTxDestination destination{DecodeDestination(request.params[0].get_str())};
    if (!IsValidDestination(destination)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    std::string error;
    const auto candidate{EnsureFollower(node).CreateCandidate(GetScriptForDestination(destination), error)};
    if (!candidate) throw JSONRPCError(RPC_MISC_ERROR, error);
    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", candidate->block->GetHash().GetHex());
    result.pushKV("mainchainblockhash", candidate->main_prev_hash.GetHex());
    result.pushKV("mainchainheight", candidate->main_prev_height);
    result.pushKV("transactions", candidate->block->vtx.size());
    return result;
},
    };
}

RPCMethod requestbmmblock()
{
    return RPCMethod{
        "requestbmmblock",
        "Mine one block of this chain by hand: build it, and have the wallet of the mainchain node offer the miners of the\n"
        "mainchain a fee to commit to it in their next block. The block can be empty: this is how a deposit gets paid, or a\n"
        "withdrawal bundle started, when there are no transactions whose fees would pay for a block.",
        {
            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The address that receives the transaction fees of the block"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Optional::NO, "What to offer the miners of the mainchain, in coins of the mainchain"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "hash", "The hash of the block"},
            {RPCResult::Type::NUM, "mainchainheight", "The height of the mainchain block the block was built on; the next one can commit to it"},
            {RPCResult::Type::NUM, "transactions", "Number of transactions in the block, the coinbase included"},
            {RPCResult::Type::STR_AMOUNT, "fees", "The transaction fees of the block, which go to the address"},
            {RPCResult::Type::STR_AMOUNT, "amount", "What was offered"},
            {RPCResult::Type::STR_HEX, "mainchaintxid", "The transaction of the mainchain wallet that makes the offer"},
        }},
        RPCExamples{HelpExampleCli("requestbmmblock", "\"address\" 0.0001")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    Follower& follower{EnsureFollower(node)};
    const CTxDestination destination{DecodeDestination(request.params[0].get_str())};
    if (!IsValidDestination(destination)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
    const CAmount amount{AmountFromValue(request.params[1])};
    if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The amount must be positive");
    std::string error;
    const auto candidate{follower.CreateCandidate(GetScriptForDestination(destination), error)};
    if (!candidate) throw JSONRPCError(RPC_MISC_ERROR, error);
    const auto txid{follower.RequestCommitment(*candidate, amount, error)};
    if (!txid) {
        follower.DropCandidate(candidate->block->GetHash());
        throw JSONRPCError(RPC_MISC_ERROR, error);
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("hash", candidate->block->GetHash().GetHex());
    result.pushKV("mainchainheight", candidate->main_prev_height);
    result.pushKV("transactions", candidate->block->vtx.size());
    result.pushKV("fees", ValueFromAmount(candidate->block->vtx[0]->vout[0].nValue));
    result.pushKV("amount", ValueFromAmount(amount));
    result.pushKV("mainchaintxid", *txid);
    return result;
},
    };
}

RPCMethod setbmm()
{
    return RPCMethod{
        "setbmm",
        "Turn continuous blind merged mining on or off. When it is on, this node builds a block of this chain whenever\n"
        "there are transactions whose fees pay for one, and has the wallet of the mainchain node offer the miners of the\n"
        "mainchain 99% of those fees to commit to it. The fees go to the address; the hundredth that is left is what the\n"
        "operator of this node earns. A block without fees is not asked for: see requestbmmblock to mine one by hand.",
        {
            {"mine", RPCArg::Type::BOOL, RPCArg::Optional::NO, "Whether to mine"},
            {"address", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The address that receives the transaction fees of the blocks. Required to turn mining on."},
            {"always", RPCArg::Type::BOOL, RPCArg::Default{false}, "For tests: ask for a block for every block of the mainchain, with or without fees, and offer `amount` for each"},
            {"amount", RPCArg::Type::AMOUNT, RPCArg::Default{"0.0001"}, "What to offer for each block if `always` is set, in coins of the mainchain"},
        },
        RPCResult{RPCResult::Type::OBJ, "", "", {{RPCResult::Type::BOOL, "mining", "Whether mining is on"}}},
        RPCExamples{HelpExampleCli("setbmm", "true \"address\"") + HelpExampleCli("setbmm", "false")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    Follower& follower{EnsureFollower(node)};
    const bool mine{request.params[0].get_bool()};
    CScript script;
    bool always{false};
    CAmount amount{10000};
    if (mine) {
        if (request.params[1].isNull()) throw JSONRPCError(RPC_INVALID_PARAMETER, "An address is required to mine");
        const CTxDestination destination{DecodeDestination(request.params[1].get_str())};
        if (!IsValidDestination(destination)) throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Error: Invalid address");
        script = GetScriptForDestination(destination);
        always = !request.params[2].isNull() && request.params[2].get_bool();
        if (!request.params[3].isNull()) amount = AmountFromValue(request.params[3]);
        if (amount <= 0) throw JSONRPCError(RPC_INVALID_PARAMETER, "The amount must be positive");
    }
    follower.SetMining(mine, script, always, amount);
    // Kept in the settings, as wallets to load are: a node restarted (after a crash, say) mines again.
    if (node.args) {
        UniValue saved{UniValue::VNULL};
        if (mine) {
            saved.setObject();
            saved.pushKV("address", request.params[1].get_str());
            saved.pushKV("always", always);
            saved.pushKV("amount", amount);
        }
        node.args->LockSettings([&](common::Settings& settings) {
            if (saved.isNull()) {
                settings.rw_settings.erase("bmm");
            } else {
                settings.rw_settings["bmm"] = saved;
            }
        });
        if (node.args->GetSettingsPath()) node.args->WriteSettingsFile();
    }
    UniValue result(UniValue::VOBJ);
    result.pushKV("mining", mine);
    return result;
},
    };
}

RPCMethod getbmminfo()
{
    return RPCMethod{
        "getbmminfo",
        "Returns the state of continuous blind merged mining.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::BOOL, "mining", "Whether mining is on"},
            {RPCResult::Type::STR, "address", /*optional=*/true, "The address that receives the transaction fees"},
            {RPCResult::Type::BOOL, "always", /*optional=*/true, "Whether a block is asked for with every block of the mainchain, with or without fees"},
            {RPCResult::Type::STR_AMOUNT, "amount", /*optional=*/true, "What is offered for each block, if always"},
            {RPCResult::Type::BOOL, "idle", /*optional=*/true, "Whether mining waits for transactions whose fees pay for a block"},
            {RPCResult::Type::STR_AMOUNT, "lastfees", /*optional=*/true, "The fees of the block last asked for"},
            {RPCResult::Type::STR_AMOUNT, "lastoffer", /*optional=*/true, "What was offered to the miners of the mainchain for it"},
            {RPCResult::Type::NUM, "requests", "Number of commitments asked for since mining was first turned on"},
            {RPCResult::Type::NUM, "blocks", "Number of blocks of this node that the mainchain committed to"},
            {RPCResult::Type::NUM, "outbid", "Number of mainchain blocks for which another node bid more, for another block of this chain"},
            {RPCResult::Type::STR, "error", /*optional=*/true, "Why the last attempt to mine failed"},
        }},
        RPCExamples{HelpExampleCli("getbmminfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    const auto mining{EnsureFollower(node).GetMining()};
    UniValue result(UniValue::VOBJ);
    result.pushKV("mining", mining.enabled);
    if (mining.enabled) {
        CTxDestination destination;
        if (ExtractDestination(mining.coinbase_script, destination)) result.pushKV("address", EncodeDestination(destination));
        result.pushKV("always", mining.always);
        if (mining.always) result.pushKV("amount", ValueFromAmount(mining.amount));
        result.pushKV("idle", mining.idle);
        if (mining.requests > 0) {
            result.pushKV("lastfees", ValueFromAmount(mining.last_fees));
            result.pushKV("lastoffer", ValueFromAmount(mining.last_offer));
        }
    }
    result.pushKV("requests", mining.requests);
    result.pushKV("blocks", mining.blocks);
    result.pushKV("outbid", mining.outbid);
    if (!mining.error.empty()) result.pushKV("error", mining.error);
    return result;
},
    };
}

RPCMethod listwithdrawals()
{
    return RPCMethod{
        "listwithdrawals",
        "Returns the withdrawals that the mainchain has not paid yet.",
        {},
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "txid", "The transaction that made the withdrawal"},
                {RPCResult::Type::NUM, "vout", "The output of that transaction that burned the coins"},
                {RPCResult::Type::STR_AMOUNT, "amount", "The amount to be paid on the mainchain"},
                {RPCResult::Type::STR_AMOUNT, "mainchainfee", "The fee offered to mainchain miners"},
                {RPCResult::Type::STR_HEX, "mainchainscript", "The output script to be paid on the mainchain"},
                {RPCResult::Type::STR, "refundaddress", "The address a refund of the withdrawal pays"},
                {RPCResult::Type::NUM, "height", "The height of the block with the withdrawal"},
                {RPCResult::Type::STR, "status", "\"waiting\" for a bundle, in which state it can be refunded, or \"bundled\" in the bundle the mainchain is voting on"},
            }},
        }},
        RPCExamples{HelpExampleCli("listwithdrawals", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const sidechain::State side{chainman.ActiveChainstate().SideState()};
    UniValue result(UniValue::VARR);
    side.ForEachWithdrawal([&](const sidechain::Withdrawal& withdrawal) {
        result.push_back(WithdrawalToJSON(withdrawal, side));
        return true;
    });
    return result;
},
    };
}

RPCMethod getwithdrawalbundle()
{
    return RPCMethod{
        "getwithdrawalbundle",
        "Returns the withdrawal bundle that the mainchain is voting on, if there is one, and otherwise\n"
        "the one that the next block could commit to, if any.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR, "status", "\"pending\": the mainchain is voting on the bundle; \"next\": the next block can commit to it; \"none\": there is no bundle and none can be made now"},
            {RPCResult::Type::STR_HEX, "hash", /*optional=*/true, "The hash of the bundle, which the mainchain votes on"},
            {RPCResult::Type::NUM, "height", /*optional=*/true, "The height of the block that committed to a pending bundle"},
            {RPCResult::Type::NUM, "withdrawals", /*optional=*/true, "The number of withdrawals in the bundle"},
            {RPCResult::Type::STR_AMOUNT, "amount", /*optional=*/true, "What the bundle pays out on the mainchain"},
            {RPCResult::Type::STR_AMOUNT, "mainchainfee", /*optional=*/true, "What the bundle pays to mainchain miners"},
            {RPCResult::Type::STR_HEX, "hex", /*optional=*/true, "The transaction of the bundle, as the mainchain node wants to receive it"},
            {RPCResult::Type::NUM, "waiting", "The number of withdrawals waiting for a later bundle"},
            {RPCResult::Type::NUM, "lastfailureheight", /*optional=*/true, "The height of the block that learned that the previous bundle failed"},
        }},
        RPCExamples{HelpExampleCli("getwithdrawalbundle", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    const sidechain::State side{chainman.ActiveChainstate().SideState()};
    UniValue result(UniValue::VOBJ);
    std::optional<CMutableTransaction> tx;
    std::vector<COutPoint> withdrawals;
    if (const auto pending{side.Bundle()}) {
        result.pushKV("status", "pending");
        tx = side.BundleTx();
        withdrawals = pending->withdrawals;
        result.pushKV("height", pending->height);
    } else if ((tx = side.NextBundle(chainman.ActiveHeight() + 1, chainman.ActiveChain().Tip()->GetBlockHash(), chainman.GetConsensus().sidechain, &withdrawals,
                                     side.MainPendingNext(*Assert(chainman.m_mainchain), chainman.ActiveHeight() + 1, chainman.GetConsensus().sidechain)))) {
        result.pushKV("status", "next");
    } else {
        result.pushKV("status", "none");
    }
    if (tx) {
        CAmount amount{0}, fee{0};
        for (const COutPoint& outpoint : withdrawals) {
            const sidechain::Withdrawal withdrawal{*Assert(side.GetWithdrawal(outpoint))};
            amount += withdrawal.amount;
            fee += withdrawal.main_fee;
        }
        result.pushKV("hash", tx->GetHash().GetHex());
        result.pushKV("withdrawals", withdrawals.size());
        result.pushKV("amount", ValueFromAmount(amount));
        result.pushKV("mainchainfee", ValueFromAmount(fee));
        DataStream blind{};
        blind << TX_NO_WITNESS(CTransaction{*tx});
        result.pushKV("hex", HexStr(blind));
    }
    size_t total{0};
    side.ForEachWithdrawal([&](const sidechain::Withdrawal&) {
        ++total;
        return true;
    });
    result.pushKV("waiting", total - withdrawals.size());
    if (side.LastFailureHeight() >= 0) result.pushKV("lastfailureheight", side.LastFailureHeight());
    return result;
},
    };
}

} // namespace

RPCMethod getsidechainstate()
{
    return RPCMethod{
        "getsidechainstate",
        "Returns a hash of the whole state of this chain as a sidechain, as of the chain tip (withdrawals, the pending\n"
        "bundle, the payouts owed, and whatever this sidechain keeps: names, assets, markets, notes...), and how many\n"
        "entries each table has. Nodes with the same chain have the same hash.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "bestblock", "The block the state is as of"},
            {RPCResult::Type::NUM, "height", "Its height"},
            {RPCResult::Type::STR_HEX, "hash", "The hash of every entry of the state, in key order"},
            {RPCResult::Type::NUM, "entries", "The number of entries"},
            {RPCResult::Type::OBJ_DYN, "tables", "Entries per table, by the table's key byte", {{RPCResult::Type::NUM, "byte", "The number of entries"}}},
        }},
        RPCExamples{HelpExampleCli("getsidechainstate", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    LOCK(::cs_main);
    Chainstate& chainstate{chainman.ActiveChainstate()};
    const sidechain::StoreView& view{chainstate.SideCache()};
    std::map<unsigned char, uint64_t> tables;
    uint64_t entries{0};
    // Counted on the way, in the one pass over the state that the hash takes.
    const uint256 hash{sidechain::StoreHash(view, {}, [&](const sidechain::StoreBytes& key, const sidechain::StoreBytes&) {
        ++tables[key.empty() ? 0 : key[0]];
        ++entries;
    })};
    UniValue result(UniValue::VOBJ);
    result.pushKV("bestblock", chainstate.m_chain.Tip()->GetBlockHash().GetHex());
    result.pushKV("height", chainstate.m_chain.Height());
    result.pushKV("hash", hash.GetHex());
    result.pushKV("entries", entries);
    UniValue by_table(UniValue::VOBJ);
    for (const auto& [table, count] : tables) {
        by_table.pushKV(std::isprint(table) ? std::string(1, static_cast<char>(table)) : strprintf("0x%02x", table), count);
    }
    result.pushKV("tables", std::move(by_table));
    return result;
},
    };
}

void RegisterSidechainRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"sidechain", &getmainchaininfo},
        {"sidechain", &syncmainchain},
        {"sidechain", &createbmmblock},
        {"sidechain", &setbmm},
        {"sidechain", &requestbmmblock},
        {"sidechain", &getbmminfo},
        {"sidechain", &listwithdrawals},
        {"sidechain", &getwithdrawalbundle},
        {"sidechain", &getsidechainstate},
    };
    for (const auto& c : commands) {
        t.appendCommand(c.name, &c);
    }
}
