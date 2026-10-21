// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_MAINCLIENT_H
#define BITCOIN_SIDECHAIN_MAINCLIENT_H

#include <univalue.h>
#include <util/fs.h>
#include <util/result.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

namespace sidechain {

/** A call to the mainchain node failed: it could not be reached, or it answered with an error. */
class MainClientError : public std::runtime_error
{
public:
    //! Whether the node was reached and refused the call; if not, the node could not be reached.
    const bool rpc_error;
    //! The message of the node's error, as it gave it, if it refused the call.
    const std::string rpc_message;
    explicit MainClientError(const std::string& message, bool rpc_error_in, std::string rpc_message_in = {})
        : std::runtime_error{message}, rpc_error{rpc_error_in}, rpc_message{std::move(rpc_message_in)} {}
};

/** JSON-RPC client for the mainchain node that this sidechain node follows. */
class MainClient
{
public:
    struct Options {
        std::string host{"127.0.0.1"};
        uint16_t port{0};
        //! "user:password"; if empty, the cookie file is read for every call.
        std::string credentials;
        fs::path cookie_file;
        //! Name of the mainchain wallet that pays for merged mining; unset to use the only loaded wallet.
        std::string wallet;
        bool wallet_set{false};
    };

    explicit MainClient(Options options) : m_options{std::move(options)} {}

    /**
     * Call a method of the mainchain node and return its result.
     * @param[in] wallet  whether the method is one of the wallet
     * @throws MainClientError
     */
    UniValue Call(const std::string& method, const UniValue& params, bool wallet = false) const;

    const Options& GetOptions() const { return m_options; }

    /**
     * What the pages show of the peg of the sidechain in `slot` (getpeginfo): getdrivechaininfo, getsidechain and
     * listwithdrawalbundles of the mainchain node, read by PegInfoFromRpc.
     * @throws MainClientError, std::runtime_error
     */
    UniValue PegInfo(uint32_t slot) const;

private:
    const Options m_options;
};

/**
 * Read the password for the mainchain node from a file (-mainchainrpcpasswordfile), so that it is never
 * on the command line, where any local user can read it.
 *
 * The password is the first line of the file, without its line ending. On Unix the file must be a regular
 * file owned by this user and readable by no one else (no group or other permission bit), or it is refused.
 * On Windows its access list is not checked: keep it in a directory only this user can read (the data
 * directory is). Errors name the file but never contain what it holds.
 */
util::Result<std::string> ReadPasswordFile(const fs::path& path);

/**
 * getpeginfo from the replies of a mainchain node over JSON-RPC: `drivechain` (getdrivechaininfo), `sidechain`
 * (getsidechain of the slot; null if none is active in it) and `bundles` (listwithdrawalbundles of the slot). With
 * the enforcer's rules (Backbone on eCash), the score a bundle needs and the blocks it has are those rules': more
 * votes than bundlethreshold within bundlemaxage blocks. What a reply does not say is left out, nothing made up.
 */
UniValue PegInfoFromRpc(uint32_t slot, const UniValue& drivechain, const UniValue& sidechain, const UniValue& bundles);

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_MAINCLIENT_H
