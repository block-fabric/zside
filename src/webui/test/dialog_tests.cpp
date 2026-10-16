// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/assets.h>
#include <webui/dialog.h>

#include <univalue.h>

#include <boost/test/unit_test.hpp>

#include <string>
#include <vector>

using namespace webui;
using dialog::Session;

namespace {
//! A call of the page of the dialog, as its bridge passes it: [method, params].
std::string CallArgs(const std::string& method, const UniValue& params = UniValue{UniValue::VOBJ})
{
    UniValue args{UniValue::VARR};
    args.push_back(method);
    args.push_back(params);
    return args.write();
}

UniValue Obj(const std::string& json)
{
    UniValue v;
    BOOST_REQUIRE(v.read(json));
    return v;
}

dialog::Request Sample()
{
    dialog::Request request;
    request.title = "Run a command of the console";
    request.message = "Command: stop\n\nRun it only if you typed it yourself.";
    request.accept = "Run";
    return request;
}

//! A session through hello and ready: the window shows.
void Show(Session& session)
{
    BOOST_REQUIRE_EQUAL(session.Call(CallArgs("hello")).status, 0);
    const auto ready{session.Call(CallArgs("ready", Obj(R"({"width":440,"height":230})")))};
    BOOST_REQUIRE_EQUAL(ready.status, 0);
    BOOST_REQUIRE(ready.step == Session::Step::SHOW);
}
} // namespace

BOOST_AUTO_TEST_SUITE(webui_dialog_tests)

BOOST_AUTO_TEST_CASE(shown_text)
{
    // The marks that turn text around are written out (audit port-2 #16, as revealBidi of the page).
    BOOST_CHECK_EQUAL(dialog::Shown("Wallet: \xE2\x80\xAE" "gnp.exe", 100), "Wallet: [U+202E]gnp.exe");
    BOOST_CHECK_EQUAL(dialog::Shown("a\xE2\x81\xA6" "b\xE2\x81\xA9" "\xE2\x80\x8F" "c\xD8\x9C" "", 100), "a[U+2066]b[U+2069][U+200F]c[U+061C]");
    // Invisible ones too, and controls; a line break stays one, "\r\n" is one, a tab is a space.
    BOOST_CHECK_EQUAL(dialog::Shown("a\xE2\x80\x8B" "b\x07" "c", 100), "a[U+200B]b[U+0007]c");
    BOOST_CHECK_EQUAL(dialog::Shown("one\r\ntwo\rthree\n\nfour\tfive", 100), "one\ntwo\nthree\n\nfour five");
    // Text of other scripts is as it is.
    BOOST_CHECK_EQUAL(dialog::Shown("مرحبا hello", 100), "مرحبا hello");
    // Invalid UTF-8 is U+FFFD.
    BOOST_CHECK_EQUAL(dialog::Shown("bad \xff\xfe end", 100), "bad \xEF\xBF\xBD" "\xEF\xBF\xBD" " end");
    // Cut with "…" after so many characters (not bytes).
    BOOST_CHECK_EQUAL(dialog::Shown("ééééé", 3), "ééé\xE2\x80\xA6" "");
    BOOST_CHECK_EQUAL(dialog::Shown("abc", 3), "abc");
}

