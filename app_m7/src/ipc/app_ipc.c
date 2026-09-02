#include "app_ipc.h"

#include <errno.h>
#include <stddef.h>
#include <stdlib.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/spinlock.h>
#include <zephyr/sys/util.h>

#include "app_ipc_protocol.h"
#include "app_motor_contract.h"

#define APP_IPC_PING_TIMEOUT_MS 250
#define APP_IPC_SEND_RETRY_MS 20
#define APP_IPC_REMOTE_REFRESH_MS 100
#define APP_IPC_REMOTE_TIMEOUT_MS 1000

BUILD_ASSERT(sizeof(struct app_ipc_frame) == 16U,
	     "Unexpected IPC frame size");

static const struct device *const ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
static struct ipc_ept ipc_endpoint;
static K_SEM_DEFINE(pong_sem, 0, 1);
static K_MUTEX_DEFINE(ping_lock);
static atomic_t endpoint_is_bound;
static atomic_t endpoint_initialized;
static atomic_t next_sequence;
static atomic_t received_sequence;
static atomic_t last_error;
static struct k_spinlock motor_state_lock;
static struct app_ipc_motor_state motor_state_cache;
static K_MUTEX_DEFINE(remote_command_lock);
static atomic_t next_command_sequence;

struct app_remote_command {
	bool enabled;
	bool remote;
	uint16_t control;
	uint16_t duty_permille;
	uint16_t accel_ramp;
	uint16_t decel_ramp;
	uint16_t timeout_ms;
	uint16_t source;
};

static struct app_remote_command remote_command = {
	.duty_permille = APP_MOTOR_COMMAND_DUTY_MIN_PERMILLE,
	.accel_ramp = 1000U,
	.decel_ramp = 4000U,
	.timeout_ms = APP_IPC_REMOTE_TIMEOUT_MS,
	.source = APP_MOTOR_COMMAND_SOURCE_SHELL,
};
static void remote_refresh_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(remote_refresh_work, remote_refresh_work_handler);

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
	atomic_set(&endpoint_is_bound, 1);
	atomic_set(&last_error, 0);
}

static void endpoint_unbound(void *priv)
{
	ARG_UNUSED(priv);
	atomic_set(&endpoint_is_bound, 0);
}

static void endpoint_received(const void *data, size_t len, void *priv)
{
	const struct app_ipc_frame *frame = data;
	uint16_t type;

	ARG_UNUSED(priv);

	if ((len < sizeof(*frame)) ||
	    (sys_le32_to_cpu(frame->magic_le) != APP_IPC_FRAME_MAGIC) ||
	    (sys_le16_to_cpu(frame->version_le) != APP_IPC_FRAME_VERSION)) {
		atomic_set(&last_error, -EBADMSG);
		return;
	}

	type = sys_le16_to_cpu(frame->type_le);
	if (type == APP_IPC_FRAME_PONG) {
		if (!frame_is_valid(frame, len, APP_IPC_FRAME_PONG) ||
		    (len != sizeof(*frame)) ||
		    (sys_le32_to_cpu(frame->payload_length_le) != 0U)) {
			atomic_set(&last_error, -EBADMSG);
			return;
		}

		atomic_set(&received_sequence,
			   (atomic_val_t)sys_le32_to_cpu(frame->sequence_le));
		k_sem_give(&pong_sem);
		return;
	}

	if (type == APP_IPC_FRAME_MOTOR_STATE) {
		const struct app_motor_state_message *message = data;
		k_spinlock_key_t key;

		if ((len != sizeof(*message)) ||
		    (sys_le32_to_cpu(frame->payload_length_le) !=
		     APP_MOTOR_STATE_WORD_COUNT * sizeof(uint16_t))) {
			atomic_set(&last_error, -EBADMSG);
			return;
		}

		key = k_spin_lock(&motor_state_lock);
		for (size_t i = 0; i < APP_MOTOR_STATE_WORD_COUNT; i++) {
			motor_state_cache.words[i] = sys_le16_to_cpu(message->words_le[i]);
		}
		motor_state_cache.sequence = sys_le32_to_cpu(frame->sequence_le);
		motor_state_cache.age_ms = k_uptime_get_32();
		motor_state_cache.valid = true;
		k_spin_unlock(&motor_state_lock, key);
		return;
	}

	atomic_set(&last_error, -EBADMSG);
}

