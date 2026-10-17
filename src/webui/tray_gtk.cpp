// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The tray icon and the notifications of the desktop on Linux, over D-Bus (GDBus), as
// QSystemTrayIcon and Notificator of chains-qt do them.

#include <webui/tray_gtk.h>
#include <webui/chainconfig.h>
#include <webui/icontint.h>

#include <logging.h>
#include <webui/assets.h>

#include <gtk/gtk.h>
#include <unistd.h>

#include <atomic>
#include <span>
#include <string>
#include <vector>

namespace webui {
namespace {

constexpr const char* ITEM_PATH{"/StatusNotifierItem"};
constexpr const char* MENU_PATH{"/MenuBar"};
constexpr const char* WATCHER{"org.kde.StatusNotifierWatcher"};
constexpr const char* WATCHER_PATH{"/StatusNotifierWatcher"};

constexpr const char* ITEM_XML{R"xml(<node>
 <interface name="org.kde.StatusNotifierItem">
  <property name="Category" type="s" access="read"/>
  <property name="Id" type="s" access="read"/>
  <property name="Title" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="WindowId" type="i" access="read"/>
  <property name="IconName" type="s" access="read"/>
  <property name="IconPixmap" type="a(iiay)" access="read"/>
  <property name="OverlayIconName" type="s" access="read"/>
  <property name="AttentionIconName" type="s" access="read"/>
  <property name="ToolTip" type="(sa(iiay)ss)" access="read"/>
  <property name="ItemIsMenu" type="b" access="read"/>
  <property name="Menu" type="o" access="read"/>
  <method name="ContextMenu"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="Activate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="SecondaryActivate"><arg name="x" type="i" direction="in"/><arg name="y" type="i" direction="in"/></method>
  <method name="Scroll"><arg name="delta" type="i" direction="in"/><arg name="orientation" type="s" direction="in"/></method>
  <signal name="NewTitle"/>
  <signal name="NewIcon"/>
  <signal name="NewToolTip"/>
  <signal name="NewStatus"><arg name="status" type="s"/></signal>
 </interface>
</node>)xml"};

constexpr const char* MENU_XML{R"xml(<node>
 <interface name="com.canonical.dbusmenu">
  <property name="Version" type="u" access="read"/>
  <property name="TextDirection" type="s" access="read"/>
  <property name="Status" type="s" access="read"/>
  <property name="IconThemePath" type="as" access="read"/>
  <method name="GetLayout">
   <arg type="i" name="parentId" direction="in"/><arg type="i" name="recursionDepth" direction="in"/><arg type="as" name="propertyNames" direction="in"/>
   <arg type="u" name="revision" direction="out"/><arg type="(ia{sv}av)" name="layout" direction="out"/>
  </method>
  <method name="GetGroupProperties">
   <arg type="ai" name="ids" direction="in"/><arg type="as" name="propertyNames" direction="in"/><arg type="a(ia{sv})" name="properties" direction="out"/>
  </method>
  <method name="GetProperty"><arg type="i" name="id" direction="in"/><arg type="s" name="name" direction="in"/><arg type="v" name="value" direction="out"/></method>
  <method name="Event"><arg type="i" name="id" direction="in"/><arg type="s" name="eventId" direction="in"/><arg type="v" name="data" direction="in"/><arg type="u" name="timestamp" direction="in"/></method>
  <method name="EventGroup"><arg type="a(isvu)" name="events" direction="in"/><arg type="ai" name="idErrors" direction="out"/></method>
  <method name="AboutToShow"><arg type="i" name="id" direction="in"/><arg type="b" name="needUpdate" direction="out"/></method>
  <method name="AboutToShowGroup"><arg type="ai" name="ids" direction="in"/><arg type="ai" name="updatesNeeded" direction="out"/><arg type="ai" name="idErrors" direction="out"/></method>
  <signal name="ItemsPropertiesUpdated"><arg type="a(ia{sv})" name="updatedProps"/><arg type="a(ias)" name="removedProps"/></signal>
  <signal name="LayoutUpdated"><arg type="u" name="revision"/><arg type="i" name="parent"/></signal>
  <signal name="ItemActivationRequested"><arg type="i" name="id"/><arg type="u" name="timestamp"/></signal>
 </interface>
</node>)xml"};

/** The pixels of a picture as StatusNotifierItem wants them: ARGB32 in network byte order. */
GVariant* Pixmaps()
{
    GVariantBuilder pixmaps;
    g_variant_builder_init(&pixmaps, G_VARIANT_TYPE("a(iiay)"));
    for (const int size : {22, 32, 64}) {
        GdkPixbuf* icon{AppIcon(size)};
        if (!icon) continue;
        GdkPixbuf* rgba{gdk_pixbuf_get_has_alpha(icon) ? icon : gdk_pixbuf_add_alpha(icon, FALSE, 0, 0, 0)};
        const int width{gdk_pixbuf_get_width(rgba)}, height{gdk_pixbuf_get_height(rgba)}, stride{gdk_pixbuf_get_rowstride(rgba)};
        const guchar* pixels{gdk_pixbuf_read_pixels(rgba)};
        std::vector<guchar> argb(static_cast<size_t>(width) * height * 4);
        for (int y{0}; y < height; ++y) {
            for (int x{0}; x < width; ++x) {
                const guchar* p{pixels + y * stride + x * 4};
                guchar* q{&argb[(static_cast<size_t>(y) * width + x) * 4]};
                q[0] = p[3];
                q[1] = p[0];
                q[2] = p[1];
                q[3] = p[2];
            }
        }
        GVariant* bytes{g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, argb.data(), argb.size(), 1)};
        g_variant_builder_add(&pixmaps, "(ii@ay)", width, height, bytes);
        if (rgba != icon) g_object_unref(rgba);
        g_object_unref(icon);
    }
    return g_variant_builder_end(&pixmaps);
}
} // namespace

