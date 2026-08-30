#ifndef APP_IPC_H_
#define APP_IPC_H_

#include <stdbool.h>
#include <stdint.h>

struct app_ipc_status {
	bool initialized;
	bool bound;
	int last_error;
};

int app_ipc_init(void);
int app_ipc_ping(uint32_t *round_trip_ms);
void app_ipc_get_status(struct app_ipc_status *status);

#endif /* APP_IPC_H_ */
