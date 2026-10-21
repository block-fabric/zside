// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The host of the pages over RPC (webui/remotehost.h): the tokens that bind a question the Chains app showed to the
// call it then runs, the digest of a call and its question, the events kept for the app.

#include <webui/bridge.h>
#include <webui/confirm.h>
#include <webui/remotehost.h>

#include <random.h>
#include <tinyformat.h>
#include <univalue.h>
#include <util/fs.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace webui;
using namespace webui::remote;
using namespace std::chrono_literals;

namespace {
Window::ConfirmDialog Question(const std::string& to = "rladdr1", const std::string& amount = "1.00000000 LYR")
{
    Window::ConfirmDialog dialog;
    dialog.title = "Send";
    dialog.message = "The wallet pays.";
    dialog.accept = "Send";
    dialog.rows = {confirm::Text("Wallet", "main"), confirm::Destination("Pay to", to), {"Amount", amount, dialog::Row::Kind::AMOUNT}};
    return dialog;
}

const std::string REQUEST{R"(["host","wallet.send.commit",{"wallet":"main","id":"1"},"main"])"};
const auto T0{std::chrono::steady_clock::time_point{} + 1000h};
} // namespace

BOOST_AUTO_TEST_SUITE(remotehost_tests)

BOOST_AUTO_TEST_CASE(token_runs_exactly_once)
{
    Bindings bindings;
    const std::string digest{Digest(REQUEST, Question())};
    const std::string token{bindings.Issue(REQUEST, digest, "w1", T0)};
    BOOST_CHECK_EQUAL(token.size(), 64U);
    BOOST_CHECK(bindings.Take(token, REQUEST, digest, "w1", T0 + 10s) == Bindings::Check::OK);
    // Replay: the same token, the same call, again: refused.
    BOOST_CHECK(bindings.Take(token, REQUEST, digest, "w1", T0 + 11s) == Bindings::Check::UNKNOWN);
    BOOST_CHECK_EQUAL(bindings.Size(), 0U);
}

