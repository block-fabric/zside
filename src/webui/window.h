// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_WINDOW_H
#define BITCOIN_WEBUI_WINDOW_H

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <webui/dialog.h>
#include <webui/windowframe.h>

namespace webui {

/**
 * The native window of the app with the web view in it (webview/webview), and
 * the event loop of the UI thread.
 *
 * Every method is for the UI thread, except Post, PostDelayed, Reply and Quit,
 * which any thread may call.
 */
class Window
{
public:
    struct Options {
        std::string title;
        int width{1180};
        int height{760};
        //! Web inspector and the context menu of the web view (-webuidevtools).
        bool devtools{false};
        //! Start maximized (the geometry saved last time).
        bool maximized{false};
        //! No title bar or borders of the system: the page draws its own title bar (the
        //! integrated title bar), where the platform can (SupportsFrameless).
        bool frameless{false};
    };
    //! The size of the window, and whether it is maximized (MainWindowGeometry of chains-qt).
    struct Geometry {
        int width{0};
        int height{0};
        bool maximized{false};
    };
    //! An item of the menu of the tray icon.
    struct TrayItem {
        std::string id;
        std::string label;
        bool enabled{true};
        bool separator{false};
    };
    //! The icon in the system tray (notification area), with its menu.
    struct Tray {
        bool visible{false};
        std::string tooltip;
        std::vector<TrayItem> menu;
    };
    struct Callbacks {
        //! The user asks to close the window (the close button, Alt+F4). The window stays until Quit.
        std::function<void()> close_requested;
        //! The page calls the bridge: the id of the call, the JSON array of its arguments.
        std::function<void(const std::string& call_id, const std::string& args_json)> call;
        //! The icon in the tray was clicked (BitcoinGUI::trayIconActivated: show or hide the window).
        std::function<void()> tray_activated;
        //! An item of the menu of the tray was chosen: its id.
        std::function<void(const std::string& id)> tray_menu;
        //! The window was minimized by the user (for "minimize to the tray").
        std::function<void()> minimized;
        //! The window was maximized or restored (by the page, the desktop or a shortcut).
        std::function<void(bool maximized)> maximized_changed;
        //! The session of the user ends (log off, shut down): the app stops (Windows).
        std::function<void()> session_ending;
    };

    /** The window of this platform. It is not shown until Show. */
    static std::unique_ptr<Window> Create(const Options& options, Callbacks callbacks);

    virtual ~Window() = default;

    /** Run the event loop until Quit. */
    virtual void Run() = 0;
    /** End the event loop. Thread-safe. */
    virtual void Quit() = 0;
    /** Run a function on the UI thread. Thread-safe. */
    virtual void Post(std::function<void()> fn) = 0;
    /** Run a function on the UI thread after a delay. Thread-safe. */
    virtual void PostDelayed(std::chrono::milliseconds delay, std::function<void()> fn) = 0;
    /** Answer a call of the page (status 0: result, otherwise error), with JSON. Thread-safe. */
    virtual void Reply(const std::string& call_id, int status, const std::string& json) = 0;

    /** Run a script in the page (not subject to the policy of the page: for the app and its tests only). */
    virtual void Eval(const std::string& script) = 0;

    virtual void SetTitle(const std::string& title) = 0;
    /** The network of the node: the icon of the window and of the tray takes its colours (NetworkStyle). */
    virtual void SetNetwork(const std::string& network) = 0;
    virtual void Show(bool minimized) = 0;
    /** Bring the window to the front (a second start of the app asks for it). */
    virtual void Present() = 0;
    virtual void Minimize() = 0;
    /** Hide the window (to the tray), and whether it shows now. */
    virtual void Hide() = 0;
    virtual bool IsVisible() = 0;
    /** The size of the window now. */
    virtual Geometry GetGeometry() = 0;

    /**
     * The integrated title bar: a window without the frame of the system, whose page draws the
     * title bar with the minimize, maximize and close buttons, moves the window when its empty
     * part is dragged and resizes it from handles at its edges.
     *
     *  - Linux (GTK): an undecorated window; the page's drags start the moves and resizes of
     *    the window manager (begin_move_drag, begin_resize_drag).
     *  - Windows (WebView2): a window without a caption: WS_OVERLAPPEDWINDOW kept, the client area
     *    extended over the caption in WM_NCCALCSIZE (the other borders, invisible on Windows 10 and
     *    11, still resize the window and carry its shadow and rounded corners); StartMove and
     *    StartResize: ReleaseCapture + WM_NCLBUTTONDOWN with HTCAPTION or the HT of the edge. A small
     *    window over the page's maximize button answers HTMAXBUTTON, for the snap layouts of Windows 11.
     *  - TODO(macos port): a hidden title bar (titlebarAppearsTransparent, titleVisibility hidden,
     *    NSWindowStyleMaskFullSizeContentView), the traffic lights of macOS kept at the left (the
     *    page then hides its own buttons and leaves room for them: "controls" of window.state);
     *    StartMove: performWindowDragWithEvent, resizing stays native.
     */
    //! Whether this platform can do without the frame of the system.
    virtual bool SupportsFrameless() = 0;
    //! Whether the window is without the frame of the system now.
    virtual bool IsFrameless() = 0;
    //! Take away or give back the frame of the system. False when it cannot change while the window is open (then at the next start).
    virtual bool SetFrameless(bool frameless) = 0;
    virtual bool IsMaximized() = 0;
    //! Maximize the window, or restore it when it is maximized.
    virtual void ToggleMaximize() = 0;
    //! Move the window with the pointer: called while the button pressed in the page's title bar is held.
    virtual void StartMove() = 0;
    //! Resize the window from an edge with the pointer, as StartMove.
    virtual void StartResize(WindowEdge edge) = 0;

