#ifndef PLATFORM_NETWORK_INCLUDES_H_
#define PLATFORM_NETWORK_INCLUDES_H_

#include <zephyr/net/socket.h>

typedef zsock_fd_set fd_set;
typedef uint16_t in_port_t;

int app_eip_socket(int family, int type, int protocol);
int app_eip_setsockopt(int socket, int level, int option_name,
		       const void *option_value, net_socklen_t option_length);
int app_eip_bind(int socket, const struct sockaddr *address,
		 net_socklen_t address_length);
int app_eip_listen(int socket, int backlog);

#define socklen_t net_socklen_t
#define FD_ZERO ZSOCK_FD_ZERO
#define FD_ISSET ZSOCK_FD_ISSET
#define FD_CLR ZSOCK_FD_CLR
#define FD_SET ZSOCK_FD_SET
#define select zsock_select

#define socket app_eip_socket
#define setsockopt app_eip_setsockopt
#define bind app_eip_bind
#define listen app_eip_listen
#define accept zsock_accept
#define recv zsock_recv
#define recvfrom zsock_recvfrom
#define send zsock_send
#define sendto zsock_sendto
#define getpeername zsock_getpeername
#define inet_ntop zsock_inet_ntop

#endif /* PLATFORM_NETWORK_INCLUDES_H_ */
