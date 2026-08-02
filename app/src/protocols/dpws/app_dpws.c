/* DPWS device discovery and WS-Transfer metadata endpoint. */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

#include "app_dpws.h"
#include "ident.h"
#include "net_cfg.h"

LOG_MODULE_REGISTER(app_dpws, LOG_LEVEL_INF);

#define APP_DPWS_PORT 3702
#define APP_DPWS_IPV4_MULTICAST "239.255.255.250"
#define APP_DPWS_IPV6_MULTICAST "ff02::c"
#define APP_DPWS_THREAD_STACK_SIZE 4096
#define APP_DPWS_THREAD_PRIORITY 8
#define APP_DPWS_PACKET_SIZE 2048
#define APP_DPWS_METADATA_PATH_PREFIX "/dpws/"

#define APP_DPWS_DISCOVERY_10 "http://schemas.xmlsoap.org/ws/2005/04/discovery"
#define APP_DPWS_DISCOVERY_11 "http://docs.oasis-open.org/ws-dd/ns/discovery/2009/01"
#define APP_DPWS_ADDRESSING_10 "http://schemas.xmlsoap.org/ws/2004/08/addressing"
#define APP_DPWS_ADDRESSING_11 "http://www.w3.org/2005/08/addressing"
#define APP_DPWS_TRANSFER "http://schemas.xmlsoap.org/ws/2004/09/transfer"
#define APP_DPWS_DEVICE_PROFILE "http://schemas.xmlsoap.org/ws/2006/02/devprof"
#define APP_DPWS_METADATA_EXCHANGE "http://schemas.xmlsoap.org/ws/2004/09/mex"

struct app_dpws_profile {
	const char *discovery_ns;
	const char *addressing_ns;
	const char *anonymous;
};

K_THREAD_STACK_DEFINE(dpws_stack, APP_DPWS_THREAD_STACK_SIZE);
static struct k_thread dpws_thread;
static atomic_t dpws_message_number;
static char dpws_request[APP_DPWS_PACKET_SIZE];
static char dpws_reply[APP_DPWS_PACKET_SIZE];
static bool dpws_started;

static const struct app_dpws_profile profile_10 = {
	.discovery_ns = APP_DPWS_DISCOVERY_10,
	.addressing_ns = APP_DPWS_ADDRESSING_10,
	.anonymous = "http://schemas.xmlsoap.org/ws/2004/08/addressing/role/anonymous",
};

static const struct app_dpws_profile profile_11 = {
	.discovery_ns = APP_DPWS_DISCOVERY_11,
	.addressing_ns = APP_DPWS_ADDRESSING_11,
	.anonymous = "http://www.w3.org/2005/08/addressing/anonymous",
};

static const struct app_dpws_profile *dpws_profile_from_request(const char *request)
{
	return strstr(request, APP_DPWS_DISCOVERY_11) != NULL ?
		&profile_11 : &profile_10;
}

static bool dpws_extract_element(const char *xml, const char *element,
				 char *value, size_t value_size)
{
	char marker[40];
	const char *start;
	const char *end;
	size_t len;

	snprintk(marker, sizeof(marker), "%s>", element);
	start = strstr(xml, marker);
	if (start == NULL) {
		return false;
	}
	start = strchr(start, '>');
	if (start == NULL) {
		return false;
	}
	start++;
	end = strchr(start, '<');
	if (end == NULL) {
		return false;
	}

	len = (size_t)(end - start);
	if (len == 0U || len >= value_size) {
		return false;
	}

	memcpy(value, start, len);
	value[len] = '\0';
	return true;
}

static bool dpws_is_probe(const char *request)
{
	return strstr(request, "/Probe") != NULL &&
	       strstr(request, "ProbeMatches") == NULL &&
	       (strstr(request, APP_DPWS_DISCOVERY_10) != NULL ||
		strstr(request, APP_DPWS_DISCOVERY_11) != NULL);
}

static void dpws_make_message_uuid(char *uuid, size_t uuid_size)
{
	uint32_t sequence = (uint32_t)atomic_inc(&dpws_message_number) + 1U;
	uint32_t cycles = k_cycle_get_32();

	snprintk(uuid, uuid_size, "00000000-0000-4000-8000-%08x%04x",
		 cycles, sequence & 0xffffU);
}

