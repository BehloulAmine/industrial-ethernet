#include "motor_control.h"

#include <errno.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

#define MOTOR_NODE DT_PATH(zephyr_user)
#define MOTOR_START_BOOST_PERMILLE 1000U
#define MOTOR_START_BOOST_MS 250U
#define MOTOR_RUN_RAMP_PERMILLE_PER_SECOND 1000U
#define MOTOR_STOP_RAMP_PERMILLE_PER_SECOND 4000U

static const struct gpio_dt_spec motor_in1 =
	GPIO_DT_SPEC_GET(MOTOR_NODE, motor_in1_gpios);
static const struct gpio_dt_spec motor_in2 =
	GPIO_DT_SPEC_GET(MOTOR_NODE, motor_in2_gpios);
static const struct pwm_dt_spec motor_pwm = PWM_DT_SPEC_GET(MOTOR_NODE);

static enum app_motor_state motor_state = APP_MOTOR_STATE_DISABLED;
static enum app_motor_direction motor_direction = APP_MOTOR_DIRECTION_FORWARD;
static uint16_t motor_duty_permille;
static uint16_t motor_target_duty_permille = APP_MOTOR_DUTY_MIN_PERMILLE;
static uint32_t motor_boost_remaining_ms;
static int motor_last_error;

static int motor_make_safe(void)
{
	int first_error = 0;
	int ret;

	ret = pwm_set_pulse_dt(&motor_pwm, 0U);
	if (ret < 0) {
		first_error = ret;
	}

	ret = gpio_pin_set_dt(&motor_in1, 0);
	if ((ret < 0) && (first_error == 0)) {
		first_error = ret;
	}

	ret = gpio_pin_set_dt(&motor_in2, 0);
	if ((ret < 0) && (first_error == 0)) {
		first_error = ret;
	}

	motor_duty_permille = 0U;
	motor_boost_remaining_ms = 0U;
	return first_error;
}

static void motor_record_fault(int error)
{
	(void)motor_make_safe();
	motor_last_error = error < 0 ? error : -EIO;
	motor_state = APP_MOTOR_STATE_FAULT;
}

static int motor_set_pwm(uint16_t duty_permille)
{
	uint32_t pulse;
	int ret;

	if (duty_permille > APP_MOTOR_DUTY_MAX_PERMILLE) {
		return -EINVAL;
	}

	pulse = (uint32_t)(((uint64_t)motor_pwm.period * duty_permille) / 1000U);
	ret = pwm_set_pulse_dt(&motor_pwm, pulse);
	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	motor_duty_permille = duty_permille;
	return 0;
}

static int motor_apply_direction(void)
{
	int in1 = motor_direction == APP_MOTOR_DIRECTION_FORWARD ? 1 : 0;
	int in2 = motor_direction == APP_MOTOR_DIRECTION_FORWARD ? 0 : 1;
	int ret;

	ret = gpio_pin_set_dt(&motor_in1, in1);
	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	ret = gpio_pin_set_dt(&motor_in2, in2);
	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	return 0;
}

static uint16_t motor_ramp_step(uint32_t rate_per_second, uint32_t elapsed_ms)
{
	uint32_t step = (rate_per_second * elapsed_ms) / 1000U;

	if ((step == 0U) && (elapsed_ms != 0U)) {
		step = 1U;
	}

	return (uint16_t)step;
}

static int motor_ramp_towards(uint16_t target, uint16_t step)
{
	uint16_t next = motor_duty_permille;

	if (next < target) {
		next = (target - next) <= step ? target : next + step;
	} else if (next > target) {
		next = (next - target) <= step ? target : next - step;
	}

	return motor_set_pwm(next);
}

int app_motor_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&motor_in1) || !gpio_is_ready_dt(&motor_in2) ||
	    !pwm_is_ready_dt(&motor_pwm)) {
		motor_record_fault(-ENODEV);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&motor_in1, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&motor_in2, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	ret = motor_make_safe();
	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	motor_direction = APP_MOTOR_DIRECTION_FORWARD;
	motor_last_error = 0;
	motor_state = APP_MOTOR_STATE_READY;
	return 0;
}

