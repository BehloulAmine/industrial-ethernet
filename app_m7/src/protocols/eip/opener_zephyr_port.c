/* Platform hooks required by OpENer's generic BSD-socket network handler. */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/fdtable.h>

#include "networkhandler.h"
#include "opener_error.h"

LOG_MODULE_DECLARE(app_eip);

MicroSeconds GetMicroSeconds(void)
{
	return (MicroSeconds)k_ticks_to_us_floor64(k_uptime_ticks());
}

MilliSeconds GetMilliSeconds(void)
{
	return (MilliSeconds)k_uptime_get();
}

EipStatus NetworkHandlerInitializePlatform(void)
{
	return kEipStatusOk;
}

int app_eip_socket(int family, int type, int protocol)
{
	int socket = zsock_socket(family, type, protocol);

	if (socket < 0) {
		LOG_ERR("socket creation failed: type=%d errno=%d", type, errno);
	}
	return socket;
}

int app_eip_setsockopt(int socket, int level, int option_name,
		       const void *option_value, net_socklen_t option_length)
{
	/* Zephyr receives IPv4 broadcasts without the BSD SO_BROADCAST gate. */
	if (level == SOL_SOCKET && option_name == SO_BROADCAST) {
		return 0;
	}

	/* OpENer passes an IPv4 address, while Zephyr expects an ip_mreq. */
	if (level == IPPROTO_IP && option_name == IP_MULTICAST_IF &&
	    option_value != NULL && option_length == sizeof(struct net_in_addr)) {
		struct net_ip_mreq request = { 0 };

		memcpy(&request.imr_interface, option_value,
		       sizeof(request.imr_interface));
		return zsock_setsockopt(socket, level, option_name, &request,
					 sizeof(request));
	}

	int ret = zsock_setsockopt(socket, level, option_name, option_value,
				   option_length);

	if (ret < 0) {
		LOG_ERR("setsockopt failed: fd=%d level=%d option=%d errno=%d",
			socket, level, option_name, errno);
	}
	return ret;
}

int app_eip_bind(int socket, const struct sockaddr *address,
		 net_socklen_t address_length)
{
	int ret = zsock_bind(socket, address, address_length);

	if (ret < 0) {
		LOG_ERR("socket bind failed: fd=%d errno=%d", socket, errno);
	}
	return ret;
}

int app_eip_listen(int socket, int backlog)
{
	int ret = zsock_listen(socket, backlog);

	if (ret < 0) {
		LOG_ERR("socket listen failed: fd=%d errno=%d", socket, errno);
	}
	return ret;
}

void ShutdownSocketPlatform(int socket_handle)
{
	(void)zsock_shutdown(socket_handle, ZSOCK_SHUT_RDWR);
}

void CloseSocketPlatform(int socket_handle)
{
	(void)zsock_close(socket_handle);
}

int SetSocketToNonBlocking(int socket_handle)
{
	int ret = zsock_fcntl(socket_handle, ZVFS_F_SETFL, ZVFS_O_NONBLOCK);

	if (ret < 0) {
		LOG_ERR("non-blocking mode failed: fd=%d errno=%d", socket_handle,
			errno);
	}
	return ret;
}

int SetQosOnSocket(const int socket_handle, CipUsint qos_value)
{
	uint8_t tos = qos_value << 2;

	return zsock_setsockopt(socket_handle, IPPROTO_IP, IP_TOS,
				&tos, sizeof(tos));
}

int GetSocketErrorNumber(void)
{
	return errno;
}

char *GetErrorMessage(int error_number)
{
	return strerror(error_number);
}

void FreeErrorMessage(char *error_message)
{
	ARG_UNUSED(error_message);
}
