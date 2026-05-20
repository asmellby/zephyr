/*
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT     silabs_series3_extmem
#define SOC_NV_FLASH_NODE DT_INST(0, soc_nv_flash)

#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <soc.h>

#include "sl_se_manager.h"
#include "sl_se_manager_extmem.h"

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(flash_silabs, CONFIG_FLASH_LOG_LEVEL);

#define CODE_REGION_PAGE_SIZE 32768
#define WRITE_BLOCK_SIZE      DT_PROP(SOC_NV_FLASH_NODE, write_block_size)
#define ERASE_BLOCK_SIZE      DT_PROP(SOC_NV_FLASH_NODE, erase_block_size)

struct flash_silabs_data {
	struct k_sem lock;
	struct flash_pages_layout page_layout[2];
	uint32_t data_region_offset;
	size_t data_region_size;
};

struct flash_silabs_config {
	struct flash_parameters flash_parameters;
};

static bool flash_silabs_read_range_is_valid(const struct device *dev, off_t offset, uint32_t size)
{
	const struct flash_silabs_data *data = dev->data;
	uint32_t data_region_end = data->data_region_offset + data->data_region_size;

	return (offset >= 0) && (offset < data_region_end) && ((data_region_end - offset) >= size);
}

static bool flash_silabs_write_range_is_valid(const struct device *dev, off_t offset, uint32_t size)
{
	const struct flash_silabs_data *data = dev->data;
	/*
	 * - The address range must be within the bounds of the flash
	 * - The address range must be in the data region
	 * - The offset and size must be word aligned
	 */
	return flash_silabs_read_range_is_valid(dev, offset, size) &&
	       (offset >= data->data_region_offset) && IS_ALIGNED(offset, WRITE_BLOCK_SIZE) &&
	       IS_ALIGNED(size, WRITE_BLOCK_SIZE);
}

static int flash_silabs_read(const struct device *dev, off_t offset, void *data, size_t size)
{
	void *address = (uint8_t *)DT_REG_ADDR(SOC_NV_FLASH_NODE) + offset;

	if (!flash_silabs_read_range_is_valid(dev, offset, size)) {
		return -EINVAL;
	}

	if (!size) {
		return 0;
	}

	memcpy(data, address, size);

	return 0;
}

static int flash_silabs_write(const struct device *dev, off_t offset, const void *data, size_t size)
{
	void *address = (uint8_t *)DT_REG_ADDR(SOC_NV_FLASH_NODE) + offset;
	struct flash_silabs_data *const dev_data = dev->data;
	sl_se_command_context_t cmd_ctx;
	sl_status_t status;
	int ret = 0;

	if (!flash_silabs_write_range_is_valid(dev, offset, size)) {
		return -EINVAL;
	}

	if (!size) {
		return 0;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);

	status = sl_se_init_command_context(&cmd_ctx);
	if (status != SL_STATUS_OK) {
		ret = -EIO;
		goto cleanup;
	}
	status = sl_se_data_region_write(&cmd_ctx, address, data, size);
	if (status != SL_STATUS_OK) {
		ret = -EIO;
	}

cleanup:
	k_sem_give(&dev_data->lock);
	return ret;
}

static int flash_silabs_erase(const struct device *dev, off_t offset, size_t size)
{
	void *address = (uint8_t *)DT_REG_ADDR(SOC_NV_FLASH_NODE) + offset;
	struct flash_silabs_data *const dev_data = dev->data;
	sl_se_command_context_t cmd_ctx;
	sl_status_t status;
	int ret = 0;

	if (!flash_silabs_write_range_is_valid(dev, offset, size)) {
		return -EINVAL;
	}

	if (!IS_ALIGNED(offset, ERASE_BLOCK_SIZE)) {
		LOG_ERR("offset 0x%lx: not on a page boundary", (long)offset);
		return -EINVAL;
	}

	if (!IS_ALIGNED(size, ERASE_BLOCK_SIZE)) {
		LOG_ERR("size %zu: not multiple of a page size", size);
		return -EINVAL;
	}

	if (!size) {
		return 0;
	}

	k_sem_take(&dev_data->lock, K_FOREVER);

	status = sl_se_init_command_context(&cmd_ctx);
	if (status != SL_STATUS_OK) {
		ret = -EIO;
		goto cleanup;
	}
	status = sl_se_data_region_erase(&cmd_ctx, address, size / ERASE_BLOCK_SIZE);
	if (status != SL_STATUS_OK) {
		ret = -EIO;
	}

cleanup:
	k_sem_give(&dev_data->lock);

	return ret;
}

