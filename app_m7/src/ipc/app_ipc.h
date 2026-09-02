#ifndef APP_IPC_H_
#define APP_IPC_H_

#include <stdbool.h>
#include <stdint.h>

#include "app_motor_contract.h"

#define APP_IPC_MOTOR_STATE_STALE_MS 100U

struct app_ipc_status {
	bool initialized;
	bool bound;
	int last_error;
};

struct app_ipc_motor_state {
	bool valid;
	bool stale;
	uint32_t sequence;
	uint32_t age_ms;
	uint16_t words[APP_MOTOR_STATE_WORD_COUNT];
};

struct app_ipc_motor_command {
	uint16_t control;
	uint16_t duty_permille;
	uint16_t accel_ramp;
	uint16_t decel_ramp;
	uint16_t timeout_ms;
	uint16_t source;
	bool remote;
};

int app_ipc_init(void);
int app_ipc_ping(uint32_t *round_trip_ms);
void app_ipc_get_status(struct app_ipc_status *status);
void app_ipc_get_motor_state(struct app_ipc_motor_state *state);
int app_ipc_motor_submit(const struct app_ipc_motor_command *command);

#endif /* APP_IPC_H_ */
