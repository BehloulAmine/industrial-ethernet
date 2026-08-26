#include "motor_potentiometer.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>

#include "motor_control.h"

#define POTENTIOMETER_NODE DT_PATH(zephyr_user)
#define POTENTIOMETER_ADC_MAX 4095U
#define POTENTIOMETER_FILTER_DIVISOR 8
#define POTENTIOMETER_DEADBAND_PERMILLE 4U

static const struct adc_dt_spec potentiometer_adc =
	ADC_DT_SPEC_GET_BY_IDX(POTENTIOMETER_NODE, 0);

static int32_t potentiometer_filtered_raw;
static uint16_t potentiometer_raw;
static uint16_t potentiometer_duty = APP_MOTOR_DUTY_MIN_PERMILLE;
static bool potentiometer_filter_ready;

static uint16_t potentiometer_raw_to_duty(uint16_t raw)
{
	uint32_t range = APP_MOTOR_DUTY_MAX_PERMILLE - APP_MOTOR_DUTY_MIN_PERMILLE;

	return APP_MOTOR_DUTY_MIN_PERMILLE +
		(uint16_t)(((uint32_t)raw * range) / POTENTIOMETER_ADC_MAX);
}

int app_motor_potentiometer_init(void)
{
	int ret;

	if (!adc_is_ready_dt(&potentiometer_adc)) {
		return -ENODEV;
	}

	ret = adc_channel_setup_dt(&potentiometer_adc);
	if (ret < 0) {
		return ret;
	}

	potentiometer_filter_ready = false;
	return 0;
}

int app_motor_potentiometer_poll(uint16_t *duty_permille)
{
	uint16_t sample;
	uint16_t new_duty;
	uint16_t difference;
	struct adc_sequence sequence = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};
	int ret;

	if (duty_permille == NULL) {
		return -EINVAL;
	}

	ret = adc_sequence_init_dt(&potentiometer_adc, &sequence);
	if (ret < 0) {
		return ret;
	}

	ret = adc_read_dt(&potentiometer_adc, &sequence);
	if (ret < 0) {
		return ret;
	}

	if (sample > POTENTIOMETER_ADC_MAX) {
		sample = POTENTIOMETER_ADC_MAX;
	}

	potentiometer_raw = sample;
	if (!potentiometer_filter_ready) {
		potentiometer_filtered_raw = sample;
		potentiometer_filter_ready = true;
	} else {
		potentiometer_filtered_raw +=
			((int32_t)sample - potentiometer_filtered_raw) /
			POTENTIOMETER_FILTER_DIVISOR;
	}

	new_duty = potentiometer_raw_to_duty((uint16_t)potentiometer_filtered_raw);
	difference = new_duty > potentiometer_duty ?
		new_duty - potentiometer_duty : potentiometer_duty - new_duty;
	if (difference >= POTENTIOMETER_DEADBAND_PERMILLE) {
		potentiometer_duty = new_duty;
	}

	*duty_permille = potentiometer_duty;
	return 0;
}

uint16_t app_motor_potentiometer_get_raw(void)
{
	return potentiometer_raw;
}
