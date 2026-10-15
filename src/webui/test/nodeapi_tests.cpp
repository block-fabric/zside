// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <addresstype.h>
#include <base58.h>
#include <common/args.h>
#include <crypto/sha256.h>
#include <key_io.h>
#include <interfaces/node.h>
#include <node/context.h>
#include <rpc/server.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <validation.h>
#include <chainparams.h>
#include <webui/appconfig.h>
#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/console.h>
#include <webui/guard.h>
#include <webui/nodeapi.h>
#include <webui/options.h>
#include <webui/sidechainnodes.h>

#include <boost/test/unit_test.hpp>

#include <atomic>
#include <chrono>
#include <deque>
#include <fstream>
#include <set>
#include <thread>

#include <sys/stat.h>

using webui::console::Check;
using webui::console::ParseCommandLine;
using webui::console::Run;

using namespace std::chrono_literals;

namespace {

/** An executor that answers "rpcNestedTest" with its params as JSON text (as qt/test/rpcnestedtests.cpp), "echo" with them. */
UniValue Echo(const std::string& method, const UniValue& params, const std::string& uri)
{
    if (method == "rpcNestedTest") return UniValue{params.write()};
    if (method == "echo") return params;
    if (method == "uri") return UniValue{uri};
    UniValue error{UniValue::VOBJ};
    error.pushKV("code", -32601);
    error.pushKV("message", "Method not found");
    throw error;
}

std::string Execute(const webui::console::ExecuteFn& fn, const std::string& line, std::string* filtered = nullptr)
{
    std::string result;
    BOOST_REQUIRE(ParseCommandLine(&fn, result, line, filtered));
    return result;
}

std::string Filtered(const std::string& line)
{
    std::string result, filtered;
    ParseCommandLine(nullptr, result, line, &filtered);
    return filtered;
}

UniValue Obj(std::initializer_list<std::pair<std::string, UniValue>> kvs)
{
    UniValue out{UniValue::VOBJ};
    for (const auto& [k, v] : kvs) out.pushKV(k, v);
    return out;
}

[[maybe_unused]] void WriteScript(const fs::path& path, const std::string& body)
{
    {
        std::ofstream file{path.std_path()};
        file << "#!/bin/sh\n" << body;
    }
    chmod(fs::PathToString(path).c_str(), 0755);
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_nodeapi_tests, TestChain100Setup)

BOOST_AUTO_TEST_CASE(console_parsing)
{
    // The cases of the console of chains-qt (qt/test/rpcnestedtests.cpp), with an executor that echoes.
    const webui::console::ExecuteFn echo{Echo};
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest"), "[]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest ''"), "[\"\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest \"\""), "[\"\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest '' abc"), "[\"\",\"abc\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest abc '' abc"), "[\"abc\",\"\",\"abc\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest abc  abc"), "[\"abc\",\"abc\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest abc\t\tabc"), "[\"abc\",\"abc\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest(abc )"), "[\"abc\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest( abc )"), "[\"abc\"]");
    BOOST_CHECK_EQUAL(Execute(echo, "rpcNestedTest(   abc   ,   cba )"), "[\"abc\",\"cba\"]");
    // Escapes: outside quotes anything, in double quotes only " and a backslash.
    BOOST_CHECK_EQUAL(Execute(echo, R"(rpcNestedTest a\ b "c\"d" "e\nf" 'g\h')"), R"(["a b","c\"d","e\\nf","g\\h"])");
    // Nesting and queries.
    BOOST_CHECK_EQUAL(Execute(echo, "echo(echo(x y)[1])"), "[\n  \"y\"\n]");
    BOOST_CHECK_EQUAL(Execute(echo, "echo(a b)[0]"), "a");
    // The wallet goes into the URI.
    std::string result;
    BOOST_CHECK(ParseCommandLine(&echo, result, "uri", nullptr, "my wallet"));
    BOOST_CHECK_EQUAL(result, "/wallet/my%20wallet");
    BOOST_CHECK(ParseCommandLine(&echo, result, "uri", nullptr, ""));
    BOOST_CHECK_EQUAL(result, "");

    BOOST_CHECK_THROW(Execute(echo, "echo() .\n"), std::runtime_error);
    BOOST_CHECK_THROW(Execute(echo, "echo() echo()"), std::runtime_error);
    BOOST_CHECK_THROW(Execute(echo, "rpcNestedTest abc,,abc"), std::runtime_error);
    BOOST_CHECK_THROW(Execute(echo, "rpcNestedTest(abc,,abc)"), std::runtime_error);
    BOOST_CHECK_THROW(Execute(echo, "rpcNestedTest(abc,,)"), std::runtime_error);
    BOOST_CHECK_THROW(Execute(echo, "echo(a)[x]"), std::runtime_error);
    BOOST_CHECK_THROW(Execute(echo, "nosuchmethod"), UniValue);
    for (const char* bad : {")", "()", "(", ","}) {
        BOOST_CHECK(!ParseCommandLine(&echo, result, bad));
    }
    BOOST_CHECK(!ParseCommandLine(nullptr, result, "echo \"unbalanced"));
}

BOOST_AUTO_TEST_CASE(console_history_filter)
{
    // Arguments of sensitive commands never go into the history (rpcnestedtests.cpp).
    BOOST_CHECK_EQUAL(Filtered("getblockchaininfo()[chain]"), "getblockchaininfo()[chain]");
    BOOST_CHECK_EQUAL(Filtered("createwallet test true"), "createwallet(…)");
    BOOST_CHECK_EQUAL(Filtered("createwalletdescriptor abc"), "createwalletdescriptor(…)");
    BOOST_CHECK_EQUAL(Filtered("migratewallet abc abc"), "migratewallet(…)");
    BOOST_CHECK_EQUAL(Filtered("signmessagewithprivkey abc"), "signmessagewithprivkey(…)");
    BOOST_CHECK_EQUAL(Filtered("signmessagewithprivkey abc,def"), "signmessagewithprivkey(…)");
    BOOST_CHECK_EQUAL(Filtered("signrawtransactionwithkey(abc)"), "signrawtransactionwithkey(…)");
    BOOST_CHECK_EQUAL(Filtered("walletpassphrase(help())"), "walletpassphrase(…)");
    BOOST_CHECK_EQUAL(Filtered("walletpassphrasechange(help(walletpassphrasechange(abc)))"), "walletpassphrasechange(…)");
    BOOST_CHECK_EQUAL(Filtered("help(encryptwallet(abc, def))"), "help(encryptwallet(…))");
    BOOST_CHECK_EQUAL(Filtered("WalletPassphrase secret 60"), "WalletPassphrase(…)");
    BOOST_CHECK(webui::console::IsSensitive("encryptwallet"));
    BOOST_CHECK(!webui::console::IsSensitive("getbalance"));

    const UniValue ok{Check("walletpassphrase \"my secret\" 600")};
    BOOST_CHECK(ok["ok"].get_bool());
    BOOST_CHECK_EQUAL(ok["filtered"].get_str(), "walletpassphrase(…)");
    const UniValue bad{Check("getblock(,)")};
    BOOST_CHECK(!bad["ok"].get_bool());
    BOOST_CHECK_EQUAL(bad["error"].get_str(), "Error: Invalid Syntax");
    BOOST_CHECK_EQUAL(Check(")")["error"].get_str(), "Error: Invalid command line");
}