#if CONFIG_FLASH_PAGE_LAYOUT
void flash_silabs_page_layout(const struct device *dev, const struct flash_pages_layout **layout,
			      size_t *layout_size)
{
	const struct flash_silabs_data *const data = dev->data;

	*layout = &data->page_layout[0];
	*layout_size = 2;
}
#endif

static const struct flash_parameters *flash_silabs_get_parameters(const struct device *dev)
{
	const struct flash_silabs_config *const config = dev->config;

	return &config->flash_parameters;
}

static int flash_silabs_get_size(const struct device *dev, uint64_t *size)
{
	ARG_UNUSED(dev);

	*size = (uint64_t)DT_REG_SIZE(SOC_NV_FLASH_NODE);

	return 0;
}

static int flash_silabs_init(const struct device *dev)
{
	struct flash_silabs_data *const data = dev->data;
	sl_se_command_context_t cmd_ctx;
	sl_status_t status;
	void *address;

	status = sl_se_init();
	if (status != SL_STATUS_OK) {
		return -EIO;
	}

	status = sl_se_init_command_context(&cmd_ctx);
	if (status != SL_STATUS_OK) {
		return -EIO;
	}
	status = sl_se_data_region_get_location(&cmd_ctx, &address, &data->data_region_size);
	if (status != SL_STATUS_OK) {
		return -EIO;
	}

	/*
	 * Use FLASH_BASE from device header for the offset calculation instead of DT property
	 * due to a bug in the device header causing the address to point to NS memory despite
	 * the app actually using S aliases. FLASH_BASE is the same constant that is added inside
	 * sl_se_data_region_get_location() to turn the offset into an absolute address, so using
	 * it here to get the offset back guarantees consistency.
	 */
	data->data_region_offset = (uint32_t)(address)-FLASH_BASE;
	data->page_layout[0].pages_count = data->data_region_offset / CODE_REGION_PAGE_SIZE;
	data->page_layout[1].pages_count = data->data_region_size / ERASE_BLOCK_SIZE;

	LOG_INF("Device %s initialized", dev->name);

	return 0;
}

static DEVICE_API(flash, flash_silabs_driver_api) = {
	.read = flash_silabs_read,
	.write = flash_silabs_write,
	.erase = flash_silabs_erase,
	.get_parameters = flash_silabs_get_parameters,
	.get_size = flash_silabs_get_size,
#ifdef CONFIG_FLASH_PAGE_LAYOUT
	.page_layout = flash_silabs_page_layout,
#endif
};

/* clang-format off */
static struct flash_silabs_data flash_silabs_data_0 = {
	.lock = Z_SEM_INITIALIZER(flash_silabs_data_0.lock, 1, 1),
	.page_layout = {
		/* Code region */
		{
			.pages_count = 0,
			.pages_size = CODE_REGION_PAGE_SIZE,
		},
		/* Data region */
		{
			.pages_count = 0,
			.pages_size = ERASE_BLOCK_SIZE,
		}
	},
};

static const struct flash_silabs_config flash_silabs_config_0 = {
	.flash_parameters = {
		.write_block_size = WRITE_BLOCK_SIZE,
		.erase_value = 0xff,
	},
};
/* clang-format on */

DEVICE_DT_INST_DEFINE(0, flash_silabs_init, NULL, &flash_silabs_data_0, &flash_silabs_config_0,
		      POST_KERNEL, CONFIG_FLASH_INIT_PRIORITY, &flash_silabs_driver_api);
