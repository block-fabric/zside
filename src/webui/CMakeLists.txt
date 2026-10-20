# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# chains-app: the node with its HTML interface in a native web view (webview/webview), no Qt.
# The program is <stem>-app (CLIENT_BIN_NAME); the targets keep the names of chains-app.

# The chain of the app, in one place: app/src/chain/chain.json, read here for the host
# (webui/chainconfig.h) and by the interface (src/chain/config.ts).
set(webui_chain_json ${CMAKE_CURRENT_SOURCE_DIR}/app/src/chain/chain.json)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${webui_chain_json})
file(READ ${webui_chain_json} webui_chain)
string(JSON WEBUI_CHAIN_NAME GET "${webui_chain}" name)
string(JSON WEBUI_CHAIN_BIN GET "${webui_chain}" bin)
string(JSON webui_chain_role GET "${webui_chain}" role)
string(JSON WEBUI_CHAIN_URI_SCHEME GET "${webui_chain}" uriScheme)
string(JSON WEBUI_CHAIN_TICKER GET "${webui_chain}" coin ticker)
string(JSON WEBUI_CHAIN_COIN_NAME GET "${webui_chain}" coin name)
string(JSON WEBUI_CHAIN_ICON_HUE GET "${webui_chain}" brand iconHue)
if(NOT WEBUI_CHAIN_NAME STREQUAL CLIENT_NAME OR NOT WEBUI_CHAIN_BIN STREQUAL CLIENT_BIN_NAME)
  message(FATAL_ERROR "${webui_chain_json} names the chain ${WEBUI_CHAIN_NAME} (${WEBUI_CHAIN_BIN}), CMakeLists.txt ${CLIENT_NAME} (${CLIENT_BIN_NAME}): "
                      "set the same in both (contrib/sidechain/rename.py does).")
endif()
if(webui_chain_role STREQUAL "sidechain")
  set(WEBUI_SIDECHAIN 1)
  string(JSON WEBUI_MAINCHAIN_NAME GET "${webui_chain}" mainchain name)
elseif(webui_chain_role STREQUAL "mainchain")
  set(WEBUI_SIDECHAIN 0)
  set(WEBUI_MAINCHAIN_NAME "")
else()
  message(FATAL_ERROR "${webui_chain_json}: role is sidechain or mainchain, not ${webui_chain_role}")
endif()
set(WEBUI_CHAIN_NETWORKS "")
foreach(network main test signet regtest)
  string(JSON p2p GET "${webui_chain}" networks ${network} p2p)
  string(JSON rpc GET "${webui_chain}" networks ${network} rpc)
  string(JSON hrp GET "${webui_chain}" networks ${network} hrp)
  set(main_rpc 0)
  if(WEBUI_SIDECHAIN)
    string(JSON main_rpc GET "${webui_chain}" mainchain networks ${network} rpc)
  endif()
  string(APPEND WEBUI_CHAIN_NETWORKS "Network{\"${network}\", ${p2p}, ${rpc}, \"${hrp}\", ${main_rpc}},\n    ")
endforeach()
string(STRIP "${WEBUI_CHAIN_NETWORKS}" WEBUI_CHAIN_NETWORKS)
# Its help links (links: website, source, issues, guide), which the app opens without asking (webui/guard.cpp).
set(WEBUI_CHAIN_LINKS "")
foreach(link website source issues guide)
  string(JSON url ERROR_VARIABLE no_link GET "${webui_chain}" links ${link})
  if(NOT no_link AND url MATCHES "^https://[A-Za-z0-9./_#?=%-]+$")
    string(APPEND WEBUI_CHAIN_LINKS "\"${url}\", ")
  elseif(NOT no_link)
    message(FATAL_ERROR "${webui_chain_json}: links.${link} is an https address of plain characters, not ${url}")
  endif()
endforeach()
string(REGEX MATCHALL "\"," webui_link_commas "${WEBUI_CHAIN_LINKS}")
list(LENGTH webui_link_commas WEBUI_CHAIN_LINKS_COUNT)
configure_file(chainconfig.h.in ${CMAKE_CURRENT_BINARY_DIR}/gen/webui/chainconfig.h @ONLY)

# The hooks of the end-to-end tests (-webuitestscript, -webuitestshots and the test.* methods of the bridge,
# which answer the dialogs of the desktop): only in a build for the tests, never in a release.
option(WEBUI_TEST_HOOKS "Build chains-app with the hooks of its end-to-end tests (never for a release)." OFF)

