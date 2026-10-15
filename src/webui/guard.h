// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_GUARD_H
#define BITCOIN_WEBUI_GUARD_H

#include <webui/window.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/**
 * What the host checks itself before it does what the page asks, when the page alone must not be
 * enough: the confirmations of the desktop (Window::Confirm, a dialog the page can neither draw nor
 * answer) and the text put in them.
 *
 * The page is trusted to draw the app, not to act for the user where it matters: a command of the
 * console that can spend, sign, show keys or change the node, and a web address that is not one of the
 * app's own, wait for the user's word given to the desktop.
 */
namespace webui::guard {

//! Asks the user in a dialog of the desktop; true when accepted (App::Confirm).
using ConfirmFn = std::function<bool(const Window::ConfirmDialog&)>;

/** The next code point of UTF-8 text at `pos` (moved past it); nothing for an invalid sequence (pos moves one byte). */
std::optional<uint32_t> NextCodePoint(std::string_view text, size_t& pos);
/** A character that is not shown as itself on one line: controls, line and paragraph separators, marks of direction, invisible ones. */
bool IsControlOrFormat(uint32_t cp);
/** A space of another width (no-break, em, ideographic...): shown as a plain one. */
bool IsOtherSpace(uint32_t cp);
void AppendUtf8(std::string& out, uint32_t cp);

/**
 * Text of the page or the chain as a dialog of the desktop shows it: one line, every control or format
 * character (a new line, a mark that turns text around) a space, spaces folded, invalid UTF-8 U+FFFD,
 * cut to `max_chars` characters with "…".
 */
std::string OneLine(std::string_view text, size_t max_chars);

/**
 * The host of a web address, in lower case; nothing when the address is not taken: not https (or http
 * when `https_only` is false), too long, not plain printable ASCII, with a user name or password before
 * the host (https://a@b/), with a host that is not letters, digits, '.' and '-', or a port that is not a
 * number.
 */
std::optional<std::string> WebHost(std::string_view url, bool https_only);

/**
 * Whether a command of the console runs without asking: one that only reads, and shows no key
 * (get..., list..., decode..., estimate..., verify..., help, and the ones that only build or decode
 * data). Every other one (send, sign, import, dump, backup, stop, addnode, a command of the chain that
 * spends...) and the ones that can show private keys (listdescriptors, gethdkeys) are confirmed first.
 */
bool ConsoleRunsQuietly(std::string_view method);

/** The confirmation of a command of the console: the name of the command first, in fixed words. Nothing for a name that is not one of a command. */
std::optional<Window::ConfirmDialog> ConsoleConfirmation(std::string_view method, std::string_view wallet);

/** The addresses the app opens without asking: its own links (none for Chains; a sidechain's are in its chain.json). */
const std::vector<std::string>& KnownLinks();

/**
 * What opening a web address takes: nothing for one of the app's own links, else the confirmation of the
 * desktop (its parsed host first, alone on its line). Throws std::invalid_argument for an address that is
 * not opened at all.
 */
std::optional<Window::ConfirmDialog> OpenUrlConfirmation(std::string_view url);

} // namespace webui::guard

#endif // BITCOIN_WEBUI_GUARD_H
