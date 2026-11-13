/**
 * @file co5300_driver.cpp
 * @brief CO5300/SH8601 QSPI AMOLED driver — panel_io (1-bit cmd, 4-bit data)
 *
 * Protocol analysis (from esp_lcd_panel_io_spi.c source):
 *   quad_mode=1 adds SPI_TRANS_MODE_QIO ONLY to the pixel-data transaction.
 *   Commands (tx_param) are always sent in 1-bit SPI — no MULTILINE_CMD/ADDR.
 *   So SH8601 receives:
 *     - Init commands : 1-bit SPI  [0x02][0x00][cmd][0x00]
 *     - Pixel data    : 4-bit QSPI [pixel bytes]
 *
 * v1–v3 used raw spi_device_polling_transmit with SPI_TRANS_MODE_QIO for ALL
 * bytes (commands + data).  SH8601 expects commands in 1-bit SPI; it likely
 * ignored them all → Display ON never executed → black screen.
 *
 * v4 (this file): back to esp_lcd_panel_io_spi with quad_mode=1.
 *   Command format: SH8601_CMD(c) puts cmd at address[15:8] of the 32-bit
 *   word.  After panel_io's byte-reversal, wire order is [0x02][0x00][cmd][0x00].
 *   This matches the SH8601 datasheet encoding per the vendor BSP comment.
 *
 * LVGL flush: lv_display_flush_ready() called synchronously after tx_color.
 *   on_color_trans_done ISR also calls it; the second call is a no-op.
 */

/* v1.2.9 — Display variant guard. 1.8" panel build (`esp32s3_18inch` env)
 * `-DDISPLAY_VARIANT_18INCH=1` ile derlerken bu dosya boş translation unit
 * olur; sh8601_18_driver.cpp aynı `co5300_*` semboller seti'ni sağlar →
 * linker tek tanım görür. Default S3 env'de DISPLAY_VARIANT_18INCH tanımsız
 * olduğu için dosya **olduğu gibi** derlenir, davranış değişmez.
 * v1.2.9b: aynı kalıp esp32s3_lcd128 env'i (GC9A01 1.28" LCD board,
 * gc9a01_128_driver.cpp) için de geçerli — guard'a LCD128 eklendi. */
#if !defined(DISPLAY_VARIANT_18INCH) && !defined(DISPLAY_VARIANT_LCD128)

#include "co5300_driver.h"
#include "../../core/os_config.h"
#include "../power/axp2101_driver.h"  // ALDO2 enable — sync wake'te skip için

#include <esp_lcd_panel_io.h>
#include <driver/spi_master.h>
#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char* TAG = "CO5300";

#define BUF_LINE_COUNT  DISPLAY_BUF_LINES
#define BUF_SIZE_BYTES  (BUF_LINE_COUNT * CO5300_H_RES * CO5300_COLOR_SIZE)

// SH8601 32-bit QSPI command encoding (from vendor BSP protocol analysis).
// Panel_io reverses bytes before sending → wire order:
//
//   SH8601_CMD(c)  : [0x02][0x00][c][0x00]  — standard write (1-bit SPI)
//                    opcode 0x02 = SPI Page Program (1-bit data follows)
//
//   SH8601_RAMWR   : [0x32][0x00][0x2C][0x00] — pixel write (4-bit QSPI data)
//                    opcode 0x32 = Quad Input Page Program → chip expects
//                    subsequent data on all 4 QSPI lines simultaneously.
//                    This is why pixel data goes through quad_mode path.
#define SH8601_CMD(c)  ((uint32_t)(0x02000000 | ((uint32_t)(uint8_t)(c) << 8)))
/* Session 46h teşhis: CO5300_QUAD_PIXELS=0 → pikseller de 1 hattan (opcode 0x02),
 * GPIO5/6/7 hiç sürülmez. NAR'da IMU yalnız bu moddayken bulunuyor (A/B). */