static int dpws_format_xaddrs(const struct app_ident_info *ident, int family,
			      char *xaddrs, size_t xaddrs_size)
{
	bool has_ipv4 = strcmp(ident->ipv4, "unavailable") != 0 &&
			strcmp(ident->ipv4, "0.0.0.0") != 0;
	bool has_ipv6 = strcmp(ident->ipv6_link_local, "unavailable") != 0;

	if (family == AF_INET6 && has_ipv6 && has_ipv4) {
		return snprintk(xaddrs, xaddrs_size,
				 "http://[%s]/dpws/%s http://%s/dpws/%s",
				 ident->ipv6_link_local, ident->uuid,
				 ident->ipv4, ident->uuid);
	}
	if (has_ipv4 && has_ipv6) {
		return snprintk(xaddrs, xaddrs_size,
				 "http://%s/dpws/%s http://[%s]/dpws/%s",
				 ident->ipv4, ident->uuid,
				 ident->ipv6_link_local, ident->uuid);
	}
	if (has_ipv6) {
		return snprintk(xaddrs, xaddrs_size, "http://[%s]/dpws/%s",
				 ident->ipv6_link_local, ident->uuid);
	}
	if (has_ipv4) {
		return snprintk(xaddrs, xaddrs_size, "http://%s/dpws/%s",
				 ident->ipv4, ident->uuid);
	}

	return -ENETDOWN;
}

static int dpws_build_probe_match(char *reply, size_t reply_size,
				  const char *request, int family)
{
	const struct app_dpws_profile *profile = dpws_profile_from_request(request);
	struct app_ident_info ident;
	char relates_to[64];
	char message_uuid[APP_IDENT_UUID_STR_LEN];
	char xaddrs[192];
	uint32_t message_number;
	int ret;

	if (!dpws_extract_element(request, "MessageID", relates_to,
				  sizeof(relates_to))) {
		return -EINVAL;
	}
	if (app_ident_get(&ident) < 0 || strcmp(ident.uuid, "unavailable") == 0) {
		return -ENODEV;
	}

	ret = dpws_format_xaddrs(&ident, family, xaddrs, sizeof(xaddrs));
	if (ret < 0 || ret >= sizeof(xaddrs)) {
		return ret < 0 ? ret : -EMSGSIZE;
	}

	dpws_make_message_uuid(message_uuid, sizeof(message_uuid));
	message_number = (uint32_t)atomic_get(&dpws_message_number);

	ret = snprintk(reply, reply_size,
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
		"xmlns:wsa=\"%s\" xmlns:wsd=\"%s\" "
		"xmlns:wsdp=\"%s\" xmlns:ie=\"urn:industrial-ethernet\">"
		"<soap:Header>"
		"<wsa:Action>%s/ProbeMatches</wsa:Action>"
		"<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
		"<wsa:RelatesTo>%s</wsa:RelatesTo>"
		"<wsa:To>%s</wsa:To>"
		"<wsd:AppSequence InstanceId=\"1\" MessageNumber=\"%u\"/>"
		"</soap:Header><soap:Body><wsd:ProbeMatches><wsd:ProbeMatch>"
		"<wsa:EndpointReference><wsa:Address>urn:uuid:%s</wsa:Address>"
		"</wsa:EndpointReference>"
		"<wsd:Types>wsdp:Device ie:IndustrialEthernetDevice</wsd:Types>"
		"<wsd:Scopes>urn:industrial-ethernet:name:%s "
		"urn:industrial-ethernet:model:%s</wsd:Scopes>"
		"<wsd:XAddrs>%s</wsd:XAddrs><wsd:MetadataVersion>1</wsd:MetadataVersion>"
		"</wsd:ProbeMatch></wsd:ProbeMatches></soap:Body></soap:Envelope>",
		profile->addressing_ns, profile->discovery_ns, APP_DPWS_DEVICE_PROFILE,
		profile->discovery_ns, message_uuid, relates_to, profile->anonymous,
		message_number, ident.uuid, APP_IDENT_HOST_NAME, ident.model, xaddrs);

	if (ret < 0 || ret >= reply_size) {
		return -EMSGSIZE;
	}
	return ret;
}

