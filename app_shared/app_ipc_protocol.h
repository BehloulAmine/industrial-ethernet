#ifndef APP_IPC_PROTOCOL_H_
#define APP_IPC_PROTOCOL_H_

#include <stdint.h>

#define APP_IPC_ENDPOINT_NAME "motor-control"
#define APP_IPC_FRAME_MAGIC 0x49504331UL
#define APP_IPC_FRAME_VERSION 1U

enum app_ipc_frame_type {
	APP_IPC_FRAME_PING = 1U,
	APP_IPC_FRAME_PONG = 2U,
	APP_IPC_FRAME_MOTOR_COMMAND = 3U,
	APP_IPC_FRAME_MOTOR_STATE = 4U,
};

/* All multi-byte fields are encoded little-endian on the IPC wire. */
struct app_ipc_frame {
	uint32_t magic_le;
	uint16_t version_le;
	uint16_t type_le;
	uint32_t sequence_le;
	uint32_t payload_length_le;
};

#endif /* APP_IPC_PROTOCOL_H_ */
