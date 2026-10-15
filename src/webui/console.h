// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_CONSOLE_H
#define BITCOIN_WEBUI_CONSOLE_H

#include <univalue.h>

#include <functional>
#include <string>

namespace webui {

/**
 * The RPC console of the node window, as the one of chains-qt (rpcconsole.cpp
 * RPCConsole::RPCParseCommandLine and RPCExecutor), without Qt.
 */
namespace console {

//! Runs one RPC method: the method, its params, the URI ("/wallet/<name>" or empty). Throws a UniValue error object.
using ExecuteFn = std::function<UniValue(const std::string& method, const UniValue& params, const std::string& uri)>;

/**
 * Split a command line into commands and their arguments, and run them if `execute` is given,
 * as a shell would (and as chains-qt does):
 *
 * - commands nest with parentheses: validateaddress(getnewaddress())
 * - arguments are separated by spaces or commas
 * - "double" and 'single' quotes; a backslash escapes outside quotes, and before " or \ in double quotes
 * - a result is queried with [key] or [index]: getblock(getbestblockhash())[tx][0]
 *
 * @param[in]  execute    runs the commands; without it the line is only parsed
 * @param[out] result     the result of the last command, as text
 * @param[out] filtered   the line with the arguments of sensitive commands (passphrases, keys) replaced by "(…)"
 * @param[in]  wallet     the wallet the commands run on; empty for none
 * @return false when the line does not end in a complete command (unbalanced quotes, no command)
 * @throws std::runtime_error on a syntax error, a UniValue error object when a command fails
 */
bool ParseCommandLine(const ExecuteFn* execute, std::string& result, const std::string& command, std::string* filtered = nullptr, const std::string& wallet = {});

/** Whether a command is one whose arguments never go into the history (createwallet, walletpassphrase...). */
bool IsSensitive(const std::string& command);

/** The text of help-console. */
const std::string& HelpConsole();

/**
 * Run a line of the console as RPCExecutor::request does: {"ok": true, "result": text} or
 * {"ok": false, "error": text}, with the messages of chains-qt ("Parse error: unbalanced ' or \"",
 * "<message> (code <n>)", "Error: <what>"). help-console is answered here.
 */
UniValue Run(const ExecuteFn& execute, const std::string& line, const std::string& wallet);

/**
 * Check a line before it runs (RPCConsole::on_lineEdit_returnPressed): {"ok": true, "filtered": line for the history}
 * or {"ok": false, "error": text}.
 */
UniValue Check(const std::string& line);

} // namespace console
} // namespace webui

#endif // BITCOIN_WEBUI_CONSOLE_H
