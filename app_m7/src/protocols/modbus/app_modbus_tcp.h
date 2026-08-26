#ifndef APP_MODBUS_TCP_H_
#define APP_MODBUS_TCP_H_

#include <stdint.h>

int app_modbus_tcp_start(void);
void app_modbus_tcp_heartbeat_tick(void);
int app_modbus_tcp_holding_read(uint16_t addr, uint16_t *value);
int app_modbus_tcp_holding_write(uint16_t addr, uint16_t value);
uint16_t app_modbus_tcp_connection_count(void);
uint16_t app_modbus_tcp_heartbeat_count(void);

#endif /* APP_MODBUS_TCP_H_ */
