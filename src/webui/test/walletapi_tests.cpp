// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <consensus/amount.h>
#include <interfaces/chain.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <rpc/server.h>
#include <txmempool.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <tinyformat.h>
#include <util/moneystr.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>
#include <chainparams.h>
#include <webui/bridge.h>
#include <webui/chainconfig.h>
#include <webui/confirm.h>
#include <webui/walletapi.h>

#include <boost/test/unit_test.hpp>

#include <set>

#include <condition_variable>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <string>

namespace {

/** A regtest chain of 100 blocks, a wallet loader, and the wallet functions of the page on it. */
struct WalletApiSetup : TestChain100Setup {
    std::unique_ptr<interfaces::WalletLoader> loader;
    std::unique_ptr<interfaces::Node> node;
    std::unique_ptr<webui::WalletApi> api;
    //! The files "the user chose" (the app's file chooser), as the app passes them.
    std::set<std::string> readable, writable;
    //! The questions of the host (the dialog window, App::AskHost), as asked, and the answers of the "user", in order: none
    //! left is a no, as in the tests of the app.
    std::vector<webui::Window::ConfirmDialog> asked;
    std::deque<webui::dialog::Answer> answers;

    WalletApiSetup()
    {
        loader = interfaces::MakeWalletLoader(*m_node.chain, *Assert(m_node.args));
        m_node.wallet_loader = loader.get();
        node = interfaces::MakeNode(m_node);
        api = std::make_unique<webui::WalletApi>([this]() -> interfaces::Node& { return *node; }, [this](const std::string& path, bool write) {
            return (write ? writable : readable).contains(path);
        }, [this](const webui::Window::ConfirmDialog& dialog) { return Ask(dialog); });
    }

    webui::dialog::Answer Ask(const webui::Window::ConfirmDialog& dialog)
    {
        asked.push_back(dialog);
        if (answers.empty()) return {};
        webui::dialog::Answer answer{std::move(answers.front())};
        answers.pop_front();
        return answer;
    }

    /** The next question is answered: yes or no, and what is typed in its passphrase field. */
    void Answer(bool yes, const std::string& passphrase = "")
    {
        webui::dialog::Answer answer;
        answer.accepted = yes;
        answer.passphrase.assign(passphrase.begin(), passphrase.end());
        answers.push_back(std::move(answer));
    }

    /** Where a fact is among the rows of a question (its first row with this label), or -1. */
    static int RowOf(const webui::Window::ConfirmDialog& dialog, const std::string& label, size_t from = 0)
    {
        for (size_t i{from}; i < dialog.rows.size(); ++i) {
            if (dialog.rows[i].label == label) return static_cast<int>(i);
        }
        return -1;
    }
    static std::string RowValue(const webui::Window::ConfirmDialog& dialog, const std::string& label)
    {
        const int i{RowOf(dialog, label)};
        return i < 0 ? std::string{} : dialog.rows[i].value;
    }

    ~WalletApiSetup()
    {
        for (auto& w : loader->getWallets()) w->remove();
        m_node.validation_signals->SyncWithValidationInterfaceQueue();
        loader->stop();
        m_node.wallet_loader = nullptr;
    }

    UniValue Call(const std::string& method, UniValue params)
    {
        return api->Call(method, params);
    }

    /** The error code of a call that must fail. */
    int ErrorOf(const std::string& method, UniValue params, std::string* message = nullptr)
    {
        try {
            api->Call(method, params);
        } catch (const webui::HostError& e) {
            if (message) *message = e.what();
            return e.code;
        }
        BOOST_FAIL("no error from " + method);
        return 0;
    }

    void Sync() { m_node.validation_signals->SyncWithValidationInterfaceQueue(); }

    void Mine(int blocks, const CScript& script)
    {
        for (int i = 0; i < blocks; ++i) CreateAndProcessBlock({}, script);
        Sync();
    }
};

UniValue Obj(std::vector<std::pair<std::string, UniValue>> kvs)
{
    UniValue o{UniValue::VOBJ};
    for (auto& [k, v] : kvs) o.pushKV(k, std::move(v));
    return o;
}

UniValue Recipient(const std::string& address, CAmount amount, const std::string& label = "", bool subtract = false)
{
    return Obj({{"address", address}, {"amount", amount}, {"label", label}, {"subtractFee", subtract}});
}

/** The items of an array result, kept (a range over the items of a temporary would dangle). */
std::vector<UniValue> Values(const UniValue& array)
{
    return array.getValues();
}

UniValue List(std::vector<UniValue> items)
{
    UniValue a{UniValue::VARR};
    for (auto& i : items) a.push_back(std::move(i));
    return a;
}

/** Records of wallet.transactions, as objects. Fields: see RecordJson in walletapi.cpp. */
std::vector<UniValue> Records(const UniValue& result)
{
    std::vector<UniValue> out;
    for (const UniValue& r : result["records"].getValues()) {
        out.push_back(Obj({{"txid", r[0]}, {"idx", r[1]}, {"type", r[2]}, {"address", r[3]}, {"label", r[4]}, {"time", r[5]},
                           {"credit", r[6]}, {"debit", r[7]}, {"status", r[8]}, {"depth", r[9]}, {"maturesIn", r[10]}, {"counts", r[11]}}));
    }
    return out;
}

} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_walletapi_tests, WalletApiSetup)

BOOST_AUTO_TEST_CASE(receive_request_encoding)
{
    // The bytes chains-qt writes for a request (RecentRequestEntry, SendCoinsRecipient), by hand.
    webui::ReceiveRequest r;
    r.id = 7;
    r.time = 1760000000;
    r.address = "rchn1qexample";
    r.label = "rent";
    r.amount = 150000000;
    r.message = "for June";
    const std::string expected{
        "01000000"                 // nVersion
        "0700000000000000"         // id
        "0078e768"                 // date, unsigned int
        "01000000"                 // recipient nVersion
        "0d" "7263686e3171657861" "6d706c65" // address
        "04" "72656e74"            // label
        "80d1f00800000000"         // amount
        "08" "666f72204a756e65"    // message
        "00"                       // payment request
        "00"};                     // authenticated merchant
    BOOST_CHECK_EQUAL(HexStr(webui::EncodeReceiveRequest(r)), expected);
    const auto back{webui::DecodeReceiveRequest(webui::EncodeReceiveRequest(r))};
    BOOST_REQUIRE(back);
    BOOST_CHECK_EQUAL(back->id, 7);
    BOOST_CHECK_EQUAL(back->time, 1760000000);
    BOOST_CHECK_EQUAL(back->address, r.address);
    BOOST_CHECK_EQUAL(back->label, "rent");
    BOOST_CHECK_EQUAL(back->amount, 150000000);
    BOOST_CHECK_EQUAL(back->message, "for June");
    BOOST_CHECK(!webui::DecodeReceiveRequest("\x01\x02"));
}

BOOST_AUTO_TEST_CASE(methods_and_errors)
{
    const auto methods{api->Methods()};
    for (const char* m : {"wallets.create", "wallet.send.prepare", "wallet.send.commit", "wallet.transactions", "wallet.psbt.sign", "message.verify"}) {
        BOOST_CHECK_MESSAGE(std::find(methods.begin(), methods.end(), m) != methods.end(), m);
    }
    BOOST_CHECK_EQUAL(ErrorOf("no.such", Obj({})), webui::BRIDGE_METHOD_NOT_FOUND);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.info", Obj({{"wallet", "nope"}})), -18);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.info", Obj({})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ErrorOf("wallets.create", Obj({{"name", ""}})), webui::BRIDGE_INVALID_REQUEST);
    // Through the bridge as the page calls them.
    BOOST_CHECK_EQUAL(Call("wallets.list", Obj({})).size(), 0U);
}

