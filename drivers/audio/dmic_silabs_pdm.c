/*
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT silabs_pdm

#include <zephyr/audio/dmic.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/clock_control_silabs.h>
#include <zephyr/drivers/dma.h>
#include <zephyr/drivers/dma/dma_silabs_ldma.h>
#include <zephyr/drivers/pinctrl.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>
#include <zephyr/pm/device_runtime.h>
#include <zephyr/pm/policy.h>
#include <zephyr/sys/util.h>

#include <sl_hal_ldma.h>
#include <sl_hal_pdm.h>

LOG_MODULE_REGISTER(dmic_silabs_pdm, CONFIG_AUDIO_DMIC_LOG_LEVEL);

/* The PDM clock is the peripheral clock divided by PRESC + 1 */
#define PDM_CLK_DIV_MAX ((_PDM_CFG1_PRESC_MASK >> _PDM_CFG1_PRESC_SHIFT) + 1)

/*
 * A CIC filter of order N grows the signal by a factor of DSR^N, which has to fit in the 32-bit
 * accumulator together with the sign bit. This bounds the DSR for each filter order.
 */
static const struct {
	uint8_t order;
	sl_hal_pdm_filter_order_t forder;
	uint16_t dsr_max;
} pdm_filters[] = {
	{5, SL_HAL_PDM_FILTER_ORDER_FIFTH, 73},
	{4, SL_HAL_PDM_FILTER_ORDER_FOURTH, 215},
	{3, SL_HAL_PDM_FILTER_ORDER_THIRD, 1290},
	{2, SL_HAL_PDM_FILTER_ORDER_SECOND, _PDM_CTRL_DSR_MASK >> _PDM_CTRL_DSR_SHIFT},
};

/*
 * Smallest DSR accepted by the clock search. Lower ratios hardly decimate the bitstream, and a
 * ratio of 0 would divide by zero when computing the resulting PCM rate. The value matches the
 * lower bound used by the Simplicity SDK microphone driver.
 */
#define PDM_DSR_MIN 3

/* Largest departure from the requested PCM rate, in thousandths, that is accepted */
#define PDM_RATE_ERROR_MAX 10

/* Each step of the gain shift is a factor of two */
#define PDM_GAIN_STEP_DB 6

/*
 * Left microphones (SELECT low) drive the data line while the clock is low, right ones (SELECT
 * high) while it is high.
 */
#define PDM_CLKPOL_LEFT  SL_HAL_PDM_CH_CLOCK_POLARITY_RISING_EDGE
#define PDM_CLKPOL_RIGHT SL_HAL_PDM_CH_CLOCK_POLARITY_FALLING_EDGE

struct dmic_silabs_pdm_config {
	PDM_TypeDef *base;
	const struct pinctrl_dev_config *pcfg;
	const struct device *clock_dev;
	const struct silabs_clock_control_cmu_config clock_cfg;
	const struct device *dma_dev;
	uint32_t dma_slot;
};

struct dmic_silabs_pdm_data {
	const struct device *dev;
	struct k_spinlock lock;
	struct k_msgq rx_queue;
	void *rx_queue_buf[CONFIG_AUDIO_DMIC_SILABS_PDM_QUEUE_SIZE];
	struct k_mem_slab *mem_slab;
	sl_hal_pdm_init_t pdm_init;
	struct dma_config dma_cfg;
	struct dma_block_config dma_blk;
	int dma_channel;
	uint32_t block_size;
	/* Blocks loaded in the DMA channel, the first one is being filled */
	void *blocks[2];
	uint8_t block_count;
	enum dmic_state state;
};

static void dmic_silabs_pdm_pm_lock_get(void)
{
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
}

