#ifndef APP_MOTOR_BUTTONS_H_
#define APP_MOTOR_BUTTONS_H_

#include <stdint.h>

#define APP_BUTTON_EVENT_START     (1U << 0)
#define APP_BUTTON_EVENT_STOP      (1U << 1)
#define APP_BUTTON_EVENT_DIRECTION (1U << 2)
#define APP_BUTTON_EVENT_RESET     (1U << 3)

int app_motor_buttons_init(void);
uint32_t app_motor_buttons_poll(void);

#endif /* APP_MOTOR_BUTTONS_H_ */