static int dpws_join_ipv6(int sock, struct net_if *iface)
{
	struct net_ipv6_mreq membership = { 0 };
	int ret;

	if (net_addr_pton(AF_INET6, APP_DPWS_IPV6_MULTICAST,
			  &membership.ipv6mr_multiaddr) < 0) {
		return -EINVAL;
	}
	membership.ipv6mr_ifindex = net_if_get_by_iface(iface);

	ret = zsock_setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP,
			       &membership, sizeof(membership));
	if (ret < 0 && errno != EALREADY && errno != EINPROGRESS) {
		return -errno;
	}
	return 0;
}

static int dpws_join_ipv4(int sock, struct net_if *iface)
{
	struct net_ip_mreqn membership = { 0 };
	int ret;

	if (net_addr_pton(AF_INET, APP_DPWS_IPV4_MULTICAST,
			  &membership.imr_multiaddr) < 0) {
		return -EINVAL;
	}
	membership.imr_ifindex = net_if_get_by_iface(iface);

	ret = zsock_setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
			       &membership, sizeof(membership));
	if (ret < 0 && errno != EALREADY && errno != EINPROGRESS) {
		return -errno;
	}
	return 0;
}

static int dpws_create_socket(int family, struct net_if *iface)
{
	int sock;
	int reuse = 1;
	int ret;

	sock = zsock_socket(family, SOCK_DGRAM, IPPROTO_UDP);
	if (sock < 0) {
		return -errno;
	}
	(void)zsock_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	if (family == AF_INET6) {
		struct sockaddr_in6 bind_addr = {
			.sin6_family = AF_INET6,
			.sin6_addr = IN6ADDR_ANY_INIT,
			.sin6_port = htons(APP_DPWS_PORT),
		};
		int ipv6_only = 1;

		(void)zsock_setsockopt(sock, IPPROTO_IPV6, IPV6_V6ONLY,
				       &ipv6_only, sizeof(ipv6_only));
		ret = zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
		if (ret == 0) {
			ret = dpws_join_ipv6(sock, iface);
		}
	} else {
		struct sockaddr_in bind_addr = {
			.sin_family = AF_INET,
			.sin_addr.s_addr = htonl(INADDR_ANY),
			.sin_port = htons(APP_DPWS_PORT),
		};

		ret = zsock_bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr));
		if (ret == 0) {
			ret = dpws_join_ipv4(sock, iface);
		}
	}

	if (ret < 0) {
		(void)zsock_close(sock);
		return ret;
	}
	return sock;
}

static void dpws_log_peer(const struct sockaddr_storage *peer)
{
	char address[NET_IPV6_ADDR_LEN];
	const void *source;
	int family = peer->ss_family;

	if (family == AF_INET6) {
		source = &((const struct sockaddr_in6 *)peer)->sin6_addr;
	} else {
		source = &((const struct sockaddr_in *)peer)->sin_addr;
	}

	if (net_addr_ntop(family, source, address, sizeof(address)) != NULL) {
		LOG_INF("DPWS Probe from %s", address);
	}
}

static void dpws_process_probe(int sock)
{
	struct sockaddr_storage peer;
	net_socklen_t peer_len = sizeof(peer);
	int reply_len;
	int len;

	len = zsock_recvfrom(sock, dpws_request, sizeof(dpws_request) - 1U, 0,
			     (struct sockaddr *)&peer, &peer_len);
	if (len < 0) {
		LOG_WRN("DPWS receive failed: %d", errno);
		return;
	}
	dpws_request[len] = '\0';
	if (!dpws_is_probe(dpws_request)) {
		return;
	}

	reply_len = dpws_build_probe_match(dpws_reply, sizeof(dpws_reply), dpws_request,
					   peer.ss_family);
	if (reply_len < 0) {
		LOG_WRN("DPWS ProbeMatch build failed: %d", reply_len);
		return;
	}

	dpws_log_peer(&peer);
	k_msleep(k_cycle_get_32() % 501U);
	if (zsock_sendto(sock, dpws_reply, (size_t)reply_len, 0,
			 (struct sockaddr *)&peer, peer_len) < 0) {
		LOG_WRN("DPWS ProbeMatch send failed: %d", errno);
	}
}