BOOST_AUTO_TEST_CASE(page_data_is_text)
{
    dialog::Request request{Sample()};
    // What came from the page: markup and marks stay text (the page puts it in with textContent).
    request.title = "Open <b>this</b>\n\"now\"\xE2\x80\xAE" "";
    request.message = "Host: <img src=x onerror=alert(1)>\n\nURL: https://e.example/\xE2\x80\xAE" "exe.txt";
    request.details = "</script><script>alert(2)</script>";
    request.level = dialog::Level::DANGER;
    const UniValue data{dialog::PageData(request)};
    // Through JSON and back (what the page gets): the same text.
    UniValue back;
    BOOST_REQUIRE(back.read(data.write()));
    // The title is one line, its marks a space (guard::OneLine), the markup as it is.
    BOOST_CHECK_EQUAL(back.find_value("title").get_str(), "Open <b>this</b> \"now\"");
    BOOST_CHECK_EQUAL(back.find_value("message").get_str(), "Host: <img src=x onerror=alert(1)>\n\nURL: https://e.example/[U+202E]exe.txt");
    BOOST_CHECK_EQUAL(back.find_value("details").get_str(), "</script><script>alert(2)</script>");
    BOOST_CHECK_EQUAL(back.find_value("accept").get_str(), "Run");
    BOOST_CHECK_EQUAL(back.find_value("cancel").get_str(), "Cancel");
    BOOST_CHECK_EQUAL(back.find_value("level").get_str(), "danger");
    BOOST_CHECK(back.find_value("passphrase").isFalse());
    BOOST_CHECK(back.find_value("look").isObject());
    BOOST_CHECK_EQUAL(dialog::LevelName(dialog::Level::INFO), "info");
    BOOST_CHECK_EQUAL(dialog::LevelName(dialog::Level::WARNING), "warning");
    // A button never empty, never more than a line.
    request.accept = "";
    request.cancel = "Keep\nit";
    const UniValue other{dialog::PageData(request)};
    BOOST_CHECK_EQUAL(other.find_value("accept").get_str(), "OK");
    BOOST_CHECK_EQUAL(other.find_value("cancel").get_str(), "Keep it");
}

BOOST_AUTO_TEST_CASE(look_from_prefs)
{
    const UniValue prefs{Obj(R"({"theme":"custom-abc","fontAdjust":40,"fontFamily":"nunito","moneyFont":"system","unit":"CHN",
        "customThemes":[{"id":"custom-zzz","name":"Other","base":"light","colors":{}},
                        {"id":"custom-abc","name":"Mine","base":"dracula","colors":{"accent":"#ff0000"}}]})")};
    const UniValue look{dialog::Look(prefs, true, "ar")};
    BOOST_CHECK_EQUAL(look.find_value("theme").get_str(), "custom-abc");
    // Only the theme of the user it names, as it is kept (the page checks it as strictly as a theme file).
    BOOST_CHECK_EQUAL(look.find_value("custom").find_value("name").get_str(), "Mine");
    BOOST_CHECK_EQUAL(look.find_value("custom").find_value("colors").find_value("accent").get_str(), "#ff0000");
    // The text size clamped as the page clamps it.
    BOOST_CHECK_EQUAL(look.find_value("fontAdjust").getInt<int>(), 12);
    BOOST_CHECK_EQUAL(look.find_value("fontFamily").get_str(), "nunito");
    BOOST_CHECK_EQUAL(look.find_value("moneyFont").get_str(), "system");
    BOOST_CHECK(look.find_value("rtl").isTrue());
    BOOST_CHECK_EQUAL(look.find_value("lang").get_str(), "ar");
    // Nothing else of the preferences.
    BOOST_CHECK(look.find_value("unit").isNull());
    BOOST_CHECK(look.find_value("customThemes").isNull());

    // A built-in theme: no theme of the user; nothing kept yet: Midnight.
    BOOST_CHECK(dialog::Look(Obj(R"({"theme":"light","customThemes":[{"id":"custom-abc"}]})"), false, "").find_value("custom").isNull());
    BOOST_CHECK_EQUAL(dialog::Look(UniValue{UniValue::VOBJ}, false, "").find_value("theme").get_str(), "midnight");
    BOOST_CHECK_EQUAL(dialog::Look(Obj(R"({"theme":12})"), false, "").find_value("theme").get_str(), "midnight");
    BOOST_CHECK_EQUAL(dialog::Look(Obj(R"({"fontAdjust":-50})"), false, "").find_value("fontAdjust").getInt<int>(), -6);
    // A theme of the user too large is left out (the page draws its base then, Midnight).
    UniValue big{Obj(R"({"theme":"custom-big","customThemes":[{"id":"custom-big","name":"Big","base":"light","colors":{}}]})")};
    UniValue themes{big.find_value("customThemes")};
    UniValue entry{themes[0]};
    entry.pushKV("junk", std::string(20000, 'x'));
    UniValue list{UniValue::VARR};
    list.push_back(entry);
    UniValue prefs_big{UniValue::VOBJ};
    prefs_big.pushKV("theme", "custom-big");
    prefs_big.pushKV("customThemes", list);
    BOOST_CHECK(dialog::Look(prefs_big, false, "").find_value("custom").isNull());
}

