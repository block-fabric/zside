// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <webui/app.h>
#include <webui/chainconfig.h>

#include <chainparams.h>
#include <clientversion.h>
#include <common/args.h>
#include <common/init.h>
#include <common/license_info.h>
#include <common/settings.h>
#include <init.h>
#include <interfaces/handler.h>
#include <interfaces/init.h>
#include <interfaces/node.h>
#include <logging.h>
#include <node/interface_ui.h>
#include <util/chaintype.h>
#include <util/exception.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <validation.h>
#include <webui/appconfig.h>
#include <webui/bridge.h>
#include <webui/events.h>
#include <webui/guard.h>
#include <webui/i18n.h>
#include <webui/assets.h>
#include <webui/nodeapi.h>
#include <webui/nodeevents.h>
#include <webui/options.h>
#include <webui/rpcpolicy.h>
#include <webui/local.h>
#include <webui/sidechainapi.h>
#include <webui/sidechainnodes.h>
#include <webui/singleinstance.h>
#ifdef WIN32
#include <webui/urischeme.h>
#endif
#ifdef ENABLE_WALLET
#include <webui/walletapi.h>
#endif
#include <webui/window.h>
#include <webui/windowframe.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <future>
#include <sstream>

using namespace std::chrono_literals;

namespace webui {
namespace {
constexpr int DEFAULT_PRUNE_TARGET_GB{2};
constexpr uint64_t GB_BYTES{1'000'000'000};
constexpr bool DEFAULT_CHOOSE_DATADIR{false};
constexpr bool DEFAULT_SPLASHSCREEN{true};
//! How often the app looks whether the node was asked to stop from elsewhere (RPC stop, a signal).
constexpr auto SHUTDOWN_POLL{200ms};

int64_t PruneGBtoMiB(int64_t gb) { return gb * GB_BYTES / (1024 * 1024); }
int PruneMiBtoGB(int64_t mib) { return (mib * 1024 * 1024 + GB_BYTES - 1) / GB_BYTES; }

std::thread::id g_ui_thread;

std::string NetworkLabel(const std::string& network)
{
    if (network == "main") return "";
    if (network == "test") return "testnet";
    return network;
}

const UniValue& Param(const UniValue& params, const std::string& key)
{
    return params.isObject() ? params.find_value(key) : NullUniValue;
}

std::string ParamStr(const UniValue& params, const std::string& key)
{
    const UniValue& value{Param(params, key)};
    if (!value.isStr()) throw HostError(BRIDGE_INVALID_REQUEST, strprintf("%s: a string is needed", key));
    return value.get_str();
}

/** Free space of the directory, or of the closest folder above it that exists (as FreeSpaceChecker). */
UniValue SpaceOf(const std::string& dir)
{
    UniValue out{UniValue::VOBJ};
    fs::path path{fs::PathFromString(dir)};
    try {
        const bool exists{fs::exists(path)};
        out.pushKV("exists", exists);
        out.pushKV("isDirectory", exists && fs::is_directory(path));
        fs::path parent{path};
        while (!parent.empty() && !fs::exists(parent)) parent = parent.parent_path();
        if (parent.empty()) {
            out.pushKV("error", "No folder of this path exists");
            return out;
        }
        out.pushKV("free", static_cast<int64_t>(fs::space(parent).available));
    } catch (const fs::filesystem_error& e) {
        out.pushKV("error", e.what());
    }
    return out;
}
} // namespace

void SetupUIArgs(ArgsManager& argsman)
{
    argsman.AddArg("-choosedatadir", strprintf("Choose data directory on startup (default: %u)", DEFAULT_CHOOSE_DATADIR), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-lang=<lang>", "Set language, for example \"de_DE\" (default: system locale)", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-min", "Start minimized", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-resetguisettings", "Reset all settings changed in the GUI", ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-splash", strprintf("Show splash screen on startup (default: %u)", DEFAULT_SPLASHSCREEN), ArgsManager::ALLOW_ANY, OptionsCategory::GUI);
    argsman.AddArg("-webuidevtools", "Enable the web inspector and the context menu of the web view (default: 0)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
    argsman.AddArg("-webuiconfig=<file>", strprintf("File of the app settings (default: %s.json in the configuration folder of the user)", webui::chain::APP), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
#ifdef WEBUI_TEST_HOOKS
    // Only in a build for the tests (cmake -DWEBUI_TEST_HOOKS=ON): a release has neither the options nor the hooks.
    argsman.AddArg("-webuitestscript=<file>", "Run this script in the page once it is loaded, with the test methods of the bridge (for the tests of the app)", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
    argsman.AddArg("-webuitestshots=<dir>", "Folder for the pictures the test script takes", ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
#endif
#ifdef WIN32
    argsman.AddArg("-webuiregisteruri", strprintf("Open %s: payment URIs with this program, for this user, unless another one does (default: 1; never with -webuitestscript)", webui::chain::URI_SCHEME), ArgsManager::ALLOW_ANY | ArgsManager::DEBUG_ONLY, OptionsCategory::GUI);
#endif
}

App::App(interfaces::Init& init, ArgsManager& args, std::vector<std::string> uris)
    : m_init(init), m_args(args),
      // The debug options of the app are taken from the command line alone (only it is read yet): a
      // chains.conf cannot run a script in the page, nor open the inspector.
      m_devtools(args.GetBoolArg("-webuidevtools", false)),
#ifdef WEBUI_TEST_HOOKS
      m_test_script(args.GetArg("-webuitestscript", "")),
      m_test_shots(args.GetArg("-webuitestshots", "")),
#endif
      m_config_path(args.GetArg("-webuiconfig", ""))
{
    LOCK(m_mutex);
    m_pending_uris = std::move(uris);
}

App::~App()
{
    if (m_node_thread.joinable()) m_node_thread.join();
}

void App::DeliverEvents(const std::string& batch_json)
{
    // The batch goes to the page as a string literal that it parses: data, never code.
    m_window->Eval("window.__chainsEvents && window.__chainsEvents(" + UniValue{batch_json}.write() + ")");
}

UniValue App::Info()
{
    UniValue info{UniValue::VOBJ};
    info.pushKV("name", CLIENT_NAME);
    info.pushKV("version", FormatFullVersion());
    info.pushKV("bugreport", CLIENT_BUGREPORT);
    {
        LOCK(m_mutex);
        info.pushKV("network", m_network);
        info.pushKV("networkLabel", NetworkLabel(m_network));
    }
    std::string datadir;
    bool configured;
    {
        LOCK(m_mutex);
        configured = !m_network.empty();
    }
    try {
        if (configured) datadir = fs::PathToString(m_args.GetDataDirNet());
    } catch (const std::exception&) {
    }
    info.pushKV("datadir", datadir);
    if (configured) info.pushKV("blockSpacing", Params().GetConsensus().nPowTargetSpacing);
#ifdef ENABLE_WALLET
    info.pushKV("walletEnabled", !m_args.GetBoolArg("-disablewallet", false));
#else
    info.pushKV("walletEnabled", false);
#endif
    info.pushKV("devtools", m_devtools);
    {
        LOCK(m_mutex);
        info.pushKV("language", m_language);
    }
    info.pushKV("test", !m_test_script.empty());
#if defined(__linux__)
    info.pushKV("platform", "linux");
#elif defined(WIN32)
    info.pushKV("platform", "windows");
#elif defined(__APPLE__)
    info.pushKV("platform", "macos");
#else
    info.pushKV("platform", "other");
#endif
    return info;
}

void App::SetPhase(const std::string& phase, UniValue data)
{
    {
        LOCK(m_mutex);
        m_phase = phase;
        m_phase_data = data;
    }
    UniValue event{UniValue::VOBJ};
    event.pushKV("phase", phase);
    event.pushKV("data", std::move(data));
    m_hub->Emit("phase", std::move(event));
}

void App::UpdateTitle()
{
    std::string title{CLIENT_NAME};
    {
        LOCK(m_mutex);
        if (!m_wallet_title.empty()) title += " - " + m_wallet_title;
        if (const std::string label{NetworkLabel(m_network)}; !label.empty()) title += " [" + label + "]";
    }
    m_window->Post([this, title] { m_window->SetTitle(title); });
}

UniValue App::Ask(UniValue dialog, UniValue fallback)
{
    if (std::this_thread::get_id() == g_ui_thread) {
        // The UI thread cannot wait for the page: show it and go on.
        m_hub->Emit("dialog", std::move(dialog));
        return fallback;
    }
    int64_t id;
    {
        LOCK(m_mutex);
        if (m_shutdown_requested) return fallback;
        id = m_next_dialog++;
        dialog.pushKV("id", id);
        m_dialogs[id] = dialog;
    }
    m_hub->Emit("dialog", dialog);
    WAIT_LOCK(m_mutex, lock);
    m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_answers.contains(id) || m_shutdown_requested; });
    m_dialogs.erase(id);
    const auto it{m_answers.find(id)};
    if (it == m_answers.end()) return fallback;
    UniValue answer{it->second};
    m_answers.erase(it);
    return answer;
}

void App::Fatal(const std::string& title, const std::string& message, const std::vector<std::string>& details)
{
    UniValue data{UniValue::VOBJ};
    data.pushKV("title", title);
    data.pushKV("message", message);
    UniValue list{UniValue::VARR};
    for (const auto& d : details) list.push_back(d);
    data.pushKV("details", std::move(list));
    {
        LOCK(m_mutex);
        if (!m_exit_code) m_exit_code = EXIT_FAILURE;
    }
    m_window->Post([this] { m_window->Show(false); });
    SetPhase("fatal", std::move(data));
}

void App::UpdateLanguage()
{
    const std::string language{ChooseLanguage(BuiltinAssets(), m_args.GetArg("-lang", ""))};
    {
        LOCK(m_mutex);
        if (language == m_language) return;
        m_language = language;
    }
    SetCoreCatalog(LoadCatalog(BuiltinAssets(), language));
    UniValue data{UniValue::VOBJ};
    data.pushKV("code", language);
    if (m_hub) m_hub->Emit("locale", std::move(data));
}

void App::CloseRequested()
{
    {
        LOCK(m_mutex);
        if (m_phase == "shutdown") return;
    }
    // As BitcoinGUI::closeEvent: minimized, if the user wants that, else the app stops.
    bool ready;
    {
        LOCK(m_mutex);
        ready = m_phase == "ready";
    }
    if (ready && PrefFlag("minimizeOnClose", false)) {
        m_window->Minimize();
        return;
    }
    RequestShutdown();
}

UniValue App::WindowState()
{
    const TitleBarMode mode{ParseTitleBarMode(m_config->Value("titleBar"))};
    const bool supported{m_window->SupportsFrameless()};
    const bool frameless{m_window->IsFrameless()};
    UniValue state{UniValue::VOBJ};
    //! The title bar chosen in the settings, and whether the window has it now (else after a restart).
    state.pushKV("titleBar", std::string{TitleBarModeName(mode)});
    state.pushKV("supported", supported);
    state.pushKV("frameless", frameless);
    state.pushKV("restartNeeded", supported && frameless != (mode == TitleBarMode::INTEGRATED));
    state.pushKV("maximized", m_window->IsMaximized());
    return state;
}

void App::RequestShutdown()
{
    {
        LOCK(m_mutex);
        if (m_shutdown_requested) return;
        m_shutdown_requested = true;
    }
    LogInfo("webui: shutdown requested");
    // Calls that wait for the node (waitfornewblock, a BMM request) return when it is asked to stop.
    if (m_node) m_node->startShutdown();
    m_cv.notify_all();
}

void App::OnInstanceMessage(const std::string& line)
{
    if (line == "activate") {
        m_window->Post([this] { m_window->Present(); });
    } else if (line.starts_with("uri ")) {
        const std::string uri{line.substr(4)};
        m_window->Post([this] { m_window->Present(); });
        bool deliver;
        {
            LOCK(m_mutex);
            deliver = m_phase == "ready";
            if (!deliver) m_pending_uris.push_back(uri);
        }
        if (deliver) {
            UniValue data{UniValue::VOBJ};
            data.pushKV("uri", uri);
            m_hub->Emit("uri", std::move(data));
        }
    }
}

void App::PollShutdown()
{
    {
        LOCK(m_mutex);
        if (m_shutdown_requested) return;
    }
    if (m_node && m_node->shutdownRequested()) {
        RequestShutdown();
        return;
    }
    m_window->PostDelayed(SHUTDOWN_POLL, [this] { PollShutdown(); });
}

bool App::IntroIfNeeded()
{
    if (!m_args.GetArg("-datadir", "").empty()) return true;
    const std::string default_dir{fs::PathToString(GetDefaultDataDir())};
    std::string dir{m_config->DataDir().value_or(default_dir)};

    if (!fs::exists(fs::PathFromString(dir)) || m_args.GetBoolArg("-choosedatadir", DEFAULT_CHOOSE_DATADIR) ||
        m_config->ResetPending() || m_args.GetBoolArg("-resetguisettings", false)) {
        try {
            SelectParams(m_args.GetChainType());
        } catch (const std::exception& e) {
            InitError(Untranslated(e.what()));
            Fatal("Error", e.what());
            return false;
        }
        const int64_t prune_arg{m_args.GetIntArg("-prune", 0)};
        UniValue data{UniValue::VOBJ};
        data.pushKV("defaultDir", default_dir);
        data.pushKV("dir", dir);
        data.pushKV("blockchainGB", static_cast<int64_t>(Params().AssumedBlockchainSize()));
        data.pushKV("chainstateGB", static_cast<int64_t>(Params().AssumedChainStateSize()));
        data.pushKV("pruneGB", prune_arg > 1 ? PruneMiBtoGB(prune_arg) : DEFAULT_PRUNE_TARGET_GB);
        data.pushKV("minPruneGB", static_cast<int64_t>(std::ceil(MIN_DISK_SPACE_FOR_BLOCK_FILES / double(GB_BYTES))));
        // -prune on the command line decides, as in the Qt intro (the box is ticked or not, and disabled).
        if (m_args.IsArgSet("-prune")) data.pushKV("pruneForced", prune_arg >= 1);
        m_window->Post([this] { m_window->Show(false); });
        SetPhase("intro", std::move(data));

        std::pair<std::string, int64_t> choice;
        {
            WAIT_LOCK(m_mutex, lock);
            m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_intro_choice || m_intro_cancelled || m_shutdown_requested; });
            if (!m_intro_choice) {
                if (!m_exit_code) m_exit_code = EXIT_SUCCESS;
                return false;
            }
            choice = *m_intro_choice;
        }
        dir = choice.first;
        m_config->SetDataDir(dir);
        m_config->SetResetPending(false);
    }
    // Only override -datadir if different from the default, to make it possible to override
    // -datadir in the chains.conf of the default data directory (as chainsd does).
    if (dir != default_dir) m_args.SoftSetArg("-datadir", dir);
    return true;
}

bool App::StartNode()
{
    // The configuration: chains.conf and settings.json.
    auto settings_abort{[this](const bilingual_str& message, const std::vector<std::string>& details) {
        UniValue dialog{UniValue::VOBJ};
        dialog.pushKV("kind", "settingsError");
        dialog.pushKV("title", CLIENT_NAME);
        dialog.pushKV("message", message.original + ".");
        dialog.pushKV("question", "Do you want to reset settings to default values, or to abort without making changes?");
        UniValue list{UniValue::VARR};
        for (const auto& d : details) list.push_back(d);
        dialog.pushKV("details", std::move(list));
        const UniValue answer{Ask(std::move(dialog), UniValue{"abort"})};
        return !(answer.isStr() && answer.get_str() == "reset");
    }};
    if (auto error{common::InitConfig(m_args, settings_abort)}) {
        InitError(error->message, error->details);
        if (error->status == common::ConfigStatus::FAILED_WRITE) {
            Fatal(error->message.original, "A fatal error occurred. Check that settings file is writable, or try running with -nosettings.", error->details);
        } else if (error->status != common::ConfigStatus::ABORTED) {
            Fatal("Error", "Error: " + error->message.original, error->details);
        } else {
            LOCK(m_mutex);
            m_exit_code = EXIT_FAILURE;
        }
        return false;
    }
    {
        LOCK(m_mutex);
        m_network = ChainTypeToString(m_args.GetChainType());
    }
    // The language option is in settings.json: the page and the node follow it from now on.
    UpdateLanguage();
    // The icon of the window in the colours of the network (NetworkStyle).
    m_window->Post([this, network = ChainTypeToString(m_args.GetChainType())] { m_window->SetNetwork(network); });

    // The debug options come from the command line alone: what chains.conf says of them is ignored.
    const std::vector<std::pair<std::string, std::string>> command_line{{"-webuitestscript", m_test_script}, {"-webuitestshots", m_test_shots}, {"-webuiconfig", m_config_path}};
    for (const auto& [arg, value] : command_line) {
        if (m_args.GetArg(arg, "") != value) LogWarning("webui: %s is taken from the command line only; the configuration file sets it, and is ignored", arg);
    }
    if (m_args.GetBoolArg("-webuidevtools", false) != m_devtools) LogWarning("webui: -webuidevtools is taken from the command line only; the configuration file sets it, and is ignored");

    // One app per data directory: a second start hands its URIs to the first one.
    std::vector<std::string> uris;
    {
        LOCK(m_mutex);
        uris = m_pending_uris;
    }
    const fs::path socket{SingleInstance::SocketPath(m_args.GetDataDirNet())};
    if (SingleInstance::SendToRunning(socket, uris)) {
        tfm::format(std::cout, "%s runs already for this data directory: %s handed to it.\n", CLIENT_NAME, uris.empty() ? "asked it to show itself" : "the URIs were");
        LOCK(m_mutex);
        m_exit_code = EXIT_SUCCESS;
        return false;
    }
    m_instance = std::make_unique<SingleInstance>();
    if (!m_instance->Listen(socket, [this](const std::string& line) { OnInstanceMessage(line); })) {
        LogWarning("webui: cannot take URIs from other starts of the app");
    }

    UpdateTitle();
    const bool minimized{m_args.GetBoolArg("-min", false)};
    m_window->Post([this, minimized] { m_window->Show(minimized); });
    SetPhase("init");

    // As BitcoinApplication::parameterSetup.
    m_args.SoftSetBoolArg("-printtoconsole", false);
    InitLogging(m_args);
    InitParameterInteraction(m_args);

    m_node = m_init.makeNode();
    m_node_events = std::make_unique<NodeEvents>(*m_node, *m_hub);
    m_node_events->SubscribeInit();

    // Messages and questions of the node, in the page (as BitcoinGUI::subscribeToCoreSignals).
    m_node_handlers.push_back(m_node->handleMessageBox([this](const bilingual_str& message, unsigned int style) {
        const unsigned int icon{style & CClientUIInterface::ICON_MASK};
        const std::string level{icon == CClientUIInterface::ICON_ERROR ? "error" : icon == CClientUIInterface::ICON_WARNING ? "warning" : "info"};
        UniValue dialog{UniValue::VOBJ};
        dialog.pushKV("kind", "message");
        dialog.pushKV("level", level);
        dialog.pushKV("title", level == "error" ? "Error" : level == "warning" ? "Warning" : "Information");
        dialog.pushKV("message", message.translated);
        if (style & CClientUIInterface::MODAL) {
            Ask(std::move(dialog), NullUniValue);
        } else {
            m_hub->Emit("notice", std::move(dialog));
        }
    }));
    m_node_handlers.push_back(m_node->handleQuestion([this](const bilingual_str& message, const std::string&, unsigned int style) {
        UniValue dialog{UniValue::VOBJ};
        dialog.pushKV("kind", "question");
        dialog.pushKV("title", CLIENT_NAME);
        dialog.pushKV("message", message.translated);
        dialog.pushKV("style", static_cast<int64_t>(style));
        const UniValue answer{Ask(std::move(dialog), UniValue{false})};
        return answer.isBool() && answer.get_bool();
    }));

    // Changes of the Settings page never applied (the app stopped first): taken back before the node
    // reads its settings, as the Cancel of the options dialog would.
    UndoOptions(*m_node, "never applied");

    if (m_args.GetBoolArg("-resetguisettings", false)) {
        // As OptionsModel::Reset: settings.json is backed up and reset, and the preferences of the interface too.
        m_node->resetSettings();
        m_config->ResetPrefs(m_network);
    }
    {
        std::optional<std::pair<std::string, int64_t>> choice;
        {
            LOCK(m_mutex);
            choice = m_intro_choice;
        }
        if (choice) {
            // As OptionsModel::SetPruneTargetGB, for the choice of the intro.
            const int64_t prune_mib{choice->second};
            const common::SettingsValue current{m_node->getPersistentSetting("prune")};
            const common::SettingsValue chosen{prune_mib};
            m_node->forceSetting("prune", chosen);
            const int64_t current_mib{current.isNum() ? current.getInt<int64_t>() : 0};
            if ((current_mib > 1) != (prune_mib > 1) || (prune_mib > 1 && PruneMiBtoGB(current_mib) != PruneMiBtoGB(prune_mib))) {
                m_node->updateRwSetting("prune", chosen);
            }
        }
    }

    if (!m_node->baseInitialize()) {
        // The error was shown, as a message of the node.
        LOCK(m_mutex);
        if (!m_exit_code) m_exit_code = EXIT_FAILURE;
        return false;
    }
    {
        LOCK(m_mutex);
        m_node_started = true;
    }

    interfaces::BlockAndHeaderTipInfo tip_info;
    const bool ok{m_node->appInitMain(&tip_info)};
    if (!ok || m_node->shutdownRequested()) {
        RequestShutdown();
        return false;
    }

    interfaces::Node* node{m_node.get()};
    m_bridge->SetRpc([node](const std::string& method, const UniValue& params, const std::string& uri) {
        return node->executeRpc(method, params, uri);
    });
    m_node_events->SubscribeNode();

    UniValue data{UniValue::VOBJ};
    data.pushKV("height", tip_info.block_height);
    data.pushKV("time", tip_info.block_time);
    data.pushKV("headerHeight", tip_info.header_height);
    data.pushKV("headerTime", tip_info.header_time);
    data.pushKV("progress", tip_info.verification_progress);
    // A restart was what the options waited for (OptionsModel::Init).
    {
        UniValue change{UniValue::VOBJ};
        change.pushKV("restartRequired", UniValue{});
        std::string network;
        {
            LOCK(m_mutex);
            network = m_network;
        }
        m_config->UpdatePrefs(network, change);
    }
    SetPhase("ready", std::move(data));
    m_window->Post([this] { UpdateTray(); });

    std::vector<std::string> waiting;
    {
        LOCK(m_mutex);
        waiting.swap(m_pending_uris);
    }
    for (const std::string& uri : waiting) {
        UniValue event{UniValue::VOBJ};
        event.pushKV("uri", uri);
        m_hub->Emit("uri", std::move(event));
    }
    m_window->PostDelayed(SHUTDOWN_POLL, [this] { PollShutdown(); });
    return true;
}

void App::NodeThread()
{
    util::ThreadRename("webui-node");
    bool clean{true};
    try {
        if (IntroIfNeeded()) StartNode();
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        Fatal("Runaway exception", strprintf("A fatal error occurred. %s can no longer continue safely and will quit.\n\n%s", CLIENT_NAME, e.what()));
        clean = false;
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
        Fatal("Runaway exception", strprintf("A fatal error occurred. %s can no longer continue safely and will quit.", CLIENT_NAME));
        clean = false;
    }

    bool started;
    {
        WAIT_LOCK(m_mutex, lock);
        // Without a node that runs (the intro was cancelled, an error, another app runs) the app ends at
        // once, unless it shows an error: then when the user closes it.
        if (!m_node_started && m_phase != "fatal") m_shutdown_requested = true;
        m_cv.wait(lock, [&]() EXCLUSIVE_LOCKS_REQUIRED(m_mutex) { return m_shutdown_requested; });
        started = m_node_started && clean;
    }

    for (auto& handler : m_node_handlers) handler->disconnect();
    if (started) {
        SetPhase("shutdown");
        // Let the page show that the node stops before the work starts.
        m_hub->Flush();
        if (m_node_events) m_node_events->Stop();
        m_bridge->Stop();
        if (m_instance) m_instance->Stop();
        LogInfo("webui: shutting the node down");
        m_node->appShutdown();
        LogInfo("webui: node stopped");
    } else {
        if (m_node_events) m_node_events->Stop();
        m_bridge->Stop();
        if (m_instance) m_instance->Stop();
    }
    m_window->Quit();
}

std::optional<std::string> App::Choose(const Window::FileDialog& dialog)
{
    std::optional<std::string> chosen;
    bool from_test{false};
#ifdef WEBUI_TEST_HOOKS
    {
        LOCK(m_mutex);
        if (!m_test_files.empty()) {
            const UniValue answer{m_test_files.front()};
            m_test_files.pop_front();
            if (answer.isStr()) chosen = answer.get_str();
            from_test = true;
        }
    }
#endif
    if (!from_test) {
        auto done{std::make_shared<std::promise<std::optional<std::string>>>()};
        m_window->Post([this, dialog, done] { m_window->ChooseFile(dialog, [done](std::optional<std::string> path) { done->set_value(std::move(path)); }); });
        auto future{done->get_future()};
        // The user takes the time needed; the app may stop meanwhile.
        while (future.wait_for(200ms) != std::future_status::ready) {
            LOCK(m_mutex);
            if (m_shutdown_requested) return std::nullopt;
        }
        chosen = future.get();
    }
    if (!chosen) return std::nullopt;
    if (!dialog.folder) {
        LOCK(m_mutex);
        (dialog.save ? m_writable : m_readable).insert(*chosen);
    }
    return chosen;
}

bool App::Confirm(const Window::ConfirmDialog& dialog)
{
#ifdef WEBUI_TEST_HOOKS
    {
        LOCK(m_mutex);
        if (!m_test_confirms.empty()) {
            const bool answer{m_test_confirms.front()};
            m_test_confirms.pop_front();
            m_test_confirmed.push_back(dialog);
            return answer;
        }
        if (!m_test_script.empty()) {
            // A test that did not expect the question: declined, and kept for test.confirmed (never a dialog
            // on the desktop of whoever runs the tests).
            m_test_confirmed.push_back(dialog);
            return false;
        }
    }
#endif
    auto done{std::make_shared<std::promise<bool>>()};
    m_window->Post([this, dialog, done] { m_window->Confirm(dialog, [done](bool yes) { done->set_value(yes); }); });
    auto future{done->get_future()};
    while (future.wait_for(200ms) != std::future_status::ready) {
        LOCK(m_mutex);
        if (m_shutdown_requested) return false;
    }
    return future.get();
}

UniValue App::NetworkPrefs()
{
    std::string network;
    {
        LOCK(m_mutex);
        network = m_network.empty() ? ChainTypeToString(m_args.GetChainType()) : m_network;
    }
    return m_config->Prefs(network);
}

bool App::PrefFlag(const std::string& key, bool fallback)
{
    const UniValue prefs{NetworkPrefs()};
    const UniValue& value{prefs.find_value(key)};
    return value.isBool() ? value.get_bool() : fallback;
}

void App::UpdateTray()
{
    std::string label;
    bool ready;
    {
        LOCK(m_mutex);
        label = NetworkLabel(m_network);
        ready = m_phase == "ready";
    }
    Window::Tray tray;
    // As BitcoinGUI: the icon shows unless the user turned it off (fHideTrayIcon).
    tray.visible = ready && PrefFlag("showTray", true);
    tray.tooltip = strprintf("%s client", CLIENT_NAME) + (label.empty() ? "" : " [" + label + "]");
    tray.menu = {
        {"toggle", "Show / Hide"},
        {"", "", true, true},
        {"send", "Send"},
        {"receive", "Receive"},
        {"", "", true, true},
        {"sign", "Sign message…"},
        {"verify", "Verify message…"},
        {"", "", true, true},
        {"options", "Options…"},
        {"node", "Node window"},
        {"", "", true, true},
        {"quit", "Quit"},
    };
    m_window->SetTray(tray);
}

void App::OnTrayMenu(const std::string& id)
{
    if (id == "quit") {
        RequestShutdown();
        return;
    }
    if (id == "toggle") {
        if (m_window->IsVisible()) m_window->Hide();
        else m_window->Present();
        return;
    }
    static const std::map<std::string, std::pair<std::string, std::string>> pages{
        {"send", {"send", ""}},
        {"receive", {"receive", ""}},
        {"sign", {"messages", "sign"}},
        {"verify", {"messages", "verify"}},
        {"options", {"settings", "main"}},
        {"node", {"node", "info"}},
    };
    const auto page{pages.find(id)};
    if (page == pages.end()) return;
    m_window->Present();
    UniValue data{UniValue::VOBJ};
    data.pushKV("page", page->second.first);
    data.pushKV("tab", page->second.second);
    m_hub->Emit("navigate", std::move(data));
}

void App::UndoOptions(interfaces::Node& node, const std::string& why)
{
    const size_t count{m_options_journal->Undo(node, [this](bool restart) {
        UniValue change{UniValue::VOBJ};
        change.pushKV("restartRequired", restart ? UniValue{true} : UniValue{});
        std::string network;
        {
            LOCK(m_mutex);
            network = m_network;
        }
        m_config->UpdatePrefs(network, change);
    })};
    if (count > 0) LogInfo("webui: %d changes of the options taken back (%s)", count, why);
}

void App::RegisterHostMethods()
{
    Bridge& b{*m_bridge};

    b.RegisterHost("app.hello", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        UniValue dialogs{UniValue::VARR};
        bool first_hello;
        {
            LOCK(m_mutex);
            out.pushKV("phase", m_phase);
            out.pushKV("phaseData", m_phase_data);
            for (const auto& [id, dialog] : m_dialogs) dialogs.push_back(dialog);
            first_hello = !m_page_ready;
            m_page_ready = true;
        }
        out.pushKV("dialogs", std::move(dialogs));
        out.pushKV("info", Info());
        m_hub->SetReady(true);
#ifdef WEBUI_TEST_HOOKS
        const std::string script_path{m_test_script};
        if (first_hello && !script_path.empty()) {
            std::ifstream file{static_cast<const std::filesystem::path&>(fs::PathFromString(script_path))};
            std::stringstream script;
            script << file.rdbuf();
            // A test hook: the script given on the command line (a debug option) runs in the page.
            // It marks that it ran: a script that does not parse (or throws at its start) runs not at
            // all, and the page would say nothing; the check after it says so and ends the app.
            const std::string code{script.str() + "\n;window.__chainsTestScriptRan = true;\n"};
            const std::string check{"if (!window.__chainsTestScriptRan) {"
                                    " const say = (method, params) => window.__chains({ kind: 'host', method, params });"
                                    " say('test.log', { message: 'SCRIPT_ERROR the test script did not run: it does not parse, or throws at its start' })"
                                    ".finally(() => say('test.exit', { code: 1 })); }"};
            m_window->PostDelayed(100ms, [this, code] { m_window->Eval(code); });
            m_window->PostDelayed(3s, [this, check] { m_window->Eval(check); });
        }
#else
        (void)first_hello;
#endif
        return out;
    });
    b.RegisterHost("app.info", [this](const UniValue&) { return Info(); });
    // The language of the interface, with its catalog (the messages of the page), and the languages there are.
    b.RegisterHost("app.locale", [this](const UniValue&) {
        std::string language;
        {
            LOCK(m_mutex);
            language = m_language;
        }
        UniValue out{UniValue::VOBJ};
        out.pushKV("code", language);
        if (auto catalog{LoadCatalog(BuiltinAssets(), language)}) {
            out.pushKV("name", catalog->find_value("name"));
            out.pushKV("rtl", catalog->find_value("rtl"));
            out.pushKV("messages", catalog->find_value("gui"));
        } else {
            out.pushKV("name", "English");
            out.pushKV("rtl", false);
            out.pushKV("messages", UniValue{UniValue::VOBJ});
        }
        return out;
    });
    b.RegisterHost("app.languages", [](const UniValue&) { return Languages(BuiltinAssets()); });
    // The language option changed (Settings, applied): the page and the node follow it at once.
    b.RegisterHost("app.updateLanguage", [this](const UniValue&) {
        UpdateLanguage();
        LOCK(m_mutex);
        return UniValue{m_language};
    });
    b.RegisterHost("app.quit", [this](const UniValue&) {
        RequestShutdown();
        return UniValue{true};
    });
    // Stop, and start again with the same options (a change of the options needs it).
    b.RegisterHost("app.restart", [this](const UniValue&) {
        if (!m_test_script.empty()) throw HostError("No restart in the tests");
        m_restart_requested = true;
        RequestShutdown();
        return UniValue{true};
    });
    b.RegisterHost("app.help", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("usage", strprintf("Usage: %s [options] [URI]", webui::chain::APP));
        out.pushKV("options", m_args.GetHelpMessage());
        return out;
    });
    b.RegisterHost("app.about", [](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("name", CLIENT_NAME);
        out.pushKV("version", FormatFullVersion());
        out.pushKV("license", LicenseInfo());
        return out;
    });