static void endpoint_error(const char *message, void *priv)
{
	ARG_UNUSED(message);
	ARG_UNUSED(priv);
	atomic_set(&last_error, -EIO);
}

static const struct ipc_ept_cfg endpoint_config = {
	.name = APP_IPC_ENDPOINT_NAME,
	.cb = {
		.bound = endpoint_bound_callback,
		.unbound = endpoint_unbound,
		.received = endpoint_received,
		.error = endpoint_error,
	},
};

int app_ipc_init(void)
{
	int ret;

	if (atomic_get(&endpoint_initialized) != 0) {
		return 0;
	}

	if (!device_is_ready(ipc_instance)) {
		atomic_set(&last_error, -ENODEV);
		return -ENODEV;
	}

	ret = ipc_service_open_instance(ipc_instance);
	if ((ret < 0) && (ret != -EALREADY)) {
		atomic_set(&last_error, ret);
		return ret;
	}

	ret = ipc_service_register_endpoint(ipc_instance, &ipc_endpoint, &endpoint_config);
	if (ret < 0) {
		atomic_set(&last_error, ret);
		return ret;
	}

	atomic_set(&endpoint_initialized, 1);
	atomic_set(&last_error, 0);
	return 0;
}

int app_ipc_ping(uint32_t *round_trip_ms)
{
	struct app_ipc_frame frame;
	uint32_t sequence;
	uint32_t start_ms;
	int ret;

	if (round_trip_ms == NULL) {
		return -EINVAL;
	}

	if (atomic_get(&endpoint_initialized) == 0) {
		return -EAGAIN;
	}

	if (atomic_get(&endpoint_is_bound) == 0) {
		return -ENOTCONN;
	}

	k_mutex_lock(&ping_lock, K_FOREVER);
	k_sem_reset(&pong_sem);
	sequence = (uint32_t)atomic_inc(&next_sequence) + 1U;
	atomic_set(&received_sequence, 0);
	start_ms = k_uptime_get_32();
	frame_init(&frame, APP_IPC_FRAME_PING, sequence, 0U);

	for (uint32_t attempt = 0U; ; attempt++) {
		ret = ipc_service_send(&ipc_endpoint, &frame, sizeof(frame));
		if ((ret >= 0) || ((ret != -EAGAIN) && (ret != -EBUSY) &&
				     (ret != -ENOBUFS)) ||
		    (attempt >= APP_IPC_SEND_RETRY_MS)) {
			break;
		}

		k_msleep(1);
	}

	if (ret < 0) {
		atomic_set(&last_error, ret);
		k_mutex_unlock(&ping_lock);
		return ret;
	}

	ret = k_sem_take(&pong_sem, K_MSEC(APP_IPC_PING_TIMEOUT_MS));
	if (ret < 0) {
		atomic_set(&last_error, -ETIMEDOUT);
		k_mutex_unlock(&ping_lock);
		return -ETIMEDOUT;
	}

	if ((uint32_t)atomic_get(&received_sequence) != sequence) {
		atomic_set(&last_error, -EBADMSG);
		k_mutex_unlock(&ping_lock);
		return -EBADMSG;
	}

	*round_trip_ms = k_uptime_get_32() - start_ms;
	atomic_set(&last_error, 0);
	k_mutex_unlock(&ping_lock);
	return 0;
}

