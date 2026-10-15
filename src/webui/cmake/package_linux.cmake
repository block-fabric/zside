# Copyright (c) 2026 The Chains developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

# The Linux release of chains-app (target deploy-app): a tarball of what `cmake --install` puts in place
# for chains-app and the programs of the node (stripped), with the desktop entry and the icon, as a
# prefix to unpack anywhere (bin/, share/). Run by `cmake --build build --target deploy-app`.
#   cmake -DBUILD_DIR=<dir> -DOUT_DIR=<dir> -DSRC_DIR=<dir> -DSTEM=<name> -DCOMPONENTS=<a,b,...> -P package_linux.cmake

foreach(var BUILD_DIR OUT_DIR SRC_DIR STEM COMPONENTS)
  if(NOT ${var})
    message(FATAL_ERROR "package_linux.cmake needs ${var}")
  endif()
endforeach()
string(REPLACE "," ";" COMPONENTS "${COMPONENTS}")

set(stage ${OUT_DIR}/${STEM})
file(REMOVE_RECURSE ${stage})
foreach(component IN LISTS COMPONENTS)
  execute_process(
    COMMAND ${CMAKE_COMMAND} --install ${BUILD_DIR} --component ${component} --prefix ${stage} --strip
    OUTPUT_QUIET
    COMMAND_ERROR_IS_FATAL ANY
  )
endforeach()
file(COPY_FILE ${SRC_DIR}/COPYING ${stage}/COPYING)
file(COPY_FILE ${SRC_DIR}/README.md ${stage}/README.md)
file(COPY_FILE ${SRC_DIR}/doc/INSTALL_linux.md ${stage}/INSTALL.md)

file(REMOVE ${OUT_DIR}/${STEM}.tar.gz)
execute_process(
  WORKING_DIRECTORY ${OUT_DIR}
  COMMAND ${CMAKE_COMMAND} -E tar czf ${STEM}.tar.gz --mtime=2026-01-01 ${STEM}
  COMMAND_ERROR_IS_FATAL ANY
)
file(REMOVE_RECURSE ${stage})
file(SHA256 ${OUT_DIR}/${STEM}.tar.gz hash)
file(WRITE ${OUT_DIR}/SHA256SUMS "${hash}  ${STEM}.tar.gz\n")
message(STATUS "${OUT_DIR}/${STEM}.tar.gz")