#ifndef CO5300_QUAD_PIXELS
#define CO5300_QUAD_PIXELS 1
#endif
#if CO5300_QUAD_PIXELS
#define SH8601_RAMWR   ((uint32_t)(0x32000000 | ((uint32_t)(0x2C) << 8)))  // 0x32002C00
#else
#define SH8601_RAMWR   ((uint32_t)(0x02000000 | ((uint32_t)(0x2C) << 8)))  // 1-bit RAMWR
#endif

#define COL_OFFSET  20   // Center 410px in 440px VRAM (fine-tuned)
#define ROW_OFFSET  0

static esp_lcd_panel_io_handle_t s_io       = NULL;
static bool                      s_panel_ok  = false;   /* [panel-id] sonucu */
static bool                      s_ready     = false;   /* co5300_init bitti */
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

// ─── Init sequence ────────────────────────────────────────────────────────────

typedef struct { uint32_t cmd; const uint8_t* data; uint8_t len; uint16_t delay_ms; } sh_cmd_t;

static const uint8_t d_fe[]  = {0x00};
static const uint8_t d_3a[]  = {0x55};
static const uint8_t d_c4[]  = {0x80};
static const uint8_t d_44[]  = {0x01, 0xD1};
static const uint8_t d_35[]  = {0x00};
static const uint8_t d_53[]  = {0x20};
static const uint8_t d_63[]  = {0xFF};
static const uint8_t d_51a[] = {0x00};
static const uint8_t d_2a[]  = {0x00, 0x00, 0x01, 0x99};
static const uint8_t d_2b[]  = {0x00, 0x00, 0x01, 0xF5};
static const uint8_t d_51b[] = {0xFF};

/* Brightness ramp-up (0x51 d_51b=0xFF) init sequence'in SONUNDAN ÇIKARILDI.
 * Sebep: DISPON (0x29) sonrası panel internal RAM rastgele içerik (genelde
 * yeşil bar) gösteriyor; brightness=255 ramp-up bunu görünür yapıyordu →
 * "wake sırasında yeşil flash" (kullanıcı raporu 2026-05-04). Şimdi init
 * brightness=0 ile bitiyor (DISPON yapılır ama görünmez); ui_service_init
 * sonrasında ilk LVGL frame'i flush edilir, SONRA boot kodu manuel olarak
 * co5300_set_brightness(255) çağırır → panel RAM'de garbage yerine bizim
 * frame görünür. */
static const sh_cmd_t s_init_cmds[] = {
    { SH8601_CMD(0x11), NULL,   0, 120 },
    { SH8601_CMD(0xFE), d_fe,   1, 0   },   /* CO5300: kullanıcı komut sayfası (page 0) */
    { SH8601_CMD(0x3A), d_3a,   1, 0   },
    { SH8601_CMD(0xC4), d_c4,   1, 0   },
    { SH8601_CMD(0x44), d_44,   2, 0   },
    { SH8601_CMD(0x35), d_35,   1, 0   },
    { SH8601_CMD(0x53), d_53,   1, 10  },
    { SH8601_CMD(0x63), d_63,   1, 10  },
    { SH8601_CMD(0x51), d_51a,  1, 10  },   /* brightness=0 (black-out) */
    { SH8601_CMD(0x2A), d_2a,   4, 0   },
    { SH8601_CMD(0x2B), d_2b,   4, 0   },
    { SH8601_CMD(0x29), NULL,   0, 10  },   /* DISPON (görünmez, çünkü brightness=0) */
};

// ─── Panel okuma teşhisi (Session 46f) ───────────────────────────────────────
/* Ana io oluşturulmadan ÖNCE geçici io'larla panel kimliği okunur (aynı CS
 * pinini iki cihaz aynı anda paylaşmasın diye). Varyantlar:
 *   - 1 MHz, cevap D1 hattından (standart 4-telli SPI, MISO=GPIO D1)
 *   - 1 MHz, cevap D0 hattından (sio_mode: tek hat, 3-telli okuma)
 * Hepsi FF/00 → panel hiçbir hattan cevap vermiyor. */