set(WEBUI_PREBUILT_DIST "" CACHE PATH "Use this already built interface (the output of `npm run build` in src/webui/app) instead of building it with npm.")

# The interface: built with npm (Svelte, TypeScript, Vite), then embedded into the program.
set(webui_app_dir ${CMAKE_CURRENT_SOURCE_DIR}/app)
if(WEBUI_PREBUILT_DIST)
  set(webui_dist ${WEBUI_PREBUILT_DIST})
  file(GLOB_RECURSE webui_dist_files CONFIGURE_DEPENDS ${webui_dist}/*)
  set(webui_dist_stamp ${webui_dist_files})
else()
  find_program(NPM_EXECUTABLE npm REQUIRED)
  set(webui_dist ${CMAKE_CURRENT_BINARY_DIR}/ui-dist)
  file(GLOB_RECURSE webui_app_sources CONFIGURE_DEPENDS
    ${webui_app_dir}/src/*
    ${webui_app_dir}/public/*
    ${CMAKE_CURRENT_SOURCE_DIR}/locale/*.ts
  )
  list(FILTER webui_app_sources EXCLUDE REGEX "\\.test\\.ts$")
  add_custom_command(
    OUTPUT ${webui_dist}/index.html
    COMMAND ${CMAKE_COMMAND} -DNPM=${NPM_EXECUTABLE} -DAPP_DIR=${webui_app_dir} -DOUT_DIR=${webui_dist} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/npm_build.cmake
    DEPENDS
      ${webui_app_sources}
      ${webui_app_dir}/index.html
      ${webui_app_dir}/dialog.html
      ${webui_app_dir}/vite.dialog.config.ts
      ${webui_app_dir}/scripts/build.mjs
      ${webui_app_dir}/package.json
      ${webui_app_dir}/package-lock.json
      ${webui_app_dir}/vite.config.ts
      ${webui_app_dir}/tsconfig.json
      ${webui_chain_json}
      ${CMAKE_CURRENT_SOURCE_DIR}/cmake/npm_build.cmake
    COMMENT "Building the interface of ${CLIENT_BIN_NAME}-app (npm)"
    VERBATIM
  )
  set(webui_dist_stamp ${webui_dist}/index.html)
endif()

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp
  COMMAND ${CMAKE_COMMAND} -DDIST_DIR=${webui_dist} -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  DEPENDS ${webui_dist_stamp} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed.cmake
  COMMENT "Embedding the interface of ${CLIENT_BIN_NAME}-app"
  VERBATIM
)

add_library(chains_webui_assets STATIC EXCLUDE_FROM_ALL
  ${CMAKE_CURRENT_BINARY_DIR}/assets_data.cpp
)
target_link_libraries(chains_webui_assets PRIVATE core_interface)

# What does not need a window: the bridge, the events, the settings of the app, the assets.
add_library(chains_webui_core STATIC EXCLUDE_FROM_ALL
  appconfig.cpp
  assets.cpp
  bridge.cpp
  rpcpolicy.cpp
  confirm.cpp
  console.cpp
  dialog.cpp
  guard.cpp
  quitguard.cpp
  events.cpp
  i18n.cpp
  icontint.cpp
  local.cpp
  nodeapi.cpp
  nodeevents.cpp
  options.cpp
  sidechainapi.cpp
  sidechainnodes.cpp
  singleinstance.cpp
  windowframe.cpp
)
target_include_directories(chains_webui_core PUBLIC ${CMAKE_CURRENT_BINARY_DIR}/gen)
target_link_libraries(chains_webui_core
  PUBLIC
    core_interface
    bitcoin_node
    bitcoin_cli
    bitcoin_common
    bitcoin_util
    univalue
    Boost::headers
)
if(WIN32)
  # The named pipe of the single instance, the link of the startup folder.
  target_link_libraries(chains_webui_core PUBLIC advapi32 ole32 shell32 uuid)
endif()

# The wallet functions of the page (the wallet pages of chains-qt), in a build with wallets.
if(ENABLE_WALLET)
  add_library(chains_webui_wallet STATIC EXCLUDE_FROM_ALL
    walletapi.cpp
  )
  target_link_libraries(chains_webui_wallet
    PUBLIC
      core_interface
      chains_webui_core
      bitcoin_wallet
      bitcoin_node
  )
endif()

# The host of the pages over RPC, in the daemon (webui/remotehost.h): what the Chains app needs to show this chain's
# pages in its own window (webuihost, describeconfirmation, webuievents). Only the daemon has it.
add_library(chains_webui_remote STATIC EXCLUDE_FROM_ALL
  remotehost.cpp
)
target_link_libraries(chains_webui_remote
  PUBLIC
    core_interface
    chains_webui_core
    $<TARGET_NAME_IF_EXISTS:chains_webui_wallet>
    bitcoin_node
)
if(TARGET bitcoind)
  target_link_libraries(bitcoind chains_webui_remote)
  target_compile_definitions(bitcoind PRIVATE ENABLE_WEBUI_REMOTE)
endif()

if(WIN32)
  # WebView2: its headers (the depends package webview2, or WEBVIEW2_INCLUDE_DIR); the loader is the
  # one built into webview.h, so no WebView2Loader.dll.
  find_path(WEBVIEW2_INCLUDE_DIR WebView2.h PATH_SUFFIXES webview2 REQUIRED)
  set(webui_window window_win32.cpp urischeme_win.cpp res/chains-app-res.rc)
  # The manifest: per monitor DPI awareness (v2), UTF-8, Windows 8 to 11, common controls 6.
  configure_file(res/chains-app.manifest.in ${CMAKE_CURRENT_BINARY_DIR}/chains-app.manifest USE_SOURCE_PERMISSIONS @ONLY)
  file(CONFIGURE
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/chains-app-manifest.rc
    CONTENT "1 /* CREATEPROCESS_MANIFEST_RESOURCE_ID */ 24 /* RT_MANIFEST */ \"${CMAKE_CURRENT_BINARY_DIR}/chains-app.manifest\""
  )
  list(APPEND webui_window ${CMAKE_CURRENT_BINARY_DIR}/chains-app-manifest.rc)
