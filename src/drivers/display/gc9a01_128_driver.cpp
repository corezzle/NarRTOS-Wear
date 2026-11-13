/**
 * @file gc9a01_128_driver.cpp
 * @brief GC9A01A SPI LCD driver — ESP32-S3-Touch-LCD-1.28 (240×240 yuvarlak)
 *
 * Board varyantı: `esp32s3_lcd128` env (-DDISPLAY_VARIANT_LCD128).
 * Mevcut co5300_driver.cpp ile **aynı fonksiyon imzalarını** (co5300_*) export
 * eder — sh8601_18_driver.cpp'deki varyant kalıbının aynısı:
 *   - Bu dosyanın TAMAMI `#ifdef DISPLAY_VARIANT_LCD128` guard'ı içinde →
 *     default AMOLED build'ında boş translation unit.
 *   - co5300_driver.cpp guard'ı LCD128 build'ında onu boşaltır → linker her
 *     env'de tek `co5300_*` sembol seti görür.
 *   - main.cpp / ui_service değişmez: co5300_init() burada GC9A01'i başlatır.
 *
 * AMOLED sürücüsünden temel farklar:
 *   - 1-bit standart SPI (QSPI değil) + ayrı DC pini → komutlar düz 8-bit.
 *   - Parlaklık panel komutuyla değil BACKLIGHT PWM'iyle (LEDC, PIN_DISP_BL).
 *   - AXP2101 rail yönetimi YOK (board'da PMIC yok) — panel 3V3'ten besleniyor.
 *   - Init sequence: GC9A01A vendor init (esp_lcd_gc9a01 örneğinden).
 *
 * Çözünürlük CO5300_H_RES/V_RES build flag'lerinden gelir (env: 240×240).
 */

#ifdef DISPLAY_VARIANT_LCD128

#include "co5300_driver.h"
#include "../../core/os_config.h"

#include <esp_lcd_panel_io.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <driver/ledc.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static bool s_ready = false;   /* co5300_init (GC9A01A) bitti */
static const char* TAG = "GC9A01_128";

#define BUF_LINE_COUNT  DISPLAY_BUF_LINES
#define BUF_SIZE_BYTES  (BUF_LINE_COUNT * CO5300_H_RES * CO5300_COLOR_SIZE)

/* GC9A01A MIPI-DCS komutları — 8-bit düz SPI (DC pini data/command ayırır) */
#define GC_CMD_SLPIN    0x10
#define GC_CMD_SLPOUT   0x11
#define GC_CMD_INVON    0x21
#define GC_CMD_DISPOFF  0x28
#define GC_CMD_DISPON   0x29
#define GC_CMD_CASET    0x2A
#define GC_CMD_RASET    0x2B
#define GC_CMD_RAMWR    0x2C

/* Backlight PWM — LEDC low-speed timer, 8-bit duty (0-255 = brightness). */
#define BL_LEDC_TIMER    LEDC_TIMER_1     /* TIMER_0 audio_service'in (buzzer) */
#define BL_LEDC_CHANNEL  LEDC_CHANNEL_1
#define BL_LEDC_FREQ_HZ  5000

static esp_lcd_panel_io_handle_t s_io        = NULL;
static lv_display_t*             s_lvgl_disp = NULL;
static bool                      s_bl_ready  = false;

// ─── DMA completion ISR ───────────────────────────────────────────────────────

static bool on_color_trans_done(esp_lcd_panel_io_handle_t panel_io,
                                esp_lcd_panel_io_event_data_t* edata,
                                void* user_ctx)
{
    (void)panel_io; (void)edata; (void)user_ctx;
    if (s_lvgl_disp) lv_display_flush_ready(s_lvgl_disp);
    return false;
}

// ─── GC9A01A vendor init sequence ────────────────────────────────────────────
// Kaynak: espressif/esp_lcd_gc9a01 component vendor_specific_init tablosu
// (üretici örnek kodları da aynı diziyi kullanıyor). Undocumented register
// blokları (0x80'ler, 0x60'lar, gamma F0-F3) panel üreticisinin kalibrasyonu —
// olduğu gibi taşınır.

typedef struct { uint8_t cmd; const uint8_t* data; uint8_t len; uint16_t delay_ms; } gc_cmd_t;