BOOST_AUTO_TEST_CASE(wallet_lifecycle)
{
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    loader->registerRpcs();
    // Unlocking for a while locks again from the scheduler of the wallets (walletpassphrase), as in the app.
    loader->start(*Assert(m_node.scheduler));
    UniValue created{Call("wallets.create", Obj({{"name", "w1"}, {"passphrase", "secret words"}}))};
    BOOST_CHECK_EQUAL(created["name"].get_str(), "w1");
    UniValue info{Call("wallet.info", Obj({{"wallet", "w1"}}))};
    BOOST_CHECK(info["encrypted"].get_bool());
    BOOST_CHECK(info["locked"].get_bool());
    BOOST_CHECK(info["hd"].get_bool());
    BOOST_CHECK(!info["privateKeysDisabled"].get_bool());

    // Unlock for a while: the passphrase is typed in the dialog of the host, never given by the page (audit 7 #1). A
    // wrong one is asked again, with why; declined, nothing happens; three wrong, an error.
    const UniValue unlock{Obj({{"wallet", "w1"}, {"seconds", 60}})};
    Answer(false);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.unlock", unlock), webui::BRIDGE_CANCELLED);
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w1"}}))["locked"].get_bool());
    for (int i{0}; i < webui::WalletApi::PASSPHRASE_TRIES; ++i) Answer(true, "wrong");
    BOOST_CHECK_EQUAL(ErrorOf("wallet.unlock", unlock), webui::WalletApi::PASSPHRASE_INCORRECT);
    asked.clear();
    Answer(true, "wrong");
    Answer(true, "secret words");
    Call("wallet.unlock", unlock);
    BOOST_REQUIRE_EQUAL(asked.size(), 2U);
    BOOST_CHECK(asked[0].passphrase && asked[0].error.empty());
    BOOST_CHECK(asked[1].error.find("incorrect") != std::string::npos);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Wallet"), "w1");
    BOOST_CHECK_EQUAL(RowValue(asked[0], "For"), "1 minute");
    BOOST_CHECK(!Call("wallet.info", Obj({{"wallet", "w1"}}))["locked"].get_bool());
    BOOST_CHECK_EQUAL(ErrorOf("wallet.unlock", Obj({{"wallet", "w1"}, {"seconds", 0}})), webui::BRIDGE_INVALID_REQUEST);
    Call("wallet.lock", Obj({{"wallet", "w1"}}));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w1"}}))["locked"].get_bool());
    BOOST_CHECK_EQUAL(ErrorOf("wallet.changePassphrase", Obj({{"wallet", "w1"}, {"old", "wrong"}, {"new", "x"}})), webui::WalletApi::PASSPHRASE_INCORRECT);
    Call("wallet.changePassphrase", Obj({{"wallet", "w1"}, {"old", "secret words"}, {"new", "new words"}}));
    Answer(true, "new words");
    Call("wallet.unlock", unlock);
    Call("wallet.lock", Obj({{"wallet", "w1"}}));
    // The page cannot give the passphrase itself.
    UniValue given{unlock};
    given.pushKV("passphrase", "new words");
    BOOST_CHECK_EQUAL(ErrorOf("wallet.unlock", given), webui::BRIDGE_CANCELLED);
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w1"}}))["locked"].get_bool());

    // A wallet without encryption, encrypted later.
    Call("wallets.create", Obj({{"name", "w2"}}));
    BOOST_CHECK(!Call("wallet.info", Obj({{"wallet", "w2"}}))["encrypted"].get_bool());
    Call("wallet.encrypt", Obj({{"wallet", "w2"}, {"passphrase", "pw"}}));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w2"}}))["encrypted"].get_bool());
    BOOST_CHECK(Call("wallets.isEncrypted", Obj({{"name", "w2"}})).get_bool());

    // Watch-only and blank wallets.
    Call("wallets.create", Obj({{"name", "watch"}, {"disablePrivateKeys", true}}));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "watch"}}))["privateKeysDisabled"].get_bool());
    Call("wallets.create", Obj({{"name", "blank"}, {"blank", true}}));
    BOOST_CHECK(!Call("wallet.info", Obj({{"wallet", "blank"}}))["canGetAddresses"].get_bool());

    // The list of the wallet folder, close and open again.
    UniValue list{Call("wallets.list", Obj({}))};
    BOOST_CHECK_EQUAL(list.size(), 4U);
    for (const UniValue& w : list.getValues()) BOOST_CHECK(w["loaded"].get_bool());
    Call("wallets.close", Obj({{"name", "w2"}}));
    Sync();
    bool found_closed{false};
    for (const UniValue& w : Values(Call("wallets.list", Obj({})))) {
        if (w["name"].get_str() == "w2") found_closed = !w["loaded"].get_bool();
    }
    BOOST_CHECK(found_closed);
    Call("wallets.open", Obj({{"name", "w2"}}));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w2"}}))["encrypted"].get_bool());

    // Names, not paths (audit 7 #14): a wallet is made, restored or opened only in the wallets folder.
    for (const char* bad : {"../escape", "/tmp/abs", "a/b", "a\\b", "c:x", ".", "..", "", "tab\there", "rlo\xe2\x80\xae"}) {
        BOOST_CHECK_MESSAGE(ErrorOf("wallets.create", Obj({{"name", bad}})) == webui::BRIDGE_INVALID_REQUEST, bad);
    }
    BOOST_CHECK_EQUAL(ErrorOf("wallets.open", Obj({{"name", "../w2"}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ErrorOf("wallets.open", Obj({{"name", "not-there"}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ErrorOf("wallets.migrate", Obj({{"name", "/etc/w"}})), webui::BRIDGE_INVALID_REQUEST);

    // Backup and restore under another name: only to and from files the user chose.
    const std::string backup{fs::PathToString(m_args.GetDataDirBase() / "w1-backup.dat")};
    BOOST_CHECK_EQUAL(ErrorOf("wallet.backup", Obj({{"wallet", "w1"}, {"file", backup}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK(!fs::exists(fs::PathFromString(backup)));
    writable.insert(backup);
    Call("wallet.backup", Obj({{"wallet", "w1"}, {"file", backup}}));
    BOOST_CHECK(fs::exists(fs::PathFromString(backup)));
    // Written by the user's choice is not chosen to be read: restoring needs its own choice.
    BOOST_CHECK_EQUAL(ErrorOf("wallets.restore", Obj({{"file", backup}, {"name", "w1-restored"}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ErrorOf("wallets.restoreMigrate", Obj({{"file", backup}, {"name", "w1-migrated"}})), webui::BRIDGE_INVALID_REQUEST);
    readable.insert(backup);
    UniValue restored{Call("wallets.restore", Obj({{"file", backup}, {"name", "w1-restored"}}))};
    BOOST_CHECK_EQUAL(restored["name"].get_str(), "w1-restored");
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w1-restored"}}))["encrypted"].get_bool());
    BOOST_CHECK_EQUAL(ErrorOf("wallets.restore", Obj({{"file", backup}, {"name", ""}})), webui::BRIDGE_INVALID_REQUEST);

    // A watch-only export of a wallet with keys.
    const std::string exported{fs::PathToString(m_args.GetDataDirBase() / "w2-watch.dat")};
    BOOST_CHECK_EQUAL(ErrorOf("wallet.exportWatchOnly", Obj({{"wallet", "w2"}, {"file", exported}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK(!fs::exists(fs::PathFromString(exported)));
    writable.insert(exported);
    Call("wallet.exportWatchOnly", Obj({{"wallet", "w2"}, {"file", exported}}));
    BOOST_CHECK(fs::exists(fs::PathFromString(exported)));
}

BOOST_AUTO_TEST_CASE(send_receive_and_history)
{
    Call("wallets.create", Obj({{"name", "alice"}, {"passphrase", "pw"}}));
    Call("wallets.create", Obj({{"name", "bob"}}));
    const std::string a1{Call("wallet.newAddress", Obj({{"wallet", "alice"}, {"label", "mining"}, {"type", "bech32"}})).get_str()};
    const std::string b1{Call("wallet.newAddress", Obj({{"wallet", "bob"}, {"label", "from alice"}})).get_str()};
    BOOST_CHECK(a1.starts_with(Params().Bech32HRP() + "1q"));
    BOOST_CHECK_EQUAL(ErrorOf("wallet.newAddress", Obj({{"wallet", "bob"}, {"type", "nonsense"}})), webui::BRIDGE_INVALID_REQUEST);

    // Coins for alice: a block, then 100 more to make it mature.
    Mine(1, GetScriptForDestination(DecodeDestination(a1)));
    auto records{Records(Call("wallet.transactions", Obj({{"wallet", "alice"}})))};
    BOOST_REQUIRE_EQUAL(records.size(), 1U);
    BOOST_CHECK_EQUAL(records[0]["type"].get_str(), "mined");
    BOOST_CHECK_EQUAL(records[0]["status"].get_str(), "immature");
    BOOST_CHECK_EQUAL(records[0]["label"].get_str(), "mining");
    BOOST_CHECK_EQUAL(records[0]["maturesIn"].getInt<int>(), 100);
    Mine(100, CScript{} << OP_TRUE);
    records = Records(Call("wallet.transactions", Obj({{"wallet", "alice"}})));
    BOOST_CHECK_EQUAL(records[0]["status"].get_str(), "confirmed");
    BOOST_CHECK(records[0]["counts"].get_bool());
    const CAmount reward{records[0]["credit"].getInt<int64_t>()};
    BOOST_CHECK_EQUAL(Call("wallet.available", Obj({{"wallet", "alice"}})).getInt<int64_t>(), reward);

    // The details of the mined transaction.
    UniValue details{Call("wallet.transaction", Obj({{"wallet", "alice"}, {"txid", records[0]["txid"]}, {"idx", 0}, {"address", a1}}))};
    BOOST_CHECK_EQUAL(details["rows"][0]["k"].get_str(), "Status");
    BOOST_CHECK(details["coinbase"].get_bool());
    BOOST_CHECK(details["coinbaseNote"].get_str().find("101 blocks") != std::string::npos);

    // What the send page refuses, with the messages of chains-qt.
    std::string message;
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.prepare", Obj({{"wallet", "alice"}, {"recipients", List({Recipient("nonsense", 1000)})}}), &message), webui::WalletApi::SEND_REFUSED);
    BOOST_CHECK_EQUAL(message, "The recipient address is not valid. Please recheck.");
    ErrorOf("wallet.send.prepare", Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, 0)})}}), &message);
    BOOST_CHECK_EQUAL(message, "The amount to pay must be larger than 0.");
    ErrorOf("wallet.send.prepare", Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, 1000), Recipient(b1, 2000)})}}), &message);
    BOOST_CHECK_EQUAL(message, "Duplicate address found: addresses should only be used once each.");
    ErrorOf("wallet.send.prepare", Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, reward + 1)})}}), &message);
    BOOST_CHECK_EQUAL(message, "The amount exceeds your balance.");

    // Amounts out of range, alone or in sum, are refused (audit 7 #19).
    const std::string b2{Call("wallet.newAddress", Obj({{"wallet", "bob"}})).get_str()};
    ErrorOf("wallet.send.prepare", Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, MAX_MONEY), Recipient(b2, MAX_MONEY)})}}), &message);
    BOOST_CHECK_EQUAL(message, "The amount exceeds your balance.");
    ErrorOf("wallet.send.prepare", Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, std::numeric_limits<int64_t>::max())})}}), &message);
    BOOST_CHECK_EQUAL(message, "The amount exceeds your balance.");

    // Made, not signed: the wallet stays locked, and no passphrase is asked of the page (audit 7 #1).
    const UniValue send_params{Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, 10 * COIN, "Bob\n\n\n(" + a1 + ")")})}, {"confTarget", 6}, {"feeRate", 2000}})};
    UniValue prepared{Call("wallet.send.prepare", send_params)};
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "alice"}}))["locked"].get_bool());
    BOOST_CHECK(asked.empty());
    // The label is one line (audit 7 #4).
    BOOST_CHECK_EQUAL(prepared["recipients"][0]["label"].get_str(), "Bob (" + a1 + ")");
    const CAmount fee{prepared["fee"].getInt<int64_t>()};
    BOOST_CHECK(fee > 0);
    // 2 sat/vB, on the size the wallet expects before signing (a signature may come out a byte shorter).
    BOOST_CHECK(fee >= prepared["vsize"].getInt<int64_t>() * 2 && fee <= (prepared["vsize"].getInt<int64_t>() + 2) * 2);
    BOOST_CHECK_EQUAL(prepared["total"].getInt<int64_t>(), 10 * COIN + fee);

    // Sent only on the user's word given to the desktop, with what the transaction pays read from it: the address whole,
    // then its amount, then its label, the fee, the total, the wallet, the network; the passphrase typed there.
    Answer(false);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.commit", Obj({{"wallet", "alice"}, {"token", prepared["token"]}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    const webui::Window::ConfirmDialog question{asked[0]};
    BOOST_CHECK_EQUAL(question.title, "Send coins");
    BOOST_CHECK(question.passphrase);
    BOOST_CHECK_EQUAL(RowValue(question, "Wallet"), "alice");
    BOOST_CHECK_EQUAL(RowValue(question, "Network"), "Regtest");
    BOOST_CHECK_EQUAL(RowValue(question, "Pay to"), b1);
    BOOST_CHECK(question.rows[RowOf(question, "Pay to")].kind == webui::dialog::Row::Kind::ADDRESS);
    BOOST_CHECK(RowOf(question, "Pay to") < RowOf(question, "Amount") && RowOf(question, "Amount") < RowOf(question, "Label"));
    BOOST_CHECK(RowOf(question, "Label") < RowOf(question, "Network fee") && RowOf(question, "Network fee") < RowOf(question, "Total"));
    BOOST_CHECK_EQUAL(RowValue(question, "Amount"), "10.00000000 CHN");
    BOOST_CHECK_EQUAL(RowValue(question, "Label"), "\u201c" "Bob (" + a1 + ")\u201d");
    BOOST_CHECK_EQUAL(RowValue(question, "Network fee"), webui::confirm::Coins(fee));
    BOOST_CHECK_EQUAL(RowValue(question, "Total"), webui::confirm::Coins(10 * COIN + fee));
    // The change: back to an address of the wallet, said apart, after the total (not counted as paid).
    BOOST_CHECK(RowOf(question, "Change back to this wallet") > RowOf(question, "Total"));
    BOOST_CHECK(Call("wallet.checkChange", Obj({{"wallet", "alice"}, {"address", RowValue(question, "Change back to this wallet")}}))["mine"].get_bool());
    // A change address of the page that is not the wallet's would be a hidden payment: refused.
    UniValue foreign_change{send_params};
    foreign_change.pushKV("changeAddress", b1);
    ErrorOf("wallet.send.prepare", foreign_change, &message);
    BOOST_CHECK_EQUAL(message, "The custom change address is not an address of this wallet.");
    BOOST_CHECK_EQUAL(webui::confirm::Coins(fee), strprintf("0.%08d CHN", fee));
    // Declined: nothing sent, and the transaction is no longer prepared.
    BOOST_CHECK(m_node.mempool->size() == 0);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.commit", Obj({{"wallet", "alice"}, {"token", prepared["token"]}})), webui::BRIDGE_HOST_ERROR);
    // Again, accepted: a wrong passphrase is asked again, the right one sends; locked again after.
    prepared = Call("wallet.send.prepare", send_params);
    asked.clear();
    Answer(true, "nope");
    Answer(true, "pw");
    UniValue committed{Call("wallet.send.commit", Obj({{"wallet", "alice"}, {"token", prepared["token"]}}))};
    BOOST_CHECK(committed["complete"].get_bool());
    BOOST_REQUIRE_EQUAL(asked.size(), 2U);
    BOOST_CHECK(asked[1].error.find("incorrect") != std::string::npos);
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "alice"}}))["locked"].get_bool());
    // Inputs of segwit: signing leaves the id as it was made.
    BOOST_CHECK_EQUAL(committed["txid"].get_str(), prepared["txid"].get_str());
    // A token is good once.
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.commit", Obj({{"wallet", "alice"}, {"token", prepared["token"]}})), webui::BRIDGE_HOST_ERROR);
    Sync();

    // The history of both wallets, and the address book of alice.
    records = Records(Call("wallet.transactions", Obj({{"wallet", "alice"}})));
    bool sent{false};
    for (const UniValue& r : records) {
        if (r["type"].get_str() == "send") {
            sent = true;
            BOOST_CHECK_EQUAL(r["address"].get_str(), b1);
            BOOST_CHECK_EQUAL(r["label"].get_str(), "Bob (" + a1 + ")");
            BOOST_CHECK_EQUAL(r["debit"].getInt<int64_t>(), -(10 * COIN + fee));
            BOOST_CHECK_EQUAL(r["status"].get_str(), "unconfirmed");
        }
    }
    BOOST_CHECK(sent);
    auto bob{Records(Call("wallet.transactions", Obj({{"wallet", "bob"}})))};
    BOOST_REQUIRE_EQUAL(bob.size(), 1U);
    BOOST_CHECK_EQUAL(bob[0]["type"].get_str(), "recv");
    BOOST_CHECK_EQUAL(bob[0]["credit"].getInt<int64_t>(), 10 * COIN);
    BOOST_CHECK_EQUAL(bob[0]["label"].get_str(), "from alice");
    bool in_book{false};
    for (const UniValue& e : Values(Call("wallet.addresses", Obj({{"wallet", "alice"}})))) {
        if (e["address"].get_str() == b1) in_book = e["label"].get_str() == "Bob (" + a1 + ")" && e["purpose"].get_str() == "send";
    }
    BOOST_CHECK(in_book);

    // An unconfirmed send can have its fee bumped, or be abandoned (only while not in the mempool).
    UniValue actions{Call("wallet.txActions", Obj({{"wallet", "alice"}, {"txid", committed["txid"]}}))};
    BOOST_CHECK(actions["canBump"].get_bool());
    BOOST_CHECK(!actions["canAbandon"].get_bool());
    BOOST_CHECK(!actions["hex"].get_str().empty());
    UniValue bump{Call("wallet.bump.prepare", Obj({{"wallet", "alice"}, {"txid", committed["txid"]}}))};
    BOOST_CHECK(bump["newFee"].getInt<int64_t>() > bump["oldFee"].getInt<int64_t>());
    // Confirmed on the desktop: the transaction it replaces, the fee before and after, the passphrase.
    asked.clear();
    Answer(true, "pw");
    UniValue bumped{Call("wallet.bump.commit", Obj({{"wallet", "alice"}, {"token", bump["token"]}}))};
    BOOST_CHECK(bumped["txid"].get_str() != committed["txid"].get_str());
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Replaces"), committed["txid"].get_str());
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Fee before"), webui::confirm::Coins(bump["oldFee"].getInt<int64_t>()));
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Network fee"), webui::confirm::Coins(bump["newFee"].getInt<int64_t>()));
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Pay to"), b1);
    Sync();
    // Mined: the coin of bob is confirmed.
    const auto bumped_tx{m_node.mempool->get(*Txid::FromHex(bumped["txid"].get_str()))};
    BOOST_REQUIRE(bumped_tx);
    CreateAndProcessBlock({CMutableTransaction{*bumped_tx}}, CScript{} << OP_TRUE);
    Sync();

    // An address in the explorer, from what the wallet knows: alice's mining address received the reward and spent it
    // whole (the bumped payment), newest first; bob's holds the 10 coins; another wallet's address is not known.
    UniValue mined_at{Call("wallet.address", Obj({{"wallet", "alice"}, {"address", a1}}))};
    BOOST_CHECK(mined_at["mine"].get_bool());
    BOOST_CHECK_EQUAL(mined_at["label"].get_str(), "mining");
    BOOST_CHECK_EQUAL(mined_at["received"].getInt<int64_t>(), reward);
    BOOST_CHECK_EQUAL(mined_at["spent"].getInt<int64_t>(), reward);
    BOOST_CHECK_EQUAL(mined_at["balance"].getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(mined_at["txCount"].getInt<int>(), 2);
    BOOST_CHECK_EQUAL(mined_at["coinCount"].getInt<int>(), 0);
    BOOST_REQUIRE_EQUAL(mined_at["history"].size(), 2U);
    BOOST_CHECK_EQUAL(mined_at["history"][0][0].get_str(), bumped["txid"].get_str());
    BOOST_CHECK_EQUAL(mined_at["history"][0][3].getInt<int64_t>(), 0);
    BOOST_CHECK_EQUAL(mined_at["history"][0][4].getInt<int64_t>(), reward);
    BOOST_CHECK_EQUAL(mined_at["history"][1][3].getInt<int64_t>(), reward);
    BOOST_CHECK(mined_at["history"][0][1].getInt<int>() > mined_at["history"][1][1].getInt<int>());
    BOOST_CHECK_EQUAL(Call("wallet.address", Obj({{"wallet", "alice"}, {"address", a1}, {"skip", 1}, {"count", 1}}))["history"][0][3].getInt<int64_t>(), reward);
    UniValue bobs{Call("wallet.address", Obj({{"wallet", "bob"}, {"address", b1}}))};
    BOOST_CHECK_EQUAL(bobs["balance"].getInt<int64_t>(), 10 * COIN);
    BOOST_REQUIRE_EQUAL(bobs["coins"].size(), 1U);
    BOOST_CHECK_EQUAL(bobs["coins"][0][2].getInt<int64_t>(), 10 * COIN);
    BOOST_CHECK(!bobs["coins"][0][4].get_bool());
    BOOST_CHECK(!Call("wallet.address", Obj({{"wallet", "alice"}, {"address", b1}}))["mine"].get_bool());
    BOOST_CHECK(!Call("wallet.address", Obj({{"wallet", "alice"}, {"address", b1}})).exists("history"));
    BOOST_CHECK_EQUAL(ErrorOf("wallet.address", Obj({{"wallet", "alice"}, {"address", a1}, {"count", 101}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.address", Obj({{"wallet", "alice"}, {"address", "rchn1qnope"}})), webui::BRIDGE_INVALID_REQUEST);

    // Coin control: the coins of bob, the estimate of a transaction made of them.
    UniValue coins{Call("wallet.coins", Obj({{"wallet", "bob"}}))};
    BOOST_REQUIRE_EQUAL(coins.size(), 1U);
    const UniValue coin{coins[0]["coins"][0]};
    BOOST_CHECK_EQUAL(coin["sats"].getInt<int64_t>(), 10 * COIN);
    UniValue summary{Call("wallet.coinControl.summary", Obj({{"wallet", "bob"}, {"selected", List({Obj({{"txid", coin["txid"]}, {"vout", coin["vout"]}})})}, {"amounts", List({UniValue{COIN}})}, {"feeRate", 1000}}))};
    BOOST_CHECK_EQUAL(summary["quantity"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(summary["amount"].getInt<int64_t>(), 10 * COIN);
    BOOST_CHECK_EQUAL(summary["fee"].getInt<int64_t>(), summary["bytes"].getInt<int64_t>());
    BOOST_CHECK_EQUAL(summary["change"].getInt<int64_t>(), 9 * COIN - summary["fee"].getInt<int64_t>());
    BOOST_CHECK(!summary["insufficient"].get_bool());
    Call("wallet.coins.lock", Obj({{"wallet", "bob"}, {"txid", coin["txid"]}, {"vout", coin["vout"]}, {"lock", true}}));
    BOOST_CHECK(Call("wallet.coins", Obj({{"wallet", "bob"}}))[0]["coins"][0]["locked"].get_bool());
    Call("wallet.coins.lock", Obj({{"wallet", "bob"}, {"txid", coin["txid"]}, {"vout", coin["vout"]}, {"lock", false}}));

    // The smart fee label of the send page: no estimates on this chain, so the fallback fee.
    UniValue fee_info{Call("wallet.fee", Obj({{"wallet", "bob"}, {"confTarget", 6}}))};
    BOOST_CHECK(fee_info["feePerK"].getInt<int64_t>() > 0);
    // What a deposit's confirmation says of its fee (confirm::FeeFn).
    BOOST_CHECK(api->FeeFor("bob", 200).value_or(0) > 0);
    BOOST_CHECK(!api->FeeFor("nobody", 200));

    // Data on the chain (a timestamp): an output of no value, shown as such on the desktop.
    UniValue stamp{Call("wallet.send.prepare", Obj({{"wallet", "bob"}, {"recipients", List({Obj({{"data", "cafebabe00"}})})}, {"feeRate", 1000}}))};
    asked.clear();
    Answer(true);
    BOOST_CHECK(Call("wallet.send.commit", Obj({{"wallet", "bob"}, {"token", stamp["token"]}}))["complete"].get_bool());
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Data"), "OP_RETURN cafebabe00");
    BOOST_CHECK(!asked[0].passphrase);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.prepare", Obj({{"wallet", "bob"}, {"recipients", List({Obj({{"data", "zz"}})})}})), webui::WalletApi::SEND_REFUSED);
}

BOOST_AUTO_TEST_CASE(psbt_round_trip)
{
    Call("wallets.create", Obj({{"name", "signer"}}));
    const std::string addr{Call("wallet.newAddress", Obj({{"wallet", "signer"}})).get_str()};
    Mine(1, GetScriptForDestination(DecodeDestination(addr)));
    Mine(100, CScript{} << OP_TRUE);
    const std::string dest{Call("wallet.newAddress", Obj({{"wallet", "signer"}, {"label", "self"}})).get_str()};

    // Create unsigned: a PSBT instead of a transaction.
    UniValue prepared{Call("wallet.send.prepare", Obj({{"wallet", "signer"}, {"recipients", List({Recipient(dest, COIN)})}, {"feeRate", 1000}}))};
    const std::string psbt{Call("wallet.send.psbt", Obj({{"wallet", "signer"}, {"token", prepared["token"]}}))["psbt"].get_str()};

    // Loaded again, from the clipboard (base64) and from a file (binary bytes).
    BOOST_CHECK_EQUAL(Call("psbt.decode", Obj({{"base64", " " + psbt + "\n"}}))["psbt"].get_str(), psbt);
    const auto raw{DecodeBase64(psbt)};
    BOOST_REQUIRE(raw);
    BOOST_CHECK_EQUAL(Call("psbt.decode", Obj({{"fileBase64", EncodeBase64(*raw)}}))["psbt"].get_str(), psbt);
    BOOST_CHECK_EQUAL(Call("psbt.decode", Obj({{"fileBase64", EncodeBase64(psbt + "\n")}}))["psbt"].get_str(), psbt);
    std::string message;
    ErrorOf("psbt.decode", Obj({{"base64", "not base64!"}}), &message);
    BOOST_CHECK_EQUAL(message, "Unable to decode PSBT from clipboard (invalid base64)");

    // Opened: the wallet can sign it; without a wallet it cannot.
    UniValue opened{Call("wallet.psbt.open", Obj({{"wallet", "signer"}, {"psbt", psbt}}))};
    BOOST_CHECK(opened["canSign"].get_bool());
    BOOST_CHECK(!opened["complete"].get_bool());
    BOOST_CHECK_EQUAL(opened["status"]["text"].get_str(), "Transaction still needs signature(s).");
    BOOST_CHECK(opened["fee"].getInt<int64_t>() > 0);
    bool own_output{false};
    for (const UniValue& o : opened["outputs"].getValues()) own_output |= o["address"].get_str() == dest && o["mine"].get_bool();
    BOOST_CHECK(own_output);
    UniValue walletless{Call("wallet.psbt.open", Obj({{"psbt", psbt}}))};
    BOOST_CHECK(!walletless["canSign"].get_bool());
    BOOST_CHECK_EQUAL(walletless["status"]["text"].get_str(), "Transaction still needs signature(s). (But no wallet is loaded.)");

    // An output to an address that is not the wallet's is a payment, shown and counted, whatever the PSBT says of it.
    Call("wallets.create", Obj({{"name", "stranger"}}));
    const std::string foreign{Call("wallet.newAddress", Obj({{"wallet", "stranger"}})).get_str()};
    UniValue to_foreign{Call("wallet.send.prepare", Obj({{"wallet", "signer"}, {"recipients", List({Recipient(foreign, COIN)})}, {"feeRate", 1000}}))};
    const std::string foreign_psbt{Call("wallet.send.psbt", Obj({{"wallet", "signer"}, {"token", to_foreign["token"]}}))["psbt"].get_str()};
    asked.clear();
    BOOST_CHECK_EQUAL(ErrorOf("wallet.psbt.sign", Obj({{"wallet", "signer"}, {"psbt", foreign_psbt}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Pay to"), foreign);
    BOOST_CHECK_EQUAL(asked[0].rows[RowOf(asked[0], "Pay to") + 1].value, webui::confirm::Coins(COIN));
    asked.clear();
    // Signed on the user's word given to the desktop, with what it spends of the wallet and pays (audit 7 #1).
    BOOST_CHECK_EQUAL(ErrorOf("wallet.psbt.sign", Obj({{"wallet", "signer"}, {"psbt", psbt}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(asked[0].title, "Sign a partially signed transaction");
    BOOST_CHECK(RowValue(asked[0], "Spends of this wallet").find("(1 of 1 inputs)") != std::string::npos);
    BOOST_CHECK(RowOf(asked[0], "Back to this wallet") >= 0);
    BOOST_CHECK(RowOf(asked[0], "Network fee") > RowOf(asked[0], "Back to this wallet"));
    Answer(true);
    UniValue signed_psbt{Call("wallet.psbt.sign", Obj({{"wallet", "signer"}, {"psbt", psbt}}))};
    BOOST_CHECK(signed_psbt["complete"].get_bool());
    BOOST_CHECK_EQUAL(signed_psbt["status"]["text"].get_str(), "Signed transaction successfully. Transaction is ready to broadcast.");
    // Signed without finalizing: the signatures stay apart, for the next signer of a multisig.
    Answer(true);
    UniValue partial{Call("wallet.psbt.sign", Obj({{"wallet", "signer"}, {"psbt", psbt}, {"finalize", false}}))};
    BOOST_CHECK_EQUAL(partial["signed"].getInt<int>(), 1);
    ErrorOf("psbt.broadcast", Obj({{"psbt", psbt}}), &message);
    BOOST_CHECK_EQUAL(message, "Unknown error processing transaction.");
    // Broadcast on the user's word too; declined, it does not go out.
    asked.clear();
    BOOST_CHECK_EQUAL(ErrorOf("psbt.broadcast", Obj({{"psbt", signed_psbt["psbt"]}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(asked[0].title, "Broadcast a transaction");
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Transaction"), prepared["txid"].get_str());
    BOOST_CHECK_EQUAL(m_node.mempool->size(), 0U);
    Answer(true);
    UniValue sent{Call("psbt.broadcast", Obj({{"psbt", signed_psbt["psbt"]}}))};
    BOOST_CHECK_EQUAL(sent["txid"].get_str(), prepared["txid"].get_str());
}

BOOST_AUTO_TEST_CASE(address_book_requests_and_messages)
{
    Call("wallets.create", Obj({{"name", "book"}, {"passphrase", "pw"}}));
    Call("wallets.create", Obj({{"name", "other"}}));
    const std::string mine{Call("wallet.newAddress", Obj({{"wallet", "book"}, {"label", "me"}, {"type", "bech32"}})).get_str()};
    const std::string theirs{Call("wallet.newAddress", Obj({{"wallet", "other"}})).get_str()};
    const std::string theirs2{Call("wallet.newAddress", Obj({{"wallet", "other"}})).get_str()};

    // Sending addresses: add, the duplicates refused, edit, delete.
    Call("wallet.addressBook.add", Obj({{"wallet", "book"}, {"address", theirs}, {"label", "Carol"}}));
    std::string message;
    ErrorOf("wallet.addressBook.add", Obj({{"wallet", "book"}, {"address", theirs}, {"label", "Again"}}), &message);
    BOOST_CHECK_EQUAL(message, "The entered address \"" + theirs + "\" is already in the address book with label \"Carol\".");
    ErrorOf("wallet.addressBook.add", Obj({{"wallet", "book"}, {"address", mine}, {"label", "x"}}), &message);
    BOOST_CHECK_EQUAL(message, "Address \"" + mine + "\" already exists as a receiving address with label \"me\" and so cannot be added as a sending address.");
    ErrorOf("wallet.addressBook.add", Obj({{"wallet", "book"}, {"address", "rchn1qnope"}}), &message);
    BOOST_CHECK_EQUAL(message, strprintf("The entered address \"rchn1qnope\" is not a valid %s address.", CLIENT_NAME));
    BOOST_CHECK(!Call("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", theirs}, {"label", "Carol"}}))["changed"].get_bool());
    // The address behind a label changes only on the user's word given to the desktop, old and new address whole (audit 7 #6).
    BOOST_CHECK_EQUAL(ErrorOf("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", theirs}, {"label", "Carol B."}, {"newAddress", theirs2}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Old address"), theirs);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "New address"), theirs2);
    BOOST_CHECK(asked[0].level == webui::dialog::Level::DANGER);
    std::map<std::string, UniValue> unchanged;
    for (const UniValue& e : Values(Call("wallet.addresses", Obj({{"wallet", "book"}})))) unchanged[e["address"].get_str()] = e;
    BOOST_CHECK(unchanged.contains(theirs) && !unchanged.contains(theirs2));
    Answer(true);
    Call("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", theirs}, {"label", "Carol B."}, {"newAddress", theirs2}}));
    // A label is one line.
    Call("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", theirs2}, {"label", "Carol\nB.\u2028"}}));
    // Receiving addresses: the label only, and never deleted.
    Call("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", mine}, {"label", "me too"}}));
    ErrorOf("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", mine}, {"newAddress", theirs}}), &message);
    BOOST_CHECK_EQUAL(message, "The address of a receiving address cannot be changed");
    ErrorOf("wallet.addressBook.delete", Obj({{"wallet", "book"}, {"address", mine}}), &message);
    BOOST_CHECK_EQUAL(message, "Only sending addresses can be deleted");
    std::map<std::string, UniValue> book;
    for (const UniValue& e : Values(Call("wallet.addresses", Obj({{"wallet", "book"}})))) book[e["address"].get_str()] = e;
    BOOST_CHECK(!book.contains(theirs));
    BOOST_CHECK_EQUAL(book[theirs2]["label"].get_str(), "Carol B.");
    asked.clear();
    BOOST_CHECK_EQUAL(book[mine]["label"].get_str(), "me too");
    BOOST_CHECK(book[mine]["mine"].get_bool());
    Call("wallet.addressBook.delete", Obj({{"wallet", "book"}, {"address", theirs2}}));
    book.clear();
    for (const UniValue& e : Values(Call("wallet.addresses", Obj({{"wallet", "book"}})))) book[e["address"].get_str()] = e;
    BOOST_CHECK(!book.contains(theirs2));

    // The explorer's "yours": which addresses of a page are the wallet's, with their labels; the others, an address
    // book entry that is not the wallet's and what is no address left out; too many at once refused.
    const std::string change{Call("wallet.newAddress", Obj({{"wallet", "book"}, {"type", "bech32"}})).get_str()};
    const UniValue owned{Call("wallet.owned", Obj({{"wallet", "book"}, {"addresses", List({UniValue{mine}, UniValue{theirs}, UniValue{"rchn1qnope"}, UniValue{change}})}}))};
    BOOST_REQUIRE_EQUAL(owned.size(), 2U);
    BOOST_CHECK_EQUAL(owned[0][0].get_str(), mine);
    BOOST_CHECK_EQUAL(owned[0][1].get_str(), "me too");
    BOOST_CHECK_EQUAL(owned[1][0].get_str(), change);
    BOOST_CHECK_EQUAL(owned[1][1].get_str(), "");
    BOOST_CHECK_EQUAL(Call("wallet.owned", Obj({{"wallet", "other"}, {"addresses", List({UniValue{mine}})}})).size(), 0U);
    UniValue too_many{UniValue::VARR};
    for (int i{0}; i < 501; ++i) too_many.push_back(mine);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.owned", Obj({{"wallet", "book"}, {"addresses", too_many}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.owned", Obj({{"wallet", "book"}})), webui::BRIDGE_INVALID_REQUEST);

    // Payment requests, kept in the wallet as chains-qt keeps them.
    UniValue r1{Call("wallet.requests.add", Obj({{"wallet", "book"}, {"address", mine}, {"label", "rent"}, {"amount", 150000000}, {"message", "June"}}))};
    UniValue r2{Call("wallet.requests.add", Obj({{"wallet", "book"}, {"address", mine}, {"label", "food"}}))};
    BOOST_CHECK_EQUAL(r1["id"].getInt<int>(), 1);
    BOOST_CHECK_EQUAL(r2["id"].getInt<int>(), 2);
    UniValue requests{Call("wallet.requests", Obj({{"wallet", "book"}}))};
    BOOST_REQUIRE_EQUAL(requests.size(), 2U);
    Call("wallet.requests.remove", Obj({{"wallet", "book"}, {"items", List({Obj({{"address", mine}, {"id", 1}})})}}));
    requests = Call("wallet.requests", Obj({{"wallet", "book"}}));
    BOOST_REQUIRE_EQUAL(requests.size(), 1U);
    BOOST_CHECK_EQUAL(requests[0]["label"].get_str(), "food");

    // Sign on the user's word given to the desktop, the passphrase typed there; then verify.
    const UniValue sign{Obj({{"wallet", "book"}, {"address", mine}, {"message", "hello Chains"}})};
    asked.clear();
    BOOST_CHECK_EQUAL(ErrorOf("wallet.signMessage", sign), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Address"), mine);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Message"), "hello Chains");
    BOOST_CHECK(asked[0].passphrase);
    Answer(true, "pw");
    const std::string signature{Call("wallet.signMessage", sign)["signature"].get_str()};
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "book"}}))["locked"].get_bool());
    UniValue verified{Call("message.verify", Obj({{"address", mine}, {"signature", signature}, {"message", "hello Chains"}}))};
    BOOST_CHECK(verified["ok"].get_bool());
    BOOST_CHECK_EQUAL(verified["message"].get_str(), "Message verified.");
    verified = Call("message.verify", Obj({{"address", mine}, {"signature", signature}, {"message", "hello Chain"}}));
    BOOST_CHECK(!verified["ok"].get_bool());
    BOOST_CHECK_EQUAL(verified["message"].get_str(), "Message verification failed.");
    verified = Call("message.verify", Obj({{"address", mine}, {"signature", "%%%"}, {"message", "x"}}));
    BOOST_CHECK_EQUAL(verified["field"].get_str(), "signature");
    // A taproot address is not one key the message can be signed with.
    const std::string tr{Call("wallet.newAddress", Obj({{"wallet", "other"}, {"type", "bech32m"}})).get_str()};
    ErrorOf("wallet.signMessage", Obj({{"wallet", "other"}, {"address", tr}, {"message", "x"}}), &message);
    BOOST_CHECK(message.starts_with("The entered address does not refer to a single key."));
}

