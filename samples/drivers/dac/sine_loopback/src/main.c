/*
 * Copyright (c) 2026 Silicon Laboratories Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The DAC generates a sine wave on a pad, the ADC samples the same pad at its own rate, and an FFT of
 * the samples recovers the frequency of the sine wave and its amplitude.
 */

#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/dac.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <arm_math.h>

#define NUM_SAMPLES       512
#define SINE_FREQUENCY_HZ CONFIG_SAMPLE_SINE_FREQUENCY_HZ

static const struct dac_dt_spec dac_spec = DAC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), dac);
static const struct adc_dt_spec adc_spec = ADC_DT_SPEC_GET_BY_NAME(DT_PATH(zephyr_user), adc);

static uint16_t samples[NUM_SAMPLES];
static float32_t signal[NUM_SAMPLES];
static float32_t spectrum[NUM_SAMPLES];
static float32_t power[NUM_SAMPLES / 2];

static uint32_t dac_mid;
static uint32_t dac_amplitude;

static int64_t first_sample_ticks;
static int64_t last_sample_ticks;

/* Writes the sine value for the current time, so that the DAC output follows the real time base */
static void dac_update(struct k_timer *timer)
{
	ARG_UNUSED(timer);

	int64_t ticks = k_uptime_ticks();
	/* Position within the period, in ticks of one second */
	int64_t phase = (ticks * SINE_FREQUENCY_HZ) % CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	float32_t angle = 2.0f * PI * (float32_t)phase / (float32_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	int32_t value = (int32_t)dac_mid + (int32_t)((float32_t)dac_amplitude * arm_sin_f32(angle));

	(void)dac_write_value_dt(&dac_spec, (uint32_t)value);
}

K_TIMER_DEFINE(dac_timer, dac_update, NULL);

static enum adc_action sample_done(const struct device *dev, const struct adc_sequence *sequence,
				   uint16_t sampling_index)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(sequence);

	int64_t ticks = k_uptime_ticks();

	if (sampling_index == 0) {
		first_sample_ticks = ticks;
	}
	last_sample_ticks = ticks;

	return ADC_ACTION_CONTINUE;
}

static int setup(void)
{
	int ret;

	if (!dac_is_ready_dt(&dac_spec) || !adc_is_ready_dt(&adc_spec)) {
		printk("DAC or ADC device not ready\n");
		return -ENODEV;
	}

	ret = dac_channel_setup_dt(&dac_spec);
	if (ret < 0) {
		printk("DAC channel setup failed (%d)\n", ret);
		return ret;
	}

	ret = adc_channel_setup_dt(&adc_spec);
	if (ret < 0) {
		printk("ADC channel setup failed (%d)\n", ret);
		return ret;
	}

	dac_mid = BIT(dac_spec.channel_cfg.resolution - 1);
	dac_amplitude = dac_mid * CONFIG_SAMPLE_AMPLITUDE_PERCENT / 100;
	return 0;
}

static int acquire(void)
{
	const struct adc_sequence_options options = {
		.interval_us = CONFIG_SAMPLE_ADC_INTERVAL_US,
		.extra_samplings = NUM_SAMPLES - 1,
		.callback = sample_done,
	};
	struct adc_sequence sequence = {
		.options = &options,
		.buffer = samples,
		.buffer_size = sizeof(samples),
	};
	int ret;

	ret = adc_sequence_init_dt(&adc_spec, &sequence);
	if (ret < 0) {
		return ret;
	}
	return adc_read_dt(&adc_spec, &sequence);
}