BOOST_AUTO_TEST_CASE(console_on_a_node)
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto node{interfaces::MakeNode(m_node)};
    const webui::console::ExecuteFn execute{[&](const std::string& m, const UniValue& p, const std::string& uri) { return node->executeRpc(m, p, uri); }};
    BOOST_CHECK_EQUAL(Execute(execute, "getblockchaininfo()[chain]"), "regtest");
    const std::string tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip()->GetBlockHash().GetHex())};
    BOOST_CHECK_EQUAL(Execute(execute, "getbestblockhash"), tip);
    BOOST_CHECK_EQUAL(Execute(execute, "getblock(getbestblockhash())[height]"), "100");
    BOOST_CHECK_EQUAL(Execute(execute, "getblock( getblock( getblockhash(100) )[hash], true)[hash]"), tip);
    // A parameter converted by the method (a number) and a JSON one.
    BOOST_CHECK_EQUAL(Execute(execute, "createrawtransaction [] {} 0"), Execute(execute, "createrawtransaction( [],  {} , 0   )"));

    // Run answers as RPCExecutor: the text, or the error with its code.
    UniValue r{Run(execute, "getblockcount", "")};
    BOOST_CHECK(r["ok"].get_bool());
    BOOST_CHECK_EQUAL(r["result"].get_str(), "100");
    r = Run(execute, "getblockhash 99999", "");
    BOOST_CHECK(!r["ok"].get_bool());
    BOOST_CHECK_EQUAL(r["error"].get_str(), "Block height out of range (code -8)");
    // A parameter the method wants as JSON, that is not.
    BOOST_CHECK_EQUAL(Run(execute, "getblockhash x", "")["error"].get_str(), "Error: Error parsing JSON: x");
    r = Run(execute, "nosuchcommand", "");
    BOOST_CHECK_EQUAL(r["error"].get_str(), "Method not found (code -32601)");
    r = Run(execute, "getblockhash 1 \"", "");
    BOOST_CHECK_EQUAL(r["error"].get_str(), "Parse error: unbalanced ' or \"");
    r = Run(execute, "getblock(getbestblockhash())[nosuchkey][1]", "");
    BOOST_CHECK_EQUAL(r["error"].get_str(), "Error: Invalid result query");
    r = Run(execute, "help-console", "");
    BOOST_CHECK(r["result"].get_str().find("getblock(getblockhash(0),1)[tx][0]") != std::string::npos);
    // A wallet that is not loaded.
    r = Run(execute, "getbalance", "nowallet");
    BOOST_CHECK(!r["ok"].get_bool());
}

BOOST_AUTO_TEST_CASE(console_asks_the_desktop_first)
{
    // Audit port-1 #3: the page may call console.run with any line. A command that does more than read
    // waits for a confirmation of the desktop, named in it; declined, it does not run.
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto node{interfaces::MakeNode(m_node)};
    std::vector<webui::Window::ConfirmDialog> asked;
    bool answer{false};
    webui::NodeApi api{[&]() -> interfaces::Node& { return *node; }, [](const std::string&) { return false; }, [&](const webui::Window::ConfirmDialog& d) {
        asked.push_back(d);
        return answer;
    }};
    const auto run{[&](const std::string& line, const std::string& wallet = {}) {
        UniValue params{UniValue::VOBJ};
        params.pushKV("line", line);
        if (!wallet.empty()) params.pushKV("wallet", wallet);
        return api.Call("console.run", params);
    }};

    // Reading runs at once, nested too.
    BOOST_CHECK_EQUAL(run("getblock(getbestblockhash())[height]")["result"].get_str(), "100");
    BOOST_CHECK_EQUAL(run("help getblockcount")["ok"].get_bool(), true);
    BOOST_CHECK(asked.empty());

    // A command that changes the node: asked, declined, not run.
    const int tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height())};
    UniValue r{run("generatetodescriptor 1 \"raw(51)\"")};
    BOOST_CHECK(!r["ok"].get_bool());
    BOOST_CHECK_EQUAL(r["error"].get_str(), "generatetodescriptor was not run: it was declined (code -1)");
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Height()), tip);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(asked[0].title, "Run a command of the console");
    BOOST_CHECK(asked[0].message.starts_with("Command: generatetodescriptor\n\n"));
    BOOST_CHECK_EQUAL(asked[0].accept, "Run");

    // Inside a reading one, it is asked too; accepted, it runs, and is asked once for the line.
    asked.clear();
    answer = true;
    r = run("echo(setban(1.2.3.4, add), setban(1.2.3.5, add))");
    BOOST_CHECK_MESSAGE(r["ok"].get_bool(), r.write());
    BOOST_CHECK(run("listbanned")["result"].get_str().find("1.2.3.5/32") != std::string::npos);
    BOOST_CHECK_EQUAL(asked.size(), 1U);

    // The wallet is named in the dialog on one line; marks that turn text around are spaces there.
    asked.clear();
    answer = false;
    run("stop", "evil\nCommand: getblockcount\xe2\x80\xae" "");
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK(asked[0].message.starts_with("Command: stop\n\n"));
    BOOST_CHECK(asked[0].message.find("on the wallet \"evil Command: getblockcount\"") != std::string::npos);

    // Commands that can show private keys are asked even though they read.
    for (const std::string m : {"listdescriptors", "gethdkeys", "dumpwallet", "sendtoaddress", "walletpassphrase", "importdescriptors", "backupwallet", "addnode", "signrawtransactionwithwallet", "walletprocesspsbt", "signmessage", "setban"}) {
        BOOST_CHECK_MESSAGE(!webui::guard::ConsoleRunsQuietly(m), m);
    }
    for (const std::string m : {"getbalance", "listtransactions", "decodepsbt", "estimatesmartfee", "verifymessage", "help", "getblockchaininfo", "createpsbt"}) {
        BOOST_CHECK_MESSAGE(webui::guard::ConsoleRunsQuietly(m), m);
    }
    // A name that is no command is never put in a dialog.
    BOOST_CHECK(!webui::guard::ConsoleConfirmation("get\nblock", ""));
    BOOST_CHECK(!webui::guard::ConsoleRunsQuietly("GETBALANCE"));
}

BOOST_AUTO_TEST_CASE(open_url_asks_the_desktop_first)
{
    // Audit port-1 #3: a web address the page names is opened on the user's word, its host first.
    const auto dialog{webui::guard::OpenUrlConfirmation("https://explorer.example/tx/00?x=1")};
    BOOST_REQUIRE(dialog);
    BOOST_CHECK_EQUAL(dialog->title, "Open a web address");
    BOOST_CHECK(dialog->message.starts_with("Host: explorer.example\n\n"));
    BOOST_CHECK(dialog->message.ends_with("https://explorer.example/tx/00?x=1"));
    const auto http{webui::guard::OpenUrlConfirmation("http://Example.com:8080/a")};
    BOOST_REQUIRE(http);
    BOOST_CHECK(http->message.starts_with("Host: example.com\n\n"));
    BOOST_CHECK(http->message.find("not encrypted") != std::string::npos);
    // Refused outright: other schemes, a user name before the host, spaces, quotes.
    for (const std::string url : {"file:///etc/passwd", "javascript:alert(1)", "https://good.example@evil.example/", "https://a b/", "https://a\"b/", "https://", "chains:x"}) {
        BOOST_CHECK_THROW(webui::guard::OpenUrlConfirmation(url), std::invalid_argument);
    }
    // The app's own links open without asking (none for Chains).
    for (const std::string& link : webui::guard::KnownLinks()) BOOST_CHECK(!webui::guard::OpenUrlConfirmation(link));
    // Text in a dialog: one line, cut.
    BOOST_CHECK_EQUAL(webui::guard::OneLine("a\n\xe2\x80\xae" "b\tc  d", 64), "a b c d");
    BOOST_CHECK_EQUAL(webui::guard::OneLine("abcdef", 4), "abc…");
}