static void dpws_thread_fn(void *arg1, void *arg2, void *arg3)
{
	struct zsock_pollfd sockets[2] = { 0 };
	struct app_ident_info ident;
	struct net_if *iface;
	int socket_count = 0;
	int sock;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	do {
		iface = net_if_get_default();
		(void)app_ident_get(&ident);
		if (iface == NULL || !net_cfg_link_is_up() || !ident.ipv6_preferred) {
			k_msleep(100);
		}
	} while (iface == NULL || !net_cfg_link_is_up() || !ident.ipv6_preferred);

	sock = dpws_create_socket(AF_INET6, iface);
	if (sock >= 0) {
		sockets[socket_count].fd = sock;
		sockets[socket_count].events = ZSOCK_POLLIN;
		socket_count++;
	} else {
		LOG_ERR("DPWS IPv6 listener failed: %d", sock);
	}

	sock = dpws_create_socket(AF_INET, iface);
	if (sock >= 0) {
		sockets[socket_count].fd = sock;
		sockets[socket_count].events = ZSOCK_POLLIN;
		socket_count++;
	} else {
		LOG_WRN("DPWS IPv4 listener unavailable: %d", sock);
	}

	if (socket_count == 0) {
		return;
	}

	LOG_INF("DPWS ready: [%s]:%d and %s:%d",
		APP_DPWS_IPV6_MULTICAST, APP_DPWS_PORT,
		APP_DPWS_IPV4_MULTICAST, APP_DPWS_PORT);

	while (true) {
		int ret = zsock_poll(sockets, socket_count, -1);

		if (ret < 0) {
			LOG_ERR("DPWS poll failed: %d", errno);
			continue;
		}
		for (int i = 0; i < socket_count; i++) {
			if ((sockets[i].revents & ZSOCK_POLLIN) != 0) {
				dpws_process_probe(sockets[i].fd);
			}
		}
	}
}

bool app_dpws_is_metadata_path(const char *path)
{
	struct app_ident_info ident;
	char expected[sizeof(APP_DPWS_METADATA_PATH_PREFIX) + APP_IDENT_UUID_STR_LEN];

	if (path == NULL || app_ident_get(&ident) < 0) {
		return false;
	}
	snprintk(expected, sizeof(expected), "%s%s",
		 APP_DPWS_METADATA_PATH_PREFIX, ident.uuid);
	return strcmp(path, expected) == 0;
}

