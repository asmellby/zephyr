/*
 * Copyright (c) 2024 Silicon Labs
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_SILABS_PINCTRL_DBUS_H_
#define ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_SILABS_PINCTRL_DBUS_H_

#include <zephyr/dt-bindings/dt-util.h>

/*
 * Silabs Series 2 DBUS configuration is encoded in a 32-bit bitfield organized
 * as follows:
 *
 * 31..29: Reserved
 * 28..24: Route register offset from peripheral config (offset of <fun>ROUTE
 *         register in GPIO_<periph>ROUTE_TypeDef)
 * 23..19: Enable bit (offset into ROUTEEN register for given function)
 * 18    : Enable bit presence (some inputs are auto-enabled)
 * 17..8 : Peripheral config offset from GPIO base (offset of <periph>ROUTE[n]
 *         register in GPIO_TypeDef)
 *  7..4 : GPIO pin
 *  3..0 : GPIO port
 */

#define SILABS_PINCTRL_GPIO_PORT_MASK        0xFUL
#define SILABS_PINCTRL_GPIO_PIN_MASK         0xF0UL
#define SILABS_PINCTRL_PERIPH_BASE_MASK      0x3FF00UL
#define SILABS_PINCTRL_HAVE_EN_MASK          0x40000UL
#define SILABS_PINCTRL_EN_BIT_MASK           0xF80000UL
#define SILABS_PINCTRL_ROUTE_MASK            0x1F000000UL

#define SILABS_DBUS(port, pin, periph_base, en_present, en_bit, route) \
	(FIELD_PREP(SILABS_PINCTRL_GPIO_PORT_MASK, port) |             \
	 FIELD_PREP(SILABS_PINCTRL_GPIO_PIN_MASK, pin) |               \
	 FIELD_PREP(SILABS_PINCTRL_PERIPH_BASE_MASK, periph_base) |    \
	 FIELD_PREP(SILABS_PINCTRL_HAVE_EN_MASK, en_present) |         \
	 FIELD_PREP(SILABS_PINCTRL_EN_BIT_MASK, en_bit) |              \
	 FIELD_PREP(SILABS_PINCTRL_ROUTE_MASK, route))

#endif /* ZEPHYR_INCLUDE_DT_BINDINGS_PINCTRL_SILABS_PINCTRL_DBUS_H_ */