static void dmic_silabs_pdm_pm_lock_put(void)
{
	pm_policy_state_lock_put(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_put(PM_STATE_STANDBY, PM_ALL_SUBSTATES);
}

/* Must be called with the lock held */
static int dmic_silabs_pdm_queue_block(struct dmic_silabs_pdm_data *data)
{
	const struct dmic_silabs_pdm_config *config = data->dev->config;
	void *block;
	int ret;

	ret = k_mem_slab_alloc(data->mem_slab, &block, K_NO_WAIT);
	if (ret < 0) {
		return ret;
	}

	data->dma_blk.dest_address = (uintptr_t)block;
	data->dma_blk.block_size = data->block_size;

	if (data->block_count == 0) {
		ret = dma_config(config->dma_dev, data->dma_channel, &data->dma_cfg);
		if (ret == 0) {
			ret = dma_start(config->dma_dev, data->dma_channel);
		}
	} else {
		ret = silabs_ldma_append_block(config->dma_dev, data->dma_channel, &data->dma_cfg);
	}
	if (ret < 0) {
		k_mem_slab_free(data->mem_slab, block);
		return ret;
	}

	data->blocks[data->block_count++] = block;

	return 0;
}

static void dmic_silabs_pdm_dma_callback(const struct device *dma_dev, void *user_data,
					 uint32_t channel, int status)
{
	struct dmic_silabs_pdm_data *data = user_data;
	const struct dmic_silabs_pdm_config *config = data->dev->config;
	void *done;
	k_spinlock_key_t key;
	int ret;

	key = k_spin_lock(&data->lock);

	if (data->state != DMIC_STATE_ACTIVE) {
		goto out;
	}

	if (status < 0) {
		LOG_ERR("DMA error %d", status);
		sl_hal_pdm_stop(config->base);
		data->state = DMIC_STATE_ERROR;
		goto out;
	}

	done = data->blocks[0];
	data->blocks[0] = data->blocks[1];
	data->block_count--;

	ret = k_msgq_put(&data->rx_queue, &done, K_NO_WAIT);
	if (ret < 0) {
		LOG_WRN("RX queue full, dropping block");
		k_mem_slab_free(data->mem_slab, done);
	}

	ret = dmic_silabs_pdm_queue_block(data);
	if (ret < 0) {
		LOG_ERR("No buffer for received data (%d), stopping", ret);
		sl_hal_pdm_stop(config->base);
		data->state = DMIC_STATE_ERROR;
	}

out:
	k_spin_unlock(&data->lock, key);
}

static void dmic_silabs_pdm_free_blocks(struct dmic_silabs_pdm_data *data)
{
	for (uint8_t i = 0; i < data->block_count; i++) {
		k_mem_slab_free(data->mem_slab, data->blocks[i]);
	}

	data->block_count = 0;
}

static void dmic_silabs_pdm_flush_queue(struct dmic_silabs_pdm_data *data)
{
	void *block;

	while (k_msgq_get(&data->rx_queue, &block, K_NO_WAIT) == 0) {
		k_mem_slab_free(data->mem_slab, block);
	}
}

static void dmic_silabs_pdm_stop(const struct device *dev, enum dmic_state state)
{
	const struct dmic_silabs_pdm_config *config = dev->config;
	struct dmic_silabs_pdm_data *data = dev->data;
	k_spinlock_key_t key;

	key = k_spin_lock(&data->lock);

	dma_stop(config->dma_dev, data->dma_channel);
	dmic_silabs_pdm_free_blocks(data);
	data->state = state;

	k_spin_unlock(&data->lock, key);

	sl_hal_pdm_deinit(config->base);

	dmic_silabs_pdm_pm_lock_put();
	pm_device_runtime_put(dev);
}

static int dmic_silabs_pdm_start(const struct device *dev)
{
	const struct dmic_silabs_pdm_config *config = dev->config;
	struct dmic_silabs_pdm_data *data = dev->data;
	k_spinlock_key_t key;
	int ret;

	ret = pm_device_runtime_get(dev);
	if (ret < 0) {
		return ret;
	}

	dmic_silabs_pdm_pm_lock_get();

	sl_hal_pdm_init(config->base, &data->pdm_init);
	sl_hal_pdm_clear(config->base);
	sl_hal_pdm_fifo_flush(config->base);

	key = k_spin_lock(&data->lock);

	/* Load two blocks so that the DMA always has the next one ready */
	ret = dmic_silabs_pdm_queue_block(data);
	if (ret == 0) {
		ret = dmic_silabs_pdm_queue_block(data);
	}

	if (ret == 0) {
		data->state = DMIC_STATE_ACTIVE;
		sl_hal_pdm_start(config->base);
	}

	k_spin_unlock(&data->lock, key);

	if (ret < 0) {
		LOG_ERR("Failed to start DMA: %d", ret);
		dmic_silabs_pdm_stop(dev, data->state);
		return ret;
	}

	return 0;
}

static int dmic_silabs_pdm_find_clock(uint32_t clock_rate, const struct dmic_cfg *cfg,
				      uint32_t dsr_max, uint32_t *div, uint32_t *dsr)
{
	const struct pdm_io_cfg *io = &cfg->io;
	uint32_t pcm_rate = cfg->streams[0].pcm_rate;
	uint32_t best_error = UINT32_MAX;
	uint32_t best_div = 0;
	uint32_t best_dsr = 0;

	for (uint32_t d = 1; d <= PDM_CLK_DIV_MAX; d++) {
		uint32_t pdm_clk = clock_rate / d;
		uint32_t n, actual, error;

		if (pdm_clk < io->min_pdm_clk_freq) {
			break;
		}

		if (io->max_pdm_clk_freq != 0 && pdm_clk > io->max_pdm_clk_freq) {
			continue;
		}

		n = DIV_ROUND_CLOSEST(pdm_clk, pcm_rate);
		if (n < PDM_DSR_MIN || n > dsr_max) {
			continue;
		}

		actual = DIV_ROUND_CLOSEST(clock_rate, d * n);
		error = (actual > pcm_rate) ? (actual - pcm_rate) : (pcm_rate - actual);

		/* Prefer the highest oversampling among equally accurate settings */
		if (error < best_error || (error == best_error && n > best_dsr)) {
			best_error = error;
			best_div = d;
			best_dsr = n;
		}
	}

	if (best_div == 0 ||
	    (uint64_t)best_error * 1000U > (uint64_t)pcm_rate * PDM_RATE_ERROR_MAX) {
		return -EINVAL;
	}

	*div = best_div;
	*dsr = best_dsr;

	return 0;
}

static uint32_t dmic_silabs_pdm_gain(uint32_t dsr, uint8_t order, int8_t gain_db)
{
	uint64_t full_scale = 1;
	int32_t gain;

	for (uint8_t i = 0; i < order; i++) {
		full_scale *= dsr;
	}

	/*
	 * Shift the filter output up as far as a full scale input stays within 32 bits. The output
	 * saturates, so full scale may reach 2^31 exactly.
	 */
	gain = 31 - (64 - __builtin_clzll(full_scale - 1U));
	gain += gain_db / PDM_GAIN_STEP_DB;

	return CLAMP(gain, 0, _PDM_CTRL_GAIN_MASK >> _PDM_CTRL_GAIN_SHIFT);
}

static int dmic_silabs_pdm_configure(const struct device *dev, struct dmic_cfg *cfg)
{
	const struct dmic_silabs_pdm_config *config = dev->config;
	struct dmic_silabs_pdm_data *data = dev->data;
	struct pdm_chan_cfg *channel = &cfg->channel;
	struct pcm_stream_cfg *stream = &cfg->streams[0];
	sl_hal_pdm_init_t *init = &data->pdm_init;
	uint32_t def_map, alt_map;
	uint32_t clock_rate;
	uint32_t sample_size;
	uint32_t frame_size;
	uint32_t dma_unit;
	uint32_t div, dsr;
	size_t filter;
	int ret;

	if (data->state == DMIC_STATE_ACTIVE) {
		LOG_ERR("Cannot configure device while it is active");
		return -EBUSY;
	}

	/*
	 * Both channels share the data line of PDM controller 0. The order of left and right in
	 * the output is selected by which clock edge each channel samples on.
	 */
	if (channel->req_num_chan == 1) {
		def_map = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT);
		alt_map = dmic_build_channel_map(0, 0, PDM_CHAN_RIGHT);
	} else {
		def_map = dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
			  dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT);
		alt_map = dmic_build_channel_map(0, 0, PDM_CHAN_RIGHT) |
			  dmic_build_channel_map(1, 0, PDM_CHAN_LEFT);
	}

	if (channel->req_num_streams != 1 || channel->req_num_chan < 1 ||
	    channel->req_num_chan > 2 ||
	    (channel->req_chan_map_lo != def_map && channel->req_chan_map_lo != alt_map) ||
	    channel->req_chan_map_hi != 0) {
		LOG_ERR("Requested channel configuration is not supported");
		return -EINVAL;
	}

	channel->act_num_streams = 1;
	channel->act_num_chan = channel->req_num_chan;
	channel->act_chan_map_lo = channel->req_chan_map_lo;
	channel->act_chan_map_hi = 0;

	if (data->state == DMIC_STATE_ERROR) {
		dmic_silabs_pdm_stop(dev, DMIC_STATE_CONFIGURED);
	}
	dmic_silabs_pdm_flush_queue(data);

	/* If either rate or width is 0, the stream is to be disabled */
	if (stream->pcm_rate == 0 || stream->pcm_width == 0) {
		data->state = DMIC_STATE_INITIALIZED;
		return 0;
	}

	if (stream->pcm_width != 16 && stream->pcm_width != 32) {
		LOG_ERR("Only 16-bit and 32-bit samples are supported");
		return -EINVAL;
	}

	if (stream->mem_slab == NULL) {
		return -EINVAL;
	}

	sample_size = stream->pcm_width / 8U;
	frame_size = sample_size * channel->req_num_chan;
	if (stream->block_size == 0 || (stream->block_size % frame_size) != 0 ||
	    stream->block_size > stream->mem_slab->info.block_size) {
		LOG_ERR("Invalid block size %u", stream->block_size);
		return -EINVAL;
	}

	/*
	 * 16-bit stereo samples are packed in pairs in one FIFO entry, everything else takes a
	 * whole entry per sample.
	 */
	dma_unit = (stream->pcm_width == 16 && channel->req_num_chan == 1) ? 2U : 4U;

	/* Each block is filled by a single DMA descriptor */
	if (stream->block_size > SL_HAL_LDMA_DESCRIPTOR_MAX_XFER_SIZE * dma_unit) {
		LOG_ERR("Block size %u does not fit in one DMA transfer", stream->block_size);
		return -EINVAL;
	}

	ret = clock_control_get_rate(config->clock_dev, (clock_control_subsys_t)&config->clock_cfg,
				     &clock_rate);
	if (ret < 0) {
		return ret;
	}

	/* Use the highest filter order that can decimate to the requested rate */
	for (filter = 0; filter < ARRAY_SIZE(pdm_filters); filter++) {
		ret = dmic_silabs_pdm_find_clock(clock_rate, cfg, pdm_filters[filter].dsr_max, &div,
						 &dsr);
		if (ret == 0) {
			break;
		}
	}

	if (ret < 0) {
		LOG_ERR("No PDM clock setting for %u Hz from %u Hz", stream->pcm_rate, clock_rate);
		return ret;
	}

	*init = (sl_hal_pdm_init_t)SL_HAL_PDM_INIT_DEFAULT;
	init->filter_order = pdm_filters[filter].forder;
	init->clk_prescaler = div - 1U;
	init->down_sampling_rate = dsr;
	init->gain = dmic_silabs_pdm_gain(dsr, pdm_filters[filter].order, stream->gain_db);

	if (channel->req_chan_map_lo == def_map) {
		init->ch0_clock_polarity = PDM_CLKPOL_LEFT;
		init->ch1_clock_polarity = PDM_CLKPOL_RIGHT;
	} else {
		init->ch0_clock_polarity = PDM_CLKPOL_RIGHT;
		init->ch1_clock_polarity = PDM_CLKPOL_LEFT;
	}

	if (channel->req_num_chan == 1) {
		init->ch0ch1_stereo_enable = false;
		init->number_channels = SL_HAL_PDM_NUMBER_OF_CHANNELS_ONE;
	} else {
		init->ch0ch1_stereo_enable = true;
		init->number_channels = SL_HAL_PDM_NUMBER_OF_CHANNELS_TWO;
	}

	if (stream->pcm_width == 32) {
		init->data_format = SL_HAL_PDM_DATA_FORMAT_FULL_32BIT;
	} else if (channel->req_num_chan == 2) {
		init->data_format = SL_HAL_PDM_DATA_FORMAT_DOUBLE_16;
	} else {
		init->data_format = SL_HAL_PDM_DATA_FORMAT_RIGHT_16;
	}

	data->dma_cfg.source_data_size = dma_unit;
	data->dma_cfg.dest_data_size = data->dma_cfg.source_data_size;
	data->dma_cfg.source_burst_length = data->dma_cfg.source_data_size;
	data->dma_cfg.dest_burst_length = data->dma_cfg.source_data_size;

	data->block_size = stream->block_size;
	data->mem_slab = stream->mem_slab;

	LOG_DBG("PDM clock %u Hz, filter order %u, DSR %u, gain %u", clock_rate / div,
		pdm_filters[filter].order, dsr, init->gain);

	data->state = DMIC_STATE_CONFIGURED;

	return 0;
}

