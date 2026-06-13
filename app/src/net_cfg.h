#ifndef NET_CFG_H_
#define NET_CFG_H_

#include <stdbool.h>
#include <zephyr/net/net_if.h>

enum net_cfg_mode {
	NET_CFG_DHCP = 0,
	NET_CFG_STATIC = 1,
};

struct net_cfg_data {
	enum net_cfg_mode mode;
	char ip[16];
	char mask[16];
	char gw[16];
};

int net_cfg_load(void);
int net_cfg_apply(struct net_if *iface);
int net_cfg_get_saved(struct net_cfg_data *out);
int net_cfg_set_saved(const struct net_cfg_data *in);
int net_cfg_get_active(struct net_cfg_data *out);
bool net_cfg_link_is_up(void);

#endif /* NET_CFG_H_ */
