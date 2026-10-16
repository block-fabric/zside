// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <webui/dialog.h>

#include <support/cleanse.h>
#include <tinyformat.h>
#include <webui/guard.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace webui::dialog {
namespace {

//! The longest texts a dialog shows (characters): more is cut with "…".
constexpr size_t MAX_TITLE{200};
constexpr size_t MAX_MESSAGE{6000};
constexpr size_t MAX_DETAILS{6000};
constexpr size_t MAX_BUTTON{60};
constexpr size_t MAX_LABEL{200};
//! The longest passphrase taken (bytes), as the page's own fields.
constexpr size_t MAX_PASSPHRASE{1024};
//! The largest theme of the user passed to the page (bytes of its JSON); the page checks it again.
constexpr size_t MAX_THEME_JSON{16 * 1024};

//! The built-in themes drawn dark (src/webui/app/src/lib/themes.ts).
constexpr std::array<std::string_view, 7> DARK_THEMES{"midnight", "dark", "ocean", "matrix", "qdarkstyle", "mocha", "dracula"};

bool IsDarkTheme(std::string_view id)
{
    return std::ranges::find(DARK_THEMES, id) != DARK_THEMES.end();
}

std::string Str(const UniValue& value, size_t max_bytes, const std::string& fallback = {})
{
    return value.isStr() && value.get_str().size() <= max_bytes ? value.get_str() : fallback;
}

/** The theme of the user a choice names, from the preferences ("customThemes"), if it is there. */
const UniValue* CustomTheme(const UniValue& prefs, const std::string& choice)
{
    if (!choice.starts_with("custom-")) return nullptr;
    const UniValue& list{prefs.find_value("customThemes")};
    if (!list.isArray()) return nullptr;
    for (const UniValue& theme : list.getValues()) {
        if (theme.isObject() && theme.find_value("id").isStr() && theme.find_value("id").get_str() == choice) return &theme;
    }
    return nullptr;
}

/** "#rgb" or "#rrggbb": whether it is dark (relative luminance under 0.2, as isDarkColor of the page); nothing for another form. */
std::optional<bool> HexIsDark(const std::string& color)
{
    std::string hex{color.starts_with('#') ? color.substr(1) : std::string{}};
    if (hex.size() == 3) hex = {hex[0], hex[0], hex[1], hex[1], hex[2], hex[2]};
    if (hex.size() != 6 || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return std::nullopt;
    double lum{0};
    constexpr std::array<double, 3> WEIGHTS{0.2126, 0.7152, 0.0722};
    for (size_t i{0}; i < 3; ++i) {
        const double s{std::stoi(hex.substr(i * 2, 2), nullptr, 16) / 255.0};
        lum += WEIGHTS[i] * (s <= 0.04045 ? s / 12.92 : std::pow((s + 0.055) / 1.055, 2.4));
    }
    return lum < 0.2;
}
} // namespace

std::string_view LevelName(Level level)
{
    switch (level) {
    case Level::INFO: return "info";
    case Level::WARNING: return "warning";
    case Level::DANGER: return "danger";
    }
    return "warning";
}

std::string Shown(std::string_view text, size_t max_chars)
{
    std::string out;
    size_t pos{0}, chars{0};
    while (pos < text.size()) {
        if (chars >= max_chars) {
            out += "…";
            break;
        }
        const auto cp{guard::NextCodePoint(text, pos)};
        uint32_t c{cp ? *cp : 0xFFFD};
        if (c == '\r') {
            // "\r\n" is one line break; a "\r" alone is one too.
            if (pos < text.size() && text[pos] == '\n') ++pos;
            c = '\n';
        }
        if (c == '\n') {
            out += '\n';
        } else if (c == '\t') {
            out += ' ';
        } else if (guard::IsControlOrFormat(c)) {
            out += strprintf("[U+%04X]", c);
        } else {
            guard::AppendUtf8(out, c);
        }
        ++chars;
    }
    return out;
}

UniValue PageData(const Request& request)
{
    UniValue out{UniValue::VOBJ};
    out.pushKV("title", guard::OneLine(request.title, MAX_TITLE));
    out.pushKV("message", Shown(request.message, MAX_MESSAGE));
    out.pushKV("details", Shown(request.details, MAX_DETAILS));
    out.pushKV("accept", guard::OneLine(request.accept.empty() ? std::string{"OK"} : request.accept, MAX_BUTTON));
    out.pushKV("cancel", guard::OneLine(request.cancel.empty() ? std::string{"Cancel"} : request.cancel, MAX_BUTTON));
    out.pushKV("level", std::string{LevelName(request.level)});
    out.pushKV("passphrase", request.passphrase);
    out.pushKV("passphraseLabel", guard::OneLine(request.passphrase_label, MAX_LABEL));
    out.pushKV("look", request.look.isObject() ? request.look : UniValue{UniValue::VOBJ});
    return out;
}

UniValue Look(const UniValue& prefs, bool rtl, const std::string& language)
{
    UniValue out{UniValue::VOBJ};
    const std::string theme{prefs.isObject() ? Str(prefs.find_value("theme"), 40, "midnight") : std::string{"midnight"}};
    out.pushKV("theme", theme);
    if (prefs.isObject()) {
        if (const UniValue* custom{CustomTheme(prefs, theme)}; custom && custom->write().size() <= MAX_THEME_JSON) {
            out.pushKV("custom", *custom);
        }
        const UniValue& adjust{prefs.find_value("fontAdjust")};
        if (adjust.isNum()) {
            // Clamped as the page clamps it (MIN_FONT_ADJUST, MAX_FONT_ADJUST).
            const double v{adjust.get_real()};
            out.pushKV("fontAdjust", std::isfinite(v) ? std::clamp<int64_t>(std::llround(v), -6, 12) : 0);
        }
        out.pushKV("fontFamily", Str(prefs.find_value("fontFamily"), 16));
        out.pushKV("moneyFont", Str(prefs.find_value("moneyFont"), 16, "embedded"));
    }
    out.pushKV("rtl", rtl);
    out.pushKV("lang", language.size() <= 16 ? language : std::string{});
    return out;
}

Scheme SchemeOf(const UniValue& prefs)
{
    const std::string theme{prefs.isObject() ? Str(prefs.find_value("theme"), 40, "midnight") : std::string{"midnight"}};
    if (theme == "system") return Scheme::DESKTOP;
    if (theme == "light") return Scheme::LIGHT;
    if (IsDarkTheme(theme)) return Scheme::DARK;
    const UniValue* custom{CustomTheme(prefs, theme)};
    // A look (Look) carries the theme of the user it names as "custom".
    if (!custom && prefs.isObject() && prefs.find_value("custom").isObject() && prefs.find_value("custom").find_value("id").isStr() &&
        prefs.find_value("custom").find_value("id").get_str() == theme) {
        custom = &prefs.find_value("custom");
    }
    if (custom) {
        const UniValue& colors{custom->find_value("colors")};
        if (colors.isObject()) {
            if (const auto dark{HexIsDark(Str(colors.find_value("bg"), 16))}) return *dark ? Scheme::DARK : Scheme::LIGHT;
        }
        const std::string base{Str(custom->find_value("base"), 40, "midnight")};
        return base == "light" ? Scheme::LIGHT : Scheme::DARK;
    }
    // What the page draws for a theme it does not know: the default, Midnight.
    return Scheme::DARK;
}

bool Serves(std::string_view path)
{
    if (const size_t cut{path.find_first_of("?#")}; cut != std::string_view::npos) path = path.substr(0, cut);
    if (path == PAGE) return true;
    constexpr std::string_view DIR{"/assets/"};
    if (!path.starts_with(DIR)) return false;
    const std::string_view name{path.substr(DIR.size())};
    if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string_view::npos) return false;
    // The page's own script and style sheet (vite.dialog.config.ts names them dialog-*), and the fonts it shares with the main page.
    if (name.starts_with("dialog-") && (name.ends_with(".js") || name.ends_with(".css"))) return true;
    return name.ends_with(".woff2");
}

