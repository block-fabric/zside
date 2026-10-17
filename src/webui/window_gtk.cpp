// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The window on Linux and the BSDs: GTK 3 with WebKitGTK 4.1, through webview/webview.
// The interface is served from the files built into the program, under the
// scheme chains-app://, with its content security policy as a header.

#include <webui/assets.h>
#include <webui/chainconfig.h>
#include <webui/dialog.h>
#include <webui/tray_gtk.h>
#include <support/cleanse.h>
#include <webui/window.h>

#include <logging.h>
#include <univalue.h>

#include <webview.h>

#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

#include <atomic>
#include <cstring>
#include <map>
#include <string>
#include <utility>

namespace webui {
namespace {

/** Answer a request of a web view with a file built into the program, if `served` takes its path under `origin`. */
void Serve(WebKitURISchemeRequest* request, std::string_view origin, bool (*served)(std::string_view), const std::string& policy)
{
    const char* path{webkit_uri_scheme_request_get_path(request)};
    const char* uri{webkit_uri_scheme_request_get_uri(request)};
    // Only the files of the app itself, from their own origin.
    const Asset* asset{nullptr};
    if (uri && std::string_view{uri}.starts_with(std::string{origin} + "/") && served(path ? path : "/")) {
        asset = FindAsset(BuiltinAssets(), path ? path : "/");
    }
    if (!asset) {
        GError* error{g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Not found")};
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    // The bytes are static: the stream does not copy or free them.
    GInputStream* stream{g_memory_input_stream_new_from_data(asset->data.data(), asset->data.size(), nullptr)};
    WebKitURISchemeResponse* response{webkit_uri_scheme_response_new(stream, asset->data.size())};
    const std::string_view mime{MimeType(asset->path)};
    const std::string content_type{mime.substr(0, mime.find(';'))};
    webkit_uri_scheme_response_set_content_type(response, content_type.c_str());
    SoupMessageHeaders* headers{soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE)};
    // The headers replace the ones of the response: the type goes in them too.
    soup_message_headers_append(headers, "Content-Type", std::string{mime}.c_str());
    soup_message_headers_append(headers, "Content-Security-Policy", policy.c_str());
    soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
    soup_message_headers_append(headers, "Cache-Control", "no-store");
    webkit_uri_scheme_response_set_http_headers(response, headers);
    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
    g_object_unref(stream);
}

void ServeAsset(WebKitURISchemeRequest* request, gpointer)
{
    Serve(request, ORIGIN, [](std::string_view) { return true; }, ContentSecurityPolicy());
}

//! The page of the dialogs of the host, under its own origin: its files only (dialog::Serves).
void ServeDialogAsset(WebKitURISchemeRequest* request, gpointer)
{
    Serve(request, dialog::ORIGIN, dialog::Serves, dialog::ContentSecurityPolicy());
}

class GtkWindowImpl final : public Window
{
public:
    GtkWindowImpl(const Options& options, Callbacks callbacks) : m_callbacks(std::move(callbacks)), m_devtools(options.devtools)
    {
        if (!gtk_init_check(nullptr, nullptr)) throw std::runtime_error("Cannot open the display (GTK init failed)");

        WebKitWebContext* context{webkit_web_context_get_default()};
        static std::atomic<bool> registered{false};
        if (!registered.exchange(true)) {
            const std::string scheme{SCHEME};
            webkit_web_context_register_uri_scheme(context, scheme.c_str(), ServeAsset, nullptr, nullptr);
            WebKitSecurityManager* security{webkit_web_context_get_security_manager(context)};
            webkit_security_manager_register_uri_scheme_as_secure(security, scheme.c_str());
            webkit_security_manager_register_uri_scheme_as_cors_enabled(security, scheme.c_str());
            // The page of the dialogs of the host (dialog.h): an origin of its own, nothing of the main page's.
            const std::string dialog_scheme{dialog::SCHEME};
            webkit_web_context_register_uri_scheme(context, dialog_scheme.c_str(), ServeDialogAsset, nullptr, nullptr);
            webkit_security_manager_register_uri_scheme_as_secure(security, dialog_scheme.c_str());
            webkit_security_manager_register_uri_scheme_as_cors_enabled(security, dialog_scheme.c_str());
        }

        m_window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
        gtk_window_set_title(GTK_WINDOW(m_window), options.title.c_str());
        gtk_window_set_default_size(GTK_WINDOW(m_window), options.width, options.height);
        if (GdkPixbuf* icon{AppIcon(128)}) {
            gtk_window_set_icon(GTK_WINDOW(m_window), icon);
            g_object_unref(icon);
        } else {
            gtk_window_set_icon_name(GTK_WINDOW(m_window), std::string{chain::STEM}.c_str());
        }
        if (options.maximized) gtk_window_maximize(GTK_WINDOW(m_window));
        // The integrated title bar: the page draws the title bar and the handles at the edges.
        m_frameless = options.frameless;
        if (m_frameless) gtk_window_set_decorated(GTK_WINDOW(m_window), FALSE);
        g_signal_connect(m_window, "window-state-event", G_CALLBACK(+[](GtkWidget*, GdkEventWindowState* event, gpointer self) -> gboolean {
                             auto* w{static_cast<GtkWindowImpl*>(self)};
                             const bool iconified{(event->new_window_state & GDK_WINDOW_STATE_ICONIFIED) != 0};
                             const bool maximized{(event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) != 0};
                             const bool changed{maximized != w->m_maximized};
                             w->m_maximized = maximized;
                             if (changed && w->m_callbacks.maximized_changed) w->m_callbacks.maximized_changed(maximized);
                             if (iconified && (event->changed_mask & GDK_WINDOW_STATE_ICONIFIED) && w->m_callbacks.minimized) w->m_callbacks.minimized();
                             return FALSE;
                         }),
                         this);
        g_signal_connect(m_window, "configure-event", G_CALLBACK(+[](GtkWidget* widget, GdkEvent*, gpointer self) -> gboolean {
                             auto* w{static_cast<GtkWindowImpl*>(self)};
                             // The size to keep is the one of the window when it is not maximized.
                             if (!w->m_maximized) gtk_window_get_size(GTK_WINDOW(widget), &w->m_width, &w->m_height);
                             return FALSE;
                         }),
                         this);
        m_width = options.width;
        m_height = options.height;
        g_signal_connect(m_window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer self) -> gboolean {
                             auto* w{static_cast<GtkWindowImpl*>(self)};
                             if (w->m_callbacks.close_requested) w->m_callbacks.close_requested();
                             // The window stays until the app quits.
                             return TRUE;
                         }),
                         this);

        m_webview = std::make_unique<webview::webview>(options.devtools, m_window);
        m_view = WEBKIT_WEB_VIEW(m_webview->browser_controller().value());

        WebKitSettings* settings{webkit_web_view_get_settings(m_view)};
        webkit_settings_set_enable_developer_extras(settings, options.devtools);
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, options.devtools);
        webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
        webkit_settings_set_enable_back_forward_navigation_gestures(settings, FALSE);
        webkit_settings_set_enable_webaudio(settings, FALSE);
        webkit_settings_set_enable_webgl(settings, FALSE);
        webkit_settings_set_enable_media(settings, FALSE);

