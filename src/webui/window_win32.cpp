// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// The window on Windows: a window of the app with WebView2 in it, through webview/webview.
//
// The interface is served from the files built into the program by the web view itself
// (WebResourceRequested) under an origin of its own, https://app.chains-app.invalid, with its
// content security policy as a header: nothing goes to the network (.invalid is never a real
// name, and every request under it is answered here). The window has the integrated title bar
// (see Window in window.h): no caption of the system, the frame kept for the resizing borders,
// the shadow, the rounded corners and snapping.

#include <webui/assets.h>
#include <webui/chainconfig.h>
#include <tinyformat.h>
#include <webui/icontint.h>
#include <webui/window.h>

#include <logging.h>
#include <support/cleanse.h>
#include <univalue.h>
#include <util/fs.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>

#include <webview.h>

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <atomic>
#include <cstring>
#include <map>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace webui {
namespace {

// windowsx.h, without its macros that take the names of methods (IsMaximized).
#define GET_X_LPARAM(lp) (static_cast<int>(static_cast<short>(LOWORD(lp))))
#define GET_Y_LPARAM(lp) (static_cast<int>(static_cast<short>(HIWORD(lp))))

using webview::detail::narrow_string;
using webview::detail::widen_string;

//! Where the interface is served from: the web view answers every request under it.
constexpr std::string_view WIN_ORIGIN{"https://app.chains-app.invalid"};

constexpr UINT WM_APP_TRAY{WM_APP + 10};
constexpr UINT TRAY_UID{1};
constexpr UINT TRAY_MENU_FIRST{1000};
//! The icon of the app in its resources (chains-app-res.rc).
constexpr WORD ICON_ID{1};
//! How long a notification keeps a hidden tray icon (the notification needs one).
constexpr UINT NOTIFY_ICON_MS{12000};

//! The smallest window, in pixels at 96 DPI.
constexpr int MIN_WIDTH{560};
constexpr int MIN_HEIGHT{400};

/** A COM event handler or completion handler calling a function. */
template <typename Interface, typename... Args>
class Handler final : public Interface
{
public:
    Handler(const IID& iid, std::function<HRESULT(Args...)> fn) : m_iid(iid), m_fn(std::move(fn)) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG refs{--m_refs};
        if (refs == 0) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, m_iid)) {
            *out = static_cast<Interface*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Invoke(Args... args) override { return m_fn(args...); }

private:
    std::atomic<ULONG> m_refs{1};
    const IID m_iid;
    std::function<HRESULT(Args...)> m_fn;
};

template <typename Interface, typename... Args, typename F>
Interface* MakeHandler(const IID& iid, F fn)
{
    return new Handler<Interface, Args...>(iid, std::function<HRESULT(Args...)>(std::move(fn)));
}

/** A COM pointer that releases what it holds. */
template <typename T>
class ComPtr
{
public:
    ComPtr() = default;
    ~ComPtr() { Reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    T** operator&() { Reset(); return &m_p; }
    T* operator->() const { return m_p; }
    T* get() const { return m_p; }
    explicit operator bool() const { return m_p != nullptr; }
    void Reset()
    {
        if (m_p) m_p->Release();
        m_p = nullptr;
    }
    template <typename U>
    bool As(const IID& iid, ComPtr<U>& out) const
    {
        return m_p && SUCCEEDED(m_p->QueryInterface(iid, reinterpret_cast<void**>(&out)));
    }

private:
    T* m_p{nullptr};
};

std::string TakeString(LPWSTR text)
{
    if (!text) return {};
    std::string out{narrow_string(text)};
    CoTaskMemFree(text);
    return out;
}

bool OwnUri(std::string_view uri)
{
    return uri.size() > WIN_ORIGIN.size() && uri.starts_with(WIN_ORIGIN) && uri[WIN_ORIGIN.size()] == '/';
}

HICON AppIcon(int size)
{
    return static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(ICON_ID), IMAGE_ICON, size, size, LR_DEFAULTCOLOR));
}

//! The icon of the app in the colours of a network (NetworkStyle: testnet, signet and regtest), or as it is.
HICON AppIcon(int size, IconTint tint)
{
    HICON base{AppIcon(size)};
    if (!base || tint.None()) return base;
    ICONINFO info{};
    if (!GetIconInfo(base, &info)) return base;
    HICON out{nullptr};
    BITMAP bm{};
    if (info.hbmColor && GetObjectW(info.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        const int width{bm.bmWidth}, height{bm.bmHeight};
        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(bmi.bmiHeader);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height; // top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
        HDC dc{GetDC(nullptr)};
        if (GetDIBits(dc, info.hbmColor, 0, height, pixels.data(), &bmi, DIB_RGB_COLORS) == height) {
            // BGRA to RGBA, tinted, and back.
            for (size_t i{0}; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
            TintPixels(pixels, 4, static_cast<size_t>(width), static_cast<size_t>(height), static_cast<size_t>(width) * 4, tint);
            for (size_t i{0}; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]);
            void* bits{nullptr};
            if (HBITMAP color{CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0)}) {
                std::memcpy(bits, pixels.data(), pixels.size());
                ICONINFO tinted{};
                tinted.fIcon = TRUE;
                tinted.hbmMask = info.hbmMask;
                tinted.hbmColor = color;
                out = CreateIconIndirect(&tinted);
                DeleteObject(color);
            }
        }
        ReleaseDC(nullptr, dc);
    }
    if (info.hbmColor) DeleteObject(info.hbmColor);
    if (info.hbmMask) DeleteObject(info.hbmMask);
    if (!out) return base;
    DestroyIcon(base);
    return out;
}

