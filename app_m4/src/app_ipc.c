#include "app_ipc.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/spinlock.h>
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
static struct k_spinlock command_lock;
static struct app_motor_command_message pending_command;
static bool command_pending;
static bool remote_mode;
static bool remote_timed_out;
static bool remote_stop_latched;
static uint16_t remote_timeout_ms;
static uint16_t last_accepted_sequence;
static uint32_t remote_command_timestamp_ms;
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

static bool command_is_valid(const struct app_motor_command_message *message,
			     size_t len)
{
	const struct app_ipc_frame *frame = &message->header;
	uint16_t control;
	uint16_t duty;
	uint16_t accel;
	uint16_t decel;
	uint16_t mode;
	uint16_t timeout;
	uint16_t sequence;

	if (!frame_is_valid(frame, len, APP_IPC_FRAME_MOTOR_COMMAND) ||
	    (len != sizeof(*message)) ||
	    (sys_le32_to_cpu(frame->payload_length_le) !=
	     APP_MOTOR_COMMAND_WORD_COUNT * sizeof(uint16_t))) {
		return false;
	}

	control = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_CONTROL]);
	duty = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_DUTY_PERMILLE]);
	accel = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_ACCEL_RAMP]);
	decel = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_DECEL_RAMP]);
	mode = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_MODE]);
	timeout = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_TIMEOUT_MS]);
	sequence = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_SEQUENCE]);

	return ((control & ~(APP_MOTOR_COMMAND_ENABLE | APP_MOTOR_COMMAND_RUN |
			     APP_MOTOR_COMMAND_DIRECTION_REVERSE |
			     APP_MOTOR_COMMAND_RESET_FAULT |
			     APP_MOTOR_COMMAND_QUICK_STOP)) == 0U) &&
	       (duty >= APP_MOTOR_DUTY_MIN_PERMILLE) &&
	       (duty <= APP_MOTOR_DUTY_MAX_PERMILLE) &&
	       (accel >= APP_MOTOR_COMMAND_ACCEL_MIN_PERMILLE_PER_SECOND) &&
	       (accel <= APP_MOTOR_COMMAND_ACCEL_MAX_PERMILLE_PER_SECOND) &&
	       (decel >= APP_MOTOR_COMMAND_ACCEL_MIN_PERMILLE_PER_SECOND) &&
	       (decel <= APP_MOTOR_COMMAND_ACCEL_MAX_PERMILLE_PER_SECOND) &&
	       (timeout >= APP_MOTOR_COMMAND_TIMEOUT_MIN_MS) &&
	       (timeout <= APP_MOTOR_COMMAND_TIMEOUT_MAX_MS) &&
	       (message->words_le[APP_MOTOR_COMMAND_RESERVED_0] == 0U) &&
	       (message->words_le[APP_MOTOR_COMMAND_RESERVED_1] == 0U) &&
	       (message->words_le[APP_MOTOR_COMMAND_RESERVED_2] == 0U) &&
	       (sequence == (uint16_t)sys_le32_to_cpu(frame->sequence_le)) &&
	       ((mode == 0U) ||
		(mode == (APP_MOTOR_COMMAND_MODE_REMOTE |
			 APP_MOTOR_COMMAND_MODE_SOURCE(APP_MOTOR_COMMAND_SOURCE_SHELL))));
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
	uint16_t type;

	ARG_UNUSED(priv);

	if ((len < sizeof(*frame)) ||
	    (sys_le32_to_cpu(frame->magic_le) != APP_IPC_FRAME_MAGIC) ||
	    (sys_le16_to_cpu(frame->version_le) != APP_IPC_FRAME_VERSION)) {
		return;
	}

	type = sys_le16_to_cpu(frame->type_le);
	if (type == APP_IPC_FRAME_PING) {
		if ((len != sizeof(*frame)) ||
		    (sys_le32_to_cpu(frame->payload_length_le) != 0U)) {
			return;
		}

		atomic_set(&pending_sequence,
			   (atomic_val_t)sys_le32_to_cpu(frame->sequence_le));
		atomic_set(&pending_ping, 1);
		return;
	}

	if (type == APP_IPC_FRAME_MOTOR_COMMAND) {
		const struct app_motor_command_message *message = data;
		k_spinlock_key_t key;

		if (!command_is_valid(message, len)) {
			return;
		}

		key = k_spin_lock(&command_lock);
		pending_command = *message;
		command_pending = true;
		k_spin_unlock(&command_lock, key);
	}
}

