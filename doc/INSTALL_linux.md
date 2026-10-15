Bitcoin Core
=============

Below are notes on installing Bitcoin Core software on Linux systems.

General Runtime Requirements
----------------------------

Bitcoin Core requires glibc (GNU C Library) 2.31 or newer.

GUI Runtime Requirements
------------------------

The GUI executable, `chains-app`, shows an HTML interface in the web view of the system
(WebKitGTK, on X11 or Wayland). Its runtime library dependencies are as follows:
- GTK 3
- WebKitGTK, API version 4.1

On Debian, Ubuntu, or their derivatives, you can run the following command to ensure all dependencies are installed:
```sh
sudo apt install libgtk-3-0 libwebkit2gtk-4.1-0
```

On Fedora, run:
```sh
sudo dnf install gtk3 webkit2gtk4.1
```

For other systems, please consult their documentation.