BOOST_AUTO_TEST_CASE(token_refuses_another_call)
{
    Bindings bindings;
    const std::string digest{Digest(REQUEST, Question())};
    // Parameters changed after the question (another amount, another destination, another method).
    const std::string other{R"(["host","wallet.send.commit",{"wallet":"main","id":"2"},"main"])"};
    std::string token{bindings.Issue(REQUEST, digest, "w1", T0)};
    BOOST_CHECK(bindings.Take(token, other, digest, "w1", T0) == Bindings::Check::REQUEST_CHANGED);
    // A token that did not fit is gone: not tried again with the right call either.
    BOOST_CHECK(bindings.Take(token, REQUEST, digest, "w1", T0) == Bindings::Check::UNKNOWN);

    // The same call, but what it would do changed (the question made again differs: another amount).
    token = bindings.Issue(REQUEST, digest, "w1", T0);
    BOOST_CHECK(bindings.Take(token, REQUEST, Digest(REQUEST, Question("rladdr1", "2.00000000 LYR")), "w1", T0) == Bindings::Check::QUESTION_CHANGED);

    // Another wallet under the same name (its identity differs).
    token = bindings.Issue(REQUEST, digest, "w1", T0);
    BOOST_CHECK(bindings.Take(token, REQUEST, digest, "w2", T0) == Bindings::Check::WALLET_CHANGED);

    // A token of no question at all.
    BOOST_CHECK(bindings.Take(std::string(64, 'a'), REQUEST, digest, "w1", T0) == Bindings::Check::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(token_expires)
{
    Bindings bindings;
    const std::string digest{Digest(REQUEST, Question())};
    std::string token{bindings.Issue(REQUEST, digest, "", T0)};
    BOOST_CHECK(bindings.Take(token, REQUEST, digest, "", T0 + TOKEN_EXPIRY) == Bindings::Check::EXPIRED);
    token = bindings.Issue(REQUEST, digest, "", T0);
    BOOST_CHECK(bindings.Take(token, REQUEST, digest, "", T0 + TOKEN_EXPIRY - 1s) == Bindings::Check::OK);
    // Expired ones go when new ones come; at most MAX_TOKENS are kept, the oldest go first.
    for (size_t i{0}; i < MAX_TOKENS + 10; ++i) bindings.Issue(REQUEST, digest, "", T0);
    BOOST_CHECK_EQUAL(bindings.Size(), MAX_TOKENS);
    const std::string first{bindings.Issue(REQUEST, digest, "", T0 + 1s)};
    bindings.Issue(REQUEST, digest, "", T0 + TOKEN_EXPIRY + 2s);
    BOOST_CHECK_EQUAL(bindings.Size(), 1U);
    BOOST_CHECK(bindings.Take(first, REQUEST, digest, "", T0 + 2s) == Bindings::Check::UNKNOWN);
}

BOOST_AUTO_TEST_CASE(digest_binds_every_fact)
{
    const std::string base{Digest(REQUEST, Question())};
    BOOST_CHECK_EQUAL(base, Digest(REQUEST, Question()));
    BOOST_CHECK(base != Digest(REQUEST, Question("rladdr2")));
    Window::ConfirmDialog locked{Question()};
    locked.passphrase = true;
    BOOST_CHECK(base != Digest(REQUEST, locked));
    Window::ConfirmDialog other_kind{Question()};
    other_kind.rows[1].kind = dialog::Row::Kind::TEXT;
    BOOST_CHECK(base != Digest(REQUEST, other_kind));
    // The parts are kept apart: moving text from the request into the question makes another digest.
    BOOST_CHECK(Digest("ab", Question()) != Digest("a", Question()));
    const UniValue json{DialogJson(Question())};
    BOOST_CHECK_EQUAL(json.find_value("rows")[1].find_value("kind").get_str(), "address");
    BOOST_CHECK_EQUAL(json.find_value("rows")[1].find_value("value").get_str(), "rladdr1");
}

BOOST_AUTO_TEST_CASE(duplicate_keys_refused)
{
    UniValue v;
    BOOST_REQUIRE(v.read(R"({"wallet":"a","params":[{"x":1,"x":2}]})"));
    BOOST_CHECK(HasDuplicateKeys(v));
    BOOST_REQUIRE(v.read(R"({"wallet":"a","wallet":"b"})"));
    BOOST_CHECK(HasDuplicateKeys(v));
    BOOST_REQUIRE(v.read(R"({"wallet":"a","params":[{"x":1,"y":2}]})"));
    BOOST_CHECK(!HasDuplicateKeys(v));
}

BOOST_AUTO_TEST_CASE(one_wallet_per_request)
{
    const auto parse{[](const std::string& json) {
        UniValue v;
        BOOST_REQUIRE(v.read(json));
        return ParseRequest(v);
    }};
    const auto refused{[](const std::string& json) {
        UniValue v;
        BOOST_REQUIRE(v.read(json));
        try {
            ParseRequest(v);
        } catch (const UniValue& error) {
            return error.find_value("message").get_str().find("wallet") != std::string::npos;
        }
        return false;
    }};
    // A host method: the wallet of its parameters (what it acts on), also named beside them the same.
    BOOST_CHECK_EQUAL(parse(R"({"kind":"host","method":"wallet.send.commit","params":{"wallet":"a","token":"t"}})").wallet, "a");
    BOOST_CHECK_EQUAL(parse(R"({"kind":"host","method":"wallet.send.commit","params":{"wallet":"a"},"wallet":"a"})").wallet, "a");
    BOOST_CHECK_EQUAL(parse(R"({"kind":"host","method":"rpc.confirmed","params":{"method":"signmessage","params":["x","y"],"wallet":"a"}})").wallet, "a");
    BOOST_CHECK_EQUAL(parse(R"({"kind":"host","method":"app.hello","params":{}})").wallet, "");
    // Bound to one, acting on another: refused (rpc.confirmed and any host method), and a wallet beside parameters naming none.
    BOOST_CHECK(refused(R"({"kind":"host","method":"wallet.send.commit","params":{"wallet":"b","token":"t"},"wallet":"a"})"));
    BOOST_CHECK(refused(R"({"kind":"host","method":"rpc.confirmed","params":{"method":"signmessage","params":["x","y"],"wallet":"b"},"wallet":"a"})"));
    BOOST_CHECK(refused(R"({"kind":"host","method":"rpc.confirmed","params":{"method":"setgenerate","params":[true]},"wallet":"a"})"));
    BOOST_CHECK(refused(R"({"kind":"host","method":"wallet.send.commit","params":{"wallet":1}})"));
    // An RPC method: the wallet of its URI.
    BOOST_CHECK_EQUAL(parse(R"({"kind":"rpc","method":"getbalances","params":[],"wallet":"a"})").wallet, "a");

    // describeconfirmation: its argument, or the wallet of the URI; both, the same.
    BOOST_CHECK_EQUAL(DescribedWallet("/wallet/a", UniValue{}), "a");
    BOOST_CHECK_EQUAL(DescribedWallet("/", UniValue{"b"}), "b");
    BOOST_CHECK_EQUAL(DescribedWallet("/wallet/a", UniValue{"a"}), "a");
    BOOST_CHECK_THROW(DescribedWallet("/wallet/a", UniValue{"b"}), UniValue);
}

BOOST_AUTO_TEST_CASE(rpc_caller_scope)
{
    // Outside a scope (the app's own window): any method.
    BOOST_CHECK_NO_THROW(CheckRpcCaller("sendtoaddress"));
    {
        const RpcCallerScope caller{[](const std::string& method) { return method == "getblockcount"; }};
        BOOST_CHECK_NO_THROW(CheckRpcCaller("getblockcount"));
        BOOST_CHECK_EXCEPTION(CheckRpcCaller("sendtoaddress"), HostError, [](const HostError& e) { return e.code == BRIDGE_NOT_ALLOWED; });
        {
            const RpcCallerScope inner{[](const std::string&) { return true; }};
            BOOST_CHECK_NO_THROW(CheckRpcCaller("sendtoaddress"));
        }
        BOOST_CHECK_THROW(CheckRpcCaller("sendtoaddress"), HostError);
    }
    BOOST_CHECK_NO_THROW(CheckRpcCaller("sendtoaddress"));
}

BOOST_AUTO_TEST_CASE(event_log)
{
    EventLog log;
    UniValue none{log.After(0, 0ms)};
    BOOST_CHECK_EQUAL(none.find_value("seq").getInt<int>(), 0);
    BOOST_CHECK(none.find_value("batches").empty());
    log.Add("[1]");
    log.Add("[2]");
    UniValue got{log.After(1, 0ms)};
    BOOST_CHECK_EQUAL(got.find_value("seq").getInt<int>(), 2);
    BOOST_CHECK_EQUAL(got.find_value("batches").size(), 1U);
    BOOST_CHECK_EQUAL(got.find_value("batches")[0].get_str(), "[2]");
    BOOST_CHECK(!got.find_value("reset").get_bool());
    // Waits for the next one.
    std::thread later{[&] { std::this_thread::sleep_for(50ms); log.Add("[3]"); }};
    got = log.After(2, 5000ms);
    later.join();
    BOOST_CHECK_EQUAL(got.find_value("batches")[0].get_str(), "[3]");
    // Older than kept: everything kept, with reset.
    for (size_t i{0}; i < EventLog::MAX_BATCHES + 5; ++i) log.Add("[x]");
    got = log.After(1, 0ms);
    BOOST_CHECK(got.find_value("reset").get_bool());
    BOOST_CHECK_EQUAL(got.find_value("batches").size(), EventLog::MAX_BATCHES);
    // A number from another run of the daemon (larger than any here): reset too.
    BOOST_CHECK(log.After(1'000'000, 0ms).find_value("reset").get_bool());
    log.Stop();
    BOOST_CHECK(log.After(log.After(0, 0ms).find_value("seq").getInt<uint64_t>(), 10s).find_value("batches").empty());
}

BOOST_AUTO_TEST_SUITE_END()

BOOST_AUTO_TEST_SUITE(remotehost_files_tests)

BOOST_AUTO_TEST_CASE(chosen_files_only_by_token)
{
    const fs::path dir{fs::path{fs::temp_directory_path()} / fs::PathFromString(strprintf("remotehost-files-%d", FastRandomContext{}.rand32()))};
    fs::create_directories(dir / "sub");
    const fs::path canonical{fs::weakly_canonical(dir)};
    const std::string file{fs::PathToString(canonical / "backup.dat")};
    ChosenFiles files;
    // Only absolute, canonical paths: nothing relative, no "..", no ".", no link, no folder.
    BOOST_CHECK(ChosenFiles::Problem(file).empty());
    BOOST_CHECK(!ChosenFiles::Problem("backup.dat").empty());
    BOOST_CHECK(!ChosenFiles::Problem(fs::PathToString(canonical / "sub" / ".." / "backup.dat")).empty());
    BOOST_CHECK(!ChosenFiles::Problem(fs::PathToString(canonical / "." / "backup.dat")).empty());
    BOOST_CHECK(!ChosenFiles::Problem(fs::PathToString(canonical / "sub")).empty());
    std::filesystem::create_symlink(canonical / "sub", canonical / "link");
    BOOST_CHECK(!ChosenFiles::Problem(fs::PathToString(canonical / "link" / "x.dat")).empty());
    std::filesystem::create_symlink(canonical / "backup.dat", canonical / "filelink");
    BOOST_CHECK(!ChosenFiles::Problem(fs::PathToString(canonical / "filelink")).empty());
    BOOST_CHECK_THROW(files.Grant(fs::PathToString(canonical / "link" / "x.dat"), true, T0), std::runtime_error);

    // A token gives its file once, within its time; a forged token (or a path given as one) gives nothing.
    const std::string token{files.Grant(file, /*write=*/true, T0)};
    BOOST_CHECK(!files.Take(file, T0));
    BOOST_CHECK(!files.Take(std::string(64, 'b'), T0));
    const auto got{files.Take(token, T0 + 1s)};
    BOOST_REQUIRE(got);
    BOOST_CHECK_EQUAL(got->path, file);
    BOOST_CHECK(got->write);
    BOOST_CHECK(!files.Take(token, T0 + 2s));
    // A file to be read must be there; one to be written may be made.
    BOOST_CHECK_THROW(files.Grant(file, false, T0), std::runtime_error);
    { std::ofstream{fs::PathToString(canonical / "read.dat")} << "x"; }
    const std::string late{files.Grant(fs::PathToString(canonical / "read.dat"), false, T0)};
    BOOST_CHECK(!files.Take(late, T0 + ChosenFiles::EXPIRY));
    // Minutes, not more: the app asks for a token right before the request that uses it.
    BOOST_CHECK(ChosenFiles::EXPIRY <= 120s);
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_CASE(chosen_file_swapped_after_grant)
{
    const fs::path dir{fs::path{fs::temp_directory_path()} / fs::PathFromString(strprintf("remotehost-swap-%d", FastRandomContext{}.rand32()))};
    fs::create_directories(dir);
    const fs::path canonical{fs::weakly_canonical(dir)};
    const fs::path chosen{canonical / "chosen.dat"};
    const fs::path elsewhere{canonical / "secret.dat"};
    { std::ofstream{fs::PathToString(chosen)} << "chosen"; }
    { std::ofstream{fs::PathToString(elsewhere)} << "secret"; }
    ChosenFiles files;

    // An existing file: still the same, it is used.
    auto token{files.Grant(fs::PathToString(chosen), false, T0)};
    auto got{files.Take(token, T0)};
    BOOST_REQUIRE(got);
    BOOST_CHECK(got->identity.exists);
    BOOST_CHECK(ChosenFiles::Changed(*got).empty());

    // A symbolic link put in its place after it was chosen (to a file the user never chose): refused, not followed.
    token = files.Grant(fs::PathToString(chosen), true, T0);
    got = files.Take(token, T0);
    BOOST_REQUIRE(got);
    fs::remove(chosen);
    std::filesystem::create_symlink(elsewhere, chosen);
    BOOST_CHECK(!ChosenFiles::Changed(*got).empty());
    // Nor is a link taken when chosen.
    BOOST_CHECK_THROW(files.Grant(fs::PathToString(chosen), false, T0), std::runtime_error);
    BOOST_CHECK_THROW(ChosenFiles::Identify(fs::PathToString(chosen)), std::runtime_error);

    // Another file moved to the path (same name, another inode): refused.
    fs::remove(chosen);
    { std::ofstream{fs::PathToString(chosen)} << "chosen"; }
    token = files.Grant(fs::PathToString(chosen), true, T0);
    got = files.Take(token, T0);
    BOOST_REQUIRE(got);
    fs::rename(elsewhere, chosen);
    BOOST_CHECK(!ChosenFiles::Changed(*got).empty());

    // A file to be made: still none, in the same folder, it is used; a link (or a file) put there first: refused.
    const fs::path fresh{canonical / "new.dat"};
    token = files.Grant(fs::PathToString(fresh), true, T0);
    got = files.Take(token, T0);
    BOOST_REQUIRE(got);
    BOOST_CHECK(!got->identity.exists);
    BOOST_CHECK(ChosenFiles::Changed(*got).empty());
    std::filesystem::create_symlink(chosen, fresh);
    BOOST_CHECK(!ChosenFiles::Changed(*got).empty());
    fs::remove(fresh);
    BOOST_CHECK(ChosenFiles::Changed(*got).empty());
    { std::ofstream{fs::PathToString(fresh)} << "planted"; }
    BOOST_CHECK(!ChosenFiles::Changed(*got).empty());

    // Its folder swapped for a link to another folder: refused (the path is no longer canonical, and the folder another).
    const fs::path sub{canonical / "sub"};
    fs::create_directories(sub);
    token = files.Grant(fs::PathToString(sub / "x.dat"), true, T0);
    got = files.Take(token, T0);
    BOOST_REQUIRE(got);
    fs::create_directories(canonical / "other");
    fs::remove(sub);
    std::filesystem::create_symlink(canonical / "other", sub);
    BOOST_CHECK(!ChosenFiles::Changed(*got).empty());
    fs::remove_all(dir);
}

BOOST_AUTO_TEST_SUITE_END()
