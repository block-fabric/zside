# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# The Windows release of chains-app (target deploy-app): a zip and an NSIS installer, from the
# stripped programs in BIN_DIR. Run by `cmake --build build_win --target deploy-app`.
#   cmake -DBIN_DIR=<dir> -DOUT_DIR=<dir> -DSRC_DIR=<dir> -DNSI=<configured .nsi> -DSTEM=<name> -P package_windows.cmake

foreach(var BIN_DIR OUT_DIR SRC_DIR NSI STEM STEMS)
  if(NOT ${var})
    message(FATAL_ERROR "package_windows.cmake needs ${var}")
  endif()
endforeach()
find_program(MAKENSIS_EXECUTABLE makensis REQUIRED)

# The zip: the folder of the installation (<stem>-app.exe at the top, the other programs in daemon).
set(stage ${OUT_DIR}/${STEM})
file(REMOVE_RECURSE ${stage})
file(MAKE_DIRECTORY ${stage}/daemon)
file(COPY ${BIN_DIR}/${STEMS}-app.exe DESTINATION ${stage})
foreach(program ${STEMS}d ${STEMS}-cli ${STEMS}-tx ${STEMS}-wallet ${STEMS}-util)
  file(COPY ${BIN_DIR}/${program}.exe DESTINATION ${stage}/daemon)
endforeach()
file(COPY_FILE ${SRC_DIR}/COPYING ${stage}/COPYING.txt)
file(COPY_FILE ${OUT_DIR}/README-windows.txt ${stage}/readme.txt)
file(REMOVE ${OUT_DIR}/${STEM}.zip)
execute_process(
  WORKING_DIRECTORY ${OUT_DIR}
  COMMAND ${CMAKE_COMMAND} -E tar cf ${STEM}.zip --format=zip --mtime=2026-01-01 ${STEM}
  COMMAND_ERROR_IS_FATAL ANY
)
file(REMOVE_RECURSE ${stage})

# The installer.
execute_process(
  WORKING_DIRECTORY ${OUT_DIR}
  COMMAND ${MAKENSIS_EXECUTABLE} -V2 -DOUTFILE=${STEM}-setup.exe ${NSI}
  COMMAND_ERROR_IS_FATAL ANY
)
file(SHA256 ${OUT_DIR}/${STEM}.zip zip_hash)
file(SHA256 ${OUT_DIR}/${STEM}-setup.exe setup_hash)
file(WRITE ${OUT_DIR}/SHA256SUMS "${zip_hash}  ${STEM}.zip\n${setup_hash}  ${STEM}-setup.exe\n")
message(STATUS "${OUT_DIR}/${STEM}.zip\n   ${OUT_DIR}/${STEM}-setup.exe")