    b.RegisterHost("intro.space", [](const UniValue& params) { return SpaceOf(ParamStr(params, "dir")); });
    b.RegisterHost("intro.choose", [this](const UniValue& params) {
        const std::string dir{ParamStr(params, "dir")};
        if (dir.empty()) throw HostError(BRIDGE_INVALID_REQUEST, "Choose a data directory");
        const UniValue prune{Param(params, "pruneGB")};
        const int64_t prune_gb{prune.isNum() ? prune.getInt<int64_t>() : 0};
        {
            LOCK(m_mutex);
            if (m_phase != "intro") throw HostError("The intro is over");
        }
        try {
            const fs::path path{fs::PathFromString(dir)};
            // If a new data directory has been created, make the wallets folder in it too.
            if (TryCreateDirectories(path)) TryCreateDirectories(path / "wallets");
        } catch (const fs::filesystem_error&) {
            throw HostError(strprintf("Error: Specified data directory \"%s\" cannot be created.", dir));
        }
        {
            LOCK(m_mutex);
            m_intro_choice = std::make_pair(dir, prune_gb > 0 ? PruneGBtoMiB(prune_gb) : int64_t{0});
        }
        m_cv.notify_all();
        return UniValue{true};
    });
    b.RegisterHost("intro.cancel", [this](const UniValue&) {
        {
            LOCK(m_mutex);
            m_intro_cancelled = true;
        }
        m_cv.notify_all();
        return UniValue{true};
    });