        // The page stays the app: no navigation away from it, no new windows.
        g_signal_connect(m_view, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) -> gboolean {
                             if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
                                 auto* navigation{WEBKIT_NAVIGATION_POLICY_DECISION(decision)};
                                 WebKitNavigationAction* action{webkit_navigation_policy_decision_get_navigation_action(navigation)};
                                 const char* uri{webkit_uri_request_get_uri(webkit_navigation_action_get_request(action))};
                                 const bool own{uri && std::string_view{uri}.starts_with(std::string{ORIGIN} + "/")};
                                 if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION || !own) {
                                     LogDebug(BCLog::RPC, "webui: navigation to %s refused\n", uri ? uri : "(none)");
                                     webkit_policy_decision_ignore(decision);
                                     return TRUE;
                                 }
                             }
                             return FALSE;
                         }),
                         nullptr);
        g_signal_connect(m_view, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu*, GdkEvent*, WebKitHitTestResult*, gpointer self) -> gboolean {
                             // The page has menus of its own; the one of the browser only with the developer tools.
                             return static_cast<GtkWindowImpl*>(self)->m_devtools ? FALSE : TRUE;
                         }),
                         this);
        g_signal_connect(m_view, "permission-request", G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer) -> gboolean {
                             webkit_permission_request_deny(request);
                             return TRUE;
                         }),
                         nullptr);
        g_signal_connect(m_view, "web-process-terminated", G_CALLBACK(+[](WebKitWebView* view, WebKitWebProcessTerminationReason reason, gpointer) {
                             LogWarning("webui: the web process ended (reason %d); reloading the interface", int(reason));
                             webkit_web_view_reload(view);
                         }),
                         nullptr);

        // The last press of a button in the page: the moves and resizes the page starts begin there. It is an input of the
        // user too (LastUserInput: the clipboard takes calls right after one).
        g_signal_connect(m_view, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* event, gpointer self) -> gboolean {
                             auto* w{static_cast<GtkWindowImpl*>(self)};
                             if (w->m_last_press) gdk_event_free(w->m_last_press);
                             w->m_last_press = gdk_event_copy(reinterpret_cast<GdkEvent*>(event));
                             if (gdk_event_get_event_type(reinterpret_cast<GdkEvent*>(event)) == GDK_BUTTON_PRESS) w->NoteUserInput();
                             return FALSE;
                         }),
                         this);
        g_signal_connect(m_view, "key-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventKey* event, gpointer self) -> gboolean {
                             // A key of the user (not a modifier alone): the page gets it as usual.
                             if (!event->is_modifier) static_cast<GtkWindowImpl*>(self)->NoteUserInput();
                             return FALSE;
                         }),
                         this);

        m_webview->bind("__chains", [this](std::string id, std::string args, void*) {
            if (m_callbacks.call) m_callbacks.call(id, args);
            // The request may hold a passphrase: not left in freed memory (the bridge took what it needs).
            memory_cleanse(args.data(), args.size());
        }, nullptr);
        m_webview->navigate(std::string{ORIGIN} + "/index.html");
    }

    ~GtkWindowImpl() override
    {
        if (m_themed) {
            if (m_themed->timer) g_source_remove(m_themed->timer);
            GtkWidget* window{m_themed->window};
            m_themed.reset();
            gtk_widget_destroy(window);
        }
        if (m_native) gtk_widget_destroy(m_native);
        m_webview.reset();
        if (m_last_press) gdk_event_free(m_last_press);
        if (m_window) gtk_widget_destroy(m_window);
    }

    void Run() override { m_webview->run(); }
    void Quit() override { m_webview->terminate(); }

    void Post(std::function<void()> fn) override
    {
        m_webview->dispatch(std::move(fn));
    }

    void PostDelayed(std::chrono::milliseconds delay, std::function<void()> fn) override
    {
        auto* heap{new std::function<void()>(std::move(fn))};
        // g_timeout_add_full is thread-safe: it adds to the default main context, which the UI thread runs.
        g_timeout_add_full(
            G_PRIORITY_DEFAULT, static_cast<guint>(delay.count()),
            +[](gpointer data) -> gboolean {
                (*static_cast<std::function<void()>*>(data))();
                return G_SOURCE_REMOVE;
            },
            heap, +[](gpointer data) { delete static_cast<std::function<void()>*>(data); });
    }

    void Reply(const std::string& call_id, int status, const std::string& json) override
    {
        // resolve() posts to the UI thread itself.
        m_webview->resolve(call_id, status, json);
    }

    void Eval(const std::string& script) override { m_webview->eval(script); }

    void SetTitle(const std::string& title) override { gtk_window_set_title(GTK_WINDOW(m_window), title.c_str()); }

    void SetNetwork(const std::string& network) override
    {
        SetIconTint(TintOf(network));
        if (GdkPixbuf* icon{AppIcon(128)}) {
            gtk_window_set_icon(GTK_WINDOW(m_window), icon);
            g_object_unref(icon);
        }
    }

    void Show(bool minimized) override
    {
        gtk_widget_show_all(m_window);
        if (minimized) gtk_window_iconify(GTK_WINDOW(m_window));
        gtk_widget_grab_focus(GTK_WIDGET(m_view));
    }

    void Present() override
    {
        gtk_window_deiconify(GTK_WINDOW(m_window));
        gtk_window_present(GTK_WINDOW(m_window));
    }

    void Minimize() override { gtk_window_iconify(GTK_WINDOW(m_window)); }

    void Hide() override { gtk_widget_hide(m_window); }
    bool IsVisible() override { return gtk_widget_get_visible(m_window); }
    Geometry GetGeometry() override { return {m_width, m_height, m_maximized}; }

    bool SupportsFrameless() override { return true; }
    bool IsFrameless() override { return m_frameless; }

    bool SetFrameless(bool frameless) override
    {
        if (frameless == m_frameless) return true;
        // An X11 window manager draws the frame, and takes it away or gives it back at once. On
        // Wayland GTK draws it itself, and only decides that when the window is made.
        bool live{!gtk_widget_get_realized(m_window)};
#ifdef GDK_WINDOWING_X11
        if (GDK_IS_X11_DISPLAY(gtk_widget_get_display(m_window))) live = true;
#endif
        if (!live) return false;
        gtk_window_set_decorated(GTK_WINDOW(m_window), frameless ? FALSE : TRUE);
        m_frameless = frameless;
        return true;
    }

    bool IsMaximized() override { return m_maximized; }

    void ToggleMaximize() override
    {
        if (m_maximized) gtk_window_unmaximize(GTK_WINDOW(m_window));
        else gtk_window_maximize(GTK_WINDOW(m_window));
    }

    void StartMove() override
    {
        int button, x, y;
        guint32 time;
        if (!Press(button, x, y, time)) return;
        gtk_window_begin_move_drag(GTK_WINDOW(m_window), button, x, y, time);
    }

    void StartResize(WindowEdge edge) override
    {
        if (m_maximized) return;
        int button, x, y;
        guint32 time;
        if (!Press(button, x, y, time)) return;
        GdkWindowEdge gdk_edge{GDK_WINDOW_EDGE_SOUTH_EAST};
        switch (edge) {
        case WindowEdge::TOP: gdk_edge = GDK_WINDOW_EDGE_NORTH; break;
        case WindowEdge::BOTTOM: gdk_edge = GDK_WINDOW_EDGE_SOUTH; break;
        case WindowEdge::LEFT: gdk_edge = GDK_WINDOW_EDGE_WEST; break;
        case WindowEdge::RIGHT: gdk_edge = GDK_WINDOW_EDGE_EAST; break;
        case WindowEdge::TOP_LEFT: gdk_edge = GDK_WINDOW_EDGE_NORTH_WEST; break;
        case WindowEdge::TOP_RIGHT: gdk_edge = GDK_WINDOW_EDGE_NORTH_EAST; break;
        case WindowEdge::BOTTOM_LEFT: gdk_edge = GDK_WINDOW_EDGE_SOUTH_WEST; break;
        case WindowEdge::BOTTOM_RIGHT: gdk_edge = GDK_WINDOW_EDGE_SOUTH_EAST; break;
        }
        gtk_window_begin_resize_drag(GTK_WINDOW(m_window), gdk_edge, button, x, y, time);
    }

    void SetTray(const Tray& tray) override
    {
        if (!m_tray) {
            m_tray = std::make_unique<GtkTray>([this] { if (m_callbacks.tray_activated) m_callbacks.tray_activated(); },
                                               [this](const std::string& id) { if (m_callbacks.tray_menu) m_callbacks.tray_menu(id); });
        }
        m_tray->Set(tray);
    }
    bool TrayAvailable() override
    {
        if (!m_tray) SetTray({});
        return m_tray->Available();
    }

    void Notify(const std::string& title, const std::string& body, const std::string& level) override { DesktopNotify(title, body, level); }

    bool OpenExternal(const std::string& uri) override
    {
        GError* error{nullptr};
        const bool ok{g_app_info_launch_default_for_uri(uri.c_str(), nullptr, &error) == TRUE};
        if (error) {
            LogWarning("webui: cannot open %s: %s", uri, error->message);
            g_error_free(error);
        }
        return ok;
    }

    void SetClipboard(const std::string& text) override
    {
        GtkClipboard* clipboard{gtk_clipboard_get(GDK_SELECTION_CLIPBOARD)};
        gtk_clipboard_set_text(clipboard, text.c_str(), static_cast<gint>(text.size()));
        gtk_clipboard_store(clipboard);
    }

    std::optional<std::string> GetClipboard() override
    {
        GtkClipboard* clipboard{gtk_clipboard_get(GDK_SELECTION_CLIPBOARD)};
        gchar* text{gtk_clipboard_wait_for_text(clipboard)};
        if (!text) return std::nullopt;
        std::string out{text};
        g_free(text);
        return out;
    }

    void Snapshot(const std::string& png_path, std::function<void(bool ok)> done) override { SnapshotView(m_view, png_path, std::move(done)); }

    std::optional<UserInput> LastUserInput() override
    {
        if (m_input_serial == 0) return std::nullopt;
        return UserInput{m_input_serial, (g_get_monotonic_time() - m_input_time) / 1000};
    }

    void NoteUserInput() override
    {
        ++m_input_serial;
        m_input_time = g_get_monotonic_time();
    }

    void Ask(const dialog::Request& request, std::function<void(dialog::Answer)> done) override
    {
        // One at a time: a dialog asked while one shows waits for it.
        if (!m_pending.Push(request, done)) return;
        ShowDialog(request, std::move(done));
    }

    void TestDialog(const std::string& action, const std::string& arg, std::function<void(std::optional<std::string>)> done) override
    {
        if (action == "kind") {
            done(m_themed ? (m_themed->session->Shown() ? "\"themed\"" : "\"loading\"") : m_native ? "\"native\"" : "\"none\"");
        } else if (action == "eval" && m_themed && m_themed->session->Shown()) {
            auto* job{new std::function<void(std::optional<std::string>)>{std::move(done)}};
            const auto finished{+[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<std::function<void(std::optional<std::string>)>> job{static_cast<std::function<void(std::optional<std::string>)>*>(data)};
                GError* error{nullptr};
#if WEBKIT_CHECK_VERSION(2, 40, 0)
                JSCValue* value{webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error)};
#else
                WebKitJavascriptResult* js{webkit_web_view_run_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error)};
                JSCValue* value{js ? JSC_VALUE(g_object_ref(webkit_javascript_result_get_js_value(js))) : nullptr};
                if (js) webkit_javascript_result_unref(js);
#endif
                std::optional<std::string> out;
                if (value) {
                    if (char* json{jsc_value_to_json(value, 0)}) {
                        out = json;
                        g_free(json);
                    } else {
                        out = "null";
                    }
                    g_object_unref(value);
                } else if (error) {
                    LogWarning("webui: test script in the dialog: %s", error->message);
                    g_error_free(error);
                }
                (*job)(out);
            }};
