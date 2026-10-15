// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <sidechain/mainclient.h>
#include <test/util/setup_common.h>
#include <util/translation.h>

#include <boost/test/unit_test.hpp>

#include <fstream>

using namespace sidechain;

namespace {
fs::path Write(const fs::path& path, const std::string& content)
{
    std::ofstream file{path.std_path(), std::ios::binary | std::ios::trunc};
    file << content;
    return path;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(sidechain_mainclient_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(password_file_first_line)
{
    const fs::path path{Write(m_args.GetDataDirBase() / "pw", "s3cret\r\nignored\n")};
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto password{ReadPasswordFile(path)};
    BOOST_REQUIRE(password);
    BOOST_CHECK_EQUAL(*password, "s3cret");
}

BOOST_AUTO_TEST_CASE(password_file_errors_hide_content)
{
    BOOST_CHECK(!ReadPasswordFile(m_args.GetDataDirBase() / "missing"));
    const fs::path empty{Write(m_args.GetDataDirBase() / "empty", "\nsecond-line\n")};
    fs::permissions(empty, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    const auto result{ReadPasswordFile(empty)};
    BOOST_REQUIRE(!result);
    BOOST_CHECK(util::ErrorString(result).original.find("second-line") == std::string::npos);
    const fs::path large{Write(m_args.GetDataDirBase() / "large", std::string(5000, 'x'))};
    fs::permissions(large, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace);
    BOOST_CHECK(!ReadPasswordFile(large));
}

#ifndef WIN32
BOOST_AUTO_TEST_CASE(password_file_refused_when_others_can_read)
{
    const fs::path path{Write(m_args.GetDataDirBase() / "pw", "topsecret\n")};
    for (const fs::perms extra : {fs::perms::group_read, fs::perms::others_read, fs::perms::group_write, fs::perms::others_write}) {
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write | extra, fs::perm_options::replace);
        const auto result{ReadPasswordFile(path)};
        BOOST_REQUIRE(!result);
        const std::string error{util::ErrorString(result).original};
        BOOST_CHECK(error.find("chmod 600") != std::string::npos);
        BOOST_CHECK(error.find("topsecret") == std::string::npos);
    }
    fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace);
    BOOST_CHECK(ReadPasswordFile(path));
    // A directory is not a password file.
    fs::create_directories(m_args.GetDataDirBase() / "dir");
    fs::permissions(m_args.GetDataDirBase() / "dir", fs::perms::owner_all, fs::perm_options::replace);
    BOOST_CHECK(!ReadPasswordFile(m_args.GetDataDirBase() / "dir"));
}
#endif

BOOST_AUTO_TEST_SUITE_END()
