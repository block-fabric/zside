// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <core_io.h>
#include <index/orchardindex.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <validation.h>
#include <zside/shielded.h>

#include <univalue.h>

namespace {

RPCMethod getshieldedinfo()
{
    return RPCMethod{
        "getshieldedinfo",
        "The shielded pool: what it holds, how many notes it has made and spent, and the root of its tree.",
        {},
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_AMOUNT, "pool", "What the pool holds: all put in, less all taken out"},
            {RPCResult::Type::NUM, "notes", "Notes ever made: the size of the tree"},
            {RPCResult::Type::NUM, "spent", "Nullifiers revealed: one per action, those of padding actions included"},
            {RPCResult::Type::STR_HEX, "root", "The root of the tree"},
            {RPCResult::Type::OBJ, "index", /*optional=*/true, "The whole tree, which wallets need to spend (-orchardindex)",
            {
                {RPCResult::Type::NUM, "notes", "Notes it holds"},
                {RPCResult::Type::STR_HEX, "root", "Its root: that of consensus, once synced"},
                {RPCResult::Type::BOOL, "synced", "Whether it has the tree of the tip"},
            }},
        }},
        RPCExamples{HelpExampleCli("getshieldedinfo", "")},
        [&](const RPCMethod& self, const JSONRPCRequest& request) -> UniValue
{
    ChainstateManager& chainman{EnsureAnyChainman(request.context)};
    UniValue result(UniValue::VOBJ);
    uint64_t size;
    {
        LOCK(::cs_main);
        const zside::ShieldedState state{chainman.ActiveChainstate().SideState().Shielded()};
        const zside::Frontier tree{state.Tree()};
        size = tree.Size();
        result.pushKV("pool", ValueFromAmount(state.Pool()));
        result.pushKV("notes", size);
        result.pushKV("spent", state.NullifierCount());
        result.pushKV("root", HexStr(tree.Root()));
    }
    if (g_orchardindex) {
        g_orchardindex->BlockUntilSyncedToCurrentChain();
        const auto [notes, root]{g_orchardindex->Tree()};
        UniValue index(UniValue::VOBJ);
        index.pushKV("notes", notes);
        index.pushKV("root", HexStr(root));
        index.pushKV("synced", g_orchardindex->GetSummary().synced);
        result.pushKV("index", std::move(index));
    }
    return result;
},
    };
}

} // namespace

void RegisterShieldedRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"shielded", &getshieldedinfo},
    };
    for (const auto& c : commands) t.appendCommand(c.name, &c);
}
