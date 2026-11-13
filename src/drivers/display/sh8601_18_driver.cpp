/**
 * @file sh8601_18_driver.cpp
 * @brief SH8601 QSPI AMOLED driver — 1.8" panel variant (368×448)
 *
 * ESP32-C6-Touch-AMOLED-1.8'in panel'i (SH8601) S3 board'una takılı.
 * Aynı QSPI protokolü, sadece resolution + init sequence farklı:
 *   - 368×448 (vs. 2.06" panel: 410×502)
 *   - 8-komut sade init sequence (üretici ESP-IDF örnek kodundan)
 *
 * Mevcut co5300_driver.cpp ile **same function signatures** export eder.
 * Multiple-definition'ı önlemek için TÜM dosya `#ifdef DISPLAY_VARIANT_18INCH`
 * guard'ı içinde: default S3 build'da (`esp32s3_wearable` env) bu dosya
 * boş translation unit'tir → `co5300_*` semboller sadece co5300_driver.cpp'den
 * gelir. `esp32s3_18inch` env'de ise `-DDISPLAY_VARIANT_18INCH=1` ile
 * derlenir, eski driver dosyası src_filter ile devre dışı bırakılır → linker
 * yine tek `co5300_*` set'i görür.
 *
 * Bu sayede `esp32s3_wearable` env'inin platformio.ini config'i değişmez.
 *
 * Pin map S3 board ile aynı (PIN_DISP_*  os_config.h'tan).
 * Resolution CO5300_H_RES / CO5300_V_RES build_flags ile override edilir
 * (env'de -DCO5300_H_RES=368 -DCO5300_V_RES=448).
 */

#ifdef DISPLAY_VARIANT_18INCH

#include "co5300_driver.h"
#include "../../core/os_config.h"
#include "../power/axp2101_driver.h"

#include <esp_lcd_panel_io.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "SH8601_18";

#define BUF_LINE_COUNT  DISPLAY_BUF_LINES
#define BUF_SIZE_BYTES  (BUF_LINE_COUNT * CO5300_H_RES * CO5300_COLOR_SIZE)

/* SH8601 QSPI command encoding — co5300_driver.cpp ile birebir aynı.
 * Panel_io byte-reverse eder → wire order:
 *   SH8601_CMD(c) : [0x02][0x00][c][0x00]   (1-bit SPI command)
 *   SH8601_RAMWR  : [0x32][0x00][0x2C][0x00] (4-bit QSPI pixel data) */
#define SH8601_CMD(c)  ((uint32_t)(0x02000000 | ((uint32_t)(uint8_t)(c) << 8)))
#define SH8601_RAMWR   ((uint32_t)(0x32000000 | ((uint32_t)(0x2C) << 8)))

/* 1.8" panel için offset yok — CASET/RASET 0'dan başlar (üretici örneği).
 * 2.06" panel'de COL_OFFSET=20 vardı (410'u 440 fiziksel VRAM'de ortala). */
#define COL_OFFSET  0
#define ROW_OFFSET  0

static esp_lcd_panel_io_handle_t s_io        = NULL;
static lv_display_t*             s_lvgl_disp = NULL;

// ─── DMA completion ISR ───────────────────────────────────────────────────────

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t* edata,
                                void* user_ctx)
{
    (void)panel_io; (void)edata; (void)user_ctx;
    if (s_lvgl_disp) lv_display_flush_ready(s_lvgl_disp);
    return false;
}

// ─── Init sequence — SH8601 1.8" panel için (üretici ESP-IDF örneği) ───────

typedef struct { uint32_t cmd; const uint8_t* data; uint8_t len; uint16_t delay_ms; } sh_cmd_t;

static const uint8_t d_44[]  = {0x01, 0xD1};   /* Tearing effect line  */
static const uint8_t d_35[]  = {0x00};         /* TE ON, vsync mode    */
static const uint8_t d_53[]  = {0x20};         /* CTRL display: BCTRL  */
static const uint8_t d_51a[] = {0x00};         /* Brightness 0 (boot)  */
/* CASET / RASET payload's are computed at runtime via H_RES/V_RES macros
 * so resolution override (-DCO5300_H_RES=368 -DCO5300_V_RES=448) propagates. */