static int apply_remote_command(const struct app_motor_command_message *message)
{
	uint16_t control = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_CONTROL]);
	uint16_t mode = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_MODE]);
	uint16_t sequence = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_SEQUENCE]);
	int ret = 0;

	if (mode == 0U) {
		if ((control & APP_MOTOR_COMMAND_RESET_FAULT) != 0U) {
			ret = app_motor_reset();
			if (ret < 0) {
				return ret;
			}
		}

		ret = app_motor_stop();
		if (ret < 0) {
			return ret;
		}

		remote_mode = false;
		remote_timed_out = false;
		remote_stop_latched = false;
		app_motor_restore_default_ramps();
		last_accepted_sequence = sequence;
		return 0;
	}

	remote_mode = true;
	remote_timed_out = false;
	remote_timeout_ms = sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_TIMEOUT_MS]);
	remote_command_timestamp_ms = k_uptime_get_32();

	ret = app_motor_set_ramps(
		sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_ACCEL_RAMP]),
		sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_DECEL_RAMP]));
	if (ret < 0) {
		return ret;
	}

	ret = app_motor_set_target_duty(
		sys_le16_to_cpu(message->words_le[APP_MOTOR_COMMAND_DUTY_PERMILLE]));
	if (ret < 0) {
		return ret;
	}

	if ((control & APP_MOTOR_COMMAND_RESET_FAULT) != 0U) {
		remote_stop_latched = false;
		ret = app_motor_reset();
	} else if ((control & APP_MOTOR_COMMAND_QUICK_STOP) != 0U) {
		ret = app_motor_quick_stop();
	} else if (remote_stop_latched &&
		   ((control & APP_MOTOR_COMMAND_RUN) != 0U)) {
		ret = -ECANCELED;
	} else if ((control & APP_MOTOR_COMMAND_RUN) == 0U) {
		/* An external stop must never be blocked by a deferred direction change. */
		ret = app_motor_stop();
		if ((ret == 0) &&
		    (app_motor_get_state() == APP_MOTOR_STATE_READY)) {
			enum app_motor_direction direction =
				(control & APP_MOTOR_COMMAND_DIRECTION_REVERSE) != 0U ?
				APP_MOTOR_DIRECTION_REVERSE : APP_MOTOR_DIRECTION_FORWARD;

			if (direction != app_motor_get_direction()) {
				ret = app_motor_set_direction(direction);
			}
		}
	} else {
		enum app_motor_direction direction =
			(control & APP_MOTOR_COMMAND_DIRECTION_REVERSE) != 0U ?
			APP_MOTOR_DIRECTION_REVERSE : APP_MOTOR_DIRECTION_FORWARD;

		if (direction != app_motor_get_direction()) {
			ret = app_motor_set_direction(direction);
		}
	}

	if ((ret == 0) && ((control & APP_MOTOR_COMMAND_RUN) != 0U) &&
	    ((control & APP_MOTOR_COMMAND_ENABLE) != 0U)) {
		ret = app_motor_start();
	}

	if (ret == 0) {
		last_accepted_sequence = sequence;
	}

	return ret;
}

static void process_pending_command(void)
{
	struct app_motor_command_message message;
	k_spinlock_key_t key;

	key = k_spin_lock(&command_lock);
	if (!command_pending) {
		k_spin_unlock(&command_lock, key);
		return;
	}
	message = pending_command;
	command_pending = false;
	k_spin_unlock(&command_lock, key);

	(void)apply_remote_command(&message);
}

static void process_remote_timeout(void)
{
	if (!remote_mode || remote_timed_out ||
	    ((k_uptime_get_32() - remote_command_timestamp_ms) <= remote_timeout_ms)) {
		return;
	}

	remote_timed_out = true;
	(void)app_motor_stop();
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

	process_pending_command();
	process_remote_timeout();

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

bool app_ipc_is_remote_mode(void)
{
	return remote_mode;
}

void app_ipc_latch_remote_stop(void)
{
	if (remote_mode) {
		remote_stop_latched = true;
	}
}

void app_ipc_clear_remote_stop(void)
{
	remote_stop_latched = false;
}

static uint16_t motor_state_flags(void)
{
	uint16_t flags = APP_MOTOR_STATE_FLAG_IPC_VALID;

	flags |= remote_mode ? APP_MOTOR_STATE_FLAG_REMOTE : APP_MOTOR_STATE_FLAG_LOCAL;
	if (remote_timed_out) {
		flags |= APP_MOTOR_STATE_FLAG_REMOTE_TIMEOUT;
	}
	if (remote_stop_latched) {
		flags |= APP_MOTOR_STATE_FLAG_REMOTE_STOP_LATCH;
	}

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
	if (remote_mode) {
		message.words_le[APP_MOTOR_STATE_COMMAND_AGE_MS] = sys_cpu_to_le16(
			(uint16_t)(k_uptime_get_32() - remote_command_timestamp_ms));
	}
	message.words_le[APP_MOTOR_STATE_LOOP_TIME_MS] = sys_cpu_to_le16(loop_time_ms);
	message.words_le[APP_MOTOR_STATE_HEARTBEAT] =
		sys_cpu_to_le16(++state_heartbeat);
	message.words_le[APP_MOTOR_STATE_LAST_ACCEPTED_SEQUENCE] =
		sys_cpu_to_le16(last_accepted_sequence);

	ret = ipc_service_send(&ipc_endpoint, &message, sizeof(message));
	if (ret < 0) {
		return;
	}
}