BOOST_AUTO_TEST_CASE(scheme_of_look)
{
    using dialog::Scheme;
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"midnight"})")) == Scheme::DARK);
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"dracula"})")) == Scheme::DARK);
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"light"})")) == Scheme::LIGHT);
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"system"})")) == Scheme::DESKTOP);
    BOOST_CHECK(dialog::SchemeOf(UniValue{UniValue::VOBJ}) == Scheme::DARK);
    // A theme of the user: its page colour when it is #rgb or #rrggbb, else its base.
    const std::string themes{R"x("customThemes":[{"id":"custom-a","base":"midnight","colors":{"bg":"#f8f8f8"}},{"id":"custom-b","base":"light","colors":{"bg":"rgb(0 0 0)"}},{"id":"custom-c","base":"dracula","colors":{}}])x"};
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"custom-a",)" + themes + "}")) == Scheme::LIGHT);
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"custom-b",)" + themes + "}")) == Scheme::LIGHT);
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"custom-c",)" + themes + "}")) == Scheme::DARK);
    // From a look (Look) as from the preferences.
    const UniValue look{dialog::Look(Obj(R"({"theme":"custom-a",)" + themes + "}"), false, "")};
    BOOST_CHECK(dialog::SchemeOf(look) == Scheme::LIGHT);
    BOOST_CHECK(dialog::SchemeOf(Obj(R"({"theme":"custom-x","custom":{"id":"custom-y","base":"light"}})")) == Scheme::DARK);
}