static const uint8_t d_eb[]  = {0x14};
static const uint8_t d_84[]  = {0x40};
static const uint8_t d_85[]  = {0xFF};
static const uint8_t d_86[]  = {0xFF};
static const uint8_t d_87[]  = {0xFF};
static const uint8_t d_88[]  = {0x0A};
static const uint8_t d_89[]  = {0x21};
static const uint8_t d_8a[]  = {0x00};
static const uint8_t d_8b[]  = {0x80};
static const uint8_t d_8c[]  = {0x01};
static const uint8_t d_8d[]  = {0x01};
static const uint8_t d_8e[]  = {0xFF};
static const uint8_t d_8f[]  = {0xFF};
static const uint8_t d_b6[]  = {0x00, 0x00};
static const uint8_t d_36[]  = {0x48};   /* MADCTL: MX|BGR — cihaz testi 2026-07-13:
                                          * 0x08 ile görüntü yatay AYNALI çıktı (yazılar
                                          * ters okunuyor). MX (0x40) panel tarama yönünü
                                          * çevirip düzeltir. */
static const uint8_t d_3a[]  = {0x05};   /* COLMOD: 16bpp RGB565 */
static const uint8_t d_90[]  = {0x08, 0x08, 0x08, 0x08};
static const uint8_t d_bd[]  = {0x06};
static const uint8_t d_bc[]  = {0x00};
static const uint8_t d_ff[]  = {0x60, 0x01, 0x04};
static const uint8_t d_c3[]  = {0x13};
static const uint8_t d_c4[]  = {0x13};
static const uint8_t d_c9[]  = {0x22};
static const uint8_t d_be[]  = {0x11};
static const uint8_t d_e1[]  = {0x10, 0x0E};
static const uint8_t d_df[]  = {0x21, 0x0C, 0x02};
static const uint8_t d_f0[]  = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A};
static const uint8_t d_f1[]  = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F};
static const uint8_t d_f2[]  = {0x45, 0x09, 0x08, 0x08, 0x26, 0x2A};
static const uint8_t d_f3[]  = {0x43, 0x70, 0x72, 0x36, 0x37, 0x6F};
static const uint8_t d_ed[]  = {0x1B, 0x0B};
static const uint8_t d_ae[]  = {0x77};
static const uint8_t d_cd[]  = {0x63};
static const uint8_t d_70[]  = {0x07, 0x07, 0x04, 0x0E, 0x0F, 0x09, 0x07, 0x08, 0x03};
static const uint8_t d_e8[]  = {0x34};
static const uint8_t d_62[]  = {0x18, 0x0D, 0x71, 0xED, 0x70, 0x70,
                                0x18, 0x0F, 0x71, 0xEF, 0x70, 0x70};
static const uint8_t d_63[]  = {0x18, 0x11, 0x71, 0xF1, 0x70, 0x70,
                                0x18, 0x13, 0x71, 0xF3, 0x70, 0x70};
static const uint8_t d_64[]  = {0x28, 0x29, 0xF1, 0x01, 0xF1, 0x00, 0x07};
static const uint8_t d_66[]  = {0x3C, 0x00, 0xCD, 0x67, 0x45, 0x45, 0x10, 0x00, 0x00, 0x00};
static const uint8_t d_67[]  = {0x00, 0x3C, 0x00, 0x00, 0x00, 0x01, 0x54, 0x10, 0x32, 0x98};
static const uint8_t d_74[]  = {0x10, 0x85, 0x80, 0x00, 0x00, 0x4E, 0x00};
static const uint8_t d_98[]  = {0x3E, 0x07};
static const uint8_t d_35[]  = {0x00};   /* TE ON, vsync mode */