namespace {
std::atomic<int> g_hue_shift{0};
std::atomic<int> g_saturation_reduction{0};
} // namespace

void SetIconTint(IconTint tint)
{
    g_hue_shift = tint.hue_shift;
    g_saturation_reduction = tint.saturation_reduction;
}

GdkPixbuf* AppIcon(int size)
{
    const Asset* asset{FindAsset(BuiltinAssets(), "/icon-256.png")};
    if (!asset) return nullptr;
    GInputStream* stream{g_memory_input_stream_new_from_data(asset->data.data(), asset->data.size(), nullptr)};
    GdkPixbuf* icon{gdk_pixbuf_new_from_stream_at_scale(stream, size, size, TRUE, nullptr, nullptr)};
    g_object_unref(stream);
    // The icon of the network (NetworkStyle): testnet, signet and regtest have colours of their own.
    const IconTint tint{g_hue_shift.load(), g_saturation_reduction.load()};
    if (icon && !tint.None() && gdk_pixbuf_get_bits_per_sample(icon) == 8) {
        guint length{0};
        guchar* pixels{gdk_pixbuf_get_pixels_with_length(icon, &length)};
        TintPixels(std::span<unsigned char>{pixels, length}, static_cast<size_t>(gdk_pixbuf_get_n_channels(icon)), static_cast<size_t>(gdk_pixbuf_get_width(icon)),
                   static_cast<size_t>(gdk_pixbuf_get_height(icon)), static_cast<size_t>(gdk_pixbuf_get_rowstride(icon)), tint);
    }
    return icon;
}

struct GtkTray::Impl {
    std::function<void()> activated;
    std::function<void(const std::string&)> menu_chosen;
    Window::Tray tray;

    GDBusConnection* bus{nullptr};
    GDBusNodeInfo* item_info{nullptr};
    GDBusNodeInfo* menu_info{nullptr};
    guint item_id{0}, menu_id{0}, owner_id{0}, watch_id{0};
    std::string service;
    bool watcher{false};
    bool registered{false};
    guint32 revision{1};
    //! The older tray (XEmbed), when the desktop has no StatusNotifierWatcher.
    GtkStatusIcon* status_icon{nullptr};
    GtkWidget* popup{nullptr};

    ~Impl()
    {
        if (watch_id) g_bus_unwatch_name(watch_id);
        if (bus) {
            if (item_id) g_dbus_connection_unregister_object(bus, item_id);
            if (menu_id) g_dbus_connection_unregister_object(bus, menu_id);
        }
        if (owner_id) g_bus_unown_name(owner_id);
        if (item_info) g_dbus_node_info_unref(item_info);
        if (menu_info) g_dbus_node_info_unref(menu_info);
        if (popup) gtk_widget_destroy(popup);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (status_icon) g_object_unref(status_icon);
        G_GNUC_END_IGNORE_DEPRECATIONS
        if (bus) g_object_unref(bus);
    }

