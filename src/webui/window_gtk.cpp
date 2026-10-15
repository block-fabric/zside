// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The window on Linux and the BSDs: GTK 3 with WebKitGTK 4.1, through webview/webview.
// The interface is served from the files built into the program, under the
// scheme chains-app://, with its content security policy as a header.

#include <webui/assets.h>
#include <webui/chainconfig.h>
#include <webui/tray_gtk.h>
#include <support/cleanse.h>
#include <webui/window.h>

#include <logging.h>

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

void ServeAsset(WebKitURISchemeRequest* request, gpointer)
{
    const char* path{webkit_uri_scheme_request_get_path(request)};
    const char* uri{webkit_uri_scheme_request_get_uri(request)};
    // Only the files of the app itself, from its own origin.
    const Asset* asset{nullptr};
    if (uri && std::string_view{uri}.starts_with(std::string{ORIGIN} + "/")) {
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
    soup_message_headers_append(headers, "Content-Security-Policy", ContentSecurityPolicy().c_str());
    soup_message_headers_append(headers, "X-Content-Type-Options", "nosniff");
    soup_message_headers_append(headers, "Cache-Control", "no-store");
    webkit_uri_scheme_response_set_http_headers(response, headers);
    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
    g_object_unref(stream);
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

        // The last press of a button in the page: the moves and resizes the page starts begin there.
        g_signal_connect(m_view, "button-press-event", G_CALLBACK(+[](GtkWidget*, GdkEventButton* event, gpointer self) -> gboolean {
                             auto* w{static_cast<GtkWindowImpl*>(self)};
                             if (w->m_last_press) gdk_event_free(w->m_last_press);
                             w->m_last_press = gdk_event_copy(reinterpret_cast<GdkEvent*>(event));
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

    void Snapshot(const std::string& png_path, std::function<void(bool ok)> done) override
    {
        struct Job {
            std::string path;
            std::function<void(bool)> done;
        };
        auto* job{new Job{png_path, std::move(done)}};
        webkit_web_view_get_snapshot(
            m_view, WEBKIT_SNAPSHOT_REGION_VISIBLE, WEBKIT_SNAPSHOT_OPTIONS_NONE, nullptr,
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

    void Confirm(const ConfirmDialog& dialog, std::function<void(bool)> done) override
    {
        // A dialog of GTK, modal to the window; Cancel is the default response.
        GtkWidget* box{gtk_message_dialog_new(GTK_WINDOW(m_window), GtkDialogFlags(GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT), GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s", dialog.title.c_str())};
        gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(box), "%s", dialog.message.c_str());
        gtk_dialog_add_buttons(GTK_DIALOG(box), "_Cancel", GTK_RESPONSE_CANCEL, dialog.accept.c_str(), GTK_RESPONSE_ACCEPT, nullptr);
        gtk_dialog_set_default_response(GTK_DIALOG(box), GTK_RESPONSE_CANCEL);
        gtk_window_set_title(GTK_WINDOW(box), dialog.title.c_str());
        auto* job{new std::function<void(bool)>{std::move(done)}};
        g_signal_connect(box, "response", G_CALLBACK(+[](GtkDialog* d, gint response, gpointer data) {
                             std::unique_ptr<std::function<void(bool)>> job{static_cast<std::function<void(bool)>*>(data)};
                             gtk_widget_destroy(GTK_WIDGET(d));
                             (*job)(response == GTK_RESPONSE_ACCEPT);
                         }),
                         job);
        gtk_widget_show_all(box);
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
        };
        auto* job{new Job{std::move(done), options.save, suffix}};
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
                             job->done(path);
                         }),
                         job);
        gtk_native_dialog_show(GTK_NATIVE_DIALOG(chooser));
    }

private:
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
};

} // namespace

std::unique_ptr<Window> Window::Create(const Options& options, Callbacks callbacks)
{
    return std::make_unique<GtkWindowImpl>(options, std::move(callbacks));
}

} // namespace webui
