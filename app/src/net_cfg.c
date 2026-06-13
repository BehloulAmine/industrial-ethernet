/*
 * Network configuration: DHCP/static IPv4, shell commands and settings storage.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/settings/settings.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/dhcpv4.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "net_cfg.h"

LOG_MODULE_REGISTER(net_cfg, LOG_LEVEL_INF);

#define NET_CFG_DEFAULT_IP   "192.168.0.3"
#define NET_CFG_DEFAULT_MASK "255.255.255.0"
#define NET_CFG_DEFAULT_GW   "192.168.0.1"
#define NET_CFG_STORE_KEY    "net_cfg/blob"
#define NET_CFG_STORE_MAGIC  0x4e455443U
#define NET_CFG_STORE_VER    1U

struct net_cfg_store {
	uint32_t magic;
	uint8_t version;
	uint8_t mode;
	char ip[16];
	char mask[16];
	char gw[16];
};

static struct net_cfg_data cfg = {
	.mode = NET_CFG_DHCP,
	.ip = NET_CFG_DEFAULT_IP,
	.mask = NET_CFG_DEFAULT_MASK,
	.gw = NET_CFG_DEFAULT_GW,
};

static K_MUTEX_DEFINE(cfg_lock);
static net_cfg_change_cb_t change_cb;

static void net_cfg_notify_changed(void)
{
	if (change_cb) {
		change_cb();
	}
}

static void net_cfg_restore_defaults(void)
{
	cfg.mode = NET_CFG_DHCP;
	snprintk(cfg.ip, sizeof(cfg.ip), "%s", NET_CFG_DEFAULT_IP);
	snprintk(cfg.mask, sizeof(cfg.mask), "%s", NET_CFG_DEFAULT_MASK);
	snprintk(cfg.gw, sizeof(cfg.gw), "%s", NET_CFG_DEFAULT_GW);
}

static bool net_cfg_is_ipv4_string(const char *value)
{
	struct in_addr tmp;

	return net_addr_pton(AF_INET, value, &tmp) == 0;
}

static bool net_cfg_store_is_valid(const struct net_cfg_store *store)
{
	return store->magic == NET_CFG_STORE_MAGIC &&
	       store->version == NET_CFG_STORE_VER &&
	       net_cfg_is_ipv4_string(store->ip) &&
	       net_cfg_is_ipv4_string(store->mask) &&
	       net_cfg_is_ipv4_string(store->gw);
}

static int net_cfg_save(const struct net_cfg_data *data)
{
	struct net_cfg_store store = {
		.magic = NET_CFG_STORE_MAGIC,
		.version = NET_CFG_STORE_VER,
		.mode = data->mode,
	};

	snprintk(store.ip, sizeof(store.ip), "%s", data->ip);
	snprintk(store.mask, sizeof(store.mask), "%s", data->mask);
	snprintk(store.gw, sizeof(store.gw), "%s", data->gw);

	return settings_save_one(NET_CFG_STORE_KEY, &store, sizeof(store));
}

static void net_cfg_copy(struct net_cfg_data *dst, const struct net_cfg_data *src)
{
	dst->mode = src->mode;
	snprintk(dst->ip, sizeof(dst->ip), "%s", src->ip);
	snprintk(dst->mask, sizeof(dst->mask), "%s", src->mask);
	snprintk(dst->gw, sizeof(dst->gw), "%s", src->gw);
}

static int net_cfg_validate(const struct net_cfg_data *data)
{
	if (!data) {
		return -EINVAL;
	}

	if (data->mode != NET_CFG_DHCP && data->mode != NET_CFG_STATIC) {
		return -EINVAL;
	}

	if (!net_cfg_is_ipv4_string(data->ip) ||
	    !net_cfg_is_ipv4_string(data->mask) ||
	    !net_cfg_is_ipv4_string(data->gw)) {
		return -EINVAL;
	}

	return 0;
}

static int net_cfg_load_string(const char *name, char *dst, size_t dst_size)
{
	ssize_t len;

	len = settings_load_one(name, dst, dst_size - 1);
	if (len == -ENOENT || len == 0) {
		return 0;
	}
	if (len < 0) {
		return (int)len;
	}
	if ((size_t)len >= dst_size) {
		return -EINVAL;
	}

	dst[len] = '\0';
	return 0;
}

int net_cfg_load(void)
{
	struct net_cfg_store store;
	uint8_t mode;
	ssize_t len;
	int ret;

	len = settings_load_one(NET_CFG_STORE_KEY, &store, sizeof(store));
	if (len == sizeof(store) && net_cfg_store_is_valid(&store)) {
		cfg.mode = (store.mode == NET_CFG_STATIC) ? NET_CFG_STATIC : NET_CFG_DHCP;
		snprintk(cfg.ip, sizeof(cfg.ip), "%s", store.ip);
		snprintk(cfg.mask, sizeof(cfg.mask), "%s", store.mask);
		snprintk(cfg.gw, sizeof(cfg.gw), "%s", store.gw);
		goto loaded;
	}

	if (len >= 0) {
		LOG_ERR("Invalid persisted config blob, restoring defaults");
		net_cfg_restore_defaults();

		ret = net_cfg_save(&cfg);
		if (ret < 0) {
			LOG_ERR("Failed to save default config: %d", ret);
			return ret;
		}

		goto loaded;
	}

	if (len != -ENOENT) {
		return (int)len;
	}

	/* One-time migration path for older builds that used separate keys. */
	len = settings_load_one("net_cfg/mode", &mode, sizeof(mode));
	if (len == sizeof(mode)) {
		cfg.mode = (mode == NET_CFG_STATIC) ? NET_CFG_STATIC : NET_CFG_DHCP;
	} else if (len < 0 && len != -ENOENT) {
		return (int)len;
	}

	ret = net_cfg_load_string("net_cfg/ip", cfg.ip, sizeof(cfg.ip));
	if (ret < 0) {
		return ret;
	}

	ret = net_cfg_load_string("net_cfg/mask", cfg.mask, sizeof(cfg.mask));
	if (ret < 0) {
		return ret;
	}

	ret = net_cfg_load_string("net_cfg/gw", cfg.gw, sizeof(cfg.gw));
	if (ret < 0) {
		return ret;
	}

	if (!net_cfg_is_ipv4_string(cfg.ip) ||
	    !net_cfg_is_ipv4_string(cfg.mask) ||
	    !net_cfg_is_ipv4_string(cfg.gw)) {
		LOG_ERR("Invalid persisted config, restoring defaults");
		net_cfg_restore_defaults();
	}

	ret = net_cfg_save(&cfg);
	if (ret < 0) {
		LOG_ERR("Failed to migrate config: %d", ret);
		return ret;
	}

	LOG_INF("Config migrated to blob storage");

