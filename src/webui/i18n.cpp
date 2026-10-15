// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/i18n.h>

#ifdef WIN32
#include <windows.h>
#endif

#include <sync.h>
#include <webui/assets.h>
#include <webui/chainconfig.h>

#include <cstdlib>
#include <map>
#include <memory>

namespace webui {
namespace {
Mutex g_core_mutex;
std::shared_ptr<const std::map<std::string, std::string>> g_core GUARDED_BY(g_core_mutex);

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t at{s.find(from)}; at != std::string::npos; at = s.find(from, at + to.size())) s.replace(at, from.size(), to);
}

bool HasCatalog(std::span<const Asset> assets, const std::string& code)
{
    return !code.empty() && FindAsset(assets, "/locales/" + code + ".json") != nullptr;
}
} // namespace

std::string NormalizeMessage(const std::string& message)
{
    std::string s{message};
    ReplaceAll(s, "...", "…");
    ReplaceAll(s, "Bitcoin Core", std::string{chain::NAME});
    ReplaceAll(s, "Bitcoin", std::string{chain::NAME});
    return s;
}

std::string SystemLocale()
{
    // As the C library and Qt read it on a Unix desktop: LC_ALL, LC_MESSAGES, then LANG ("de_DE.UTF-8@euro").
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value{std::getenv(var)};
        if (!value || !*value) continue;
        std::string locale{value};
        locale = locale.substr(0, locale.find_first_of(".@"));
        if (locale == "C" || locale == "POSIX") return "";
        return locale;
    }
#ifdef WIN32
    // Windows: the language of the user ("fr-FR"), as QLocale::system() reads it there.
    wchar_t name[LOCALE_NAME_MAX_LENGTH]{};
    if (const int n{GetUserDefaultLocaleName(name, LOCALE_NAME_MAX_LENGTH)}; n > 1) {
        std::string locale;
        for (int i{0}; i < n - 1 && name[i]; ++i) locale += name[i] < 128 ? static_cast<char>(name[i]) : '?';
        ReplaceAll(locale, "-", "_");
        return locale;
    }
#endif
    return "";
}

std::string ChooseLanguage(std::span<const Asset> assets, const std::string& wanted)
{
    std::string lang_territory{wanted.empty() ? SystemLocale() : wanted};
    ReplaceAll(lang_territory, "-", "_");
    if (lang_territory.empty() || lang_territory == "en" || lang_territory.starts_with("en_")) return "";
    if (HasCatalog(assets, lang_territory)) return lang_territory;
    const std::string lang{lang_territory.substr(0, lang_territory.find('_'))};
    if (HasCatalog(assets, lang)) return lang;
    return "";
}

std::optional<UniValue> LoadCatalog(std::span<const Asset> assets, const std::string& code)
{
    if (code.empty()) return std::nullopt;
    const Asset* asset{FindAsset(assets, "/locales/" + code + ".json")};
    if (!asset) return std::nullopt;
    UniValue catalog;
    if (!catalog.read(std::string_view{reinterpret_cast<const char*>(asset->data.data()), asset->data.size()}) || !catalog.isObject()) return std::nullopt;
    return catalog;
}

UniValue Languages(std::span<const Asset> assets)
{
    const Asset* asset{FindAsset(assets, "/locales/index.json")};
    UniValue list;
    if (!asset || !list.read(std::string_view{reinterpret_cast<const char*>(asset->data.data()), asset->data.size()}) || !list.isArray()) return UniValue{UniValue::VARR};
    return list;
}

void SetCoreCatalog(std::optional<UniValue> catalog)
{
    std::shared_ptr<std::map<std::string, std::string>> core;
    if (catalog && catalog->find_value("core").isObject()) {
        core = std::make_shared<std::map<std::string, std::string>>();
        const UniValue& messages{catalog->find_value("core")};
        for (size_t i{0}; i < messages.size(); ++i) {
            // Plural forms are not used by the node: the first form stands.
            const UniValue& value{messages.getValues()[i]};
            if (value.isStr()) core->emplace(messages.getKeys()[i], value.get_str());
            else if (value.isArray() && !value.empty() && value[0].isStr()) core->emplace(messages.getKeys()[i], value[0].get_str());
        }
    }
    LOCK(g_core_mutex);
    g_core = std::move(core);
}

std::string TranslateCore(const char* message)
{
    std::shared_ptr<const std::map<std::string, std::string>> core;
    {
        LOCK(g_core_mutex);
        core = g_core;
    }
    if (!core) return message;
    const auto it{core->find(NormalizeMessage(message))};
    return it == core->end() ? std::string{message} : it->second;
}

} // namespace webui