static void panel_id_probe(const char* label, int hz, bool sio)
{
    esp_lcd_panel_io_spi_config_t cfg = {};
    cfg.cs_gpio_num       = PIN_DISP_CS;
    cfg.dc_gpio_num       = -1;
    cfg.spi_mode          = 0;
    cfg.pclk_hz           = hz;
    cfg.trans_queue_depth = 2;
    cfg.lcd_cmd_bits      = 32;
    cfg.lcd_param_bits    = 8;
    cfg.flags.sio_mode    = sio ? 1 : 0;
    esp_lcd_panel_io_handle_t io = NULL;
    if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &cfg, &io) != ESP_OK) {
        ESP_LOGW(TAG, "[panel-diag] %s: io olusturulamadi", label);
        return;
    }
    uint8_t id04[3] = {0}, pm = 0, da = 0, db = 0, dc = 0;
    esp_err_t e = esp_lcd_panel_io_rx_param(io, (int)(0x03000000 | (0x04 << 8)), id04, 3);
    esp_lcd_panel_io_rx_param(io, (int)(0x03000000 | (0x0A << 8)), &pm, 1);
    esp_lcd_panel_io_rx_param(io, (int)(0x03000000 | (0xDA << 8)), &da, 1);
    esp_lcd_panel_io_rx_param(io, (int)(0x03000000 | (0xDB << 8)), &db, 1);
    esp_lcd_panel_io_rx_param(io, (int)(0x03000000 | (0xDC << 8)), &dc, 1);
    bool dead = ((id04[0] & id04[1] & id04[2] & pm & da & db & dc) == 0xFF) ||
                ((id04[0] | id04[1] | id04[2] | pm | da | db | dc) == 0x00);
    ESP_LOGW(TAG, "[panel-diag] %-22s 04h=%02X %02X %02X 0Ah=%02X DA/DB/DC=%02X %02X %02X (%s) -> %s",
             label, id04[0], id04[1], id04[2], pm, da, db, dc, esp_err_to_name(e),
             dead ? "cevap YOK" : "CEVAP VAR");
    esp_lcd_panel_io_del(io);
}

// ─── co5300_init ─────────────────────────────────────────────────────────────

