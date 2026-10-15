// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host side of the pages of zSide (src/webui/app/src/chain/local.ts): the RPC methods of the shielded
// pool and the shielded wallet they call (src/rpc/shielded.cpp, src/wallet/rpc/shielded.cpp). They need
// no host method of their own.

#include <webui/local.h>

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
        // An address of the account (a new one looks unrelated to the others); the first call of a locked
        // wallet whose account is not set up yet sets it up with the keys.
        {"getshieldedaddress", Bridge::Thread::WORKER},
        // The viewing key of the account: the page shows it after a warning, never stores it.
        {"exportshieldedviewingkey", Bridge::Thread::WORKER},
        // A viewing key watched in a wallet without keys: the blocks are scanned again, which takes long.
        {"importshieldedviewingkey", Bridge::Thread::LONG},
        // Payments out of the pool and into it: each proves a bundle (Halo 2, seconds), on the long lane
        // so that the quick calls of the pages never wait behind a proof.
        {"sendshielded", Bridge::Thread::LONG},
        {"shieldcoins", Bridge::Thread::LONG},
        {"unshieldcoins", Bridge::Thread::LONG},
        {"bumpshieldedfee", Bridge::Thread::LONG},
        // Melt & cast: plans the wallet carries out as the blocks come.
        {"meltcoins", Bridge::Thread::WORKER},
        {"castcoins", Bridge::Thread::WORKER},
        {"cancelmeltcast", Bridge::Thread::WORKER},
    };
}

std::set<std::string> KeyedRpcMethods()
{
    // Each spends with the keys of the account (or locks the coins it plans to melt); getshieldedaddress sets
    // the account up from the keys the first time.
    return {"sendshielded", "shieldcoins", "unshieldcoins", "bumpshieldedfee", "meltcoins", "getshieldedaddress"};
}

void RegisterHost(Bridge& bridge, NodeFn node)
{
    (void)bridge;
    (void)node;
}

} // namespace webui::local
