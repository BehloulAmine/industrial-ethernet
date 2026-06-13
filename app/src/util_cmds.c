/*
 * Small shell utilities for interactive bring-up and diagnostics.
 */

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

static int cmd_reboot(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Rebooting...");
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

static int cmd_uptime(const struct shell *sh, size_t argc, char **argv)
{
	int64_t uptime_ms;
	int64_t seconds;
	int64_t milliseconds;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uptime_ms = k_uptime_get();
	seconds = uptime_ms / 1000;
	milliseconds = uptime_ms % 1000;

	shell_print(sh, "Uptime: %lld.%03lld s", seconds, milliseconds);

	return 0;
}

SHELL_CMD_REGISTER(reboot, NULL, "Reboot the board", cmd_reboot);
SHELL_CMD_REGISTER(uptime, NULL, "Show system uptime", cmd_uptime);
