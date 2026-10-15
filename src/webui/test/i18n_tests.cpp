// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The languages of chains-app (audit M6): the catalog chosen as chains-qt chooses its translation, and
// the messages of the node translated through it.

#include <test/util/setup_common.h>
#include <webui/chainconfig.h>
#include <webui/assets.h>
#include <webui/i18n.h>

#include <boost/test/unit_test.hpp>

#include <cstdlib>

namespace {
// The environment, on every platform (Windows has no setenv).
void SetEnv(const char* name, const char* value)
{
#ifdef WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}
void UnsetEnv(const char* name)
{
#ifdef WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}
} // namespace

BOOST_AUTO_TEST_SUITE(webui_i18n_tests)

BOOST_AUTO_TEST_CASE(choose_and_translate)
{
    const auto assets{webui::BuiltinAssets()};
    // The catalogs of chains-qt are built into the program.
    const UniValue languages{webui::Languages(assets)};
    BOOST_CHECK(languages.size() > 90);

    // The language and territory if there is a catalog for it, else the language; English needs none.
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, "pt_BR"), "pt_BR");
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, "de_CH"), "de");
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, "de-AT"), "de");
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, "en_US"), "");
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, "xx_YY"), "");

    // Without -lang: the locale of the system (LC_ALL, LC_MESSAGES, LANG).
    const char* lang_was{std::getenv("LANG")};
    const std::string saved{lang_was ? lang_was : ""};
    UnsetEnv("LC_ALL");
    UnsetEnv("LC_MESSAGES");
    SetEnv("LANG", "fr_FR.UTF-8");
    BOOST_CHECK_EQUAL(webui::SystemLocale(), "fr_FR");
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, ""), "fr");
    SetEnv("LANG", "C.UTF-8");
    BOOST_CHECK_EQUAL(webui::ChooseLanguage(assets, ""), "");
    if (lang_was) SetEnv("LANG", saved.c_str());
    else UnsetEnv("LANG");

    // A catalog has the messages of the interface and of the node.
    const auto de{webui::LoadCatalog(assets, "de")};
    BOOST_REQUIRE(de);
    BOOST_CHECK(de->find_value("gui").isObject());
    BOOST_CHECK(!webui::LoadCatalog(assets, "xx"));

    // The messages of the node, translated as G_TRANSLATION_FUN of chains-app translates them.
    const std::string message{"Block verification was interrupted"};
    BOOST_CHECK_EQUAL(webui::TranslateCore(message.c_str()), message);
    webui::SetCoreCatalog(de);
    const std::string translated{webui::TranslateCore(message.c_str())};
    BOOST_CHECK(translated != message);
    BOOST_CHECK(!translated.empty());
    BOOST_CHECK_EQUAL(webui::TranslateCore("A message nobody translated"), "A message nobody translated");
    webui::SetCoreCatalog(std::nullopt);
    BOOST_CHECK_EQUAL(webui::TranslateCore(message.c_str()), message);

    BOOST_CHECK_EQUAL(webui::NormalizeMessage("Loading wallet..."), "Loading wallet…");
    BOOST_CHECK_EQUAL(webui::NormalizeMessage("Bitcoin Core is running"), std::string{webui::chain::NAME} + " is running");
}

BOOST_AUTO_TEST_SUITE_END()