elseif(APPLE)
  set(webui_window window_generic.cpp)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(WEBKITGTK REQUIRED IMPORTED_TARGET gtk+-3.0 webkit2gtk-4.1)
  set(webui_window window_gtk.cpp tray_gtk.cpp)
endif()

add_executable(chains-app WIN32
  main.cpp
  app.cpp
  ${webui_window}
  ../init/chains-app.cpp
)
set_target_properties(chains-app PROPERTIES OUTPUT_NAME ${CLIENT_BIN_NAME}-app)
if(WEBUI_TEST_HOOKS)
  target_compile_definitions(chains-app PRIVATE WEBUI_TEST_HOOKS)
endif()
# The vendored webview header is not ours: no warnings from it.
target_include_directories(chains-app SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/webview)
target_link_libraries(chains-app
  core_interface
  chains_webui_core
  chains_webui_assets
  $<TARGET_NAME_IF_EXISTS:chains_webui_wallet>
  bitcoin_node
  $<TARGET_NAME_IF_EXISTS:bitcoin_wallet>
  $<TARGET_NAME_IF_EXISTS:PkgConfig::WEBKITGTK>
)
if(WIN32)
  target_include_directories(chains-app SYSTEM PRIVATE ${WEBVIEW2_INCLUDE_DIR})
  target_compile_definitions(chains-app PRIVATE WEBVIEW_MSWEBVIEW2_BUILTIN_IMPL=1 WEBVIEW_MSWEBVIEW2_EXPLICIT_LINK=1)
  target_link_libraries(chains-app advapi32 dwmapi gdi32 ole32 shell32 shlwapi user32 uuid version)
elseif(APPLE)
  target_link_libraries(chains-app "-framework WebKit" "-framework Cocoa")
endif()
install_binary_component(chains-app)
if(NOT WIN32 AND NOT APPLE)
  # The desktop entry: the app in the menus of the desktop, and the handler of its payment links (BIP 21).
  configure_file(${PROJECT_SOURCE_DIR}/share/app.desktop.in ${CMAKE_CURRENT_BINARY_DIR}/${CLIENT_BIN_NAME}-app.desktop @ONLY)
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/${CLIENT_BIN_NAME}-app.desktop
    DESTINATION ${CMAKE_INSTALL_DATADIR}/applications
    COMPONENT chains-app
  )
  # The chain's own mark (app/scripts/appicon.py), as the window and the tray show it.
  install(FILES ${CMAKE_CURRENT_SOURCE_DIR}/app/public/icon-256.png
    DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/256x256/apps
    RENAME ${CLIENT_BIN_NAME}.png
    COMPONENT chains-app
  )
