// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/rpcpolicy.h>

#include <webui/chainconfig.h>
#include <webui/confirm.h>
#include <webui/local.h>

#include <map>
#include <set>

namespace webui {
namespace {
using enum Bridge::Thread;

// Keep in order; LONG for the methods that may run for long (they never hold up the quick ones).
const std::map<std::string, Bridge::Thread>& Methods()
{
    static const std::map<std::string, Bridge::Thread> methods{[] {
    std::map<std::string, Bridge::Thread> methods{
        // The chain and the node.
        {"getbestblockhash", WORKER},
        {"getblock", WORKER},
        {"getblockchaininfo", WORKER},
        {"getblockcount", WORKER},
        {"getblockhash", WORKER},
        {"getblockstats", WORKER},
        {"getblockheader", WORKER},
        {"getmempoolentry", WORKER},
        {"getmempoolinfo", WORKER},
        {"getmininginfo", WORKER},
        {"getnetworkhashps", WORKER},
        {"getnetworkinfo", WORKER},
        {"getpeerinfo", WORKER},
        {"getrawmempool", WORKER},
        {"getrawtransaction", WORKER},
        {"gettxout", WORKER},
        // Which transactions spend the outputs of one shown (the explorer): from the mempool, or from -txospenderindex if on.
        {"gettxspendingprevout", WORKER},
        {"help", WORKER},
        {"listbanned", WORKER},
        {"uptime", WORKER},
#if WEBUI_SIDECHAIN
        // A sidechain (the Peg and Merged mining pages, the dashboard): the mainchain it follows, its
        // withdrawals and bundles, blind merged mining. A withdrawal and its refund are confirmed on the desktop
        // (rpc.confirmed, confirm.cpp: audit 7 #1).
        {"getbmminfo", WORKER},
        {"getdepositaddress", WORKER},
        {"getmainchaininfo", WORKER},
        {"getwithdrawalbundle", WORKER},
        {"listwithdrawals", WORKER},
        {"syncmainchain", LONG},
        // (setbmm and requestbmmblock spend the wallet of the mainchain node: host methods bmm.set and
        // bmm.request, confirmed on the desktop; sidechainapi.cpp.)
#else
        // Mining with the processor (the Mine page): turning it on is confirmed (rpc.confirmed, confirm.cpp).
        {"getgenerate", WORKER},
        // Drivechains (the Sidechains page): what they read. Deposits, votes, acks and proposals are confirmed on the
        // desktop first (rpc.confirmed, confirm.cpp: audit 7 #1).
        {"getdrivechaininfo", WORKER},
        {"getsidechainevents", WORKER},
        {"listactivesidechains", WORKER},
        {"listsidechaindeposits", WORKER},
        {"listsidechainproposals", WORKER},
        {"listwithdrawalbundles", WORKER},
#endif
        // Addresses, scripts and PSBTs (multisig lounge, proof of funds, crypto tools).
        {"createpsbt", WORKER},
        {"decodepsbt", WORKER},
        {"deriveaddresses", WORKER},
        {"finalizepsbt", WORKER},
        {"getdescriptorinfo", WORKER},
        {"scantxoutset", LONG},
        {"utxoupdatepsbt", WORKER},
        {"validateaddress", WORKER},
        {"verifymessage", WORKER},
        // Wallets: what the pages read. Unlocking is the host's (wallet.unlock: the passphrase typed in its dialog).
        {"getaddressinfo", WORKER},
        {"getbalances", WORKER},
        {"getnewaddress", WORKER},
        {"getwalletinfo", WORKER},
        {"listunspent", WORKER},
        {"listwallets", WORKER},
        {"walletlock", WORKER},
        // Nothing that spends or signs: payments, signatures of messages and of PSBTs, and broadcasts go through the host
        // (walletapi.cpp, confirm.cpp), which shows what they do in its own dialog first (audit 7 #1).
    };
    // The methods of this chain's own pages (local.cpp), but for those the host confirms (local::ConfirmedRpcMethods):
    // never the page's to call by itself.
    const auto& confirmed{confirm::Methods()};
    for (const auto& [name, thread] : local::RpcMethods()) {
        if (!confirmed.contains(name)) methods.emplace(name, thread);
    }
    return methods;
    }()};
    return methods;
}
} // namespace

std::optional<Bridge::Thread> PageRpcPolicy(const std::string& method)
{
    const auto it{Methods().find(method)};
    if (it == Methods().end()) return std::nullopt;
    return it->second;
}

std::vector<std::string> PageRpcMethods()
{
    std::vector<std::string> out;
    for (const auto& [name, thread] : Methods()) out.push_back(name);
    return out;
}

bool KeyedWalletRpc(const std::string& method)
{
    // The kit's own methods of the page need no keys: what does goes through the host (confirm.cpp). The chain's own pages
    // may read with them (local::KeyedRpcMethods): those of them in the policy.
    static const std::set<std::string> methods{[] {
        std::set<std::string> m;
        for (const std::string& name : local::KeyedRpcMethods()) {
            if (PageRpcPolicy(name)) m.insert(name);
        }
        return m;
    }()};
    return methods.contains(method);
}

} // namespace webui
