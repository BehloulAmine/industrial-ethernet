#ifndef APP_MOTOR_POTENTIOMETER_H_
#define APP_MOTOR_POTENTIOMETER_H_

#include <stdint.h>

int app_motor_potentiometer_init(void);
int app_motor_potentiometer_poll(uint16_t *duty_permille);
uint16_t app_motor_potentiometer_get_raw(void);

#endif /* APP_MOTOR_POTENTIOMETER_H_ */
