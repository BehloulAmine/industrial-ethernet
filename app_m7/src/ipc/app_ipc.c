#include "app_ipc.h"

#include <errno.h>
#include <stddef.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app_ipc_protocol.h"

#define APP_IPC_PING_TIMEOUT_MS 250

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

static void frame_init(struct app_ipc_frame *frame, enum app_ipc_frame_type type,
		       uint32_t sequence, uint32_t value)
{
	frame->magic_le = sys_cpu_to_le32(APP_IPC_FRAME_MAGIC);
	frame->version_le = sys_cpu_to_le16(APP_IPC_FRAME_VERSION);
	frame->type_le = sys_cpu_to_le16((uint16_t)type);
	frame->sequence_le = sys_cpu_to_le32(sequence);
	frame->value_le = sys_cpu_to_le32(value);
}

static bool frame_is_valid(const struct app_ipc_frame *frame, size_t len,
			   enum app_ipc_frame_type type)
{
	return (len == sizeof(*frame)) &&
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

	ARG_UNUSED(priv);

	if (!frame_is_valid(frame, len, APP_IPC_FRAME_PONG)) {
		atomic_set(&last_error, -EBADMSG);
		return;
	}

	atomic_set(&received_sequence, (atomic_val_t)sys_le32_to_cpu(frame->sequence_le));
	k_sem_give(&pong_sem);
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
	frame_init(&frame, APP_IPC_FRAME_PING, sequence, start_ms);

	ret = ipc_service_send(&ipc_endpoint, &frame, sizeof(frame));
	if (ret < 0) {
		atomic_set(&last_error, ret);
		k_mutex_unlock(&ping_lock);
		return ret;
	}

	ret = k_sem_take(&pong_sem, K_MSEC(APP_IPC_PING_TIMEOUT_MS));
	if (ret < 0) {
		atomic_set(&last_error, ret);
		k_mutex_unlock(&ping_lock);
		return ret;
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

void app_ipc_get_status(struct app_ipc_status *status)
{
	if (status == NULL) {
		return;
	}

	status->initialized = atomic_get(&endpoint_initialized) != 0;
	status->bound = atomic_get(&endpoint_is_bound) != 0;
	status->last_error = (int)atomic_get(&last_error);
}

static int cmd_m4_ping(const struct shell *sh, size_t argc, char **argv)
{
	uint32_t round_trip_ms;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	ret = app_ipc_ping(&round_trip_ms);
	if (ret < 0) {
		shell_error(sh, "M4 ping failed: %d", ret);
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

SHELL_STATIC_SUBCMD_SET_CREATE(m4_cmds,
	SHELL_CMD(ping, NULL, "Ping the M4 over IPC", cmd_m4_ping),
	SHELL_CMD(status, NULL, "Show M7-to-M4 IPC status", cmd_m4_status),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(m4, &m4_cmds, "M4 IPC diagnostics", NULL);
