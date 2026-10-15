// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/secretfile.h>

#include <random.h>
#include <tinyformat.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <cstdio>

#ifdef WIN32
#include <fstream>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace common {

util::Result<void> WriteSecretFile(const fs::path& path, std::string_view secret)
{
    const std::string name{fs::PathToString(path)};
    // Next to it (same folder, so that the rename is atomic), with a name no one can guess.
    const fs::path temp{fs::PathFromString(fs::PathToString(path) + ".new-" + HexStr(GetRandHash()).substr(0, 16))};
#ifndef WIN32
    const int fd{open(temp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, S_IRUSR | S_IWUSR)};
    if (fd < 0) return util::Error{Untranslated(strprintf("Cannot create %s", fs::PathToString(temp)))};
    // The umask can only take bits away; make sure no one else was left any.
    bool ok{fchmod(fd, S_IRUSR | S_IWUSR) == 0};
    size_t written{0};
    while (ok && written < secret.size()) {
        const ssize_t n{write(fd, secret.data() + written, secret.size() - written)};
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) ok = false;
        else written += n;
    }
    ok = ok && fsync(fd) == 0;
    ok = (close(fd) == 0) && ok;
#else
    bool ok{false};
    {
        std::ofstream file{temp.std_path(), std::ios::binary | std::ios::trunc};
        if (file.is_open()) {
            file.write(secret.data(), secret.size());
            file.flush();
            ok = file.good();
        }
    }
#endif
    if (!ok) {
        std::error_code ec;
        fs::remove(temp, ec);
        return util::Error{Untranslated(strprintf("Cannot write %s", name))};
    }
    if (!RenameOver(temp, path)) {
        std::error_code ec;
        fs::remove(temp, ec);
        return util::Error{Untranslated(strprintf("Cannot replace %s", name))};
    }
    return {};
}

} // namespace common
