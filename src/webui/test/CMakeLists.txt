# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Unit tests of the host side of chains-app: the bridge, the events, the app
# settings, the assets and the single-instance socket. No window is needed.
add_executable(test_chains-app
  main.cpp
  appconfig_tests.cpp
  assets_tests.cpp
  bridge_tests.cpp
  chainconfig_tests.cpp
  dialog_tests.cpp
  sidechainapi_tests.cpp
  events_tests.cpp
  hardening_tests.cpp
  i18n_tests.cpp
  nodeapi_tests.cpp
  nodeevents_tests.cpp
  singleinstance_tests.cpp
  windowframe_tests.cpp
  $<$<BOOL:${ENABLE_WALLET}>:walletapi_tests.cpp>
)
# The tests of the host side of the chain's own pages (local.cpp), when it has some: test/local_tests.cpp.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/local_tests.cpp)
  target_sources(test_chains-app PRIVATE local_tests.cpp)
endif()
set_target_properties(test_chains-app PROPERTIES OUTPUT_NAME test_${CLIENT_BIN_NAME}-app)
target_link_libraries(test_chains-app
  core_interface
  chains_webui_core
  chains_webui_assets
  $<TARGET_NAME_IF_EXISTS:chains_webui_wallet>
  test_util
  bitcoin_node
  Boost::headers
)
if(WIN32)
  # The UTF-8 code page, as test_bitcoin.
  add_windows_application_manifest(test_chains-app)
endif()
add_test(NAME webui_tests COMMAND test_chains-app)
install_binary_component(test_chains-app INTERNAL)

# The tests of the interface (vitest), and the end-to-end smoke test of chains-app in a virtual
# display (it also saves pictures of every page in e2e-shots).
if(NPM_EXECUTABLE)
  add_test(NAME webui_frontend_tests COMMAND ${NPM_EXECUTABLE} test WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../app)
  add_test(NAME webui_frontend_types COMMAND ${NPM_EXECUTABLE} run check WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../app)
endif()
find_program(XVFB_RUN_EXECUTABLE xvfb-run)
find_package(Python3 COMPONENTS Interpreter)
# The hooks of the tests are in chains-app only with WEBUI_TEST_HOOKS (never in a release): this checks
# the program has them exactly when it should.
add_test(NAME webui_test_hooks_built
  COMMAND ${CMAKE_COMMAND} -DAPP=$<TARGET_FILE:chains-app> -DEXPECT=${WEBUI_TEST_HOOKS} -P ${CMAKE_CURRENT_SOURCE_DIR}/../cmake/check_test_hooks.cmake
)
# The end-to-end tests drive the app with its test hooks.
if(WEBUI_SIDECHAIN)
  # A sidechain: its blocks need a mainchain. The app against a live regtest mainchain (chainsd, in
  # WEBUI_MAINCHAIN_BIN_DIR): a deposit, merged mining, a withdrawal and its bundle paid. The other
  # end-to-end tests are the mainchain's (they mine their own blocks): not run here.
  set(WEBUI_MAINCHAIN_BIN_DIR "$ENV{MAINCHAIN_BIN_DIR}" CACHE PATH "Folder of chainsd, the mainchain node of the end-to-end test of a sidechain (default: $MAINCHAIN_BIN_DIR)")
  if(WEBUI_TEST_HOOKS AND XVFB_RUN_EXECUTABLE AND Python3_Interpreter_FOUND AND EXISTS "${WEBUI_MAINCHAIN_BIN_DIR}/chainsd")
    add_test(NAME webui_e2e_sidechain
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_sidechain.py
        --app $<TARGET_FILE:chains-app>
        --mainchain-bin ${WEBUI_MAINCHAIN_BIN_DIR}
        --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-sidechain
        --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
    )
    set_tests_properties(webui_e2e_sidechain PROPERTIES TIMEOUT 2400 RUN_SERIAL TRUE)
    # Simple mode, the default of a first start (sidechain_simple.js): Home, Drivechains in two tabs, every page
    # of the sidebar in Midnight and Light, the switch to Advanced and back (the other scripts run in Advanced).
    add_test(NAME webui_e2e_sidechain_simple
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_sidechain.py
        --app $<TARGET_FILE:chains-app>
        --mainchain-bin ${WEBUI_MAINCHAIN_BIN_DIR}
        --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-sidechain-simple
        --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
        --port 41640
        --script sidechain_simple.js
        --js-var SIMPLE=true
    )
    set_tests_properties(webui_e2e_sidechain_simple PROPERTIES TIMEOUT 2400 RUN_SERIAL TRUE)
    # The page scripts of the chain's own pages, the same way: e2e/local.cmake calls
    # webui_local_e2e(<name> <port> [options of run_sidechain.py]) for each e2e/<name>.js, so that a merge of
    # the template never conflicts with them. The template has none.
    function(webui_local_e2e name port)
      add_test(NAME webui_e2e_${name}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_sidechain.py
          --app $<TARGET_FILE:chains-app>
          --mainchain-bin ${WEBUI_MAINCHAIN_BIN_DIR}
          --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-${name}
          --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
          --port ${port}
          --script ${name}.js
          ${ARGN}
      )
      set_tests_properties(webui_e2e_${name} PROPERTIES TIMEOUT 2400 RUN_SERIAL TRUE)
    endfunction()
    include(${CMAKE_CURRENT_SOURCE_DIR}/e2e/local.cmake OPTIONAL)
  endif()
