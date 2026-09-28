/*
 * sdcard.c - the dev kit's microSD slot, FAT at /sdcard (ESP-IDF's VFS: open
 * files with fopen). From the board's schematic: the P4's SDMMC slot 0 on its
 * own pins (CLK 43, CMD 44, D0-D3 39-42, 4 bits, 51K pull-ups); the card's
 * supply SD1_VDD comes from the P4's LDO channel 4 through a P-MOSFET whose
 * gate is GPIO45 (low: on).
 */
#include "sdcard.h"
#include <stdio.h>
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"


#define PIN_SD_POWER	GPIO_NUM_45		/* low: the card has power */
#define LDO_SD		4			/* the P4's LDO channel for SD1_VDD */

bool sdcard_mount (const char *mount_point)
{
	gpio_config_t power = {
		.pin_bit_mask = 1ull << PIN_SD_POWER,
		.mode = GPIO_MODE_OUTPUT,
	};
	gpio_config (&power);
	gpio_set_level (PIN_SD_POWER, 0);

	sd_pwr_ctrl_ldo_config_t ldo = {.ldo_chan_id = LDO_SD};
	sd_pwr_ctrl_handle_t pwr = NULL;
	if (sd_pwr_ctrl_new_on_chip_ldo (&ldo, &pwr) != ESP_OK)
	{
		printf ("sdcard: no LDO %d\n", LDO_SD);
		return false;
	}

	sdmmc_host_t host = SDMMC_HOST_DEFAULT ();
	host.slot = SDMMC_HOST_SLOT_0;
	host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;	/* 40 MHz */
	host.pwr_ctrl_handle = pwr;
	sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT ();
	slot.width = 4;

	esp_vfs_fat_sdmmc_mount_config_t mount = {
		.format_if_mount_failed = false,
		.max_files = 4,
		.allocation_unit_size = 16 * 1024,
	};
	sdmmc_card_t *card;
	esp_err_t e = esp_vfs_fat_sdmmc_mount (mount_point, &host, &slot, &mount, &card);
	if (e != ESP_OK)
	{
		printf ("sdcard: can't mount (%s)\n", esp_err_to_name (e));
		return false;
	}
	printf ("sdcard: %s, %llu MB, %u kHz, FAT at %s\n", card->cid.name,
		(unsigned long long) card->csd.capacity * card->csd.sector_size / (1024 * 1024),
		(unsigned) card->real_freq_khz, mount_point);
	return true;
}
