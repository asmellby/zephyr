# SPDX-License-Identifier: Apache-2.0

find_program(
  RENODE
  renode
  )

set(RENODE_FLAGS
  --disable-xwt
  --port -2
  --pid-file renode.pid
  )

# Check if there is any Renode script overlay defined for the target board
set(resc_overlay_file ${APPLICATION_SOURCE_DIR}/boards/${BOARD}.resc)
if(EXISTS ${resc_overlay_file})
  set(RENODE_OVERLAY include "@${resc_overlay_file}\;")
  message(STATUS "Found Renode script overlay: ${resc_overlay_file}")
endif()

add_custom_target(run_renode
  COMMAND
  ${RENODE}
  ${RENODE_FLAGS}
  -e '$$elf=@${PROJECT_BINARY_DIR}/${KERNEL_ELF_NAME}\; include @${RENODE_SCRIPT}\; ${RENODE_OVERLAY} s'
  WORKING_DIRECTORY ${APPLICATION_BINARY_DIR}
  DEPENDS ${logical_target_for_zephyr_elf}
  USES_TERMINAL
  )

# Runs the application in a Renode shared with other applications, built separately and possibly
# for other boards: see scripts/support/renode_multi.py
if(DEFINED RENODE_UART)
  get_filename_component(app_name ${APPLICATION_SOURCE_DIR} NAME)
  set(RENODE_MULTI_ARGS
    --renode ${RENODE}
    --elf ${PROJECT_BINARY_DIR}/${KERNEL_ELF_NAME}
    --resc ${RENODE_SCRIPT}
    --uart ${RENODE_UART}
    --config ${PROJECT_BINARY_DIR}/.config
    --default-name ${app_name}
    )
  if(EXISTS ${resc_overlay_file})
    list(APPEND RENODE_MULTI_ARGS --overlay ${resc_overlay_file})
  endif()
  if(DEFINED RENODE_MULTI_NAME)
    list(APPEND RENODE_MULTI_ARGS --name ${RENODE_MULTI_NAME})
  endif()
  if(DEFINED RENODE_MULTI_PORT)
    list(APPEND RENODE_MULTI_ARGS --port ${RENODE_MULTI_PORT})
  endif()
  if(DEFINED RENODE_MULTI_WAIT_FOR)
    list(APPEND RENODE_MULTI_ARGS --wait-for ${RENODE_MULTI_WAIT_FOR})
  endif()

  add_custom_target(run_renode_multi
    COMMAND
    ${PYTHON_EXECUTABLE}
    ${ZEPHYR_BASE}/scripts/support/renode_multi.py
    ${RENODE_MULTI_ARGS}
    WORKING_DIRECTORY ${APPLICATION_BINARY_DIR}
    DEPENDS ${logical_target_for_zephyr_elf}
    USES_TERMINAL
    )
endif()