BOOST_AUTO_TEST_CASE(node_api)
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    auto node{interfaces::MakeNode(m_node)};
    std::set<std::string> readable;
    webui::NodeApi api{[&]() -> interfaces::Node& { return *node; }, [&](const std::string& path) { return readable.contains(path); }, [](const webui::Window::ConfirmDialog&) { return false; }};

    // The hash calculator: text, and the bytes of hexadecimal (cryptotoolsdialog.cpp; the tests of chains-qt).
    UniValue h{api.Call("tools.hashes", Obj({{"text", "abc"}, {"hex", false}}))};
    BOOST_CHECK_EQUAL(h["sha256"].get_str(), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    BOOST_CHECK_EQUAL(h["sha256d"].get_str(), "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");
    BOOST_CHECK_EQUAL(h["sha1"].get_str(), "a9993e364706816aba3e25717850c26c9cd0d89d");
    BOOST_CHECK_EQUAL(h["ripemd160"].get_str(), "8eb208f7e05d987a9b044a8e98c6b087f15a0bfc");
    BOOST_CHECK_EQUAL(h["bytes"].getInt<int>(), 3);
    BOOST_CHECK_EQUAL(api.Call("tools.hashes", Obj({{"text", "61 62\n63"}, {"hex", true}}))["sha256"].get_str(), h["sha256"].get_str());
    BOOST_CHECK_EQUAL(api.Call("tools.hashes", Obj({{"text", "6x"}, {"hex", true}}))["error"].get_str(), "This is not hexadecimal data.");

    // The merkle tree of the transactions of the tip is its merkle root.
    const CBlockIndex* tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain().Tip())};
    UniValue block{node->executeRpc("getblock", [&] { UniValue p{UniValue::VARR}; p.push_back(tip->GetBlockHash().GetHex()); return p; }(), "")};
    UniValue txids{UniValue::VARR};
    for (const UniValue& id : block["tx"].getValues()) txids.push_back(id);
    UniValue merkle{api.Call("tools.merkle", Obj({{"txids", txids}}))};
    BOOST_CHECK_EQUAL(merkle["levels"].getValues().back()[0].get_str(), tip->hashMerkleRoot.GetHex());
    // Three ids: the third is paired with itself.
    const auto levels{webui::MerkleLevels({std::string(64, '1'), std::string(64, '2'), std::string(64, '3')})};
    BOOST_CHECK_EQUAL(levels.size(), 3U);
    BOOST_CHECK_EQUAL(levels[1].size(), 2U);
    UniValue bad_ids{UniValue::VARR};
    bad_ids.push_back("xyz");
    BOOST_CHECK_EQUAL(api.Call("tools.merkle", Obj({{"txids", bad_ids}}))["error"].get_str(), "\"xyz\" is not a transaction id (64 hexadecimal characters).");

    // The address decoder.
    const std::string address{EncodeDestination(WitnessV0KeyHash{coinbaseKey.GetPubKey()})};
    const UniValue decoded{api.Call("tools.decode", Obj({{"text", address}}))};
    BOOST_CHECK_EQUAL(decoded["bech32"]["prefix"].get_str(), Params().Bech32HRP());
    BOOST_CHECK_EQUAL(decoded["bech32"]["encoding"].get_str(), "bech32");
    BOOST_CHECK_EQUAL(decoded["bech32"]["witnessVersion"].getInt<int>(), 0);
    BOOST_CHECK_EQUAL(decoded["bech32"]["witnessProgram"].get_str(), HexStr(coinbaseKey.GetPubKey().GetID()));
    std::vector<unsigned char> versioned{111};
    for (const unsigned char c : coinbaseKey.GetPubKey().GetID()) versioned.push_back(c);
    const UniValue base58{api.Call("tools.decode", Obj({{"text", EncodeBase58Check(versioned)}}))};
    BOOST_CHECK_EQUAL(base58["base58"]["version"].getInt<int>(), 111);
    BOOST_CHECK_EQUAL(base58["base58"]["payload"].get_str(), HexStr(coinbaseKey.GetPubKey().GetID()));
    BOOST_CHECK(!api.Call("tools.decode", Obj({{"text", "hello"}})).exists("base58"));

    // Deposit addresses (the checks of the sidechain page).
    UniValue deposit{api.Call("drivechain.depositAddress", Obj({{"text", " s1_erin_b91ea9 "}}))};
    BOOST_CHECK_EQUAL(deposit["kind"].get_str(), "valid");
    BOOST_CHECK_EQUAL(deposit["slot"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(deposit["address"].get_str(), "erin");
    BOOST_CHECK_EQUAL(api.Call("drivechain.depositAddress", Obj({{"text", "s1_eric_b91ea9"}}))["kind"].get_str(), "invalid");
    BOOST_CHECK_EQUAL(api.Call("drivechain.depositAddress", Obj({{"text", "alice"}}))["kind"].get_str(), "plain");

    // Files: only the ones the user chose.
    const fs::path file{m_args.GetDataDirBase() / "stamp-me.txt"};
    {
        std::ofstream out{file.std_path()};
        out << "chains";
    }
    BOOST_CHECK_THROW(api.Call("timestamp.hashFile", Obj({{"path", fs::PathToString(file)}})), webui::HostError);
    readable.insert(fs::PathToString(file));
    const UniValue stamped{api.Call("timestamp.hashFile", Obj({{"path", fs::PathToString(file)}}))};
    BOOST_CHECK_EQUAL(stamped["sha256"].get_str(), webui::HashesOf({'c', 'h', 'a', 'i', 'n', 's'})["sha256"].get_str());
    BOOST_CHECK_EQUAL(stamped["size"].getInt<int>(), 6);

    // The search for a stamp walks back from a block, a number of blocks at a time.
    const UniValue search{api.Call("timestamp.search", Obj({{"payload", "5354414d50" + stamped["sha256"].get_str()}, {"from", tip->GetBlockHash().GetHex()}, {"count", 30}}))};
    BOOST_CHECK_EQUAL(search["searched"].getInt<int>(), 30);
    BOOST_CHECK(search["found"].empty());
    BOOST_CHECK_EQUAL(search["next"].get_str(), tip->GetAncestor(70)->GetBlockHash().GetHex());
    const UniValue to_genesis{api.Call("timestamp.search", Obj({{"payload", "00"}, {"from", tip->GetAncestor(5)->GetBlockHash().GetHex()}, {"count", 100}}))};
    BOOST_CHECK_EQUAL(to_genesis["searched"].getInt<int>(), 6);
    BOOST_CHECK(to_genesis["next"].isNull());

    // Peers and bans: no peer 7 here; a ban of a subnet lifted.
    BOOST_CHECK(!api.Call("peers.ban", Obj({{"id", 7}, {"seconds", 3600}})).get_bool());
    BOOST_CHECK(!api.Call("peers.disconnect", Obj({{"id", 7}})).get_bool());
    BOOST_CHECK_THROW(api.Call("bans.unban", Obj({{"address", "not an address"}})), webui::HostError);
}

#if WEBUI_SIDECHAIN
BOOST_AUTO_TEST_CASE(mainchain_host_on_another_machine_asks_the_desktop)
{
    // Audit port-1 #4: the node sends the credentials of the mainchain node to the host of mainchainConnect,
    // unencrypted. A host that is not this machine waits for the desktop; declined, nothing changes.
    auto node{interfaces::MakeNode(m_node)};
    std::vector<webui::Window::ConfirmDialog> asked;
    bool answer{false};
    webui::NodeOptions options{*node, *m_node.args, [](std::optional<bool>) { return false; }, {}, {}, {}, [&](const webui::Window::ConfirmDialog& d) {
        asked.push_back(d);
        return answer;
    }};
    for (const std::string local : {"127.0.0.1", "127.1.2.3", "localhost", "::1", ""}) {
        options.Set("mainchainConnect", local);
        BOOST_CHECK_EQUAL(options.Get()["mainchainConnect"].get_str(), local);
    }
    BOOST_CHECK(asked.empty());
    BOOST_CHECK_EXCEPTION(options.Set("mainchainConnect", "evil.example"), std::runtime_error, [](const std::runtime_error& e) {
        return std::string{e.what()}.find("declined") != std::string::npos;
    });
    BOOST_CHECK_EQUAL(options.Get()["mainchainConnect"].get_str(), "");
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK(asked[0].message.starts_with("Host: evil.example\n\n"));
    BOOST_CHECK(asked[0].message.find("unencrypted") != std::string::npos);
    answer = true;
    options.Set("mainchainConnect", "192.168.1.20");
    BOOST_CHECK_EQUAL(options.Get()["mainchainConnect"].get_str(), "192.168.1.20");
    BOOST_CHECK_EQUAL(asked.size(), 2U);
    // Text that is no host is refused before any dialog.
    for (const std::string bad : {"a b", "x@y", "h/p", "Host: x\nevil", "\xe2\x80\xae" "abc"}) {
        BOOST_CHECK_THROW(options.Set("mainchainConnect", bad), std::runtime_error);
    }
    BOOST_CHECK_EQUAL(asked.size(), 2U);
    // Without a desktop to ask, another machine is refused.
    webui::NodeOptions blind{*node, *m_node.args, [](std::optional<bool>) { return false; }};
    BOOST_CHECK_THROW(blind.Set("mainchainConnect", "10.0.0.1"), std::runtime_error);
    options.Set("mainchainConnect", "");
}
#endif

BOOST_AUTO_TEST_CASE(node_options)
{
    auto node{interfaces::MakeNode(m_node)};
    std::optional<bool> restart;
    // What the file chooser and the confirmation of the desktop give for a program (nothing: cancelled).
    std::optional<std::string> program;
    int program_asked{0};
    // The answers of the confirmations of the desktop (accepted unless told), and what they asked.
    std::deque<bool> answers;
    std::vector<std::string> asked;
    webui::NodeOptions options{*node, *m_node.args, [&](std::optional<bool> set) {
        if (set) restart = *set;
        return restart.value_or(false);
    }, {}, [&](const std::string&, const std::string&) {
        ++program_asked;
        return program;
    }, {}, [&](const webui::Window::ConfirmDialog& d) {
        asked.push_back(d.title + ": " + d.message);
        if (answers.empty()) return true;
        const bool yes{answers.front()};
        answers.pop_front();
        return yes;
    }};
    UniValue now{options.Get()};
    BOOST_CHECK(!now["prune"].get_bool());
    BOOST_CHECK_EQUAL(now["pruneSize"].getInt<int>(), 2);
    BOOST_CHECK(!now["proxyUse"].get_bool());
    BOOST_CHECK_EQUAL(now["proxyIp"].get_str(), "127.0.0.1");
    BOOST_CHECK_EQUAL(now["proxyPort"].get_str(), "9050");
    BOOST_CHECK(!now["restartRequired"].get_bool());
    BOOST_CHECK_EQUAL(now["limits"]["dbcacheMin"].getInt<int>(), 4);

    // The database cache: written as a string, as OptionsModel does, and a restart is needed.
    now = options.Set("dbcache", 500);
    BOOST_CHECK_EQUAL(node->getPersistentSetting("dbcache").write(), "\"500\"");
    BOOST_CHECK_EQUAL(now["dbcache"].getInt<int>(), 500);
    BOOST_CHECK(now["restartRequired"].get_bool());
    BOOST_CHECK_THROW(options.Set("dbcache", 1), std::runtime_error);
    BOOST_CHECK_THROW(options.Set("par", 1000), std::runtime_error);

    // Prune: the size set while off waits in prune-prev, and comes back when it is turned on.
    restart.reset();
    options.Set("pruneSize", 7);
    BOOST_CHECK(!restart.value_or(false));
    BOOST_CHECK_EQUAL(options.Get()["pruneSize"].getInt<int>(), 7);
    BOOST_CHECK(!options.Get()["prune"].get_bool());
    options.Set("prune", true);
    BOOST_CHECK(options.Get()["prune"].get_bool());
    BOOST_CHECK_EQUAL(options.Get()["pruneSize"].getInt<int>(), 7);
    BOOST_CHECK(node->getPersistentSetting("prune-prev").isNull());
    BOOST_CHECK(restart.value_or(false));
    options.Set("prune", false);
    BOOST_CHECK_EQUAL(node->getPersistentSetting("prune").write(), "\"0\"");
    BOOST_CHECK_EQUAL(options.Get()["pruneSize"].getInt<int>(), 7);

    // A proxy: its address while it is off goes to proxy-prev (nothing asked: it is not used); on, to proxy.
    options.Set("proxyIp", "10.0.0.2");
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy-prev").get_str(), "10.0.0.2:9050");
    BOOST_CHECK(!options.Get()["proxyUse"].get_bool());
    BOOST_CHECK(asked.empty());
    // Turning it on asks the desktop, saying where all connections would go (audit 2026-10-14 item 5);
    // declined, nothing changes.
    answers = {false};
    UniValue declined{options.Set("proxyUse", true)};
    BOOST_CHECK(declined["declined"].get_bool());
    BOOST_CHECK(!declined["proxyUse"].get_bool());
    BOOST_CHECK(node->getPersistentSetting("proxy").isNull());
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK(asked[0].find("Route all connections through 10.0.0.2:9050?") != std::string::npos);
    options.Set("proxyUse", true);
    BOOST_CHECK_EQUAL(asked.size(), 2U);
    BOOST_CHECK(!options.Get().exists("declined"));
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.2:9050");
    // A new address of the proxy in use: asked, with the new address and the one now.
    answers = {false};
    BOOST_CHECK(options.Set("proxyPort", 9150)["declined"].get_bool());
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.2:9050");
    BOOST_CHECK(asked.back().find("through 10.0.0.2:9150?") != std::string::npos);
    BOOST_CHECK(asked.back().find("It is 10.0.0.2:9050 now.") != std::string::npos);
    options.Set("proxyPort", 9150);
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.2:9150");
    // The address and the port at once: one question.
    const size_t before{asked.size()};
    options.Set("proxyAddress", Obj({{"ip", "10.0.0.3"}, {"port", 1080}}));
    BOOST_CHECK_EQUAL(asked.size(), before + 1);
    BOOST_CHECK(asked.back().find("through 10.0.0.3:1080?") != std::string::npos);
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.3:1080");
    options.Set("proxyAddress", Obj({{"ip", "10.0.0.2"}, {"port", 9150}}));
    // No user name or password in a proxy address.
    BOOST_CHECK_THROW(options.Set("proxyIp", "user:pass@10.0.0.9"), std::runtime_error);
    BOOST_CHECK_THROW(options.Set("proxyAddress", Obj({{"ip", "u@10.0.0.9"}, {"port", 1080}})), std::runtime_error);
    BOOST_CHECK(!webui::NodeOptions::ValidProxyAddress("user:pass@10.0.0.9"));
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.2:9150");
    // Turning it off asks nothing.
    const size_t before_off{asked.size()};
    options.Set("proxyUse", false);
    BOOST_CHECK_EQUAL(asked.size(), before_off);
    BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "");

    // A change made while a confirmation is open (another call of the page) waits for it: what is turned
    // on is the address the user was shown, never one set meanwhile (review of 598f5c70ca, TOCTOU).
    {
        std::atomic<bool> other_done{false};
        std::thread other;
        UniValue other_result;
        std::vector<std::string> other_asked;
        // The second call's own confirmation: declined (its address is not the one the user wants).
        webui::NodeOptions second{*node, *m_node.args, [](std::optional<bool>) { return false; }, {}, {}, {}, [&](const webui::Window::ConfirmDialog& d) {
            const std::string& message{d.message};
            other_asked.push_back(message);
            return false;
        }};
        webui::NodeOptions first{*node, *m_node.args, [](std::optional<bool>) { return false; }, {}, {}, {}, [&](const webui::Window::ConfirmDialog& d) {
            const std::string& message{d.message};
            BOOST_CHECK(message.find("Route all connections through 10.0.0.2:9150?") != std::string::npos);
            // While the dialog is open, another call sets an address of its own (the proxy is still off).
            other = std::thread{[&] {
                other_result = second.Set("proxyIp", "10.6.6.6");
                other_done = true;
            }};
            std::this_thread::sleep_for(300ms);
            BOOST_CHECK(!other_done);
            return true;
        }};
        first.Set("proxyUse", true);
        // Turned on: the address the user accepted.
        BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.2:9150");
        other.join();
        // Then the other change ran: the proxy now in use, its new address had to be asked, and was declined.
        BOOST_REQUIRE_EQUAL(other_asked.size(), 1U);
        BOOST_CHECK(other_asked[0].find("10.6.6.6:9150") != std::string::npos);
        BOOST_CHECK(other_result["declined"].get_bool());
        BOOST_CHECK_EQUAL(node->getPersistentSetting("proxy").get_str(), "10.0.0.2:9150");
        options.Set("proxyUse", false);
    }
    BOOST_CHECK_EQUAL(options.Get()["proxyIp"].get_str(), "10.0.0.2");
    BOOST_CHECK_EQUAL(options.Get()["proxyPort"].get_str(), "9150");
    BOOST_CHECK_THROW(options.Set("proxyIp", "not a proxy"), std::runtime_error);
    BOOST_CHECK_THROW(options.Set("torPort", 70000), std::runtime_error);
    BOOST_CHECK(webui::NodeOptions::ValidProxyAddress("127.0.0.1"));
    BOOST_CHECK(webui::NodeOptions::ValidProxyAddress("[::1]"));
    BOOST_CHECK(!webui::NodeOptions::ValidProxyAddress("127.0.0.1:9050"));
    BOOST_CHECK(!webui::NodeOptions::ValidProxyAddress("x y"));

    // Flags and text.
    options.Set("listen", false);
    BOOST_CHECK(!options.Get()["listen"].get_bool());
    // The RPC server turned on: asked; declined, it stays off; off again, nothing asked.
    answers = {false};
    BOOST_CHECK(options.Set("server", true)["declined"].get_bool());
    BOOST_CHECK(!options.Get()["server"].get_bool());
    BOOST_CHECK(asked.back().find("Turn on the RPC server?") != std::string::npos);
    options.Set("server", true);
    BOOST_CHECK(options.Get()["server"].get_bool());
    const size_t before_server_off{asked.size()};
    options.Set("server", false);
    BOOST_CHECK_EQUAL(asked.size(), before_server_off);
    options.Set("server", true);
    // The Tor proxy: asked too, with its own question.
    answers = {false};
    BOOST_CHECK(options.Set("torUse", true)["declined"].get_bool());
    BOOST_CHECK(asked.back().find("Reach Tor onion services through 127.0.0.1:9050?") != std::string::npos);
    BOOST_CHECK(!options.Get()["torUse"].get_bool());
    // Without a way to ask the desktop, these changes are refused.
    webui::NodeOptions unasked{*node, *m_node.args, [](std::optional<bool>) { return false; }};
    BOOST_CHECK_THROW(unasked.Set("torUse", true), std::runtime_error);
    BOOST_CHECK(!options.Get()["torUse"].get_bool());
    // The external signer is a program the node runs: the page cannot name one (audit 2026-10-14 item 2b),
    // only the file chooser and the confirmation of the desktop can. The page can take it away.
    BOOST_CHECK_THROW(options.Set("signer", "/usr/local/bin/hwi"), std::runtime_error);
    BOOST_CHECK_EQUAL(options.Get()["signer"].get_str(), "");
    BOOST_CHECK_EQUAL(program_asked, 0);
    program.reset();
    BOOST_CHECK(options.ChooseProgram("signer").isNull());
    BOOST_CHECK_EQUAL(program_asked, 1);
    BOOST_CHECK_EQUAL(options.Get()["signer"].get_str(), "");
    program = "/usr/local/bin/hwi";
    BOOST_CHECK_EQUAL(options.ChooseProgram("signer")["signer"].get_str(), "/usr/local/bin/hwi");
    BOOST_CHECK_EQUAL(node->getPersistentSetting("signer").get_str(), "/usr/local/bin/hwi");
    BOOST_CHECK_THROW(options.ChooseProgram("dbcache"), std::runtime_error);
    for (const char* name : {"blocknotify", "walletnotify", "alertnotify", "startupnotify", "shutdownnotify"}) {
        BOOST_CHECK_THROW(options.Set(name, "/bin/sh"), std::runtime_error);
        BOOST_CHECK_THROW(options.ChooseProgram(name), std::runtime_error);
    }
    options.Set("signer", "");
    BOOST_CHECK_EQUAL(options.Get()["signer"].get_str(), "");
    BOOST_CHECK_THROW(options.Set("listen", "yes"), std::runtime_error);
    BOOST_CHECK_THROW(options.Set("nosuchoption", true), std::runtime_error);

    // A setting on the command line wins, and is listed.
    m_node.args->LockSettings([](common::Settings& settings) { settings.command_line_options["par"] = {"3"}; });
    BOOST_CHECK(options.Get()["ignored"].exists("par"));
    BOOST_CHECK(options.Get()["overridden"].get_str().find("-par=3 ") != std::string::npos);
    m_node.args->LockSettings([](common::Settings& settings) { settings.command_line_options.erase("par"); });
}

