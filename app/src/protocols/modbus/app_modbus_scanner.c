/*
 * Modbus Unit-ID 2 scanner window.
 *
 * Unit-ID 1 owns the scanner mapping table at registers[50..59].
 * Unit-ID 2 exposes scanner_regs[0..9] as a dynamic window over the data
 * selected by that mapping table.
 */

#include <errno.h>
#include <stdbool.h>

#include <zephyr/kernel.h>

#include "app_modbus_scanner.h"
#include "modbus_map.h"

static uint16_t scanner_local_regs[APP_MODBUS_SCANNER_REG_COUNT];
static K_MUTEX_DEFINE(scanner_local_regs_lock);

static app_modbus_scanner_reg_rd_t unit1_reg_rd;
static app_modbus_scanner_reg_wr_t unit1_reg_wr;

static uint16_t read_count;
static uint16_t write_count;
static uint16_t last_addr;
static int last_error;
static uint32_t last_access_ms;
static uint16_t scanner_period_ms;

static void update_access_diag(uint16_t addr, bool write, int err)
{
	uint32_t now_ms = k_uptime_get_32();

	if (last_access_ms != 0U) {
		scanner_period_ms = (uint16_t)(now_ms - last_access_ms);
	}

	last_access_ms = now_ms;
	last_addr = addr;
	last_error = err;

	if (write) {
		write_count++;
	} else {
		read_count++;
	}
}

static int scanner_map_rd(uint16_t scanner_addr, uint16_t *holding_addr)
{
	if (!unit1_reg_rd) {
		return -ENODEV;
	}

	return unit1_reg_rd(APP_MB_HREG_SCANNER_MAP_BASE + scanner_addr, holding_addr);
}

void app_modbus_scanner_init(app_modbus_scanner_reg_rd_t reg_rd,
			     app_modbus_scanner_reg_wr_t reg_wr)
{
	unit1_reg_rd = reg_rd;
	unit1_reg_wr = reg_wr;
}

int app_modbus_scanner_holding_reg_rd(uint16_t addr, uint16_t *reg)
{
	uint16_t holding_addr;
	int ret;

	if (!reg) {
		return -EINVAL;
	}

	if (addr >= APP_MODBUS_SCANNER_REG_COUNT) {
		update_access_diag(addr, false, -ENOTSUP);
		return -ENOTSUP;
	}

	ret = scanner_map_rd(addr, &holding_addr);
	if (ret < 0) {
		update_access_diag(addr, false, ret);
		return ret;
	}

	if (holding_addr == APP_MB_SCAN_MAP_FREE) {
		k_mutex_lock(&scanner_local_regs_lock, K_FOREVER);
		*reg = scanner_local_regs[addr];
		k_mutex_unlock(&scanner_local_regs_lock);
		update_access_diag(addr, false, 0);
		return 0;
	}

	ret = unit1_reg_rd ? unit1_reg_rd(holding_addr, reg) : -ENODEV;

	update_access_diag(addr, false, ret);
	return ret;
}

int app_modbus_scanner_holding_reg_wr(uint16_t addr, uint16_t reg)
{
	uint16_t holding_addr;
	int ret;

	if (addr >= APP_MODBUS_SCANNER_REG_COUNT) {
		update_access_diag(addr, true, -ENOTSUP);
		return -ENOTSUP;
	}

	ret = scanner_map_rd(addr, &holding_addr);
	if (ret < 0) {
		update_access_diag(addr, true, ret);
		return ret;
	}

	if (holding_addr == APP_MB_SCAN_MAP_FREE) {
		k_mutex_lock(&scanner_local_regs_lock, K_FOREVER);
		scanner_local_regs[addr] = reg;
		k_mutex_unlock(&scanner_local_regs_lock);
		update_access_diag(addr, true, 0);
		return 0;
	}

	ret = unit1_reg_wr ? unit1_reg_wr(holding_addr, reg) : -ENODEV;

	update_access_diag(addr, true, ret);
	return ret;
}