    void Choose(const std::string& id)
    {
        if (menu_chosen) menu_chosen(id);
    }

    // --- StatusNotifierItem ---

    void Register()
    {
        if (!bus || !watcher || !tray.visible || registered) return;
        registered = true;
        g_dbus_connection_call(bus, WATCHER, WATCHER_PATH, WATCHER, "RegisterStatusNotifierItem", g_variant_new("(s)", service.c_str()),
                               nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
                               +[](GObject* source, GAsyncResult* result, gpointer) {
                                   GError* error{nullptr};
                                   GVariant* reply{g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &error)};
                                   if (reply) g_variant_unref(reply);
                                   if (error) {
                                       LogWarning("webui: the tray did not take the icon: %s", error->message);
                                       g_error_free(error);
                                   }
                               },
                               nullptr);
    }

    void Signal(const char* path, const char* interface, const char* name, GVariant* params)
    {
        if (bus) g_dbus_connection_emit_signal(bus, nullptr, path, interface, name, params, nullptr);
        else if (params) g_variant_unref(g_variant_ref_sink(params));
    }

    GVariant* ItemProperty(const std::string& name)
    {
        if (name == "Category") return g_variant_new_string("ApplicationStatus");
        if (name == "Id") return g_variant_new_string(std::string{chain::APP}.c_str());
        if (name == "Title") return g_variant_new_string(tray.tooltip.c_str());
        if (name == "Status") return g_variant_new_string(tray.visible ? "Active" : "Passive");
        if (name == "WindowId") return g_variant_new_int32(0);
        if (name == "IconName") return g_variant_new_string("");
        if (name == "OverlayIconName" || name == "AttentionIconName") return g_variant_new_string("");
        if (name == "IconPixmap") return Pixmaps();
        if (name == "ToolTip") {
            GVariantBuilder empty;
            g_variant_builder_init(&empty, G_VARIANT_TYPE("a(iiay)"));
            return g_variant_new("(sa(iiay)ss)", "", &empty, tray.tooltip.c_str(), "");
        }
        if (name == "ItemIsMenu") return g_variant_new_boolean(FALSE);
        if (name == "Menu") return g_variant_new_object_path(MENU_PATH);
        return nullptr;
    }

    // --- The menu (com.canonical.dbusmenu): the root is 0, the items 1..n in order ---

    GVariant* ItemProperties(int id)
    {
        GVariantBuilder props;
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        if (id == 0) {
            g_variant_builder_add(&props, "{sv}", "children-display", g_variant_new_string("submenu"));
        } else if (id >= 1 && static_cast<size_t>(id) <= tray.menu.size()) {
            const Window::TrayItem& item{tray.menu[id - 1]};
            if (item.separator) {
                g_variant_builder_add(&props, "{sv}", "type", g_variant_new_string("separator"));
            } else {
                g_variant_builder_add(&props, "{sv}", "label", g_variant_new_string(item.label.c_str()));
                g_variant_builder_add(&props, "{sv}", "enabled", g_variant_new_boolean(item.enabled));
            }
            g_variant_builder_add(&props, "{sv}", "visible", g_variant_new_boolean(TRUE));
        }
        return g_variant_builder_end(&props);
    }

    GVariant* Layout(int parent)
    {
        GVariantBuilder children;
        g_variant_builder_init(&children, G_VARIANT_TYPE("av"));
        if (parent == 0) {
            for (size_t i{0}; i < tray.menu.size(); ++i) {
                GVariantBuilder none;
                g_variant_builder_init(&none, G_VARIANT_TYPE("av"));
                const int id{static_cast<int>(i) + 1};
                g_variant_builder_add(&children, "v", g_variant_new("(i@a{sv}av)", id, ItemProperties(id), &none));
            }
        }
        return g_variant_new("(i@a{sv}av)", parent, ItemProperties(parent), &children);
    }

    void MenuEvent(int id, const std::string& event)
    {
        if (event != "clicked" || id < 1 || static_cast<size_t>(id) > tray.menu.size()) return;
        const Window::TrayItem& item{tray.menu[id - 1]};
        if (!item.separator && item.enabled) Choose(item.id);
    }

    // --- XEmbed fallback ---

    void UpdateStatusIcon()
    {
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        if (watcher || !tray.visible) {
            if (status_icon) gtk_status_icon_set_visible(status_icon, FALSE);
            return;
        }
        if (!status_icon) {
            GdkPixbuf* icon{AppIcon(64)};
            status_icon = icon ? gtk_status_icon_new_from_pixbuf(icon) : gtk_status_icon_new_from_icon_name(std::string{chain::STEM}.c_str());
            if (icon) g_object_unref(icon);
            g_signal_connect(status_icon, "activate", G_CALLBACK(+[](GtkStatusIcon*, gpointer self) {
                                 auto* impl{static_cast<Impl*>(self)};
                                 if (impl->activated) impl->activated();
                             }),
                             this);
            g_signal_connect(status_icon, "popup-menu", G_CALLBACK(+[](GtkStatusIcon* icon, guint button, guint time, gpointer self) {
                                 static_cast<Impl*>(self)->Popup(icon, button, time);
                             }),
                             this);
        }
        gtk_status_icon_set_tooltip_text(status_icon, tray.tooltip.c_str());
        gtk_status_icon_set_visible(status_icon, TRUE);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }

    void Popup(GtkStatusIcon* icon, guint button, guint time)
    {
        if (popup) gtk_widget_destroy(popup);
        popup = gtk_menu_new();
        for (const Window::TrayItem& item : tray.menu) {
            GtkWidget* entry{item.separator ? gtk_separator_menu_item_new() : gtk_menu_item_new_with_label(item.label.c_str())};
            gtk_widget_set_sensitive(entry, item.enabled);
            if (!item.separator) {
                g_object_set_data_full(G_OBJECT(entry), "chains-id", g_strdup(item.id.c_str()), g_free);
                g_signal_connect(entry, "activate", G_CALLBACK(+[](GtkMenuItem* menu_item, gpointer self) {
                                     const char* id{static_cast<const char*>(g_object_get_data(G_OBJECT(menu_item), "chains-id"))};
                                     if (id) static_cast<Impl*>(self)->Choose(id);
                                 }),
                                 this);
            }
            gtk_menu_shell_append(GTK_MENU_SHELL(popup), entry);
        }
        gtk_widget_show_all(popup);
        G_GNUC_BEGIN_IGNORE_DEPRECATIONS
        gtk_menu_popup(GTK_MENU(popup), nullptr, nullptr, gtk_status_icon_position_menu, icon, button, time);
        G_GNUC_END_IGNORE_DEPRECATIONS
    }
};