static int send_remote_command_locked(bool remote)
{
	struct app_motor_command_message message = { 0 };
	uint32_t sequence;
	int ret;

	if ((atomic_get(&endpoint_initialized) == 0) ||
	    (atomic_get(&endpoint_is_bound) == 0)) {
		return -ENOTCONN;
	}

	sequence = (uint32_t)atomic_inc(&next_command_sequence) + 1U;
	frame_init(&message.header, APP_IPC_FRAME_MOTOR_COMMAND, sequence,
		   APP_MOTOR_COMMAND_WORD_COUNT * sizeof(uint16_t));
	message.words_le[APP_MOTOR_COMMAND_CONTROL] =
		sys_cpu_to_le16(remote_command.control);
	message.words_le[APP_MOTOR_COMMAND_DUTY_PERMILLE] =
		sys_cpu_to_le16(remote_command.duty_permille);
	message.words_le[APP_MOTOR_COMMAND_ACCEL_RAMP] =
		sys_cpu_to_le16(remote_command.accel_ramp);
	message.words_le[APP_MOTOR_COMMAND_DECEL_RAMP] =
		sys_cpu_to_le16(remote_command.decel_ramp);
	message.words_le[APP_MOTOR_COMMAND_MODE] = sys_cpu_to_le16(
		remote ? APP_MOTOR_COMMAND_MODE_REMOTE |
			 APP_MOTOR_COMMAND_MODE_SOURCE(remote_command.source) : 0U);
	message.words_le[APP_MOTOR_COMMAND_TIMEOUT_MS] =
		sys_cpu_to_le16(remote_command.timeout_ms);
	message.words_le[APP_MOTOR_COMMAND_SEQUENCE] =
		sys_cpu_to_le16((uint16_t)sequence);

	for (uint32_t attempt = 0U; ; attempt++) {
		ret = ipc_service_send(&ipc_endpoint, &message, sizeof(message));
		if ((ret >= 0) || ((ret != -EAGAIN) && (ret != -EBUSY) &&
				     (ret != -ENOBUFS)) ||
		    (attempt >= APP_IPC_SEND_RETRY_MS)) {
			break;
		}
		k_msleep(1);
	}

	if (ret < 0) {
		atomic_set(&last_error, ret);
	}

	return ret;
}

static void remote_refresh_work_handler(struct k_work *work)
{
	int ret;

	ARG_UNUSED(work);

	k_mutex_lock(&remote_command_lock, K_FOREVER);
	if (!remote_command.enabled) {
		k_mutex_unlock(&remote_command_lock);
		return;
	}

	ret = send_remote_command_locked(true);
	if (ret >= 0) {
		(void)k_work_reschedule(&remote_refresh_work,
					K_MSEC(APP_IPC_REMOTE_REFRESH_MS));
	}
	k_mutex_unlock(&remote_command_lock);
}

int app_ipc_motor_submit(const struct app_ipc_motor_command *command)
{
	int ret;

	if ((command == NULL) ||
	    ((command->source != APP_MOTOR_COMMAND_SOURCE_SHELL) &&
	     (command->source != APP_MOTOR_COMMAND_SOURCE_MODBUS)) ||
	    (command->duty_permille < APP_MOTOR_COMMAND_DUTY_MIN_PERMILLE) ||
	    (command->duty_permille > APP_MOTOR_COMMAND_DUTY_MAX_PERMILLE) ||
	    (command->accel_ramp < APP_MOTOR_COMMAND_ACCEL_MIN_PERMILLE_PER_SECOND) ||
	    (command->accel_ramp > APP_MOTOR_COMMAND_ACCEL_MAX_PERMILLE_PER_SECOND) ||
	    (command->decel_ramp < APP_MOTOR_COMMAND_ACCEL_MIN_PERMILLE_PER_SECOND) ||
	    (command->decel_ramp > APP_MOTOR_COMMAND_ACCEL_MAX_PERMILLE_PER_SECOND) ||
	    (command->timeout_ms < APP_MOTOR_COMMAND_TIMEOUT_MIN_MS) ||
	    (command->timeout_ms > APP_MOTOR_COMMAND_TIMEOUT_MAX_MS)) {
		return -EINVAL;
	}

	k_mutex_lock(&remote_command_lock, K_FOREVER);
	remote_command.control = command->control;
	remote_command.duty_permille = command->duty_permille;
	remote_command.accel_ramp = command->accel_ramp;
	remote_command.decel_ramp = command->decel_ramp;
	remote_command.timeout_ms = command->timeout_ms;
	remote_command.source = command->source;
	remote_command.remote = command->remote;
	remote_command.enabled = command->remote;
	ret = send_remote_command_locked(command->remote);
	if ((ret >= 0) && command->remote) {
		(void)k_work_reschedule(&remote_refresh_work,
					K_MSEC(APP_IPC_REMOTE_REFRESH_MS));
	} else if ((ret >= 0) && !command->remote) {
		(void)k_work_cancel_delayable(&remote_refresh_work);
	}
	k_mutex_unlock(&remote_command_lock);

	return ret;
}

