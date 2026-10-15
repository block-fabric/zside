#!/usr/bin/env python3
# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""A pretend desktop on a private session bus, for the end-to-end tests of chains-app.

It owns the names a desktop has: the notification service (org.freedesktop.Notifications)
and the tray (org.kde.StatusNotifierWatcher). It writes what chains-app does with them to a
JSON file: the notifications sent, the tray icon registered, its properties and its menu.
When the icon registers, it reads its menu. When the app sends the notification
"e2e: click tray", it clicks an item of the menu (--click, by label), as a user would.

    fake_desktop.py --out <file.json> [--click "Node window"]

Run with DBUS_SESSION_BUS_ADDRESS set to the private bus of the test.
"""

import argparse
import json
import sys

import gi

gi.require_version("Gio", "2.0")
from gi.repository import Gio, GLib  # noqa: E402

NOTIFY_XML = """<node><interface name="org.freedesktop.Notifications">
 <method name="Notify"><arg type="s" direction="in"/><arg type="u" direction="in"/><arg type="s" direction="in"/>
  <arg type="s" direction="in"/><arg type="s" direction="in"/><arg type="as" direction="in"/><arg type="a{sv}" direction="in"/>
  <arg type="i" direction="in"/><arg type="u" direction="out"/></method>
 <method name="GetCapabilities"><arg type="as" direction="out"/></method>
 <method name="GetServerInformation"><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/><arg type="s" direction="out"/></method>
</interface></node>"""

WATCHER_XML = """<node><interface name="org.kde.StatusNotifierWatcher">
 <method name="RegisterStatusNotifierItem"><arg type="s" direction="in"/></method>
 <method name="RegisterStatusNotifierHost"><arg type="s" direction="in"/></method>
 <property name="RegisteredStatusNotifierItems" type="as" access="read"/>
 <property name="IsStatusNotifierHostRegistered" type="b" access="read"/>
 <property name="ProtocolVersion" type="i" access="read"/>
</interface></node>"""


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", required=True)
    parser.add_argument("--click", default="")
    args = parser.parse_args()

    record = {"notifications": [], "items": [], "properties": {}, "menu": [], "clicked": None, "errors": []}

    def save():
        with open(args.out, "w", encoding="utf-8") as f:
            json.dump(record, f, indent=1)

    bus = Gio.bus_get_sync(Gio.BusType.SESSION, None)
    notify_info = Gio.DBusNodeInfo.new_for_xml(NOTIFY_XML)
    watcher_info = Gio.DBusNodeInfo.new_for_xml(WATCHER_XML)
    counter = [0]
    service = [None]

    def click():
        target = next((i for i in record["menu"] if i["label"] == args.click), None)
        if target is None:
            record["errors"].append(f"no menu item {args.click}")
        else:
            try:
                bus.call_sync(service[0], "/MenuBar", "com.canonical.dbusmenu", "Event",
                              GLib.Variant("(isvu)", (target["id"], "clicked", GLib.Variant("s", ""), 0)), None, Gio.DBusCallFlags.NONE, 5000, None)
                record["clicked"] = args.click
            except GLib.Error as e:
                record["errors"].append(str(e))
        save()
        return False

    def on_notify(conn, sender, path, iface, method, params, invocation):
        if method == "Notify":
            app, _, _, summary, body, _, hints, _ = params.unpack()
            counter[0] += 1
            record["notifications"].append({"app": app, "summary": summary, "body": body, "urgency": hints.get("urgency"), "image": "image-data" in hints})
            save()
            if summary == "e2e: click tray" and service[0]:
                GLib.timeout_add(300, click)
            invocation.return_value(GLib.Variant("(u)", (counter[0],)))
        elif method == "GetCapabilities":
            invocation.return_value(GLib.Variant("(as)", (["body"],)))
        else:
            invocation.return_value(GLib.Variant("(ssss)", ("fake", "chains", "1", "1.2")))

    def read_item(service):
        def get(prop):
            v = bus.call_sync(service, "/StatusNotifierItem", "org.freedesktop.DBus.Properties", "Get",
                              GLib.Variant("(ss)", ("org.kde.StatusNotifierItem", prop)), None, Gio.DBusCallFlags.NONE, 5000, None)
            return v.unpack()[0]
        try:
            record["properties"] = {
                "Id": get("Id"),
                "Status": get("Status"),
                "Title": get("Title"),
                "Menu": get("Menu"),
                "IconPixmapSizes": [[w, h] for (w, h, _) in get("IconPixmap")],
            }
            layout = bus.call_sync(service, "/MenuBar", "com.canonical.dbusmenu", "GetLayout",
                                   GLib.Variant("(iias)", (0, -1, [])), None, Gio.DBusCallFlags.NONE, 5000, None).unpack()
            items = []
            for child in layout[1][2]:
                item_id, props, _ = child
                items.append({"id": item_id, "label": props.get("label", ""), "type": props.get("type", ""), "enabled": props.get("enabled", True)})
            record["menu"] = items
        except Exception as e:  # noqa: BLE001
            record["errors"].append(repr(e))
        save()


    def on_watcher(conn, sender, path, iface, method, params, invocation):
        if method == "RegisterStatusNotifierItem":
            item = params.unpack()[0]
            # A service name, or an object path on the sender's connection.
            name = item if not item.startswith("/") else sender
            record["items"].append(item)
            save()
            invocation.return_value(None)
            service[0] = name
            GLib.timeout_add(500, lambda: (read_item(name), False)[1])
        else:
            invocation.return_value(None)

    def get_watcher(conn, sender, path, iface, prop):
        if prop == "RegisteredStatusNotifierItems":
            return GLib.Variant("as", record["items"])
        if prop == "IsStatusNotifierHostRegistered":
            return GLib.Variant("b", True)
        return GLib.Variant("i", 0)

    bus.register_object("/org/freedesktop/Notifications", notify_info.interfaces[0], on_notify, None, None)
    bus.register_object("/StatusNotifierWatcher", watcher_info.interfaces[0], on_watcher, get_watcher, None)
    Gio.bus_own_name_on_connection(bus, "org.freedesktop.Notifications", Gio.BusNameOwnerFlags.NONE, None, None)
    Gio.bus_own_name_on_connection(bus, "org.kde.StatusNotifierWatcher", Gio.BusNameOwnerFlags.NONE, None, None)
    save()
    print("fake desktop ready", flush=True)
    GLib.MainLoop().run()


if __name__ == "__main__":
    sys.exit(main())
