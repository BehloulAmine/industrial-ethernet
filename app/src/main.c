/*
 * Phase 1: DHCP/static IPv4, ping support and LED heartbeat.
 * Target: STM32H747I-DISCO Cortex-M7.
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/settings/settings.h>
#include <zephyr/logging/log.h>

#include "app_modbus_tcp.h"
#include "net_cfg.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static struct net_mgmt_event_callback mgmt_cb;

static void net_event_handler(struct net_mgmt_event_callback *cb,
			      uint64_t mgmt_event, struct net_if *iface)
{
	struct net_if_config *cfg;
	struct net_if_addr *dhcp_addr = NULL;

	ARG_UNUSED(cb);

	if (mgmt_event != NET_EVENT_IPV4_DHCP_BOUND) {
		return;
	}

	cfg = net_if_get_config(iface);
	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *ifaddr = &cfg->ip.ipv4->unicast[i].ipv4;

		if (ifaddr->is_used && ifaddr->addr_type == NET_ADDR_DHCP) {
			dhcp_addr = ifaddr;
			break;
		}
	}

	if (dhcp_addr) {
		char addr_str[NET_IPV4_ADDR_LEN];

		net_addr_ntop(AF_INET, &dhcp_addr->address.in_addr,
			      addr_str, sizeof(addr_str));
		LOG_INF("DHCP bound: %s", addr_str);
	}
}

int main(void)
{
	struct net_if *iface;
	int ret;

	LOG_INF("=== H747 Demo Phase 1 ===");

	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
		return -1;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

	net_mgmt_init_event_callback(&mgmt_cb, net_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&mgmt_cb);

	ret = settings_subsys_init();
	if (ret < 0) {
		LOG_ERR("settings_subsys_init failed: %d", ret);
	} else {
		ret = net_cfg_load();
		if (ret < 0) {
			LOG_ERR("net_cfg_load failed: %d", ret);
		}
	}

	iface = net_if_get_default();
	if (!iface) {
		LOG_ERR("No network interface found");
		return -1;
	}

	ret = net_cfg_apply(iface);
	if (ret < 0) {
		LOG_ERR("net_cfg_apply failed: %d", ret);
	}

	ret = app_modbus_tcp_start();
	if (ret < 0) {
		LOG_ERR("app_modbus_tcp_start failed: %d", ret);
	}

	while (1) {
		gpio_pin_toggle_dt(&led);
		app_modbus_tcp_heartbeat_tick();
		k_msleep(500);
	}

	return 0;
}
