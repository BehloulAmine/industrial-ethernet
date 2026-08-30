#include "app_ipc.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app_ipc_protocol.h"
#include "app_motor_contract.h"
#include "motor_control.h"

BUILD_ASSERT(sizeof(struct app_ipc_frame) == 16U,
	     "Unexpected IPC frame size");

static const struct device *const ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
static struct ipc_ept ipc_endpoint;
static atomic_t endpoint_bound;
static atomic_t endpoint_initialized;
static atomic_t pending_ping;
static atomic_t pending_sequence;
static uint32_t state_sequence;
static uint16_t state_heartbeat;

static void frame_init(struct app_ipc_frame *frame, enum app_ipc_frame_type type,
		       uint32_t sequence, uint32_t value)
{
	frame->magic_le = sys_cpu_to_le32(APP_IPC_FRAME_MAGIC);
	frame->version_le = sys_cpu_to_le16(APP_IPC_FRAME_VERSION);
	frame->type_le = sys_cpu_to_le16((uint16_t)type);
	frame->sequence_le = sys_cpu_to_le32(sequence);
	frame->payload_length_le = sys_cpu_to_le32(value);
}

static bool frame_is_valid(const struct app_ipc_frame *frame, size_t len,
			   enum app_ipc_frame_type type)
{
	return (len >= sizeof(*frame)) &&
	       (sys_le32_to_cpu(frame->magic_le) == APP_IPC_FRAME_MAGIC) &&
	       (sys_le16_to_cpu(frame->version_le) == APP_IPC_FRAME_VERSION) &&
	       (sys_le16_to_cpu(frame->type_le) == (uint16_t)type);
}

static void endpoint_bound_callback(void *priv)
{
	ARG_UNUSED(priv);
	atomic_set(&endpoint_bound, 1);
}

static void endpoint_unbound_callback(void *priv)
{
	ARG_UNUSED(priv);
	atomic_set(&endpoint_bound, 0);
}

static void endpoint_received(const void *data, size_t len, void *priv)
{
	const struct app_ipc_frame *frame = data;

	ARG_UNUSED(priv);

	if (!frame_is_valid(frame, len, APP_IPC_FRAME_PING) ||
	    (len != sizeof(*frame)) ||
	    (sys_le32_to_cpu(frame->payload_length_le) != 0U)) {
		return;
	}

	atomic_set(&pending_sequence,
		   (atomic_val_t)sys_le32_to_cpu(frame->sequence_le));
	atomic_set(&pending_ping, 1);
}

static const struct ipc_ept_cfg endpoint_config = {
	.name = APP_IPC_ENDPOINT_NAME,
	.cb = {
		.bound = endpoint_bound_callback,
		.unbound = endpoint_unbound_callback,
		.received = endpoint_received,
	},
};

int app_ipc_init(void)
{
	int ret;

	if (atomic_get(&endpoint_initialized) != 0) {
		return 0;
	}

	if (!device_is_ready(ipc_instance)) {
		return -ENODEV;
	}

	ret = ipc_service_open_instance(ipc_instance);
	if ((ret < 0) && (ret != -EALREADY)) {
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc_instance, &ipc_endpoint, &endpoint_config);
	if (ret < 0) {
		return ret;
	}

	atomic_set(&endpoint_initialized, 1);
	return 0;
}

void app_ipc_process(void)
{
	struct app_ipc_frame frame;
	uint32_t sequence;
	int ret;

	if ((atomic_get(&endpoint_initialized) == 0) ||
	    (atomic_get(&endpoint_bound) == 0) ||
	    !atomic_cas(&pending_ping, 1, 0)) {
		return;
	}

	sequence = (uint32_t)atomic_get(&pending_sequence);
	frame_init(&frame, APP_IPC_FRAME_PONG, sequence, 0U);
	ret = ipc_service_send(&ipc_endpoint, &frame, sizeof(frame));
	if (ret < 0) {
		atomic_set(&pending_ping, 1);
	}
}

static uint16_t motor_state_flags(void)
{
	uint16_t flags = APP_MOTOR_STATE_FLAG_LOCAL | APP_MOTOR_STATE_FLAG_IPC_VALID;

	switch (app_motor_get_state()) {
	case APP_MOTOR_STATE_DISABLED:
		flags |= APP_MOTOR_STATE_FLAG_DISABLED;
		break;
	case APP_MOTOR_STATE_READY:
		flags |= APP_MOTOR_STATE_FLAG_READY;
		break;
	case APP_MOTOR_STATE_RUNNING:
		flags |= APP_MOTOR_STATE_FLAG_RUNNING;
		break;
	case APP_MOTOR_STATE_STOPPING:
		flags |= APP_MOTOR_STATE_FLAG_STOPPING;
		break;
	case APP_MOTOR_STATE_FAULT:
		flags |= APP_MOTOR_STATE_FLAG_FAULT;
		break;
	}

	return flags;
}

void app_ipc_publish_motor_state(uint16_t buttons, uint16_t potentiometer_raw,
				 uint16_t loop_time_ms)
{
	struct app_motor_state_message message = { 0 };
	int ret;

	if ((atomic_get(&endpoint_initialized) == 0) ||
	    (atomic_get(&endpoint_bound) == 0)) {
		return;
	}

	frame_init(&message.header, APP_IPC_FRAME_MOTOR_STATE, ++state_sequence,
		   APP_MOTOR_STATE_WORD_COUNT * sizeof(uint16_t));
	message.words_le[APP_MOTOR_STATE_FLAGS] =
		sys_cpu_to_le16(motor_state_flags());
	message.words_le[APP_MOTOR_STATE_APPLIED_DUTY_PERMILLE] =
		sys_cpu_to_le16(app_motor_get_duty_permille());
	message.words_le[APP_MOTOR_STATE_TARGET_DUTY_PERMILLE] =
		sys_cpu_to_le16(app_motor_get_target_duty_permille());
	message.words_le[APP_MOTOR_STATE_DIRECTION] =
		sys_cpu_to_le16((uint16_t)app_motor_get_direction());
	message.words_le[APP_MOTOR_STATE_FAULT_CODE] =
		sys_cpu_to_le16((uint16_t)app_motor_get_last_error());
	message.words_le[APP_MOTOR_STATE_BUTTONS] = sys_cpu_to_le16(buttons);
	message.words_le[APP_MOTOR_STATE_POTENTIOMETER_RAW] =
		sys_cpu_to_le16(potentiometer_raw);
	message.words_le[APP_MOTOR_STATE_LOOP_TIME_MS] = sys_cpu_to_le16(loop_time_ms);
	message.words_le[APP_MOTOR_STATE_HEARTBEAT] =
		sys_cpu_to_le16(++state_heartbeat);

	ret = ipc_service_send(&ipc_endpoint, &message, sizeof(message));
	if (ret < 0) {
		return;
	}
}