#ifndef WIN32
// The programs of the sidechain are shell scripts here.
BOOST_AUTO_TEST_CASE(sidechain_nodes)
{
    // The network of the node, as chains-app has it on the command line.
    m_node.args->ForceSetArg("-chain", "regtest");
    UniValue stored{UniValue::VOBJ};
    std::optional<std::string> folder;
    // The confirmations of the desktop: the answer given, and what they showed.
    bool accept{false};
    std::vector<std::string> asked;
    std::vector<std::string> asked_titles;
    const fs::path release_archive{m_args.GetDataDirBase() / "release.tar.gz"};
    std::vector<webui::SidechainNodes::Release> releases{{"thunder", webui::SidechainNodes::Platform(), "1.0", "https://example.invalid/thunder-1.0.tar.gz", std::string(64, 'b')}};
    webui::SidechainNodes nodes{*m_node.args, [&] { return stored; }, [&](const UniValue& v) { stored = v; }, [&](const std::string&, const std::string&) { return folder; },
                                [&](const std::string& title, const std::string& message, const std::string&) {
                                    asked_titles.push_back(title);
                                    asked.push_back(message);
                                    return accept;
                                },
                                releases};

    // A sidechain whose programs are scripts: the node writes where it was started, the client answers.
    const fs::path bin{m_args.GetDataDirBase() / "fakebin"};
    fs::create_directories(bin);
    const fs::path started{m_args.GetDataDirBase() / "started.txt"};
    WriteScript(bin / "faked", "echo \"$@\" > '" + fs::PathToString(started) + "'\n");
    WriteScript(bin / "fake-cli", R"(for a in "$@"; do last="$a"; done
case "$*" in
  *getblockcount*) echo 42 ;;
  *getmainchaininfo*) echo '{"connected": true, "height": 100}' ;;
  *getbmminfo*) echo '{"mining": true, "idle": true, "blocks": 3}' ;;
  *getnewaddress*) echo sideaddress ;;
  *requestbmmblock*) echo '{"transactions": 3, "amount": 0.0001}' ;;
  *stop*) echo "Fake stopping" ;;
  *) echo "error: unknown command" >&2; exit 1 ;;
