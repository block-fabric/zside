// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_SIDECHAINNODES_H
#define BITCOIN_WEBUI_SIDECHAINNODES_H

#include <univalue.h>
#include <util/fs.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

class ArgsManager;

namespace webui {

class Bridge;

/**
 * The Sidechain Nodes window of chains-qt (sidechainnodes.cpp), for the page: the sidechains
 * that can be run from here (the built-in ones and the ones the user added), where their
 * programs are (chosen by the user in a folder chooser, or downloaded with a hash check), and
 * their nodes: started in the background pointed at this node, stopped, asked how they are
 * doing, merged mining turned on and off, one block mined for a fee, the end of their log.
 *
 * Only programs in a folder the user chose (or a download the user checked by its hash) are
 * ever run, and only the programs of a sidechain: <stem>d, <stem>-cli, <stem>-qt. A download is
 * https only, has a SHA-256 checked before anything is unpacked, and happens only once the user
 * accepted it in a dialog of the desktop (ConfirmFn) showing the address, the hash and the folder:
 * the page can ask for one, it cannot make one happen.
 */
class SidechainNodes
{
public:
    struct Sidechain {
        std::string name;
        //! Stem of the names of its programs: <stem>d, <stem>-cli, <stem>-qt.
        std::string stem;
        int slot{0};
        std::string description;
        bool builtin{true};
    };

    //! The value of the app settings with the sidechains added and their folders (AppConfig::Value), and how to keep it.
    using LoadFn = std::function<UniValue()>;
    using StoreFn = std::function<void(const UniValue&)>;
    //! Ask the user for a folder (title, the folder to start in); nothing when cancelled.
    using ChooseFolderFn = std::function<std::optional<std::string>(const std::string& title, const std::string& start)>;
    /**
     * Ask the user in a dialog of the desktop, which the page can neither draw nor answer (Window::Confirm):
     * title, message, the text of the button that accepts. True when accepted.
     */
    using ConfirmFn = std::function<bool(const std::string& title, const std::string& message, const std::string& accept)>;

    /** A release of a sidechain built into this app: where to download it, and the SHA-256 of the archive. */
    struct Release {
        std::string stem;
        //! The platform it is for ("x86_64-linux-gnu", "x86_64-w64-mingw32").
        std::string platform;
        std::string version;
        std::string url;
        std::string sha256;
    };
    /** The releases built into this app (sidechainreleases.h), and the platform of this build. */
    static std::vector<Release> BuiltinReleases();
    static std::string Platform();

    SidechainNodes(ArgsManager& args, LoadFn load, StoreFn store, ChooseFolderFn choose_folder, ConfirmFn confirm,
                   std::vector<Release> releases = BuiltinReleases());

    /** Make the methods (sidechainNodes.*) available to the page. */
    void Register(Bridge& bridge);

    /** Run a method directly (the tests; Register uses the same functions). Throws HostError. */
    UniValue Call(const std::string& method, const UniValue& params);

    /** The longest name of a sidechain, in characters. */
    static constexpr size_t NAME_MAX_CHARS{64};
    /**
     * Whether a name can be given to a sidechain added by hand: valid UTF-8, one line, of at most
     * NAME_MAX_CHARS characters, none of them a control or format character (a new line, a tab, a
     * mark that turns text around). The name is shown in the confirmation of a download.
     */
    static bool IsValidName(std::string_view name);
    /**
     * A name as it is shown, whatever was stored (settings written by hand, an older version): every
     * control or format character becomes a space, spaces are folded, invalid UTF-8 becomes U+FFFD, and
     * it is cut to NAME_MAX_CHARS characters.
     */
    static std::string DisplayName(std::string_view name);
    /**
     * The host of a download address, in lower case; nothing when the address is not taken: not https, too
     * long, not plain printable ASCII, with a user name or password before the host (https://a@b/), with a
     * host that is not letters, digits, '.' and '-', or a port that is not a number.
     */
    static std::optional<std::string> DownloadHost(std::string_view url);

    std::vector<Sidechain> Sidechains() const;
    std::string BinDir(const std::string& stem) const;
    /** Where the node of a sidechain run from here keeps its data: <datadir>/sidechains/<stem>/data. */
    fs::path DataDir(const Sidechain& sidechain) const;
    /** The arguments a program of a sidechain is run with to reach its node on the network of this node. */
    std::vector<std::string> NetworkArgs(const Sidechain& sidechain) const;
    /**
     * The arguments its node is started with on top of those (pointed at this node). Never a password: with
     * -rpcpassword, the node reads it from PasswordFile (-mainchainrpcpasswordfile), which WritePasswordFile
     * writes before the node starts.
     */
    std::vector<std::string> NodeArgs(const Sidechain& sidechain, const std::optional<std::string>& wallet) const;
    /** <sidechain data dir>/mainchain.rpcpass */
    fs::path PasswordFile(const Sidechain& sidechain) const;
    /** Write the RPC password of this node to PasswordFile, owner-only and replaced atomically; remove it if this node has none. */
    void WritePasswordFile(const Sidechain& sidechain) const;

    /** The output of a program run to its end: exit code (-1: it could not be started), stdout, stderr. */
    struct Output {
        int code{-1};
        std::string out;
        std::string err;
    };
    /** Run a program and wait for it (at most `timeout_s` seconds, then it is killed). */
    static Output Run(const std::vector<std::string>& argv, int timeout_s);
    /** Start a program that keeps running after this one, sharing nothing with it. */
    static bool Launch(const std::vector<std::string>& argv);

private:
    std::optional<Sidechain> Find(const std::string& stem) const;
    std::string Program(const Sidechain& sidechain, const std::string& suffix) const;
    /** Run a command of the node of a sidechain: its answer (JSON, or a bare word as a string), or the error. */
    std::pair<std::optional<UniValue>, std::string> Cli(const Sidechain& sidechain, const std::vector<std::string>& command) const;
    void SetBinDir(const std::string& stem, const std::string& dir);

    ArgsManager& m_args;
    LoadFn m_load;
    StoreFn m_store;
    ChooseFolderFn m_choose_folder;
    ConfirmFn m_confirm;
    std::vector<Release> m_releases;
};

} // namespace webui

#endif // BITCOIN_WEBUI_SIDECHAINNODES_H
