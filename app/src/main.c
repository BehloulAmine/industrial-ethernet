/*
 * Phase 1 — DHCP + Ping + LED heartbeat
 * STM32H747I-DISCO (Cortex-M7)
 */

#include <zephyr/kernel.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/net_event.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* LED0 — LD1 green on STM32H747I-DISCO */
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Network management callback */
static struct net_mgmt_event_callback mgmt_cb;

static void net_event_handler(struct net_mgmt_event_callback *cb,
			      uint32_t mgmt_event, struct net_if *iface)
{
	if (mgmt_event == NET_EVENT_IPV4_DHCP_BOUND) {
		struct net_if_config *cfg = net_if_get_config(iface);
		struct net_if_addr *unicast = NULL;

		for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
			if (cfg->ip.ipv4->unicast[i].ipv4.is_used) {
				unicast = &cfg->ip.ipv4->unicast[i].ipv4;
				break;
			}
		}

		if (unicast) {
			LOG_INF("DHCP bound: %s",
				net_sprint_ipv4_addr(&unicast->address.in_addr));
		}
	}
}

int main(void)
{
	LOG_INF("=== H747 Demo Phase 1 ===");

	/* Setup LED */
	if (!gpio_is_ready_dt(&led)) {
		LOG_ERR("LED device not ready");
		return -1;
	}
	gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE);

	/* Register for DHCP events */
	net_mgmt_init_event_callback(&mgmt_cb, net_event_handler,
				     NET_EVENT_IPV4_DHCP_BOUND);
	net_mgmt_add_event_callback(&mgmt_cb);

	/* Start DHCP on the default interface */
	struct net_if *iface = net_if_get_default();

	if (iface) {
		net_dhcpv4_start(iface);
		LOG_INF("DHCP client started, waiting for lease...");
	} else {
		LOG_ERR("No network interface found");
	}

	/* Heartbeat loop — blink LED to show firmware is alive */
	while (1) {
		gpio_pin_toggle_dt(&led);
		k_msleep(500);
	}

	return 0;
}