#if WEBKIT_CHECK_VERSION(2, 40, 0)
            webkit_web_view_evaluate_javascript(m_themed->web, arg.c_str(), static_cast<gssize>(arg.size()), nullptr, nullptr, nullptr, finished, job);
#else
            webkit_web_view_run_javascript(m_themed->web, arg.c_str(), nullptr, finished, job);
#endif
        } else if (action == "snapshot" && m_themed && m_themed->session->Shown()) {
            SnapshotView(m_themed->web, arg, [done = std::move(done)](bool ok) { done(ok ? "true" : "false"); });
        } else if (action == "native" && m_native) {
            if (arg == "state") {
                UniValue state{UniValue::VOBJ};
                state.pushKV("title", m_native_title);
                state.pushKV("message", m_native_message);
                done(state.write());
            } else {
                gtk_dialog_response(GTK_DIALOG(m_native), arg == "accept" ? GTK_RESPONSE_ACCEPT : GTK_RESPONSE_CANCEL);
                done("true");
            }
        } else {
            done(std::nullopt);
        }
    }

    void ChooseFile(const FileDialog& options, std::function<void(std::optional<std::string>)> done) override
    {
        GtkFileChooserNative* chooser{gtk_file_chooser_native_new(
            options.title.c_str(), GTK_WINDOW(m_window), options.folder ? GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER : options.save ? GTK_FILE_CHOOSER_ACTION_SAVE : GTK_FILE_CHOOSER_ACTION_OPEN,
            options.save ? "_Save" : options.folder ? "_Select" : "_Open", "_Cancel")};
        GtkFileChooser* fc{GTK_FILE_CHOOSER(chooser)};
        gtk_native_dialog_set_modal(GTK_NATIVE_DIALOG(chooser), TRUE);
        if (!options.start_dir.empty()) gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(chooser), options.start_dir.c_str());
        std::string suffix;
        for (const auto& [name, patterns] : options.filters) {
            GtkFileFilter* filter{gtk_file_filter_new()};
            std::string label{name};
            if (!patterns.empty()) {
                label += " (";
                for (size_t i = 0; i < patterns.size(); ++i) label += (i ? " " : "") + patterns[i];
                label += ")";
            }
            gtk_file_filter_set_name(filter, label.c_str());
            for (const auto& pattern : patterns) {
                gtk_file_filter_add_pattern(filter, pattern.c_str());
                if (suffix.empty() && pattern.starts_with("*.") && pattern.find_first_of("*?[", 1) == std::string::npos) suffix = pattern.substr(1);
            }
            gtk_file_chooser_add_filter(fc, filter);
        }
        if (options.save) {
            gtk_file_chooser_set_do_overwrite_confirmation(fc, TRUE);
            if (!options.suggested_name.empty()) gtk_file_chooser_set_current_name(fc, options.suggested_name.c_str());
        }
        struct Job {
            std::function<void(std::optional<std::string>)> done;
            bool save;
            std::string suffix;
            GtkWindowImpl* self;
        };
        auto* job{new Job{std::move(done), options.save, suffix, this}};
        g_signal_connect(chooser, "response", G_CALLBACK(+[](GtkNativeDialog* dialog, gint response, gpointer data) {
                             std::unique_ptr<Job> job{static_cast<Job*>(data)};
                             std::optional<std::string> path;
                             if (response == GTK_RESPONSE_ACCEPT) {
                                 if (gchar* name{gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog))}) {
                                     path = name;
                                     g_free(name);
                                 }
                             }
                             if (path && job->save && !job->suffix.empty()) {
                                 // A name without an extension gets the one of the filter (GUIUtil::getSaveFileName).
                                 const auto slash{path->rfind('/')};
                                 const std::string base{slash == std::string::npos ? *path : path->substr(slash + 1)};
                                 if (base.find('.') == std::string::npos) *path += job->suffix;
                             }
                             g_object_unref(dialog);
                             job->self->SetScheme(dialog::Scheme::DESKTOP);
                             job->done(path);
                         }),
                         job);
        // Dark or light as the app (a chooser of GTK in this process follows the setting; one of the portal, the desktop).
        SetScheme(options.scheme);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }

