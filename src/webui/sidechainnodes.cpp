// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/sidechainnodes.h>

#include <chainparams.h>
#include <chainparamsbase.h>
#include <common/args.h>
#include <common/secretfile.h>
#include <crypto/sha256.h>
#include <logging.h>
#include <tinyformat.h>
#include <util/chaintype.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <support/cleanse.h>
#include <webui/bridge.h>
#include <webui/guard.h>
#include <webui/sidechainreleases.h>

#include <chrono>
#include <fstream>
#include <thread>

#ifndef WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <util/subprocess.h>
#include <windows.h>
#endif

namespace webui {
using util::SplitString;
using util::TrimString;
namespace {
#ifdef WIN32
const std::string EXE{".exe"};
#else
const std::string EXE{};
#endif

const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string ParamStr(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return value.get_str();
}

std::optional<std::string> ParamWallet(const UniValue& params)
{
    const UniValue& wallet{Param(params, "wallet")};
    if (wallet.isStr()) return wallet.get_str();
    return std::nullopt;
}

/** SHA-256 of a file, in hexadecimal; empty if it cannot be read. */
std::string FileHash(const fs::path& path)
{
    std::ifstream file{path.std_path(), std::ios::binary};
    if (!file) return {};
    CSHA256 hasher;
    std::vector<char> chunk(1 << 20);
    while (file) {
        file.read(chunk.data(), chunk.size());
        if (file.gcount() > 0) hasher.Write(reinterpret_cast<const unsigned char*>(chunk.data()), file.gcount());
    }
    unsigned char hash[CSHA256::OUTPUT_SIZE];
    hasher.Finalize(hash);
    return HexStr(hash);
}

std::string Text(const UniValue& value) { return value.isStr() ? value.get_str() : value.write(); }

bool Exists(const fs::path& path)
{
    std::error_code ec;
    return std::filesystem::exists(path, ec);
}

UniValue Said(std::vector<std::string> lines)
{
    UniValue out{UniValue::VOBJ};
    UniValue log{UniValue::VARR};
    for (auto& line : lines) log.push_back(std::move(line));
    out.pushKV("log", std::move(log));
    return out;
}
} // namespace

using guard::IsControlOrFormat;
using guard::NextCodePoint;

bool SidechainNodes::IsValidName(std::string_view name)
{
    size_t pos{0}, chars{0};
    while (pos < name.size()) {
        const auto cp{NextCodePoint(name, pos)};
        if (!cp || IsControlOrFormat(*cp)) return false;
        if (++chars > NAME_MAX_CHARS) return false;
    }
    return chars > 0;
}

std::string SidechainNodes::DisplayName(std::string_view name)
{
    std::string out{guard::OneLine(name, NAME_MAX_CHARS)};
    return out.empty() ? std::string{"(no name)"} : out;
}

std::optional<std::string> SidechainNodes::DownloadHost(std::string_view url)
{
    return guard::WebHost(url, /*https_only=*/true);
}

SidechainNodes::SidechainNodes(ArgsManager& args, LoadFn load, StoreFn store, ChooseFolderFn choose_folder, ConfirmFn confirm, std::vector<Release> releases)
    : m_args(args), m_load(std::move(load)), m_store(std::move(store)), m_choose_folder(std::move(choose_folder)), m_confirm(std::move(confirm)), m_releases(std::move(releases))
{
}

std::vector<SidechainNodes::Release> SidechainNodes::BuiltinReleases()
{
    std::vector<Release> out;
    for (const auto& r : SIDECHAIN_RELEASES) {
        if (r.stem) out.push_back({r.stem, r.platform, r.version, r.url, r.sha256});
    }
    return out;
}

std::string SidechainNodes::Platform()
{
#if defined(WIN32)
    return "x86_64-w64-mingw32";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__aarch64__)
    return "aarch64-linux-gnu";
#else
    return "x86_64-linux-gnu";
#endif
}