    /**
     * Show, change or take away the icon in the tray. Desktops with a tray of the
     * StatusNotifierItem kind (KDE, most others; GNOME with an extension) and the older
     * kind (XEmbed) both have it.
     */
    virtual void SetTray(const Tray& tray) = 0;
    /** Whether this desktop has a tray to show the icon in. */
    virtual bool TrayAvailable() = 0;

    /**
     * A notification of the desktop (Notificator of chains-qt): a title, a text, and its kind:
     * "info", "warning" or "critical". Shown by the notification service of the desktop.
     */
    virtual void Notify(const std::string& title, const std::string& body, const std::string& level) = 0;

    /** Open a file, a folder or a web address with the application of the desktop for it. */
    virtual bool OpenExternal(const std::string& uri) = 0;

    virtual void SetClipboard(const std::string& text) = 0;
    virtual std::optional<std::string> GetClipboard() = 0;

    /**
     * The last real input of the user in the main window: a key pressed or a button of the pointer, as the platform saw it
     * (never an event the page makes up), by its serial number and how long ago (milliseconds). Nothing when it cannot be
     * told. The clipboard takes calls only right after one (audit 7 #5).
     */
    struct UserInput {
        uint64_t serial{0};
        int64_t age_ms{0};
    };
    virtual std::optional<UserInput> LastUserInput() = 0;
    /** For the tests (WEBUI_TEST_HOOKS): as if the user pressed a key now. */
    virtual void NoteUserInput() = 0;

    /** A file chooser of the desktop, to open a file or to save one. */
    struct FileDialog {
        bool save{false};
        std::string title;
        //! The name proposed when saving.
        std::string suggested_name;
        //! Filters: a name and its patterns ("Wallet Data", {"*.dat"}). The first one is chosen.
        std::vector<std::pair<std::string, std::vector<std::string>>> filters;
        //! Choose a folder, not a file.
        bool folder{false};
        //! The folder the chooser opens in.
        std::string start_dir;
        //! Dark or light as the look of the app (the chooser is the system's: GTK's prefer-dark setting, the dark mode
        //! of Windows), or as the desktop (the theme "system").
        dialog::Scheme scheme{dialog::Scheme::DESKTOP};
    };
    /**
     * Show a file chooser; `done` gets the path chosen, or nothing when it was cancelled. When
     * saving, the extension of the first filter is added to a name without one (as the Qt GUI
     * does), and the user confirms overwriting a file.
     */
    virtual void ChooseFile(const FileDialog& options, std::function<void(std::optional<std::string>)> done) = 0;

    /**
     * A question of the host, not of the page, with two buttons, `accept` and Cancel (Cancel is the default). Used
     * before the app does something the page asked for where the page's word is not enough (a command of the
     * console, a web address, a program the node runs, a deposit...): the page can neither draw nor answer it.
     * Shown with Ask; App::Confirm.
     */
    struct ConfirmDialog {
        std::string title;
        std::string message;
        std::string accept;
        //! Data shown apart, in a monospaced block, or nothing.
        std::string details{};
        dialog::Level level{dialog::Level::WARNING};
        //! The facts decoded by the host, in order (dialog::Row): a destination before its amount, the fee, the total.
        std::vector<dialog::Row> rows{};
        //! A field for the passphrase of the wallet (an encrypted wallet that is locked): asked here, never by the page.
        bool passphrase{false};
        std::string passphrase_label{};
        //! An error shown at the field (the passphrase typed before was wrong).
        std::string error{};
        //! The button that declines, when "Cancel" is not the word.
        std::string cancel{};
        //! What kind of question it is, for the limits of App::Confirm (audit 7 #8): the title when empty.
        std::string kind{};
    };

    /**
     * Ask the user in the dialog window of the host (dialog.h): a modal window of its own over this one, with a web
     * view of its own showing the page of the dialogs in the look of the app; the dialog of the system instead when
     * that window cannot be made or its page says nothing in time (never an answer the user did not give). One at a
     * time: a dialog asked while one shows waits for it. `done` gets the answer (declined when the window goes).
     */
    virtual void Ask(const dialog::Request& request, std::function<void(dialog::Answer)> done) = 0;

    /**
     * A question is likely soon (the page shows the review of what it would ask): the dialog window may be made ahead,
     * its page loaded, so that it shows at once. Nothing on a platform where it is quick anyway.
     */
    virtual void PrepareDialog() {}

    /**
     * For the tests (WEBUI_TEST_HOOKS) only, on the dialog shown now: "kind" (JSON "themed", "native" or "none"),
     * "eval" (run `arg` in the page of the themed dialog: its result as JSON), "snapshot" (a picture of the themed
     * dialog in the PNG file `arg`: true or false), "native" (the dialog of the system: `arg` "accept" or "cancel"
     * answers it, "state" gives its title and text). `done` gets nothing when there is no such dialog.
     */
    virtual void TestDialog(const std::string& action, const std::string& arg, std::function<void(std::optional<std::string>)> done) = 0;

    /** Save a picture of what the page shows now as PNG. For the tests. */
    virtual void Snapshot(const std::string& png_path, std::function<void(bool ok)> done) = 0;
};

} // namespace webui

#endif // BITCOIN_WEBUI_WINDOW_H