namespace {
void ItemMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant*, GDBusMethodInvocation* invocation, gpointer data)
{
    auto* impl{static_cast<GtkTray::Impl*>(data)};
    const std::string name{method};
    if (name == "Activate" || name == "SecondaryActivate") {
        if (impl->activated) impl->activated();
    }
    // ContextMenu and Scroll: the desktop shows the menu itself (Menu); nothing to scroll.
    g_dbus_method_invocation_return_value(invocation, nullptr);
}

GVariant* ItemGet(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError** error, gpointer data)
{
    GVariant* value{static_cast<GtkTray::Impl*>(data)->ItemProperty(property)};
    if (!value) g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "No property %s", property);
    return value;
}

void MenuMethod(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* method, GVariant* params, GDBusMethodInvocation* invocation, gpointer data)
{
    auto* impl{static_cast<GtkTray::Impl*>(data)};
    const std::string name{method};
    if (name == "GetLayout") {
        gint32 parent, depth;
        GVariant* names;
        g_variant_get(params, "(ii@as)", &parent, &depth, &names);
        g_variant_unref(names);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(u@(ia{sv}av))", impl->revision, impl->Layout(parent)));
    } else if (name == "GetGroupProperties") {
        GVariantIter* ids;
        GVariant* names;
        g_variant_get(params, "(ai@as)", &ids, &names);
        g_variant_unref(names);
        GVariantBuilder out;
        g_variant_builder_init(&out, G_VARIANT_TYPE("a(ia{sv})"));
        gint32 id;
        while (g_variant_iter_next(ids, "i", &id)) g_variant_builder_add(&out, "(i@a{sv})", id, impl->ItemProperties(id));
        g_variant_iter_free(ids);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(a(ia{sv}))", &out));
    } else if (name == "GetProperty") {
        gint32 id;
        const gchar* property;
        g_variant_get(params, "(i&s)", &id, &property);
        GVariant* props{impl->ItemProperties(id)};
        GVariant* value{g_variant_lookup_value(props, property, nullptr)};
        g_variant_unref(g_variant_ref_sink(props));
        if (!value) value = g_variant_new_string("");
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(v)", value));
    } else if (name == "Event") {
        gint32 id;
        const gchar* event;
        GVariant* event_data;
        guint32 timestamp;
        g_variant_get(params, "(i&svu)", &id, &event, &event_data, &timestamp);
        const std::string event_name{event};
        g_variant_unref(event_data);
        g_dbus_method_invocation_return_value(invocation, nullptr);
        impl->MenuEvent(id, event_name);
    } else if (name == "EventGroup") {
        GVariantIter* events;
        g_variant_get(params, "(a(isvu))", &events);
        gint32 id;
        const gchar* event;
        GVariant* event_data;
        guint32 timestamp;
        std::vector<std::pair<int, std::string>> chosen;
        while (g_variant_iter_next(events, "(i&svu)", &id, &event, &event_data, &timestamp)) {
            chosen.emplace_back(id, event);
            g_variant_unref(event_data);
        }
        g_variant_iter_free(events);
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
        for (const auto& [item, e] : chosen) impl->MenuEvent(item, e);
    } else if (name == "AboutToShow") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(b)", FALSE));
    } else if (name == "AboutToShowGroup") {
        g_dbus_method_invocation_return_value(invocation, g_variant_new("(@ai@ai)", g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0), g_variant_new_array(G_VARIANT_TYPE_INT32, nullptr, 0)));
    } else {
        g_dbus_method_invocation_return_error(invocation, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "No method %s", method);
    }
}