static int remote_command_update(bool activate_remote, uint16_t control,
				 uint16_t duty_permille)
{
	int ret;

	k_mutex_lock(&remote_command_lock, K_FOREVER);
	if (!remote_command.enabled && !activate_remote) {
		k_mutex_unlock(&remote_command_lock);
		return -EACCES;
	}

	remote_command.enabled = true;
	remote_command.remote = true;
	remote_command.control = control;
	remote_command.duty_permille = duty_permille;
	remote_command.timeout_ms = APP_IPC_REMOTE_TIMEOUT_MS;
	remote_command.source = APP_MOTOR_COMMAND_SOURCE_SHELL;
	ret = send_remote_command_locked(true);
	if (ret >= 0) {
		(void)k_work_reschedule(&remote_refresh_work,
					K_MSEC(APP_IPC_REMOTE_REFRESH_MS));
	}
	k_mutex_unlock(&remote_command_lock);
	return ret;
}

static int remote_command_disable(void)
{
	int ret;

	k_mutex_lock(&remote_command_lock, K_FOREVER);
	remote_command.source = APP_MOTOR_COMMAND_SOURCE_SHELL;
	remote_command.timeout_ms = APP_IPC_REMOTE_TIMEOUT_MS;
	ret = send_remote_command_locked(false);
	if (ret >= 0) {
		remote_command.enabled = false;
		remote_command.control = 0U;
		(void)k_work_cancel_delayable(&remote_refresh_work);
	}
	k_mutex_unlock(&remote_command_lock);
	return ret;
}

static int remote_command_reset(void)
{
	int ret;

	k_mutex_lock(&remote_command_lock, K_FOREVER);
	remote_command.source = APP_MOTOR_COMMAND_SOURCE_SHELL;
	remote_command.timeout_ms = APP_IPC_REMOTE_TIMEOUT_MS;
	remote_command.control = APP_MOTOR_COMMAND_ENABLE |
		APP_MOTOR_COMMAND_RESET_FAULT;
	ret = send_remote_command_locked(remote_command.enabled);
	if (ret >= 0 && remote_command.enabled) {
		(void)k_work_reschedule(&remote_refresh_work,
					K_MSEC(APP_IPC_REMOTE_REFRESH_MS));
	}
	k_mutex_unlock(&remote_command_lock);
	return ret;
}

void app_ipc_get_status(struct app_ipc_status *status)
{
	if (status == NULL) {
		return;
	}

	status->initialized = atomic_get(&endpoint_initialized) != 0;
	status->bound = atomic_get(&endpoint_is_bound) != 0;
	status->last_error = (int)atomic_get(&last_error);
}

void app_ipc_get_motor_state(struct app_ipc_motor_state *state)
{
	k_spinlock_key_t key;
	uint32_t now;

	if (state == NULL) {
		return;
	}

	key = k_spin_lock(&motor_state_lock);
	*state = motor_state_cache;
	k_spin_unlock(&motor_state_lock, key);

	if (!state->valid) {
		state->stale = true;
		state->age_ms = 0U;
		return;
	}

	now = k_uptime_get_32();
	state->age_ms = now - state->age_ms;
	state->stale = state->age_ms > APP_IPC_MOTOR_STATE_STALE_MS;
}

static int cmd_m4_ping(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t round_trip_ms;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = app_ipc_ping(&round_trip_ms);
	if (ret < 0) {
		if (ret == -ETIMEDOUT) {
			shell_error(sh, "M4 ping timed out waiting for PONG");
		} else {
			shell_error(sh, "M4 ping request could not be sent: %d", ret);
		}
		return ret;
	}

	shell_print(sh, "M4 pong in %u ms", (unsigned int)round_trip_ms);
	return 0;
}

static int cmd_m4_status(const struct shell *sh, size_t argc, char **argv)
{
	struct app_ipc_status status;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_ipc_get_status(&status);
	shell_print(sh, "IPC initialized : %s", status.initialized ? "yes" : "no");
	shell_print(sh, "M4 endpoint    : %s", status.bound ? "bound" : "not bound");
	shell_print(sh, "Last error     : %d", status.last_error);
	return 0;
}