BOOST_AUTO_TEST_CASE(origin_serves_the_dialog_only)
{
    BOOST_CHECK(dialog::Serves("/dialog.html"));
    BOOST_CHECK(dialog::Serves("/dialog.html?x"));
    BOOST_CHECK(dialog::Serves("/assets/dialog-AbC_12.js"));
    BOOST_CHECK(dialog::Serves("/assets/dialog-AbC_12.css"));
    BOOST_CHECK(dialog::Serves("/assets/inter-latin-wght-normal-Dx4kXJAl.woff2"));
    // Nothing of the main page: its document, its script and style sheet, its catalogs.
    BOOST_CHECK(!dialog::Serves("/"));
    BOOST_CHECK(!dialog::Serves("/index.html"));
    BOOST_CHECK(!dialog::Serves("/assets/index-Ssjtiqr0.js"));
    BOOST_CHECK(!dialog::Serves("/assets/style-CZ52V_lx.css"));
    BOOST_CHECK(!dialog::Serves("/locales/fr.json"));
    BOOST_CHECK(!dialog::Serves("/assets/../index.html"));
    BOOST_CHECK(!dialog::Serves("/assets/dialog-x.html"));
    BOOST_CHECK(!dialog::Serves("/assets/sub/dialog-x.js"));
    const std::string policy{dialog::ContentSecurityPolicy()};
    for (const char* part : {"default-src 'none'", "script-src 'self'", "style-src 'self'", "connect-src 'none'", "frame-ancestors 'none'", "form-action 'none'"}) {
        BOOST_CHECK_MESSAGE(policy.find(part) != std::string::npos, part);
    }
    BOOST_CHECK(policy.find("unsafe") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(builtin_dialog_page)
{
    // The page of the dialogs is built into the program, with its script and style sheet, all served by its origin.
    const Asset* page{FindAsset(BuiltinAssets(), dialog::PAGE)};
    BOOST_REQUIRE(page);
    const std::string html{reinterpret_cast<const char*>(page->data.data()), page->data.size()};
    std::vector<std::string> refs;
    for (const std::string attr : {"src=\"", "href=\""}) {
        for (size_t at{html.find(attr)}; at != std::string::npos; at = html.find(attr, at + 1)) {
            const size_t start{at + attr.size()};
            refs.push_back(html.substr(start, html.find('"', start) - start));
        }
    }
    BOOST_CHECK(refs.size() >= 2);
    for (const std::string& ref : refs) {
        BOOST_CHECK_MESSAGE(dialog::Serves(ref), ref);
        BOOST_CHECK_MESSAGE(FindAsset(BuiltinAssets(), ref), ref);
    }
    // Every file of its own is served, and the main page is not.
    bool script{false};
    for (const Asset& asset : BuiltinAssets()) {
        if (asset.path.starts_with("/assets/dialog-") && asset.path.ends_with(".js")) script = true;
    }
    BOOST_CHECK(script);
    BOOST_CHECK(!dialog::Serves("/index.html"));
}

BOOST_AUTO_TEST_CASE(session_in_order)
{
    Session session{Sample()};
    // Nothing before hello: no answer before the page was shown.
    BOOST_CHECK_NE(session.Call(CallArgs("ready", Obj(R"({"width":400,"height":200})"))).status, 0);
    BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":true})"))).status, 0);
    const auto hello{session.Call(CallArgs("hello"))};
    BOOST_CHECK_EQUAL(hello.status, 0);
    BOOST_CHECK(hello.step == Session::Step::NONE);
    BOOST_CHECK_EQUAL(Obj(hello.json).find_value("title").get_str(), "Run a command of the console");
    // Once.
    BOOST_CHECK_NE(session.Call(CallArgs("hello")).status, 0);
    // Not answered before the window shows.
    BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":true})"))).status, 0);
    BOOST_CHECK(!session.Answered());
    BOOST_CHECK_NE(session.Call(CallArgs("ready", Obj(R"({"width":"wide","height":200})"))).status, 0);
    const auto ready{session.Call(CallArgs("ready", Obj(R"({"width":400,"height":200})")))};
    BOOST_CHECK(ready.step == Session::Step::SHOW);
    BOOST_CHECK(session.Shown());
    BOOST_CHECK_NE(session.Call(CallArgs("ready", Obj(R"({"width":400,"height":200})"))).status, 0);
    // An answer is a yes or a no.
    BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":"yes"})"))).status, 0);
    BOOST_CHECK_NE(session.Call(CallArgs("answer")).status, 0);
    const auto answer{session.Call(CallArgs("answer", Obj(R"({"accept":true})")))};
    BOOST_CHECK(answer.step == Session::Step::ANSWER);
    BOOST_CHECK(session.Answered());
    BOOST_CHECK(session.TakeAnswer().accepted);
    // Once: nothing after it changes it.
    BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":false})"))).status, 0);
    BOOST_CHECK_NE(session.Call(CallArgs("hello")).status, 0);
    // Calls that are not calls.
    Session other{Sample()};
    for (const std::string bad : {"", "{}", "[]", "[1]", "not json", R"(["nope",{}])"}) {
        const auto reply{other.Call(bad)};
        BOOST_CHECK_MESSAGE(reply.status != 0 && reply.step == Session::Step::NONE, bad);
    }
}

BOOST_AUTO_TEST_CASE(session_declined)
{
    Session session{Sample()};
    Show(session);
    BOOST_CHECK(session.Call(CallArgs("answer", Obj(R"({"accept":false})"))).step == Session::Step::ANSWER);
    const auto answer{session.TakeAnswer()};
    BOOST_CHECK(!answer.accepted);
    BOOST_CHECK(answer.passphrase.empty());
}

