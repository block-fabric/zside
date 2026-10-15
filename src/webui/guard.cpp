// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/guard.h>

#include <webui/chainconfig.h>

#include <tinyformat.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace webui::guard {

/** The next code point of UTF-8 text at `pos` (moved past it); nothing for an invalid sequence (pos moves one byte). */
std::optional<uint32_t> NextCodePoint(std::string_view text, size_t& pos)
{
    const auto byte{[&](size_t i) { return static_cast<unsigned char>(text[i]); }};
    const unsigned char lead{byte(pos)};
    size_t len;
    uint32_t cp;
    if (lead < 0x80) {
        ++pos;
        return lead;
    } else if ((lead & 0xE0) == 0xC0) {
        len = 2;
        cp = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        len = 3;
        cp = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        len = 4;
        cp = lead & 0x07;
    } else {
        ++pos;
        return std::nullopt;
    }
    if (pos + len > text.size()) {
        ++pos;
        return std::nullopt;
    }
    for (size_t i{1}; i < len; ++i) {
        if ((byte(pos + i) & 0xC0) != 0x80) {
            ++pos;
            return std::nullopt;
        }
        cp = (cp << 6) | (byte(pos + i) & 0x3F);
    }
    // Overlong forms, surrogates, and what is past the last code point are not UTF-8.
    if ((len == 2 && cp < 0x80) || (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000) || (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
        ++pos;
        return std::nullopt;
    }
    pos += len;
    return cp;
}

/** A character that is not shown as itself on one line: controls, line and paragraph separators, marks of direction, invisible ones. */
bool IsControlOrFormat(uint32_t cp)
{
    return cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0xAD || cp == 0x061C || cp == 0x180E ||
           (cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2028 && cp <= 0x202E) || (cp >= 0x2060 && cp <= 0x206F) ||
           cp == 0xFEFF || (cp >= 0xFFF9 && cp <= 0xFFFB) || (cp >= 0xE0000 && cp <= 0xE007F);
}

/** A space of another width (no-break, em, ideographic...): shown as a plain one. */
bool IsOtherSpace(uint32_t cp)
{
    return cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

void AppendUtf8(std::string& out, uint32_t cp)
{
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

std::string OneLine(std::string_view text, size_t max_chars)
{
    std::string out;
    size_t pos{0}, chars{0};
    bool space{true}; // no space at the start
    bool cut{false};
    while (pos < text.size()) {
        const auto cp{NextCodePoint(text, pos)};
        const uint32_t c{cp ? *cp : 0xFFFD};
        if (c == ' ' || IsControlOrFormat(c) || IsOtherSpace(c)) {
            if (!space) {
                out += ' ';
                ++chars;
                space = true;
            }
        } else {
            if (chars >= max_chars) {
                cut = true;
                break;
            }
            AppendUtf8(out, c);
            ++chars;
            space = false;
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (cut && max_chars > 0) {
        // Room for the ellipsis within the limit.
        size_t count{0}, keep{0};
        for (size_t i{0}; i < out.size(); ++i) {
            if ((static_cast<unsigned char>(out[i]) & 0xC0) != 0x80) {
                if (count == max_chars - 1) break;
                ++count;
            }
            keep = i + 1;
        }
        out.resize(keep);
        while (!out.empty() && out.back() == ' ') out.pop_back();
        out += "\xe2\x80\xa6";
    }
    return out;
}

std::optional<std::string> WebHost(std::string_view url, bool https_only)
{
    std::string_view scheme{"https://"};
    if (!url.starts_with(scheme)) {
        if (https_only || !url.starts_with("http://")) return std::nullopt;
        scheme = "http://";
    }
    if (url.size() > 2048) return std::nullopt;
    for (const char c : url) {
        // Printable ASCII, no space, no quotes, no backslash (a host written with them is read differently by programs).
        if (c <= 0x20 || c >= 0x7F || c == '"' || c == '\'' || c == '\\') return std::nullopt;
    }
    const size_t end{url.find_first_of("/?#", scheme.size())};
    std::string_view authority{url.substr(scheme.size(), end == std::string_view::npos ? std::string_view::npos : end - scheme.size())};
    // https://github.com@evil.example/ goes to evil.example: an address with a user name is refused.
    if (authority.find('@') != std::string_view::npos) return std::nullopt;
    std::string_view host{authority};
    if (const size_t colon{authority.rfind(':')}; colon != std::string_view::npos) {
        const std::string_view port{authority.substr(colon + 1)};
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string_view::npos) return std::nullopt;
        host = authority.substr(0, colon);
    }
    if (host.empty() || host.size() > 253 || host.front() == '.' || host.front() == '-' || host.back() == '.' || host.back() == '-' ||
        host.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-") != std::string_view::npos ||
        host.find("..") != std::string_view::npos) {
        return std::nullopt;
    }
    return ToLower(host);
}

namespace {
//! Read-only families of commands: they show the chain, the node or the wallet, and change nothing that matters.
constexpr std::array QUIET_PREFIXES{std::string_view{"get"}, std::string_view{"list"}, std::string_view{"decode"}, std::string_view{"estimate"}, std::string_view{"verify"}};
//! Of those, the ones that can show private keys (an argument asks for them): always confirmed.
constexpr std::array KEY_READERS{std::string_view{"gethdkeys"}, std::string_view{"listdescriptors"}};
//! Other commands that only read, or build or decode data without a key.
constexpr std::array QUIET{
    std::string_view{"help"}, std::string_view{"uptime"}, std::string_view{"echo"}, std::string_view{"echojson"}, std::string_view{"ping"},
    std::string_view{"validateaddress"}, std::string_view{"analyzepsbt"}, std::string_view{"deriveaddresses"}, std::string_view{"combinepsbt"},
    std::string_view{"createpsbt"}, std::string_view{"createrawtransaction"}, std::string_view{"converttopsbt"}, std::string_view{"joinpsbts"},
    std::string_view{"finalizepsbt"}, std::string_view{"utxoupdatepsbt"}, std::string_view{"createmultisig"},
};

bool IsCommandName(std::string_view method)
{
    return !method.empty() && method.size() <= 64 && method.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_") == std::string_view::npos;
}
} // namespace

bool ConsoleRunsQuietly(std::string_view method)
{
    if (!IsCommandName(method)) return false;
    if (std::ranges::find(KEY_READERS, method) != KEY_READERS.end()) return false;
    if (std::ranges::find(QUIET, method) != QUIET.end()) return true;
    return std::ranges::any_of(QUIET_PREFIXES, [&](std::string_view prefix) { return method.starts_with(prefix); });
}

std::optional<Window::ConfirmDialog> ConsoleConfirmation(std::string_view method, std::string_view wallet)
{
    // A command is named in lower case letters, digits and '_' (the dispatcher knows no other): anything else
    // is not shown in a dialog of the desktop.
    if (!IsCommandName(method)) return std::nullopt;
    Window::ConfirmDialog dialog;
    dialog.title = "Run a command of the console";
    dialog.message = strprintf("Command: %s\n\n"
                               "The console asks to run this command%s. It can spend coins, sign, show private keys, or change "
                               "the wallet or the node.\n\nRun it only if you typed it yourself.",
                               method, wallet.empty() ? std::string{} : strprintf(" on the wallet \"%s\"", OneLine(wallet, 64)));
    dialog.accept = "Run";
    return dialog;
}

const std::vector<std::string>& KnownLinks()
{
    // The help links of chain.json (webui/chainconfig.h).
    static const std::vector<std::string> links{chain::LINKS.begin(), chain::LINKS.end()};
    return links;
}

std::optional<Window::ConfirmDialog> OpenUrlConfirmation(std::string_view url)
{
    const auto host{WebHost(url, /*https_only=*/false)};
    if (!host) throw std::invalid_argument{"Only web addresses (http, https) with a plain host are opened"};
    const auto& known{KnownLinks()};
    if (std::ranges::find(known, url) != known.end()) return std::nullopt;
    Window::ConfirmDialog dialog;
    dialog.title = "Open a web address";
    dialog.message = strprintf("Host: %s\n\nThe app asks to open this address in your web browser%s:\n\n%s",
                               *host, url.starts_with("http://") ? " (not encrypted: http)" : "", OneLine(url, 300));
    dialog.accept = "Open";
    return dialog;
}

} // namespace webui::guard