int app_dpws_build_metadata(char *xml, size_t xml_size, const char *request)
{
	struct app_ident_info ident;
	char relates_to[64] = "";
	char message_uuid[APP_IDENT_UUID_STR_LEN];
	char presentation_url[96];
	uint64_t uptime_s = (uint64_t)k_uptime_get() / 1000U;
	int ret;

	if (xml == NULL || request == NULL ||
	    strstr(request, APP_DPWS_TRANSFER "/Get") == NULL) {
		return -EINVAL;
	}
	(void)dpws_extract_element(request, "MessageID", relates_to, sizeof(relates_to));
	if (app_ident_get(&ident) < 0) {
		return -ENODEV;
	}

	dpws_make_message_uuid(message_uuid, sizeof(message_uuid));
	if (strcmp(ident.ipv6_link_local, "unavailable") != 0) {
		snprintk(presentation_url, sizeof(presentation_url), "http://[%s]/",
			 ident.ipv6_link_local);
	} else {
		snprintk(presentation_url, sizeof(presentation_url), "http://%s/", ident.ipv4);
	}

	ret = snprintk(xml, xml_size,
		"<?xml version=\"1.0\" encoding=\"utf-8\"?>"
		"<soap:Envelope xmlns:soap=\"http://www.w3.org/2003/05/soap-envelope\" "
		"xmlns:wsa=\"%s\" xmlns:wsx=\"%s\" xmlns:wsdp=\"%s\" "
		"xmlns:ie=\"urn:industrial-ethernet\" xml:lang=\"en\">"
		"<soap:Header><wsa:Action>%s/GetResponse</wsa:Action>"
		"<wsa:MessageID>urn:uuid:%s</wsa:MessageID>"
		"<wsa:RelatesTo>%s</wsa:RelatesTo>"
		"<wsa:To>%s</wsa:To></soap:Header><soap:Body><wsx:Metadata>"
		"<wsx:MetadataSection Dialect=\"%s/ThisModel\"><wsdp:ThisModel>"
		"<wsdp:Manufacturer>%s</wsdp:Manufacturer>"
		"<wsdp:ManufacturerUrl>https://www.st.com/</wsdp:ManufacturerUrl>"
		"<wsdp:ModelName>%s</wsdp:ModelName><wsdp:ModelNumber>%s</wsdp:ModelNumber>"
		"<wsdp:ModelUrl>https://www.st.com/stm32h747i-disco</wsdp:ModelUrl>"
		"<wsdp:PresentationUrl>%s</wsdp:PresentationUrl>"
		"</wsdp:ThisModel></wsx:MetadataSection>"
		"<wsx:MetadataSection Dialect=\"%s/ThisDevice\"><wsdp:ThisDevice>"
		"<wsdp:FriendlyName>%s</wsdp:FriendlyName>"
		"<wsdp:FirmwareVersion>%s</wsdp:FirmwareVersion>"
		"<wsdp:SerialNumber>%s</wsdp:SerialNumber>"
		"</wsdp:ThisDevice></wsx:MetadataSection>"
		"<wsx:MetadataSection Dialect=\"urn:industrial-ethernet:device-info\">"
		"<ie:DeviceInfo><ie:ProductCode>STM32H747-DEMO</ie:ProductCode>"
		"<ie:ProductRange>Industrial Ethernet Demo</ie:ProductRange>"
		"<ie:ProductCapability>Modbus TCP, EtherNet/IP, HTTP, DPWS</ie:ProductCapability>"
		"<ie:HardwareRevision>%s</ie:HardwareRevision>"
		"<ie:FriendlyNameSource>Device</ie:FriendlyNameSource>"
		"<ie:PhysicalLocation>Local network</ie:PhysicalLocation>"
		"<ie:NodeName>%s</ie:NodeName><ie:TimeSinceBoot>%llu</ie:TimeSinceBoot>"
		"<ie:ServicesSupported>HTTP,ModbusTCP,EtherNetIP,WS-Discovery</ie:ServicesSupported>"
		"<ie:MacAddress>%s</ie:MacAddress><ie:IPv4Address>%s</ie:IPv4Address>"
		"<ie:IPv6Address>%s</ie:IPv6Address><ie:DeviceUUID>%s</ie:DeviceUUID>"
		"</ie:DeviceInfo></wsx:MetadataSection>"
		"</wsx:Metadata></soap:Body></soap:Envelope>",
		APP_DPWS_ADDRESSING_10, APP_DPWS_METADATA_EXCHANGE,
		APP_DPWS_DEVICE_PROFILE, APP_DPWS_TRANSFER, message_uuid,
		relates_to, profile_10.anonymous,
		APP_DPWS_DEVICE_PROFILE, ident.manufacturer, ident.model, ident.model,
		presentation_url, APP_DPWS_DEVICE_PROFILE, ident.device_name,
		ident.firmware_version, ident.hardware_id, ident.model,
		APP_IDENT_HOST_NAME, uptime_s, ident.mac, ident.ipv4,
		ident.ipv6_link_local, ident.uuid);

	if (ret < 0 || ret >= xml_size) {
		return -EMSGSIZE;
	}
	return ret;
}

int app_dpws_start(void)
{
	if (dpws_started) {
		return 0;
	}

	(void)k_thread_create(&dpws_thread, dpws_stack,
			      K_THREAD_STACK_SIZEOF(dpws_stack), dpws_thread_fn,
			      NULL, NULL, NULL, APP_DPWS_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&dpws_thread, "dpws");
	dpws_started = true;
	return 0;
}
