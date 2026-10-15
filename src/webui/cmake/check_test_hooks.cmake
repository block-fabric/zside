# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Checks that chains-app (APP) has the hooks of the tests exactly when EXPECT is true (WEBUI_TEST_HOOKS):
# a release must not have the bridge methods that answer the dialogs of the desktop.
if(NOT EXISTS "${APP}")
  message(FATAL_ERROR "No program ${APP}")
endif()
file(STRINGS "${APP}" found REGEX "test\\.(confirmAnswer|fileAnswer|confirmed)|-webuitestscript=<file>")
if(EXPECT)
  if(NOT found)
    message(FATAL_ERROR "${APP} was built with WEBUI_TEST_HOOKS but has no test hooks")
  endif()
  message(STATUS "${APP}: test hooks built in, as asked (a build for the tests)")
else()
  if(found)
    message(FATAL_ERROR "${APP} has test hooks although WEBUI_TEST_HOOKS is OFF: ${found}")
  endif()
  message(STATUS "${APP}: no test hooks")
endif()
