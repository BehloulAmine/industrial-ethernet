#ifndef APP_IPC_H_
#define APP_IPC_H_

#include <stdint.h>

int app_ipc_init(void);
void app_ipc_process(void);
void app_ipc_publish_motor_state(uint16_t buttons, uint16_t potentiometer_raw,
				 uint16_t loop_time_ms);

#endif /* APP_IPC_H_ */