static int cmd_m4_state(const struct shell *sh, size_t argc, char **argv)
{
	struct app_ipc_motor_state state;
	uint16_t flags;
	const char *mode;
	const char *motor_state;
	const char *safety = "OK";

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	app_ipc_get_motor_state(&state);
	if (!state.valid) {
		shell_error(sh, "No motor state received from M4 yet");
		return -EAGAIN;
	}

	flags = state.words[APP_MOTOR_STATE_FLAGS];
	mode = (flags & APP_MOTOR_STATE_FLAG_REMOTE) != 0U ? "Remote" : "Local";
	if ((flags & APP_MOTOR_STATE_FLAG_FAULT) != 0U) {
		motor_state = "Fault";
	} else if ((flags & APP_MOTOR_STATE_FLAG_STOPPING) != 0U) {
		motor_state = "Stopping";
	} else if ((flags & APP_MOTOR_STATE_FLAG_RUNNING) != 0U) {
		motor_state = "Running";
	} else if ((flags & APP_MOTOR_STATE_FLAG_READY) != 0U) {
		motor_state = "Ready";
	} else {
		motor_state = "Disabled";
	}

	if ((flags & APP_MOTOR_STATE_FLAG_REMOTE_STOP_LATCH) != 0U) {
		safety = "Physical STOP latched";
	} else if ((flags & APP_MOTOR_STATE_FLAG_REMOTE_TIMEOUT) != 0U) {
		safety = "Remote command timeout";
	}

	shell_print(sh, "Control mode    : %s", mode);
	shell_print(sh, "Motor state    : %s", motor_state);
	shell_print(sh, "Applied command: %u %%",
		    state.words[APP_MOTOR_STATE_APPLIED_DUTY_PERMILLE] / 10U);
	shell_print(sh, "Target command : %u %%",
		    state.words[APP_MOTOR_STATE_TARGET_DUTY_PERMILLE] / 10U);
	shell_print(sh, "Direction      : %s",
		    state.words[APP_MOTOR_STATE_DIRECTION] == 0U ? "forward" : "reverse");
	shell_print(sh, "Fault          : %d",
		    (int16_t)state.words[APP_MOTOR_STATE_FAULT_CODE]);
	shell_print(sh, "Safety         : %s", safety);
	shell_print(sh, "Buttons        : 0x%04x",
		    state.words[APP_MOTOR_STATE_BUTTONS]);
	shell_print(sh, "Potentiometer  : %u / 4095",
		    state.words[APP_MOTOR_STATE_POTENTIOMETER_RAW]);
	shell_print(sh, "M7 -> M4 command age : %u ms",
		    state.words[APP_MOTOR_STATE_COMMAND_AGE_MS]);
	shell_print(sh, "Last accepted command : %u",
		    state.words[APP_MOTOR_STATE_LAST_ACCEPTED_SEQUENCE]);
	shell_print(sh, "M4 -> M7 cache age / heartbeat : %u ms / %u",
		    (unsigned int)state.age_ms,
		    state.words[APP_MOTOR_STATE_HEARTBEAT]);
	return 0;
}

static int report_remote_result(const struct shell *sh, int ret)
{
	if (ret < 0) {
		if (ret == -EACCES) {
			shell_error(sh, "Remote mode is disabled; run 'm4 remote' first");
			return ret;
		}

		shell_error(sh, "M4 command could not be queued: %d", ret);
		return ret;
	}

	shell_print(sh, "M4 command queued");
	return 0;
}

static int cmd_m4_remote(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return report_remote_result(sh, remote_command_update(true,
		APP_MOTOR_COMMAND_ENABLE,
							 APP_MOTOR_COMMAND_DUTY_MIN_PERMILLE));
}

static int cmd_m4_local(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return report_remote_result(sh, remote_command_disable());
}

static int cmd_m4_start(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return report_remote_result(sh, remote_command_update(false,
		APP_MOTOR_COMMAND_ENABLE | APP_MOTOR_COMMAND_RUN |
			(remote_command.control & APP_MOTOR_COMMAND_DIRECTION_REVERSE),
		remote_command.duty_permille));
}

static int cmd_m4_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return report_remote_result(sh, remote_command_update(false,
		APP_MOTOR_COMMAND_ENABLE |
			(remote_command.control & APP_MOTOR_COMMAND_DIRECTION_REVERSE),
		remote_command.duty_permille));
}

