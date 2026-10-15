// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/rpcpolicy.h>

#include <webui/chainconfig.h>
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
        {"help", WORKER},
        {"listbanned", WORKER},
        {"uptime", WORKER},
#if WEBUI_SIDECHAIN
        // A sidechain (the Peg and Merged mining pages, the dashboard): the mainchain it follows, its
        // withdrawals and bundles, blind merged mining.
        {"createwithdrawal", WORKER},
        {"getbmminfo", WORKER},
        {"getdepositaddress", WORKER},
        {"getmainchaininfo", WORKER},
        {"getwithdrawalbundle", WORKER},
        {"listwithdrawals", WORKER},
        {"refundwithdrawal", WORKER},
        {"syncmainchain", LONG},
        // (setbmm and requestbmmblock spend the wallet of the mainchain node: host methods bmm.set and
        // bmm.request, confirmed on the desktop; sidechainapi.cpp.)
#else
        // Mining with the processor (the Mine page).
        {"getgenerate", WORKER},
        {"setgenerate", WORKER},
        // Drivechains (the Sidechains page).
        {"acksidechain", WORKER},
        {"createsidechainproposal", WORKER},
        {"getdrivechaininfo", WORKER},
        {"getsidechainevents", WORKER},
        {"listactivesidechains", WORKER},
        {"listsidechaindeposits", WORKER},
        {"listsidechainproposals", WORKER},
        {"listwithdrawalbundles", WORKER},
        {"removesidechainproposal", WORKER},
        {"setdefaultwithdrawalvote", WORKER},
        {"setwithdrawalvote", WORKER},
#endif
        // Addresses, scripts and PSBTs (multisig lounge, proof of funds, crypto tools).
        {"createpsbt", WORKER},
        {"decodepsbt", WORKER},
        {"deriveaddresses", WORKER},
        {"finalizepsbt", WORKER},
        {"getdescriptorinfo", WORKER},
        {"scantxoutset", LONG},
        {"sendrawtransaction", WORKER},
        {"utxoupdatepsbt", WORKER},
        {"validateaddress", WORKER},
        {"verifymessage", WORKER},
        // Wallets: what the pages read, and the unlock "for a while" the user asks for.
        {"getaddressinfo", WORKER},
        {"getbalances", WORKER},
        {"getnewaddress", WORKER},
        {"getwalletinfo", WORKER},
        {"listunspent", WORKER},
        {"listwallets", WORKER},
        {"walletlock", WORKER},
        {"walletpassphrase", WORKER},
        // What needs the keys: run as is by a wallet that needs no unlocking (a locked one answers -13,
        // and the page asks for the passphrase and goes through wallet.withKeys).
#if !WEBUI_SIDECHAIN
        {"createsidechaindeposit", WORKER},
#endif
        {"send", WORKER},
        {"signmessage", WORKER},
        {"walletprocesspsbt", WORKER},
    };
    // The methods of this chain's own pages (local.cpp).
    for (const auto& [name, thread] : local::RpcMethods()) methods.emplace(name, thread);
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
    static const std::set<std::string> methods{[] {
        std::set<std::string> m{"send", "signmessage", "walletprocesspsbt",
#if WEBUI_SIDECHAIN
                                "createwithdrawal", "refundwithdrawal",
#else
                                "createsidechaindeposit",
#endif
        };
        // The keyed methods of this chain's own pages (local.cpp).
        m.merge(local::KeyedRpcMethods());
        return m;
    }()};
    return methods.contains(method);
}

} // namespace webui
