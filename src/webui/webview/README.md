# webview (vendored)

`webview.h` is the single-header amalgamation of
[webview/webview](https://github.com/webview/webview) release **0.12.0**
(commit 3ab4b5d722438fc8a13e6ca830c5e2372d19a01d), MIT licence (see `LICENSE`).

It is used by `chains-app` (src/webui) with one change, marked "Chains:": on Windows the
built-in WebView2 loader takes the data folder from `WEBVIEW2_USER_DATA_FOLDER`, as the loader
of Microsoft does (chains-app sets it to `%LOCALAPPDATA%\Chains\WebView2`).

On Linux it uses WebKitGTK 4.1 with GTK 3, on Windows WebView2, on macOS WKWebView.

To update: replace `webview.h` with `core/include/webview/webview.h` of the new
release, apply the change marked "Chains:" again, and update the version above.