BOOST_AUTO_TEST_CASE(registered_in_the_bridge)
{
    std::mutex mutex;
    std::condition_variable cv;
    std::map<std::string, std::pair<int, std::string>> replies;
    webui::Bridge bridge{[&](const std::string& id, int status, const std::string& json) {
                             std::lock_guard lock{mutex};
                             replies[id] = {status, json};
                             cv.notify_all();
                         },
                         [](std::function<void()> fn) { fn(); }};
    api->Register(bridge);
    bridge.Handle("1", R"([{"rid":"a","kind":"host","method":"wallets.create","params":{"name":"viabridge"}}])");
    bridge.Handle("2", R"([{"rid":"b","kind":"host","method":"wallet.info","params":{"wallet":"missing"}}])");
    std::unique_lock lock{mutex};
    BOOST_REQUIRE(cv.wait_for(lock, std::chrono::seconds{30}, [&] { return replies.size() == 2; }));
    BOOST_CHECK_EQUAL(replies["1"].first, 0);
    BOOST_CHECK(replies["1"].second.find("viabridge") != std::string::npos);
    BOOST_CHECK_EQUAL(replies["2"].first, 1);
    BOOST_CHECK(replies["2"].second.find("-18") != std::string::npos);
    lock.unlock();
    bridge.Stop();
}

