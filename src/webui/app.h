// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WEBUI_APP_H
#define BITCOIN_WEBUI_APP_H

#include <sync.h>
#include <univalue.h>
#include <util/fs.h>
#include <webui/window.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

class ArgsManager;
namespace interfaces {
class Handler;
class Init;
class Node;
} // namespace interfaces

namespace webui {

class AppConfig;
class Bridge;
class NodeApi;
class SidechainApi;
class SidechainNodes;
class EventHub;
class NodeEvents;
class OptionsJournal;
class SingleInstance;
class WalletApi;

/** The options of the app on top of the ones of the node (the GUI options of chains-qt, and the debug ones of chains-app). */
void SetupUIArgs(ArgsManager& args);

/**
 * chains-app: the node, in this process, with its interface in a web view.
 *
 * The UI thread runs the window. The node starts and stops on a thread of its
 * own (as InitExecutor does for the Qt GUI): the intro, the configuration,
 * init, then, once a shutdown is asked for, the shutdown. The page follows
 * along through events, and asks the node through the bridge.
 *
 * Phases, as the page sees them: "loading" (before the window shows), "intro"
 * (choose the data directory), "init" (the node starts), "ready",
 * "shutdown" (the node stops), "fatal" (an error the app cannot go on after).
 */
class App
{
public:
    App(interfaces::Init& init, ArgsManager& args, std::vector<std::string> uris);
    ~App();

    /** Run the app; returns the exit status of the process. */
    int Run();

    /** Ask for the app to stop (from any thread). */
    void RequestShutdown() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Whether the user asked for the app to start again after it stopped (Settings). */
    bool RestartRequested() const { return m_restart_requested.load(); }

