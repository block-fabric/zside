// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_TRAY_GTK_H
#define BITCOIN_WEBUI_TRAY_GTK_H

#include <webui/icontint.h>
#include <webui/window.h>

#include <functional>
#include <memory>
#include <string>

typedef struct _GdkPixbuf GdkPixbuf;

namespace webui {

/**
 * The icon of the app in the tray of a Linux desktop, as QSystemTrayIcon does it:
 * a StatusNotifierItem on the session bus, with its menu as com.canonical.dbusmenu,
 * registered with the StatusNotifierWatcher of the desktop when there is one; else the
 * older XEmbed tray icon of GTK. UI thread only.
 */
class GtkTray
{
public:
    GtkTray(std::function<void()> activated, std::function<void(const std::string&)> menu);
    ~GtkTray();
    GtkTray(const GtkTray&) = delete;
    GtkTray& operator=(const GtkTray&) = delete;

    void Set(const Window::Tray& tray);
    bool Available() const;

    struct Impl;

private:
    std::unique_ptr<Impl> m_impl;
};

/** A notification of the desktop over D-Bus (org.freedesktop.Notifications), as Notificator does. UI thread. */
void DesktopNotify(const std::string& title, const std::string& body, const std::string& level);

/** The icon of the app (built into the program), or null. Owned by the caller. */
GdkPixbuf* AppIcon(int size);

/** The tint of the icon of the app from now on (the network: NetworkStyle). */
void SetIconTint(IconTint tint);

} // namespace webui

#endif // BITCOIN_WEBUI_TRAY_GTK_H