int app_motor_start(void)
{
	int ret;

	if (motor_state == APP_MOTOR_STATE_FAULT) {
		return -EIO;
	}

	if (motor_state == APP_MOTOR_STATE_RUNNING) {
		return 0;
	}

	if (motor_state != APP_MOTOR_STATE_READY) {
		return -EBUSY;
	}

	ret = motor_apply_direction();
	if (ret < 0) {
		return ret;
	}

	ret = motor_set_pwm(MOTOR_START_BOOST_PERMILLE);
	if (ret < 0) {
		return ret;
	}

	motor_boost_remaining_ms = MOTOR_START_BOOST_MS;
	motor_state = APP_MOTOR_STATE_RUNNING;
	return 0;
}

int app_motor_stop(void)
{
	if (motor_state == APP_MOTOR_STATE_FAULT) {
		return -EIO;
	}

	if ((motor_state == APP_MOTOR_STATE_READY) ||
	    (motor_state == APP_MOTOR_STATE_DISABLED)) {
		return 0;
	}

	motor_boost_remaining_ms = 0U;
	motor_state = APP_MOTOR_STATE_STOPPING;
	return 0;
}

int app_motor_toggle_direction(void)
{
	if (motor_state == APP_MOTOR_STATE_FAULT) {
		return -EIO;
	}

	if (motor_state != APP_MOTOR_STATE_READY) {
		return -EBUSY;
	}

	motor_direction = motor_direction == APP_MOTOR_DIRECTION_FORWARD ?
		APP_MOTOR_DIRECTION_REVERSE : APP_MOTOR_DIRECTION_FORWARD;
	return 0;
}

int app_motor_reset(void)
{
	int ret = motor_make_safe();

	if (ret < 0) {
		motor_record_fault(ret);
		return ret;
	}

	motor_direction = APP_MOTOR_DIRECTION_FORWARD;
	motor_last_error = 0;
	motor_state = APP_MOTOR_STATE_READY;
	return 0;
}

int app_motor_set_target_duty(uint16_t duty_permille)
{
	if ((duty_permille < APP_MOTOR_DUTY_MIN_PERMILLE) ||
	    (duty_permille > APP_MOTOR_DUTY_MAX_PERMILLE)) {
		return -ERANGE;
	}

	motor_target_duty_permille = duty_permille;
	return 0;
}

int app_motor_process(uint32_t elapsed_ms)
{
	uint16_t step;
	int ret;

	if (motor_state == APP_MOTOR_STATE_RUNNING) {
		if (motor_boost_remaining_ms != 0U) {
			if (elapsed_ms >= motor_boost_remaining_ms) {
				motor_boost_remaining_ms = 0U;
			} else {
				motor_boost_remaining_ms -= elapsed_ms;
			}
			return 0;
		}

		step = motor_ramp_step(MOTOR_RUN_RAMP_PERMILLE_PER_SECOND,
					     elapsed_ms);
		return motor_ramp_towards(motor_target_duty_permille, step);
	}

	if (motor_state == APP_MOTOR_STATE_STOPPING) {
		step = motor_ramp_step(MOTOR_STOP_RAMP_PERMILLE_PER_SECOND,
					     elapsed_ms);
		ret = motor_ramp_towards(0U, step);
		if (ret < 0) {
			return ret;
		}

		if (motor_duty_permille == 0U) {
			ret = motor_make_safe();
			if (ret < 0) {
				motor_record_fault(ret);
				return ret;
			}
			motor_state = APP_MOTOR_STATE_READY;
		}
	}

	return 0;
}

void app_motor_fail(int error)
{
	motor_record_fault(error);
}

enum app_motor_state app_motor_get_state(void)
{
	return motor_state;
}

enum app_motor_direction app_motor_get_direction(void)
{
	return motor_direction;
}

uint16_t app_motor_get_duty_permille(void)
{
	return motor_duty_permille;
}

uint16_t app_motor_get_target_duty_permille(void)
{
	return motor_target_duty_permille;
}

int app_motor_get_last_error(void)
{
	return motor_last_error;
}
