// Copyright (c) 2015-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_HTTPRPC_H
#define BITCOIN_HTTPRPC_H

#include <any>
#include <string>

class JSONRPCRequest;
class UniValue;
enum HTTPStatusCode : int;

/** Start HTTP RPC subsystem.
 * Precondition; HTTP and RPC has been started.
 */
bool StartHTTPRPC(const std::any& context);
/** Interrupt HTTP RPC subsystem.
 */
void InterruptHTTPRPC();
/** Stop HTTP RPC subsystem.
 * Precondition; HTTP and RPC has been stopped.
 */
void StopHTTPRPC();

/** Whether an RPC user may call a method (-rpcwhitelist, -rpcwhitelistdefault), as the HTTP RPC server decides it: also
 * for the methods run by name inside a call of that user (webuihost of webui/remotehost.h). */
bool RpcUserMayCall(const std::string& user, const std::string& method);

/** Execute a single HTTP request containing one or more JSONRPC requests.
 * Specified `jreq` will be modified and `status` will be returned.
 */
UniValue ExecuteHTTPRPC(const UniValue& valRequest, JSONRPCRequest& jreq, HTTPStatusCode& status);

/** Start HTTP REST subsystem.
 * Precondition; HTTP and RPC has been started.
 */
void StartREST(const std::any& context);
/** Interrupt RPC REST subsystem.
 */
void InterruptREST();
/** Stop HTTP REST subsystem.
 * Precondition; HTTP and RPC has been stopped.
 */
void StopREST();

#endif // BITCOIN_HTTPRPC_H