loaded:
	LOG_INF("Loaded config: mode=%s ip=%s mask=%s gw=%s",
		cfg.mode == NET_CFG_STATIC ? "static" : "dhcp",
		cfg.ip, cfg.mask, cfg.gw);

	return 0;
}

int net_cfg_start(struct net_if *iface)
{
	struct net_cfg_data active_cfg;

	net_cfg_get_saved(&active_cfg);

	if (!iface) {
		iface = net_if_get_default();
	}
	if (!iface) {
		LOG_ERR("No network interface");
		return -ENODEV;
	}

	if (active_cfg.mode == NET_CFG_STATIC) {
		struct in_addr addr, mask, gw;
		struct net_if_addr *ifaddr;

		if (net_addr_pton(AF_INET, active_cfg.ip, &addr) < 0) {
			LOG_ERR("Invalid IP: %s", active_cfg.ip);
			return -EINVAL;
		}
		if (net_addr_pton(AF_INET, active_cfg.mask, &mask) < 0) {
			LOG_ERR("Invalid mask: %s", active_cfg.mask);
			return -EINVAL;
		}
		if (net_addr_pton(AF_INET, active_cfg.gw, &gw) < 0) {
			LOG_ERR("Invalid gateway: %s", active_cfg.gw);
			return -EINVAL;
		}

		ifaddr = net_if_ipv4_addr_add(iface, &addr, NET_ADDR_MANUAL, 0);
		if (!ifaddr) {
			LOG_ERR("Failed to add static IP address: %s", active_cfg.ip);
			return -ENOSPC;
		}

		if (!net_if_ipv4_set_netmask_by_addr(iface, &addr, &mask)) {
			LOG_ERR("Failed to set netmask: %s", active_cfg.mask);
			return -EIO;
		}
		net_if_ipv4_set_gw(iface, &gw);

		LOG_INF("Static IP configured: %s / %s gw %s",
			active_cfg.ip, active_cfg.mask, active_cfg.gw);
	} else {
		net_dhcpv4_start(iface);
		LOG_INF("DHCP client started, waiting for lease...");
	}

	return 0;
}

