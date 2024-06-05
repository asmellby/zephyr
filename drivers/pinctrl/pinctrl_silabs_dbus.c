/*
 * Copyright (c) 2024 Silicon Labs
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/drivers/pinctrl.h>
#include <zephyr/arch/cpu.h>

#include <soc_gpio.h>

int pinctrl_configure_pins(const pinctrl_soc_pin_t *pins, uint8_t pin_cnt, uintptr_t reg)
{
	ARG_UNUSED(reg);

	for (uint8_t i = 0U; i < pin_cnt; i++) {
		mem_addr_t enable_reg, route_reg;

		/* Configure GPIO */
		GPIO_PinModeSet(pins[i].port, pins[i].pin, pins[i].mode, pins[i].dout);

		/* Configure DBUS */
		enable_reg = DT_REG_ADDR(DT_NODELABEL(gpio))
			     + (pins[i].base_offset * sizeof(mem_addr_t));
		route_reg = enable_reg + (pins[i].route_offset * sizeof(mem_addr_t));

		sys_write32(pins[i].port | (pins[i].pin << 16), route_reg);

		if (pins[i].en_bit != 0xFFU) {
			sys_set_bit(enable_reg, pins[i].en_bit);
		}
	}

	return 0;
}