GVariant* MenuGet(GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar* property, GError** error, gpointer)
{
    const std::string name{property};
    if (name == "Version") return g_variant_new_uint32(3);
    if (name == "TextDirection") return g_variant_new_string("ltr");
    if (name == "Status") return g_variant_new_string("normal");
    if (name == "IconThemePath") return g_variant_new_strv(nullptr, 0);
    g_set_error(error, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_PROPERTY, "No property %s", property);
    return nullptr;
}

const GDBusInterfaceVTable ITEM_VTABLE{ItemMethod, ItemGet, nullptr, {}};
const GDBusInterfaceVTable MENU_VTABLE{MenuMethod, MenuGet, nullptr, {}};
} // namespace

GtkTray::GtkTray(std::function<void()> activated, std::function<void(const std::string&)> menu) : m_impl{std::make_unique<Impl>()}
{
    m_impl->activated = std::move(activated);
    m_impl->menu_chosen = std::move(menu);
    GError* error{nullptr};
    m_impl->bus = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error);
    if (!m_impl->bus) {
        LogDebug(BCLog::RPC, "webui: no session bus for the tray: %s\n", error ? error->message : "");
        if (error) g_error_free(error);
        return;
    }
    m_impl->item_info = g_dbus_node_info_new_for_xml(ITEM_XML, nullptr);
    m_impl->menu_info = g_dbus_node_info_new_for_xml(MENU_XML, nullptr);
    m_impl->item_id = g_dbus_connection_register_object(m_impl->bus, ITEM_PATH, m_impl->item_info->interfaces[0], &ITEM_VTABLE, m_impl.get(), nullptr, nullptr);
    m_impl->menu_id = g_dbus_connection_register_object(m_impl->bus, MENU_PATH, m_impl->menu_info->interfaces[0], &MENU_VTABLE, m_impl.get(), nullptr, nullptr);
    // The name of the item, as the StatusNotifierItem specification has it.
    m_impl->service = "org.kde.StatusNotifierItem-" + std::to_string(getpid()) + "-1";
    m_impl->owner_id = g_bus_own_name_on_connection(m_impl->bus, m_impl->service.c_str(), G_BUS_NAME_OWNER_FLAGS_NONE, nullptr, nullptr, nullptr, nullptr);
    // The tray of the desktop may come (and go) at any time: a panel restarts.
    m_impl->watch_id = g_bus_watch_name_on_connection(
        m_impl->bus, WATCHER, G_BUS_NAME_WATCHER_FLAGS_NONE,
        +[](GDBusConnection*, const gchar*, const gchar*, gpointer data) {
            auto* impl{static_cast<Impl*>(data)};
            impl->watcher = true;
            impl->registered = false;
            impl->UpdateStatusIcon();
            impl->Register();
        },
        +[](GDBusConnection*, const gchar*, gpointer data) {
            auto* impl{static_cast<Impl*>(data)};
            impl->watcher = false;
            impl->registered = false;
            impl->UpdateStatusIcon();
        },
        m_impl.get(), nullptr);
}

