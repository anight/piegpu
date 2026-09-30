/*
 * app_main.c - ESP-IDF's entry point: the app's main () (renamed
 * pgpu_app_main by the build, main/CMakeLists.txt), on core 0 with the main
 * task's stack (sdkconfig.defaults). The link's reply parser runs on core 1.
 * The console's UART gets its driver, so that the apps can read keys (stdin;
 * without the driver, reads return nothing).
 */
#include <stdio.h>
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "sdcard.h"

int pgpu_app_main (void);

void app_main (void)
{
	printf ("pgpu: ESP32-P4 host\n");
	if (uart_driver_install (CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, NULL, 0) == ESP_OK)
	{
		uart_vfs_dev_use_driver (CONFIG_ESP_CONSOLE_UART_NUM);
	}
#ifdef PGPU_SDCARD
	sdcard_mount (PGPU_SDCARD);		/* the app's files (PGPU_SDCARD: where) */
#endif
	pgpu_app_main ();
}
