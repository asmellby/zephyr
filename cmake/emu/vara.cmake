# Copyright The Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0

# Emulation targets for the vara SoC emulator.
#
# VARA_BOARD is resolved by boards/common/vara.board.cmake. Extra arguments
# for 'vara run' are taken from VARA_ARGS, either a CMake variable or an
# environment variable, for example VARA_ARGS="--halt-time realtime".

find_program(VARA vara)

if(VARA STREQUAL VARA-NOTFOUND)
  message(STATUS "vara not found, run_vara and debugserver_vara are unavailable")
  foreach(target run_vara debugserver_vara)
    add_custom_target(${target}
      COMMAND ${CMAKE_COMMAND} -E echo "vara not found: install it or add it to PATH"
      COMMAND ${CMAKE_COMMAND} -E false
      )
  endforeach()
  return()
endif()

zephyr_get(VARA_ARGS)
separate_arguments(vara_args UNIX_COMMAND "${VARA_ARGS}")

set(vara_elf ${PROJECT_BINARY_DIR}/${KERNEL_ELF_NAME})

add_custom_target(run_vara
  COMMAND ${VARA} run --board ${VARA_BOARD} ${vara_args} ${vara_elf}
  WORKING_DIRECTORY ${APPLICATION_BINARY_DIR}
  DEPENDS ${logical_target_for_zephyr_elf}
  USES_TERMINAL
  )

add_custom_target(debugserver_vara
  COMMAND ${VARA} run --board ${VARA_BOARD} --gdb 3333 ${vara_args} ${vara_elf}
  WORKING_DIRECTORY ${APPLICATION_BINARY_DIR}
  DEPENDS ${logical_target_for_zephyr_elf}
  USES_TERMINAL
  )
