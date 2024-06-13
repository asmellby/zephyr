/*
 * Copyright (c) 2016 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS   1000

/* The devicetree node identifier for the "led0" alias. */
#define LED0_NODE DT_ALIAS(led0)

/*
 * A build error on this line means your board is unsupported.
 * See the sample documentation for information on how to fix this.
 */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int ret;
	bool led_state = true;
	const struct device *const dev = DEVICE_DT_GET_ANY(silabs_si7006);
	struct sensor_value temp = {0};
	struct sensor_value hum = {0};

	if (!gpio_is_ready_dt(&led)) {
		return 0;
	}

	ret = gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return 0;
	}

	if (!device_is_ready(dev)) {
		printk("Device %s is not ready\n", dev->name);
		return 0;
	}

	printk("device is %p, name is %s\n", dev, dev->name);

	while (1) {
		ret = gpio_pin_toggle_dt(&led);
		if (ret < 0) {
			return 0;
		}

		led_state = !led_state;
		printf("LED state: %s\n", led_state ? "ON" : "OFF");
		k_msleep(SLEEP_TIME_MS);

		ret = sensor_sample_fetch(dev);
		if (ret < 0) {
			printk("Failed sample fetch\n");
		}

		ret = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp);
		if (ret < 0) {
			printk("Failed channel get\n");
		}
		ret = sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &hum);
		if (ret < 0) {
			printk("Failed channel get\n");
		}
		printf("temp %llu mdeg C, hum %llu m%% RH\n",
			sensor_value_to_milli(&temp),
			sensor_value_to_milli(&hum));

	}
	return 0;
}