    /** The user closes the window: the app stops, or the window is minimized (options). UI thread. */
    void CloseRequested() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

private:
    void RegisterHostMethods();
    void NodeThread() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool IntroIfNeeded() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool StartNode() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void SetPhase(const std::string& phase, UniValue data = UniValue{UniValue::VOBJ}) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    UniValue Info() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The title bar and the state of the window, for the page (window.state). UI thread. */
    UniValue WindowState();
    void UpdateTitle() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void PollShutdown() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void DeliverEvents(const std::string& batch_json);
    void OnInstanceMessage(const std::string& line) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** The preferences of the interface on the network of the node (or of the command line before it is known). */
    UniValue NetworkPrefs() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    bool PrefFlag(const std::string& key, bool fallback) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Show the icon in the tray as the preferences say (BitcoinGUI::setTrayIconVisible). UI thread. */
    void UpdateTray() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** An item of the menu of the tray was chosen. UI thread. */
    void OnTrayMenu(const std::string& id) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * A file chooser of the desktop (not on the UI thread): the path chosen, or nothing. The tests
     * answer it in place of the user. The page may use the file afterwards.
     */
    std::optional<std::string> Choose(const Window::FileDialog& dialog) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /**
     * Ask the user in the dialog window of the host (Window::Ask: in the look of the app, never of the page), not on a
     * thread of the UI; false when cancelled or quitting.
     */
    bool Confirm(const Window::ConfirmDialog& dialog) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** A confirmation as the dialog window shows it: its text, and the look of the app now (the preferences of the network shown). */
    dialog::Request DialogRequest(const Window::ConfirmDialog& confirm) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /**
     * Show a dialog in the page and wait for its answer (on a thread that is not the UI thread).
     * Without an answer (the app stops), `fallback`.
     */
    UniValue Ask(UniValue dialog, UniValue fallback) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    /** Take back the changes of the options never applied (OptionsJournal), with the restart flag they set. */
    void UndoOptions(interfaces::Node& node, const std::string& why) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    /** Choose the language again (-lang, the option, the system), for the page and the node; an event if it changed. */
    void UpdateLanguage() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);
    void Fatal(const std::string& title, const std::string& message, const std::vector<std::string>& details = {}) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex);

    interfaces::Init& m_init;
    ArgsManager& m_args;
    //! The debug options, from the command line only.
    const bool m_devtools;
    const std::string m_test_script;
    const std::string m_test_shots;
    const std::string m_config_path;
    std::unique_ptr<AppConfig> m_config;
    //! The changes of the Settings page waiting for Apply.
    std::unique_ptr<OptionsJournal> m_options_journal;
    std::unique_ptr<interfaces::Node> m_node;
    std::unique_ptr<Window> m_window;
    std::unique_ptr<EventHub> m_hub;
    std::unique_ptr<Bridge> m_bridge;
    std::unique_ptr<NodeEvents> m_node_events;
    std::unique_ptr<SingleInstance> m_instance;
    //! The wallet functions of the page (null in a build without wallets).
    std::unique_ptr<WalletApi> m_wallet_api;
    //! The node window and the tools (console, peers, bans, deposit addresses, hashes, stamps).
    std::unique_ptr<NodeApi> m_node_api;
    //! The nodes of sidechains run from here.
    std::unique_ptr<SidechainNodes> m_sidechain_nodes;
    std::unique_ptr<SidechainApi> m_sidechain_api;
    std::thread m_node_thread;
    std::atomic<bool> m_restart_requested{false};
    //! Messages and questions of the node.
    std::vector<std::unique_ptr<interfaces::Handler>> m_node_handlers;

    Mutex m_mutex;
    std::condition_variable m_cv;
    std::string m_phase GUARDED_BY(m_mutex){"loading"};
    UniValue m_phase_data GUARDED_BY(m_mutex){UniValue::VOBJ};
    std::string m_network GUARDED_BY(m_mutex);
    std::string m_wallet_title GUARDED_BY(m_mutex);
    //! The language of the interface ("" for English).
    std::string m_language GUARDED_BY(m_mutex);
    bool m_shutdown_requested GUARDED_BY(m_mutex){false};
    bool m_page_ready GUARDED_BY(m_mutex){false};
    std::vector<std::string> m_pending_uris GUARDED_BY(m_mutex);
    //! The intro's choice: the data directory, and the prune setting in MiB (0: off).
    std::optional<std::pair<std::string, int64_t>> m_intro_choice GUARDED_BY(m_mutex);
    bool m_intro_cancelled GUARDED_BY(m_mutex){false};
    //! Dialogs waiting for an answer of the page, by id, and their answers.
    std::map<int64_t, UniValue> m_dialogs GUARDED_BY(m_mutex);
    std::map<int64_t, UniValue> m_answers GUARDED_BY(m_mutex);
    int64_t m_next_dialog GUARDED_BY(m_mutex){1};
    bool m_node_started GUARDED_BY(m_mutex){false};
    std::optional<int> m_exit_code GUARDED_BY(m_mutex);
    //! Files the user chose in a file chooser: the only ones the page may read, or write.
    std::set<std::string> m_readable GUARDED_BY(m_mutex);
    std::set<std::string> m_writable GUARDED_BY(m_mutex);
    //! Answers of the file choosers given by the tests (-webuitestscript), in place of the user.
    std::deque<UniValue> m_test_files GUARDED_BY(m_mutex);
    //! Answers of the confirmations of the host given by the tests (-webuitestscript), and what they asked: an answer
    //! given here, or the dialog shown (the dialog window, or the dialog of the system), for the test to answer there.
    enum class TestConfirm { YES, NO, SHOW, FALLBACK };
    std::deque<TestConfirm> m_test_confirms GUARDED_BY(m_mutex);
    std::vector<Window::ConfirmDialog> m_test_confirmed GUARDED_BY(m_mutex);
    //! The notifications sent to the desktop, for the tests.
    std::vector<UniValue> m_notifications GUARDED_BY(m_mutex);
};

} // namespace webui

#endif // BITCOIN_WEBUI_APP_H