static const sh_cmd_t s_init_cmds[] = {
    { SH8601_CMD(0x11), NULL,   0, 120 },   /* SLPOUT — 120ms */
    { SH8601_CMD(0x44), d_44,   2, 0   },   /* Tearing line setup */
    { SH8601_CMD(0x35), d_35,   1, 0   },   /* TE ON */
    { SH8601_CMD(0x53), d_53,   1, 10  },   /* BCTRL enable */
    { SH8601_CMD(0x51), d_51a,  1, 10  },   /* Brightness=0 (boot spinner black) */
    { SH8601_CMD(0x29), NULL,   0, 10  },   /* DISPON (görünmez, çünkü brightness=0) */
};

// ─── co5300_init ─────────────────────────────────────────────────────────────

esp_err_t co5300_init(void)
{
    ESP_LOGI(TAG, "Initializing SH8601 QSPI AMOLED 1.8\" (%dx%d)",
             CO5300_H_RES, CO5300_V_RES);

    /* 0. Display power rail (ALDO2) — axp2101_init voltajı set etti ama
     *    enable bit'i set etmedi. Burada açılır. Sync wake'te skip. */
    axp2101_set_rail(AXP_RAIL_LDO2, true);
    vTaskDelay(pdMS_TO_TICKS(10));
    ESP_LOGI(TAG, "Display rail (ALDO2) enabled");

    /* 1. Hardware reset */
    gpio_reset_pin((gpio_num_t)PIN_DISP_RST);
    gpio_set_direction((gpio_num_t)PIN_DISP_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_DISP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)PIN_DISP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 2. SPI bus (QSPI 4-bit, ESP32-S3 board'da SPI2_HOST) */
    spi_bus_config_t bus_cfg = {
        .data0_io_num    = PIN_DISP_D0,
        .data1_io_num    = PIN_DISP_D1,
        .sclk_io_num     = PIN_DISP_SCLK,
        .data2_io_num    = PIN_DISP_D2,
        .data3_io_num    = PIN_DISP_D3,
        .data4_io_num    = -1,
        .data5_io_num    = -1,
        .data6_io_num    = -1,
        .data7_io_num    = -1,
        .max_transfer_sz = BUF_SIZE_BYTES + 64,
        .flags           = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_GPIO_PINS,
        .isr_cpu_id      = ESP_INTR_CPU_AFFINITY_AUTO,
        .intr_flags      = 0,
    };
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 3. Panel IO — quad_mode=1 (QSPI 4-bit pixel data, 1-bit commands).
     *    Bu 1.8" panel'in normal modu; eski co5300_driver.cpp'deki debug
     *    quad_mode=0 hack'i taşınmadı. 40 MHz güvenli QSPI clock. */
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num         = PIN_DISP_CS,
        .dc_gpio_num         = -1,
        .spi_mode            = 0,
        .pclk_hz             = 40 * 1000 * 1000,
        .trans_queue_depth   = 10,
        .on_color_trans_done = on_color_trans_done,
        .user_ctx            = NULL,
        .lcd_cmd_bits        = 32,
        .lcd_param_bits      = 8,
        .flags = {
            .quad_mode = 1,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    /* 4. Send static init commands (SLPOUT, TE, BCTRL, brightness, DISPON) */
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const sh_cmd_t* c = &s_init_cmds[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, (int)c->cmd, c->data, c->len));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    /* 5. Set CASET / RASET to FULL panel — runtime computed from H/V_RES.
     *    Bu sayede -DCO5300_H_RES=368 / -DCO5300_V_RES=448 override propagates. */
    {
        uint16_t xmax = CO5300_H_RES - 1;   /* 367 for 1.8" panel */
        uint16_t ymax = CO5300_V_RES - 1;   /* 447 for 1.8" panel */
        uint8_t caset[4] = { 0x00, 0x00, (uint8_t)(xmax >> 8), (uint8_t)(xmax & 0xFF) };
        uint8_t raset[4] = { 0x00, 0x00, (uint8_t)(ymax >> 8), (uint8_t)(ymax & 0xFF) };
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x2A), caset, 4));
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x2B), raset, 4));
    }

    ESP_LOGI(TAG, "SH8601 1.8\" init OK");

    /* 6. VRAM clear — tüm pikselleri siyah'a yaz (boot artifact önle).
     *    1.8" panel'de offset yok, fiziksel = mantıksal genişlik (368). */
    {
        const int PHYS_W      = CO5300_H_RES;   /* 368 */
        const int CHUNK_LINES = 10;
        const size_t chunk_sz = (size_t)PHYS_W * CHUNK_LINES * CO5300_COLOR_SIZE;
        uint8_t* black = (uint8_t*)heap_caps_calloc(1, chunk_sz,
                                                     MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (black) {
            for (int y = 0; y < CO5300_V_RES; y += CHUNK_LINES) {
                int h    = ((y + CHUNK_LINES) <= CO5300_V_RES) ? CHUNK_LINES : (CO5300_V_RES - y);
                int yend = y + h - 1;
                uint8_t caset[4] = { 0x00, 0x00,
                                     (uint8_t)((PHYS_W - 1) >> 8),
                                     (uint8_t)((PHYS_W - 1) & 0xFF) };
                uint8_t raset[4] = { (uint8_t)(y    >> 8), (uint8_t)(y    & 0xFF),
                                     (uint8_t)(yend >> 8), (uint8_t)(yend & 0xFF) };
                esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x2A), caset, 4);
                esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x2B), raset, 4);
                esp_lcd_panel_io_tx_color(s_io, (int)SH8601_RAMWR, black,
                                          (size_t)PHYS_W * h * CO5300_COLOR_SIZE);
                vTaskDelay(pdMS_TO_TICKS(3));
            }
            heap_caps_free(black);
            ESP_LOGI(TAG, "VRAM cleared: 0..%d x 0..%d", PHYS_W - 1, CO5300_V_RES - 1);
        } else {
            ESP_LOGW(TAG, "VRAM clear skipped: alloc failed");
        }
    }

    return ESP_OK;
}

