// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>
#include <webui/chainconfig.h>
#include <webui/appconfig.h>

#include <boost/test/unit_test.hpp>

#include <fstream>

BOOST_FIXTURE_TEST_SUITE(webui_appconfig_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(round_trip)
{
    const fs::path path{m_args.GetDataDirBase() / "config" / "chains-app.json"};
    {
        webui::AppConfig config{path};
        config.Load();
        BOOST_CHECK(!config.DataDir());
        BOOST_CHECK(!config.ResetPending());
        BOOST_CHECK_EQUAL(config.Prefs("main").size(), 0U);
        config.SetDataDir("/some/dir");
        config.SetResetPending(true);
        UniValue changes{UniValue::VOBJ};
        changes.pushKV("theme", "dark");
        changes.pushKV("fontSize", 2);
        changes.pushKV("columns", UniValue{UniValue::VARR});
        const UniValue prefs{config.UpdatePrefs("regtest", changes)};
        BOOST_CHECK_EQUAL(prefs.find_value("theme").get_str(), "dark");
    }
    BOOST_CHECK(fs::exists(path));
    webui::AppConfig config{path};
    config.Load();
    BOOST_CHECK_EQUAL(*config.DataDir(), "/some/dir");
    BOOST_CHECK(config.ResetPending());
    // Preferences are per network.
    BOOST_CHECK_EQUAL(config.Prefs("main").size(), 0U);
    BOOST_CHECK_EQUAL(config.Prefs("regtest").find_value("fontSize").getInt<int>(), 2);

    // A null removes a key, others stay.
    UniValue changes{UniValue::VOBJ};
    changes.pushKV("theme", UniValue{});
    changes.pushKV("unit", "sat");
    const UniValue prefs{config.UpdatePrefs("regtest", changes)};
    BOOST_CHECK(prefs.find_value("theme").isNull());
    BOOST_CHECK_EQUAL(prefs.find_value("unit").get_str(), "sat");
    BOOST_CHECK_EQUAL(prefs.find_value("fontSize").getInt<int>(), 2);

    config.SetResetPending(false);
    BOOST_CHECK(!config.ResetPending());
    config.ResetPrefs("regtest");
    BOOST_CHECK_EQUAL(config.Prefs("regtest").size(), 0U);
    // The data directory survives a reset of the preferences.
    BOOST_CHECK_EQUAL(*config.DataDir(), "/some/dir");
}

BOOST_AUTO_TEST_CASE(unreadable_file)
{
    const fs::path path{m_args.GetDataDirBase() / "broken.json"};
    {
        std::ofstream file{static_cast<const std::filesystem::path&>(path)};
        file << "{ not json";
    }
    webui::AppConfig config{path};
    config.Load();
    BOOST_CHECK(!config.DataDir());
    // It is written anew on the first change.
    config.SetDataDir("/x");
    webui::AppConfig again{path};
    again.Load();
    BOOST_CHECK_EQUAL(*again.DataDir(), "/x");
}

BOOST_AUTO_TEST_CASE(size_limit)
{
    webui::AppConfig config{m_args.GetDataDirBase() / "big.json"};
    UniValue changes{UniValue::VOBJ};
    changes.pushKV("blob", std::string(webui::AppConfig::MAX_PREFS_SIZE + 1, 'x'));
    BOOST_CHECK_THROW(config.UpdatePrefs("main", changes), std::runtime_error);
    BOOST_CHECK_EQUAL(config.Prefs("main").size(), 0U);
}

BOOST_AUTO_TEST_CASE(default_path)
{
    const fs::path path{webui::AppConfig::DefaultPath()};
    BOOST_CHECK_EQUAL(fs::PathToString(path.filename()), std::string{webui::chain::APP} + ".json");
    BOOST_CHECK_EQUAL(fs::PathToString(path.parent_path().filename()), std::string{webui::chain::NAME});
}

BOOST_AUTO_TEST_SUITE_END()
