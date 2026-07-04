#ifndef APP_MODBUS_SCANNER_H_
#define APP_MODBUS_SCANNER_H_

#include <stdint.h>

#define APP_MODBUS_SCANNER_UNIT_ID 2
#define APP_MODBUS_SCANNER_REG_COUNT 10

typedef int (*app_modbus_scanner_reg_rd_t)(uint16_t addr, uint16_t *reg);
typedef int (*app_modbus_scanner_reg_wr_t)(uint16_t addr, uint16_t reg);

void app_modbus_scanner_init(app_modbus_scanner_reg_rd_t reg_rd,
			     app_modbus_scanner_reg_wr_t reg_wr);
int app_modbus_scanner_holding_reg_rd(uint16_t addr, uint16_t *reg);
int app_modbus_scanner_holding_reg_wr(uint16_t addr, uint16_t reg);

#endif /* APP_MODBUS_SCANNER_H_ */
