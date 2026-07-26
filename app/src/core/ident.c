/* Board and firmware identification shared by application protocols. */

#include <errno.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/shell/shell.h>

#include "ident.h"
#include "net_cfg.h"

#define APP_IDENT_HW_ID_LEN 12

static void ident_format_hardware_id(char *dst, size_t dst_size)
{
	uint8_t device_id[APP_IDENT_HW_ID_LEN];
	ssize_t len;
	size_t offset = 0U;

	len = hwinfo_get_device_id(device_id, sizeof(device_id));
	if (len <= 0) {
		snprintk(dst, dst_size, "unavailable");
		return;
	}

	for (ssize_t i = 0; i < len && offset + 2U < dst_size; i++) {
		int ret = snprintk(&dst[offset], dst_size - offset, "%02x", device_id[i]);

		if (ret != 2) {
			break;
		}
		offset += 2U;
	}
	dst[offset] = '\0';
}

static bool ident_format_mac(struct net_if *iface, char *dst, size_t dst_size)
{
	const struct net_linkaddr *link_addr = net_if_get_link_addr(iface);

	if (link_addr == NULL || link_addr->len < 6U) {
		snprintk(dst, dst_size, "unavailable");
		return false;
	}

	snprintk(dst, dst_size, "%02x:%02x:%02x:%02x:%02x:%02x",
		 link_addr->addr[0], link_addr->addr[1], link_addr->addr[2],
		 link_addr->addr[3], link_addr->addr[4], link_addr->addr[5]);
	return true;
}

static void ident_format_uuid(struct net_if *iface, char *dst, size_t dst_size)
{
	const struct net_linkaddr *link_addr = net_if_get_link_addr(iface);

	if (link_addr == NULL || link_addr->len < 6U) {
		snprintk(dst, dst_size, "unavailable");
		return;
	}

	snprintk(dst, dst_size,
		 "00000000-0000-4000-8000-%02x%02x%02x%02x%02x%02x",
		 link_addr->addr[0], link_addr->addr[1], link_addr->addr[2],
		 link_addr->addr[3], link_addr->addr[4], link_addr->addr[5]);
}

static void ident_format_ipv4(char *dst, size_t dst_size)
{
	struct net_cfg_data active;

	if (net_cfg_get_active(&active) < 0) {
		snprintk(dst, dst_size, "unavailable");
		return;
	}

	snprintk(dst, dst_size, "%s", active.ip);
}

static bool ident_format_ipv6(struct net_if *iface, char *dst, size_t dst_size,
			      bool *preferred)
{
	struct net_in6_addr *address;

	address = net_if_ipv6_get_ll(iface, NET_ADDR_PREFERRED);
	*preferred = address != NULL;
	if (address == NULL) {
		address = net_if_ipv6_get_ll(iface, NET_ADDR_ANY_STATE);
	}
	if (address == NULL || net_addr_ntop(AF_INET6, address, dst, dst_size) == NULL) {
		snprintk(dst, dst_size, "unavailable");
		return false;
	}

	return true;
}

int app_ident_get(struct app_ident_info *info)
{
	struct net_if *iface;

	if (info == NULL) {
		return -EINVAL;
	}

	memset(info, 0, sizeof(*info));
	info->device_name = APP_IDENT_DEVICE_NAME;
	info->manufacturer = APP_IDENT_MANUFACTURER;
	info->model = APP_IDENT_MODEL;
	snprintk(info->firmware_version, sizeof(info->firmware_version), "%u.%u.%u",
		 APP_IDENT_FW_VERSION_MAJOR, APP_IDENT_FW_VERSION_MINOR,
		 APP_IDENT_FW_VERSION_PATCH);
	ident_format_hardware_id(info->hardware_id, sizeof(info->hardware_id));

	iface = net_if_get_default();
	if (iface == NULL) {
		snprintk(info->mac, sizeof(info->mac), "unavailable");
		snprintk(info->ipv4, sizeof(info->ipv4), "unavailable");
		snprintk(info->ipv6_link_local, sizeof(info->ipv6_link_local), "unavailable");
		snprintk(info->uuid, sizeof(info->uuid), "unavailable");
		return -ENODEV;
	}

	(void)ident_format_mac(iface, info->mac, sizeof(info->mac));
	ident_format_uuid(iface, info->uuid, sizeof(info->uuid));
	ident_format_ipv4(info->ipv4, sizeof(info->ipv4));
	(void)ident_format_ipv6(iface, info->ipv6_link_local,
				sizeof(info->ipv6_link_local), &info->ipv6_preferred);
	return 0;
}

static int cmd_ident(const struct shell *sh, size_t argc, char **argv)
{
	struct app_ident_info info;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)app_ident_get(&info);
	shell_print(sh, "Device name     : %s", info.device_name);
	shell_print(sh, "Manufacturer    : %s", info.manufacturer);
	shell_print(sh, "Model           : %s", info.model);
	shell_print(sh, "Firmware version: %s", info.firmware_version);
	shell_print(sh, "Hardware ID     : %s", info.hardware_id);
	shell_print(sh, "MAC address     : %s", info.mac);
	shell_print(sh, "IPv4 address    : %s", info.ipv4);
	shell_print(sh, "IPv6 link-local : %s%s", info.ipv6_link_local,
		    info.ipv6_preferred ? "" : " (tentative/unavailable)");
	shell_print(sh, "Device UUID     : %s", info.uuid);
	return 0;
}

SHELL_CMD_REGISTER(ident, NULL, "Show board and firmware identity", cmd_ident);
