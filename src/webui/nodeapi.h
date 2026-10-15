// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_NODEAPI_H
#define BITCOIN_WEBUI_NODEAPI_H

#include <univalue.h>
#include <webui/guard.h>

#include <functional>
#include <string>
#include <vector>

namespace interfaces {
class Node;
} // namespace interfaces

namespace webui {

class Bridge;

/**
 * Host methods of the node window and of the tools of chains-qt that the RPC methods alone do
 * not cover:
 *
 *   console.check {line}            the line for the history, or why it cannot run (RPCConsole)
 *   console.run {line, wallet?}     run a console line: nested commands, queries (RPCExecutor); a command
 *                                   that does more than read is confirmed in a dialog of the desktop first
 *   peers.disconnect {id}           disconnect a peer (RPCConsole::disconnectSelectedNode)
 *   peers.ban {id, seconds}         ban the address of a peer and disconnect it (banSelectedNode)
 *   bans.unban {address}            lift a ban (unbanSelectedNode)
 *   drivechain.depositAddress {text}  what a deposit destination is: plain, valid (slot), invalid
 *   tools.hashes {text, hex}        the hashes of the hash calculator (CryptoToolsDialog)
 *   tools.merkle {txids}            the levels of a merkle tree
 *   tools.decode {text}             Base58Check and Bech32 contents of a string
 *   timestamp.hashFile {path}       the SHA-256 of a file the user chose (TimestampDialog)
 *   timestamp.search {payload, from, count}  look for a stamp in `count` blocks from `from` back
 */
class NodeApi
{
public:
    using NodeFn = std::function<interfaces::Node&()>;
    //! Whether the page may read a file: only one the user chose.
    using ReadableFn = std::function<bool(const std::string& path)>;

    NodeApi(NodeFn node, ReadableFn readable, guard::ConfirmFn confirm);

    void Register(Bridge& bridge);

    /** Run a method directly (the tests; Register uses the same functions). Throws HostError. */
    UniValue Call(const std::string& method, const UniValue& params);

    std::vector<std::string> Methods() const;

private:
    NodeFn m_node;
    ReadableFn m_readable;
    guard::ConfirmFn m_confirm;
};

/** The hashes of the hash calculator for some bytes: sha256, sha256d, sha256dId, ripemd160, hash160, sha512, sha1, bytes. */
UniValue HashesOf(const std::vector<unsigned char>& data);

/** The levels of the merkle tree of transaction ids (hex, as shown), from the ids up to the root. Throws std::runtime_error. */
std::vector<std::vector<std::string>> MerkleLevels(const std::vector<std::string>& txids);

} // namespace webui

#endif // BITCOIN_WEBUI_NODEAPI_H
