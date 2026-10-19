// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/appconfig.h>
#include <webui/chainconfig.h>

#include <logging.h>
#include <util/fs_helpers.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <utility>

#ifdef WIN32
#include <shlobj.h>
#endif

namespace webui {
namespace {
const char* const KEY_DATADIR{"datadir"};
const char* const KEY_RESET{"reset"};
const char* const KEY_PREFS{"prefs"};

/** Whether a value is made only of plain values. */
bool Plain(const UniValue& value, int depth = 0)
{
    if (depth > 8) return false;
    if (value.isArray() || value.isObject()) {
        for (const UniValue& v : value.getValues()) {
            if (!Plain(v, depth + 1)) return false;
        }
    }
    return true;
}

UniValue ObjectWithout(const UniValue& object, const std::string& key)
{
    UniValue out{UniValue::VOBJ};
    if (!object.isObject()) return out;
    for (size_t i{0}; i < object.size(); ++i) {
        if (object.getKeys()[i] != key) out.pushKV(object.getKeys()[i], object.getValues()[i]);
    }
    return out;
}

UniValue ObjectWith(const UniValue& object, const std::string& key, UniValue value)
{
    UniValue out{ObjectWithout(object, key)};
    out.pushKV(key, std::move(value));
    return out;
}
} // namespace

namespace {
//! <stem>-app.json
fs::path FileName() { return fs::u8path(std::string{chain::APP} + ".json"); }
} // namespace

AppConfig::AppConfig(fs::path path) : m_path(std::move(path)) {}

fs::path AppConfig::DefaultPath()
{
#ifdef WIN32
    // In the local folder, as the node's default data directory and WebView2's profile: a file in
    // the roaming one would create %APPDATA%\<name>, which older nodes took for a data directory.
    const fs::path path{GetSpecialFolderPath(CSIDL_LOCAL_APPDATA) / CLIENT_NAME / FileName()};
    MoveLegacy(GetSpecialFolderPath(CSIDL_APPDATA) / CLIENT_NAME / FileName(), path);
    return path;
#else
    fs::path home;
    if (const char* h{std::getenv("HOME")}; h && *h) home = fs::PathFromString(h);
    else home = fs::PathFromString("/");
#ifdef __APPLE__
    return home / "Library" / "Application Support" / CLIENT_NAME / FileName();
#else
    fs::path config;
    if (const char* xdg{std::getenv("XDG_CONFIG_HOME")}; xdg && *xdg) config = fs::PathFromString(xdg);
    else config = home / ".config";
    return config / CLIENT_NAME / FileName();
#endif
#endif
}

bool AppConfig::MoveLegacy(const fs::path& legacy, const fs::path& current)
{
    std::error_code ec;
    if (std::filesystem::exists(current, ec) || !std::filesystem::is_regular_file(legacy, ec)) return false;
    std::ifstream in{static_cast<const std::filesystem::path&>(legacy), std::ios::binary};
    if (!in.is_open()) return false;
    std::stringstream text;
    text << in.rdbuf();
    in.close();
    fs::create_directories(current.parent_path(), ec);
    fs::path temp{current};
    temp += ".new";
    {
        std::ofstream out{static_cast<const std::filesystem::path&>(temp), std::ios::binary | std::ios::trunc};
        if (!out.is_open()) return false;
        out << text.str();
        out.flush();
        if (!out.good()) {
            out.close();
            fs::remove(temp, ec);
            return false;
        }
    }
    if (!RenameOver(temp, current)) {
        fs::remove(temp, ec);
        return false;
    }
    fs::remove(legacy, ec);
    // Only an empty folder: fs::remove does not remove a folder that holds anything.
    if (std::filesystem::is_directory(legacy.parent_path(), ec) && std::filesystem::is_empty(legacy.parent_path(), ec)) {
        fs::remove(legacy.parent_path(), ec);
    }
    LogInfo("webui: moved the app settings from %s to %s", fs::PathToString(legacy), fs::PathToString(current));
    return true;
}

void AppConfig::Load()
{
    LOCK(m_mutex);
    m_root = UniValue{UniValue::VOBJ};
    std::ifstream file{static_cast<const std::filesystem::path&>(m_path)};
    if (!file.is_open()) return;
    std::stringstream text;
    text << file.rdbuf();
    UniValue root;
    if (!root.read(text.str()) || !root.isObject()) {
        LogWarning("webui: the app settings in %s cannot be read; starting from defaults", fs::PathToString(m_path));
        return;
    }
    m_root = std::move(root);
}

void AppConfig::Save() const
{
    try {
        fs::create_directories(m_path.parent_path());
    } catch (const fs::filesystem_error& e) {
        LogWarning("webui: cannot create %s: %s", fs::PathToString(m_path.parent_path()), e.what());
        return;
    }
    fs::path temp{m_path};
    temp += ".new";
    {
        std::ofstream file{static_cast<const std::filesystem::path&>(temp), std::ios::trunc};
        if (!file.is_open()) {
            LogWarning("webui: cannot write the app settings to %s", fs::PathToString(temp));
            return;
        }
        file << m_root.write(2) << "\n";
        if (!file.good()) {
            LogWarning("webui: cannot write the app settings to %s", fs::PathToString(temp));
            return;
        }
    }
    if (!RenameOver(temp, m_path)) {
        LogWarning("webui: cannot replace %s", fs::PathToString(m_path));
    }
}

std::optional<std::string> AppConfig::DataDir() const
{
    LOCK(m_mutex);
    const UniValue& dir{m_root.find_value(KEY_DATADIR)};
    if (!dir.isStr() || dir.get_str().empty()) return std::nullopt;
    return dir.get_str();
}

void AppConfig::SetDataDir(const std::string& dir)
{
    LOCK(m_mutex);
    m_root = ObjectWith(m_root, KEY_DATADIR, UniValue{dir});
    Save();
}

bool AppConfig::ResetPending() const
{
    LOCK(m_mutex);
    const UniValue& reset{m_root.find_value(KEY_RESET)};
    return reset.isBool() && reset.get_bool();
}

void AppConfig::SetResetPending(bool reset)
{
    LOCK(m_mutex);
    m_root = reset ? ObjectWith(m_root, KEY_RESET, UniValue{true}) : ObjectWithout(m_root, KEY_RESET);
    Save();
}

UniValue AppConfig::Prefs(const std::string& network) const
{
    LOCK(m_mutex);
    const UniValue& prefs{m_root.find_value(KEY_PREFS).find_value(network)};
    return prefs.isObject() ? prefs : UniValue{UniValue::VOBJ};
}

UniValue AppConfig::UpdatePrefs(const std::string& network, const UniValue& changes)
{
    LOCK(m_mutex);
    const UniValue& all{m_root.find_value(KEY_PREFS)};
    UniValue prefs{all.find_value(network).isObject() ? all.find_value(network) : UniValue{UniValue::VOBJ}};
    if (changes.isObject()) {
        for (size_t i{0}; i < changes.size(); ++i) {
            const std::string& key{changes.getKeys()[i]};
            const UniValue& value{changes.getValues()[i]};
            if (key.empty() || !Plain(value)) continue;
            prefs = value.isNull() ? ObjectWithout(prefs, key) : ObjectWith(prefs, key, value);
        }
    }
    if (prefs.write().size() > MAX_PREFS_SIZE) {
        throw std::runtime_error("The preferences are too large");
    }
    m_root = ObjectWith(m_root, KEY_PREFS, ObjectWith(all, network, prefs));
    Save();
    return prefs;
}

UniValue AppConfig::Value(const std::string& key) const
{
    LOCK(m_mutex);
    if (key == KEY_DATADIR || key == KEY_RESET || key == KEY_PREFS) return {};
    return m_root.find_value(key);
}

void AppConfig::SetValue(const std::string& key, const UniValue& value)
{
    if (key.empty() || key == KEY_DATADIR || key == KEY_RESET || key == KEY_PREFS) throw std::runtime_error("This key is not for values of the app");
    LOCK(m_mutex);
    m_root = value.isNull() ? ObjectWithout(m_root, key) : ObjectWith(m_root, key, value);
    Save();
}

void AppConfig::ResetPrefs(const std::string& network)
{
    LOCK(m_mutex);
    m_root = ObjectWith(m_root, KEY_PREFS, ObjectWithout(m_root.find_value(KEY_PREFS), network));
    Save();
}

} // namespace webui