esp_err_t co5300_init(void)
{
    ESP_LOGI(TAG, "Initializing CO5300 QSPI AMOLED (%dx%d)", CO5300_H_RES, CO5300_V_RES);

    // 0. Display power rail (ALDO2 / DS1_PWR_EN) — burada AÇILIYOR.
    //    axp2101_init voltajı 3.3V'a ayarladı ama enable bit'i SET ETMEDİ.
    //    co5300_init çağrıldığında demek ki UI başlatılıyor → display lazım →
    //    rail'i şimdi açıyoruz. Sync wake'te co5300_init çağrılmazsa rail
    //    kapalı kalır → display IC tamamen güçsüz → ekran asla açılamaz.
    axp2101_set_rail(AXP_RAIL_LDO2, true);
    vTaskDelay(pdMS_TO_TICKS(10));  // rail settle (LDO turn-on ~5ms tipik)
    ESP_LOGI(TAG, "Display rail (ALDO2) enabled");

    // 1. Hardware reset
    gpio_reset_pin((gpio_num_t)PIN_DISP_RST);
    gpio_set_direction((gpio_num_t)PIN_DISP_RST, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_DISP_RST, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level((gpio_num_t)PIN_DISP_RST, 1);
    vTaskDelay(pdMS_TO_TICKS(120));

    // 2. SPI bus
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

#ifdef BOARD_VARIANT_NAR
    /* Session 46f: panel hangi hattan / hangi hızda cevap veriyor? (reset sonrası) */
    panel_id_probe("1MHz, cevap D1 (MISO)", 1 * 1000 * 1000, false);
    panel_id_probe("1MHz, cevap D0 (3-tel)", 1 * 1000 * 1000, true);
    /* SLPOUT sonrası da dene (bazı paneller uykudayken okumaya cevap vermez) */
    {
        esp_lcd_panel_io_spi_config_t c = {};
        c.cs_gpio_num = PIN_DISP_CS; c.dc_gpio_num = -1; c.pclk_hz = 1000000;
        c.trans_queue_depth = 2; c.lcd_cmd_bits = 32; c.lcd_param_bits = 8;
        esp_lcd_panel_io_handle_t t = NULL;
        if (esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)SPI2_HOST, &c, &t) == ESP_OK) {
            esp_lcd_panel_io_tx_param(t, (int)SH8601_CMD(0x11), NULL, 0);
            esp_lcd_panel_io_del(t);
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    panel_id_probe("SLPOUT sonrasi, D1", 1 * 1000 * 1000, false);
    panel_id_probe("SLPOUT sonrasi, D0", 1 * 1000 * 1000, true);
#endif

    // 3. Panel IO — quad_mode=1: commands in 1-bit SPI, pixel data in 4-bit QSPI.
    //    lcd_cmd_bits=32 so the full [0x02][0x00][cmd][0x00] frame is sent as cmd.
    //    Session 46d: v1.4.81 teşhis kalıntısı quad_mode=0 geri alındı. RAMWR
    //    opcode'u 0x32 (Quad Page Program) → panel pikselleri 4 hattan bekler;
    //    1-bit gönderilince hiçbir piksel doğru yazılmıyordu (siyah ekran).
    //    Saat 40 MHz bırakıldı (v1.0: 80) — yeni flex/B2B konnektörde güvenli pay.
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
            .quad_mode = CO5300_QUAD_PIXELS,
        },
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)SPI2_HOST, &io_cfg, &s_io));

    // 4. Send init sequence
    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]); i++) {
        const sh_cmd_t* c = &s_init_cmds[i];
        ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(s_io, (int)c->cmd, c->data, c->len));
        if (c->delay_ms) vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
    }

    ESP_LOGI(TAG, "CO5300 init OK");

    /* Session 46e teşhis: panel QSPI'ye cevap veriyor mu? Okuma opcode'u 0x03.
     * Hepsi FF ya da 00 → panel hiç cevap vermiyor (temas / besleme / arayüz
     * modu); anlamlı değer → panel canlı, sorun başka yerde. 0x0A (RDDPM):
     * bit7 booster, bit4 sleep-out... bit2 display-on beklenir (~0x9C). */
    {
        uint8_t id04[3] = {0}, pm = 0, da = 0, db = 0, dc = 0;
        esp_err_t e1 = esp_lcd_panel_io_rx_param(s_io, (int)(0x03000000 | (0x04 << 8)), id04, 3);
        esp_err_t e2 = esp_lcd_panel_io_rx_param(s_io, (int)(0x03000000 | (0x0A << 8)), &pm, 1);
        esp_lcd_panel_io_rx_param(s_io, (int)(0x03000000 | (0xDA << 8)), &da, 1);
        esp_lcd_panel_io_rx_param(s_io, (int)(0x03000000 | (0xDB << 8)), &db, 1);
        esp_lcd_panel_io_rx_param(s_io, (int)(0x03000000 | (0xDC << 8)), &dc, 1);
        bool dead = ((id04[0] & id04[1] & id04[2] & pm & da & db & dc) == 0xFF) ||
                    ((id04[0] | id04[1] | id04[2] | pm | da | db | dc) == 0x00);
        s_panel_ok = !dead;
        ESP_LOGW(TAG, "[panel-id] 04h=%02X %02X %02X (%s) 0Ah(power)=%02X (%s) DA/DB/DC=%02X %02X %02X -> %s",
                 id04[0], id04[1], id04[2], esp_err_to_name(e1), pm, esp_err_to_name(e2), da, db, dc,
                 dead ? "PANEL CEVAP VERMIYOR" : "panel cevap veriyor");
    }

    // ── VRAM temizleme ─────────────────────────────────────────────────────────
    // Fiziksel display LVGL'nin bildiği 410px'den daha geniş.
    // Fazla sütunların VRAM değeri yeşil (0x07E0) kalır → sağda şerit görünür.
    // CASET'i 440px'e genişleterek tüm fiziksel sütunları siyaha yazıyoruz.
    // LVGL yine 410px render eder; 410..439 arası sütunlar siyah kalır.
    {
        const int PHYS_W      = 440;
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

    /* Session 46d: v1.4.79 "AGGRESSIVE PANEL RECOVERY" teşhis bloğu kaldırıldı.
     * İnit bittikten SONRA SWRESET + HW reset atıp 0xFE/0xC4'ü yeniden
     * göndermiyordu (QSPI yazma modu sıfırlanıyordu), ALDO2'yi kapatıp açıyor
     * ve her açılışı 10 sn bekletiyordu. */

    s_ready = true;
    return ESP_OK;
}

