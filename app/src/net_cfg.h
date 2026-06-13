#ifndef NET_CFG_H_
#define NET_CFG_H_

#include <zephyr/net/net_if.h>

int net_cfg_load(void);
int net_cfg_apply(struct net_if *iface);

#endif /* NET_CFG_H_ */