std::vector<SidechainNodes::Sidechain> SidechainNodes::Sidechains() const
{
    std::vector<Sidechain> list{
        {"Thunder", "thunder", 2, "Large blocks, for volume", true},
        {"Hivemind", "hivemind", 9, "Prediction markets", true},
        {"BitNames", "bitnames", 3, "Names, and their .x domains", true},
        {"Sidechain template", "sidechain", 0, "A sidechain with nothing but what every sidechain needs", true},
    };
    const UniValue custom{m_load().find_value("custom")};
    if (custom.isArray()) {
        for (const UniValue& entry : custom.getValues()) {
            const UniValue& name{entry.find_value("name")};
            const UniValue& stem{entry.find_value("stem")};
            const UniValue& slot{entry.find_value("slot")};
            // Shown cleaned up, whatever the settings hold: a name is one short line.
            if (name.isStr() && stem.isStr() && slot.isNum()) list.push_back({DisplayName(name.get_str()), stem.get_str(), slot.getInt<int>(), "Added by you", false});
        }
    }
    return list;
}

std::optional<SidechainNodes::Sidechain> SidechainNodes::Find(const std::string& stem) const
{
    for (const Sidechain& s : Sidechains()) {
        if (s.stem == stem) return s;
    }
    return std::nullopt;
}

std::string SidechainNodes::BinDir(const std::string& stem) const
{
    const UniValue all{m_load()};
    const UniValue& dir{all.find_value("bindir").find_value(stem)};
    return dir.isStr() ? dir.get_str() : std::string{};
}

void SidechainNodes::SetBinDir(const std::string& stem, const std::string& dir)
{
    UniValue all{m_load()};
    if (!all.isObject()) all = UniValue{UniValue::VOBJ};
    UniValue dirs{UniValue::VOBJ};
    const UniValue& old{all.find_value("bindir")};
    if (old.isObject()) {
        for (size_t i{0}; i < old.size(); ++i) {
            if (old.getKeys()[i] != stem) dirs.pushKV(old.getKeys()[i], old.getValues()[i]);
        }
    }
    if (!dir.empty()) dirs.pushKV(stem, dir);
    UniValue next{UniValue::VOBJ};
    for (size_t i{0}; i < all.size(); ++i) {
        if (all.getKeys()[i] != "bindir") next.pushKV(all.getKeys()[i], all.getValues()[i]);
    }
    next.pushKV("bindir", std::move(dirs));
    m_store(next);
}

std::string SidechainNodes::Program(const Sidechain& sidechain, const std::string& suffix) const
{
    return fs::PathToString(fs::PathFromString(BinDir(sidechain.stem)) / fs::PathFromString(sidechain.stem + suffix + EXE));
}

fs::path SidechainNodes::DataDir(const Sidechain& sidechain) const
{
    // With the data of this node, apart from whatever else on this computer goes by the name of the sidechain.
    return m_args.GetDataDirBase() / "sidechains" / fs::PathFromString(sidechain.stem) / "data";
}

std::vector<std::string> SidechainNodes::NetworkArgs(const Sidechain& sidechain) const
{
    std::vector<std::string> args{"-datadir=" + fs::PathToString(DataDir(sidechain))};
    switch (m_args.GetChainType()) {
    case ChainType::MAIN: break;
    case ChainType::TESTNET: args.emplace_back("-testnet"); break;
    case ChainType::SIGNET: args.emplace_back("-signet"); break;
    case ChainType::REGTEST:
        args.emplace_back("-regtest");
        args.push_back(strprintf("-sidechainslot=%d", sidechain.slot));
        break;
    default: break;
    }
    return args;
}

std::vector<std::string> SidechainNodes::NodeArgs(const Sidechain& sidechain, const std::optional<std::string>& wallet) const
{
    std::vector<std::string> args{NetworkArgs(sidechain)};
    args.push_back(strprintf("-mainchainrpcport=%d", m_args.GetIntArg("-rpcport", BaseParams().RPCPort())));
    if (m_args.IsArgSet("-rpcpassword")) {
        // Never the password itself: any local user can read the command line of a process.
        args.push_back("-mainchainrpcuser=" + m_args.GetArg("-rpcuser", ""));
        args.push_back("-mainchainrpcpasswordfile=" + fs::PathToString(PasswordFile(sidechain)));
    } else {
        args.push_back("-mainchainrpccookiefile=" + fs::PathToString(m_args.GetDataDirNet() / ".cookie"));
    }
    if (wallet) args.push_back("-mainchainrpcwallet=" + *wallet);
    return args;
}