int main(void)
{
	arm_rfft_fast_instance_f32 fft;
	float32_t mean, rms, peak_power;
	uint32_t peak;
	int ret;

	ret = setup();
	if (ret < 0) {
		printk("Frequency check: FAIL\n");
		return 0;
	}

	printk("Generating %d Hz, DAC updated every %d us, ADC sampled every %d us\n",
	       SINE_FREQUENCY_HZ, CONFIG_SAMPLE_DAC_UPDATE_US, CONFIG_SAMPLE_ADC_INTERVAL_US);

	k_timer_start(&dac_timer, K_NO_WAIT, K_USEC(CONFIG_SAMPLE_DAC_UPDATE_US));
	/* Let the output settle before sampling */
	k_sleep(K_MSEC(20));

	ret = acquire();
	k_timer_stop(&dac_timer);
	if (ret < 0) {
		printk("ADC read failed (%d)\n", ret);
		printk("Frequency check: FAIL\n");
		return 0;
	}

	/* The sampling rate the ADC really ran at, from the time stamps of the first and last sample */
	float32_t duration = (float32_t)(last_sample_ticks - first_sample_ticks) /
			     (float32_t)CONFIG_SYS_CLOCK_TICKS_PER_SEC;
	float32_t sample_rate = (float32_t)(NUM_SAMPLES - 1) / duration;

	/* Remove the DC level, then apply a Hann window to limit leakage between bins */
	for (int i = 0; i < NUM_SAMPLES; i++) {
		signal[i] = (float32_t)samples[i];
	}
	arm_mean_f32(signal, NUM_SAMPLES, &mean);
	arm_offset_f32(signal, -mean, signal, NUM_SAMPLES);
	arm_rms_f32(signal, NUM_SAMPLES, &rms);
	for (int i = 0; i < NUM_SAMPLES; i++) {
		float32_t w = 0.5f - 0.5f * arm_cos_f32(2.0f * PI * i / (NUM_SAMPLES - 1));

		signal[i] *= w;
	}

	arm_rfft_fast_init_512_f32(&fft);
	arm_rfft_fast_f32(&fft, signal, spectrum, 0);
	/* spectrum[0] and [1] hold the DC and Nyquist bins; the others are complex pairs */
	spectrum[1] = 0.0f;
	arm_cmplx_mag_squared_f32(spectrum, power, NUM_SAMPLES / 2);
	power[0] = 0.0f;
	arm_max_f32(power, NUM_SAMPLES / 2, &peak_power, &peak);

	/* Parabolic interpolation of the peak between its neighbouring bins */
	float32_t offset = 0.0f;

	if (peak > 0 && peak < NUM_SAMPLES / 2 - 1) {
		float32_t left = power[peak - 1];
		float32_t right = power[peak + 1];
		float32_t denominator = left - 2.0f * peak_power + right;

		if (denominator != 0.0f) {
			offset = 0.5f * (left - right) / denominator;
		}
	}

	float32_t bin_width = sample_rate / NUM_SAMPLES;
	float32_t frequency = ((float32_t)peak + offset) * bin_width;
	/* Amplitude of a sine from its RMS value, in ADC codes */
	float32_t amplitude = rms * 1.41421356f;
	float32_t expected_amplitude = (float32_t)dac_amplitude *
				       BIT(adc_spec.resolution) / BIT(dac_spec.channel_cfg.resolution);

	printk("ADC ran at %d.%03d Hz, mean %d, amplitude %d codes (expected %d)\n",
	       (int)sample_rate, (int)(sample_rate * 1000) % 1000, (int)mean, (int)amplitude,
	       (int)expected_amplitude);
	printk("Recovered frequency %d.%03d Hz (bin %u of %d.%03d Hz)\n", (int)frequency,
	       (int)(frequency * 1000) % 1000, peak, (int)bin_width,
	       (int)(bin_width * 1000) % 1000);

	bool frequency_ok = fabsf(frequency - SINE_FREQUENCY_HZ) <= bin_width;
	bool amplitude_ok = amplitude > 0.8f * expected_amplitude && amplitude < 1.2f * expected_amplitude;

	printk("Frequency check: %s\n", frequency_ok && amplitude_ok ? "PASS" : "FAIL");
	return 0;
}
