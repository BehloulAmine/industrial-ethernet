/*
 * Modbus TCP server.
 *
 * The Zephyr Modbus server handles standard function codes through the
 * callbacks below. TCP framing is provided here through a RAW ADU backend.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/modbus/modbus.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/util.h>

#include "app_modbus_tcp.h"
#include "modbus_map.h"
#include "net_cfg.h"

LOG_MODULE_REGISTER(app_modbus_tcp, LOG_LEVEL_INF);

#define APP_MODBUS_TCP_PORT 502
#define APP_MODBUS_UNIT_ID 1
#define APP_MODBUS_THREAD_STACK_SIZE 4096
#define APP_MODBUS_THREAD_PRIORITY 7

#define APP_MODBUS_FC23_READ_WRITE_REGS 0x17
#define APP_MODBUS_TCP_BACKLOG 2
#define APP_MODBUS_RAW_TIMEOUT_MS 1000

static uint16_t holding_regs[APP_MODBUS_HOLDING_REG_COUNT] = {
	[0] = 0x0747,
	[1] = 1,
};

static struct modbus_adu tcp_adu;
static K_SEM_DEFINE(raw_response_ready, 0, 1);
static int server_iface = -1;
static bool server_started;
static uint16_t connection_count;
static uint16_t heartbeat_counter;
static int16_t last_status;

K_THREAD_STACK_DEFINE(modbus_tcp_stack, APP_MODBUS_THREAD_STACK_SIZE);
static struct k_thread modbus_tcp_thread;
static k_tid_t modbus_tcp_tid;

static uint16_t ipv4_word_msw(const char *ip)
{
	struct in_addr addr;
	uint32_t host_addr;

	if (net_addr_pton(AF_INET, ip, &addr) < 0) {
		return 0;
	}

	host_addr = ntohl(addr.s_addr);
	return (uint16_t)(host_addr >> 16);
}

static uint16_t ipv4_word_lsw(const char *ip)
{
	struct in_addr addr;
	uint32_t host_addr;

	if (net_addr_pton(AF_INET, ip, &addr) < 0) {
		return 0;
	}

	host_addr = ntohl(addr.s_addr);
	return (uint16_t)(host_addr & 0xffffU);
}

static void ipv4_words_to_string(uint16_t msw, uint16_t lsw, char *dst, size_t dst_len)
{
	uint32_t host_addr = ((uint32_t)msw << 16) | lsw;
	struct in_addr addr = {
		.s_addr = htonl(host_addr),
	};

	net_addr_ntop(AF_INET, &addr, dst, dst_len);
}

static void sync_holding_regs_from_saved_cfg(void)
{
	struct net_cfg_data saved;

	if (net_cfg_get_saved(&saved) < 0) {
		return;
	}

	holding_regs[APP_MB_HREG_SIGNATURE] = 0x0747;
	holding_regs[APP_MB_HREG_MODE] = (uint16_t)saved.mode;
	holding_regs[APP_MB_HREG_IP_MSW] = ipv4_word_msw(saved.ip);
	holding_regs[APP_MB_HREG_IP_LSW] = ipv4_word_lsw(saved.ip);
	holding_regs[APP_MB_HREG_MASK_MSW] = ipv4_word_msw(saved.mask);
	holding_regs[APP_MB_HREG_MASK_LSW] = ipv4_word_lsw(saved.mask);
	holding_regs[APP_MB_HREG_GW_MSW] = ipv4_word_msw(saved.gw);
	holding_regs[APP_MB_HREG_GW_LSW] = ipv4_word_lsw(saved.gw);
}

static int apply_holding_regs_command(uint16_t command)
{
	struct net_cfg_data saved = { 0 };
	int ret;

	if (command == APP_MB_CMD_NONE) {
		return 0;
	}

	if (holding_regs[APP_MB_HREG_MODE] > NET_CFG_STATIC) {
		return -EINVAL;
	}

	saved.mode = (enum net_cfg_mode)holding_regs[APP_MB_HREG_MODE];
	ipv4_words_to_string(holding_regs[APP_MB_HREG_IP_MSW],
			     holding_regs[APP_MB_HREG_IP_LSW],
			     saved.ip, sizeof(saved.ip));
	ipv4_words_to_string(holding_regs[APP_MB_HREG_MASK_MSW],
			     holding_regs[APP_MB_HREG_MASK_LSW],
			     saved.mask, sizeof(saved.mask));
	ipv4_words_to_string(holding_regs[APP_MB_HREG_GW_MSW],
			     holding_regs[APP_MB_HREG_GW_LSW],
			     saved.gw, sizeof(saved.gw));

	ret = net_cfg_set_saved(&saved);
	if (ret < 0) {
		return ret;
	}

	if (command == APP_MB_CMD_SAVE_APPLY) {
		ret = net_cfg_apply(NULL);
		if (ret < 0) {
			return ret;
		}
	}

	return 0;
}

static int input_reg_rd(uint16_t addr, uint16_t *reg)
{
	struct net_cfg_data active = { 0 };
	uint32_t uptime_s = (uint32_t)(k_uptime_get() / 1000);

	if (addr >= APP_MODBUS_INPUT_REG_COUNT) {
		return -ENOTSUP;
	}

	(void)net_cfg_get_active(&active);

	switch (addr) {
	case APP_MB_IREG_SIGNATURE:
		*reg = 0x0747;
		break;
	case APP_MB_IREG_MAP_VERSION:
		*reg = APP_MODBUS_MAP_VERSION;
		break;
	case APP_MB_IREG_UPTIME_LSW:
		*reg = (uint16_t)(uptime_s & 0xffffU);
		break;
	case APP_MB_IREG_UPTIME_MSW:
		*reg = (uint16_t)(uptime_s >> 16);
		break;
	case APP_MB_IREG_ACTIVE_MODE:
		*reg = (uint16_t)active.mode;
		break;
	case APP_MB_IREG_LINK_STATUS:
		*reg = net_cfg_link_is_up() ? 1U : 0U;
		break;
	case APP_MB_IREG_IP_MSW:
		*reg = ipv4_word_msw(active.ip);
		break;
	case APP_MB_IREG_IP_LSW:
		*reg = ipv4_word_lsw(active.ip);
		break;
	case APP_MB_IREG_MASK_MSW:
		*reg = ipv4_word_msw(active.mask);
		break;
	case APP_MB_IREG_MASK_LSW:
		*reg = ipv4_word_lsw(active.mask);
		break;
	case APP_MB_IREG_GW_MSW:
		*reg = ipv4_word_msw(active.gw);
		break;
	case APP_MB_IREG_GW_LSW:
		*reg = ipv4_word_lsw(active.gw);
		break;
	case APP_MB_IREG_HEARTBEAT_LSW:
		*reg = heartbeat_counter;
		break;
	case APP_MB_IREG_LAST_STATUS:
		*reg = (uint16_t)last_status;
		break;
	case APP_MB_IREG_CONN_COUNT:
		*reg = connection_count;
		break;
	default:
		*reg = 0;
		break;
	}

	return 0;
}

static int holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	if (addr >= APP_MODBUS_HOLDING_REG_COUNT) {
		return -ENOTSUP;
	}

	sync_holding_regs_from_saved_cfg();
	*reg = holding_regs[addr];
	return 0;
}

static int holding_reg_wr(uint16_t addr, uint16_t reg)
{
	int ret;

	if (addr >= APP_MODBUS_HOLDING_REG_COUNT) {
		return -ENOTSUP;
	}

	holding_regs[addr] = reg;

	if (addr == APP_MB_HREG_COMMAND) {
		ret = apply_holding_regs_command(reg);
		last_status = (int16_t)ret;
		holding_regs[APP_MB_HREG_STATUS] = (uint16_t)ret;
		holding_regs[APP_MB_HREG_COMMAND] = APP_MB_CMD_NONE;

		if (ret == 0) {
			sync_holding_regs_from_saved_cfg();
		}
	}

	return 0;
}

static bool fc23_read_write_holding_regs(const int iface,
					 const struct modbus_adu *const rx_adu,
					 struct modbus_adu *const tx_adu,
					 uint8_t *const excep_code,
					 void *const user_data)
{
	uint16_t read_addr;
	uint16_t read_qty;
	uint16_t write_addr;
	uint16_t write_qty;
	uint8_t write_bytes;
	uint8_t response_bytes;

	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	if (rx_adu->length < 9) {
		*excep_code = MODBUS_EXC_ILLEGAL_DATA_VAL;
		return true;
	}

	read_addr = sys_get_be16(&rx_adu->data[0]);
	read_qty = sys_get_be16(&rx_adu->data[2]);
	write_addr = sys_get_be16(&rx_adu->data[4]);
	write_qty = sys_get_be16(&rx_adu->data[6]);
	write_bytes = rx_adu->data[8];

	if (read_qty == 0 || read_qty > 125 ||
	    write_qty == 0 || write_qty > 121 ||
	    write_bytes != write_qty * sizeof(uint16_t) ||
	    rx_adu->length != 9 + write_bytes) {
		*excep_code = MODBUS_EXC_ILLEGAL_DATA_VAL;
		return true;
	}

	for (uint16_t i = 0; i < write_qty; i++) {
		uint16_t reg = sys_get_be16(&rx_adu->data[9 + (i * sizeof(uint16_t))]);

		if (holding_reg_wr(write_addr + i, reg) != 0) {
			*excep_code = MODBUS_EXC_ILLEGAL_DATA_ADDR;
			return true;
		}
	}

	response_bytes = read_qty * sizeof(uint16_t);
	tx_adu->data[0] = response_bytes;
	tx_adu->length = response_bytes + 1;

	for (uint16_t i = 0; i < read_qty; i++) {
		uint16_t reg;

		if (holding_reg_rd(read_addr + i, &reg) != 0) {
			*excep_code = MODBUS_EXC_ILLEGAL_DATA_ADDR;
			return true;
		}

		sys_put_be16(reg, &tx_adu->data[1 + (i * sizeof(uint16_t))]);
	}

	return true;
}

MODBUS_CUSTOM_FC_DEFINE(fc23, fc23_read_write_holding_regs,
			APP_MODBUS_FC23_READ_WRITE_REGS, NULL);

static struct modbus_user_callbacks server_callbacks = {
	.input_reg_rd = input_reg_rd,
	.holding_reg_rd = holding_reg_rd,
	.holding_reg_wr = holding_reg_wr,
};

static int raw_tx_cb(const int iface, const struct modbus_adu *adu, void *user_data)
{
	ARG_UNUSED(iface);
	ARG_UNUSED(user_data);

	tcp_adu.trans_id = adu->trans_id;
	tcp_adu.proto_id = adu->proto_id;
	tcp_adu.length = adu->length;
	tcp_adu.unit_id = adu->unit_id;
	tcp_adu.fc = adu->fc;
	memcpy(tcp_adu.data, adu->data, MIN(adu->length, sizeof(tcp_adu.data)));

	k_sem_give(&raw_response_ready);
	return 0;
}

static const struct modbus_iface_param server_param = {
	.mode = MODBUS_MODE_RAW,
	.server = {
		.user_cb = &server_callbacks,
		.unit_id = APP_MODBUS_UNIT_ID,
	},
	.rawcb = {
		.raw_tx_cb = raw_tx_cb,
		.user_data = NULL,
	},
};

static int init_modbus_server(void)
{
	int ret;

	sync_holding_regs_from_saved_cfg();
	holding_regs[APP_MB_HREG_STATUS] = 0;

	server_iface = modbus_iface_get_by_name("RAW_0");
	if (server_iface < 0) {
		LOG_ERR("Failed to get RAW_0 Modbus interface: %d", server_iface);
		return server_iface;
	}

	ret = modbus_init_server(server_iface, server_param);
	if (ret < 0) {
		LOG_ERR("modbus_init_server failed: %d", ret);
		return ret;
	}

	ret = modbus_register_user_fc(server_iface, &modbus_cfg_fc23);
	if (ret < 0) {
		LOG_ERR("Failed to register FC23 handler: %d", ret);
		return ret;
	}

	LOG_INF("Modbus server ready: unit=%d, holding=%d, input=%d",
		APP_MODBUS_UNIT_ID, APP_MODBUS_HOLDING_REG_COUNT,
		APP_MODBUS_INPUT_REG_COUNT);
	return 0;
}

static int send_all(int client, const void *buf, size_t len)
{
	const uint8_t *cursor = buf;

	while (len > 0) {
		int ret = zsock_send(client, cursor, len, 0);

		if (ret < 0) {
			return -errno;
		}
		if (ret == 0) {
			return -ENOTCONN;
		}

		cursor += ret;
		len -= ret;
	}

	return 0;
}

static int send_modbus_reply(int client, const struct modbus_adu *adu)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];
	int ret;

	modbus_raw_put_header(adu, header);

	ret = send_all(client, header, sizeof(header));
	if (ret < 0) {
		return ret;
	}

	ret = send_all(client, adu->data, adu->length);
	if (ret < 0) {
		return ret;
	}

	return 0;
}

static int handle_modbus_connection(int client)
{
	uint8_t header[MODBUS_MBAP_AND_FC_LENGTH];
	int ret;

	ret = zsock_recv(client, header, sizeof(header), ZSOCK_MSG_WAITALL);
	if (ret <= 0) {
		return ret == 0 ? -ENOTCONN : -errno;
	}

	modbus_raw_get_header(&tcp_adu, header);
	if (tcp_adu.length > sizeof(tcp_adu.data)) {
		return -EMSGSIZE;
	}

	ret = zsock_recv(client, tcp_adu.data, tcp_adu.length, ZSOCK_MSG_WAITALL);
	if (ret <= 0) {
		return ret == 0 ? -ENOTCONN : -errno;
	}

	k_sem_reset(&raw_response_ready);
	ret = modbus_raw_submit_rx(server_iface, &tcp_adu);
	if (ret < 0) {
		LOG_ERR("modbus_raw_submit_rx failed: %d", ret);
		return ret;
	}

	if (k_sem_take(&raw_response_ready, K_MSEC(APP_MODBUS_RAW_TIMEOUT_MS)) != 0) {
		LOG_ERR("Modbus response timeout");
		modbus_raw_set_server_failure(&tcp_adu);
	}

	return send_modbus_reply(client, &tcp_adu);
}

static void modbus_tcp_thread_fn(void *arg1, void *arg2, void *arg3)
{
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(APP_MODBUS_TCP_PORT),
	};
	int server;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		LOG_ERR("socket failed: %d", errno);
		return;
	}

	if (zsock_bind(server, (struct net_sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("bind port %d failed: %d", APP_MODBUS_TCP_PORT, errno);
		(void)zsock_close(server);
		return;
	}

	if (zsock_listen(server, APP_MODBUS_TCP_BACKLOG) < 0) {
		LOG_ERR("listen failed: %d", errno);
		(void)zsock_close(server);
		return;
	}

	LOG_INF("Modbus TCP listening on port %d", APP_MODBUS_TCP_PORT);

	while (true) {
		struct sockaddr_in client_addr;
		net_socklen_t client_addr_len = sizeof(client_addr);
		int client;
		int ret;

		client = zsock_accept(server, (struct net_sockaddr *)&client_addr,
				      &client_addr_len);
		if (client < 0) {
			LOG_ERR("accept failed: %d", errno);
			continue;
		}

		connection_count++;
		do {
			ret = handle_modbus_connection(client);
		} while (ret == 0);

		(void)zsock_close(client);
	}
}

int app_modbus_tcp_start(void)
{
	int ret;

	if (server_started) {
		return 0;
	}

	ret = init_modbus_server();
	if (ret < 0) {
		return ret;
	}

	modbus_tcp_tid = k_thread_create(&modbus_tcp_thread, modbus_tcp_stack,
					 K_THREAD_STACK_SIZEOF(modbus_tcp_stack),
					 modbus_tcp_thread_fn,
					 NULL, NULL, NULL,
					 APP_MODBUS_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(modbus_tcp_tid, "modbus_tcp");

	server_started = true;
	return 0;
}

void app_modbus_tcp_heartbeat_tick(void)
{
	heartbeat_counter++;
}
