// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_NODERPC_H
#define BITCOIN_QT_NODERPC_H

#include <univalue.h>

#include <QString>

#include <functional>
#include <initializer_list>
#include <optional>
#include <string>

class ClientModel;
class QObject;
class QTableWidgetItem;

namespace NodeRpc {
/** Returns the name of the wallet the user is looking at, if there is one. */
using WalletNameFn = std::function<std::optional<QString>()>;

/**
 * Run an RPC method of the node this GUI belongs to.
 * @param[in]  wallet  name of the wallet to run a wallet method on
 * @param[out] error   set when the call fails
 */
std::optional<UniValue> Call(ClientModel* client_model, const std::string& method, const UniValue& params, QString& error, const std::optional<QString>& wallet = std::nullopt);

/** What CallAsync does with the result, on the GUI thread: the result, or nothing and the error. */
using Done = std::function<void(std::optional<UniValue> result, const QString& error)>;

/**
 * Run an RPC method like Call, but on a thread of its own, one call after the other (as the RPC
 * console does): some take long, a BMM request waits on the mainchain node for up to a minute.
 * `done` runs on the GUI thread afterwards, unless `receiver` was deleted meanwhile.
 */
void CallAsync(QObject* receiver, ClientModel* client_model, const std::string& method, const UniValue& params, Done done,
               const std::optional<QString>& wallet = std::nullopt);

/**
 * Run `work` on the thread of CallAsync, after the calls queued before it. What it returns runs on
 * the GUI thread afterwards, unless `receiver` was deleted meanwhile. `work` must not touch widgets.
 */
void RunAsync(QObject* receiver, std::function<std::function<void()>()> work);

/**
 * Stop the thread of CallAsync, before the node shuts down (BitcoinApplication::requestShutdown):
 * the calls still queued are skipped, the one under way is waited for, and later ones are dropped.
 * GUI thread only.
 */
void Stop();

/**
 * Take calls again after Stop, on a new thread: for the tests, where a node is set up again after
 * another one shut down (AppTests). GUI thread only.
 */
void Restart();

/** The parameters of a call. */
UniValue Args(std::initializer_list<UniValue> values);
/** Text of a string or number. */
QString Text(const UniValue& value);
/** A table cell that cannot be edited. */
QTableWidgetItem* Item(const QString& text);
/** Hash rate with a unit, like "12.3 MH/s". */
QString HashRate(double hashes_per_second);
} // namespace NodeRpc

#endif // BITCOIN_QT_NODERPC_H