fs::path SidechainNodes::PasswordFile(const Sidechain& sidechain) const
{
    return DataDir(sidechain) / "mainchain.rpcpass";
}

void SidechainNodes::WritePasswordFile(const Sidechain& sidechain) const
{
    const fs::path path{PasswordFile(sidechain)};
    if (!m_args.IsArgSet("-rpcpassword")) {
        // The cookie file is used: no password is left lying around.
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return;
    }
    std::string password{m_args.GetArg("-rpcpassword", "") + "\n"};
    const auto written{common::WriteSecretFile(path, password)};
    memory_cleanse(password.data(), password.size());
    if (!written) throw HostError(strprintf("%s: the password of this node could not be written for it (%s)", sidechain.name, util::ErrorString(written).original));
}

std::pair<std::optional<UniValue>, std::string> SidechainNodes::Cli(const Sidechain& sidechain, const std::vector<std::string>& command) const
{
    std::vector<std::string> argv{Program(sidechain, "-cli")};
    for (const auto& a : NetworkArgs(sidechain)) argv.push_back(a);
    argv.emplace_back("-rpcclienttimeout=10");
    for (const auto& c : command) argv.push_back(c);
    const Output result{Run(argv, 30)};
    const std::string out{TrimString(result.out)};
    const std::string err{TrimString(result.err)};
    if (result.code == -1 && out.empty() && err.empty()) return {std::nullopt, "The program could not be started."};
    if (result.code != 0) return {std::nullopt, err.empty() ? out : err};
    UniValue value;
    // A command that answers with a bare word, like a new address, is not JSON.
    if (!value.read(out)) value = UniValue{out};
    return {value, ""};
}

