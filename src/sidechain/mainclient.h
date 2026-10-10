// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_SIDECHAIN_MAINCLIENT_H
#define BITCOIN_SIDECHAIN_MAINCLIENT_H

#include <univalue.h>
#include <util/fs.h>

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

private:
    const Options m_options;
};

} // namespace sidechain

#endif // BITCOIN_SIDECHAIN_MAINCLIENT_H
