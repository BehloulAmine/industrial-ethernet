/*
 * Embedded HTTP dashboard for the industrial Ethernet demo.
 *
 * This first phase uses Zephyr sockets directly and keeps the route/API model
 * small. The static assets live in flash as C strings; they can later move to
 * LittleFS or Zephyr HTTP server v2 without changing the frontend contract.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/util.h>

#include "app_modbus_scanner.h"
#include "app_modbus_tcp.h"
#include "app_web.h"
#include "app_web_assets.h"
#include "app_web_logo.h"
#include "modbus_map.h"
#include "net_cfg.h"

LOG_MODULE_REGISTER(app_web, LOG_LEVEL_INF);

#define APP_WEB_PORT 80
#define APP_WEB_THREAD_STACK_SIZE 6144
#define APP_WEB_THREAD_PRIORITY 8
#define APP_WEB_BACKLOG 2
#define APP_WEB_REQ_BUF_SIZE 1536
#define APP_WEB_JSON_BUF_SIZE 2048

K_THREAD_STACK_DEFINE(app_web_stack, APP_WEB_THREAD_STACK_SIZE);
static struct k_thread app_web_thread;
static k_tid_t app_web_tid;
static bool web_started;

static int send_all(int client, const void *buf, size_t len)
{
	const uint8_t *cursor = buf;

	while (len > 0) {
		int ret = zsock_send(client, cursor, len, 0);

		if (ret < 0) {
			return -errno;
		}
		if (ret == 0) {
			return -ENOTCONN;
		}

		cursor += ret;
		len -= ret;
	}

	return 0;
}

static int send_response_data(int client, int code, const char *reason,
			      const char *type, const void *body, size_t body_len);

static int send_response(int client, int code, const char *reason,
			 const char *type, const char *body)
{
	size_t body_len = body ? strlen(body) : 0U;

	return send_response_data(client, code, reason, type, body, body_len);
}

static int send_response_data(int client, int code, const char *reason,
			      const char *type, const void *body, size_t body_len)
{
	char header[256];
	int len;
	int ret;

	len = snprintk(header, sizeof(header),
		      "HTTP/1.1 %d %s\r\n"
		      "Content-Type: %s\r\n"
		      "Content-Length: %u\r\n"
		      "Connection: close\r\n"
		      "Cache-Control: no-store\r\n"
		      "\r\n",
		      code, reason, type, (unsigned int)body_len);
	if (len < 0 || len >= sizeof(header)) {
		return -EMSGSIZE;
	}

	ret = send_all(client, header, (size_t)len);
	if (ret < 0 || body_len == 0U) {
		return ret;
	}

	return send_all(client, body, body_len);
}

static int send_not_found(int client)
{
	return send_response(client, 404, "Not Found", "application/json",
			     "{\"error\":\"not_found\"}");
}

static int send_bad_request(int client)
{
	return send_response(client, 400, "Bad Request", "application/json",
			     "{\"error\":\"bad_request\"}");
}

static uint16_t read_holding_or_zero(uint16_t addr)
{
	uint16_t value = 0U;

	(void)app_modbus_tcp_holding_read(addr, &value);
	return value;
}

static int json_status(char *json, size_t json_len)
{
	struct net_cfg_data active = { 0 };
	uint16_t last_status = 0U;

	(void)net_cfg_get_active(&active);
	last_status = read_holding_or_zero(APP_MB_HREG_STATUS);

	return snprintk(json, json_len,
			"{\"mode\":%u,\"ip\":\"%s\",\"mask\":\"%s\",\"gw\":\"%s\","
			"\"link\":%s,\"modbus_connections\":%u,"
			"\"heartbeat\":%u,\"last_status\":%d}",
			(unsigned int)active.mode, active.ip, active.mask, active.gw,
			net_cfg_link_is_up() ? "true" : "false",
			(unsigned int)app_modbus_tcp_connection_count(),
			(unsigned int)app_modbus_tcp_heartbeat_count(),
			(int16_t)last_status);
}

static int json_registers(char *json, size_t json_len)
{
	size_t off = 0U;
	int ret;

	ret = snprintk(json, json_len, "{\"count\":%u,\"holding\":[",
		       (unsigned int)APP_MODBUS_HOLDING_REG_COUNT);
	if (ret < 0 || ret >= json_len) {
		return -EMSGSIZE;
	}
	off = (size_t)ret;

	for (uint16_t i = 0; i < APP_MODBUS_HOLDING_REG_COUNT; i++) {
		uint16_t value;

		ret = app_modbus_tcp_holding_read(i, &value);
		if (ret < 0) {
			value = 0U;
		}

		ret = snprintk(&json[off], json_len - off, "%s%u",
			       i == 0U ? "" : ",", (unsigned int)value);
		if (ret < 0 || ret >= json_len - off) {
			return -EMSGSIZE;
		}
		off += (size_t)ret;
	}

	ret = snprintk(&json[off], json_len - off, "]}");
	if (ret < 0 || ret >= json_len - off) {
		return -EMSGSIZE;
	}

	return (int)(off + (size_t)ret);
}

static int json_scanner(char *json, size_t json_len)
{
	uint16_t mapping[APP_MODBUS_SCANNER_REG_COUNT];
	uint16_t values[APP_MODBUS_SCANNER_REG_COUNT];
	size_t off = 0U;
	int ret;

	for (uint16_t i = 0; i < APP_MODBUS_SCANNER_REG_COUNT; i++) {
		(void)app_modbus_tcp_holding_read(APP_MB_HREG_SCANNER_MAP_BASE + i,
						  &mapping[i]);
		if (app_modbus_scanner_holding_reg_rd(i, &values[i]) < 0) {
			values[i] = 0U;
		}
	}

	ret = snprintk(json, json_len, "{\"count\":%u,\"mapping\":[",
		       (unsigned int)APP_MODBUS_SCANNER_REG_COUNT);
	if (ret < 0 || ret >= json_len) {
		return -EMSGSIZE;
	}
	off = (size_t)ret;

	for (uint16_t i = 0; i < APP_MODBUS_SCANNER_REG_COUNT; i++) {
		ret = snprintk(&json[off], json_len - off, "%s%u",
			       i == 0U ? "" : ",", (unsigned int)mapping[i]);
		if (ret < 0 || ret >= json_len - off) {
			return -EMSGSIZE;
		}
		off += (size_t)ret;
	}

	ret = snprintk(&json[off], json_len - off, "],\"values\":[");
	if (ret < 0 || ret >= json_len - off) {
		return -EMSGSIZE;
	}
	off += (size_t)ret;

	for (uint16_t i = 0; i < APP_MODBUS_SCANNER_REG_COUNT; i++) {
		ret = snprintk(&json[off], json_len - off, "%s%u",
			       i == 0U ? "" : ",", (unsigned int)values[i]);
		if (ret < 0 || ret >= json_len - off) {
			return -EMSGSIZE;
		}
		off += (size_t)ret;
	}

	ret = snprintk(&json[off], json_len - off, "]}");
	if (ret < 0 || ret >= json_len - off) {
		return -EMSGSIZE;
	}

	return (int)(off + (size_t)ret);
}

static int parse_value(const char *body, uint16_t *value)
{
	const char *cursor;
	char *end;
	unsigned long parsed;

	if (!body || !value) {
		return -EINVAL;
	}

	cursor = strstr(body, "value");
	cursor = cursor ? cursor + strlen("value") : body;
	while (*cursor != '\0' && (*cursor < '0' || *cursor > '9')) {
		cursor++;
	}
	if (*cursor == '\0') {
		return -EINVAL;
	}

	parsed = strtoul(cursor, &end, 10);
	if (end == cursor || parsed > UINT16_MAX) {
		return -EINVAL;
	}

	*value = (uint16_t)parsed;
	return 0;
}

static int parse_id_after_prefix(const char *path, const char *prefix, uint16_t *id)
{
	const char *cursor;
	char *end;
	unsigned long parsed;

	if (strncmp(path, prefix, strlen(prefix)) != 0) {
		return -EINVAL;
	}

	cursor = path + strlen(prefix);
	parsed = strtoul(cursor, &end, 10);
	if (end == cursor || *end != '\0' || parsed > UINT16_MAX) {
		return -EINVAL;
	}

	*id = (uint16_t)parsed;
	return 0;
}

static int parse_request_line(char *req, char *method, size_t method_len,
			      char *path, size_t path_len)
{
	char *method_end;
	char *path_end;
	size_t len;

	method_end = strchr(req, ' ');
	if (!method_end) {
		return -EINVAL;
	}

	len = (size_t)(method_end - req);
	if (len == 0U || len >= method_len) {
		return -EINVAL;
	}
	memcpy(method, req, len);
	method[len] = '\0';

	while (*method_end == ' ') {
		method_end++;
	}

	path_end = strchr(method_end, ' ');
	if (!path_end) {
		return -EINVAL;
	}

	len = (size_t)(path_end - method_end);
	if (len == 0U || len >= path_len) {
		return -EINVAL;
	}
	memcpy(path, method_end, len);
	path[len] = '\0';

	return 0;
}

static int handle_api_get(int client, const char *path)
{
	static char json[APP_WEB_JSON_BUF_SIZE];
	int len;

	if (strcmp(path, "/api/status") == 0) {
		len = json_status(json, sizeof(json));
	} else if (strcmp(path, "/api/registers") == 0) {
		len = json_registers(json, sizeof(json));
	} else if (strcmp(path, "/api/scanner") == 0) {
		len = json_scanner(json, sizeof(json));
	} else {
		return send_not_found(client);
	}

	if (len < 0 || len >= sizeof(json)) {
		return send_response(client, 500, "Internal Server Error",
				     "application/json", "{\"error\":\"json_overflow\"}");
	}

	return send_response(client, 200, "OK", "application/json", json);
}

static int handle_api_post(int client, const char *path, const char *body)
{
	uint16_t id;
	uint16_t value;
	int ret;

	ret = parse_value(body, &value);
	if (ret < 0) {
		return send_bad_request(client);
	}

	if (parse_id_after_prefix(path, "/api/registers/", &id) == 0) {
		ret = app_modbus_tcp_holding_write(id, value);
	} else if (parse_id_after_prefix(path, "/api/scanner/", &id) == 0) {
		ret = app_modbus_scanner_holding_reg_wr(id, value);
	} else {
		return send_not_found(client);
	}

	if (ret < 0) {
		return send_bad_request(client);
	}

	return send_response(client, 200, "OK", "application/json", "{\"ok\":true}");
}

static int handle_http_request(int client, char *req)
{
	char method[8];
	char path[96];
	char *body;

	if (parse_request_line(req, method, sizeof(method), path, sizeof(path)) < 0) {
		return send_bad_request(client);
	}

	body = strstr(req, "\r\n\r\n");
	if (body) {
		body += 4;
	}

	if (strcmp(method, "GET") == 0) {
		if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
			return send_response(client, 200, "OK", "text/html; charset=utf-8",
					     app_web_index_html);
		}
		if (strcmp(path, "/app.css") == 0) {
			return send_response(client, 200, "OK", "text/css; charset=utf-8",
					     app_web_css);
		}
		if (strcmp(path, "/app.js") == 0) {
			return send_response(client, 200, "OK",
					     "application/javascript; charset=utf-8",
					     app_web_js);
		}
		if (strcmp(path, "/logo.jpg") == 0) {
			return send_response_data(client, 200, "OK", "image/jpeg",
						  app_web_logo_jpg,
						  app_web_logo_jpg_len);
		}
		if (strncmp(path, "/api/", 5) == 0) {
			return handle_api_get(client, path);
		}

		return send_not_found(client);
	}

	if (strcmp(method, "POST") == 0) {
		return handle_api_post(client, path, body);
	}

	return send_response(client, 405, "Method Not Allowed", "application/json",
			     "{\"error\":\"method_not_allowed\"}");
}

static void app_web_thread_fn(void *arg1, void *arg2, void *arg3)
{
	struct sockaddr_in bind_addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(APP_WEB_PORT),
	};
	int server;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	server = zsock_socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (server < 0) {
		LOG_ERR("HTTP socket failed: %d", errno);
		return;
	}

	if (zsock_bind(server, (struct net_sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
		LOG_ERR("HTTP bind port %d failed: %d", APP_WEB_PORT, errno);
		(void)zsock_close(server);
		return;
	}

	if (zsock_listen(server, APP_WEB_BACKLOG) < 0) {
		LOG_ERR("HTTP listen failed: %d", errno);
		(void)zsock_close(server);
		return;
	}

	LOG_INF("HTTP dashboard listening on port %d", APP_WEB_PORT);

	while (true) {
		struct sockaddr_in client_addr;
		net_socklen_t client_addr_len = sizeof(client_addr);
		char req[APP_WEB_REQ_BUF_SIZE];
		int client;
		int len;

		client = zsock_accept(server, (struct net_sockaddr *)&client_addr,
				      &client_addr_len);
		if (client < 0) {
			LOG_ERR("HTTP accept failed: %d", errno);
			continue;
		}

		len = zsock_recv(client, req, sizeof(req) - 1U, 0);
		if (len > 0) {
			req[len] = '\0';
			(void)handle_http_request(client, req);
		}

		(void)zsock_close(client);
	}
}

int app_web_start(void)
{
	if (web_started) {
		return 0;
	}

	app_web_tid = k_thread_create(&app_web_thread, app_web_stack,
				      K_THREAD_STACK_SIZEOF(app_web_stack),
				      app_web_thread_fn,
				      NULL, NULL, NULL,
				      APP_WEB_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(app_web_tid, "app_web");

	web_started = true;
	return 0;
}
