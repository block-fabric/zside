// Copyright (c) 2026 The Chains developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// chains-app: the Chains node with its interface in a native web view.

#include <bitcoin-build-config.h> // IWYU pragma: keep

#include <clientversion.h>
#include <compat/compat.h>
#include <common/args.h>
#include <common/license_info.h>
#include <common/system.h>
#include <init.h>
#include <interfaces/init.h>
#include <noui.h>
#include <node/interface_ui.h>
#include <tinyformat.h>
#include <util/exception.h>
#include <util/strencodings.h>
#include <util/threadnames.h>
#include <util/translation.h>
#include <util/syserror.h>
#include <webui/app.h>
#include <webui/chainconfig.h>
#include <webui/i18n.h>

#ifndef WIN32
#include <cerrno>
#include <unistd.h>
#endif

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

//! The messages of the node, in the language of the interface (the catalogs of chains-qt, webui/i18n.h).
const TranslateFn G_TRANSLATION_FUN{[](const char* message) { return webui::TranslateCore(message); }};

namespace {
//! The scheme of payment URIs (BIP 21) of this chain.
const std::string URI_SCHEME{std::string{webui::chain::URI_SCHEME} + ":"};

bool StartsWithScheme(const std::string& arg)
{
    if (arg.size() < URI_SCHEME.size()) return false;
    for (size_t i{0}; i < URI_SCHEME.size(); ++i) {
        if (ToLower(arg[i]) != URI_SCHEME[i]) return false;
    }
    return true;
}
} // namespace

MAIN_FUNCTION
{
    std::unique_ptr<interfaces::Init> init{interfaces::MakeGuiInit(argc, argv)};

    SetupEnvironment();
    util::ThreadSetInternalName("main");

    // Messages before the window is there go to the terminal (and the log), as with chainsd.
    btcsignals::scoped_connection handler_message_box{::uiInterface.ThreadSafeMessageBox.connect(noui_ThreadSafeMessageBox)};
    btcsignals::scoped_connection handler_question{::uiInterface.ThreadSafeQuestion.connect(noui_ThreadSafeQuestion)};
    btcsignals::scoped_connection handler_init_message{::uiInterface.InitMessage.connect(noui_InitMessage)};

    SetupServerArgs(gArgs, init->canListenIpc());
    webui::SetupUIArgs(gArgs);
    std::string error;
    if (!gArgs.ParseParameters(argc, argv, error)) {
        InitError(Untranslated(strprintf("Error parsing command line arguments: %s", error)));
        return EXIT_FAILURE;
    }

    // Payment URIs on the command line; anything else that is not an option is an error (as chains-qt).
    std::vector<std::string> uris;
    bool uri_seen{false};
    for (int i{1}; i < argc; ++i) {
        const std::string arg{argv[i]};
        bool invalid_token{!arg.starts_with("-")};
        if (StartsWithScheme(arg)) {
            invalid_token = false;
            uri_seen = true;
            uris.push_back(arg);
        }
        if (uri_seen && arg.starts_with("-")) {
            InitError(Untranslated(strprintf("Options ('%s') cannot follow a BIP-21 payment URI", arg)));
            return EXIT_FAILURE;
        }
        if (invalid_token) {
            InitError(Untranslated(strprintf("Command line contains unexpected token '%s', see %s -h for a list of options.", arg, webui::chain::APP)));
            return EXIT_FAILURE;
        }
    }

    if (HelpRequested(gArgs) || gArgs.GetBoolArg("-version", false)) {
        std::string usage{strprintf("%s version %s\n", CLIENT_NAME, FormatFullVersion())};
        if (gArgs.GetBoolArg("-version", false)) {
            usage += FormatParagraph(LicenseInfo());
        } else {
            usage += strprintf("\nThe node of %s with its wallet and its interface in one program.\n\n"
                               "Usage: %s [options] [URI]\n\n", CLIENT_NAME, webui::chain::APP);
            usage += gArgs.GetHelpMessage();
        }
        tfm::format(std::cout, "%s", usage);
        return EXIT_SUCCESS;
    }

    try {
        webui::App app{*init, gArgs, std::move(uris)};
        const int status{app.Run()};
#ifndef WIN32
        if (app.RestartRequested() && status == EXIT_SUCCESS) {
            // Start again as the user asked (Settings: restart), with the same options, once the node stopped.
            // The payment URIs given at start were handled already.
            std::vector<char*> args;
            for (int i{0}; i < argc; ++i) {
                if (!StartsWithScheme(argv[i])) args.push_back(argv[i]);
            }
            args.push_back(nullptr);
            execv("/proc/self/exe", args.data());
            tfm::format(std::cerr, "%s: could not start again: %s\n", CLIENT_NAME, SysErrorString(errno));
        }
#endif
        return status;
    } catch (const std::exception& e) {
        PrintExceptionContinue(&e, "Runaway exception");
        tfm::format(std::cerr, "%s: %s\n", CLIENT_NAME, e.what());
    } catch (...) {
        PrintExceptionContinue(nullptr, "Runaway exception");
    }
    return EXIT_FAILURE;
}
