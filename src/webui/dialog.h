// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_DIALOG_H
#define BITCOIN_WEBUI_DIALOG_H

#include <support/allocators/secure.h>
#include <univalue.h>

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/**
 * The dialogs of the host: what the app asks the user itself, not the page (a command of the console, a web
 * address, a program the node runs, a deposit, a bond...). The page can neither draw nor answer them (audit
 * port-1 #4): they are shown in a window of their own, owned by the host, in the look of the app.
 *
 *  - The window: a small modal top-level window over the main one (transient for it, centred on it), without
 *    the frame of the system, with a web view of its own. The main window takes no input while it shows.
 *  - Its page: a page built into the program (dialog.html, src/webui/app/src/dialog), served under an origin of
 *    its own (chains-dialog://dialog, https://dialog.chains-app.invalid on Windows): not the origin of the main
 *    page, so no storage, script or bridge of the main page reaches it, and the main page has no handle on it.
 *  - Its bridge: one function, __chainsDialog, which only the dialog's web view has: "hello" gives the page what
 *    to show (PageData: text only, put in the page with textContent, never as HTML), "ready" its size (the window
 *    shows then), "answer" the user's choice, once (Session).
 *  - Its look: the theme of the app as the host keeps it (the preferences of the network: a built-in theme, or a
 *    theme of the user, checked by the page as strictly as a theme file), its font and text size (Look).
 *  - Without the web view (it cannot be made, or the page says nothing in time), the dialog of the system is
 *    shown instead (the fallback): never an answer the user did not give.
 *  - One at a time: the window shows the dialogs asked meanwhile after it, in order (Pending).
 */