static int cmd_m4_quick_stop(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return report_remote_result(sh, remote_command_update(false,
		APP_MOTOR_COMMAND_ENABLE | APP_MOTOR_COMMAND_QUICK_STOP |
			(remote_command.control & APP_MOTOR_COMMAND_DIRECTION_REVERSE),
		remote_command.duty_permille));
}

static int require_motor_stopped(const struct shell *sh)
{
	struct app_ipc_motor_state state;

	app_ipc_get_motor_state(&state);
	if (!state.valid || state.stale) {
		shell_error(sh, "Motor state is unavailable or stale");
		return -EAGAIN;
	}

	if ((state.words[APP_MOTOR_STATE_FLAGS] &
	     (APP_MOTOR_STATE_FLAG_RUNNING | APP_MOTOR_STATE_FLAG_STOPPING)) != 0U) {
		shell_error(sh, "Stop the motor before changing direction");
		return -EBUSY;
	}

	return 0;
}

static int cmd_m4_reset(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	return report_remote_result(sh, remote_command_reset());
}

static int cmd_m4_forward(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ret = require_motor_stopped(sh);
	if (ret < 0) {
		return ret;
	}
	return report_remote_result(sh, remote_command_update(false,
		APP_MOTOR_COMMAND_ENABLE, remote_command.duty_permille));
}

static int cmd_m4_reverse(const struct shell *sh, size_t argc, char **argv)
{
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	ret = require_motor_stopped(sh);
	if (ret < 0) {
		return ret;
	}
	return report_remote_result(sh, remote_command_update(false,
		APP_MOTOR_COMMAND_ENABLE | APP_MOTOR_COMMAND_DIRECTION_REVERSE,
		remote_command.duty_permille));
}

static int cmd_m4_duty(const struct shell *sh, size_t argc, char **argv)
{
	char *end;
	unsigned long duty;

	if (argc != 2U) {
		return -EINVAL;
	}

	duty = strtoul(argv[1], &end, 10);
	if ((*end != '\0') || (duty < APP_MOTOR_COMMAND_DUTY_MIN_PERMILLE) ||
	    (duty > APP_MOTOR_COMMAND_DUTY_MAX_PERMILLE)) {
		shell_error(sh, "Duty must be between 800 and 1000 permille");
		return -EINVAL;
	}

	return report_remote_result(sh, remote_command_update(false,
		remote_command.control,
							 (uint16_t)duty));
}

static int cmd_m4_pause_refresh(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	k_mutex_lock(&remote_command_lock, K_FOREVER);
	remote_command.enabled = false;
	(void)k_work_cancel_delayable(&remote_refresh_work);
	k_mutex_unlock(&remote_command_lock);
	shell_print(sh, "Remote refresh paused; M4 timeout should stop the motor in 1 s");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(m4_cmds,
	SHELL_CMD(ping, NULL, "Ping the M4 over IPC", cmd_m4_ping),
	SHELL_CMD(status, NULL, "Show M7-to-M4 IPC status", cmd_m4_status),
	SHELL_CMD(state, NULL, "Show cached M4 motor state", cmd_m4_state),
	SHELL_CMD(remote, NULL, "Enable M4 remote mode", cmd_m4_remote),
	SHELL_CMD(local, NULL, "Return M4 to local control", cmd_m4_local),
	SHELL_CMD(start, NULL, "Start motor in remote mode", cmd_m4_start),
	SHELL_CMD(stop, NULL, "Stop motor with ramp in remote mode", cmd_m4_stop),
	SHELL_CMD(quickstop, NULL, "Stop motor immediately in remote mode", cmd_m4_quick_stop),
	SHELL_CMD(reset, NULL, "Clear remote STOP latch and reset motor", cmd_m4_reset),
	SHELL_CMD(forward, NULL, "Select forward direction while stopped", cmd_m4_forward),
	SHELL_CMD(reverse, NULL, "Select reverse direction while stopped", cmd_m4_reverse),
	SHELL_CMD_ARG(duty, NULL, "Set remote duty: 800..1000", cmd_m4_duty, 2, 0),
	SHELL_CMD(pause, NULL, "Pause remote refresh for timeout validation", cmd_m4_pause_refresh),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(m4, &m4_cmds, "M4 IPC diagnostics", NULL);
