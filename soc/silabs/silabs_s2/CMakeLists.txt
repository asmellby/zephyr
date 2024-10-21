# Copyright (c) 2024 Silicon Laboratories Inc.
# SPDX-License-Identifier: Apache-2.0

zephyr_include_directories(${ZEPHYR_BASE}/modules/hal_silabs/simplicity_sdk/config)

zephyr_sources(soc.c)