bool co5300_is_ready(void) { return s_ready; }

bool co5300_panel_responded(void) { return s_panel_ok; }

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

    /* NOT (2026-05-03): Önceki tur eklenen "QSPI 2-pixel-per-burst column
     * alignment" patch'i (ax1 = x1 & ~1, ax2 = x2 | 1, slow-path padding)
     * geri alındı. Patch toggle/row gibi geniş widget'larda görsel artifact
     * tetikledi (kullanıcı raporu: WiFi + Airplane mode toggle'larda label
     * iki yana ayrılıp toggle ortada). Alarm dial double-dot için ayrı
     * çözüm aranıyor; bu sırada panel'in default davranışı (LVGL'nin verdiği
     * ham bbox) korunuyor — uzun süredir çalışan, kanıtlanmış patika. */
    int x1 = area->x1 + COL_OFFSET,  x2 = area->x2 + COL_OFFSET;
    int y1 = area->y1 + ROW_OFFSET,  y2 = area->y2 + ROW_OFFSET;
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
    /* NULL-safe — bkz co5300_sleep yorumu. */
    if (!s_io) {
        return ESP_OK;
    }
    // [wake-trace] her brightness çağrısını log'la — ekranın "kendi kendine
    // açılması" şikayetinde hangi kod yolunun parlaklığı 255'e döndürdüğünü
    // tespit için. (255'in yanı sıra 0/40/N tüm değerler basılır.)
    ESP_LOGI(TAG, "[wake-trace] set_brightness(%u)", (unsigned)brightness);
    return esp_lcd_panel_io_tx_param(s_io, (int)SH8601_CMD(0x51), &brightness, 1);
}

esp_err_t co5300_sleep(bool enable)
{
    /* NULL-safe: co5300_init henüz çağrılmamışsa s_io == NULL.
     * Sync wake'te task_ui spawn edilmediği için bu durum oluşur. SPI
     * io handle yokken esp_lcd_panel_io_tx_param panic eder → 60sn cycle
     * reboot (kullanıcı raporu 2026-05-12). Display zaten unpowered olduğu
     * için bu sefer no-op yeterli. */
    if (!s_io) {
        ESP_LOGW(TAG, "co5300_sleep: SPI io NULL — display unpowered, skipping");
        return ESP_OK;
    }
    // [wake-trace] SLPIN/SLPOUT çağrılarını log'la (ekran on/off pivot noktası).
    ESP_LOGI(TAG, "[wake-trace] sleep(%s)", enable ? "SLPIN" : "SLPOUT");
    esp_err_t ret = esp_lcd_panel_io_tx_param(
        s_io, (int)SH8601_CMD(enable ? 0x10 : 0x11), NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(enable ? 5 : 120));
    return ret;
}

#endif /* !DISPLAY_VARIANT_18INCH */