static const gc_cmd_t s_init_cmds[] = {
    { 0xEF, NULL,  0, 0 },
    { 0xEB, d_eb,  1, 0 },
    { 0xFE, NULL,  0, 0 },
    { 0xEF, NULL,  0, 0 },
    { 0xEB, d_eb,  1, 0 },
    { 0x84, d_84,  1, 0 },
    { 0x85, d_85,  1, 0 },
    { 0x86, d_86,  1, 0 },
    { 0x87, d_87,  1, 0 },
    { 0x88, d_88,  1, 0 },
    { 0x89, d_89,  1, 0 },
    { 0x8A, d_8a,  1, 0 },
    { 0x8B, d_8b,  1, 0 },
    { 0x8C, d_8c,  1, 0 },
    { 0x8D, d_8d,  1, 0 },
    { 0x8E, d_8e,  1, 0 },
    { 0x8F, d_8f,  1, 0 },
    { 0xB6, d_b6,  2, 0 },
    { 0x36, d_36,  1, 0 },
    { 0x3A, d_3a,  1, 0 },
    { 0x90, d_90,  4, 0 },
    { 0xBD, d_bd,  1, 0 },
    { 0xBC, d_bc,  1, 0 },
    { 0xFF, d_ff,  3, 0 },
    { 0xC3, d_c3,  1, 0 },
    { 0xC4, d_c4,  1, 0 },
    { 0xC9, d_c9,  1, 0 },
    { 0xBE, d_be,  1, 0 },
    { 0xE1, d_e1,  2, 0 },
    { 0xDF, d_df,  3, 0 },
    { 0xF0, d_f0,  6, 0 },
    { 0xF1, d_f1,  6, 0 },
    { 0xF2, d_f2,  6, 0 },
    { 0xF3, d_f3,  6, 0 },
    { 0xED, d_ed,  2, 0 },
    { 0xAE, d_ae,  1, 0 },
    { 0xCD, d_cd,  1, 0 },
    { 0x70, d_70,  9, 0 },
    { 0xE8, d_e8,  1, 0 },
    { 0x62, d_62, 12, 0 },
    { 0x63, d_63, 12, 0 },
    { 0x64, d_64,  7, 0 },
    { 0x66, d_66, 10, 0 },
    { 0x67, d_67, 10, 0 },
    { 0x74, d_74,  7, 0 },
    { 0x98, d_98,  2, 0 },
    { 0x35, d_35,  1, 0 },              /* TE ON */
    { GC_CMD_INVON,  NULL, 0, 0   },    /* GC9A01: inversion ON şart */
    { GC_CMD_SLPOUT, NULL, 0, 120 },
    { GC_CMD_DISPON, NULL, 0, 20  },    /* Görünmez — backlight henüz kapalı */
};

// ─── Backlight (LEDC PWM) ────────────────────────────────────────────────────

static esp_err_t backlight_init(void)
{
    ledc_timer_config_t timer_cfg = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num       = BL_LEDC_TIMER,
        .freq_hz         = BL_LEDC_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
        .deconfigure     = false,
    };
    esp_err_t ret = ledc_timer_config(&timer_cfg);
    if (ret != ESP_OK) return ret;

    ledc_channel_config_t ch_cfg = {
        .gpio_num   = PIN_DISP_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .intr_type  = LEDC_INTR_DISABLE,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,                 /* Boot: kapalı (garbage flash önle) */
        .hpoint     = 0,
        .sleep_mode = LEDC_SLEEP_MODE_NO_ALIVE_NO_PD,
        .flags      = { .output_invert = 0 },
    };
    ret = ledc_channel_config(&ch_cfg);
    if (ret == ESP_OK) s_bl_ready = true;
    return ret;
}

static void backlight_set(uint8_t level)
{
    if (!s_bl_ready) return;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL, level);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CHANNEL);
}

// ─── co5300_init ─────────────────────────────────────────────────────────────

