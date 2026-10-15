// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <crypto/sha256.h>
#include <util/strencodings.h>
#include <webui/assets.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <string>
#include <string_view>

namespace {
std::span<const unsigned char> Bytes(std::string_view text)
{
    return {reinterpret_cast<const unsigned char*>(text.data()), text.size()};
}

std::string Hash(std::string_view text)
{
    std::array<unsigned char, CSHA256::OUTPUT_SIZE> hash;
    CSHA256().Write(reinterpret_cast<const unsigned char*>(text.data()), text.size()).Finalize(hash.data());
    return EncodeBase64(hash);
}

const std::string_view INDEX{
    "<!doctype html><html><head><meta charset=\"utf-8\">"
    "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'self'\">"
    "<link rel=\"icon\" href=\"/icon.svg\">"
    "<script type=\"module\" crossorigin src=\"/assets/index-abc.js\"></script>"
    "<link rel=\"modulepreload\" href=\"/assets/x.js\">"
    "<link rel=\"stylesheet\" crossorigin href=\"/assets/index-def.css\">"
    "</head><body><div id=\"app\"></div></body></html>"};
const std::string_view SCRIPT{"console.log('</script><b>');"};
const std::string_view STYLE{"@font-face{src:url(/assets/font.woff2) format('woff2')}body{background:url(\"data:image/png;base64,AA==\")}"};
const std::string_view FONT{"\x01\x02\x03"};

const webui::Asset FAKE[]{
    {"/index.html", Bytes(INDEX)},
    {"/assets/index-abc.js", Bytes(SCRIPT)},
    {"/assets/index-def.css", Bytes(STYLE)},
    {"/assets/font.woff2", Bytes(FONT)},
};
} // namespace

BOOST_AUTO_TEST_SUITE(webui_assets_tests)

BOOST_AUTO_TEST_CASE(find)
{
    BOOST_CHECK_EQUAL(webui::FindAsset(FAKE, "/")->path, "/index.html");
    BOOST_CHECK_EQUAL(webui::FindAsset(FAKE, "")->path, "/index.html");
    BOOST_CHECK_EQUAL(webui::FindAsset(FAKE, "/index.html?x=1#y")->path, "/index.html");
    BOOST_CHECK_EQUAL(webui::FindAsset(FAKE, "/assets/font.woff2")->path, "/assets/font.woff2");
    BOOST_CHECK(webui::FindAsset(FAKE, "/assets/missing.js") == nullptr);
    BOOST_CHECK(webui::FindAsset(FAKE, "/../etc/passwd") == nullptr);
    BOOST_CHECK(webui::FindAsset(FAKE, "assets/font.woff2") == nullptr);
}

BOOST_AUTO_TEST_CASE(mime_types)
{
    BOOST_CHECK_EQUAL(webui::MimeType("/index.html"), "text/html; charset=utf-8");
    BOOST_CHECK_EQUAL(webui::MimeType("/a.js"), "text/javascript; charset=utf-8");
    BOOST_CHECK_EQUAL(webui::MimeType("/a.css"), "text/css; charset=utf-8");
    BOOST_CHECK_EQUAL(webui::MimeType("/a.woff2"), "font/woff2");
    BOOST_CHECK_EQUAL(webui::MimeType("/a.svg"), "image/svg+xml");
    BOOST_CHECK_EQUAL(webui::MimeType("/a.unknown"), "application/octet-stream");
}

BOOST_AUTO_TEST_CASE(policy)
{
    const std::string csp{webui::ContentSecurityPolicy()};
    BOOST_CHECK(csp.find("default-src 'none'") != std::string::npos);
    BOOST_CHECK(csp.find("script-src 'self'") != std::string::npos);
    BOOST_CHECK(csp.find("connect-src 'none'") != std::string::npos);
    BOOST_CHECK(csp.find("unsafe-inline") == std::string::npos);
    BOOST_CHECK(csp.find("unsafe-eval") == std::string::npos);
    BOOST_CHECK(csp.find("http") == std::string::npos);
}

BOOST_AUTO_TEST_CASE(inline_document)
{
    const auto doc{webui::InlineDocument(FAKE)};
    BOOST_REQUIRE(doc);
    // The script is in the document, made safe for the element, and allowed by its hash.
    const std::string script{"console.log('<\\/script><b>');"};
    BOOST_CHECK(doc->find("<script type=\"module\">" + script + "</script>") != std::string::npos);
    BOOST_CHECK(doc->find("'sha256-" + Hash(script) + "'") != std::string::npos);
    // The style sheet too, with the font as a data: URI; the data: URI that was there stays.
    const std::string font_uri{"url(data:font/woff2;base64," + EncodeBase64(Bytes(FONT)) + ")"};
    BOOST_CHECK(doc->find(font_uri) != std::string::npos);
    BOOST_CHECK(doc->find("url(\"data:image/png;base64,AA==\")") != std::string::npos);
    const size_t style_start{doc->find("<style>") + 7};
    const std::string style{doc->substr(style_start, doc->find("</style>") - style_start)};
    BOOST_CHECK(doc->find("'sha256-" + Hash(style) + "'") != std::string::npos);
    // Nothing is loaded from elsewhere, and the old policy is replaced.
    BOOST_CHECK(doc->find("src=\"/") == std::string::npos);
    BOOST_CHECK(doc->find("href=\"/") == std::string::npos);
    BOOST_CHECK(doc->find("default-src 'self'") == std::string::npos);
    BOOST_CHECK(doc->find("unsafe-inline") == std::string::npos);
    BOOST_CHECK(doc->find("<div id=\"app\"></div>") != std::string::npos);
}

BOOST_AUTO_TEST_CASE(inline_document_missing_file)
{
    const webui::Asset broken[]{FAKE[0], FAKE[1]};
    BOOST_CHECK(!webui::InlineDocument(broken));
    BOOST_CHECK(!webui::InlineDocument(std::span<const webui::Asset>{}));
}

BOOST_AUTO_TEST_CASE(builtin_interface)
{
    // The interface of this build: an index, scripts and styles it names, all there.
    const auto assets{webui::BuiltinAssets()};
    const webui::Asset* index{webui::FindAsset(assets, "/")};
    BOOST_REQUIRE(index);
    const std::string_view html{reinterpret_cast<const char*>(index->data.data()), index->data.size()};
    BOOST_CHECK(html.find("Content-Security-Policy") != std::string_view::npos);
    BOOST_CHECK(html.find("http://") == std::string_view::npos);
    BOOST_CHECK(html.find("https://") == std::string_view::npos);
    for (const auto& asset : assets) {
        BOOST_CHECK(asset.path.starts_with("/"));
        BOOST_CHECK(asset.path.find("..") == std::string_view::npos);
        BOOST_CHECK(webui::MimeType(asset.path) != "application/octet-stream" || asset.path.ends_with(".map"));
        if (asset.path.ends_with(".js")) {
            const std::string_view code{reinterpret_cast<const char*>(asset.data.data()), asset.data.size()};
            // No code from elsewhere.
            BOOST_CHECK_MESSAGE(code.find("import(\"http") == std::string_view::npos, asset.path);
        }
    }
    BOOST_CHECK(webui::InlineDocument(assets));
}

BOOST_AUTO_TEST_SUITE_END()