    b.RegisterHost("dialog.answer", [this](const UniValue& params) {
        const UniValue id{Param(params, "id")};
        if (!id.isNum()) throw HostError(BRIDGE_INVALID_REQUEST, "id: the id of the dialog is needed");
        {
            LOCK(m_mutex);
            if (!m_dialogs.contains(id.getInt<int64_t>())) return UniValue{false};
            m_answers[id.getInt<int64_t>()] = Param(params, "answer");
        }
        m_cv.notify_all();
        return UniValue{true};
    });

    b.RegisterHost("prefs.get", [this](const UniValue&) {
        std::string network;
        {
            LOCK(m_mutex);
            network = m_network.empty() ? ChainTypeToString(m_args.GetChainType()) : m_network;
        }
        return m_config->Prefs(network);
    });
    b.RegisterHost("prefs.set", [this](const UniValue& params) {
        if (!params.isObject()) throw HostError(BRIDGE_INVALID_REQUEST, "prefs.set takes an object of the preferences to change");
        std::string network;
        {
            LOCK(m_mutex);
            network = m_network.empty() ? ChainTypeToString(m_args.GetChainType()) : m_network;
        }
        UniValue prefs;
        try {
            prefs = m_config->UpdatePrefs(network, params);
        } catch (const std::runtime_error& e) {
            throw HostError(e.what());
        }
        if (params.exists("showTray")) m_window->Post([this] { UpdateTray(); });
        return prefs;
    });