elseif(WEBUI_TEST_HOOKS AND XVFB_RUN_EXECUTABLE AND Python3_Interpreter_FOUND)
  add_test(NAME webui_e2e_smoke
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_smoke.py
      --app $<TARGET_FILE:chains-app>
      --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e
      --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
  )
  set_tests_properties(webui_e2e_smoke PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)
  if(ENABLE_WALLET)
    # The fixes of the audit (doc/webui-audit.md): chain text that is not UTF-8, long calls, a long history,
    # keys for one call, contrast, narrow windows, settings that wait for Apply, -disablewallet, a language.
    add_test(NAME webui_e2e_audit
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_audit.py
        --app $<TARGET_FILE:chains-app>
        --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-audit
        --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
    )
    set_tests_properties(webui_e2e_audit PROPERTIES TIMEOUT 3000 RUN_SERIAL TRUE)
    # The wallet pages, driven as a user would (also saves pictures of them in e2e-shots).
    add_test(NAME webui_e2e_wallet
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_wallet.py
        --app $<TARGET_FILE:chains-app>
        --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-wallet
        --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
    )
    set_tests_properties(webui_e2e_wallet PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)
    # Simple mode, the default of a first start: Home, the sidebar, Move coins to a real deposit, the
    # glossary, then the switch (the other e2e scripts run in Advanced; also saves pictures in e2e-shots).
    add_test(NAME webui_e2e_simple
      COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_simple.py
        --app $<TARGET_FILE:chains-app>
        --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-simple
        --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
    )
    set_tests_properties(webui_e2e_simple PROPERTIES TIMEOUT 1500 RUN_SERIAL TRUE)
    # The node and Chains pages, with a pretend desktop (tray, notifications) on a private session
    # bus and a second node as a peer (also saves pictures of these pages in e2e-shots).
    find_program(DBUS_DAEMON_EXECUTABLE dbus-daemon)
    if(DBUS_DAEMON_EXECUTABLE)
      add_test(NAME webui_e2e_node
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_node.py
          --app $<TARGET_FILE:chains-app>
          --tmp ${CMAKE_CURRENT_BINARY_DIR}/e2e-node
          --shots ${CMAKE_CURRENT_BINARY_DIR}/e2e-shots
      )
      set_tests_properties(webui_e2e_node PROPERTIES TIMEOUT 2000 RUN_SERIAL TRUE)
    endif()
  endif()
endif()
