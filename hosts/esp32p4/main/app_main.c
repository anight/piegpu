/*
 * app_main.c - ESP-IDF's entry point: the app's main () (renamed
 * pgpu_app_main by the build, main/CMakeLists.txt), on core 0 with the main
 * task's stack (sdkconfig.defaults). The link's reply parser runs on core 1.
 */
#include <stdio.h>

int pgpu_app_main (void);

void app_main (void)
{
	printf ("pgpu: ESP32-P4 host\n");
	pgpu_app_main ();
}
