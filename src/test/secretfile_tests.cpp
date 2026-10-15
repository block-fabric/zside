// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/secretfile.h>
#include <test/util/setup_common.h>

#include <boost/test/unit_test.hpp>

#include <fstream>
#include <iterator>

BOOST_FIXTURE_TEST_SUITE(secretfile_tests, BasicTestingSetup)

namespace {
std::string Read(const fs::path& path)
{
    std::ifstream file{path.std_path(), std::ios::binary};
    return {std::istreambuf_iterator<char>{file}, {}};
}
size_t FilesIn(const fs::path& dir)
{
    return std::distance(fs::directory_iterator{dir}, fs::directory_iterator{});
}
} // namespace

BOOST_AUTO_TEST_CASE(write_secret_file)
{
    const fs::path dir{m_args.GetDataDirBase() / "secret"};
    fs::create_directories(dir);
    const fs::path path{dir / "mainchain.rpcpass"};
    BOOST_REQUIRE(common::WriteSecretFile(path, "first\n"));
    BOOST_CHECK_EQUAL(Read(path), "first\n");
#ifndef WIN32
    BOOST_CHECK(fs::status(path).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
#endif

    // An existing file, readable by everyone, is replaced by one only its owner can read.
    fs::permissions(path, fs::perms::all, fs::perm_options::replace);
    BOOST_REQUIRE(common::WriteSecretFile(path, "second\n"));
    BOOST_CHECK_EQUAL(Read(path), "second\n");
#ifndef WIN32
    BOOST_CHECK(fs::status(path).permissions() == (fs::perms::owner_read | fs::perms::owner_write));
#endif
    // Nothing is left behind.
    BOOST_CHECK_EQUAL(FilesIn(dir), 1U);

    // A directory that does not exist: an error that does not contain the secret.
    const auto result{common::WriteSecretFile(dir / "missing" / "file", "hidden-secret")};
    BOOST_REQUIRE(!result);
    BOOST_CHECK(util::ErrorString(result).original.find("hidden-secret") == std::string::npos);
}

BOOST_AUTO_TEST_SUITE_END()