BOOST_AUTO_TEST_SUITE_END()

// --- The fixes of the audit of chains-app (doc/webui-audit.md) ---

namespace {
/** A node without wallets (-disablewallet): no wallet loader. */
struct NoWalletSetup : TestingSetup {
    std::unique_ptr<interfaces::Node> node;
    std::unique_ptr<webui::WalletApi> api;
    NoWalletSetup()
    {
        node = interfaces::MakeNode(m_node);
        api = std::make_unique<webui::WalletApi>([this]() -> interfaces::Node& { return *node; });
    }
};
} // namespace

BOOST_FIXTURE_TEST_SUITE(webui_walletapi_audit_tests, WalletApiSetup)

BOOST_FIXTURE_TEST_CASE(wallets_disabled, NoWalletSetup)
{
    // C1: with -disablewallet the Wallets page called the wallet loader, and the app aborted on an assertion.
    BOOST_REQUIRE(!m_node.wallet_loader);
    for (const char* method : {"wallets.list", "wallets.open", "wallets.create", "wallets.close", "wallets.isEncrypted", "wallet.info",
                               "wallet.transactions", "wallet.send.prepare"}) {
        try {
            api->Call(method, Obj({{"name", "w"}, {"wallet", "w"}}));
            BOOST_ERROR(std::string{method} + " answered");
        } catch (const webui::HostError& e) {
            BOOST_CHECK_MESSAGE(e.code == webui::WalletApi::WALLETS_DISABLED || e.code == webui::BRIDGE_INVALID_REQUEST, method);
        }
    }
    try {
        api->Call("wallets.list", Obj({}));
    } catch (const webui::HostError& e) {
        BOOST_CHECK_EQUAL(e.code, webui::WalletApi::WALLETS_DISABLED);
        BOOST_CHECK(std::string{e.what()}.find("disablewallet") != std::string::npos);
    }
    // What needs no wallet still works.
    BOOST_CHECK(!api->Call("message.verify", Obj({{"address", "x"}, {"signature", "y"}, {"message", "z"}}))["ok"].get_bool());
}

