#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>

#include "motor_buttons.h"
#include "motor_control.h"
#include "motor_potentiometer.h"

#define MOTOR_LOOP_PERIOD_MS 10
#define READY_BLINK_PERIOD_MS 500
#define STOPPING_BLINK_PERIOD_MS 100

static const struct gpio_dt_spec blue_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec red_led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);

static bool status_leds_init(void)
{
	if (!gpio_is_ready_dt(&blue_led) || !gpio_is_ready_dt(&red_led)) {
		return false;
	}

	if (gpio_pin_configure_dt(&blue_led, GPIO_OUTPUT_INACTIVE) < 0) {
		return false;
	}

	if (gpio_pin_configure_dt(&red_led, GPIO_OUTPUT_INACTIVE) < 0) {
		return false;
	}

	return true;
}

static void status_leds_update(void)
{
	enum app_motor_state state = app_motor_get_state();
	bool blue_on = false;
	bool red_on = false;

	switch (state) {
	case APP_MOTOR_STATE_DISABLED:
		break;
	case APP_MOTOR_STATE_READY:
		blue_on = ((k_uptime_get() / READY_BLINK_PERIOD_MS) % 2) != 0;
		break;
	case APP_MOTOR_STATE_RUNNING:
		blue_on = true;
		break;
	case APP_MOTOR_STATE_STOPPING:
		blue_on = ((k_uptime_get() / STOPPING_BLINK_PERIOD_MS) % 2) != 0;
		break;
	case APP_MOTOR_STATE_FAULT:
		red_on = true;
		break;
	}

	(void)gpio_pin_set_dt(&blue_led, blue_on);
	(void)gpio_pin_set_dt(&red_led, red_on);
}

int main(void)
{
	(void)k_thread_name_set(k_current_get(), "motor_main");

	bool leds_ready = status_leds_init();
	int motor_ret = app_motor_init();
	int buttons_ret = app_motor_buttons_init();
	int potentiometer_ret = app_motor_potentiometer_init();

	if ((motor_ret < 0) || (buttons_ret < 0) || (potentiometer_ret < 0)) {
		if (leds_ready) {
			(void)gpio_pin_set_dt(&blue_led, 0);
			(void)gpio_pin_set_dt(&red_led, 1);
		}

		while (true) {
			k_sleep(K_FOREVER);
		}
	}

	while (true) {
		uint16_t requested_duty;
		uint32_t events = app_motor_buttons_poll();
		int ret = app_motor_potentiometer_poll(&requested_duty);

		if (ret < 0) {
			app_motor_fail(ret);
		} else {
			(void)app_motor_set_target_duty(requested_duty);
		}

		/* STOP has absolute priority if several buttons are pressed together. */
		if ((events & APP_BUTTON_EVENT_STOP) != 0U) {
			(void)app_motor_stop();
		} else if ((events & APP_BUTTON_EVENT_RESET) != 0U) {
			(void)app_motor_reset();
		} else {
			if ((events & APP_BUTTON_EVENT_DIRECTION) != 0U) {
				(void)app_motor_toggle_direction();
			}

			if ((events & APP_BUTTON_EVENT_START) != 0U) {
				(void)app_motor_start();
			}
		}

		(void)app_motor_process(MOTOR_LOOP_PERIOD_MS);

		if (leds_ready) {
			status_leds_update();
		}

		k_msleep(MOTOR_LOOP_PERIOD_MS);
	}

	return 0;
}
