# Copyright (c) 2021, Sateesh Kotapati
# SPDX-License-Identifier: Apache-2.0

board_runner_args(jlink "--device=EFR32MG24BxxxF1536" "--reset-after-load")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

set(SUPPORTED_EMU_PLATFORMS renode)
set(RENODE_SCRIPT ${CMAKE_CURRENT_LIST_DIR}/support/xg24_dk2601b.resc)
set(RENODE_UART "sysbus.usart0")
include(${ZEPHYR_BASE}/boards/common/renode.board.cmake)
include(${ZEPHYR_BASE}/boards/common/renode_robot.board.cmake)