//! A file:// address of the app (the debug log, the configuration file) as a path, or nothing.
std::optional<std::wstring> LocalFile(const std::string& uri)
{
    if (!uri.starts_with("file:///")) return std::nullopt;
    std::string path;
    const std::string_view encoded{std::string_view{uri}.substr(8)};
    for (size_t i{0}; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            const auto byte{ToIntegral<uint8_t>(std::string{encoded.substr(i + 1, 2)}, 16)};
            if (!byte) return std::nullopt;
            path += static_cast<char>(*byte);
            i += 2;
        } else {
            path += encoded[i] == '/' ? '\\' : encoded[i];
        }
    }
    return widen_string(path);
}

class Win32Window final : public Window
{
public:
    Win32Window(const Options& options, Callbacks callbacks)
        : m_callbacks(std::move(callbacks)), m_devtools(options.devtools), m_frameless(options.frameless), m_start_maximized(options.maximized)
    {
        // Per monitor DPI awareness (v2; also in the manifest), then COM for WebView2 and the dialogs.
        webview::detail::enable_dpi_awareness();
        m_com_ok = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        // The data of WebView2 (its caches) in the local folder of the app, not in the roaming one
        // webview/webview would choose; the runtime takes it from the environment.
        if (GetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", nullptr, 0) == 0) {
            const fs::path folder{GetSpecialFolderPath(CSIDL_LOCAL_APPDATA) / fs::u8path(std::string{chain::NAME}) / "WebView2"};
            SetEnvironmentVariableW(L"WEBVIEW2_USER_DATA_FOLDER", folder.wstring().c_str());
        }
        m_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
        HINSTANCE instance{GetModuleHandleW(nullptr)};

        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.hInstance = instance;
        wc.lpszClassName = L"ChainsAppWindow";
        wc.hIcon = AppIcon(GetSystemMetrics(SM_CXICON));
        wc.hIconSm = AppIcon(GetSystemMetrics(SM_CXSMICON));
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
        wc.hbrBackground = nullptr;
        wc.lpfnWndProc = +[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            Win32Window* self{nullptr};
            if (msg == WM_NCCREATE) {
                self = static_cast<Win32Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                self->m_hwnd = hwnd;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            } else {
                self = reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
            }
            if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
            return self->WndProc(msg, wp, lp);
        };
        RegisterClassExW(&wc);

        // The window over the maximize button of the page: Windows 11 shows its snap layouts there.
        WNDCLASSEXW cc{};
        cc.cbSize = sizeof(cc);
        cc.hInstance = instance;
        cc.lpszClassName = L"ChainsAppCaptionButton";
        cc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
        cc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        cc.lpfnWndProc = +[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
            auto* self{reinterpret_cast<Win32Window*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA))};
            if (msg == WM_NCCREATE) {
                self = static_cast<Win32Window*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            }
            if (!self) return DefWindowProcW(hwnd, msg, wp, lp);
            return self->CaptionButtonProc(hwnd, msg, wp, lp);
        };
        RegisterClassExW(&cc);

        // The size asked, in pixels at 96 DPI, on the monitor of the pointer, centred in its work area.
        POINT cursor{};
        GetCursorPos(&cursor);
        HMONITOR monitor{MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY)};
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(monitor, &mi);
        UINT dpi_x{96}, dpi_y{96};
        if (HMODULE shcore{LoadLibraryW(L"shcore.dll")}) {
            using GetDpiForMonitorFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
            if (auto fn{reinterpret_cast<GetDpiForMonitorFn>(reinterpret_cast<void*>(GetProcAddress(shcore, "GetDpiForMonitor")))}) fn(monitor, 0, &dpi_x, &dpi_y);
            FreeLibrary(shcore);
        }
        const RECT work{mi.rcWork};
        const int work_w{work.right - work.left}, work_h{work.bottom - work.top};
        const int w{std::min(MulDiv(options.width, dpi_x, 96), work_w)};
        const int h{std::min(MulDiv(options.height, dpi_x, 96), work_h)};
        m_width = options.width;
        m_height = options.height;

        CreateWindowExW(WS_EX_APPWINDOW, L"ChainsAppWindow", widen_string(options.title).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                        work.left + (work_w - w) / 2, work.top + (work_h - h) / 2, w, h, nullptr, nullptr, instance, this);
        if (!m_hwnd) throw std::runtime_error("Cannot make the window");
        m_dpi = GetDpiForWindow(m_hwnd);
        // Rounded corners on Windows 11, and the frame (with the caption gone) for the shadow.
        const DWORD corners{2 /* DWMWCP_ROUND */};
        DwmSetWindowAttribute(m_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corners, sizeof(corners));
        webview::detail::apply_window_theme(m_hwnd);
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

        m_caption_button = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, L"ChainsAppCaptionButton", L"", WS_CHILD, 0, 0, 0, 0, m_hwnd, nullptr, instance, this);
        if (m_caption_button) SetLayeredWindowAttributes(m_caption_button, 0, 1, LWA_ALPHA);

        m_webview = std::make_unique<webview::webview>(options.devtools, m_hwnd);
        m_widget = static_cast<HWND>(m_webview->widget().value());
        m_controller = static_cast<ICoreWebView2Controller*>(m_webview->browser_controller().value());
        ResizeWidget();
        SetupWebView();

        m_webview->bind("__chains", [this](std::string id, std::string args, void*) {
            if (m_callbacks.call) m_callbacks.call(id, args);
            // The request may hold a passphrase: not left in freed memory (the bridge took what it needs).
            memory_cleanse(args.data(), args.size());
        }, nullptr);
        // Where the maximize button of the page is (for the snap layouts): measured by the page
        // itself, sent here; the page knows nothing of it.
        m_webview->bind("__chainsCaption", [this](std::string id, std::string args, void*) {
            OnCaptionRect(args);
            m_webview->resolve(id, 0, "null");
        }, nullptr);
        m_webview->init(CAPTION_SCRIPT);
        m_webview->navigate(std::string{WIN_ORIGIN} + "/index.html");
    }

    ~Win32Window() override
    {
        RemoveTray();
        m_webview.reset();
        if (m_caption_button) DestroyWindow(m_caption_button);
        if (m_hwnd) {
            SetWindowLongPtrW(m_hwnd, GWLP_USERDATA, 0);
            DestroyWindow(m_hwnd);
        }
        for (HICON icon : {m_window_icon, m_window_icon_small, m_tray_icon}) {
            if (icon) DestroyIcon(icon);
        }
        if (m_com_ok) CoUninitialize();
    }

    void Run() override { m_webview->run(); }
    void Quit() override
    {
        m_webview->dispatch([] { PostQuitMessage(0); });
    }
    void Post(std::function<void()> fn) override { m_webview->dispatch(std::move(fn)); }
    void PostDelayed(std::chrono::milliseconds delay, std::function<void()> fn) override
    {
        m_webview->dispatch([this, delay, fn = std::move(fn)]() mutable {
            const UINT_PTR id{++m_next_timer};
            m_timers.emplace(id, std::move(fn));
            SetTimer(m_hwnd, id, static_cast<UINT>(std::max<int64_t>(delay.count(), USER_TIMER_MINIMUM)), nullptr);
        });
    }
    void Reply(const std::string& call_id, int status, const std::string& json) override { m_webview->resolve(call_id, status, json); }
    void Eval(const std::string& script) override { m_webview->eval(script); }
    void SetTitle(const std::string& title) override { SetWindowTextW(m_hwnd, widen_string(title).c_str()); }

    void SetNetwork(const std::string& network) override
    {
        // The icons of the window and of the tray in the colours of the network (NetworkStyle).
        m_tint = TintOf(network);
        for (auto [which, metric, icon] : {std::tuple{ICON_BIG, SM_CXICON, &m_window_icon}, std::tuple{ICON_SMALL, SM_CXSMICON, &m_window_icon_small}}) {
            HICON tinted{AppIcon(GetSystemMetrics(metric), m_tint)};
            if (!tinted) continue;
            SendMessageW(m_hwnd, WM_SETICON, which, reinterpret_cast<LPARAM>(tinted));
            if (*icon) DestroyIcon(*icon);
            *icon = tinted;
        }
        if (m_tray_icon) {
            DestroyIcon(m_tray_icon);
            m_tray_icon = nullptr;
        }
        if (m_tray_added) AddOrUpdateTray(false);
    }

    void Show(bool minimized) override
    {
        if (minimized) ShowWindow(m_hwnd, SW_SHOWMINNOACTIVE);
        else ShowWindow(m_hwnd, m_start_maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        m_start_maximized = false;
        UpdateWindow(m_hwnd);
        if (!minimized && m_controller) m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
    }

    void Present() override
    {
        if (IsIconic(m_hwnd)) ShowWindow(m_hwnd, SW_RESTORE);
        else ShowWindow(m_hwnd, SW_SHOW);
        SetForegroundWindow(m_hwnd);
    }

    void Minimize() override { ShowWindow(m_hwnd, SW_MINIMIZE); }
    void Hide() override { ShowWindow(m_hwnd, SW_HIDE); }
    bool IsVisible() override { return IsWindowVisible(m_hwnd); }

    Geometry GetGeometry() override
    {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(m_hwnd, &placement)) {
            const RECT& r{placement.rcNormalPosition};
            const int dpi{static_cast<int>(GetDpiForWindow(m_hwnd))};
            m_width = MulDiv(r.right - r.left, 96, dpi);
            m_height = MulDiv(r.bottom - r.top, 96, dpi);
        }
        return {m_width, m_height, IsZoomed(m_hwnd) != FALSE};
    }

    bool SupportsFrameless() override { return true; }
    bool IsFrameless() override { return m_frameless; }
    bool SetFrameless(bool frameless) override
    {
        if (frameless == m_frameless) return true;
        m_frameless = frameless;
        SetWindowPos(m_hwnd, nullptr, 0, 0, 0, 0, SWP_FRAMECHANGED | SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        PlaceCaptionButton();
        return true;
    }
    bool IsMaximized() override { return IsZoomed(m_hwnd) != FALSE; }
    void ToggleMaximize() override { ShowWindow(m_hwnd, IsZoomed(m_hwnd) ? SW_RESTORE : SW_MAXIMIZE); }

    void StartMove() override { StartDrag(HTCAPTION); }

    void StartResize(WindowEdge edge) override
    {
        if (IsZoomed(m_hwnd)) return;
        WPARAM hit{HTBOTTOMRIGHT};
        switch (edge) {
        case WindowEdge::TOP: hit = HTTOP; break;
        case WindowEdge::BOTTOM: hit = HTBOTTOM; break;
        case WindowEdge::LEFT: hit = HTLEFT; break;
        case WindowEdge::RIGHT: hit = HTRIGHT; break;
        case WindowEdge::TOP_LEFT: hit = HTTOPLEFT; break;
        case WindowEdge::TOP_RIGHT: hit = HTTOPRIGHT; break;
        case WindowEdge::BOTTOM_LEFT: hit = HTBOTTOMLEFT; break;
        case WindowEdge::BOTTOM_RIGHT: hit = HTBOTTOMRIGHT; break;
        }
        StartDrag(hit);
    }

    void SetTray(const Tray& tray) override
    {
        m_tray = tray;
        if (tray.visible) AddOrUpdateTray(false);
        else if (!m_notify_icon_until) RemoveTray();
    }
    bool TrayAvailable() override { return true; }

    void Notify(const std::string& title, const std::string& body, const std::string& level) override
    {
        // A balloon of the icon in the notification area (a toast on Windows 10 and 11). Without
        // the icon of the tray, one is shown for the time of the notification.
        if (!m_tray_added) {
            AddOrUpdateTray(true);
            m_notify_icon_until = GetTickCount64() + NOTIFY_ICON_MS;
            PostDelayed(std::chrono::milliseconds{NOTIFY_ICON_MS + 100}, [this] {
                if (m_notify_icon_until && GetTickCount64() >= m_notify_icon_until) {
                    m_notify_icon_until = 0;
                    if (!m_tray.visible) RemoveTray();
                }
            });
        }
        NOTIFYICONDATAW nid{TrayData()};
        nid.uFlags = NIF_INFO;
        wcsncpy(nid.szInfoTitle, widen_string(title).c_str(), std::size(nid.szInfoTitle) - 1);
        wcsncpy(nid.szInfo, widen_string(body.empty() ? std::string{" "} : body).c_str(), std::size(nid.szInfo) - 1);
        nid.dwInfoFlags = level == "critical" ? NIIF_ERROR : level == "warning" ? NIIF_WARNING : NIIF_INFO;
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }

    bool OpenExternal(const std::string& uri) override
    {
        std::wstring target;
        if (uri.starts_with("https://") || uri.starts_with("http://") || uri.starts_with("mailto:")) {
            target = widen_string(uri);
        } else if (const auto path{LocalFile(uri)}) {
            // Only the text files the app opens (the debug log, the configuration file): never a program.
            const std::wstring ext{PathFindExtensionW(path->c_str())};
            if (_wcsicmp(ext.c_str(), L".log") != 0 && _wcsicmp(ext.c_str(), L".conf") != 0 && _wcsicmp(ext.c_str(), L".txt") != 0) {
                LogWarning("webui: not opening %s", uri);
                return false;
            }
            target = *path;
            const auto result{reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL))};
            if (result > 32) return true;
            // A file type without an application (.conf): Notepad, as chains-qt does.
            return reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, L"open", L"notepad.exe", (L"\"" + target + L"\"").c_str(), nullptr, SW_SHOWNORMAL)) > 32;
        } else {
            LogWarning("webui: not opening %s", uri);
            return false;
        }
        const auto result{reinterpret_cast<INT_PTR>(ShellExecuteW(m_hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL))};
        if (result <= 32) LogWarning("webui: cannot open %s (%d)", uri, int(result));
        return result > 32;
    }

    void SetClipboard(const std::string& text) override
    {
        const std::wstring wide{widen_string(text)};
        if (!OpenClipboard(m_hwnd)) return;
        EmptyClipboard();
        if (HGLOBAL mem{GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t))}) {
            if (void* p{GlobalLock(mem)}) {
                memcpy(p, wide.c_str(), (wide.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(mem);
                if (!SetClipboardData(CF_UNICODETEXT, mem)) GlobalFree(mem);
            } else {
                GlobalFree(mem);
            }
        }
        CloseClipboard();
    }

    std::optional<std::string> GetClipboard() override
    {
        if (!OpenClipboard(m_hwnd)) return std::nullopt;
        std::optional<std::string> out;
        if (HANDLE data{GetClipboardData(CF_UNICODETEXT)}) {
            if (const auto* text{static_cast<const wchar_t*>(GlobalLock(data))}) {
                out = narrow_string(text);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return out;
    }

    void Confirm(const ConfirmDialog& dialog, std::function<void(bool)> done) override
    {
        // A message box of the system, owned by the window (modal to it); Cancel is the default button.
        Post([this, dialog, done = std::move(done)] {
            const std::wstring text{widen_string(dialog.message + "\n\n" + dialog.accept + "?")};
            const int answer{MessageBoxW(m_hwnd, text.c_str(), widen_string(dialog.title).c_str(), MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2 | MB_TASKMODAL)};
            done(answer == IDOK);
        });
    }

    void ChooseFile(const FileDialog& options, std::function<void(std::optional<std::string>)> done) override
    {
        // Shown after this call returns: the dialog runs a loop of its own.
        Post([this, options, done = std::move(done)] { done(RunFileDialog(options)); });
    }

    void Snapshot(const std::string& png_path, std::function<void(bool ok)> done) override
    {
        ComPtr<ICoreWebView2> view;
        IStream* stream{nullptr};
        if (!m_controller || FAILED(m_controller->get_CoreWebView2(&view)) ||
            FAILED(SHCreateStreamOnFileEx(widen_string(png_path).c_str(), STGM_CREATE | STGM_WRITE | STGM_SHARE_EXCLUSIVE, FILE_ATTRIBUTE_NORMAL, TRUE, nullptr, &stream))) {
            done(false);
            return;
        }
        auto* handler{MakeHandler<ICoreWebView2CapturePreviewCompletedHandler, HRESULT>(
            IID_ICoreWebView2CapturePreviewCompletedHandler, [stream, done](HRESULT result) {
                stream->Commit(STGC_DEFAULT);
                stream->Release();
                done(SUCCEEDED(result));
                return S_OK;
            })};
        if (FAILED(view->CapturePreview(COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, stream, handler))) {
            stream->Release();
            done(false);
        }
        handler->Release();
    }

private:
    //! Sends the place of the maximize button of the page to the host, whenever it may have moved.
    static constexpr const char* CAPTION_SCRIPT{R"js((() => {
  let last = '';
  const send = () => {
    const button = document.querySelector('[role="group"][aria-label="Window"] button:nth-child(2)');
    let rect = null;
    if (button && button.offsetParent !== null) {
      const r = button.getBoundingClientRect();
      const s = window.devicePixelRatio || 1;
      rect = [Math.round(r.left * s), Math.round(r.top * s), Math.round(r.right * s), Math.round(r.bottom * s)];
    }
    const text = JSON.stringify(rect);
    if (text !== last && typeof window.__chainsCaption === 'function') {
      last = text;
      window.__chainsCaption(rect).catch(() => { last = ''; });
    }
  };
  window.addEventListener('resize', send);
  setInterval(send, 400);
  document.addEventListener('DOMContentLoaded', send);
})();)js"};
    //! Shows the page's maximize button under the pointer while the window over it has the pointer.
    static constexpr const char* CAPTION_HOVER_SCRIPT{R"js(((on, down) => {
  const b = document.querySelector('[role="group"][aria-label="Window"] button:nth-child(2)');
  if (!b) return;
  b.style.background = on ? (down ? 'var(--line)' : 'var(--raised)') : '';
  b.style.color = on ? 'var(--text)' : '';
}))js"};

    LRESULT WndProc(UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp && m_frameless) {
                // No caption: the client area goes up to the top of the window. The other borders stay
                // (invisible on Windows 10 and 11: they resize the window and carry its shadow).
                auto* params{reinterpret_cast<NCCALCSIZE_PARAMS*>(lp)};
                const LONG top{params->rgrc[0].top};
                const LRESULT result{DefWindowProcW(m_hwnd, msg, wp, lp)};
                if (result != 0) return result;
                params->rgrc[0].top = top;
                if (IsZoomed(m_hwnd)) {
                    // A maximized window goes beyond the monitor by the width of its frame.
                    const UINT dpi{GetDpiForWindow(m_hwnd)};
                    params->rgrc[0].top += GetSystemMetricsForDpi(SM_CYFRAME, dpi) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
                }
                return 0;
            }
            break;
        case WM_NCHITTEST:
            if (m_frameless) {
                // Only what the web view does not cover reaches here: the borders, and the top edge.
                const LRESULT hit{DefWindowProcW(m_hwnd, msg, wp, lp)};
                if (hit != HTCLIENT || IsZoomed(m_hwnd)) return hit;
                POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                ScreenToClient(m_hwnd, &p);
                const int border{GetSystemMetricsForDpi(SM_CYFRAME, GetDpiForWindow(m_hwnd)) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, GetDpiForWindow(m_hwnd))};
                if (p.y < border) return HTTOP;
                return hit;
            }
            break;
        case WM_SIZE:
            ResizeWidget();
            if (wp == SIZE_MINIMIZED) {
                if (m_controller) m_controller->put_IsVisible(FALSE);
                if (m_callbacks.minimized) m_callbacks.minimized();
            } else {
                if (m_controller) m_controller->put_IsVisible(TRUE);
                const bool maximized{wp == SIZE_MAXIMIZED};
                if (maximized != m_maximized) {
                    m_maximized = maximized;
                    if (m_callbacks.maximized_changed) m_callbacks.maximized_changed(maximized);
                }
            }
            PlaceCaptionButton();
            return 0;
        case WM_MOVE:
        case WM_MOVING:
            if (m_controller) m_controller->NotifyParentWindowPositionChanged();
            break;
        case WM_GETMINMAXINFO: {
            auto* info{reinterpret_cast<MINMAXINFO*>(lp)};
            const int dpi{m_hwnd ? static_cast<int>(GetDpiForWindow(m_hwnd)) : 96};
            info->ptMinTrackSize.x = MulDiv(MIN_WIDTH, dpi, 96);
            info->ptMinTrackSize.y = MulDiv(MIN_HEIGHT, dpi, 96);
            return 0;
        }
        case WM_DPICHANGED: {
            m_dpi = HIWORD(wp);
            const RECT* r{reinterpret_cast<const RECT*>(lp)};
            SetWindowPos(m_hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_SETTINGCHANGE:
            if (lp && lstrcmpW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0) webview::detail::apply_window_theme(m_hwnd);
            break;
        case WM_ACTIVATE:
            if (LOWORD(wp) != WA_INACTIVE && m_controller) m_controller->MoveFocus(COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC);
            break;
        case WM_CLOSE:
            if (m_callbacks.close_requested) m_callbacks.close_requested();
            // The window stays until the app quits.
            return 0;
        case WM_QUERYENDSESSION:
            // As chains-qt (WinShutdownMonitor): the node stops cleanly before Windows ends the session.
            ShutdownBlockReasonCreate(m_hwnd, widen_string(strprintf("%s is shutting down. Do not interrupt it.", chain::NAME)).c_str());
            if (m_callbacks.session_ending) m_callbacks.session_ending();
            return FALSE;
        case WM_TIMER: {
            KillTimer(m_hwnd, wp);
            const auto it{m_timers.find(wp)};
            if (it == m_timers.end()) return 0;
            auto fn{std::move(it->second)};
            m_timers.erase(it);
            fn();
            return 0;
        }
        case WM_APP_TRAY:
            return OnTrayMessage(wp, lp);
        case WM_COMMAND:
            if (HIWORD(wp) == 0 && LOWORD(wp) >= TRAY_MENU_FIRST && LOWORD(wp) < TRAY_MENU_FIRST + m_tray.menu.size()) {
                const std::string id{m_tray.menu[LOWORD(wp) - TRAY_MENU_FIRST].id};
                if (m_callbacks.tray_menu) m_callbacks.tray_menu(id);
                return 0;
            }
            break;
        default:
            if (msg == m_taskbar_created && m_taskbar_created) {
                // Explorer started again: the icon has to be added again.
                m_tray_added = false;
                if (m_tray.visible) AddOrUpdateTray(false);
                return 0;
            }
        }
        return DefWindowProcW(m_hwnd, msg, wp, lp);
    }

    /** The window over the page's maximize button: answers HTMAXBUTTON, as Windows 11 wants for its snap layouts. */
    LRESULT CaptionButtonProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
    {
        switch (msg) {
        case WM_NCHITTEST:
            return HTMAXBUTTON;
        case WM_NCMOUSEMOVE:
            if (!m_caption_hover) {
                m_caption_hover = true;
                TRACKMOUSEEVENT track{sizeof(TRACKMOUSEEVENT), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                TrackMouseEvent(&track);
                CaptionHover(true, false);
            }
            return 0;
        case WM_NCMOUSELEAVE:
            m_caption_hover = false;
            m_caption_down = false;
            CaptionHover(false, false);
            return 0;
        case WM_NCLBUTTONDOWN:
        case WM_NCLBUTTONDBLCLK:
            m_caption_down = true;
            CaptionHover(true, true);
            return 0;
        case WM_NCLBUTTONUP:
            if (m_caption_down) {
                m_caption_down = false;
                CaptionHover(true, false);
                ToggleMaximize();
            }
            return 0;
        case WM_NCRBUTTONUP:
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    }

    void CaptionHover(bool on, bool down)
    {
        Eval(std::string{CAPTION_HOVER_SCRIPT} + "(" + (on ? "true" : "false") + "," + (down ? "true" : "false") + ")");
    }

    void OnCaptionRect(const std::string& args_json)
    {
        // args_json: [[left, top, right, bottom]] in pixels of the client area, or [null].
        m_caption_rect.reset();
        UniValue args;
        if (args.read(args_json) && args.isArray() && args.size() == 1 && args[0].isArray() && args[0].size() == 4) {
            RECT r;
            for (size_t i{0}; i < 4; ++i) {
                if (!args[0][i].isNum()) return PlaceCaptionButton();
            }
            r.left = args[0][0].getInt<int>();
            r.top = args[0][1].getInt<int>();
            r.right = args[0][2].getInt<int>();
            r.bottom = args[0][3].getInt<int>();
            if (r.right > r.left && r.bottom > r.top) m_caption_rect = r;
        }
        PlaceCaptionButton();
    }

    void PlaceCaptionButton()
    {
        if (!m_caption_button) return;
        if (!m_frameless || !m_caption_rect || IsIconic(m_hwnd)) {
            ShowWindow(m_caption_button, SW_HIDE);
            return;
        }
        const RECT& r{*m_caption_rect};
        SetWindowPos(m_caption_button, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void ResizeWidget()
    {
        if (!m_widget) return;
        RECT r{};
        GetClientRect(m_hwnd, &r);
        MoveWindow(m_widget, r.left, r.top, r.right - r.left, r.bottom - r.top, TRUE);
    }

    void StartDrag(WPARAM hit)
    {
        // Only while the button pressed in the page is still down: the system then follows the
        // pointer until it is released (a move or resize started after the release would stick).
        const int button{GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON};
        if (!(GetAsyncKeyState(button) & 0x8000)) return;
        POINT p{};
        GetCursorPos(&p);
        ReleaseCapture();
        // Posted: the loop of the move runs out of the call of the web view that asked for it.
        PostMessageW(m_hwnd, WM_NCLBUTTONDOWN, hit, MAKELPARAM(p.x, p.y));
    }

    void SetupWebView()
    {
        ComPtr<ICoreWebView2> view;
        if (!m_controller || FAILED(m_controller->get_CoreWebView2(&view))) throw std::runtime_error("No web view");

        ComPtr<ICoreWebView2Settings> settings;
        if (SUCCEEDED(view->get_Settings(&settings))) {
            settings->put_AreDefaultContextMenusEnabled(m_devtools);
            settings->put_AreDefaultScriptDialogsEnabled(TRUE);
            settings->put_IsZoomControlEnabled(FALSE);
            settings->put_AreHostObjectsAllowed(FALSE);
            settings->put_IsBuiltInErrorPageEnabled(TRUE);
            ComPtr<ICoreWebView2Settings3> s3;
            if (settings.As(IID_ICoreWebView2Settings3, s3)) s3->put_AreBrowserAcceleratorKeysEnabled(m_devtools);
            ComPtr<ICoreWebView2Settings4> s4;
            if (settings.As(IID_ICoreWebView2Settings4, s4)) {
                s4->put_IsGeneralAutofillEnabled(FALSE);
                s4->put_IsPasswordAutosaveEnabled(FALSE);
            }
            ComPtr<ICoreWebView2Settings5> s5;
            if (settings.As(IID_ICoreWebView2Settings5, s5)) s5->put_IsPinchZoomEnabled(FALSE);
            ComPtr<ICoreWebView2Settings6> s6;
            if (settings.As(IID_ICoreWebView2Settings6, s6)) s6->put_IsSwipeNavigationEnabled(FALSE);
        }

        // The files of the interface, from the program.
        ComPtr<ICoreWebView2_2> view2;
        if (!view.As(IID_ICoreWebView2_2, view2) || FAILED(view2->get_Environment(&m_environment))) throw std::runtime_error("This WebView2 is too old");
        const std::wstring filter{widen_string(std::string{WIN_ORIGIN} + "/*")};
        view->AddWebResourceRequestedFilter(filter.c_str(), COREWEBVIEW2_WEB_RESOURCE_CONTEXT_ALL);
        EventRegistrationToken token;
        auto* resources{MakeHandler<ICoreWebView2WebResourceRequestedEventHandler, ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs*>(
            IID_ICoreWebView2WebResourceRequestedEventHandler, [this](ICoreWebView2*, ICoreWebView2WebResourceRequestedEventArgs* args) {
                ServeAsset(args);
                return S_OK;
            })};
        view->add_WebResourceRequested(resources, &token);
        resources->Release();

        // The page stays the app: no navigation away from it, no new windows, no downloads, no permissions.
        auto* navigation{MakeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
            IID_ICoreWebView2NavigationStartingEventHandler, [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                LPWSTR uri{nullptr};
                args->get_Uri(&uri);
                const std::string target{TakeString(uri)};
                if (!OwnUri(target)) {
                    LogDebug(BCLog::RPC, "webui: navigation to %s refused\n", target);
                    args->put_Cancel(TRUE);
                }
                return S_OK;
            })};
        view->add_NavigationStarting(navigation, &token);
        navigation->Release();
        auto* frames{MakeHandler<ICoreWebView2NavigationStartingEventHandler, ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs*>(
            IID_ICoreWebView2NavigationStartingEventHandler, [](ICoreWebView2*, ICoreWebView2NavigationStartingEventArgs* args) {
                args->put_Cancel(TRUE);
                return S_OK;
            })};
        view->add_FrameNavigationStarting(frames, &token);
        frames->Release();
        auto* windows{MakeHandler<ICoreWebView2NewWindowRequestedEventHandler, ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs*>(
            IID_ICoreWebView2NewWindowRequestedEventHandler, [](ICoreWebView2*, ICoreWebView2NewWindowRequestedEventArgs* args) {
                args->put_Handled(TRUE);
                return S_OK;
            })};
        view->add_NewWindowRequested(windows, &token);
        windows->Release();
        auto* permissions{MakeHandler<ICoreWebView2PermissionRequestedEventHandler, ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs*>(
            IID_ICoreWebView2PermissionRequestedEventHandler, [](ICoreWebView2*, ICoreWebView2PermissionRequestedEventArgs* args) {
                args->put_State(COREWEBVIEW2_PERMISSION_STATE_DENY);
                return S_OK;
            })};
        view->add_PermissionRequested(permissions, &token);
        permissions->Release();
        ComPtr<ICoreWebView2_4> view4;
        if (view.As(IID_ICoreWebView2_4, view4)) {
            auto* downloads{MakeHandler<ICoreWebView2DownloadStartingEventHandler, ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs*>(
                IID_ICoreWebView2DownloadStartingEventHandler, [](ICoreWebView2*, ICoreWebView2DownloadStartingEventArgs* args) {
                    args->put_Cancel(TRUE);
                    return S_OK;
                })};
            view4->add_DownloadStarting(downloads, &token);
            downloads->Release();
        }
        auto* failed{MakeHandler<ICoreWebView2ProcessFailedEventHandler, ICoreWebView2*, ICoreWebView2ProcessFailedEventArgs*>(
            IID_ICoreWebView2ProcessFailedEventHandler, [](ICoreWebView2* sender, ICoreWebView2ProcessFailedEventArgs* args) {
                COREWEBVIEW2_PROCESS_FAILED_KIND kind;
                args->get_ProcessFailedKind(&kind);
                LogWarning("webui: a process of the web view failed (kind %d)", int(kind));
                if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_UNRESPONSIVE) sender->Reload();
                return S_OK;
            })};
        view->add_ProcessFailed(failed, &token);
        failed->Release();
    }

    void ServeAsset(ICoreWebView2WebResourceRequestedEventArgs* args)
    {
        ComPtr<ICoreWebView2WebResourceRequest> request;
        if (FAILED(args->get_Request(&request))) return;
        LPWSTR raw{nullptr};
        request->get_Uri(&raw);
        const std::string uri{TakeString(raw)};
        const Asset* asset{OwnUri(uri) ? FindAsset(BuiltinAssets(), std::string_view{uri}.substr(WIN_ORIGIN.size())) : nullptr};
        ComPtr<ICoreWebView2WebResourceResponse> response;
        if (!asset) {
            m_environment->CreateWebResourceResponse(nullptr, 404, L"Not Found", L"Content-Type: text/plain", &response);
        } else {
            IStream* stream{SHCreateMemStream(asset->data.data(), static_cast<UINT>(asset->data.size()))};
            const std::string headers{"Content-Type: " + std::string{MimeType(asset->path)} + "\r\n" +
                                      "Content-Security-Policy: " + ContentSecurityPolicy() + "\r\n" +
                                      "X-Content-Type-Options: nosniff\r\n" +
                                      "Cache-Control: no-store"};
            m_environment->CreateWebResourceResponse(stream, 200, L"OK", widen_string(headers).c_str(), &response);
            if (stream) stream->Release();
        }
        if (response) args->put_Response(response.get());
    }

    NOTIFYICONDATAW TrayData()
    {
        NOTIFYICONDATAW nid{};
        nid.cbSize = sizeof(nid);
        nid.hWnd = m_hwnd;
        nid.uID = TRAY_UID;
        return nid;
    }

    void AddOrUpdateTray(bool for_notification)
    {
        NOTIFYICONDATAW nid{TrayData()};
        nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
        nid.uCallbackMessage = WM_APP_TRAY;
        if (!m_tray_icon) m_tray_icon = AppIcon(GetSystemMetrics(SM_CXSMICON), m_tint);
        nid.hIcon = m_tray_icon;
        const std::string tip{m_tray.tooltip.empty() ? std::string{chain::NAME} : m_tray.tooltip};
        wcsncpy(nid.szTip, widen_string(tip).c_str(), std::size(nid.szTip) - 1);
        if (m_tray_added) {
            Shell_NotifyIconW(NIM_MODIFY, &nid);
            if (!for_notification) m_notify_icon_until = 0;
            return;
        }
        if (!Shell_NotifyIconW(NIM_ADD, &nid)) return;
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        m_tray_added = true;
    }

    void RemoveTray()
    {
        if (!m_tray_added) return;
        NOTIFYICONDATAW nid{TrayData()};
        Shell_NotifyIconW(NIM_DELETE, &nid);
        m_tray_added = false;
    }

    LRESULT OnTrayMessage(WPARAM, LPARAM lp)
    {
        switch (LOWORD(lp)) {
        case WM_LBUTTONUP:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            if (m_tray.visible && m_callbacks.tray_activated) m_callbacks.tray_activated();
            else if (!m_tray.visible) Present();
            break;
        case NIN_BALLOONUSERCLICK:
            Present();
            break;
        case WM_CONTEXTMENU:
            if (m_tray.visible) ShowTrayMenu();
            break;
        }
        return 0;
    }

    void ShowTrayMenu()
    {
        HMENU menu{CreatePopupMenu()};
        if (!menu) return;
        for (size_t i{0}; i < m_tray.menu.size(); ++i) {
            const TrayItem& item{m_tray.menu[i]};
            if (item.separator) {
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            } else {
                AppendMenuW(menu, MF_STRING | (item.enabled ? 0 : MF_GRAYED), TRAY_MENU_FIRST + i, widen_string(item.label).c_str());
            }
        }
        POINT p{};
        GetCursorPos(&p);
        // The menu closes when the user clicks elsewhere only if the window is in the foreground.
        SetForegroundWindow(m_hwnd);
        TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | (GetSystemMetrics(SM_MENUDROPALIGNMENT) ? TPM_RIGHTALIGN : TPM_LEFTALIGN), p.x, p.y, m_hwnd, nullptr);
        PostMessageW(m_hwnd, WM_NULL, 0, 0);
        DestroyMenu(menu);
    }

    std::optional<std::string> RunFileDialog(const FileDialog& options)
    {
        ComPtr<IFileDialog> dialog;
        if (FAILED(CoCreateInstance(options.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IFileDialog, reinterpret_cast<void**>(&dialog)))) {
            LogWarning("webui: no file dialog");
            return std::nullopt;
        }
        FILEOPENDIALOGOPTIONS flags{};
        dialog->GetOptions(&flags);
        flags |= FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR;
        if (options.folder) flags |= FOS_PICKFOLDERS;
        if (options.save) flags |= FOS_OVERWRITEPROMPT;
        else flags |= FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST;
        dialog->SetOptions(flags);
        if (!options.title.empty()) dialog->SetTitle(widen_string(options.title).c_str());
        std::vector<std::wstring> names, specs;
        std::wstring suffix;
        for (const auto& [name, patterns] : options.filters) {
            std::wstring spec, label{widen_string(name)};
            for (size_t i{0}; i < patterns.size(); ++i) {
                spec += (i ? L";" : L"") + widen_string(patterns[i]);
                if (suffix.empty() && patterns[i].starts_with("*.") && patterns[i].find_first_of("*?[", 1) == std::string::npos) suffix = widen_string(patterns[i].substr(2));
            }
            if (!patterns.empty()) label += L" (" + spec + L")";
            names.push_back(label);
            specs.push_back(spec.empty() ? L"*.*" : spec);
        }
        if (!options.folder && !names.empty()) {
            std::vector<COMDLG_FILTERSPEC> filter;
            for (size_t i{0}; i < names.size(); ++i) filter.push_back({names[i].c_str(), specs[i].c_str()});
            dialog->SetFileTypes(static_cast<UINT>(filter.size()), filter.data());
            dialog->SetFileTypeIndex(1);
        }
        // The extension of the first filter on a name without one (GUIUtil::getSaveFileName).
        if (options.save && !suffix.empty()) dialog->SetDefaultExtension(suffix.c_str());
        if (options.save && !options.suggested_name.empty()) dialog->SetFileName(widen_string(options.suggested_name).c_str());
        if (!options.start_dir.empty()) {
            ComPtr<IShellItem> folder;
            if (SUCCEEDED(SHCreateItemFromParsingName(widen_string(options.start_dir).c_str(), nullptr, IID_IShellItem, reinterpret_cast<void**>(&folder)))) dialog->SetFolder(folder.get());
        }
        if (FAILED(dialog->Show(m_hwnd))) return std::nullopt;
        ComPtr<IShellItem> item;
        if (FAILED(dialog->GetResult(&item))) return std::nullopt;
        LPWSTR path{nullptr};
        if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return std::nullopt;
        return TakeString(path);
    }

    Callbacks m_callbacks;
    const bool m_devtools;
    bool m_frameless;
    bool m_start_maximized;
    bool m_com_ok{false};
    HWND m_hwnd{nullptr};
    HWND m_widget{nullptr};
    HWND m_caption_button{nullptr};
    std::optional<RECT> m_caption_rect;
    bool m_caption_hover{false};
    bool m_caption_down{false};
    std::unique_ptr<webview::webview> m_webview;
    ICoreWebView2Controller* m_controller{nullptr};
    ComPtr<ICoreWebView2Environment> m_environment;
    UINT m_dpi{96};
    int m_width{0};
    int m_height{0};
    bool m_maximized{false};
    std::map<UINT_PTR, std::function<void()>> m_timers;
    UINT_PTR m_next_timer{100};
    UINT m_taskbar_created{0};
    Tray m_tray;
    bool m_tray_added{false};
    HICON m_tray_icon{nullptr};
    //! The tint of the network, and the icons of the window made with it (destroyed with the window).
    IconTint m_tint;
    HICON m_window_icon{nullptr};
    HICON m_window_icon_small{nullptr};
    uint64_t m_notify_icon_until{0};
};

} // namespace

std::unique_ptr<Window> Window::Create(const Options& options, Callbacks callbacks)
{
    try {
        return std::make_unique<Win32Window>(options, std::move(callbacks));
    } catch (const webview::exception& e) {
        LogError("webui: cannot show the interface: %s", e.what());
        // No WebView2 Runtime (Windows 10 before 2021, or removed).
        if (e.error().code() == WEBVIEW_ERROR_MISSING_DEPENDENCY && MessageBoxW(nullptr, widen_string(strprintf("%s cannot show its interface: the Microsoft Edge WebView2 Runtime is missing.\n\nOpen its download page?", chain::NAME)).c_str(),
                        widen_string(std::string{chain::NAME}).c_str(), MB_YESNO | MB_ICONERROR) == IDYES) {
            ShellExecuteW(nullptr, L"open", L"https://go.microsoft.com/fwlink/p/?LinkId=2124703", nullptr, nullptr, SW_SHOWNORMAL);
        }
        throw std::runtime_error(std::string{"WebView2: "} + e.what());
    }
}

} // namespace webui