UniValue SidechainNodes::Call(const std::string& method, const UniValue& params)
{
    const auto chosen{[&]() -> Sidechain {
        const auto s{Find(ParamStr(params, "stem"))};
        if (!s) throw HostError("Select a sidechain in the list first.");
        return *s;
    }};
    const auto installed{[&](const Sidechain& s) {
        const std::string dir{BinDir(s.stem)};
        return !dir.empty() && Exists(fs::PathFromString(Program(s, "d")));
    }};

    if (method == "sidechainNodes.list") {
        UniValue out{UniValue::VOBJ};
        // Sidechain nodes follow this node over RPC.
        out.pushKV("server", m_args.GetBoolArg("-server", false));
        out.pushKV("network", ChainTypeToString(m_args.GetChainType()));
        UniValue list{UniValue::VARR};
        for (const Sidechain& s : Sidechains()) {
            UniValue entry{UniValue::VOBJ};
            entry.pushKV("name", s.name);
            entry.pushKV("stem", s.stem);
            entry.pushKV("slot", s.slot);
            entry.pushKV("description", s.description);
            entry.pushKV("builtin", s.builtin);
            const std::string dir{BinDir(s.stem)};
            entry.pushKV("dir", dir);
            entry.pushKV("installed", installed(s));
            entry.pushKV("programs", installed(s) ? dir : dir.empty() ? std::string{"not on this computer yet: Locate or Download"} : strprintf("no %s in %s", s.stem + "d", dir));
            entry.pushKV("dataDir", fs::PathToString(DataDir(s)));
            for (const Release& r : m_releases) {
                if (r.stem != s.stem || r.platform != Platform()) continue;
                UniValue release{UniValue::VOBJ};
                release.pushKV("version", r.version);
                release.pushKV("url", r.url);
                release.pushKV("sha256", r.sha256);
                entry.pushKV("release", std::move(release));
                break;
            }
            list.push_back(std::move(entry));
        }
        out.pushKV("sidechains", std::move(list));
        return out;
    }
    if (method == "sidechainNodes.status") {
        const Sidechain s{chosen()};
        UniValue out{UniValue::VOBJ};
        out.pushKV("stem", s.stem);
        if (!installed(s)) {
            out.pushKV("installed", false);
            out.pushKV("running", false);
            return out;
        }
        out.pushKV("installed", true);
        const auto [blocks, blocks_error] = Cli(s, {"getblockcount"});
        out.pushKV("running", blocks.has_value());
        if (!blocks) return out;
        out.pushKV("blocks", *blocks);
        if (const auto [info, error] = Cli(s, {"getmainchaininfo"}); info && info->isObject()) {
            const UniValue& connected{info->find_value("connected")};
            out.pushKV("mainchain", connected.isBool() && connected.get_bool() ? strprintf("yes, at block %s", Text(info->find_value("height"))) : strprintf("no: %s", Text(info->find_value("error"))));
        }
        if (const auto [info, error] = Cli(s, {"getbmminfo"}); info && info->isObject()) {
            const bool mining{info->find_value("mining").isBool() && info->find_value("mining").get_bool()};
            // A node that mines only asks for a block when the block has something to do.
            const bool idle{mining && info->find_value("idle").isBool() && info->find_value("idle").get_bool()};
            std::string text{!mining ? "off" : idle ? strprintf("auto, waiting for fees, %s blocks", Text(info->find_value("blocks"))) : strprintf("auto, %s blocks", Text(info->find_value("blocks")))};
            if (info->exists("error")) text += strprintf(" (%s)", Text(info->find_value("error")));
            out.pushKV("mining", mining);
            out.pushKV("miningText", text);
        }
        return out;
    }
    if (method == "sidechainNodes.locate") {
        const Sidechain s{chosen()};
        const auto dir{m_choose_folder(strprintf("The folder with %s", s.stem + "d"), BinDir(s.stem))};
        if (!dir) return UniValue{};
        if (!Exists(fs::PathFromString(*dir) / fs::PathFromString(s.stem + "d" + EXE))) throw HostError(strprintf("There is no %s in that folder.", s.stem + "d"));
        SetBinDir(s.stem, *dir);
        return Said({strprintf("%s: programs in %s", s.name, *dir)});
    }
    if (method == "sidechainNodes.download") {
        const Sidechain s{chosen()};
        // The release built into the app ({release: true}), or an address and a hash the user typed.
        std::string url, expected, version;
        const bool builtin{Param(params, "release").isTrue()};
        if (builtin) {
            for (const Release& r : m_releases) {
                if (r.stem == s.stem && r.platform == Platform()) {
                    url = r.url;
                    expected = ToLower(r.sha256);
                    version = r.version;
                }
            }
            if (url.empty()) throw HostError(strprintf("This app has no release of %s for this computer built in.", s.name));
        } else {
            url = TrimString(ParamStr(params, "url"));
            expected = ToLower(TrimString(ParamStr(params, "sha256")));
        }
        if (url.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "The address of the release is needed.");
        // A hash is required, and checked before anything is unpacked.
        if (expected.size() != 64 || !IsHex(expected)) throw HostError("A SHA-256 hash is 64 hexadecimal characters. Programs that hold coins are not installed unchecked.");
        // https only: an http download can be replaced on its way, and curl takes other schemes (file:, dict:...).
        if (!url.starts_with("https://")) throw HostError("The address of a release starts with https://");
        // No user name before the host: https://github.com@evil.example/ is a download from evil.example.
        const size_t path_start{url.find_first_of("/?#", 8)};
        if (url.substr(8, path_start == std::string::npos ? std::string::npos : path_start - 8).find('@') != std::string::npos) throw HostError("The address of a release has no user name or password before its host (user@host): it would hide where the download comes from.");
        const std::optional<std::string> host{DownloadHost(url)};
        if (!host) throw HostError("The address of a release is https://, then a host of letters, digits, '.' and '-', with no spaces or quotes.");
        const fs::path target{m_args.GetDataDirBase() / "sidechains" / fs::PathFromString(s.stem)};
        // The user accepts the download in a dialog of the desktop, which the page cannot answer. What the
        // page gave (the name, the address) comes after what this app decides: the title says whether the
        // release is built into the app or was typed in it, in fixed words; the first line is the host the
        // download comes from, parsed here, alone on its line.
        const std::string title{builtin ? "Install a release built into this app" : "Install from an address typed in this app"};
        const std::string trust{builtin ? strprintf("This is the release %s built into this app.", version)
                                        : std::string{"This address was typed in the app. It is NOT a release built into this app."}};
        const std::string question{strprintf("Host: %s\n%s\n\nSidechain: %s\nFrom: %s\nSHA-256: %s\nInto: %s\n\n"
                                             "The programs in the archive will run on this computer with access to your files: accept only if you trust %s.",
                                             *host, trust, s.name, url, expected, fs::PathToString(target), *host)};
        if (!m_confirm || !m_confirm(title, question, "Download and install")) {
            return Said({strprintf("%s: the download was not accepted.", s.name)});
        }
        std::error_code ec;
        std::filesystem::create_directories(target, ec);
        const fs::path archive{target / "download.tar.gz"};
        std::filesystem::remove(archive, ec);
        std::vector<std::string> log{strprintf("%s: downloading %s", s.name, url)};
        const Output fetch{Run({"curl", "--fail", "--location", "--silent", "--show-error", "--proto", "=https", "--proto-redir", "=https", "--output", fs::PathToString(archive), url}, 3600)};
        if (fetch.code == -1 && fetch.err.empty()) {
            log.emplace_back("The download needs the program curl, which could not be started.");
            return Said(log);
        }
        if (fetch.code != 0) {
            std::filesystem::remove(archive, ec);
            log.push_back(strprintf("%s: the download failed. %s", s.name, TrimString(fetch.err)));
            return Said(log);
        }
        const std::string hash{FileHash(archive)};
        if (hash != expected) {
            std::filesystem::remove(archive, ec);
            log.push_back(strprintf("%s: the download does NOT have the expected hash and was thrown away. It has %s.", s.name, hash));
            UniValue out{Said(log)};
            out.pushKV("error", "The download does not have the hash you gave. It was thrown away.");
            return out;
        }
        log.push_back(strprintf("%s: the hash matches; unpacking", s.name));
        const Output unpack{Run({"tar", "-xzf", fs::PathToString(archive), "-C", fs::PathToString(target)}, 600)};
        std::filesystem::remove(archive, ec);
        if (unpack.code != 0) {
            log.push_back(strprintf("%s: unpacking failed. %s", s.name, TrimString(unpack.err)));
            return Said(log);
        }
        // The programs are somewhere in what was unpacked.
        std::optional<fs::path> found;
        for (auto it{std::filesystem::recursive_directory_iterator(target, ec)}; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (it->is_regular_file(ec) && it->path().filename() == fs::PathFromString(s.stem + "d" + EXE)) {
                found = it->path().parent_path();
                break;
            }
        }
        if (!found) {
            log.push_back(strprintf("%s: the archive has no %s in it.", s.name, s.stem + "d"));
            return Said(log);
        }
        SetBinDir(s.stem, fs::PathToString(*found));
        log.push_back(strprintf("%s: installed in %s", s.name, fs::PathToString(*found)));
        return Said(log);
    }
    if (method == "sidechainNodes.add") {
        const std::string name{TrimString(ParamStr(params, "name"))};
        const std::string stem{TrimString(ParamStr(params, "stem"))};
        const UniValue& slot{Param(params, "slot")};
        if (name.empty() || stem.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "The name of the sidechain and of its programs are needed.");
        // The name is shown in the confirmation of a download: one line, printable, not too long.
        if (!IsValidName(name)) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("The name of a sidechain is one line of at most %d characters, without control characters.", NAME_MAX_CHARS));
        // The stem names programs: letters, digits, '-' and '_' only, so that it cannot point anywhere else.
        if (stem.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos) {
            throw HostError("What the programs are named after has letters, digits, '-' and '_' only.");
        }
        if (!slot.isNum() || slot.getInt<int>() < 0 || slot.getInt<int>() > 65535) throw HostError(BRIDGE_INVALID_REQUEST, "The slot is a number from 0 to 65535.");
        if (Find(stem)) throw HostError("A sidechain with these programs is in the list already.");
        UniValue all{m_load()};
        if (!all.isObject()) all = UniValue{UniValue::VOBJ};
        UniValue custom{all.find_value("custom").isArray() ? all.find_value("custom") : UniValue{UniValue::VARR}};
        UniValue entry{UniValue::VOBJ};
        entry.pushKV("name", name);
        entry.pushKV("stem", stem);
        entry.pushKV("slot", slot.getInt<int>());
        custom.push_back(std::move(entry));
        UniValue next{UniValue::VOBJ};
        for (size_t i{0}; i < all.size(); ++i) {
            if (all.getKeys()[i] != "custom") next.pushKV(all.getKeys()[i], all.getValues()[i]);
        }
        next.pushKV("custom", std::move(custom));
        m_store(next);
        return Said({strprintf("%s was added to the list.", name)});
    }
    if (method == "sidechainNodes.remove") {
        const Sidechain s{chosen()};
        if (s.builtin) throw HostError(strprintf("%s comes with this wallet and stays in the list. Only a sidechain that was added by hand can be removed.", s.name));
        UniValue all{m_load()};
        UniValue custom{UniValue::VARR};
        for (const UniValue& entry : all.find_value("custom").getValues()) {
            if (!entry.find_value("stem").isStr() || entry.find_value("stem").get_str() != s.stem) custom.push_back(entry);
        }
        UniValue next{UniValue::VOBJ};
        for (size_t i{0}; i < all.size(); ++i) {
            if (all.getKeys()[i] != "custom") next.pushKV(all.getKeys()[i], all.getValues()[i]);
        }
        next.pushKV("custom", std::move(custom));
        m_store(next);
        // Where its programs are is forgotten with it.
        SetBinDir(s.stem, "");
        return Said({strprintf("%s was taken off the list.", s.name)});
    }
    if (method == "sidechainNodes.start" || method == "sidechainNodes.startWallet") {
        const Sidechain s{chosen()};
        if (!installed(s)) throw HostError("Select a sidechain whose programs are on this computer.");
        std::error_code ec;
        std::filesystem::create_directories(DataDir(s), ec);
        WritePasswordFile(s);
        const bool wallet_window{method == "sidechainNodes.startWallet"};
        std::vector<std::string> argv{Program(s, wallet_window ? "-qt" : "d")};
        for (const auto& a : NodeArgs(s, ParamWallet(params))) argv.push_back(a);
        argv.emplace_back(wallet_window ? "-server" : "-daemon");
        const bool ok{Launch(argv)};
        const std::string what{wallet_window ? (ok ? "wallet started" : "the wallet could not be started") : (ok ? "node started" : "the node could not be started")};
        return Said({strprintf("%s: %s", s.name, what)});
    }
    if (method == "sidechainNodes.stop") {
        const Sidechain s{chosen()};
        const auto [result, error] = Cli(s, {"stop"});
        return Said({result ? strprintf("%s: stopping", s.name) : strprintf("%s: not stopped. %s", s.name, error)});
    }
    if (method == "sidechainNodes.mineOnce") {
        const Sidechain s{chosen()};
        const std::string amount{TrimString(ParamStr(params, "amount"))};
        const auto [address, address_error] = Cli(s, {"getnewaddress", "mining"});
        if (!address) return Said({strprintf("%s: %s (the sidechain node needs a wallet: turn automatic mining on once, or open its wallet)", s.name, address_error)});
        const auto [result, error] = Cli(s, {"requestbmmblock", Text(*address), amount});
        if (!result || !result->isObject()) return Said({strprintf("%s: %s", s.name, error)});
        const UniValue& txs{result->find_value("transactions")};
        const UniValue& offered{result->find_value("amount")};
        return Said({strprintf("%s: asked for one block with %d transactions, offering %s. It is mined if the next block of this chain takes the offer.",
                               s.name, txs.isNum() ? txs.getInt<int>() - 1 : 0, offered.isNum() ? offered.getValStr() : amount)});
    }
    if (method == "sidechainNodes.mining") {
        const Sidechain s{chosen()};
        const UniValue& on{Param(params, "on")};
        if (!on.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "on: true or false");
        if (!on.get_bool()) {
            const auto [result, error] = Cli(s, {"setbmm", "false"});
            return Said({strprintf("%s: %s", s.name, result ? std::string{"mining turned off"} : error)});
        }
        // The fees of the blocks go to a wallet of the sidechain node; it gets one if it has none.
        const auto [wallets, error] = Cli(s, {"listwallets"});
        if (!wallets) return Said({strprintf("%s: %s", s.name, error)});
        if (!wallets->isArray() || wallets->empty()) {
            const auto [created, create_error] = Cli(s, {"createwallet", "main"});
            if (!created) {
                // It may exist and not be loaded.
                const auto [loaded, load_error] = Cli(s, {"loadwallet", "main"});
                if (!loaded) return Said({strprintf("%s: %s", s.name, create_error)});
            }
        }
        const auto [address, address_error] = Cli(s, {"getnewaddress", "mining"});
        if (!address) return Said({strprintf("%s: %s", s.name, address_error)});
        const auto [result, set_error] = Cli(s, {"setbmm", "true", Text(*address)});
        return Said({strprintf("%s: %s", s.name, result ? strprintf("automatic mining turned on; fees go to %s", Text(*address)) : set_error)});
    }
    if (method == "sidechainNodes.log") {
        const Sidechain s{chosen()};
        // The data directory has a folder per network.
        fs::path dir{DataDir(s)};
        switch (m_args.GetChainType()) {
        case ChainType::TESTNET: dir /= "testnet"; break;
        case ChainType::SIGNET: dir /= "signet"; break;
        case ChainType::REGTEST: dir /= "regtest"; break;
        default: break;
        }
        const fs::path file_path{dir / "debug.log"};
        std::ifstream file{file_path.std_path(), std::ios::binary};
        if (!file) return Said({strprintf("%s has no log at %s yet.", s.name, fs::PathToString(file_path))});
        file.seekg(0, std::ios::end);
        const std::streamoff size{file.tellg()};
        file.seekg(std::max<std::streamoff>(0, size - 4000));
        std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::vector<std::string> lines{strprintf("--- the end of %s ---", fs::PathToString(file_path))};
        const auto parts{SplitString(text, '\n')};
        // The first line is cut in the middle.
        for (size_t i{parts.size() > 1 ? 1u : 0u}; i < parts.size(); ++i) {
            if (!parts[i].empty()) lines.push_back(parts[i]);
        }
        return Said(lines);
    }
    throw HostError(BRIDGE_METHOD_NOT_FOUND, "No method " + method);
}

