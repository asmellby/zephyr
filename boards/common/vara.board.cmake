# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Board support for the vara SoC emulator.
#
# The vara board is resolved in this order, first match wins:
# - VARA_BOARD, given as a vara board name or a path to a board .toml file
# - <app>/boards/<board target>.vara.toml or <app>/boards/<board>.vara.toml
# - <board dir>/support/<board target>.vara.toml or <board dir>/support/<board>.vara.toml
# - the Zephyr board name, which selects a board built into vara

list(APPEND SUPPORTED_EMU_PLATFORMS vara)

zephyr_get(VARA_BOARD)
if(NOT VARA_BOARD)
  foreach(dir ${APPLICATION_SOURCE_DIR}/boards ${BOARD_DIR}/support)
    foreach(name ${NORMALIZED_BOARD_TARGET} ${BOARD})
      if(EXISTS ${dir}/${name}.vara.toml)
        set(VARA_BOARD ${dir}/${name}.vara.toml)
        break()
      endif()
    endforeach()
    if(VARA_BOARD)
      break()
    endif()
  endforeach()
endif()
if(NOT VARA_BOARD)
  set(VARA_BOARD ${BOARD})
endif()
message(STATUS "vara board: ${VARA_BOARD}")

board_set_sim_runner_ifnset(vara)
board_runner_args(vara "--vara-board=${VARA_BOARD}")
board_finalize_runner_args(vara)