esp_err_t co5300_init(void)
{
    ESP_LOGI(TAG, "Initializing GC9A01A SPI LCD 1.28\" (%dx%d)",
             CO5300_H_RES, CO5300_V_RES);

    /* 1. Backlight PWM — duty=0 ile başlar; ilk frame flush'lanana kadar
     *    panel karanlık kalır (AMOLED'deki brightness=0 boot kalıbının
     *    LCD karşılığı). co5300_set_brightness(255) main.cpp'den gelir. */
    ESP_ERROR_CHECK(backlight_init());

    /* 2. Hardware reset */
    gpio_reset_pin((gpio_num_t)PIN_DISP_RST);
    gpio_set_direction((gpio_num_t)PIN_DISP_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_DISP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)PIN_DISP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    /* 3. SPI bus — 1-bit standart SPI (MOSI+SCLK), SPI2_HOST + DMA */
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num     = PIN_DISP_MOSI;
    bus_cfg.miso_io_num     = -1;
    bus_cfg.sclk_io_num     = PIN_DISP_SCLK;
    bus_cfg.quadwp_io_num   = -1;
    bus_cfg.quadhd_io_num   = -1;
    bus_cfg.max_transfer_sz = BUF_SIZE_BYTES + 64;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    /* 4. Panel IO — DC pini ile 8-bit komut/parametre; 40 MHz pclk
     *    (GC9A01A datasheet 80 MHz'e kadar; 40 jumper'sız PCB için güvenli). */
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num         = PIN_DISP_CS;
    io_cfg.dc_gpio_num         = PIN_DISP_DC;
    io_cfg.spi_mode            = 0;
    io_cfg.pclk_hz             = 40 * 1000 * 1000;
    io_cfg.trans_queue_depth   = 10;
    io_cfg.on_color_trans_done = on_color_trans_done;
    io_cfg.user_ctx            = NULL;
    io_cfg.lcd_cmd_bits        = 8;
    io_cfg.lcd_param_bits      = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    /* 5. Vendor init sequence + SLPOUT + DISPON */
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const gc_cmd_t* c = &s_init_cmds[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, c->cmd, c->data, c->len));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    ESP_LOGI(TAG, "GC9A01A init OK");

    /* 6. VRAM clear — panel RAM'deki boot garbage'ını siyahla doldur.
     *    Backlight kapalıyken yapılır; BL açıldığında temiz ekran görünür. */
    {
        const int CHUNK_LINES = 20;
        const size_t chunk_sz = (size_t)CO5300_H_RES * CHUNK_LINES * CO5300_COLOR_SIZE;
        uint8_t* black = (uint8_t*)heap_caps_calloc(1, chunk_sz,
                                                    MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        if (black) {
            for (int y = 0; y < CO5300_V_RES; y += CHUNK_LINES) {
                int h    = ((y + CHUNK_LINES) <= CO5300_V_RES) ? CHUNK_LINES : (CO5300_V_RES - y);
                int yend = y + h - 1;
                uint8_t caset[4] = { 0x00, 0x00,
                                     (uint8_t)((CO5300_H_RES - 1) >> 8),
                                     (uint8_t)((CO5300_H_RES - 1) & 0xFF) };
                uint8_t raset[4] = { (uint8_t)(y    >> 8), (uint8_t)(y    & 0xFF),
                                     (uint8_t)(yend >> 8), (uint8_t)(yend & 0xFF) };
                esp_lcd_panel_io_tx_param(s_io, GC_CMD_CASET, caset, 4);
                esp_lcd_panel_io_tx_param(s_io, GC_CMD_RASET, raset, 4);
                esp_lcd_panel_io_tx_color(s_io, GC_CMD_RAMWR, black,
                                          (size_t)CO5300_H_RES * h * CO5300_COLOR_SIZE);
                vTaskDelay(pdMS_TO_TICKS(2));
            }
            heap_caps_free(black);
            ESP_LOGI(TAG, "VRAM cleared: %dx%d", CO5300_H_RES, CO5300_V_RES);
        } else {
            ESP_LOGW(TAG, "VRAM clear skipped: alloc failed");
        }
    }

    s_ready = true;
    return ESP_OK;
}

bool co5300_is_ready(void) { return s_ready; }

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

    int x1 = area->x1, x2 = area->x2;
    int y1 = area->y1, y2 = area->y2;
    int w  = x2 - x1 + 1;
    int h  = y2 - y1 + 1;

    uint8_t caset[4] = {(uint8_t)(x1>>8),(uint8_t)(x1&0xFF),(uint8_t)(x2>>8),(uint8_t)(x2&0xFF)};
    uint8_t raset[4] = {(uint8_t)(y1>>8),(uint8_t)(y1&0xFF),(uint8_t)(y2>>8),(uint8_t)(y2&0xFF)};
    esp_lcd_panel_io_tx_param(s_io, GC_CMD_CASET, caset, 4);
    esp_lcd_panel_io_tx_param(s_io, GC_CMD_RASET, raset, 4);

    esp_err_t ret = esp_lcd_panel_io_tx_color(s_io, GC_CMD_RAMWR,
                                              px_map, (size_t)(w * h * 2));
    if (ret != ESP_OK) ESP_LOGE(TAG, "tx_color failed: 0x%x", ret);
}

// ─── Brightness / Sleep ───────────────────────────────────────────────────────

esp_err_t co5300_set_brightness(uint8_t brightness)
{
    /* LCD'de parlaklık panel komutu değil backlight PWM'i. main.cpp'nin
     * brightness=0 → içerik flush → brightness=255 boot kalıbı aynen çalışır:
     * BL kapalıyken panel RAM'e spinner çizilir, sonra BL açılır. */
    ESP_LOGI(TAG, "[wake-trace] set_brightness(%u) → backlight PWM", (unsigned)brightness);
    backlight_set(brightness);
    return ESP_OK;
}

esp_err_t co5300_sleep(bool enable)
{
    if (!s_io) {
        ESP_LOGW(TAG, "co5300_sleep: SPI io NULL — display unpowered, skipping");
        return ESP_OK;
    }
    ESP_LOGI(TAG, "[wake-trace] sleep(%s)", enable ? "SLPIN" : "SLPOUT");
    backlight_set(enable ? 0 : 255);   /* BL'siz LCD içeriği zaten görünmez */
    esp_err_t ret = esp_lcd_panel_io_tx_param(
        s_io, enable ? GC_CMD_SLPIN : GC_CMD_SLPOUT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(enable ? 5 : 120));
    return ret;
}

#endif /* DISPLAY_VARIANT_LCD128 */