void SidechainNodes::Register(Bridge& bridge)
{
    for (const char* method : {"sidechainNodes.list", "sidechainNodes.status", "sidechainNodes.locate", "sidechainNodes.download", "sidechainNodes.add",
                               "sidechainNodes.remove", "sidechainNodes.start", "sidechainNodes.startWallet", "sidechainNodes.stop", "sidechainNodes.mineOnce",
                               "sidechainNodes.mining", "sidechainNodes.log"}) {
        // Downloads, folder choosers, and the nodes started, stopped and asked: apart from the quick calls.
        const std::string name{method};
        const bool quick{name == "sidechainNodes.list" || name == "sidechainNodes.log"};
        bridge.RegisterHost(name, [this, name](const UniValue& params) { return Call(name, params); }, quick ? Bridge::Thread::WORKER : Bridge::Thread::LONG);
    }
}

#ifndef WIN32
SidechainNodes::Output SidechainNodes::Run(const std::vector<std::string>& argv, int timeout_s)
{
    Output result;
    if (argv.empty()) return result;
    int out_pipe[2], err_pipe[2];
    if (pipe2(out_pipe, O_CLOEXEC) != 0) return result;
    if (pipe2(err_pipe, O_CLOEXEC) != 0) {
        close(out_pipe[0]);
        close(out_pipe[1]);
        return result;
    }
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    const pid_t pid{fork()};
    if (pid == 0) {
        // The child: nothing of this program but the pipes; then the program, found on PATH for curl and tar.
        const int devnull{open("/dev/null", O_RDONLY)};
        if (devnull >= 0) dup2(devnull, 0);
        dup2(out_pipe[1], 1);
        dup2(err_pipe[1], 2);
        const long limit{sysconf(_SC_OPEN_MAX)};
        for (int fd{3}; fd < (limit > 0 ? limit : 4096); ++fd) close(fd);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(out_pipe[1]);
    close(err_pipe[1]);
    if (pid < 0) {
        close(out_pipe[0]);
        close(err_pipe[0]);
        return result;
    }
    const auto deadline{std::chrono::steady_clock::now() + std::chrono::seconds{timeout_s}};
    pollfd fds[2]{{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    int open_count{2};
    bool killed{false};
    char buffer[65536];
    while (open_count > 0) {
        const auto left{std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count()};
        if (left <= 0) {
            kill(pid, SIGKILL);
            killed = true;
            break;
        }
        const int ready{poll(fds, 2, static_cast<int>(std::min<int64_t>(left, 1000)))};
        if (ready < 0 && errno != EINTR) break;
        for (int i{0}; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n{read(fds[i].fd, buffer, sizeof(buffer))};
            if (n > 0) {
                (i == 0 ? result.out : result.err).append(buffer, n);
                // A program that writes without end is cut off.
                if (result.out.size() + result.err.size() > 16 * 1024 * 1024) {
                    kill(pid, SIGKILL);
                    killed = true;
                }
            } else if (n == 0 || errno != EINTR) {
                close(fds[i].fd);
                fds[i].fd = -1;
                --open_count;
            }
        }
        if (killed) break;
    }
    for (auto& fd : fds) {
        if (fd.fd >= 0) close(fd.fd);
    }
    int status{0};
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (killed) {
        result.code = -2;
        if (result.err.empty()) result.err = "The program took too long and was stopped.";
    } else if (WIFEXITED(status)) {
        result.code = WEXITSTATUS(status) == 127 && result.out.empty() && result.err.empty() ? -1 : WEXITSTATUS(status);
    } else {
        result.code = -2;
    }
    return result;
}

bool SidechainNodes::Launch(const std::vector<std::string>& argv)
{
    if (argv.empty()) return false;
    std::vector<char*> args;
    for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
    args.push_back(nullptr);
    // Whether the program could be started: the grandchild tells through a pipe closed on exec.
    int status_pipe[2];
    if (pipe2(status_pipe, O_CLOEXEC) != 0) return false;
    const pid_t pid{fork()};
    if (pid == 0) {
        // A session of its own, and a second fork: it is not a child of this program, and keeps running after it.
        setsid();
        if (fork() != 0) _exit(0);
        const int devnull{open("/dev/null", O_RDWR)};
        if (devnull >= 0) {
            dup2(devnull, 0);
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        // Whatever this program has open (the ports this node listens on among it) is closed in the one started.
        const long limit{sysconf(_SC_OPEN_MAX)};
        for (int fd{3}; fd < (limit > 0 ? limit : 4096); ++fd) {
            if (fd != status_pipe[1]) close(fd);
        }
        execv(args[0], args.data());
        const char failed{1};
        (void)!write(status_pipe[1], &failed, 1);
        _exit(127);
    }
    close(status_pipe[1]);
    if (pid < 0) {
        close(status_pipe[0]);
        return false;
    }
    int status{0};
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    char failed{0};
    ssize_t n;
    while ((n = read(status_pipe[0], &failed, 1)) < 0 && errno == EINTR) {
    }
    close(status_pipe[0]);
    return n == 0;
}
#else
SidechainNodes::Output SidechainNodes::Run(const std::vector<std::string>& argv, int)
{
    // TODO(windows port): a time limit, as on the other platforms.
    namespace sp = subprocess;
    Output result;
    try {
        auto process{sp::Popen(argv, sp::input{sp::PIPE}, sp::output{sp::PIPE}, sp::error{sp::PIPE})};
        auto [out, err] = process.communicate(std::string{});
        result.out.assign(out.buf.begin(), out.buf.end());
        result.err.assign(err.buf.begin(), err.buf.end());
        result.code = process.retcode();
    } catch (const std::exception& e) {
        result.err = e.what();
    }
    return result;
}

bool SidechainNodes::Launch(const std::vector<std::string>& argv)
{
    // TODO(windows port): built and tested with the Windows build of chains-app.
    std::wstring command;
    for (const auto& a : argv) {
        if (!command.empty()) command += L' ';
        command += L'"' + std::wstring(a.begin(), a.end()) + L'"';
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &info)) return false;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    return true;
}
#endif

} // namespace webui