int net_cfg_get_saved(struct net_cfg_data *out)
{
	if (!out) {
		return -EINVAL;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	net_cfg_copy(out, &cfg);
	k_mutex_unlock(&cfg_lock);
	return 0;
}

int net_cfg_set_saved(const struct net_cfg_data *in)
{
	int ret;

	ret = net_cfg_validate(in);
	if (ret < 0) {
		return ret;
	}

	ret = net_cfg_save(in);
	if (ret < 0) {
		return ret;
	}

	k_mutex_lock(&cfg_lock, K_FOREVER);
	net_cfg_copy(&cfg, in);
	k_mutex_unlock(&cfg_lock);

	net_cfg_notify_changed();
	return 0;
}

void net_cfg_register_change_cb(net_cfg_change_cb_t cb)
{
	change_cb = cb;
}

int net_cfg_get_active(struct net_cfg_data *out)
{
	struct net_if *iface;
	struct net_if_config *if_cfg;
	struct net_in_addr netmask;
	struct net_in_addr gw;
	struct net_if_addr *selected = NULL;
	enum net_cfg_mode mode;

	if (!out) {
		return -EINVAL;
	}

	net_cfg_get_saved(out);
	mode = out->mode;

	iface = net_if_get_default();
	if (!iface) {
		return -ENODEV;
	}

	if_cfg = net_if_get_config(iface);
	if (!if_cfg || !if_cfg->ip.ipv4) {
		return -ENODATA;
	}

	for (int i = 0; i < NET_IF_MAX_IPV4_ADDR; i++) {
		struct net_if_addr *ifaddr = &if_cfg->ip.ipv4->unicast[i].ipv4;

		if (!ifaddr->is_used) {
			continue;
		}

		if (ifaddr->addr_type == NET_ADDR_DHCP) {
			selected = ifaddr;
			mode = NET_CFG_DHCP;
			break;
		}

		if (!selected && ifaddr->addr_type == NET_ADDR_MANUAL) {
			selected = ifaddr;
			mode = NET_CFG_STATIC;
		}
	}

	out->mode = mode;

	if (!selected) {
		snprintk(out->ip, sizeof(out->ip), "%s", "0.0.0.0");
		snprintk(out->mask, sizeof(out->mask), "%s", "0.0.0.0");
		snprintk(out->gw, sizeof(out->gw), "%s", "0.0.0.0");
		return 0;
	}

	net_addr_ntop(AF_INET, &selected->address.in_addr, out->ip, sizeof(out->ip));
	netmask = net_if_ipv4_get_netmask_by_addr(iface, &selected->address.in_addr);
	gw = net_if_ipv4_get_gw(iface);
	net_addr_ntop(AF_INET, &netmask, out->mask, sizeof(out->mask));
	net_addr_ntop(AF_INET, &gw, out->gw, sizeof(out->gw));

	return 0;
}

bool net_cfg_link_is_up(void)
{
	struct net_if *iface = net_if_get_default();

	if (!iface) {
		return false;
	}

	return net_if_is_up(iface) && net_if_is_carrier_ok(iface);
}

static int cmd_net_cfg_show(const struct shell *sh, size_t argc, char **argv)
{
	struct net_cfg_data saved;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	net_cfg_get_saved(&saved);

	shell_print(sh, "Mode    : %s", saved.mode == NET_CFG_DHCP ? "dhcp" : "static");
	shell_print(sh, "IP      : %s", saved.ip);
	shell_print(sh, "Netmask : %s", saved.mask);
	shell_print(sh, "Gateway : %s", saved.gw);
	return 0;
}

static int cmd_net_cfg_dhcp(const struct shell *sh, size_t argc, char **argv)
{
	struct net_cfg_data saved;
	int ret;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	net_cfg_get_saved(&saved);
	saved.mode = NET_CFG_DHCP;

	ret = net_cfg_set_saved(&saved);
	if (ret < 0) {
		shell_error(sh, "Failed to save config: %d", ret);
		return ret;
	}

	shell_print(sh, "Mode set to DHCP. Reboot to activate.");
	return 0;
}

static int cmd_net_cfg_static(const struct shell *sh, size_t argc, char **argv)
{
	struct net_cfg_data saved;
	struct in_addr tmp;
	int ret;

	if (argc < 2) {
		shell_print(sh, "Usage: ip static <ip> [mask] [gateway]");
		shell_print(sh, "  e.g: ip static 192.168.1.3 255.255.255.0 192.168.1.1");
		return -EINVAL;
	}

	if (net_addr_pton(AF_INET, argv[1], &tmp) < 0) {
		shell_error(sh, "Invalid IP address: %s", argv[1]);
		return -EINVAL;
	}

	net_cfg_get_saved(&saved);
	snprintk(saved.ip, sizeof(saved.ip), "%s", argv[1]);

	if (argc >= 3) {
		if (net_addr_pton(AF_INET, argv[2], &tmp) < 0) {
			shell_error(sh, "Invalid netmask: %s", argv[2]);
			return -EINVAL;
		}
		snprintk(saved.mask, sizeof(saved.mask), "%s", argv[2]);
	}

	if (argc >= 4) {
		if (net_addr_pton(AF_INET, argv[3], &tmp) < 0) {
			shell_error(sh, "Invalid gateway: %s", argv[3]);
			return -EINVAL;
		}
		snprintk(saved.gw, sizeof(saved.gw), "%s", argv[3]);
	}

	saved.mode = NET_CFG_STATIC;
	ret = net_cfg_set_saved(&saved);
	if (ret < 0) {
		shell_error(sh, "Failed to save config: %d", ret);
		return ret;
	}

	shell_print(sh, "Static IP saved: %s / %s gw %s", saved.ip, saved.mask, saved.gw);
	shell_print(sh, "Reboot to activate.");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(net_cfg_cmds,
	SHELL_CMD(show, NULL, "Show current network config", cmd_net_cfg_show),
	SHELL_CMD(dhcp, NULL, "Switch to DHCP mode", cmd_net_cfg_dhcp),
	SHELL_CMD(static, NULL, "Set static IP: static <ip> [mask] [gw]", cmd_net_cfg_static),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(ip, &net_cfg_cmds, "Network IP configuration", NULL);
