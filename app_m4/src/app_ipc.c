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

BUILD_ASSERT(sizeof(struct app_ipc_frame) == 16U,
	     "Unexpected IPC frame size");

static const struct device *const ipc_instance = DEVICE_DT_GET(DT_NODELABEL(ipc0));
static struct ipc_ept ipc_endpoint;
static atomic_t endpoint_bound;
static atomic_t endpoint_initialized;
static atomic_t pending_ping;
static atomic_t pending_sequence;

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

	if (!frame_is_valid(frame, len, APP_IPC_FRAME_PING)) {
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