esac
)");

    BOOST_CHECK_EQUAL(nodes.Sidechains().size(), 4U);
    nodes.Call("sidechainNodes.add", Obj({{"name", "Fake chain"}, {"stem", "fake"}, {"slot", 5}}));
    BOOST_CHECK_EQUAL(nodes.Sidechains().size(), 5U);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Again"}, {"stem", "fake"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Bad"}, {"stem", "../evil"}, {"slot", 6}})), webui::HostError);
    // A name is one line of at most 64 printable characters: it is shown in the confirmation of a
    // download, where fake lines would pass for what the app says (audit 2026-10-14 item 4).
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Evil\nFrom: https://github.com/official/x.tar.gz"}, {"stem", "evil1"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Evil\r\n\n\n\nThis is the release built into this app."}, {"stem", "evil2"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Tab\there"}, {"stem", "evil3"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Turned \xe2\x80\xae" "elif"}, {"stem", "evil4"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", "Line\xe2\x80\xa8two"}, {"stem", "evil5"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.add", Obj({{"name", std::string(65, 'x')}, {"stem", "evil6"}, {"slot", 6}})), webui::HostError);
    BOOST_CHECK_EQUAL(nodes.Sidechains().size(), 5U);
    BOOST_CHECK(webui::SidechainNodes::IsValidName(std::string(64, 'x')));
    BOOST_CHECK(webui::SidechainNodes::IsValidName("Chaîne d'été \xe2\x80\x94 2"));
    BOOST_CHECK(!webui::SidechainNodes::IsValidName(""));
    BOOST_CHECK(!webui::SidechainNodes::IsValidName("bad \xff utf-8"));
    // A name in the settings that did not come through add (written by hand) is cleaned up where it is shown.
    {
        UniValue tampered{stored};
        UniValue custom{tampered["custom"]};
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("name", "Evil\nFrom: https://github.com/official/x.tar.gz\n\n\n" + std::string(100, 'y'));
        entry.pushKV("stem", "evil");
        entry.pushKV("slot", 7);
        custom.push_back(entry);
        UniValue saved{stored};
        tampered.pushKV("custom", custom); // pushKV replaces the key
        stored = tampered;
        const std::string shown{nodes.Sidechains().back().name};
        BOOST_CHECK(shown.find('\n') == std::string::npos);
        BOOST_CHECK(shown.starts_with("Evil From: https://github.com/official/x.tar.gz y"));
        BOOST_CHECK(shown.ends_with("\xe2\x80\xa6"));
        stored = saved;
    }
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DisplayName("  a \t\r\n b\xe2\x80\xae c  "), "a b c");
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DisplayName(std::string(64, 'z')), std::string(64, 'z'));
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DisplayName(std::string(65, 'z')), std::string(63, 'z') + "\xe2\x80\xa6");
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DisplayName("\n\n"), "(no name)");
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DisplayName("a\xff" "b"), "a\xef\xbf\xbd" "b");
    // The host of a download: https, no user name, plain letters.
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DownloadHost("https://GitHub.com/a/b.tar.gz").value_or("-"), "github.com");
    BOOST_CHECK_EQUAL(webui::SidechainNodes::DownloadHost("https://example.com:8443?x").value_or("-"), "example.com");
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://github.com@evil.example/x.tar.gz"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://user:pw@evil.example/x.tar.gz"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://evil.example:/x"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://evil.example:x/"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https:///x"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://[::1]/x"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://ex%41mple.com/x"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("https://g\xc3\xaethub.com/x"));
    BOOST_CHECK(!webui::SidechainNodes::DownloadHost("http://example.com/x"));

    UniValue list{nodes.Call("sidechainNodes.list", UniValue{UniValue::VOBJ})};
    BOOST_CHECK(!list["sidechains"][4]["installed"].get_bool());
    BOOST_CHECK_EQUAL(list["sidechains"][4]["programs"].get_str(), "not on this computer yet: Locate or Download");
    BOOST_CHECK(!nodes.Call("sidechainNodes.status", Obj({{"stem", "fake"}}))["running"].get_bool());

    // Locate: a folder without the node is refused; cancelled is nothing.
    folder = fs::PathToString(m_args.GetDataDirBase());
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.locate", Obj({{"stem", "fake"}})), webui::HostError);
    folder.reset();
    BOOST_CHECK(nodes.Call("sidechainNodes.locate", Obj({{"stem", "fake"}})).isNull());
    folder = fs::PathToString(bin);
    BOOST_CHECK_EQUAL(nodes.Call("sidechainNodes.locate", Obj({{"stem", "fake"}}))["log"][0].get_str(), "Fake chain: programs in " + fs::PathToString(bin));
    BOOST_CHECK(nodes.Call("sidechainNodes.list", UniValue{UniValue::VOBJ})["sidechains"][4]["installed"].get_bool());

    const UniValue status{nodes.Call("sidechainNodes.status", Obj({{"stem", "fake"}}))};
    BOOST_CHECK(status["running"].get_bool());
    BOOST_CHECK_EQUAL(status["blocks"].getInt<int>(), 42);
    BOOST_CHECK_EQUAL(status["mainchain"].get_str(), "yes, at block 100");
    BOOST_CHECK_EQUAL(status["miningText"].get_str(), "auto, waiting for fees, 3 blocks");

    // The node starts in the background, pointed at this node, on the network of this node.
    BOOST_CHECK_EQUAL(nodes.Call("sidechainNodes.start", Obj({{"stem", "fake"}, {"wallet", "w1"}}))["log"][0].get_str(), "Fake chain: node started");
    for (int i{0}; i < 100 && !fs::exists(started); ++i) std::this_thread::sleep_for(std::chrono::milliseconds{50});
    std::ifstream args_file{started.std_path()};
    std::string args;
    std::getline(args_file, args);
    BOOST_CHECK_MESSAGE(args.find("-regtest -sidechainslot=5") != std::string::npos, args);
    BOOST_CHECK(args.find("-mainchainrpcwallet=w1") != std::string::npos);
    BOOST_CHECK(args.find("-mainchainrpccookiefile=") != std::string::npos);
    BOOST_CHECK(args.ends_with("-daemon"));

    BOOST_CHECK_EQUAL(nodes.Call("sidechainNodes.mineOnce", Obj({{"stem", "fake"}, {"amount", "0.0001"}}))["log"][0].get_str(),
                      "Fake chain: asked for one block with 2 transactions, offering 0.0001. It is mined if the next block of this chain takes the offer.");
    BOOST_CHECK_EQUAL(nodes.Call("sidechainNodes.stop", Obj({{"stem", "fake"}}))["log"][0].get_str(), "Fake chain: stopping");
    BOOST_CHECK(nodes.Call("sidechainNodes.log", Obj({{"stem", "fake"}}))["log"][0].get_str().starts_with("Fake chain has no log at "));

    // With an RPC password (-rpcpassword), the node gets the path of a file only the user can read, never the
    // password: any local user can read the command line of a process (audit 2026-10-14 item 1).
    {
        const webui::SidechainNodes::Sidechain fake{nodes.Sidechains().back()};
        BOOST_REQUIRE_EQUAL(fake.stem, "fake");
        const fs::path password_file{nodes.PasswordFile(fake)};
        BOOST_CHECK(password_file == nodes.DataDir(fake) / "mainchain.rpcpass");
        {
            std::ofstream stale{password_file.std_path()};
            stale << "stale\n";
        }
        fs::permissions(password_file, fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read, fs::perm_options::replace);
        m_node.args->ForceSetArg("-rpcuser", "mainuser");
        m_node.args->ForceSetArg("-rpcpassword", "pw-on-argv-would-leak");
        fs::remove(started);
        nodes.Call("sidechainNodes.start", Obj({{"stem", "fake"}}));
        for (int i{0}; i < 100 && !fs::exists(started); ++i) std::this_thread::sleep_for(std::chrono::milliseconds{50});
        std::ifstream argv_file{started.std_path()};
        std::string argv;
        std::getline(argv_file, argv);
        BOOST_CHECK_MESSAGE(argv.find("pw-on-argv-would-leak") == std::string::npos, argv);
        BOOST_CHECK_MESSAGE(argv.find("-mainchainrpcpassword=") == std::string::npos, argv);
        BOOST_CHECK(argv.find("-mainchainrpcuser=mainuser") != std::string::npos);
        BOOST_CHECK_MESSAGE(argv.find("-mainchainrpcpasswordfile=" + fs::PathToString(password_file)) != std::string::npos, argv);
        for (const auto& a : nodes.NodeArgs(fake, std::nullopt)) BOOST_CHECK(a.find("pw-on-argv-would-leak") == std::string::npos);
        std::ifstream written{password_file.std_path()};
        std::string content;
        std::getline(written, content);
        BOOST_CHECK_EQUAL(content, "pw-on-argv-would-leak");
        BOOST_CHECK(fs::status(password_file).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
        m_node.args->LockSettings([](common::Settings& settings) {
            settings.forced_settings.erase("rpcuser");
            settings.forced_settings.erase("rpcpassword");
        });
        // Back on the cookie file: the password file goes.
        nodes.Call("sidechainNodes.start", Obj({{"stem", "fake"}}));
        BOOST_CHECK(!fs::exists(password_file));
    }

    // Downloads only over https, only with a hash to check, and only once the user accepted them in a
    // dialog of the desktop showing the address, the hash and the folder (audit 2026-10-14 item 2a).
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", "file:///etc/passwd"}, {"sha256", std::string(64, 'a')}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", "http://example.com/x.tar.gz"}, {"sha256", std::string(64, 'a')}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", "https://example.com/x.tar.gz"}, {"sha256", "abc"}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", "https://example.com/x.tar.gz"}, {"sha256", ""}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"release", true}})), webui::HostError);
    // A user name before the host hides where the download comes from (audit 2026-10-14 item 4).
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", "https://github.com@evil.example/x.tar.gz"}, {"sha256", std::string(64, 'a')}})), webui::HostError);
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", "https://github.com:443@evil.example/x.tar.gz"}, {"sha256", std::string(64, 'a')}})), webui::HostError);
    BOOST_CHECK(asked.empty());
    {
        // curl and tar are scripts that say they ran: curl "downloads" a local archive.
        const fs::path tools{m_args.GetDataDirBase() / "fakepath"};
        fs::create_directories(tools);
        const fs::path ran{m_args.GetDataDirBase() / "ran.txt"};
        const fs::path source{m_args.GetDataDirBase() / "archive-source"};
        fs::create_directories(source / "fake-1.0" / "bin");
        WriteScript(source / "fake-1.0" / "bin" / "faked", "exit 0\n");
        BOOST_REQUIRE_EQUAL(webui::SidechainNodes::Run({"tar", "-czf", fs::PathToString(release_archive), "-C", fs::PathToString(source), "fake-1.0"}, 30).code, 0);
        WriteScript(tools / "curl", "echo curl \"$@\" >> '" + fs::PathToString(ran) + "'\nout=''\nprev=''\nfor a in \"$@\"; do [ \"$prev\" = --output ] && out=\"$a\"; prev=\"$a\"; done\ncp '" + fs::PathToString(release_archive) + "' \"$out\"\n");
        WriteScript(tools / "tar", "echo tar >> '" + fs::PathToString(ran) + "'\nexec /usr/bin/env -u FAKE PATH=/usr/bin:/bin tar \"$@\"\n");
        const std::string old_path{std::getenv("PATH") ? std::getenv("PATH") : ""};
        setenv("PATH", (fs::PathToString(tools) + ":" + old_path).c_str(), 1);
        const auto ran_text{[&] {
            std::ifstream f{ran.std_path()};
            return std::string{std::istreambuf_iterator<char>{f}, {}};
        }};
        std::ifstream archive_in{release_archive.std_path(), std::ios::binary};
        const std::string archive_bytes{std::istreambuf_iterator<char>{archive_in}, {}};
        unsigned char digest[CSHA256::OUTPUT_SIZE];
        CSHA256().Write(reinterpret_cast<const unsigned char*>(archive_bytes.data()), archive_bytes.size()).Finalize(digest);
        const std::string good_hash{HexStr(digest)};
        const std::string url{"https://example.invalid/fake-1.0.tar.gz"};

        // Refused in the dialog of the desktop: nothing is downloaded.
        accept = false;
        BOOST_CHECK_EQUAL(nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", url}, {"sha256", good_hash}}))["log"][0].get_str(), "Fake chain: the download was not accepted.");
        BOOST_REQUIRE_EQUAL(asked.size(), 1U);
        // The shape of the dialog: the title is the trust statement of the app, in fixed words; the first
        // line is the host alone; what the page gave (the name, the address) comes after.
        BOOST_CHECK_EQUAL(asked_titles[0], "Install from an address typed in this app");
        {
            const auto lines{util::SplitString(asked[0], '\n')};
            BOOST_REQUIRE_GE(lines.size(), 5U);
            BOOST_CHECK_EQUAL(lines[0], "Host: example.invalid");
            BOOST_CHECK_EQUAL(lines[1], "This address was typed in the app. It is NOT a release built into this app.");
            BOOST_CHECK_EQUAL(lines[2], "");
            BOOST_CHECK_EQUAL(lines[3], "Sidechain: Fake chain");
            BOOST_CHECK_EQUAL(lines[4], "From: " + url);
        }
        BOOST_CHECK(asked[0].find(url) != std::string::npos);
        BOOST_CHECK(asked[0].find(good_hash) != std::string::npos);
        BOOST_CHECK(asked[0].find(fs::PathToString(m_args.GetDataDirBase() / "sidechains" / "fake")) != std::string::npos);
        BOOST_CHECK(ran_text().empty());

        // Accepted, but the archive does not have the hash: thrown away before anything is unpacked.
        accept = true;
        const UniValue mismatch{nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", url}, {"sha256", std::string(64, 'c')}}))};
        BOOST_CHECK(mismatch.exists("error"));
        BOOST_CHECK(ran_text().find("curl") != std::string::npos);
        BOOST_CHECK(ran_text().find("--proto =https --proto-redir =https") != std::string::npos);
        BOOST_CHECK_MESSAGE(ran_text().find("\ntar\n") == std::string::npos, ran_text());
        BOOST_CHECK(!fs::exists(m_args.GetDataDirBase() / "sidechains" / "fake" / "download.tar.gz"));
        BOOST_CHECK(!fs::exists(m_args.GetDataDirBase() / "sidechains" / "fake" / "fake-1.0"));

        // Accepted, with its hash: unpacked and installed.
        const UniValue installed{nodes.Call("sidechainNodes.download", Obj({{"stem", "fake"}, {"url", url}, {"sha256", good_hash}}))};
        BOOST_CHECK_MESSAGE(installed["log"][installed["log"].size() - 1].get_str().starts_with("Fake chain: installed in"), installed.write());
        BOOST_CHECK_EQUAL(asked.size(), 3U);
        BOOST_CHECK(ran_text().find("\ntar\n") != std::string::npos);

        // The release built in: offered in the list, its address and hash shown in the confirmation.
        const UniValue entries{nodes.Call("sidechainNodes.list", UniValue{UniValue::VOBJ})["sidechains"]};
        BOOST_CHECK_EQUAL(entries[0]["stem"].get_str(), "thunder");
        BOOST_CHECK_EQUAL(entries[0]["release"]["url"].get_str(), "https://example.invalid/thunder-1.0.tar.gz");
        BOOST_CHECK(!entries[1].exists("release"));
        accept = false;
        nodes.Call("sidechainNodes.download", Obj({{"stem", "thunder"}, {"release", true}}));
        BOOST_CHECK(asked.back().find("https://example.invalid/thunder-1.0.tar.gz") != std::string::npos);
        BOOST_CHECK(asked.back().find(std::string(64, 'b')) != std::string::npos);
        BOOST_CHECK(asked.back().starts_with("Host: example.invalid\nThis is the release 1.0 built into this app.\n\nSidechain: Thunder\n"));
        BOOST_CHECK_EQUAL(asked_titles.back(), "Install a release built into this app");
        setenv("PATH", old_path.c_str(), 1);
    }

    // Built-in sidechains stay; added ones go, with their folder.
    BOOST_CHECK_THROW(nodes.Call("sidechainNodes.remove", Obj({{"stem", "thunder"}})), webui::HostError);
    nodes.Call("sidechainNodes.remove", Obj({{"stem", "fake"}}));
    BOOST_CHECK_EQUAL(nodes.Sidechains().size(), 4U);
    BOOST_CHECK(nodes.BinDir("fake").empty());

    // A program that does not end in time is stopped; one that is not there says so.
    const auto slow{webui::SidechainNodes::Run({"/bin/sh", "-c", "sleep 5"}, 1)};
    BOOST_CHECK_EQUAL(slow.code, -2);
    BOOST_CHECK_EQUAL(webui::SidechainNodes::Run({"/no/such/program"}, 5).code, -1);
    const auto echo{webui::SidechainNodes::Run({"/bin/sh", "-c", "echo out; echo err >&2; exit 3"}, 5)};
    BOOST_CHECK_EQUAL(echo.code, 3);
    BOOST_CHECK_EQUAL(echo.out, "out\n");
    BOOST_CHECK_EQUAL(echo.err, "err\n");
    BOOST_CHECK(!webui::SidechainNodes::Launch({"/no/such/program"}));
    m_node.args->LockSettings([](common::Settings& settings) { settings.forced_settings.erase("chain"); });
}
#endif

BOOST_AUTO_TEST_CASE(app_values)
{
    webui::AppConfig config{m_args.GetDataDirBase() / "app.json"};
    config.Load();
    UniValue geometry{UniValue::VOBJ};
    geometry.pushKV("width", 900);
    config.SetValue("window", geometry);
    BOOST_CHECK_THROW(config.SetValue("prefs", geometry), std::runtime_error);
    BOOST_CHECK_THROW(config.SetValue("datadir", geometry), std::runtime_error);
    webui::AppConfig again{m_args.GetDataDirBase() / "app.json"};
    again.Load();
    BOOST_CHECK_EQUAL(again.Value("window")["width"].getInt<int>(), 900);
    BOOST_CHECK(again.Value("prefs").isNull());
    again.SetValue("window", UniValue{});
    BOOST_CHECK(again.Value("window").isNull());
}

BOOST_AUTO_TEST_SUITE_END()

// --- The changes of the Settings page wait for Apply (audit M7) ---

BOOST_FIXTURE_TEST_SUITE(webui_options_journal_tests, TestingSetup)

BOOST_AUTO_TEST_CASE(cancel_and_apply)
{
    auto node{interfaces::MakeNode(m_node)};
    webui::AppConfig config{m_args.GetDataDirBase() / "chains-app-journal.json"};
    webui::OptionsJournal journal{config, *m_node.args, [] { return std::string{"optionsUndo:regtest"}; }};
    std::optional<bool> restart;
    webui::NodeOptions options{*node, *m_node.args, [&](std::optional<bool> set) {
        if (set) restart = *set;
        return restart.value_or(false);
    }, [&](const std::string& setting) { journal.Record(*node, setting, restart.value_or(false)); }};

    // A stray click on "Prune block storage", and a database cache.
    BOOST_CHECK_EQUAL(journal.Pending(), 0U);
    options.Set("prune", true);
    options.Set("dbcache", 700);
    options.Set("dbcache", 800);
    BOOST_CHECK(node->getPersistentSetting("prune").isStr());
    BOOST_CHECK_EQUAL(node->getPersistentSetting("dbcache").write(), "\"800\"");
    BOOST_CHECK(restart.value_or(false));
    // prune, prune-prev and dbcache changed; the journal is in the settings of the app (a crash keeps it).
    BOOST_CHECK_EQUAL(journal.Pending(), 3U);
    webui::AppConfig reread{m_args.GetDataDirBase() / "chains-app-journal.json"};
    reread.Load();
    BOOST_CHECK(reread.Value("optionsUndo:regtest").isObject());

    // Cancel: all of it as it was, the restart flag too.
    std::optional<bool> restored_restart;
    BOOST_CHECK_EQUAL(journal.Undo(*node, [&](bool r) { restored_restart = r; }), 3U);
    BOOST_CHECK(node->getPersistentSetting("prune").isNull());
    BOOST_CHECK(node->getPersistentSetting("dbcache").isNull());
    BOOST_CHECK(!options.Get()["prune"].get_bool());
    BOOST_CHECK(restored_restart.has_value() && !*restored_restart);
    BOOST_CHECK_EQUAL(journal.Pending(), 0U);
    BOOST_CHECK_EQUAL(journal.Undo(*node, {}), 0U);

    // Apply: the changes stay, and nothing is left to take back.
    options.Set("dbcache", 600);
    BOOST_CHECK_EQUAL(journal.Pending(), 1U);
    journal.Commit();
    BOOST_CHECK_EQUAL(journal.Pending(), 0U);
    BOOST_CHECK_EQUAL(journal.Undo(*node, {}), 0U);
    BOOST_CHECK_EQUAL(node->getPersistentSetting("dbcache").write(), "\"600\"");

    // A value set before the changes comes back as it was, not removed.
    options.Set("dbcache", 900);
    journal.Undo(*node, {});
    BOOST_CHECK_EQUAL(node->getPersistentSetting("dbcache").write(), "\"600\"");
}

BOOST_AUTO_TEST_SUITE_END()