namespace webui::dialog {

//! Where the page of the dialogs is served from, where the platform lets the app serve a scheme (GTK).
inline constexpr std::string_view SCHEME{"chains-dialog"};
inline constexpr std::string_view ORIGIN{"chains-dialog://dialog"};
//! Where it is served from on Windows (WebView2): its own origin, beside the one of the main page.
inline constexpr std::string_view WIN_ORIGIN{"https://dialog.chains-app.invalid"};
//! The page.
inline constexpr std::string_view PAGE{"/dialog.html"};
//! The name of the bridge of the dialog's web view.
inline constexpr std::string_view BRIDGE{"__chainsDialog"};
//! How long the page has to say it is ready before the dialog of the system is shown instead (milliseconds).
inline constexpr int READY_TIMEOUT_MS{10000};

//! How grave what is asked is: the colour of the icon and of the button that accepts.
enum class Level { INFO, WARNING, DANGER };

//! The most rows (Row) a dialog shows; more are cut. A confirmation never has more (confirm.cpp refuses it instead).
constexpr size_t MAX_ROWS{80};

/**
 * A fact the dialog shows apart from its text, on a line of its own: what a spend pays and to whom, what a vote
 * changes (audit 7 #1). The host decodes them from the transaction or the parameters itself; the page shows them as
 * text (textContent), an address whole (wrapped, never cut), in the order given: a destination before its amount.
 */
struct Row {
    enum class Kind {
        //! A label and a value (a wallet, a network, a slot).
        TEXT,
        //! A destination: an address, a script, a deposit address, shown whole in a monospaced font.
        ADDRESS,
        //! An amount (of the recipient above it, a fee).
        AMOUNT,
        //! What is paid in all: shown heavier.
        TOTAL,
        //! A heading of the rows after it (one recipient of several).
        HEADING,
        //! A line of text across the rows (what the host could not know before, such as a fee chosen later).
        NOTE,
    };
    std::string label;
    std::string value;
    Kind kind{Kind::TEXT};
};

//! The name of a kind of row, as the page reads it.
std::string_view RowKindName(Row::Kind kind);

/** A question of the host. */
struct Request {
    std::string title;
    //! The text: paragraphs separated by an empty line; a line break stays one.
    std::string message;
    //! Data shown apart, in a monospaced block (an address, a command), or nothing.
    std::string details;
    //! The button that accepts ("Run", "Open"...). Cancel is the other one, and the default.
    std::string accept;
    std::string cancel{"Cancel"};
    Level level{Level::WARNING};
    //! The facts shown apart, in order (Row): decoded by the host.
    std::vector<Row> rows;
    //! A passphrase field: the answer carries what was typed in it (only when accepted).
    bool passphrase{false};
    std::string passphrase_label;
    //! An error shown at the field (the passphrase typed before was wrong).
    std::string error;
    //! Set when whoever asked no longer waits (the page cancelled its request): a question still waiting its turn is
    //! not shown (Pending::Next). Null for one that cannot be withdrawn.
    std::shared_ptr<std::atomic<bool>> withdrawn;
    //! The look of the app (Look).
    UniValue look{UniValue::VOBJ};
    //! The tests only (WEBUI_TEST_HOOKS): the themed window is not made, as when its web view cannot be (the fallback shows).
    bool test_fallback{false};
};

/** What the user answered. */
struct Answer {
    bool accepted{false};
    //! What was typed in the passphrase field (accepted, with a field), else empty.
    SecureString passphrase;
};

/** The name of a level ("info", "warning", "danger"), as the page reads it. */
std::string_view LevelName(Level level);

/**
 * Text of the host (with what it took from the page or the chain in it) as a dialog shows it: valid UTF-8 (U+FFFD
 * for what is not), line breaks kept ("\r\n" one), a tab a space, and every other control or format character (a
 * mark that turns text around, U+202E, an invisible one) written out as "[U+202E]" (as revealBidi of the page): what
 * is read is the order of the characters. Cut to `max_chars` characters with "…".
 */
std::string Shown(std::string_view text, size_t max_chars);

/** What the page shows ("hello"): every text through Shown (the title and the buttons on one line), the look. */
UniValue PageData(const Request& request);

/**
 * The text of the dialog of the system (the fallback), from what the page would show (PageData): the message, the
 * facts one a line ("label: value"), the details and the error.
 */
std::string FallbackText(const UniValue& page);

/**
 * The look of the dialogs from the preferences of the app (of the network shown): the theme chosen (the page
 * checks it is one), the theme of the user it names if it is one (only that one, the page checks it as a theme
 * file), the text size, the fonts, and the direction of the language of the app.
 */
UniValue Look(const UniValue& prefs, bool rtl, const std::string& language);

/**
 * Whether a look is dark, light, or follows the desktop (the theme "system"): for the file choosers and the dialogs of
 * the system. From the preferences, or from a look (Look).
 */
enum class Scheme { DESKTOP, LIGHT, DARK };
Scheme SchemeOf(const UniValue& prefs);

/** Whether the origin of the dialogs serves a file: the page, its script and style sheet, and the fonts; nothing of the main page. */
bool Serves(std::string_view path);

/** The content security policy of the page of the dialogs: its own files, no network, no frames, no forms. */
std::string ContentSecurityPolicy();

struct Size {
    int width{0};
    int height{0};
};

/**
 * One dialog, as its page goes through it, on the UI thread: the platform passes every call of the page's bridge to
 * Call, answers the page with the reply, and does what its step says (show the window, close it with the answer).
 *
 *  1. "hello" (once): the reply is PageData.
 *  2. "ready" {width, height} (once, after hello): the size the page needs, in CSS pixels; the window shows.
 *  3. "answer" {accept, passphrase?} (once, after ready): the dialog ends. A passphrase is taken only when the
 *     request has a field and the user accepted, and is then needed.
 *
 * Anything else, out of order, or after the answer is refused (an error to the page, nothing else happens).
 */
class Session
{
public:
    enum class Step { NONE, SHOW, ANSWER };
    struct Reply {
        //! 0 for a result, else an error (the bridge's convention).
        int status{0};
        std::string json{"null"};
        Step step{Step::NONE};
    };

    explicit Session(Request request);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    /** A call of the page: the JSON array of the arguments of __chainsDialog ([method, params]). */
    Reply Call(const std::string& args_json);

    /** The size of the window for what the page asked (after SHOW), within the largest one it may have. */
    Size WindowSize(int max_width, int max_height) const;

    /** The answer (after ANSWER); moved out. */
    Answer TakeAnswer();

    const Request& request() const { return m_request; }
    bool Shown() const { return m_ready; }
    bool Answered() const { return m_answered; }

private:
    Request m_request;
    bool m_hello{false};
    bool m_ready{false};
    bool m_answered{false};
    double m_width{0};
    double m_height{0};
    Answer m_answer;
};

/** The dialogs asked while one shows, in order, on the UI thread. */
class Pending
{
public:
    using Done = std::function<void(Answer)>;

    /** A dialog asked: true when it is to be shown now (none shows), false when it waits its turn. */
    bool Push(Request request, Done done);
    /**
     * The one shown is answered: the next one, if there is one (it shows now), or nothing (none shows). A question
     * withdrawn meanwhile (Request::withdrawn) is skipped: its `done` gets a declined answer at once.
     */
    std::optional<std::pair<Request, Done>> Next();
    bool Busy() const { return m_busy; }
    size_t Waiting() const { return m_queue.size(); }

private:
    bool m_busy{false};
    std::deque<std::pair<Request, Done>> m_queue;
};

} // namespace webui::dialog

#endif // BITCOIN_WEBUI_DIALOG_H