BOOST_AUTO_TEST_CASE(history_in_parts)
{
    // H2: the page loads the history once, then only the transactions an event names, and the newest ones
    // for the overview; it brings the status of the others up to date with each block by itself.
    Call("wallets.create", Obj({{"name", "h"}}));
    const std::string a{Call("wallet.newAddress", Obj({{"wallet", "h"}})).get_str()};
    Mine(3, GetScriptForDestination(DecodeDestination(a)));
    Mine(2, CScript{} << OP_TRUE);
    const UniValue all{Call("wallet.transactions", Obj({{"wallet", "h"}}))};
    BOOST_CHECK_EQUAL(all["records"].size(), 3U);
    BOOST_CHECK_EQUAL(all["height"].getInt<int>(), 105);
    BOOST_CHECK_EQUAL(all["maturity"].getInt<int>(), 101);
    // The fields the status follows from: block height, state, coinbase, abandoned, trusted; then the role of a
    // coinbase output in the chain's words (local::CoinbaseRoles): none for blocks no role is known of.
    const UniValue& first{all["records"][0]};
    BOOST_REQUIRE_EQUAL(first.size(), 19U);
    BOOST_CHECK_EQUAL(first[18].get_str(), "");
    const int height{first[13].getInt<int>()};
    BOOST_CHECK(height >= 101 && height <= 103);
    BOOST_CHECK_EQUAL(first[14].get_str(), "c");
    BOOST_CHECK(first[15].get_bool());
    BOOST_CHECK(!first[16].get_bool());
    BOOST_CHECK(first[17].get_bool());
    BOOST_CHECK_EQUAL(first[9].getInt<int>(), 105 - height + 1);

    // Some transactions only, and the ids no longer in the wallet.
    const std::string txid{first[0].get_str()};
    const std::string unknown{std::string(64, 'a')};
    const UniValue some{Call("wallet.transactions", Obj({{"wallet", "h"}, {"txids", List({UniValue{txid}, UniValue{unknown}})}}))};
    BOOST_CHECK_EQUAL(some["records"].size(), 1U);
    BOOST_CHECK_EQUAL(some["records"][0][0].get_str(), txid);
    BOOST_REQUIRE_EQUAL(some["gone"].size(), 1U);
    BOOST_CHECK_EQUAL(some["gone"][0].get_str(), unknown);
    BOOST_CHECK_EQUAL(Call("wallet.transactions", Obj({{"wallet", "h"}, {"txids", List({})}}))["records"].size(), 0U);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.transactions", Obj({{"wallet", "h"}, {"txids", List({UniValue{"zz"}})}})), webui::BRIDGE_INVALID_REQUEST);

    // The newest: at least as many as asked, from the end of the history.
    const UniValue latest{Call("wallet.transactions", Obj({{"wallet", "h"}, {"latest", 2}}))};
    BOOST_CHECK(latest["records"].size() >= 2U);
    BOOST_CHECK_EQUAL(latest["height"].getInt<int>(), 105);

    // The balance chart, made on the host from the amounts the wallet keeps.
    const UniValue chart{Call("wallet.balanceHistory", Obj({{"wallet", "h"}}))};
    BOOST_REQUIRE(chart.size() >= 1U);
    CAmount total{0};
    for (const UniValue& r : all["records"].getValues()) total += r[6].getInt<int64_t>() + r[7].getInt<int64_t>();
    BOOST_CHECK_EQUAL(chart[chart.size() - 1][1].getInt<int64_t>(), total);
    const UniValue two{Call("wallet.balanceHistory", Obj({{"wallet", "h"}, {"points", 2}}))};
    BOOST_CHECK(two.size() <= 3U);
    BOOST_CHECK_EQUAL(two[two.size() - 1][1].getInt<int64_t>(), total);
}