static int dmic_silabs_pdm_trigger(const struct device *dev, enum dmic_trigger cmd)
{
	struct dmic_silabs_pdm_data *data = dev->data;

	switch (cmd) {
	case DMIC_TRIGGER_PAUSE:
		if (data->state == DMIC_STATE_ACTIVE) {
			dmic_silabs_pdm_stop(dev, DMIC_STATE_PAUSED);
		}
		break;

	case DMIC_TRIGGER_STOP:
	case DMIC_TRIGGER_RESET:
		if (data->state == DMIC_STATE_ACTIVE || data->state == DMIC_STATE_ERROR) {
			dmic_silabs_pdm_stop(dev, DMIC_STATE_CONFIGURED);
		}
		dmic_silabs_pdm_flush_queue(data);
		if (cmd == DMIC_TRIGGER_RESET) {
			data->state = DMIC_STATE_INITIALIZED;
		} else if (data->state == DMIC_STATE_PAUSED) {
			data->state = DMIC_STATE_CONFIGURED;
		}
		break;

	case DMIC_TRIGGER_START:
	case DMIC_TRIGGER_RELEASE:
		if (data->state == DMIC_STATE_ACTIVE) {
			break;
		}

		if (data->state == DMIC_STATE_ERROR) {
			dmic_silabs_pdm_stop(dev, DMIC_STATE_CONFIGURED);
		}

		if (data->state != DMIC_STATE_CONFIGURED && data->state != DMIC_STATE_PAUSED) {
			LOG_ERR("Device is not configured");
			return -EIO;
		}

		return dmic_silabs_pdm_start(dev);

	default:
		LOG_ERR("Invalid command: %d", cmd);
		return -EINVAL;
	}

	return 0;
}