std::string ContentSecurityPolicy()
{
    return "default-src 'none'; script-src 'self'; style-src 'self'; img-src data:; font-src 'self'; connect-src 'none'; "
           "media-src 'none'; object-src 'none'; frame-src 'none'; frame-ancestors 'none'; worker-src 'none'; "
           "base-uri 'none'; form-action 'none'";
}

Session::Session(Request request) : m_request(std::move(request)) {}

Session::~Session()
{
    if (!m_answer.passphrase.empty()) memory_cleanse(m_answer.passphrase.data(), m_answer.passphrase.size());
}

Session::Reply Session::Call(const std::string& args_json)
{
    const auto error{[](const std::string& message) {
        Reply reply;
        reply.status = 1;
        reply.json = UniValue{message}.write();
        return reply;
    }};
    UniValue args;
    if (!args.read(args_json) || !args.isArray() || args.empty() || !args[0].isStr()) return error("A call of the dialog is [method, params].");
    const std::string method{args[0].get_str()};
    const UniValue params{args.size() > 1 ? args[1] : UniValue{UniValue::VOBJ}};
    if (m_answered) return error("The dialog has been answered.");

    if (method == "hello") {
        if (m_hello) return error("hello: once");
        m_hello = true;
        Reply reply;
        reply.json = PageData(m_request).write();
        return reply;
    }
    if (method == "ready") {
        if (!m_hello || m_ready) return error("ready: once, after hello");
        const UniValue& w{params.isObject() ? params.find_value("width") : NullUniValue};
        const UniValue& h{params.isObject() ? params.find_value("height") : NullUniValue};
        if (!w.isNum() || !h.isNum() || !std::isfinite(w.get_real()) || !std::isfinite(h.get_real())) return error("ready: {width, height}");
        m_ready = true;
        m_width = w.get_real();
        m_height = h.get_real();
        Reply reply;
        reply.json = "true";
        reply.step = Step::SHOW;
        return reply;
    }
    if (method == "answer") {
        if (!m_ready) return error("answer: after ready");
        const UniValue& accept{params.isObject() ? params.find_value("accept") : NullUniValue};
        if (!accept.isBool()) return error("answer: {accept: true or false}");
        Answer answer;
        answer.accepted = accept.get_bool();
        const UniValue& pass{params.find_value("passphrase")};
        if (m_request.passphrase && answer.accepted) {
            if (!pass.isStr() || pass.get_str().empty()) return error("answer: the passphrase is needed");
            if (pass.get_str().size() > MAX_PASSPHRASE) return error("answer: the passphrase is too long");
            answer.passphrase.assign(pass.get_str().begin(), pass.get_str().end());
        } else if (!pass.isNull()) {
            return error("answer: no passphrase was asked for");
        }
        m_answered = true;
        m_answer = std::move(answer);
        Reply reply;
        reply.json = "true";
        reply.step = Step::ANSWER;
        return reply;
    }
    return error("Unknown method of the dialog");
}

Size Session::WindowSize(int max_width, int max_height) const
{
    // Within 320 to 720 pixels wide and 120 pixels high to the largest the window may be.
    const auto fit{[](double v, int lo, int hi) {
        hi = std::max(hi, lo);
        return std::clamp(static_cast<int>(std::ceil(v)), lo, hi);
    }};
    return {fit(m_width, 320, std::min(720, max_width)), fit(m_height, 120, max_height)};
}

Answer Session::TakeAnswer()
{
    Answer out{std::move(m_answer)};
    m_answer = {};
    return out;
}

bool Pending::Push(Request request, Done done)
{
    if (!m_busy) {
        m_busy = true;
        return true;
    }
    m_queue.emplace_back(std::move(request), std::move(done));
    return false;
}

std::optional<std::pair<Request, Pending::Done>> Pending::Next()
{
    if (m_queue.empty()) {
        m_busy = false;
        return std::nullopt;
    }
    auto next{std::move(m_queue.front())};
    m_queue.pop_front();
    return next;
}

} // namespace webui::dialog
