// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_ASSETS_H
#define BITCOIN_WEBUI_ASSETS_H

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace webui {

/** A file of the interface, built into the program. */
struct Asset {
    //! Path from the root of the interface, starting with "/": "/index.html", "/assets/index-1a2b3c.js".
    std::string_view path;
    std::span<const unsigned char> data;
};

/** The files of the interface (generated at build time from the output of the frontend build). */
std::span<const Asset> BuiltinAssets();

/** The files of a set of assets that a request path names, or nothing. "/" is "/index.html"; a query or fragment is ignored. */
const Asset* FindAsset(std::span<const Asset> assets, std::string_view request_path);

/** The media type of a file, from its extension. */
std::string_view MimeType(std::string_view path);

/** The scheme the interface is served from where the platform lets the app serve one: chains-app://app/ */
inline constexpr std::string_view SCHEME{"chains-app"};
inline constexpr std::string_view ORIGIN{"chains-app://app"};

/**
 * The content security policy of the interface: nothing but its own files,
 * no remote content, no eval, no plug-ins, no frames.
 */
std::string ContentSecurityPolicy();

/**
 * One self-contained HTML document with every script, style sheet and font of
 * the interface inside it, for the platforms where the app cannot serve files
 * under a scheme of its own: the script and the style sheet inline (allowed by
 * their hashes in the policy, not by 'unsafe-inline'), fonts and images as
 * data: URIs. Nothing if the index or a file it names is missing.
 */
std::optional<std::string> InlineDocument(std::span<const Asset> assets);

} // namespace webui

#endif // BITCOIN_WEBUI_ASSETS_H