static int dmic_silabs_pdm_read(const struct device *dev, uint8_t stream, void **buffer,
				size_t *size, int32_t timeout)
{
	struct dmic_silabs_pdm_data *data = dev->data;
	int ret;

	ARG_UNUSED(stream);

	if (data->state != DMIC_STATE_CONFIGURED && data->state != DMIC_STATE_ACTIVE &&
	    data->state != DMIC_STATE_PAUSED && data->state != DMIC_STATE_ERROR) {
		LOG_ERR("Device is not configured");
		return -EIO;
	}

	ret = k_msgq_get(&data->rx_queue, buffer,
			 (data->state == DMIC_STATE_ERROR) ? K_NO_WAIT : SYS_TIMEOUT_MS(timeout));
	if (ret < 0) {
		return (data->state == DMIC_STATE_ERROR) ? -EIO : ret;
	}

	*size = data->block_size;

	return 0;
}

static int dmic_silabs_pdm_pm_action(const struct device *dev, enum pm_device_action action)
{
	const struct dmic_silabs_pdm_config *config = dev->config;
	struct dmic_silabs_pdm_data *data = dev->data;
	int ret;

	switch (action) {
	case PM_DEVICE_ACTION_RESUME:
		ret = clock_control_on(config->clock_dev,
				       (clock_control_subsys_t)&config->clock_cfg);
		if (ret < 0 && ret != -EALREADY) {
			return ret;
		}

		ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_DEFAULT);
		if (ret < 0) {
			return ret;
		}
		break;

	case PM_DEVICE_ACTION_SUSPEND:
		if (data->state == DMIC_STATE_ACTIVE) {
			return -EBUSY;
		}

		ret = pinctrl_apply_state(config->pcfg, PINCTRL_STATE_SLEEP);
		if (ret < 0 && ret != -ENOENT) {
			return ret;
		}

		ret = clock_control_off(config->clock_dev,
					(clock_control_subsys_t)&config->clock_cfg);
		if (ret < 0) {
			return ret;
		}
		break;

	case PM_DEVICE_ACTION_TURN_ON:
	case PM_DEVICE_ACTION_TURN_OFF:
		break;

	default:
		return -ENOTSUP;
	}

	return 0;
}

