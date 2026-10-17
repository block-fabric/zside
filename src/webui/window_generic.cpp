// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The window on macOS (WKWebView), through webview/webview (Windows: window_win32.cpp).
//
// Not built or tested on this branch yet (the Linux window is window_gtk.cpp).
// The interface is loaded as one self-contained document (InlineDocument), as
// webview/webview offers no way to serve a scheme of the app on these
// platforms. Closing the window ends the event loop; the app then shuts the
// node down as it does after Quit.

#include <webui/assets.h>
#include <support/cleanse.h>
#include <webui/window.h>

#include <logging.h>

#include <webview.h>

#include <stdexcept>
#include <thread>
#include <utility>

namespace webui {
namespace {

class GenericWindow final : public Window
{
public:
    GenericWindow(const Options& options, Callbacks callbacks) : m_callbacks(std::move(callbacks))
    {
        m_webview = std::make_unique<webview::webview>(options.devtools, nullptr);
        m_webview->set_title(options.title);
        m_webview->set_size(options.width, options.height, WEBVIEW_HINT_NONE);
        m_webview->bind("__chains", [this](std::string id, std::string args, void*) {
            if (m_callbacks.call) m_callbacks.call(id, args);
            // The request may hold a passphrase: not left in freed memory (the bridge took what it needs).
            memory_cleanse(args.data(), args.size());
        }, nullptr);
        const auto document{InlineDocument(BuiltinAssets())};
        if (!document) throw std::runtime_error("The interface is missing from this build");
        m_webview->set_html(*document);
    }

    void Run() override { m_webview->run(); }
    void Quit() override { m_webview->terminate(); }
    void Post(std::function<void()> fn) override { m_webview->dispatch(std::move(fn)); }
    void PostDelayed(std::chrono::milliseconds delay, std::function<void()> fn) override
    {
        // TODO(windows/macos port): a timer of the platform. A thread is enough for the few uses.
        std::thread([this, delay, fn = std::move(fn)]() mutable {
            std::this_thread::sleep_for(delay);
            Post(std::move(fn));
        }).detach();
    }
    void Reply(const std::string& call_id, int status, const std::string& json) override { m_webview->resolve(call_id, status, json); }
    void Eval(const std::string& script) override { m_webview->eval(script); }
    void SetTitle(const std::string& title) override { m_webview->set_title(title); }
    // TODO(windows/macos port): the icon of the network (the window has no icon of the app yet there).
    void SetNetwork(const std::string&) override {}
    void Show(bool) override {}
    void Present() override {}
    void Minimize() override {}
    // TODO(windows/macos port): hiding, the tray (Shell_NotifyIcon, NSStatusItem) and notifications of the platform.
    void Hide() override {}
    bool IsVisible() override { return true; }
    Geometry GetGeometry() override { return {}; }
    // TODO(windows/macos port): the integrated title bar (see Window in window.h). Until then the
    // window keeps the frame of the system, and the page its own title bar hidden.
    bool SupportsFrameless() override { return false; }
    bool IsFrameless() override { return false; }
    bool SetFrameless(bool) override { return false; }
    bool IsMaximized() override { return false; }
    void ToggleMaximize() override {}
    void StartMove() override {}
    void StartResize(WindowEdge) override {}
    void SetTray(const Tray&) override {}
    bool TrayAvailable() override { return false; }
    void Notify(const std::string&, const std::string&, const std::string&) override {}
    bool OpenExternal(const std::string&) override { return false; }
    void SetClipboard(const std::string&) override { LogWarning("webui: no clipboard on this platform yet"); }
    std::optional<std::string> GetClipboard() override { return std::nullopt; }
    // TODO(macos port): the last key or click of the user in the window (NSEvent), for the clipboard.
    std::optional<UserInput> LastUserInput() override { return std::nullopt; }
    void NoteUserInput() override {}
    void Snapshot(const std::string&, std::function<void(bool ok)> done) override { done(false); }
    void Ask(const dialog::Request&, std::function<void(dialog::Answer)> done) override
    {
        // TODO(macos port): the dialog window of the host (a modal NSPanel with a WKWebView of its own showing the page
        // of the dialogs, dialog.h), NSAlert as its fallback. Until then nothing that needs a confirmation is done.
        LogWarning("webui: no confirmation dialogs on this platform yet");
        done({});
    }
    void TestDialog(const std::string&, const std::string&, std::function<void(std::optional<std::string>)> done) override { done(std::nullopt); }
    void ChooseFile(const FileDialog&, std::function<void(std::optional<std::string>)> done) override
    {
        // TODO(windows/macos port): the file dialogs of the platform.
        LogWarning("webui: no file dialogs on this platform yet");
        done(std::nullopt);
    }

private:
    Callbacks m_callbacks;
    std::unique_ptr<webview::webview> m_webview;
};

} // namespace

std::unique_ptr<Window> Window::Create(const Options& options, Callbacks callbacks)
{
    return std::make_unique<GenericWindow>(options, std::move(callbacks));
}

} // namespace webui