BOOST_AUTO_TEST_CASE(keys_for_one_call)
{
    // M2, audit 7 #1: the RPC methods of the pages that sign or need the keys run through the host (rpc.confirmed), which
    // asks on the desktop, with the passphrase of a locked wallet typed there, and unlocks the keys for those calls alone.
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    loader->registerRpcs();
    webui::confirm::ConfirmedRpc confirmed{[this]() -> interfaces::Node& { return *node; },
                                           [this](const webui::Window::ConfirmDialog& dialog) { return Ask(dialog); },
                                           [this](const std::string& wallet, webui::Window::ConfirmDialog dialog, bool always, bool fee_unknown, const std::function<UniValue()>& run) {
                                               return api->WithKeys(wallet, std::move(dialog), always, fee_unknown, run);
                                           },
                                           [this](const std::string& wallet, int64_t vsize) { return api->FeeFor(wallet, vsize); }};
    Call("wallets.create", Obj({{"name", "k"}, {"passphrase", "pw"}}));
    // A new address of a locked wallet whose keys are needed: the passphrase in the dialog of the host.
    Answer(true, "pw");
    const std::string a{Call("wallet.newAddress", Obj({{"wallet", "k"}, {"type", "legacy"}})).get_str()};
    // (Asked only if the wallet had no address ready: an answer not taken is not left for the next question.)
    answers.clear();
    Mine(1, GetScriptForDestination(DecodeDestination(a)));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "k"}}))["locked"].get_bool());

    const auto confirm_call{[&](const UniValue& params, std::string* message = nullptr) {
        try {
            return std::make_pair(0, confirmed.Call(params));
        } catch (const webui::HostError& e) {
            if (message) *message = e.what();
            return std::make_pair(e.code, UniValue{});
        }
    }};
    // A confirmation is never cut: one with more facts than a dialog shows is refused, not asked, and nothing runs.
    {
        webui::Window::ConfirmDialog huge;
        huge.title = "Pay";
        for (size_t i{0}; i <= webui::dialog::MAX_ROWS; ++i) huge.rows.push_back({"To", strprintf("address %u", i), webui::dialog::Row::Kind::ADDRESS});
        bool ran{false};
        asked.clear();
        try {
            api->WithKeys("k", huge, /*always=*/true, /*fee_unknown=*/false, [&] { ran = true; return UniValue{}; });
            BOOST_ERROR("asked");
        } catch (const webui::HostError& e) {
            BOOST_CHECK_EQUAL(e.code, webui::BRIDGE_INVALID_REQUEST);
        }
        BOOST_CHECK(!ran);
        BOOST_CHECK(asked.empty());
    }
    const UniValue sign{Obj({{"method", "signmessage"}, {"params", List({UniValue{a}, UniValue{"I own this"}})}})};
    asked.clear();
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({sign})}})).first, webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK(asked[0].passphrase);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Address"), a);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Message"), "I own this");
    for (int i{0}; i < webui::WalletApi::PASSPHRASE_TRIES; ++i) Answer(true, "bad");
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({sign})}})).first, webui::WalletApi::PASSPHRASE_INCORRECT);
    const UniValue bad_call{Obj({{"method", "signmessage"}, {"params", List({UniValue{"not an address"}, UniValue{"x"}})}})};
    asked.clear();
    Answer(true, "pw");
    const auto [code, answers]{confirm_call(Obj({{"wallet", "k"}, {"calls", List({sign, bad_call})}}))};
    BOOST_CHECK_EQUAL(code, 0);
    BOOST_REQUIRE_EQUAL(answers.size(), 2U);
    BOOST_CHECK(answers[0]["result"].isStr());
    BOOST_CHECK(answers[1]["error"]["code"].isNum());
    // One question for both, each address said.
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Address"), a);
    BOOST_CHECK(RowOf(asked[0], "Address", RowOf(asked[0], "Address") + 1) > 0);
    // Locked again at once, whatever the page does next.
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "k"}}))["locked"].get_bool());
    // Only the methods the host confirms, no others; never one of the node for a wallet, nor one of a wallet without one.
    const UniValue dump{Obj({{"method", "dumpwallet"}, {"params", List({UniValue{"/tmp/x"}})}})};
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({dump})}})).first, webui::BRIDGE_METHOD_NOT_FOUND);
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"method", "signmessage"}, {"params", List({UniValue{a}, UniValue{"x"}})}})).first, webui::BRIDGE_INVALID_REQUEST);
#if WEBUI_SIDECHAIN
    // A withdrawal: the address of the mainchain whole, then the amount, the fee for its miners; declined, nothing is made.
    asked.clear();
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"method", "createwithdrawal"}, {"params", List({UniValue{"mainchainaddress"}, UniValue{"1.5"}, UniValue{"0.001"}})}})).first, webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], strprintf("Pay on %s", webui::chain::MAINCHAIN_NAME)), "mainchainaddress");
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Amount"), "1.50000000 " + CURRENCY_UNIT);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Leaves this wallet"), "1.50100000 " + CURRENCY_UNIT);
    BOOST_CHECK(asked[0].passphrase);
    // Review of audit 7: what is described is what runs (no "args", alias or unknown name; nothing missing), and a spend is
    // one call, one question.
    const auto withdraw{[&](const UniValue& p) { return confirm_call(Obj({{"wallet", "k"}, {"method", "createwithdrawal"}, {"params", p}})).first; }};
    asked.clear();
    BOOST_CHECK_EQUAL(withdraw(Obj({{"args", List({UniValue{"mainchainaddress"}, UniValue{"1.5"}})}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(withdraw(Obj({{"mainchainaddress", "x"}, {"amount", "1"}, {"args", List({UniValue{"y"}})}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(withdraw(List({UniValue{"mainchainaddress"}})), webui::BRIDGE_INVALID_REQUEST);
    const UniValue one_withdrawal{Obj({{"method", "createwithdrawal"}, {"params", List({UniValue{"mainchainaddress"}, UniValue{"1.5"}})}})};
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({one_withdrawal, one_withdrawal})}})).first, webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK(asked.empty());
    BOOST_CHECK_EQUAL(withdraw(Obj({{"amount", "2"}, {"mainchainaddress", "mainchainaddress"}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Amount"), "2.00000000 " + CURRENCY_UNIT);
    // Signatures of a proof: every call shown, the same twice too.
    asked.clear();
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({sign, sign})}})).first, webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK(RowOf(asked[0], "Address", RowOf(asked[0], "Address") + 1) > 0);
    BOOST_CHECK(RowOf(asked[0], "2 of 2") > 0);
#else
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"method", "setgenerate"}, {"params", List({UniValue{true}, UniValue{a}})}})).first, webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "k"}}))["locked"].get_bool());

    // A method of the node: asked, with what it changes; turning the miner off needs no question.
    asked.clear();
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"method", "setgenerate"}, {"params", List({UniValue{true}, UniValue{a}, UniValue{2}})}})).first, webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Rewards to"), a);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Threads"), "2");
    BOOST_CHECK(confirm_call(Obj({{"method", "setgenerate"}, {"params", List({UniValue{false}})}})).first != webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(asked.size(), 1U);
    // The default withdrawal vote: what it becomes; accepted, the node votes so.
    Answer(true);
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"method", "setdefaultwithdrawalvote"}, {"params", List({UniValue{"abstain"}})}})).first, 0);
    BOOST_CHECK_EQUAL(RowValue(asked.back(), "Default vote"), "abstain");
    BOOST_CHECK_EQUAL(node->executeRpc("getdrivechaininfo", UniValue{UniValue::VARR}, "/")["defaultwithdrawalvote"].get_str(), "abstain");

    // Review of audit 7: what is described is what runs. Named parameters only by the method's own names, put in order; never
    // "args", an alias or a name the method does not have (the RPC server would read them otherwise than the dialog).
    asked.clear();
    const auto gen_call{[&](const UniValue& p) { return confirm_call(Obj({{"method", "setgenerate"}, {"params", p}})).first; }};
    BOOST_CHECK_EQUAL(gen_call(Obj({{"args", List({UniValue{true}, UniValue{a}})}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(gen_call(Obj({{"generate", false}, {"args", List({UniValue{true}})}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(gen_call(Obj({{"gen", true}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK(asked.empty());
    BOOST_CHECK_EQUAL(gen_call(Obj({{"address", a}, {"generate", true}})), webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(RowValue(asked[0], "Rewards to"), a);
    // "Off" only when it says false: missing or null is refused, anything else asked.
    BOOST_CHECK(gen_call(Obj({{"generate", false}})) != webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(gen_call(List({})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(gen_call(List({UniValue{}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(gen_call(Obj({{"address", a}})), webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(asked.size(), 1U);
    BOOST_CHECK_EQUAL(gen_call(List({UniValue{"false"}})), webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(gen_call(List({UniValue{0}})), webui::BRIDGE_CANCELLED);
    BOOST_CHECK_EQUAL(asked.size(), 3U);
    // A call missing a parameter the dialog would show: refused, not described.
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"method", "signmessage"}, {"params", List({UniValue{a}})}})).first, webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(asked.size(), 3U);

    // Batches: never of what moves coins (one call, one question); of signatures, every call shown, the same twice too; more
    // than a dialog shows refused, never cut.
    const UniValue deposit{Obj({{"method", "createsidechaindeposit"}, {"params", List({UniValue{1}, UniValue{"s1_x_y"}, UniValue{"1"}})}})};
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({deposit})}})).first, webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({deposit, deposit})}})).first, webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(asked.size(), 3U);
    asked.clear();
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", List({sign, sign})}})).first, webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 1U);
    const int first{RowOf(asked[0], "Address")};
    BOOST_CHECK(first >= 0);
    BOOST_CHECK(RowOf(asked[0], "Address", first + 1) > first);
    BOOST_CHECK(RowOf(asked[0], "Message", RowOf(asked[0], "Message") + 1) > 0);
    BOOST_CHECK(RowOf(asked[0], "1 of 2") >= 0);
    BOOST_CHECK(RowOf(asked[0], "2 of 2") > RowOf(asked[0], "1 of 2"));
    // As many as one question takes: every one of them shown; one more, refused.
    UniValue many{UniValue::VARR};
    for (size_t i{0}; i < webui::confirm::ConfirmedRpc::MAX_CALLS; ++i) many.push_back(sign);
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", many}})).first, webui::BRIDGE_CANCELLED);
    BOOST_REQUIRE_EQUAL(asked.size(), 2U);
    BOOST_CHECK(asked[1].rows.size() <= webui::dialog::MAX_ROWS);
    BOOST_CHECK_EQUAL(std::ranges::count_if(asked[1].rows, [&](const webui::dialog::Row& r) { return r.label == "Address"; }), static_cast<long>(webui::confirm::ConfirmedRpc::MAX_CALLS));
    many.push_back(sign);
    BOOST_CHECK_EQUAL(confirm_call(Obj({{"wallet", "k"}, {"calls", many}})).first, webui::BRIDGE_INVALID_REQUEST);
    BOOST_CHECK_EQUAL(asked.size(), 2U);
#endif
}

BOOST_AUTO_TEST_CASE(dust)
{
    // L4: the send form refuses dust as chains-qt does (GUIUtil::isDust), before the wallet does.
    Call("wallets.create", Obj({{"name", "d"}}));
    const std::string a{Call("wallet.newAddress", Obj({{"wallet", "d"}})).get_str()};
    BOOST_CHECK(Call("send.isDust", Obj({{"address", a}, {"amount", 100}})).get_bool());
    BOOST_CHECK(!Call("send.isDust", Obj({{"address", a}, {"amount", 100000}})).get_bool());
    BOOST_CHECK(!Call("send.isDust", Obj({{"address", "nonsense"}, {"amount", 1}})).get_bool());
}

BOOST_AUTO_TEST_SUITE_END()
