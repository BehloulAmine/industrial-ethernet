#include "motor_buttons.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

#define MOTOR_NODE DT_PATH(zephyr_user)
#define BUTTON_DEBOUNCE_SAMPLES 3U

struct app_button {
	struct gpio_dt_spec gpio;
	uint32_t event;
	bool candidate;
	bool stable;
	uint8_t candidate_samples;
};

static struct app_button buttons[] = {
	{
		.gpio = GPIO_DT_SPEC_GET(MOTOR_NODE, start_gpios),
		.event = APP_BUTTON_EVENT_START,
	},
	{
		.gpio = GPIO_DT_SPEC_GET(MOTOR_NODE, stop_gpios),
		.event = APP_BUTTON_EVENT_STOP,
	},
	{
		.gpio = GPIO_DT_SPEC_GET(MOTOR_NODE, direction_gpios),
		.event = APP_BUTTON_EVENT_DIRECTION,
	},
	{
		.gpio = GPIO_DT_SPEC_GET(MOTOR_NODE, reset_gpios),
		.event = APP_BUTTON_EVENT_RESET,
	},
};

int app_motor_buttons_init(void)
{
	int ret;

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		if (!gpio_is_ready_dt(&buttons[i].gpio)) {
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&buttons[i].gpio, GPIO_INPUT);
		if (ret < 0) {
			return ret;
		}

		buttons[i].candidate = false;
		buttons[i].stable = false;
		buttons[i].candidate_samples = 0U;
	}

	return 0;
}

uint32_t app_motor_buttons_poll(void)
{
	uint32_t events = 0U;

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		int value = gpio_pin_get_dt(&buttons[i].gpio);
		bool pressed;

		if (value < 0) {
			continue;
		}

		pressed = value != 0;
		if (pressed != buttons[i].candidate) {
			buttons[i].candidate = pressed;
			buttons[i].candidate_samples = 1U;
			continue;
		}

		if (buttons[i].candidate_samples < BUTTON_DEBOUNCE_SAMPLES) {
			buttons[i].candidate_samples++;
		}

		if ((buttons[i].candidate_samples == BUTTON_DEBOUNCE_SAMPLES) &&
		    (buttons[i].stable != buttons[i].candidate)) {
			buttons[i].stable = buttons[i].candidate;
			if (buttons[i].stable) {
				events |= buttons[i].event;
			}
		}
	}

	return events;
}

uint16_t app_motor_buttons_get_state(void)
{
	uint16_t state = 0U;

	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		if (buttons[i].stable) {
			state |= (uint16_t)buttons[i].event;
		}
	}

	return state;
}
