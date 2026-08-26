#include "motor_control.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/pwm.h>

#define MOTOR_NODE DT_PATH(zephyr_user)
#define MOTOR_FIXED_DUTY_PERMILLE 1000U

static const struct gpio_dt_spec motor_in1 =
	GPIO_DT_SPEC_GET(MOTOR_NODE, motor_in1_gpios);
static const struct gpio_dt_spec motor_in2 =
	GPIO_DT_SPEC_GET(MOTOR_NODE, motor_in2_gpios);
static const struct pwm_dt_spec motor_pwm = PWM_DT_SPEC_GET(MOTOR_NODE);

static enum app_motor_state motor_state = APP_MOTOR_STATE_FAULT;
static enum app_motor_direction motor_direction = APP_MOTOR_DIRECTION_FORWARD;
static uint16_t motor_duty_permille;
static int motor_last_error;

static void motor_record_fault(int error)
{
	(void)pwm_set_pulse_dt(&motor_pwm, 0U);
	(void)gpio_pin_set_dt(&motor_in1, 0);
	(void)gpio_pin_set_dt(&motor_in2, 0);

	motor_duty_permille = 0U;
	motor_last_error = error;
	motor_state = APP_MOTOR_STATE_FAULT;
}

static int motor_set_pwm(uint16_t duty_permille)
{
	uint32_t pulse;
	int ret;

	if (duty_permille > 1000U)
	{
		return -EINVAL;
	}

	pulse = (uint32_t)(((uint64_t)motor_pwm.period * duty_permille) / 1000U);
	ret = pwm_set_pulse_dt(&motor_pwm, pulse);
	if (ret < 0)
	{
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
	if (ret < 0)
	{
		motor_record_fault(ret);
		return ret;
	}

	ret = gpio_pin_set_dt(&motor_in2, in2);
	if (ret < 0)
	{
		motor_record_fault(ret);
		return ret;
	}

	return 0;
}

static int motor_make_safe(void)
{
	int first_error = 0;
	int ret;

	ret = pwm_set_pulse_dt(&motor_pwm, 0U);
	if (ret < 0)
	{
		first_error = ret;
	}

	ret = gpio_pin_set_dt(&motor_in1, 0);
	if ((ret < 0) && (first_error == 0))
	{
		first_error = ret;
	}

	ret = gpio_pin_set_dt(&motor_in2, 0);
	if ((ret < 0) && (first_error == 0))
	{
		first_error = ret;
	}

	motor_duty_permille = 0U;
	return first_error;
}

int app_motor_init(void)
{
	int ret;

	if (!gpio_is_ready_dt(&motor_in1) || !gpio_is_ready_dt(&motor_in2) ||
		!pwm_is_ready_dt(&motor_pwm))
	{
		motor_record_fault(-ENODEV);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&motor_in1, GPIO_OUTPUT_INACTIVE);
	if (ret < 0)
	{
		motor_record_fault(ret);
		return ret;
	}

	ret = gpio_pin_configure_dt(&motor_in2, GPIO_OUTPUT_INACTIVE);
	if (ret < 0)
	{
		motor_record_fault(ret);
		return ret;
	}

	ret = motor_make_safe();
	if (ret < 0)
	{
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

	if (motor_state == APP_MOTOR_STATE_FAULT)
	{
		return -EIO;
	}

	if (motor_state == APP_MOTOR_STATE_RUNNING)
	{
		return 0;
	}

	ret = motor_apply_direction();
	if (ret < 0)
	{
		return ret;
	}

	ret = motor_set_pwm(MOTOR_FIXED_DUTY_PERMILLE);
	if (ret < 0)
	{
		return ret;
	}

	motor_state = APP_MOTOR_STATE_RUNNING;
	return 0;
}

int app_motor_stop(void)
{
	int ret;

	ret = motor_make_safe();
	if (ret < 0)
	{
		motor_record_fault(ret);
		return ret;
	}

	if (motor_state != APP_MOTOR_STATE_FAULT)
	{
		motor_state = APP_MOTOR_STATE_READY;
	}

	return 0;
}

int app_motor_toggle_direction(void)
{
	if (motor_state == APP_MOTOR_STATE_FAULT)
	{
		return -EIO;
	}

	if (motor_state != APP_MOTOR_STATE_READY)
	{
		return -EBUSY;
	}

	motor_direction = motor_direction == APP_MOTOR_DIRECTION_FORWARD ? APP_MOTOR_DIRECTION_REVERSE : APP_MOTOR_DIRECTION_FORWARD;
	return 0;
}

int app_motor_reset(void)
{
	int ret = motor_make_safe();

	if (ret < 0)
	{
		motor_record_fault(ret);
		return ret;
	}

	motor_direction = APP_MOTOR_DIRECTION_FORWARD;
	motor_last_error = 0;
	motor_state = APP_MOTOR_STATE_READY;
	return 0;
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

int app_motor_get_last_error(void)
{
	return motor_last_error;
}
