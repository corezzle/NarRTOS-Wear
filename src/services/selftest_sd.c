/**
 * selftest_sd.c — SD kart testi (SDMMC 1-bit, pinler os_config.h).
 *
 * Kart yoksa bağlanma zaman aşımına düşer → SKIP. Kart var ama FAT yoksa
 * FAIL (kart okundu ama biçimlendirilmemiş). Kart asla biçimlendirilmez.
 * Test bitince kart ayrılır; OS başka yerde SD kullanmıyor.
 */
#include "selftest_sd.h"
#include "../core/os_config.h"

#include <stdio.h>
#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char* TAG = "SelftestSD";

int selftest_sd_run(char* detail, size_t n)
{
    if (PIN_SD_CLK < 0 || PIN_SD_CMD < 0 || PIN_SD_D0 < 0) {
        snprintf(detail, n, "bu kartta SD yuvasi yok");
        return 2;
    }

    esp_vfs_fat_sdmmc_mount_config_t mc = {
        .format_if_mount_failed = false,   /* kullanıcının kartını ASLA biçimlendirme */
        .max_files = 2,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)PIN_SD_CLK;
    slot.cmd = (gpio_num_t)PIN_SD_CMD;
    slot.d0  = (gpio_num_t)PIN_SD_D0;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    sdmmc_card_t* card = NULL;
    esp_err_t e = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mc, &card);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "mount: %s", esp_err_to_name(e));
        if (e == ESP_ERR_TIMEOUT || e == ESP_ERR_NOT_FOUND ||
            e == ESP_ERR_INVALID_RESPONSE || e == ESP_ERR_INVALID_CRC) {
            snprintf(detail, n, "kart yok / cevap yok (%s)", esp_err_to_name(e));
            return 2;
        }
        if (e == ESP_FAIL) {
            snprintf(detail, n, "kart var ama FAT bolumu yok");
            return 1;
        }
        snprintf(detail, n, "baglanamadi (%s)", esp_err_to_name(e));
        return 1;
    }

    const char* path = "/sdcard/nrtst.txt";
    const char* msg  = "narrtos-selftest";
    unsigned long long mb =
        ((unsigned long long)card->csd.capacity * (unsigned long long)card->csd.sector_size) /
        (1024ULL * 1024ULL);

    int ret = 1;
    FILE* f = fopen(path, "w");
    if (!f) {
        snprintf(detail, n, "%llu MB, dosya yazilamadi (salt okunur?)", mb);
    } else {
        fputs(msg, f);
        fclose(f);
        char buf[32] = {0};
        f = fopen(path, "r");
        if (!f) {
            snprintf(detail, n, "%llu MB, yazilan dosya okunamadi", mb);
        } else {
            if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
            fclose(f);
            if (strcmp(buf, msg) == 0) {
                snprintf(detail, n, "%.8s %llu MB, yaz/oku OK", card->cid.name, mb);
                ret = 0;
            } else {
                snprintf(detail, n, "%llu MB, okunan veri farkli", mb);
            }
        }
        remove(path);
    }
    esp_vfs_fat_sdcard_unmount("/sdcard", card);
    return ret;
}