// ─── LVGL flush callback ─────────────────────────────────────────────────────

void co5300_lvgl_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map)
{
    s_lvgl_disp = disp;

    static bool first = true;
    if (first) {
        first = false;
        ESP_LOGI(TAG, "First flush: x1=%d y1=%d x2=%d y2=%d",
                 area->x1, area->y1, area->x2, area->y2);
    }

    int x1 = area->x1 + COL_OFFSET, x2 = area->x2 + COL_OFFSET;
    int y1 = area->y1 + ROW_OFFSET, y2 = area->y2 + ROW_OFFSET;
    int w  = area->x2 - area->x1 + 1;
    int h  = area->y2 - area->y1 + 1;

    uint8_t caset[4] = {(uint8_t)(x1>>8),(uint8_t)(x1&0xFF),(uint8_t)(x2>>8),(uint8_t)(x2&0xFF)};
    uint8_t raset[4] = {(uint8_t)(y1>>8),(uint8_t)(y1&0xFF),(uint8_t)(y2>>8),(uint8_t)(y2&0xFF)};
    esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x2A), caset, 4);
    esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x2B), raset, 4);

    esp_err_t ret = esp_lcd_panel_io_tx_color(s_io, (int)SH8601_RAMWR,
                                              px_map, (size_t)(w * h * 2));
    if (ret != ESP_OK) ESP_LOGE(TAG, "tx_color failed: 0x%x", ret);
}

// ─── Brightness / Sleep ───────────────────────────────────────────────────────

esp_err_t co5300_set_brightness(uint8_t brightness)
{
    if (!s_io) return ESP_OK;   /* sync wake guard — co5300_init henüz çağrılmadı */
    ESP_LOGI(TAG, "[wake-trace] set_brightness(%u)", (unsigned)brightness);
    return esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x51), &brightness, 1);
}

esp_err_t co5300_sleep(bool enable)
{
    if (!s_io) {
        ESP_LOGW(TAG, "co5300_sleep: SPI io NULL — display unpowered, skipping");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "[wake-trace] sleep(%s)", enable ? "SLPIN" : "SLPOUT");
    esp_err_t ret = esp_lcd_panel_io_tx_param(
        s_io, (int)SH8601_CMD(enable ? 0x10 : 0x11), NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(enable ? 5 : 120));
    return ret;
}

#endif /* DISPLAY_VARIANT_18INCH */
