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
#include <rpc/server.h>
#include <txmempool.h>
#include <script/script.h>
#include <test/util/setup_common.h>
#include <util/strencodings.h>
#include <validation.h>
#include <validationinterface.h>
#include <chainparams.h>
#include <webui/bridge.h>
#include <webui/walletapi.h>

#include <boost/test/unit_test.hpp>

#include <set>

#include <condition_variable>
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

    WalletApiSetup()
    {
        loader = interfaces::MakeWalletLoader(*m_node.chain, *Assert(m_node.args));
        m_node.wallet_loader = loader.get();
        node = interfaces::MakeNode(m_node);
        api = std::make_unique<webui::WalletApi>([this]() -> interfaces::Node& { return *node; }, [this](const std::string& path, bool write) {
            return (write ? writable : readable).contains(path);
        });
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
    UniValue created{Call("wallets.create", Obj({{"name", "w1"}, {"passphrase", "secret words"}}))};
    BOOST_CHECK_EQUAL(created["name"].get_str(), "w1");
    UniValue info{Call("wallet.info", Obj({{"wallet", "w1"}}))};
    BOOST_CHECK(info["encrypted"].get_bool());
    BOOST_CHECK(info["locked"].get_bool());
    BOOST_CHECK(info["hd"].get_bool());
    BOOST_CHECK(!info["privateKeysDisabled"].get_bool());

    // Unlock, lock, change the passphrase.
    BOOST_CHECK_EQUAL(ErrorOf("wallet.unlock", Obj({{"wallet", "w1"}, {"passphrase", "wrong"}})), webui::WalletApi::PASSPHRASE_INCORRECT);
    Call("wallet.unlock", Obj({{"wallet", "w1"}, {"passphrase", "secret words"}}));
    BOOST_CHECK(!Call("wallet.info", Obj({{"wallet", "w1"}}))["locked"].get_bool());
    Call("wallet.lock", Obj({{"wallet", "w1"}}));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "w1"}}))["locked"].get_bool());
    BOOST_CHECK_EQUAL(ErrorOf("wallet.changePassphrase", Obj({{"wallet", "w1"}, {"old", "wrong"}, {"new", "x"}})), webui::WalletApi::PASSPHRASE_INCORRECT);
    Call("wallet.changePassphrase", Obj({{"wallet", "w1"}, {"old", "secret words"}, {"new", "new words"}}));
    Call("wallet.unlock", Obj({{"wallet", "w1"}, {"passphrase", "new words"}}));
    Call("wallet.lock", Obj({{"wallet", "w1"}}));

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

    // The wallet is encrypted: the passphrase unlocks it for the call only.
    const UniValue send_params{Obj({{"wallet", "alice"}, {"recipients", List({Recipient(b1, 10 * COIN, "Bob")})}, {"confTarget", 6}, {"feeRate", 2000}})};
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.prepare", send_params), webui::WalletApi::UNLOCK_NEEDED);
    UniValue with_pass{send_params};
    with_pass.pushKV("passphrase", "nope");
    BOOST_CHECK_EQUAL(ErrorOf("wallet.send.prepare", with_pass), webui::WalletApi::PASSPHRASE_INCORRECT);
    with_pass = send_params;
    with_pass.pushKV("passphrase", "pw");
    UniValue prepared{Call("wallet.send.prepare", with_pass)};
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "alice"}}))["locked"].get_bool());
    const CAmount fee{prepared["fee"].getInt<int64_t>()};
    BOOST_CHECK(fee > 0);
    // 2 sat/vB, on the size the wallet expects before signing (a signature may come out a byte shorter).
    BOOST_CHECK(fee >= prepared["vsize"].getInt<int64_t>() * 2 && fee <= (prepared["vsize"].getInt<int64_t>() + 2) * 2);
    BOOST_CHECK_EQUAL(prepared["total"].getInt<int64_t>(), 10 * COIN + fee);
    UniValue committed{Call("wallet.send.commit", Obj({{"wallet", "alice"}, {"token", prepared["token"]}}))};
    BOOST_CHECK(committed["complete"].get_bool());
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
            BOOST_CHECK_EQUAL(r["label"].get_str(), "Bob");
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
        if (e["address"].get_str() == b1) in_book = e["label"].get_str() == "Bob" && e["purpose"].get_str() == "send";
    }
    BOOST_CHECK(in_book);

    // An unconfirmed send can have its fee bumped, or be abandoned (only while not in the mempool).
    UniValue actions{Call("wallet.txActions", Obj({{"wallet", "alice"}, {"txid", committed["txid"]}}))};
    BOOST_CHECK(actions["canBump"].get_bool());
    BOOST_CHECK(!actions["canAbandon"].get_bool());
    BOOST_CHECK(!actions["hex"].get_str().empty());
    UniValue bump{Call("wallet.bump.prepare", Obj({{"wallet", "alice"}, {"txid", committed["txid"]}}))};
    BOOST_CHECK(bump["newFee"].getInt<int64_t>() > bump["oldFee"].getInt<int64_t>());
    BOOST_CHECK_EQUAL(ErrorOf("wallet.bump.commit", Obj({{"wallet", "alice"}, {"token", bump["token"]}})), webui::WalletApi::UNLOCK_NEEDED);
    UniValue bumped{Call("wallet.bump.commit", Obj({{"wallet", "alice"}, {"token", bump["token"]}, {"passphrase", "pw"}}))};
    BOOST_CHECK(bumped["txid"].get_str() != committed["txid"].get_str());
    Sync();
    // Mined: the coin of bob is confirmed.
    const auto bumped_tx{m_node.mempool->get(*Txid::FromHex(bumped["txid"].get_str()))};
    BOOST_REQUIRE(bumped_tx);
    CreateAndProcessBlock({CMutableTransaction{*bumped_tx}}, CScript{} << OP_TRUE);
    Sync();

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

    // Signed, then broadcast.
    UniValue signed_psbt{Call("wallet.psbt.sign", Obj({{"wallet", "signer"}, {"psbt", psbt}}))};
    BOOST_CHECK(signed_psbt["complete"].get_bool());
    BOOST_CHECK_EQUAL(signed_psbt["status"]["text"].get_str(), "Signed transaction successfully. Transaction is ready to broadcast.");
    ErrorOf("psbt.broadcast", Obj({{"psbt", psbt}}), &message);
    BOOST_CHECK_EQUAL(message, "Unknown error processing transaction.");
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
    Call("wallet.addressBook.edit", Obj({{"wallet", "book"}, {"address", theirs}, {"label", "Carol B."}, {"newAddress", theirs2}}));
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
    BOOST_CHECK_EQUAL(book[mine]["label"].get_str(), "me too");
    BOOST_CHECK(book[mine]["mine"].get_bool());
    Call("wallet.addressBook.delete", Obj({{"wallet", "book"}, {"address", theirs2}}));
    book.clear();
    for (const UniValue& e : Values(Call("wallet.addresses", Obj({{"wallet", "book"}})))) book[e["address"].get_str()] = e;
    BOOST_CHECK(!book.contains(theirs2));

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

    // Sign (with the passphrase for the call) and verify.
    const UniValue sign{Obj({{"wallet", "book"}, {"address", mine}, {"message", "hello Chains"}})};
    BOOST_CHECK_EQUAL(ErrorOf("wallet.signMessage", sign), webui::WalletApi::UNLOCK_NEEDED);
    UniValue sign_pass{sign};
    sign_pass.pushKV("passphrase", "pw");
    const std::string signature{Call("wallet.signMessage", sign_pass)["signature"].get_str()};
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
                               "wallet.transactions", "wallet.withKeys"}) {
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
    // M2: deposits, stamps, multisig signing and proofs unlocked the wallet for ten minutes, and the page
    // had to lock it again. wallet.withKeys unlocks it for its calls alone, in the host.
    if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
    loader->registerRpcs();
    Call("wallets.create", Obj({{"name", "k"}, {"passphrase", "pw"}}));
    const std::string a{Call("wallet.newAddress", Obj({{"wallet", "k"}, {"type", "legacy"}, {"passphrase", "pw"}})).get_str()};
    Mine(1, GetScriptForDestination(DecodeDestination(a)));
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "k"}}))["locked"].get_bool());

    const UniValue sign{Obj({{"method", "signmessage"}, {"params", List({UniValue{a}, UniValue{"I own this"}})}})};
    BOOST_CHECK_EQUAL(ErrorOf("wallet.withKeys", Obj({{"wallet", "k"}, {"calls", List({sign})}})), webui::WalletApi::UNLOCK_NEEDED);
    BOOST_CHECK_EQUAL(ErrorOf("wallet.withKeys", Obj({{"wallet", "k"}, {"calls", List({sign})}, {"passphrase", "bad"}})), webui::WalletApi::PASSPHRASE_INCORRECT);
    const UniValue bad_call{Obj({{"method", "signmessage"}, {"params", List({UniValue{"not an address"}, UniValue{"x"}})}})};
    const UniValue answers{Call("wallet.withKeys", Obj({{"wallet", "k"}, {"calls", List({sign, bad_call})}, {"passphrase", "pw"}}))};
    BOOST_REQUIRE_EQUAL(answers.size(), 2U);
    BOOST_CHECK(answers[0]["result"].isStr());
    BOOST_CHECK(answers[1]["error"]["code"].isNum());
    // Locked again at once, whatever the page does next.
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "k"}}))["locked"].get_bool());
    // Only the methods that need keys, no others.
    const UniValue dump{Obj({{"method", "dumpwallet"}, {"params", List({UniValue{"/tmp/x"}})}})};
    BOOST_CHECK_EQUAL(ErrorOf("wallet.withKeys", Obj({{"wallet", "k"}, {"calls", List({dump})}, {"passphrase", "pw"}})), webui::BRIDGE_METHOD_NOT_FOUND);
    BOOST_CHECK(Call("wallet.info", Obj({{"wallet", "k"}}))["locked"].get_bool());
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
