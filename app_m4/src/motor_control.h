#ifndef APP_MOTOR_CONTROL_H_
#define APP_MOTOR_CONTROL_H_

#include <stdint.h>

enum app_motor_state {
	APP_MOTOR_STATE_READY,
	APP_MOTOR_STATE_RUNNING,
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

enum app_motor_state app_motor_get_state(void);
enum app_motor_direction app_motor_get_direction(void);
uint16_t app_motor_get_duty_permille(void);
int app_motor_get_last_error(void);

#endif /* APP_MOTOR_CONTROL_H_ */