static int dmic_silabs_pdm_init(const struct device *dev)
{
	const struct dmic_silabs_pdm_config *config = dev->config;
	struct dmic_silabs_pdm_data *data = dev->data;
	int ret;

	data->dev = dev;

	k_msgq_init(&data->rx_queue, (char *)data->rx_queue_buf, sizeof(void *),
		    ARRAY_SIZE(data->rx_queue_buf));

	if (!device_is_ready(config->dma_dev)) {
		LOG_ERR("DMA device not ready");
		return -ENODEV;
	}

	data->dma_channel = dma_request_channel(config->dma_dev, NULL);
	if (data->dma_channel < 0) {
		LOG_ERR("Failed to request DMA channel");
		return -ENODEV;
	}

	data->dma_blk.source_address = (uintptr_t)&config->base->RXDATA;
	data->dma_blk.source_addr_adj = DMA_ADDR_ADJ_NO_CHANGE;
	data->dma_blk.dest_addr_adj = DMA_ADDR_ADJ_INCREMENT;
	data->dma_cfg.dma_slot = config->dma_slot;
	data->dma_cfg.channel_direction = PERIPHERAL_TO_MEMORY;
	data->dma_cfg.complete_callback_en = 1;
	data->dma_cfg.block_count = 1;
	data->dma_cfg.head_block = &data->dma_blk;
	data->dma_cfg.dma_callback = dmic_silabs_pdm_dma_callback;
	data->dma_cfg.user_data = data;

	ret = pm_device_driver_init(dev, dmic_silabs_pdm_pm_action);
	if (ret < 0) {
		return ret;
	}

	data->state = DMIC_STATE_INITIALIZED;

	return 0;
}

