#ifndef APP_MOTOR_CONTROL_H_
#define APP_MOTOR_CONTROL_H_

#include <stdint.h>

#define APP_MOTOR_DUTY_MIN_PERMILLE 800U
#define APP_MOTOR_DUTY_MAX_PERMILLE 1000U

enum app_motor_state {
	APP_MOTOR_STATE_DISABLED,
	APP_MOTOR_STATE_READY,
	APP_MOTOR_STATE_RUNNING,
	APP_MOTOR_STATE_STOPPING,
	APP_MOTOR_STATE_FAULT,
};

enum app_motor_direction {
	APP_MOTOR_DIRECTION_FORWARD,
	APP_MOTOR_DIRECTION_REVERSE,
};

int app_motor_init(void);
int app_motor_start(void);
int app_motor_stop(void);
int app_motor_toggle_direction(void);
int app_motor_reset(void);
int app_motor_set_target_duty(uint16_t duty_permille);
int app_motor_process(uint32_t elapsed_ms);
void app_motor_fail(int error);

enum app_motor_state app_motor_get_state(void);
enum app_motor_direction app_motor_get_direction(void);
uint16_t app_motor_get_duty_permille(void);
uint16_t app_motor_get_target_duty_permille(void);
int app_motor_get_last_error(void);

#endif /* APP_MOTOR_CONTROL_H_ */