BOOST_AUTO_TEST_CASE(session_passphrase)
{
    // No field: no passphrase taken.
    {
        Session session{Sample()};
        Show(session);
        BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":true,"passphrase":"secret"})"))).status, 0);
        BOOST_CHECK(!session.Answered());
    }
    dialog::Request request{Sample()};
    request.passphrase = true;
    request.passphrase_label = "Passphrase of the wallet";
    BOOST_CHECK(dialog::PageData(request).find_value("passphrase").isTrue());
    BOOST_CHECK_EQUAL(dialog::PageData(request).find_value("passphraseLabel").get_str(), "Passphrase of the wallet");
    {
        Session session{request};
        Show(session);
        // Accepted: the passphrase is needed, not empty, not too long.
        BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":true})"))).status, 0);
        BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":true,"passphrase":""})"))).status, 0);
        UniValue params{UniValue::VOBJ};
        params.pushKV("accept", true);
        params.pushKV("passphrase", std::string(2000, 'p'));
        BOOST_CHECK_NE(session.Call(CallArgs("answer", params)).status, 0);
        BOOST_CHECK(session.Call(CallArgs("answer", Obj(R"({"accept":true,"passphrase":"correct horse"})"))).step == Session::Step::ANSWER);
        const auto answer{session.TakeAnswer()};
        BOOST_CHECK(answer.accepted);
        BOOST_CHECK(answer.passphrase == "correct horse");
    }
    {
        // Declined: no passphrase needed, none taken.
        Session session{request};
        Show(session);
        BOOST_CHECK_NE(session.Call(CallArgs("answer", Obj(R"({"accept":false,"passphrase":"x"})"))).status, 0);
        BOOST_CHECK(session.Call(CallArgs("answer", Obj(R"({"accept":false})"))).step == Session::Step::ANSWER);
        BOOST_CHECK(session.TakeAnswer().passphrase.empty());
    }
}

BOOST_AUTO_TEST_CASE(window_size)
{
    Session session{Sample()};
    BOOST_REQUIRE_EQUAL(session.Call(CallArgs("hello")).status, 0);
    BOOST_REQUIRE_EQUAL(session.Call(CallArgs("ready", Obj(R"({"width":440.4,"height":5000})"))).status, 0);
    // As the page asks, within the largest the window may be.
    const auto size{session.WindowSize(1600, 700)};
    BOOST_CHECK_EQUAL(size.width, 441);
    BOOST_CHECK_EQUAL(size.height, 700);
    Session tiny{Sample()};
    BOOST_REQUIRE_EQUAL(tiny.Call(CallArgs("hello")).status, 0);
    BOOST_REQUIRE_EQUAL(tiny.Call(CallArgs("ready", Obj(R"({"width":-5,"height":3})"))).status, 0);
    BOOST_CHECK_EQUAL(tiny.WindowSize(1600, 700).width, 320);
    BOOST_CHECK_EQUAL(tiny.WindowSize(1600, 700).height, 120);
    Session wide{Sample()};
    BOOST_REQUIRE_EQUAL(wide.Call(CallArgs("hello")).status, 0);
    BOOST_REQUIRE_EQUAL(wide.Call(CallArgs("ready", Obj(R"({"width":5000,"height":300})"))).status, 0);
    BOOST_CHECK_EQUAL(wide.WindowSize(1600, 700).width, 720);
    // A small screen: never more than it.
    BOOST_CHECK_EQUAL(wide.WindowSize(500, 700).width, 500);
}

BOOST_AUTO_TEST_CASE(one_at_a_time)
{
    dialog::Pending pending;
    std::vector<std::string> answered;
    const auto done{[&](const std::string& name) { return [&answered, name](dialog::Answer a) { answered.push_back(name + (a.accepted ? ":yes" : ":no")); }; }};
    dialog::Request first{Sample()}, second{Sample()}, third{Sample()};
    second.title = "second";
    third.title = "third";
    // The first shows at once; the others wait, in order.
    BOOST_CHECK(pending.Push(first, done("first")));
    BOOST_CHECK(pending.Busy());
    BOOST_CHECK(!pending.Push(second, done("second")));
    BOOST_CHECK(!pending.Push(third, done("third")));
    BOOST_CHECK_EQUAL(pending.Waiting(), 2U);
    auto next{pending.Next()};
    BOOST_REQUIRE(next);
    BOOST_CHECK_EQUAL(next->first.title, "second");
    next->second(dialog::Answer{true, {}});
    next = pending.Next();
    BOOST_REQUIRE(next);
    BOOST_CHECK_EQUAL(next->first.title, "third");
    BOOST_CHECK(pending.Busy());
    // The last one answered: none shows, the next asked shows at once.
    BOOST_CHECK(!pending.Next());
    BOOST_CHECK(!pending.Busy());
    BOOST_CHECK(pending.Push(first, done("again")));
    BOOST_CHECK_EQUAL(answered.size(), 1U);
    BOOST_CHECK_EQUAL(answered[0], "second:yes");
}

BOOST_AUTO_TEST_SUITE_END()