GtkTray::~GtkTray() = default;

void GtkTray::Set(const Window::Tray& tray)
{
    const bool was_visible{m_impl->tray.visible};
    m_impl->tray = tray;
    m_impl->UpdateStatusIcon();
    if (!m_impl->bus) return;
    if (tray.visible && !was_visible) m_impl->registered = false;
    m_impl->Register();
    m_impl->Signal(ITEM_PATH, "org.kde.StatusNotifierItem", "NewToolTip", nullptr);
    m_impl->Signal(ITEM_PATH, "org.kde.StatusNotifierItem", "NewTitle", nullptr);
    m_impl->Signal(ITEM_PATH, "org.kde.StatusNotifierItem", "NewStatus", g_variant_new("(s)", tray.visible ? "Active" : "Passive"));
    m_impl->Signal(MENU_PATH, "com.canonical.dbusmenu", "LayoutUpdated", g_variant_new("(ui)", ++m_impl->revision, 0));
}

bool GtkTray::Available() const
{
    if (m_impl->watcher) return true;
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    // An XEmbed tray: the icon is in it once shown.
    return m_impl->status_icon && gtk_status_icon_is_embedded(m_impl->status_icon);
    G_GNUC_END_IGNORE_DEPRECATIONS
}

void DesktopNotify(const std::string& title, const std::string& body, const std::string& level)
{
    GError* error{nullptr};
    GDBusConnection* bus{g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &error)};
    if (!bus) {
        LogDebug(BCLog::RPC, "webui: no session bus for notifications: %s\n", error ? error->message : "");
        if (error) g_error_free(error);
        return;
    }
    GVariantBuilder actions;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    // Urgency: low 0, normal 1, critical 2 (as Notificator).
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(level == "critical" ? 2 : 1));
    g_variant_builder_add(&hints, "{sv}", "desktop-entry", g_variant_new_string(std::string{chain::APP}.c_str()));
    if (GdkPixbuf* icon{AppIcon(64)}) {
        GdkPixbuf* rgba{gdk_pixbuf_get_has_alpha(icon) ? icon : gdk_pixbuf_add_alpha(icon, FALSE, 0, 0, 0)};
        const int width{gdk_pixbuf_get_width(rgba)}, height{gdk_pixbuf_get_height(rgba)}, stride{gdk_pixbuf_get_rowstride(rgba)};
        const gsize size{static_cast<gsize>(stride) * (height - 1) + width * 4};
        GVariant* pixels{g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE, gdk_pixbuf_read_pixels(rgba), size, 1)};
        g_variant_builder_add(&hints, "{sv}", "image-data", g_variant_new("(iiibii@ay)", width, height, stride, TRUE, 8, 4, pixels));
        if (rgba != icon) g_object_unref(rgba);
        g_object_unref(icon);
    }
    // The body may be read as markup by the notification server (body-markup): written as text, so that a label or text
    // of the chain in it cannot make a link, load an image or hide the rest (audit 7 #16). The summary is plain text.
    gchar* escaped{g_markup_escape_text(body.c_str(), -1)};
    g_dbus_connection_call(bus, "org.freedesktop.Notifications", "/org/freedesktop/Notifications", "org.freedesktop.Notifications", "Notify",
                           g_variant_new("(susssasa{sv}i)", std::string{chain::NAME}.c_str(), 0u, "", title.c_str(), escaped, &actions, &hints, -1),
                           nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr,
                           +[](GObject* source, GAsyncResult* result, gpointer) {
                               GError* call_error{nullptr};
                               GVariant* reply{g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), result, &call_error)};
                               if (reply) g_variant_unref(reply);
                               if (call_error) {
                                   LogDebug(BCLog::RPC, "webui: no notification shown: %s\n", call_error->message);
                                   g_error_free(call_error);
                               }
                           },
                           nullptr);
    g_free(escaped);
    g_object_unref(bus);
}

} // namespace webui
