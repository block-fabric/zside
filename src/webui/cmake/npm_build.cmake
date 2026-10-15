# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# Builds the frontend (src/webui/app) into OUT_DIR:
#   cmake -DNPM=<npm> -DAPP_DIR=<dir> -DOUT_DIR=<dir> -P npm_build.cmake
# The packages are installed with `npm ci` (exact versions of package-lock.json)
# when node_modules is missing or older than the lock file; after that the
# build needs no network.

foreach(var NPM APP_DIR OUT_DIR)
  if(NOT ${var})
    message(FATAL_ERROR "npm_build.cmake needs ${var}")
  endif()
endforeach()

set(stamp "${APP_DIR}/node_modules/.package-lock.json")
if(NOT EXISTS "${stamp}" OR "${APP_DIR}/package-lock.json" IS_NEWER_THAN "${stamp}")
  message(STATUS "Installing the packages of the interface (npm ci)")
  execute_process(
    COMMAND "${NPM}" ci --no-audit --no-fund --prefer-offline
    WORKING_DIRECTORY "${APP_DIR}"
    RESULT_VARIABLE result
  )
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "npm ci failed (${result}). The first build needs the npm registry, or a filled npm cache.")
  endif()
endif()

execute_process(
  COMMAND "${NPM}" run --silent build -- --outDir "${OUT_DIR}" --emptyOutDir
  WORKING_DIRECTORY "${APP_DIR}"
  RESULT_VARIABLE result
)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "The build of the interface failed (${result})")
endif()
