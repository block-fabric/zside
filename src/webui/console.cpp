// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/console.h>

#include <rpc/client.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <webui/bridge.h>

#include <algorithm>
#include <cassert>
#include <stdexcept>
#include <vector>

namespace webui::console {
namespace {
//! Commands that handle private keys or passphrases: their arguments stay out of the history (rpcconsole.cpp historyFilter).
const std::vector<std::string> SENSITIVE{
    "createwallet",
    "createwalletdescriptor",
    "migratewallet",
    "signmessagewithprivkey",
    "signrawtransactionwithkey",
    "walletpassphrase",
    "walletpassphrasechange",
    "encryptwallet",
};
} // namespace

bool IsSensitive(const std::string& command)
{
    const std::string lower{ToLower(command)};
    return std::ranges::find(SENSITIVE, lower) != SENSITIVE.end();
}

// The parser of RPCConsole::RPCParseCommandLine, as it is there, with the wallet name as a string.
bool ParseCommandLine(const ExecuteFn* execute, std::string& strResult, const std::string& strCommand, std::string* const pstrFilteredOut, const std::string& wallet_name)
{
    std::vector<std::vector<std::string>> stack;
    stack.emplace_back();

    enum CmdParseState {
        STATE_EATING_SPACES,
        STATE_EATING_SPACES_IN_ARG,
        STATE_EATING_SPACES_IN_BRACKETS,
        STATE_ARGUMENT,
        STATE_SINGLEQUOTED,
        STATE_DOUBLEQUOTED,
        STATE_ESCAPE_OUTER,
        STATE_ESCAPE_DOUBLEQUOTED,
        STATE_COMMAND_EXECUTED,
        STATE_COMMAND_EXECUTED_INNER
    } state = STATE_EATING_SPACES;
    std::string curarg;
    UniValue lastResult;
    bool command_parsed = false;
    unsigned nDepthInsideSensitive = 0;
    size_t filter_begin_pos = 0, chpos;
    std::vector<std::pair<size_t, size_t>> filter_ranges;

    auto add_to_current_stack = [&](const std::string& strArg) {
        if (stack.back().empty() && (!nDepthInsideSensitive) && IsSensitive(strArg)) {
            nDepthInsideSensitive = 1;
            filter_begin_pos = chpos;
        }
        // Make sure stack is not empty before adding something
        if (stack.empty()) {
            stack.emplace_back();
        }
        stack.back().push_back(strArg);
    };

    auto close_out_params = [&]() {
        if (nDepthInsideSensitive) {
            if (!--nDepthInsideSensitive) {
                assert(filter_begin_pos);
                filter_ranges.emplace_back(filter_begin_pos, chpos);
                filter_begin_pos = 0;
            }
        }
        stack.pop_back();
    };

    std::string strCommandTerminated = strCommand;
    if (strCommandTerminated.empty() || strCommandTerminated.back() != '\n') strCommandTerminated += "\n";
    for (chpos = 0; chpos < strCommandTerminated.size(); ++chpos) {
        char ch = strCommandTerminated[chpos];
        switch (state) {
        case STATE_COMMAND_EXECUTED_INNER:
        case STATE_COMMAND_EXECUTED: {
            bool breakParsing = true;
            switch (ch) {
            case '[':
                curarg.clear();
                state = STATE_COMMAND_EXECUTED_INNER;
                break;
            default:
                if (state == STATE_COMMAND_EXECUTED_INNER) {
                    if (ch != ']') {
                        // append char to the current argument (which is also used for the query command)
                        curarg += ch;
                        break;
                    }
                    if (curarg.size() && execute) {
                        // if we have a value query, query arrays with index and objects with a string key
                        UniValue subelement;
                        if (lastResult.isArray()) {
                            const auto parsed{ToIntegral<size_t>(curarg)};
                            if (!parsed) {
                                throw std::runtime_error("Invalid result query");
                            }
                            subelement = lastResult[parsed.value()];
                        } else if (lastResult.isObject())
                            subelement = lastResult.find_value(curarg);
                        else
                            throw std::runtime_error("Invalid result query"); // no array or object: abort
                        lastResult = subelement;
                    }

                    state = STATE_COMMAND_EXECUTED;
                    break;
                }
                // don't break parsing when the char is required for the next argument
                breakParsing = false;

                // pop the stack and return the result to the current command arguments
                close_out_params();

                // don't stringify the json in case of a string to avoid doublequotes
                if (lastResult.isStr())
                    curarg = lastResult.get_str();
                else
                    curarg = lastResult.write(2);

                // if we have a non empty result, use it as stack argument otherwise as general result
                if (curarg.size()) {
                    if (stack.size())
                        add_to_current_stack(curarg);
                    else
                        strResult = curarg;
                }
                curarg.clear();
                // assume eating space state
                state = STATE_EATING_SPACES;
            }
            if (breakParsing) break;
            [[fallthrough]];
        }
        case STATE_ARGUMENT: // In or after argument
        case STATE_EATING_SPACES_IN_ARG:
        case STATE_EATING_SPACES_IN_BRACKETS:
        case STATE_EATING_SPACES: // Handle runs of whitespace
            switch (ch) {
            case '"': state = STATE_DOUBLEQUOTED; break;
            case '\'': state = STATE_SINGLEQUOTED; break;
            case '\\': state = STATE_ESCAPE_OUTER; break;
            case '(':
            case ')':
            case '\n':
                if (state == STATE_EATING_SPACES_IN_ARG) throw std::runtime_error("Invalid Syntax");
                if (state == STATE_ARGUMENT) {
                    if (ch == '(' && stack.size() && stack.back().size() > 0) {
                        if (nDepthInsideSensitive) {
                            ++nDepthInsideSensitive;
                        }
                        stack.emplace_back();
                    }

                    // don't allow commands after executed commands on baselevel
                    if (!stack.size()) throw std::runtime_error("Invalid Syntax");

                    add_to_current_stack(curarg);
                    curarg.clear();
                    state = STATE_EATING_SPACES_IN_BRACKETS;
                }
                if ((ch == ')' || ch == '\n') && stack.size() > 0 && stack.back().size() > 0) {
                    if (execute) {
                        // Convert argument list to JSON objects in method-dependent way,
                        // and pass it along with the method name to the dispatcher.
                        UniValue params = RPCConvertValues(stack.back()[0], std::vector<std::string>(stack.back().begin() + 1, stack.back().end()));
                        std::string method = stack.back()[0];
                        std::string uri;
                        if (!wallet_name.empty()) uri = WalletUri(wallet_name);
                        lastResult = (*execute)(method, params, uri);
                    }

                    command_parsed = true;
                    state = STATE_COMMAND_EXECUTED;
                    curarg.clear();
                }
                break;
            case ' ':
            case ',':
            case '\t':
                if (state == STATE_EATING_SPACES_IN_ARG && curarg.empty() && ch == ',')
                    throw std::runtime_error("Invalid Syntax");

                else if (state == STATE_ARGUMENT) // Space ends argument
                {
                    add_to_current_stack(curarg);
                    curarg.clear();
                }
                if ((state == STATE_EATING_SPACES_IN_BRACKETS || state == STATE_ARGUMENT) && ch == ',') {
                    state = STATE_EATING_SPACES_IN_ARG;
                    break;
                }
                state = STATE_EATING_SPACES;
                break;
            default:
                curarg += ch;
                state = STATE_ARGUMENT;
            }
            break;
        case STATE_SINGLEQUOTED: // Single-quoted string
            switch (ch) {
            case '\'': state = STATE_ARGUMENT; break;
            default: curarg += ch;
            }
            break;
        case STATE_DOUBLEQUOTED: // Double-quoted string
            switch (ch) {
            case '"': state = STATE_ARGUMENT; break;
            case '\\': state = STATE_ESCAPE_DOUBLEQUOTED; break;
            default: curarg += ch;
            }
            break;
        case STATE_ESCAPE_OUTER: // '\' outside quotes
            curarg += ch;
            state = STATE_ARGUMENT;
            break;
        case STATE_ESCAPE_DOUBLEQUOTED:                  // '\' in double-quoted text
            if (ch != '"' && ch != '\\') curarg += '\\'; // keep '\' for everything but the quote and '\' itself
            curarg += ch;
            state = STATE_DOUBLEQUOTED;
            break;
        }
    }
    if (pstrFilteredOut) {
        if (STATE_COMMAND_EXECUTED == state) {
            assert(!stack.empty());
            close_out_params();
        }
        *pstrFilteredOut = strCommand;
        for (auto i = filter_ranges.rbegin(); i != filter_ranges.rend(); ++i) {
            pstrFilteredOut->replace(i->first, i->second - i->first, "(…)");
        }
    }
    switch (state) // final state
    {
    case STATE_COMMAND_EXECUTED:
        if (lastResult.isStr())
            strResult = lastResult.get_str();
        else
            strResult = lastResult.write(2);
        [[fallthrough]];
    case STATE_ARGUMENT:
        return true;
    case STATE_EATING_SPACES:
        // Reaching this state without ever parsing a command means the line
        // held no command name (e.g. ")", "()", "(", ","); treat it as invalid.
        return command_parsed;
    default: // ERROR to end in one of the other states
        return false;
    }
}

const std::string& HelpConsole()
{
    static const std::string text{
        "\n"
        "This console accepts RPC commands using the standard syntax.\n"
        "   example:    getblockhash 0\n\n"

        "This console can also accept RPC commands using the parenthesized syntax.\n"
        "   example:    getblockhash(0)\n\n"

        "Commands may be nested when specified with the parenthesized syntax.\n"
        "   example:    getblock(getblockhash(0) 1)\n\n"

        "A space or a comma can be used to delimit arguments for either syntax.\n"
        "   example:    getblockhash 0\n"
        "               getblockhash,0\n\n"

        "Named results can be queried with a non-quoted key string in brackets using the parenthesized syntax.\n"
        "   example:    getblock(getblockhash(0) 1)[tx]\n\n"

        "Results without keys can be queried with an integer in brackets using the parenthesized syntax.\n"
        "   example:    getblock(getblockhash(0),1)[tx][0]\n\n"};
    return text;
}

UniValue Run(const ExecuteFn& execute, const std::string& line, const std::string& wallet)
{
    UniValue out{UniValue::VOBJ};
    const auto fail{[&](const std::string& error) {
        out.pushKV("ok", false);
        out.pushKV("error", error);
        return out;
    }};
    try {
        std::string result;
        const std::string executable{line + "\n"};
        // Catch the console-only help command before the RPC call and reply with its text as an RPC reply would.
        if (executable == "help-console\n") {
            out.pushKV("ok", true);
            out.pushKV("result", HelpConsole());
            return out;
        }
        if (!ParseCommandLine(&execute, result, executable, nullptr, wallet)) {
            return fail("Parse error: unbalanced ' or \"");
        }
        out.pushKV("ok", true);
        out.pushKV("result", result);
        return out;
    } catch (const UniValue& objError) {
        // Nice formatting for standard-format errors; raw JSON for anything else.
        const UniValue& code{objError.find_value("code")};
        const UniValue& message{objError.find_value("message")};
        if (code.isNum() && message.isStr()) return fail(strprintf("%s (code %d)", message.get_str(), code.getInt<int>()));
        return fail(objError.write());
    } catch (const std::exception& e) {
        return fail(std::string{"Error: "} + e.what());
    }
}

UniValue Check(const std::string& line)
{
    UniValue out{UniValue::VOBJ};
    std::string filtered;
    try {
        std::string dummy;
        if (!ParseCommandLine(nullptr, dummy, line, &filtered)) {
            // Failed to parse command, so we cannot even filter it for the history
            throw std::runtime_error("Invalid command line");
        }
    } catch (const std::exception& e) {
        out.pushKV("ok", false);
        out.pushKV("error", std::string{"Error: "} + e.what());
        return out;
    }
    out.pushKV("ok", true);
    out.pushKV("filtered", filtered);
    return out;
}

} // namespace webui::console
