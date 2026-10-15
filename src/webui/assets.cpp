// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/assets.h>

#include <crypto/sha256.h>
#include <util/strencodings.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

namespace webui {
namespace {
std::string_view Text(const Asset& asset)
{
    return {reinterpret_cast<const char*>(asset.data.data()), asset.data.size()};
}

std::string Sha256Base64(std::string_view text)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(text.data()), text.size()).Finalize(hash.data());
    return EncodeBase64(hash);
}

std::string DataUri(const Asset& asset)
{
    return "data:" + std::string{MimeType(asset.path)} + ";base64," + EncodeBase64(asset.data);
}

/** The value of attribute `name` in the tag `tag` ("<script ... src="/x.js">"). */
std::optional<std::string> Attribute(std::string_view tag, std::string_view name)
{
    const std::string key{" " + std::string{name} + "=\""};
    const size_t start{tag.find(key)};
    if (start == std::string_view::npos) return std::nullopt;
    const size_t value_start{start + key.size()};
    const size_t end{tag.find('"', value_start)};
    if (end == std::string_view::npos) return std::nullopt;
    return std::string{tag.substr(value_start, end - value_start)};
}

/** Replace every url(...) of a style sheet that names a file of the assets with its data: URI. */
std::optional<std::string> InlineUrls(std::span<const Asset> assets, std::string_view css)
{
    std::string out;
    size_t pos{0};
    for (;;) {
        const size_t start{css.find("url(", pos)};
        if (start == std::string_view::npos) break;
        const size_t end{css.find(')', start)};
        if (end == std::string_view::npos) break;
        std::string_view target{css.substr(start + 4, end - start - 4)};
        if (!target.empty() && (target.front() == '"' || target.front() == '\'')) target = target.substr(1, target.size() >= 2 ? target.size() - 2 : 0);
        out.append(css.substr(pos, start - pos));
        if (target.starts_with("data:")) {
            out.append(css.substr(start, end - start + 1));
        } else {
            const Asset* asset{FindAsset(assets, target.starts_with("./") ? target.substr(1) : target)};
            if (!asset) return std::nullopt;
            out += "url(" + DataUri(*asset) + ")";
        }
        pos = end + 1;
    }
    out.append(css.substr(pos));
    return out;
}
} // namespace

const Asset* FindAsset(std::span<const Asset> assets, std::string_view request_path)
{
    std::string_view path{request_path};
    if (const size_t cut{path.find_first_of("?#")}; cut != std::string_view::npos) path = path.substr(0, cut);
    if (path.empty() || path == "/") path = "/index.html";
    for (const Asset& asset : assets) {
        if (asset.path == path) return &asset;
    }
    return nullptr;
}

std::string_view MimeType(std::string_view path)
{
    static constexpr std::array<std::pair<std::string_view, std::string_view>, 13> TYPES{{
        {".html", "text/html; charset=utf-8"},
        {".js", "text/javascript; charset=utf-8"},
        {".mjs", "text/javascript; charset=utf-8"},
        {".css", "text/css; charset=utf-8"},
        {".json", "application/json"},
        {".svg", "image/svg+xml"},
        {".png", "image/png"},
        {".ico", "image/x-icon"},
        {".woff2", "font/woff2"},
        {".woff", "font/woff"},
        {".ttf", "font/ttf"},
        {".txt", "text/plain; charset=utf-8"},
        {".webp", "image/webp"},
    }};
    for (const auto& [ext, type] : TYPES) {
        if (path.ends_with(ext)) return type;
    }
    return "application/octet-stream";
}

std::string ContentSecurityPolicy()
{
    return "default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self' data:; font-src 'self'; "
           "connect-src 'none'; media-src 'none'; object-src 'none'; frame-src 'none'; worker-src 'none'; "
           "base-uri 'none'; form-action 'none'";
}

std::optional<std::string> InlineDocument(std::span<const Asset> assets)
{
    const Asset* index{FindAsset(assets, "/index.html")};
    if (!index) return std::nullopt;
    const std::string_view html{Text(*index)};

    std::vector<std::string> script_hashes, style_hashes;
    std::string out;
    size_t pos{0};
    for (;;) {
        const size_t start{html.find('<', pos)};
        if (start == std::string_view::npos) break;
        const size_t end{html.find('>', start)};
        if (end == std::string_view::npos) break;
        const std::string_view tag{html.substr(start, end - start + 1)};
        out.append(html.substr(pos, start - pos));
        pos = end + 1;

        if (tag.starts_with("<script")) {
            const auto src{Attribute(tag, "src")};
            if (!src) {
                out.append(tag);
                continue;
            }
            const Asset* script{FindAsset(assets, *src)};
            if (!script) return std::nullopt;
            // Skip the closing tag of the external script.
            if (html.substr(pos).starts_with("</script>")) pos += std::string_view{"</script>"}.size();
            std::string code{Text(*script)};
            // The code stays code: a "</script" inside a string would end the element.
            for (size_t at{code.find("</script")}; at != std::string::npos; at = code.find("</script", at + 2)) code.replace(at, 2, "<\\/");
            script_hashes.push_back(Sha256Base64(code));
            out += "<script type=\"module\">" + code + "</script>";
        } else if (tag.starts_with("<link") && Attribute(tag, "rel") == "stylesheet") {
            const auto href{Attribute(tag, "href")};
            const Asset* sheet{href ? FindAsset(assets, *href) : nullptr};
            if (!sheet) return std::nullopt;
            const auto css{InlineUrls(assets, Text(*sheet))};
            if (!css) return std::nullopt;
            style_hashes.push_back(Sha256Base64(*css));
            out += "<style>" + *css + "</style>";
        } else if (tag.starts_with("<link") && (Attribute(tag, "rel") == "modulepreload" || Attribute(tag, "rel") == "icon")) {
            // Everything is in the document already.
        } else if (tag.starts_with("<meta") && Attribute(tag, "http-equiv") == "Content-Security-Policy") {
            // Replaced by the policy of the inline document, below.
        } else {
            out.append(tag);
        }
    }
    out.append(html.substr(pos));

    std::string policy{"default-src 'none'; script-src"};
    for (const auto& h : script_hashes) policy += " 'sha256-" + h + "'";
    policy += "; style-src";
    for (const auto& h : style_hashes) policy += " 'sha256-" + h + "'";
    policy += "; img-src data:; font-src data:; connect-src 'none'; media-src 'none'; object-src 'none'; frame-src 'none'; "
              "worker-src 'none'; base-uri 'none'; form-action 'none'";
    const std::string meta{"<meta http-equiv=\"Content-Security-Policy\" content=\"" + policy + "\">"};
    const size_t head{out.find("<head>")};
    if (head == std::string::npos) return std::nullopt;
    out.insert(head + 6, meta);
    return out;
}

} // namespace webui
