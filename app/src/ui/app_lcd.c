/*
 * Local industrial dashboard for the STM32H747I-DISCO 4-inch LCD.
 * The display controller is owned by the Cortex-M7 board target in Zephyr.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/display.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/reboot.h>

#include "app_lcd.h"
#include "app_modbus_scanner.h"
#include "app_modbus_tcp.h"
#include "modbus_map.h"
#include "net_cfg.h"

LOG_MODULE_REGISTER(app_lcd, LOG_LEVEL_INF);

#define APP_LCD_THREAD_STACK_SIZE 4096
#define APP_LCD_THREAD_PRIORITY 9
#define APP_LCD_REFRESH_MS 500U
#define APP_LCD_SCANNER_ROWS 5U
#define APP_LCD_REBOOT_CONFIRM_MS 3000U
#define APP_LCD_START_IN_STANDBY 1

#if defined(CONFIG_LVGL) && DT_HAS_CHOSEN(zephyr_display)

#include <lvgl.h>

#if DT_HAS_COMPAT_STATUS_OKAY(orisetech_otm8009a)
#define APP_LCD_PANEL_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(orisetech_otm8009a)
#elif DT_HAS_COMPAT_STATUS_OKAY(frida_nt35510)
#define APP_LCD_PANEL_NODE DT_COMPAT_GET_ANY_STATUS_OKAY(frida_nt35510)
#else
#error "Unsupported LCD panel"
#endif

K_THREAD_STACK_DEFINE(app_lcd_stack, APP_LCD_THREAD_STACK_SIZE);
static struct k_thread app_lcd_thread;
static k_tid_t app_lcd_tid;
static struct k_work_delayable reboot_work;
static bool lcd_started;
static bool lcd_sleeping;
static bool lcd_wake_requested;
static uint32_t reboot_confirm_until;
static const struct device *const lcd_panel = DEVICE_DT_GET(APP_LCD_PANEL_NODE);

static lv_obj_t *ip_value;
static lv_obj_t *mode_value;
static lv_obj_t *connections_value;
static lv_obj_t *heartbeat_value;
static lv_obj_t *link_value;
static lv_obj_t *modbus_value;
static lv_obj_t *status_value;
static lv_obj_t *scanner_bars[APP_LCD_SCANNER_ROWS];
static lv_obj_t *scanner_values[APP_LCD_SCANNER_ROWS];
static lv_obj_t *reboot_button;
static lv_obj_t *reboot_label;
static lv_obj_t *sleep_button;
static lv_obj_t *sleep_label;

static void refresh_dashboard(void);
static int enter_standby(void);

static void reboot_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	LOG_WRN("LCD reboot requested");
	sys_reboot(SYS_REBOOT_COLD);
}

static void reset_reboot_button(void)
{
	reboot_confirm_until = 0U;
	lv_label_set_text(reboot_label, "Reboot");
	lv_obj_set_style_bg_color(reboot_button, lv_color_hex(0x263442), 0);
}

static void reboot_button_event(lv_event_t *event)
{
	uint32_t now = k_uptime_get_32();

	ARG_UNUSED(event);

	if (reboot_confirm_until != 0U && (int32_t)(now - reboot_confirm_until) < 0) {
		lv_label_set_text(reboot_label, "Restarting");
		lv_obj_add_state(reboot_button, LV_STATE_DISABLED);
		(void)k_work_reschedule(&reboot_work, K_MSEC(300));
		return;
	}

	reboot_confirm_until = now + APP_LCD_REBOOT_CONFIRM_MS;
	lv_label_set_text(reboot_label, "Confirm reboot");
	lv_obj_set_style_bg_color(reboot_button, lv_color_hex(0xc48628), 0);
}

static int enter_standby(void)
{
	int ret;

	if (lcd_sleeping) {
		return 0;
	}

	ret = display_set_brightness(lcd_panel, 0U);
	if (ret < 0) {
		LOG_WRN("LCD brightness could not be reduced: %d", ret);
	}

	ret = display_blanking_on(lcd_panel);
	if (ret < 0) {
		LOG_ERR("LCD standby failed: %d", ret);
		(void)display_set_brightness(lcd_panel, UINT8_MAX);
		return ret;
	}

	lcd_sleeping = true;
	reset_reboot_button();
	lv_label_set_text(sleep_label, "Sleeping");
	lv_obj_add_state(sleep_button, LV_STATE_DISABLED);
	LOG_INF("LCD entered standby");
	return 0;
}

static void sleep_button_event(lv_event_t *event)
{
	ARG_UNUSED(event);

	(void)enter_standby();
}

static void wake_display_event(lv_event_t *event)
{
	lv_indev_t *indev = lv_event_get_user_data(event);

	if (!lcd_sleeping) {
		return;
	}

	lcd_wake_requested = true;
	lv_indev_wait_release(indev);
}

static void wake_display(void)
{
	int ret;

	lcd_wake_requested = false;
	ret = display_blanking_off(lcd_panel);
	if (ret < 0) {
		LOG_ERR("LCD wake failed: %d", ret);
		return;
	}
	ret = display_set_brightness(lcd_panel, UINT8_MAX);
	if (ret < 0) {
		LOG_WRN("LCD brightness could not be restored: %d", ret);
	}

	lcd_sleeping = false;
	lv_label_set_text(sleep_label, "Sleep");
	lv_obj_clear_state(sleep_button, LV_STATE_DISABLED);
	refresh_dashboard();
	lv_obj_invalidate(lv_screen_active());
	LOG_INF("LCD woke from standby");
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int32_t x, int32_t y,
			    const lv_font_t *font, lv_color_t color)
{
	lv_obj_t *label = lv_label_create(parent);

	lv_label_set_text(label, text);
	lv_obj_set_pos(label, x, y);
	lv_obj_set_style_text_font(label, font, 0);
	lv_obj_set_style_text_color(label, color, 0);
	return label;
}

static lv_obj_t *make_panel(lv_obj_t *parent, int32_t x, int32_t y,
			    int32_t width, int32_t height)
{
	lv_obj_t *panel = lv_obj_create(parent);

	lv_obj_set_pos(panel, x, y);
	lv_obj_set_size(panel, width, height);
	lv_obj_set_style_radius(panel, 6, 0);
	lv_obj_set_style_border_width(panel, 1, 0);
	lv_obj_set_style_border_color(panel, lv_color_hex(0x314154), 0);
	lv_obj_set_style_bg_color(panel, lv_color_hex(0x19232e), 0);
	lv_obj_set_style_pad_all(panel, 12, 0);
	lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
	return panel;
}

static lv_obj_t *make_kpi(lv_obj_t *parent, const char *name, int32_t x)
{
	lv_obj_t *card = make_panel(parent, x, 82, 180, 82);

	make_label(card, name, 0, 0, &lv_font_montserrat_14,
		   lv_color_hex(0x93a4b8));
	return make_label(card, "-", 0, 28, &lv_font_montserrat_20,
			  lv_color_hex(0xf7fafc));
}

static void create_network_panel(lv_obj_t *screen)
{
	lv_obj_t *panel = make_panel(screen, 20, 180, 365, 244);

	make_label(panel, "Network and services", 0, 0, &lv_font_montserrat_20,
		   lv_color_hex(0xf7fafc));
	make_label(panel, "Ethernet link", 0, 50, &lv_font_montserrat_14,
		   lv_color_hex(0x93a4b8));
	link_value = make_label(panel, "-", 235, 50, &lv_font_montserrat_14,
				lv_color_hex(0xf7fafc));
	make_label(panel, "Modbus TCP", 0, 88, &lv_font_montserrat_14,
		   lv_color_hex(0x93a4b8));
	modbus_value = make_label(panel, "Port 502", 235, 88, &lv_font_montserrat_14,
				  lv_color_hex(0x16b67a));
	make_label(panel, "Scanner window", 0, 126, &lv_font_montserrat_14,
		   lv_color_hex(0x93a4b8));
	make_label(panel, "Unit-ID 2", 235, 126, &lv_font_montserrat_14,
		   lv_color_hex(0xf7fafc));
	make_label(panel, "Last command", 0, 164, &lv_font_montserrat_14,
		   lv_color_hex(0x93a4b8));
	status_value = make_label(panel, "-", 235, 164, &lv_font_montserrat_14,
				  lv_color_hex(0xf7fafc));
}

static void create_scanner_panel(lv_obj_t *screen)
{
	lv_obj_t *panel = make_panel(screen, 400, 180, 380, 244);

	make_label(panel, "Process window", 0, 0, &lv_font_montserrat_20,
		   lv_color_hex(0xf7fafc));

	for (uint32_t i = 0; i < APP_LCD_SCANNER_ROWS; i++) {
		int32_t y = 45 + (int32_t)i * 34;
		lv_obj_t *bar;

		lv_obj_t *slot = make_label(panel, "S0", 0, y,
					    &lv_font_montserrat_14,
					    lv_color_hex(0x93a4b8));
		lv_label_set_text_fmt(slot, "S%u", (unsigned int)i);

		bar = lv_bar_create(panel);
		lv_obj_set_pos(bar, 38, y + 2);
		lv_obj_set_size(bar, 245, 12);
		lv_bar_set_range(bar, 0, UINT16_MAX);
		lv_obj_set_style_bg_color(bar, lv_color_hex(0x2b3949), LV_PART_MAIN);
		lv_obj_set_style_bg_color(bar, lv_color_hex(0x2f6fed), LV_PART_INDICATOR);
		lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
		lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
		scanner_bars[i] = bar;

		scanner_values[i] = make_label(panel, "0", 296, y - 2,
					       &lv_font_montserrat_14,
					       lv_color_hex(0xf7fafc));
		lv_obj_set_width(scanner_values[i], 52);
		lv_obj_set_style_text_align(scanner_values[i], LV_TEXT_ALIGN_RIGHT, 0);
	}
}

static void create_dashboard(void)
{
	lv_obj_t *screen = lv_screen_active();

	lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

	make_label(screen, "Industrial Ethernet", 20, 14, &lv_font_montserrat_24,
		   lv_color_hex(0xf7fafc));
	make_label(screen, "Local supervision / STM32H747 / Zephyr", 20, 47,
		   &lv_font_montserrat_14, lv_color_hex(0x93a4b8));
	reboot_button = lv_button_create(screen);
	lv_obj_set_pos(reboot_button, 635, 18);
	lv_obj_set_size(reboot_button, 88, 38);
	lv_obj_set_style_radius(reboot_button, 5, 0);
	lv_obj_set_style_bg_color(reboot_button, lv_color_hex(0x263442), 0);
	lv_obj_set_style_bg_color(reboot_button, lv_color_hex(0x2f6fed), LV_STATE_PRESSED);
	lv_obj_add_event_cb(reboot_button, reboot_button_event, LV_EVENT_CLICKED, NULL);
	reboot_label = lv_label_create(reboot_button);
	lv_label_set_text(reboot_label, "Reboot");
	lv_obj_set_style_text_font(reboot_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(reboot_label, lv_color_hex(0xf7fafc), 0);
	lv_obj_center(reboot_label);
	sleep_button = lv_button_create(screen);
	lv_obj_set_pos(sleep_button, 535, 18);
	lv_obj_set_size(sleep_button, 82, 38);
	lv_obj_set_style_radius(sleep_button, 5, 0);
	lv_obj_set_style_bg_color(sleep_button, lv_color_hex(0x263442), 0);
	lv_obj_set_style_bg_color(sleep_button, lv_color_hex(0x2f6fed), LV_STATE_PRESSED);
	lv_obj_add_event_cb(sleep_button, sleep_button_event, LV_EVENT_CLICKED, NULL);
	sleep_label = lv_label_create(sleep_button);
	lv_label_set_text(sleep_label, "Sleep");
	lv_obj_set_style_text_font(sleep_label, &lv_font_montserrat_14, 0);
	lv_obj_set_style_text_color(sleep_label, lv_color_hex(0xf7fafc), 0);
	lv_obj_center(sleep_label);
	make_label(screen, "M7", 738, 22, &lv_font_montserrat_14,
		   lv_color_hex(0x2fda9a));

	ip_value = make_kpi(screen, "IP address", 20);
	mode_value = make_kpi(screen, "IP mode", 215);
	connections_value = make_kpi(screen, "Modbus clients", 410);
	heartbeat_value = make_kpi(screen, "Heartbeat", 605);

	create_network_panel(screen);
	create_scanner_panel(screen);

	make_label(screen, "HTTP 80", 20, 449, &lv_font_montserrat_14,
		   lv_color_hex(0x708399));
	make_label(screen, "MODBUS TCP 502", 110, 449, &lv_font_montserrat_14,
		   lv_color_hex(0x708399));
}

static void refresh_dashboard(void)
{
	struct net_cfg_data active = { 0 };
	uint16_t status = 0U;

	if (net_cfg_get_active(&active) == 0) {
		lv_label_set_text(ip_value, active.ip);
		lv_label_set_text(mode_value,
				  active.mode == NET_CFG_STATIC ? "Static" : "DHCP");
	}

	lv_label_set_text(link_value, net_cfg_link_is_up() ? "UP" : "DOWN");
	lv_obj_set_style_text_color(link_value,
				    net_cfg_link_is_up() ? lv_color_hex(0x16b67a) :
							   lv_color_hex(0xe05a5a), 0);
	lv_label_set_text_fmt(connections_value, "%u",
			      (unsigned int)app_modbus_tcp_connection_count());
	lv_label_set_text_fmt(heartbeat_value, "%u",
			      (unsigned int)app_modbus_tcp_heartbeat_count());

	if (app_modbus_tcp_holding_read(APP_MB_HREG_STATUS, &status) == 0) {
		lv_label_set_text_fmt(status_value, "%d", (int16_t)status);
	}

	for (uint16_t i = 0; i < APP_LCD_SCANNER_ROWS; i++) {
		uint16_t value = 0U;

		(void)app_modbus_scanner_holding_reg_rd(i, &value);
		lv_bar_set_value(scanner_bars[i], value, LV_ANIM_OFF);
		lv_label_set_text_fmt(scanner_values[i], "%u", (unsigned int)value);
	}
}

static void app_lcd_thread_fn(void *arg1, void *arg2, void *arg3)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	lv_indev_t *indev;
	uint32_t next_refresh = 0U;

	ARG_UNUSED(arg1);
	ARG_UNUSED(arg2);
	ARG_UNUSED(arg3);

	if (!device_is_ready(display)) {
		LOG_ERR("LCD device is not ready");
		return;
	}
	if (!device_is_ready(lcd_panel)) {
		LOG_ERR("LCD panel device is not ready");
		return;
	}

	create_dashboard();
	for (indev = lv_indev_get_next(NULL); indev != NULL;
	     indev = lv_indev_get_next(indev)) {
		lv_indev_add_event_cb(indev, wake_display_event, LV_EVENT_PRESSED, indev);
	}
	refresh_dashboard();
	lv_timer_handler();
	if (APP_LCD_START_IN_STANDBY != 0) {
		(void)enter_standby();
	}

	LOG_INF("Local LCD dashboard started");
	while (true) {
		uint32_t now = k_uptime_get_32();

		if (lcd_wake_requested) {
			wake_display();
		}

		if (reboot_confirm_until != 0U &&
		    (int32_t)(now - reboot_confirm_until) >= 0) {
			reset_reboot_button();
		}

		if (!lcd_sleeping && (int32_t)(now - next_refresh) >= 0) {
			refresh_dashboard();
			next_refresh = now + APP_LCD_REFRESH_MS;
		}
		lv_timer_handler();
		k_msleep(10);
	}
}

int app_lcd_start(void)
{
	if (lcd_started) {
		return 0;
	}

	k_work_init_delayable(&reboot_work, reboot_work_handler);

	app_lcd_tid = k_thread_create(&app_lcd_thread, app_lcd_stack,
				      K_THREAD_STACK_SIZEOF(app_lcd_stack),
				      app_lcd_thread_fn, NULL, NULL, NULL,
				      APP_LCD_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(app_lcd_tid, "app_lcd");
	lcd_started = true;
	return 0;
}

#else

int app_lcd_start(void)
{
	LOG_WRN("LCD disabled: build with SHIELD=st_b_lcd40_dsi1_mb1166");
	return -ENODEV;
}

#endif /* CONFIG_LVGL && DT_HAS_CHOSEN(zephyr_display) */
