// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_I18N_H
#define BITCOIN_WEBUI_I18N_H

#include <univalue.h>

#include <optional>
#include <span>
#include <string>

namespace webui {

struct Asset;

/**
 * The languages of chains-app: the catalogs made from the ones of chains-qt (src/webui/app/scripts/i18n.mjs),
 * built into the program as /locales/<code>.json. A catalog has the messages of the interface ("gui",
 * which the page translates) and of the node ("core", which G_TRANSLATION_FUN translates here).
 */

/** The locale of the system, as chains-qt takes it (QLocale::system().name()): "de_DE", or "". */
std::string SystemLocale();

/**
 * The language to use, as BitcoinApplication::initTranslations chooses it: -lang (the command line, or the
 * language option kept in settings.json), else the locale of the system; the catalog of the language and
 * territory ("pt_BR") if there is one, else of the language ("pt"). "" for English (no catalog).
 */
std::string ChooseLanguage(std::span<const Asset> assets, const std::string& wanted);

/** The catalog of a language (the parsed JSON), or nothing. */
std::optional<UniValue> LoadCatalog(std::span<const Asset> assets, const std::string& code);

/** The languages there are catalogs for: [{code, name, rtl, messages}]. */
UniValue Languages(std::span<const Asset> assets);

/** Translate the messages of the node with this catalog from now on (null: English). Thread-safe. */
void SetCoreCatalog(std::optional<UniValue> catalog);

/** A message of the node in the language chosen, or as it is. What G_TRANSLATION_FUN of chains-app does. */
std::string TranslateCore(const char* message);

/** A message as the catalogs key it: "…" for "...", "Chains" for "Bitcoin". */
std::string NormalizeMessage(const std::string& message);

} // namespace webui

#endif // BITCOIN_WEBUI_I18N_H