static DEVICE_API(dmic, dmic_silabs_pdm_api) = {
	.configure = dmic_silabs_pdm_configure,
	.trigger = dmic_silabs_pdm_trigger,
	.read = dmic_silabs_pdm_read,
};

#define DMIC_SILABS_PDM_INIT(inst)                                                                 \
	PINCTRL_DT_INST_DEFINE(inst);                                                              \
	PM_DEVICE_DT_INST_DEFINE(inst, dmic_silabs_pdm_pm_action);                                 \
                                                                                                   \
	static const struct dmic_silabs_pdm_config dmic_silabs_pdm_config_##inst = {               \
		.base = (PDM_TypeDef *)DT_INST_REG_ADDR(inst),                                     \
		.pcfg = PINCTRL_DT_INST_DEV_CONFIG_GET(inst),                                      \
		.clock_dev = DEVICE_DT_GET(DT_INST_CLOCKS_CTLR(inst)),                             \
		.clock_cfg = SILABS_DT_INST_CLOCK_CFG(inst),                                       \
		.dma_dev = DEVICE_DT_GET(DT_INST_DMAS_CTLR_BY_NAME(inst, rx)),                     \
		.dma_slot = SILABS_LDMA_REQSEL_TO_SLOT(DT_INST_DMAS_CELL_BY_NAME(inst, rx, slot)), \
	};                                                                                         \
                                                                                                   \
	static struct dmic_silabs_pdm_data dmic_silabs_pdm_data_##inst;                            \
                                                                                                   \
	DEVICE_DT_INST_DEFINE(inst, dmic_silabs_pdm_init, PM_DEVICE_DT_INST_GET(inst),             \
			      &dmic_silabs_pdm_data_##inst, &dmic_silabs_pdm_config_##inst,        \
			      POST_KERNEL, CONFIG_AUDIO_DMIC_INIT_PRIORITY, &dmic_silabs_pdm_api);

DT_INST_FOREACH_STATUS_OKAY(DMIC_SILABS_PDM_INIT)