    const auto need_node{[this]() -> interfaces::Node& {
        LOCK(m_mutex);
        if (!m_node_started || !m_node || m_phase != "ready") throw HostError(BRIDGE_NOT_READY, "The node is not running");
        return *m_node;
    }};
    b.RegisterHost("node.state", [this, need_node](const UniValue&) {
        need_node();
        return m_node_events->Snapshot();
    });
    b.RegisterHost("node.setNetworkActive", [need_node](const UniValue& params) {
        const UniValue active{Param(params, "active")};
        if (!active.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "active: true or false");
        need_node().setNetworkActive(active.get_bool());
        return UniValue{true};
    });
    b.RegisterHost("node.getSetting", [need_node](const UniValue& params) {
        interfaces::Node& node{need_node()};
        const std::string name{ParamStr(params, "name")};
        UniValue out{UniValue::VOBJ};
        out.pushKV("value", node.getPersistentSetting(name));
        out.pushKV("ignored", node.isSettingIgnored(name));
        return out;
    });
    b.RegisterHost("window.setWallet", [this](const UniValue& params) {
        const UniValue name{Param(params, "name")};
        {
            LOCK(m_mutex);
            m_wallet_title = name.isStr() ? name.get_str() : "";
        }
        UpdateTitle();
        return UniValue{true};
    });
    b.RegisterHost("window.minimize", [this](const UniValue&) {
        m_window->Minimize();
        return UniValue{true};
    }, Bridge::Thread::UI);
    // The integrated title bar of the page (doc/webui-layouts.md): its buttons, its drags, its setting.
    b.RegisterHost("window.state", [this](const UniValue&) { return WindowState(); }, Bridge::Thread::UI);
    b.RegisterHost("window.isMaximized", [this](const UniValue&) { return UniValue{m_window->IsMaximized()}; }, Bridge::Thread::UI);
    b.RegisterHost("window.toggleMaximize", [this](const UniValue&) {
        m_window->ToggleMaximize();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.close", [this](const UniValue&) {
        // As the close button of the system: minimized or to the tray if the user wants that, else the app stops.
        CloseRequested();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.startMove", [this](const UniValue&) {
        m_window->StartMove();
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.startResize", [this](const UniValue& params) {
        const auto edge{ParseWindowEdge(ParamStr(params, "edge"))};
        if (!edge) throw HostError(BRIDGE_INVALID_REQUEST, "edge: top, bottom, left, right, top-left, top-right, bottom-left or bottom-right");
        m_window->StartResize(*edge);
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("window.setTitleBar", [this](const UniValue& params) {
        const UniValue mode{Param(params, "mode")};
        if (!mode.isStr() || (mode.get_str() != "integrated" && mode.get_str() != "system")) throw HostError(BRIDGE_INVALID_REQUEST, "mode: integrated or system");
        m_config->SetValue("titleBar", mode);
        // At once where the platform can; otherwise at the next start (window.state says so).
        if (m_window->SupportsFrameless()) m_window->SetFrameless(ParseTitleBarMode(mode) == TitleBarMode::INTEGRATED);
        return WindowState();
    }, Bridge::Thread::UI);
    // Files: the user chooses them in a file chooser of the desktop; the page reads and writes only those.
    b.RegisterHost("file.choose", [this](const UniValue& params) {
        Window::FileDialog dialog;
        dialog.save = Param(params, "save").isBool() && Param(params, "save").get_bool();
        dialog.folder = Param(params, "folder").isBool() && Param(params, "folder").get_bool();
        dialog.title = Param(params, "title").isStr() ? Param(params, "title").get_str() : (dialog.save ? "Save" : "Open");
        if (Param(params, "name").isStr()) dialog.suggested_name = Param(params, "name").get_str();
        if (const UniValue& filters{Param(params, "filters")}; filters.isArray()) {
            for (const UniValue& f : filters.getValues()) {
                std::vector<std::string> patterns;
                if (f.find_value("patterns").isArray()) {
                    for (const UniValue& pattern : f.find_value("patterns").getValues()) {
                        if (pattern.isStr()) patterns.push_back(pattern.get_str());
                    }
                }
                dialog.filters.emplace_back(f.find_value("name").isStr() ? f.find_value("name").get_str() : "Files", std::move(patterns));
            }
        }
        const auto chosen{Choose(dialog)};
        return chosen ? UniValue{*chosen} : UniValue{};
    }, Bridge::Thread::LONG);
    b.RegisterHost("file.read", [this](const UniValue& params) {
        const std::string path{ParamStr(params, "path")};
        {
            LOCK(m_mutex);
            if (!m_readable.contains(path)) throw HostError(BRIDGE_INVALID_REQUEST, "Only a file chosen by the user can be read");
        }
        std::ifstream file{static_cast<const std::filesystem::path&>(fs::PathFromString(path)), std::ios::binary};
        if (!file) throw HostError(strprintf("Cannot open %s", path));
        file.seekg(0, std::ios::end);
        const auto size{static_cast<int64_t>(file.tellg())};
        // The largest file the page reads is a PSBT (MAX_FILE_SIZE_PSBT).
        if (size < 0 || size >= 100'000'000) throw HostError("The file must be smaller than 100 MB");
        file.seekg(0);
        std::string data(static_cast<size_t>(size), '\0');
        file.read(data.data(), size);
        UniValue out{UniValue::VOBJ};
        out.pushKV("path", path);
        out.pushKV("size", size);
        out.pushKV("base64", EncodeBase64(data));
        return out;
    });
    b.RegisterHost("file.write", [this](const UniValue& params) {
        const std::string path{ParamStr(params, "path")};
        {
            LOCK(m_mutex);
            if (!m_writable.contains(path)) throw HostError(BRIDGE_INVALID_REQUEST, "Only a file chosen by the user can be written");
        }
        std::string data;
        if (Param(params, "base64").isStr()) {
            auto bytes{DecodeBase64(Param(params, "base64").get_str())};
            if (!bytes) throw HostError(BRIDGE_INVALID_REQUEST, "base64: not base64");
            data.assign(bytes->begin(), bytes->end());
        } else {
            data = ParamStr(params, "text");
        }
        std::ofstream file{static_cast<const std::filesystem::path&>(fs::PathFromString(path)), std::ios::binary | std::ios::trunc};
        if (!file || !file.write(data.data(), data.size()) || !(file.flush())) throw HostError(strprintf("There was an error trying to save to %s.", path));
        return UniValue{path};
    });

    b.RegisterHost("clipboard.write", [this](const UniValue& params) {
        m_window->SetClipboard(ParamStr(params, "text"));
        return UniValue{true};
    }, Bridge::Thread::UI);
    b.RegisterHost("clipboard.read", [this](const UniValue&) {
        const auto text{m_window->GetClipboard()};
        return text ? UniValue{*text} : UniValue{};
    }, Bridge::Thread::UI);

    // The node window and the tools of chains-qt.
    m_node_api = std::make_unique<NodeApi>(need_node, [this](const std::string& path) {
        LOCK(m_mutex);
        return m_readable.contains(path);
    }, [this](const Window::ConfirmDialog& dialog) { return Confirm(dialog); });
    m_node_api->Register(b);

#if WEBUI_SIDECHAIN
    // The pages of a sidechain: its mainchain node, through the node's own connection to it.
    m_sidechain_api = std::make_unique<SidechainApi>(need_node, [this](const Window::ConfirmDialog& dialog) { return Confirm(dialog); });
    m_sidechain_api->Register(b);
#else
    // The nodes of sidechains run from here (the Sidechain Nodes window).
    const auto sidechain_key{[this] {
        LOCK(m_mutex);
        return "sidechainNodes:" + (m_network.empty() ? ChainTypeToString(m_args.GetChainType()) : m_network);
    }};
    m_sidechain_nodes = std::make_unique<SidechainNodes>(
        m_args,
        [this, sidechain_key] {
            UniValue value{m_config->Value(sidechain_key())};
            return value.isObject() ? value : UniValue{UniValue::VOBJ};
        },
        [this, sidechain_key](const UniValue& value) { m_config->SetValue(sidechain_key(), value); },
        [this](const std::string& title, const std::string& start) {
            Window::FileDialog dialog;
            dialog.folder = true;
            dialog.title = title;
            dialog.start_dir = start;
            return Choose(dialog);
        },
        [this](const std::string& title, const std::string& message, const std::string& accept) {
            return Confirm({.title = title, .message = message, .accept = accept});
        });
    m_sidechain_nodes->Register(b);
#endif
    // The host methods of this chain's own pages (local.cpp).
    local::RegisterHost(b, need_node);

    // The options of the node (OptionsModel), in settings.json; the flag of a restart in the preferences.
    const auto options{[this, need_node]() {
        return NodeOptions{need_node(), m_args, [this](std::optional<bool> set) {
            if (set) {
                UniValue change{UniValue::VOBJ};
                change.pushKV("restartRequired", *set);
                std::string network;
                {
                    LOCK(m_mutex);
                    network = m_network;
                }
                m_config->UpdatePrefs(network, change);
                return *set;
            }
            return PrefFlag("restartRequired", false);
        }, [this](const std::string& setting) {
            interfaces::Node* node;
            {
                LOCK(m_mutex);
                node = m_node.get();
            }
            if (node) m_options_journal->Record(*node, setting, PrefFlag("restartRequired", false));
        }, [this](const std::string& title, const std::string& current) -> std::optional<std::string> {
            // A program the node runs: chosen by the user in the file chooser of the desktop, then confirmed
            // in a dialog of the desktop. Never a text of the page.
            Window::FileDialog dialog;
            dialog.title = title;
            if (!current.empty()) dialog.start_dir = fs::PathToString(fs::PathFromString(current).parent_path());
            const auto path{Choose(dialog)};
            if (!path) return std::nullopt;
            const std::string question{strprintf("%s\n\nProgram: %s\n\nThe node will run this program with your rights whenever it needs it. "
                                                 "Accept only if you know what it is.", title, *path)};
            if (!Confirm({.title = title, .message = question, .accept = "Use this program"})) return std::nullopt;
            return path;
        }, [this](const std::string& title, const std::string& current) -> std::optional<std::string> {
            // A file the node reads (the cookie file of the mainchain node): chosen by the user in the file
            // chooser of the desktop, never a text of the page.
            Window::FileDialog dialog;
            dialog.title = title;
            if (!current.empty()) dialog.start_dir = fs::PathToString(fs::PathFromString(current).parent_path());
            return Choose(dialog);
        }, [this](const Window::ConfirmDialog& dialog) { return Confirm(dialog); }};
    }};
    const auto with_pending{[this](UniValue out) {
        // The changes waiting for Apply (the restart flag they may have set is not one).
        out.pushKV("pending", static_cast<int64_t>(m_options_journal->Pending()));
        return out;
    }};
    b.RegisterHost("options.get", [options, with_pending](const UniValue&) { return with_pending(options().Get()); });
    b.RegisterHost("options.apply", [this, need_node, options, with_pending](const UniValue&) {
        need_node();
        m_options_journal->Commit();
        return with_pending(options().Get());
    });
    b.RegisterHost("options.cancel", [this, need_node, options, with_pending](const UniValue&) {
        UndoOptions(need_node(), "cancelled");
        return with_pending(options().Get());
    });
    b.RegisterHost("options.set", [options, with_pending](const UniValue& params) {
        // A long call: some changes wait for the answer of a confirmation of the desktop.
        try {
            return with_pending(options().Set(ParamStr(params, "name"), Param(params, "value")));
        } catch (const std::runtime_error& e) {
            throw HostError(BRIDGE_INVALID_REQUEST, e.what());
        }
    }, Bridge::Thread::LONG);
    b.RegisterHost("options.chooseProgram", [options, with_pending](const UniValue& params) {
        // A setting that names a program (the external signer): through the file chooser and a confirmation
        // of the desktop. Null when the user cancelled.
        try {
            auto out{options().ChooseProgram(ParamStr(params, "name"))};
            return out.isNull() ? out : with_pending(std::move(out));
        } catch (const std::runtime_error& e) {
            throw HostError(BRIDGE_INVALID_REQUEST, e.what());
        }
    }, Bridge::Thread::LONG);
    b.RegisterHost("options.chooseFile", [options, with_pending](const UniValue& params) {
        // A setting that names a file the node reads (a sidechain's mainchain cookie or password file): through
        // the file chooser of the desktop. Null when the user cancelled.
        try {
            auto out{options().ChooseFile(ParamStr(params, "name"))};
            return out.isNull() ? out : with_pending(std::move(out));
        } catch (const std::runtime_error& e) {
            throw HostError(BRIDGE_INVALID_REQUEST, e.what());
        }
    }, Bridge::Thread::LONG);
    b.RegisterHost("options.validProxy", [](const UniValue& params) { return UniValue{NodeOptions::ValidProxyAddress(ParamStr(params, "address"))}; });
    b.RegisterHost("options.reset", [this, need_node](const UniValue&) {
        // As OptionsModel::Reset: settings.json backed up and reset, the settings of the app backed up and
        // cleared (but for the data directory), the intro shown at the next start; then the app shuts down.
        interfaces::Node& node{need_node()};
        // Not while a change of the options waits for a confirmation of the desktop.
        std::unique_lock changes{NodeOptions::ChangesMutex()};
        node.resetSettings();
        std::string network;
        {
            LOCK(m_mutex);
            network = m_network;
        }
        try {
            const fs::path backup{m_args.GetDataDirNet() / "guisettings.json.bak"};
            std::ofstream file{backup.std_path(), std::ios::trunc};
            file << m_config->Prefs(network).write(2) << "\n";
        } catch (const std::exception& e) {
            LogWarning("webui: cannot back up the settings of the app: %s", e.what());
        }
        m_config->ResetPrefs(network);
        m_options_journal->Commit();
        m_config->SetResetPending(true);
        if (NodeOptions::StartAtLogin(m_args)) NodeOptions::SetStartAtLogin(m_args, false);
        changes.unlock();
        RequestShutdown();
        return UniValue{true};
    });

    b.RegisterHost("node.paths", [this, need_node](const UniValue&) {
        need_node();
        UniValue out{UniValue::VOBJ};
        out.pushKV("datadir", fs::PathToString(m_args.GetDataDirNet()));
        out.pushKV("blocksdir", fs::PathToString(m_args.GetBlocksDirPath()));
        out.pushKV("debuglog", fs::PathToString(LogInstance().m_file_path));
        out.pushKV("config", fs::PathToString(m_args.GetConfigFilePath()));
        return out;
    });
    // Open the debug log or the configuration file with the application of the desktop (GUIUtil::openDebugLogfile,
    // openBitcoinConf), or a web page of a third-party explorer (http and https only).
    b.RegisterHost("app.open", [this, need_node](const UniValue& params) {
        const std::string what{ParamStr(params, "what")};
        std::string uri;
        if (what == "debuglog" || what == "config") {
            need_node();
            fs::path path{what == "debuglog" ? LogInstance().m_file_path : m_args.GetConfigFilePath()};
            if (what == "config") {
                // The file is made if it is not there.
                std::ofstream file{path.std_path(), std::ios_base::app};
                if (!file.good()) throw HostError("The configuration file could not be opened.");
            } else if (!fs::exists(path)) {
                throw HostError("There is no debug log file yet.");
            }
            uri = "file://";
            for (const auto& part : util::SplitString(fs::PathToString(fs::absolute(path)), '/')) {
                if (!part.empty()) uri += "/" + PercentEncode(part);
            }
        } else if (what == "url") {
            // The page names the address: one that is not a link of the app's own is opened on the user's
            // word, given to the desktop, with its host first (a page could otherwise send what it read away).
            uri = ParamStr(params, "url");
            std::optional<Window::ConfirmDialog> dialog;
            try {
                dialog = guard::OpenUrlConfirmation(uri);
            } catch (const std::invalid_argument& e) {
                throw HostError(BRIDGE_INVALID_REQUEST, e.what());
            }
            if (dialog && !Confirm(*dialog)) return UniValue{false};
        } else {
            throw HostError(BRIDGE_INVALID_REQUEST, "what: debuglog, config or url");
        }
        auto done{std::make_shared<std::promise<bool>>()};
        m_window->Post([this, uri, done] { done->set_value(m_window->OpenExternal(uri)); });
        auto future{done->get_future()};
        if (future.wait_for(10s) != std::future_status::ready) return UniValue{false};
        const bool ok{future.get()};
        if (!ok && what == "config") throw HostError("The configuration file could not be opened.");
        return UniValue{ok};
    }, Bridge::Thread::LONG);

    // Notifications of the desktop (Notificator) and the tray.
    b.RegisterHost("app.notify", [this](const UniValue& params) {
        const std::string title{ParamStr(params, "title")};
        const std::string body{Param(params, "body").isStr() ? Param(params, "body").get_str() : ""};
        const std::string level{Param(params, "level").isStr() ? Param(params, "level").get_str() : "info"};
        {
            LOCK(m_mutex);
            UniValue sent{UniValue::VOBJ};
            sent.pushKV("title", title);
            sent.pushKV("body", body);
            sent.pushKV("level", level);
            m_notifications.push_back(std::move(sent));
            if (m_notifications.size() > 100) m_notifications.erase(m_notifications.begin());
        }
        m_window->Post([this, title, body, level] { m_window->Notify(title, body, level); });
        return UniValue{true};
    });
    b.RegisterHost("tray.state", [this](const UniValue&) {
        UniValue out{UniValue::VOBJ};
        out.pushKV("available", m_window->TrayAvailable());
        out.pushKV("visible", PrefFlag("showTray", true));
        out.pushKV("windowVisible", m_window->IsVisible());
        return out;
    }, Bridge::Thread::UI);

#ifdef ENABLE_WALLET
    // The wallet functions (the wallet pages of chains-qt).
    m_wallet_api = std::make_unique<WalletApi>(need_node, [this](const std::string& path, bool write) {
        LOCK(m_mutex);
        return (write ? m_writable : m_readable).contains(path);
    });
    m_wallet_api->Register(b);
#endif

#ifdef WEBUI_TEST_HOOKS
    if (!m_test_script.empty()) {
        // The hooks of the tests of the app: only in a build for the tests (WEBUI_TEST_HOOKS), and only
        // with the debug option -webuitestscript of the command line.
        b.RegisterHost("test.log", [](const UniValue& params) {
            const std::string message{params.isObject() && params.find_value("message").isStr() ? params.find_value("message").get_str() : params.write()};
            LogInfo("webui test: %s", message);
            tfm::format(std::cout, "webui test: %s\n", message);
            std::cout.flush();
            return UniValue{true};
        });
        b.RegisterHost("test.snapshot", [this](const UniValue& params) {
            const std::string name{ParamStr(params, "name")};
            if (name.empty() || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") != std::string::npos) {
                throw HostError(BRIDGE_INVALID_REQUEST, "name: letters, digits, '.', '_' and '-'");
            }
            const fs::path dir{fs::PathFromString(m_test_shots)};
            if (dir.empty()) throw HostError("No -webuitestshots folder");
            fs::create_directories(dir);
            const std::string path{fs::PathToString(dir / fs::PathFromString(name + ".png"))};
            auto done{std::make_shared<std::promise<bool>>()};
            m_window->Post([this, path, done] { m_window->Snapshot(path, [done](bool ok) { done->set_value(ok); }); });
            auto future{done->get_future()};
            if (future.wait_for(30s) != std::future_status::ready || !future.get()) throw HostError("The snapshot failed");
            return UniValue{path};
        }, Bridge::Thread::LONG);
        b.RegisterHost("test.fileAnswer", [this](const UniValue& params) {
            // The answer of the next file chooser: a path, or null for "cancelled".
            LOCK(m_mutex);
            m_test_files.push_back(Param(params, "path"));
            return UniValue{true};
        });
        b.RegisterHost("test.confirmAnswer", [this](const UniValue& params) {
            // The answer of the next confirmation of the desktop (Window::Confirm): true for accepted.
            const UniValue& yes{Param(params, "yes")};
            if (!yes.isBool()) throw HostError(BRIDGE_INVALID_REQUEST, "yes: true or false");
            LOCK(m_mutex);
            m_test_confirms.push_back(yes.get_bool());
            return UniValue{true};
        });
        b.RegisterHost("test.refused", [this](const UniValue&) {
            // The RPC methods the policy refused to the pages (a page that calls one fails in a release).
            UniValue out{UniValue::VARR};
            for (const std::string& m : m_bridge->Refused()) out.push_back(m);
            return out;
        });
        b.RegisterHost("test.confirmed", [this](const UniValue&) {
            // What the confirmations answered by the tests asked, in order.
            LOCK(m_mutex);
            UniValue out{UniValue::VARR};
            for (const auto& d : m_test_confirmed) {
                UniValue entry{UniValue::VOBJ};
                entry.pushKV("title", d.title);
                entry.pushKV("message", d.message);
                out.push_back(std::move(entry));
            }
            return out;
        });
        b.RegisterHost("test.notifications", [this](const UniValue&) {
            LOCK(m_mutex);
            UniValue out{UniValue::VARR};
            for (const auto& n : m_notifications) out.push_back(n);
            return out;
        });
        // What the user does with the window and the tray, done by the tests.
        b.RegisterHost("test.window", [this](const UniValue& params) {
            const std::string action{ParamStr(params, "action")};
            if (action == "trayMenu") m_window->Post([this, id = ParamStr(params, "id")] { OnTrayMenu(id); });
            else if (action == "trayActivate") m_window->Post([this] { if (m_window->IsVisible()) m_window->Hide(); else m_window->Present(); });
            else if (action == "minimize") m_window->Post([this] { m_window->Minimize(); });
            else if (action == "close") m_window->Post([this] { CloseRequested(); });
            else if (action == "present") m_window->Post([this] { m_window->Present(); });
            return UniValue{true};
        });
        b.RegisterHost("test.exit", [this](const UniValue& params) {
            const UniValue code{Param(params, "code")};
            {
                LOCK(m_mutex);
                m_exit_code = code.isNum() ? code.getInt<int>() : EXIT_FAILURE;
            }
            RequestShutdown();
            return UniValue{true};
        });
    }
#endif
}

int App::Run()
{
    g_ui_thread = std::this_thread::get_id();
    const std::string config_path{m_config_path};
    m_config = std::make_unique<AppConfig>(config_path.empty() ? AppConfig::DefaultPath() : fs::PathFromString(config_path));
    m_config->Load();
    // The language from the command line or the system, until settings.json is read (-lang there).
    {
        const std::string language{ChooseLanguage(BuiltinAssets(), m_args.GetArg("-lang", ""))};
        {
            LOCK(m_mutex);
            m_language = language;
        }
        SetCoreCatalog(LoadCatalog(BuiltinAssets(), language));
    }
    m_options_journal = std::make_unique<OptionsJournal>(*m_config, m_args, [this] {
        LOCK(m_mutex);
        return "optionsUndo:" + (m_network.empty() ? ChainTypeToString(m_args.GetChainType()) : m_network);
    });

#ifdef WIN32
    // The payment URIs of the chain (the installer of chains-qt registers them for all users).
    if (m_args.GetBoolArg("-webuiregisteruri", true) && m_args.GetArg("-webuitestscript", "").empty()) RegisterUriScheme(std::string{webui::chain::URI_SCHEME}, CLIENT_NAME);
#endif

    Window::Options options;
    options.title = CLIENT_NAME;
    options.devtools = m_devtools;
    // The size the window had last time (MainWindowGeometry).
    if (const UniValue geometry{m_config->Value("window")}; geometry.isObject()) {
        const UniValue& width{geometry.find_value("width")};
        const UniValue& height{geometry.find_value("height")};
        if (width.isNum() && height.isNum() && width.getInt<int>() >= 400 && height.getInt<int>() >= 300) {
            options.width = std::min(width.getInt<int>(), 10000);
            options.height = std::min(height.getInt<int>(), 10000);
        }
        options.maximized = geometry.find_value("maximized").isBool() && geometry.find_value("maximized").get_bool();
    }
    // The integrated title bar unless the user chose the one of the system (Settings → Appearance).
    options.frameless = ParseTitleBarMode(m_config->Value("titleBar")) == TitleBarMode::INTEGRATED;
    Window::Callbacks callbacks;
    callbacks.close_requested = [this] { CloseRequested(); };
    callbacks.minimized = [this] {
        // As BitcoinGUI::changeEvent: minimized to the tray, if the user wants that and there is a tray.
        if (PrefFlag("minimizeToTray", false) && PrefFlag("showTray", true) && m_window->TrayAvailable()) {
            m_window->Post([this] { m_window->Hide(); });
        }
    };
    callbacks.maximized_changed = [this](bool maximized) {
        UniValue data{UniValue::VOBJ};
        data.pushKV("maximized", maximized);
        if (m_hub) m_hub->Emit("window", std::move(data), "window");
    };
    callbacks.tray_activated = [this] {
        if (m_window->IsVisible()) m_window->Hide();
        else m_window->Present();
    };
    callbacks.tray_menu = [this](const std::string& id) { OnTrayMenu(id); };
    callbacks.session_ending = [this] { RequestShutdown(); };
    callbacks.call = [this](const std::string& id, const std::string& args) { m_bridge->Handle(id, args); };
    m_window = Window::Create(options, std::move(callbacks));

    m_hub = std::make_unique<EventHub>(
        [this](const std::string& batch) { DeliverEvents(batch); },
        [this](std::chrono::milliseconds delay, std::function<void()> fn) {
            if (delay.count() == 0) m_window->Post(std::move(fn));
            else m_window->PostDelayed(delay, std::move(fn));
        });
    m_bridge = std::make_unique<Bridge>(
        [this](const std::string& id, int status, const std::string& json) { m_window->Reply(id, status, json); },
        [this](std::function<void()> fn) { m_window->Post(std::move(fn)); });
    // An exception a host method did not expect: the internal error dialog of chains-qt (handleNonFatalException).
    m_bridge->SetInternalError([this](const std::string& method, const std::string& what) {
        LogWarning("webui: internal error in %s: %s", method, what);
        UniValue dialog{UniValue::VOBJ};
        dialog.pushKV("kind", "message");
        dialog.pushKV("level", "error");
        dialog.pushKV("title", "Internal error");
        dialog.pushKV("message", strprintf("An internal error occurred. %s will attempt to continue safely. This is an unexpected bug which can be reported as described below.", CLIENT_NAME));
        UniValue details{UniValue::VARR};
        details.push_back(strprintf("%s: %s", method, what));
        details.push_back(CLIENT_BUGREPORT);
        dialog.pushKV("details", std::move(details));
        m_hub->Emit("dialog", std::move(dialog));
    });
    // The pages call the RPC methods they need, and no other (the console runs any command, a command that
    // does more than read on the user's word given to the desktop). They keep to it in the tests too: the
    // policy that ships is the one tested (audit port-1 #5). A test script (-webuitestscript, in a build with
    // WEBUI_TEST_HOOKS) sets the node up (wallets, blocks) with requests of the kind "testrpc", which a
    // release does not take.
    m_bridge->SetRpcPolicy(PageRpcPolicy);
#ifdef WEBUI_TEST_HOOKS
    if (!m_test_script.empty()) m_bridge->OpenTestRpc();
#endif
    RegisterHostMethods();

    m_node_thread = std::thread([this] { NodeThread(); });
    m_window->Run();

    // The loop ends after the node stopped (Quit), or when the window went away without asking.
    RequestShutdown();
    if (const Window::Geometry geometry{m_window->GetGeometry()}; geometry.width > 0 && geometry.height > 0) {
        UniValue saved{UniValue::VOBJ};
        saved.pushKV("width", geometry.width);
        saved.pushKV("height", geometry.height);
        saved.pushKV("maximized", geometry.maximized);
        m_config->SetValue("window", saved);
    }
    if (m_node_thread.joinable()) m_node_thread.join();

    m_bridge.reset();
    int status;
    bool started;
    {
        LOCK(m_mutex);
        status = m_exit_code.value_or(EXIT_SUCCESS);
        started = m_node_started;
    }
    if (m_node && started && status == EXIT_SUCCESS) status = m_node->getExitStatus();
    for (auto& handler : m_node_handlers) handler->disconnect();
    m_node_events.reset();
    m_window.reset();
    return status;
}

} // namespace webui