endif()

# The release (target deploy-app), with chains-app and the programs of the node, in release-app of the
# build: on Windows a zip and an installer (NSIS), on Linux a tarball. Never with the test hooks.
set(webui_release ${PROJECT_BINARY_DIR}/release-app)
if(WEBUI_TEST_HOOKS)
  set(webui_no_test_hooks COMMAND ${CMAKE_COMMAND} -E echo "deploy-app: this build has the test hooks (WEBUI_TEST_HOOKS=ON); a release is made with them OFF"
                          COMMAND ${CMAKE_COMMAND} -E false)
endif()
if(MINGW AND TARGET bitcoind AND TARGET bitcoin-cli AND TARGET bitcoin-tx AND TARGET bitcoin-wallet AND TARGET bitcoin-util)
  set(webui_stem ${CLIENT_BIN_NAME}-app-${CLIENT_VERSION_STRING}-win64)
  set(CLIENT_URL ${PROJECT_HOMEPAGE_URL})
  set(BIN_DIR ${webui_release}/bin)
  set(abs_top_srcdir ${PROJECT_SOURCE_DIR})
  configure_file(res/setup-app.nsi.in ${webui_release}/${CLIENT_BIN_NAME}-app-setup.nsi @ONLY)
  string(LENGTH "${CLIENT_NAME}" webui_name_length)
  string(REPEAT "=" ${webui_name_length} WEBUI_README_RULE)
  configure_file(res/README-windows.txt.in ${webui_release}/README-windows.txt @ONLY)
  add_custom_target(deploy-app
    ${webui_no_test_hooks}
    COMMAND ${CMAKE_COMMAND} -E make_directory ${webui_release}/bin
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:chains-app> -o ${webui_release}/bin/${CLIENT_BIN_NAME}-app.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoind> -o ${webui_release}/bin/${CLIENT_BIN_NAME}d.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-cli> -o ${webui_release}/bin/${CLIENT_BIN_NAME}-cli.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-tx> -o ${webui_release}/bin/${CLIENT_BIN_NAME}-tx.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-wallet> -o ${webui_release}/bin/${CLIENT_BIN_NAME}-wallet.exe
    COMMAND ${CMAKE_STRIP} $<TARGET_FILE:bitcoin-util> -o ${webui_release}/bin/${CLIENT_BIN_NAME}-util.exe
    COMMAND ${CMAKE_COMMAND} -DBIN_DIR=${webui_release}/bin -DOUT_DIR=${webui_release} -DSRC_DIR=${PROJECT_SOURCE_DIR}
      -DNSI=${webui_release}/${CLIENT_BIN_NAME}-app-setup.nsi -DSTEMS=${CLIENT_BIN_NAME} -DSTEM=${webui_stem} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_windows.cmake
    DEPENDS chains-app bitcoind bitcoin-cli bitcoin-tx bitcoin-wallet bitcoin-util
    COMMENT "The Windows release of ${CLIENT_BIN_NAME}-app: ${webui_release}/${webui_stem}.zip and -setup.exe"
    VERBATIM
  )
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND TARGET bitcoind AND TARGET bitcoin-cli)
  set(webui_stem ${CLIENT_BIN_NAME}-app-${CLIENT_VERSION_STRING}-${CMAKE_SYSTEM_PROCESSOR}-linux-gnu)
  set(webui_components chains-app)
  foreach(program bitcoind bitcoin-cli bitcoin-tx bitcoin-wallet bitcoin-util)
    if(TARGET ${program})
      list(APPEND webui_components ${program})
    endif()
  endforeach()
  string(REPLACE ";" "," webui_components_arg "${webui_components}")
  add_custom_target(deploy-app
    ${webui_no_test_hooks}
    COMMAND ${CMAKE_COMMAND} -DBUILD_DIR=${PROJECT_BINARY_DIR} -DOUT_DIR=${webui_release} -DSRC_DIR=${PROJECT_SOURCE_DIR}
      -DSTEM=${webui_stem} -DCOMPONENTS=${webui_components_arg} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/package_linux.cmake
    DEPENDS ${webui_components}
    COMMENT "The Linux release of ${CLIENT_BIN_NAME}-app: ${webui_release}/${webui_stem}.tar.gz"
    VERBATIM
  )
endif()

if(BUILD_TESTS)
  add_subdirectory(test)
endif()
