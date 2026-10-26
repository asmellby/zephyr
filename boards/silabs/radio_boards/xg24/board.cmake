# SPDX-License-Identifier: Apache-2.0

board_runner_args(jlink "--device=EFR32MG24BxxxF1536")
include(${ZEPHYR_BASE}/boards/common/jlink.board.cmake)

board_runner_args(silabs_commander "--device=${CONFIG_SOC}")
include(${ZEPHYR_BASE}/boards/common/silabs_commander.board.cmake)

board_runner_args(openocd)
include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)

set(vara_boards xg24_rb4186c xg24_rb4187c xgm240_rb4316a xgm240_rb4317a)
if(BOARD IN_LIST vara_boards)
  include(${ZEPHYR_BASE}/boards/common/vara.board.cmake)
endif()