private:
    using Done = std::function<void(dialog::Answer)>;

    //! The dialog window of the host shown now (dialog.h): its window, its web view, where its page is.
    struct Themed {
        uint64_t id{0};
        std::unique_ptr<dialog::Session> session;
        GtkWidget* window{nullptr};
        std::unique_ptr<webview::webview> view;
        WebKitWebView* web{nullptr};
        Done done;
        guint timer{0};
    };

    static void SnapshotView(WebKitWebView* view, const std::string& png_path, std::function<void(bool ok)> done)
    {
        struct Job {
            std::string path;
            std::function<void(bool)> done;
        };
        auto* job{new Job{png_path, std::move(done)}};
        webkit_web_view_get_snapshot(
            view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
            +[](GObject* source, GAsyncResult* result, gpointer data) {
                std::unique_ptr<Job> job{static_cast<Job*>(data)};
                GError* error{nullptr};
                cairo_surface_t* surface{webkit_web_view_get_snapshot_finish(WEBKIT_WEB_VIEW(source), result, &error)};
                bool ok{false};
                if (surface) {
                    ok = cairo_surface_write_to_png(surface, job->path.c_str()) == CAIRO_STATUS_SUCCESS;
                    cairo_surface_destroy(surface);
                } else if (error) {
                    LogWarning("webui: snapshot failed: %s", error->message);
                    g_error_free(error);
                }
                job->done(ok);
            },
            job);
    }

    /**
     * Dark or light for the dialogs of GTK (the file choosers, the fallback dialog): GTK's prefer-dark setting of
     * this program, set as the app's look while one shows, and back to the desktop's after (the theme "system").
     */
    void SetScheme(dialog::Scheme scheme)
    {
        GtkSettings* settings{gtk_settings_get_default()};
        if (!settings) return;
        if (!m_desktop_dark) {
            gboolean dark{FALSE};
            g_object_get(settings, "gtk-application-prefer-dark-theme", &dark, nullptr);
            m_desktop_dark = dark != FALSE;
        }
        const gboolean dark{scheme == dialog::Scheme::DARK ? TRUE : scheme == dialog::Scheme::LIGHT ? FALSE : (*m_desktop_dark ? TRUE : FALSE)};
        g_object_set(settings, "gtk-application-prefer-dark-theme", dark, nullptr);
    }

    void ShowDialog(const dialog::Request& request, Done done)
    {
        if (!request.test_fallback && OpenThemed(request, done)) return;
        OpenNative(request, std::move(done));
    }

    /** The dialog shown is answered: its answer, then the next dialog asked meanwhile, if any. */
    void Finish(const Done& done, dialog::Answer answer)
    {
        done(std::move(answer));
        if (auto next{m_pending.Next()}) {
            Post([this, request = std::move(next->first), done = std::move(next->second)] { ShowDialog(request, done); });
        }
    }

    /** The dialog window, with the page of the dialogs. False when it cannot be made (then the dialog of GTK). */
    bool OpenThemed(const dialog::Request& request, const Done& done)
    {
        auto themed{std::make_unique<Themed>()};
        themed->id = ++m_dialog_serial;
        themed->session = std::make_unique<dialog::Session>(request);
        themed->done = done;
        GtkWidget* window{gtk_window_new(GTK_WINDOW_TOPLEVEL)};
        GtkWindow* gw{GTK_WINDOW(window)};
        gtk_window_set_title(gw, request.title.c_str());
        // Modal to the main window, over it and centred on it, without the frame of the system (the page draws it).
        gtk_window_set_transient_for(gw, GTK_WINDOW(m_window));
        gtk_window_set_modal(gw, TRUE);
        gtk_window_set_destroy_with_parent(gw, TRUE);
        gtk_window_set_decorated(gw, FALSE);
        gtk_window_set_type_hint(gw, GDK_WINDOW_TYPE_HINT_DIALOG);
        gtk_window_set_position(gw, GTK_WIN_POS_CENTER_ON_PARENT);
        gtk_window_set_resizable(gw, FALSE);
        gtk_window_set_skip_taskbar_hint(gw, TRUE);
        gtk_window_set_skip_pager_hint(gw, TRUE);
        gtk_window_set_default_size(gw, 460, 240);
        themed->window = window;
        try {
            themed->view = std::make_unique<webview::webview>(false, window);
            themed->web = WEBKIT_WEB_VIEW(themed->view->browser_controller().value());
        } catch (const std::exception& e) {
            LogWarning("webui: the dialog window cannot be made (%s): the dialog of the system instead", e.what());
            themed->view.reset();
            gtk_widget_destroy(window);
            return false;
        }
        WebKitWebView* web{themed->web};
        WebKitSettings* settings{webkit_web_view_get_settings(web)};
        webkit_settings_set_enable_developer_extras(settings, FALSE);
        webkit_settings_set_javascript_can_access_clipboard(settings, FALSE);
        webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
        webkit_settings_set_enable_back_forward_navigation_gestures(settings, FALSE);
        webkit_settings_set_enable_webaudio(settings, FALSE);
        webkit_settings_set_enable_webgl(settings, FALSE);
        webkit_settings_set_enable_media(settings, FALSE);
        // The page of the dialogs stays itself: no navigation, no new windows, no menu, no permissions.
        g_signal_connect(web, "decide-policy", G_CALLBACK(+[](WebKitWebView*, WebKitPolicyDecision* decision, WebKitPolicyDecisionType type, gpointer) -> gboolean {
                             if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
                                 auto* navigation{WEBKIT_NAVIGATION_POLICY_DECISION(decision)};
                                 WebKitNavigationAction* action{webkit_navigation_policy_decision_get_navigation_action(navigation)};
                                 const char* uri{webkit_uri_request_get_uri(webkit_navigation_action_get_request(action))};
                                 const bool own{uri && std::string_view{uri} == std::string{dialog::ORIGIN} + std::string{dialog::PAGE}};
                                 if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION || !own) {
                                     webkit_policy_decision_ignore(decision);
                                     return TRUE;
                                 }
                             }
                             return FALSE;
                         }),
                         nullptr);
        g_signal_connect(web, "context-menu", G_CALLBACK(+[](WebKitWebView*, WebKitContextMenu*, GdkEvent*, WebKitHitTestResult*, gpointer) -> gboolean { return TRUE; }), nullptr);
        g_signal_connect(web, "permission-request", G_CALLBACK(+[](WebKitWebView*, WebKitPermissionRequest* request, gpointer) -> gboolean {
                             webkit_permission_request_deny(request);
                             return TRUE;
                         }),
                         nullptr);
        const uint64_t id{themed->id};
        auto* tag{new DialogTag{this, id}};
        // The page gone (its process ended, or it did not load): the dialog of the system instead.
        g_signal_connect_data(web, "web-process-terminated", G_CALLBACK(+[](WebKitWebView*, WebKitWebProcessTerminationReason reason, gpointer data) {
                                  auto* t{static_cast<DialogTag*>(data)};
                                  LogWarning("webui: the process of the dialog window ended (reason %d)", int(reason));
                                  t->self->Fallback(t->id);
                              }),
                              tag, +[](gpointer data, GClosure*) { delete static_cast<DialogTag*>(data); }, GConnectFlags(0));
        auto* tag2{new DialogTag{this, id}};
        g_signal_connect_data(web, "load-failed", G_CALLBACK(+[](WebKitWebView*, WebKitLoadEvent, gchar* uri, GError* error, gpointer data) -> gboolean {
                                  auto* t{static_cast<DialogTag*>(data)};
                                  LogWarning("webui: the page of the dialog window did not load (%s): %s", uri ? uri : "", error ? error->message : "");
                                  t->self->Fallback(t->id);
                                  return TRUE;
                              }),
                              tag2, +[](gpointer data, GClosure*) { delete static_cast<DialogTag*>(data); }, GConnectFlags(0));
        // The button that accepts takes clicks and keys a moment after the window is on the screen and has the focus, and
        // again after each time it lost it (audit 7 #7): the page is told when the window shows, hides, takes or loses
        // the focus, and starts the delay then (dialog.ts, arm).
        for (const char* signal : {"map-event", "focus-in-event"}) {
            auto* tag_arm{new DialogTag{this, id}};
            g_signal_connect_data(window, signal, G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                                      auto* t{static_cast<DialogTag*>(data)};
                                      t->self->ArmThemed(t->id, true);
                                      return FALSE;
                                  }),
                                  tag_arm, +[](gpointer data, GClosure*) { delete static_cast<DialogTag*>(data); }, GConnectFlags(0));
        }
        for (const char* signal : {"unmap-event", "focus-out-event"}) {
            auto* tag_disarm{new DialogTag{this, id}};
            g_signal_connect_data(window, signal, G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                                      auto* t{static_cast<DialogTag*>(data)};
                                      t->self->ArmThemed(t->id, false);
                                      return FALSE;
                                  }),
                                  tag_disarm, +[](gpointer data, GClosure*) { delete static_cast<DialogTag*>(data); }, GConnectFlags(0));
        }
        auto* tag_state{new DialogTag{this, id}};
        g_signal_connect_data(window, "window-state-event", G_CALLBACK(+[](GtkWidget*, GdkEventWindowState* event, gpointer data) -> gboolean {
                                  auto* t{static_cast<DialogTag*>(data)};
                                  if (event->new_window_state & (GDK_WINDOW_STATE_ICONIFIED | GDK_WINDOW_STATE_WITHDRAWN)) t->self->ArmThemed(t->id, false);
                                  return FALSE;
                              }),
                              tag_state, +[](gpointer data, GClosure*) { delete static_cast<DialogTag*>(data); }, GConnectFlags(0));
        // Closed by the desktop (Alt+F4): Cancel.
        auto* tag3{new DialogTag{this, id}};
        g_signal_connect_data(window, "delete-event", G_CALLBACK(+[](GtkWidget*, GdkEvent*, gpointer data) -> gboolean {
                                  auto* t{static_cast<DialogTag*>(data)};
                                  t->self->CancelThemed(t->id);
                                  return TRUE;
                              }),
                              tag3, +[](gpointer data, GClosure*) { delete static_cast<DialogTag*>(data); }, GConnectFlags(0));
        // Its bridge: this web view's only, never the main page's.
        themed->view->bind(std::string{dialog::BRIDGE}, [this, id](std::string call_id, std::string args, void*) {
            OnDialogCall(id, call_id, args);
            // The answer may hold a passphrase: not left in freed memory.
            memory_cleanse(args.data(), args.size());
        }, nullptr);
        // The page says nothing in time: the dialog of the system instead.
        themed->timer = g_timeout_add_full(
            G_PRIORITY_DEFAULT, dialog::READY_TIMEOUT_MS,
            +[](gpointer data) -> gboolean {
                auto* t{static_cast<DialogTag*>(data)};
                if (t->self->m_themed && t->self->m_themed->id == t->id) {
                    t->self->m_themed->timer = 0;
                    if (!t->self->m_themed->session->Shown()) {
                        LogWarning("webui: the dialog window did not show in time: the dialog of the system instead");
                        t->self->Fallback(t->id);
                    }
                }
                return G_SOURCE_REMOVE;
            },
            new DialogTag{this, id}, +[](gpointer data) { delete static_cast<DialogTag*>(data); });
        m_themed = std::move(themed);
        m_themed->view->navigate(std::string{dialog::ORIGIN} + std::string{dialog::PAGE});
        return true;
    }

    struct DialogTag {
        GtkWindowImpl* self;
        uint64_t id;
    };

    void OnDialogCall(uint64_t id, const std::string& call_id, const std::string& args)
    {
        if (!m_themed || m_themed->id != id) return;
        dialog::Session::Reply reply{m_themed->session->Call(args)};
        if (reply.step == dialog::Session::Step::ANSWER) {
            // Not answered: the page goes with its window.
            dialog::Answer answer{m_themed->session->TakeAnswer()};
            Done done{m_themed->done};
            CloseThemed();
            Finish(done, std::move(answer));
            return;
        }
        m_themed->view->resolve(call_id, reply.status, reply.json);
        if (reply.step == dialog::Session::Step::SHOW) ShowThemed();
    }

    void ShowThemed()
    {
        // As large as the page asks, within the work area of the monitor of the main window.
        int max_w{1200}, max_h{800};
        GdkDisplay* display{gtk_widget_get_display(m_window)};
        GdkWindow* main{gtk_widget_get_window(m_window)};
        if (GdkMonitor* monitor{main ? gdk_display_get_monitor_at_window(display, main) : gdk_display_get_primary_monitor(display)}) {
            GdkRectangle area;
            gdk_monitor_get_workarea(monitor, &area);
            max_w = area.width - 32;
            max_h = area.height * 85 / 100;
        }
        const dialog::Size size{m_themed->session->WindowSize(max_w, max_h)};
        GtkWindow* gw{GTK_WINDOW(m_themed->window)};
        gtk_window_set_default_size(gw, size.width, size.height);
        gtk_widget_set_size_request(m_themed->window, size.width, size.height);
        gtk_widget_show_all(m_themed->window);
        gtk_window_present(gw);
        gtk_widget_grab_focus(GTK_WIDGET(m_themed->web));
    }

    /** Tell the page of the dialog window that it shows with the focus (the delay of its button starts), or not (it waits). */
    void ArmThemed(uint64_t id, bool on)
    {
        if (!m_themed || m_themed->id != id || !m_themed->session->Shown() || m_themed->session->Answered()) return;
        m_themed->view->eval(on ? "window.__chainsDialogArm && window.__chainsDialogArm(true)" : "window.__chainsDialogArm && window.__chainsDialogArm(false)");
    }

    /** The dialog window goes (hidden now, destroyed once what its web view has queued ran). */
    void CloseThemed()
    {
        if (!m_themed) return;
        std::unique_ptr<Themed> themed{std::move(m_themed)};
        if (themed->timer) g_source_remove(themed->timer);
        themed->timer = 0;
        gtk_widget_hide(themed->window);
        Themed* raw{themed.release()};
        Post([raw] {
            std::unique_ptr<Themed> gone{raw};
            GtkWidget* window{gone->window};
            gone.reset();
            gtk_widget_destroy(window);
        });
    }

    void CancelThemed(uint64_t id)
    {
        if (!m_themed || m_themed->id != id || m_themed->session->Answered()) return;
        Done done{m_themed->done};
        CloseThemed();
        Finish(done, {});
    }

    /** The dialog window failed before an answer: the same question in the dialog of the system. */
    void Fallback(uint64_t id)
    {
        if (!m_themed || m_themed->id != id || m_themed->session->Answered()) return;
        const dialog::Request request{m_themed->session->request()};
        Done done{m_themed->done};
        CloseThemed();
        OpenNative(request, std::move(done));
    }

    /** The dialog of GTK, modal to the window; Cancel is the default response. */
    void OpenNative(const dialog::Request& request, Done done)
    {
        if (request.rows.size() > dialog::MAX_NATIVE_ROWS) {
            // The dialog of GTK does not scroll: facts off the screen are never accepted unseen.
            LogWarning("webui: no dialog window for a question of %u facts: declined", request.rows.size());
            return Finish(done, {});
        }
        const UniValue page{dialog::PageData(request)};
        const std::string title{page.find_value("title").get_str()};
        const std::string message{dialog::FallbackText(page)};
        SetScheme(dialog::SchemeOf(request.look));
        GtkWidget* box{gtk_message_dialog_new(GTK_WINDOW(m_window), GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT),
                                              request.level == dialog::Level::INFO ? GTK_MESSAGE_INFO : GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", title.c_str())};
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(box), "%s", message.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(box), page.find_value("cancel").get_str().c_str(), GTK_RESPONSE_CANCEL, page.find_value("accept").get_str().c_str(), GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(box), GTK_RESPONSE_CANCEL);
        gtk_window_set_title(GTK_WINDOW(box), title.c_str());
        GtkWidget* entry{nullptr};
        if (request.passphrase) {
            entry = gtk_entry_new();
            gtk_entry_set_visibility(GTK_ENTRY(entry), FALSE);
            gtk_entry_set_input_purpose(GTK_ENTRY(entry), GTK_INPUT_PURPOSE_PASSWORD);
            gtk_entry_set_placeholder_text(GTK_ENTRY(entry), page.find_value("passphraseLabel").get_str().c_str());
            gtk_container_add(GTK_CONTAINER(gtk_message_dialog_get_message_area(GTK_MESSAGE_DIALOG(box))), entry);
        }
        struct Job {
            GtkWindowImpl* self;
            Done done;
            GtkWidget* entry;
        };
        auto* job{new Job{this, std::move(done), entry}};
        g_signal_connect(box, "response", G_CALLBACK(+[](GtkDialog* d, gint response, gpointer data) {
                             std::unique_ptr<Job> job{static_cast<Job*>(data)};
                             dialog::Answer answer;
                             answer.accepted = response == GTK_RESPONSE_ACCEPT;
                             if (answer.accepted && job->entry) {
                                 const char* text{gtk_entry_get_text(GTK_ENTRY(job->entry))};
                                 answer.passphrase.assign(text ? text : "");
                                 // Nothing typed is no answer to a question that needs it.
                                 if (answer.passphrase.empty()) answer.accepted = false;
                                 gtk_entry_set_text(GTK_ENTRY(job->entry), "");
                             }
                             job->self->m_native = nullptr;
                             gtk_widget_destroy(GTK_WIDGET(d));
                             job->self->SetScheme(dialog::Scheme::DESKTOP);
                             job->self->Finish(job->done, std::move(answer));
                         }),
                         job);
        m_native = box;
        m_native_title = title;
        m_native_message = message;
        // The button that accepts, as in the dialog window (audit 7 #7): taken a moment after the dialog shows with the
        // focus, and again after each time it lost it.
        gtk_dialog_set_response_sensitive(GTK_DIALOG(box), GTK_RESPONSE_ACCEPT, FALSE);
        const auto arm_native{+[](GtkWidget* widget, GdkEvent*, gpointer data) -> gboolean {
            auto* self{static_cast<GtkWindowImpl*>(data)};
            if (self->m_native != widget) return FALSE;
            const uint64_t serial{++self->m_native_arm};
            gtk_dialog_set_response_sensitive(GTK_DIALOG(widget), GTK_RESPONSE_ACCEPT, FALSE);
            auto* tag{new DialogTag{self, serial}};
            g_timeout_add_full(G_PRIORITY_DEFAULT, NATIVE_ARM_MS, +[](gpointer d) -> gboolean {
                auto* t{static_cast<DialogTag*>(d)};
                if (t->self->m_native && t->self->m_native_arm == t->id) gtk_dialog_set_response_sensitive(GTK_DIALOG(t->self->m_native), GTK_RESPONSE_ACCEPT, TRUE);
                return G_SOURCE_REMOVE;
            }, tag, +[](gpointer d) { delete static_cast<DialogTag*>(d); });
            return FALSE;
        }};
        const auto disarm_native{+[](GtkWidget* widget, GdkEvent*, gpointer data) -> gboolean {
            auto* self{static_cast<GtkWindowImpl*>(data)};
            if (self->m_native != widget) return FALSE;
            ++self->m_native_arm;
            gtk_dialog_set_response_sensitive(GTK_DIALOG(widget), GTK_RESPONSE_ACCEPT, FALSE);
            return FALSE;
        }};
        g_signal_connect(box, "map-event", G_CALLBACK(arm_native), this);
        g_signal_connect(box, "focus-in-event", G_CALLBACK(arm_native), this);
        g_signal_connect(box, "focus-out-event", G_CALLBACK(disarm_native), this);
        g_signal_connect(box, "unmap-event", G_CALLBACK(disarm_native), this);
        gtk_widget_show_all(box);
    }

    /**
     * Where and when the button that starts a move or a resize was pressed: the last press in the
     * page (the page asks right after it, the button still down), else the pointer now.
     */
    bool Press(int& button, int& x, int& y, guint32& time)
    {
        if (m_last_press && m_last_press->type == GDK_BUTTON_PRESS) {
            const GdkEventButton& press{m_last_press->button};
            button = static_cast<int>(press.button);
            x = static_cast<int>(press.x_root);
            y = static_cast<int>(press.y_root);
            time = press.time;
            return true;
        }
        GdkSeat* seat{gdk_display_get_default_seat(gtk_widget_get_display(m_window))};
        GdkDevice* pointer{seat ? gdk_seat_get_pointer(seat) : nullptr};
        if (!pointer) return false;
        gdk_device_get_position(pointer, nullptr, &x, &y);
        button = 1;
        time = gtk_get_current_event_time();
        return true;
    }

    Callbacks m_callbacks;
    const bool m_devtools;
    GtkWidget* m_window{nullptr};
    std::unique_ptr<webview::webview> m_webview;
    WebKitWebView* m_view{nullptr};
    std::unique_ptr<GtkTray> m_tray;
    int m_width{0};
    int m_height{0};
    bool m_maximized{false};
    bool m_frameless{false};
    GdkEvent* m_last_press{nullptr};
    //! The dialogs of the host: the window shown now (or the dialog of GTK instead), the ones waiting their turn.
    dialog::Pending m_pending;
    std::unique_ptr<Themed> m_themed;
    uint64_t m_dialog_serial{0};
    GtkWidget* m_native{nullptr};
    std::string m_native_title;
    std::string m_native_message;
    //! The delay of the button that accepts of the dialog of GTK: started again at each focus (the timers of older ones do nothing).
    uint64_t m_native_arm{0};
    static constexpr guint NATIVE_ARM_MS{1000};
    //! The last input of the user in the main window (LastUserInput): its serial number, and when (g_get_monotonic_time).
    uint64_t m_input_serial{0};
    gint64 m_input_time{0};
    //! GTK's prefer-dark setting of the desktop, before the app set it for a dialog.
    std::optional<bool> m_desktop_dark;
};

} // namespace

std::unique_ptr<Window> Window::Create(const Options& options, Callbacks callbacks)
{
    return std::make_unique<GtkWindowImpl>(options, std::move(callbacks));
}

} // namespace webui
