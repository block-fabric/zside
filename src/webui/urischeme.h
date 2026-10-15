// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_URISCHEME_H
#define BITCOIN_WEBUI_URISCHEME_H

#include <string>

namespace webui {

/**
 * Windows: open the payment URIs of a scheme ("chains") with this program, for this user
 * (HKCU\Software\Classes\<scheme>, what the installer of chains-qt writes for all users). Left
 * alone when it already opens with a program that still exists (another installation the user
 * chose). Whether the scheme opens with this program now.
 */
bool RegisterUriScheme(const std::string& scheme, const std::string& description);

} // namespace webui

#endif // BITCOIN_WEBUI_URISCHEME_H
