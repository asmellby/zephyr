.. zephyr:code-sample:: dac-sine-loopback
   :name: DAC sine wave loopback
   :relevant-api: dac_interface adc_interface

   Generate a sine wave with the DAC, sample it with the ADC and recover its frequency with an FFT.

Overview
********

A timer writes the value of a sine wave to the DAC every :kconfig:option:`CONFIG_SAMPLE_DAC_UPDATE_US`
microseconds, computed from the system time so that the output follows the real time base. The
ADC samples the same pad 512 times at :kconfig:option:`CONFIG_SAMPLE_ADC_INTERVAL_US`, and the
sample measures the rate it really ran at from the time stamps of the first and last samples.

The samples are processed with CMSIS-DSP: the mean is removed, a Hann window applied, and a real
FFT computed. The peak bin, refined by parabolic interpolation, gives the frequency of the sine
wave, and the RMS of the samples its amplitude. The check passes when the frequency is within one
FFT bin of :kconfig:option:`CONFIG_SAMPLE_SINE_FREQUENCY_HZ` and the amplitude within 20 % of the
generated one.

On the Silicon Labs EFR32xG24 and EFR32xG26 boards, VDAC0 channel 0 drives a pad through its
auxiliary output and the analog bus, and IADC0 reads the same pad through another line of the
bus, so no external wiring is needed. The sample runs the same on the kits and in Renode.

Requirements
************

The board's :ref:`/zephyr,user <dt-zephyr-user>` node must have ``io-channels`` named ``dac`` and
``adc`` that reach the same pad, with the DAC and the ADC using the same reference voltage. See the
overlays in :zephyr_file:`samples/drivers/dac/sine_loopback/boards`.

Building and Running
********************

.. zephyr-app-commands::
   :zephyr-app: samples/drivers/dac/sine_loopback
   :board: xg24_rb4186c
   :goals: build flash
   :compact:

Sample Output
=============

.. code-block:: console

   Generating 50 Hz, DAC updated every 250 us, ADC sampled every 1000 us
   ADC ran at 992.969 Hz, mean 2027, amplitude 1422 codes (expected 1433)
   Recovered frequency 50.186 Hz (bin 26 of 1.939 Hz)
   Frequency check: PASS
