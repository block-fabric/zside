// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_APPCONFIG_H
#define BITCOIN_WEBUI_APPCONFIG_H

#include <sync.h>
#include <univalue.h>
#include <util/fs.h>

#include <optional>
#include <string>

namespace webui {

/**
 * The settings of the app itself, outside of any data directory, as the Qt GUI
 * keeps them with QSettings: the data directory chosen in the intro, and the
 * preferences of the interface (theme, unit, text size...), one set per network.
 *
 * Settings of the node stay where the node keeps them (settings.json in the
 * data directory), as with the Qt GUI.
 *
 * The file is JSON, written whole on every change (to a temporary file that
 * replaces the old one).
 */
class AppConfig
{
public:
    explicit AppConfig(fs::path path);

    /** Where the file is kept on this system: <config dir>/Chains/chains-app.json. */
    static fs::path DefaultPath();

    /** Read the file. A missing file is an empty configuration; an unreadable one too, with a warning. */
    void Load() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    const fs::path& Path() const { return m_path; }

    //! The data directory chosen in the intro (QSettings "strDataDir").
    std::optional<std::string> DataDir() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetDataDir(const std::string& dir) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    //! The settings were reset: show the intro again (QSettings "fReset").
    bool ResetPending() const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetResetPending(bool reset) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** The preferences of the interface on a network (an object; empty if none). */
    UniValue Prefs(const std::string& network) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Change preferences of a network: the keys of `changes` replace the ones
     * there, a null value removes its key. Only plain values (strings,
     * numbers, booleans) and arrays/objects of them are kept.
     * @return the preferences afterwards
     */
    UniValue UpdatePrefs(const std::string& network, const UniValue& changes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * A value the app keeps for itself, apart from the preferences the page sets: the geometry
     * of the window, where the programs of sidechains are. Null if none.
     */
    UniValue Value(const std::string& key) const EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Keep a value of the app (null removes it). The keys of the file itself are refused. */
    void SetValue(const std::string& key, const UniValue& value) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Forget the preferences of a network (-resetguisettings). */
    void ResetPrefs(const std::string& network) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Most bytes of preferences a network can keep. */
    static constexpr size_t MAX_PREFS_SIZE{256 * 1024};

private:
    void Save() const EXCLUSIVE_LOCKS_REQUIRED(m_mutex);

    const fs::path m_path;
    mutable Mutex m_mutex;
    UniValue m_root GUARDED_BY(m_mutex){UniValue::VOBJ};
};

} // namespace webui

#endif // BITCOIN_WEBUI_APPCONFIG_H
