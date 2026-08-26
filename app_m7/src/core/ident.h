#ifndef IDENT_H_
#define IDENT_H_

#include <stdbool.h>

#define APP_IDENT_DEVICE_NAME "STM32H747 Industrial Demo"
#define APP_IDENT_HOST_NAME "industrial-ethernet"
#define APP_IDENT_MANUFACTURER "STMicroelectronics"
#define APP_IDENT_MODEL "STM32H747I-DISCO"
#define APP_IDENT_FW_VERSION_MAJOR 0
#define APP_IDENT_FW_VERSION_MINOR 7
#define APP_IDENT_FW_VERSION_PATCH 0

#define APP_IDENT_FW_VERSION_STR_LEN 16
#define APP_IDENT_MAC_STR_LEN 18
#define APP_IDENT_IPV4_STR_LEN 16
#define APP_IDENT_IPV6_STR_LEN 46
#define APP_IDENT_UUID_STR_LEN 37
#define APP_IDENT_HW_ID_STR_LEN 25

struct app_ident_info {
	const char *device_name;
	const char *manufacturer;
	const char *model;
	char firmware_version[APP_IDENT_FW_VERSION_STR_LEN];
	char hardware_id[APP_IDENT_HW_ID_STR_LEN];
	char mac[APP_IDENT_MAC_STR_LEN];
	char ipv4[APP_IDENT_IPV4_STR_LEN];
	char ipv6_link_local[APP_IDENT_IPV6_STR_LEN];
	char uuid[APP_IDENT_UUID_STR_LEN];
	bool ipv6_preferred;
};

int app_ident_get(struct app_ident_info *info);

#endif /* IDENT_H_ */
