/**
 * @file twai_driver.cpp
 * @brief ESP32-S3 dahili TWAI denetleyicisi (bkz. header). NarRTOS-Drive'dan taşındı.
 *
 *  - tx_queue_len = 0: cihaz hatta YAZMAZ (yanlışlıkla bile mümkün olmasın).
 *  - rx_queue_len = 64: 250 kbit/s yoğun hat ~2000 frame/sn; alıcı görev kendi
 *    çekirdeğinde ve yüksek öncelikte, kuyruk yalnız kısa tıkanmalar için.
 *  - Bus-off: hat arızasında denetleyici kendini kapatır; otomatik kurtarma başlatılır.
 *  - Filtre yok: hangi PGN'lerin ilgilendiği sunucuda belli olur; cihazda süzmek bilgi atmaktır.
 */
#include "twai_driver.h"
#include "../../services/can_service.h"

#if defined(TRACKER_FEATURES) && ENABLE_CAN

#include "../display/co5300_driver.h"     /* co5300_is_ready(): GPIO 4-7 ekran QSPI hatları */
#include <driver/twai.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

/* NAR tasmasında dışarı verilen padler ekranın QSPI veri hatları (D0-D3 = GPIO 4-7). Ekran
 * takılı değil ama sürücü boot'ta bu pinleri kurar; TWAI'nin pinleri ekran init'i BİTTİKTEN
 * sonra devralması gerekir (piezo GPIO6 ile aynı kalıp). gpio_reset_pin() SPI'nin matris
 * çıkışını koparır; flush'lar artık bu iki pini süremez. */
#define CAN_SHARES_DISPLAY_PIN ((PIN_CAN_TX >= 4 && PIN_CAN_TX <= 7) || (PIN_CAN_RX >= 4 && PIN_CAN_RX <= 7))

static const char* TAG = "twai";
static bool         s_running = false;
static bool         s_booting = false;
static uint32_t     s_kbps    = CAN_BITRATE_DEFAULT_KBPS;
static TaskHandle_t s_task    = NULL;

static twai_timing_config_t timing_for(uint32_t kbps)
{
    switch (kbps) {
        case 125:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_125KBITS();  return t; }
        case 500:  { twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();  return t; }
        case 1000: { twai_timing_config_t t = TWAI_TIMING_CONFIG_1MBITS();    return t; }
        default:   { twai_timing_config_t t = TWAI_TIMING_CONFIG_250KBITS();  return t; }
    }
}

static void twai_rx_task(void* arg)
{
    (void)arg;
    for (;;) {
        twai_message_t msg;
        esp_err_t err = twai_receive(&msg, pdMS_TO_TICKS(1000));
        if (err == ESP_OK) {
            if (!msg.rtr)   /* uzaktan istek frame'inde veri yok */
                can_service_ingest(msg.identifier, msg.extd, msg.data, msg.data_length_code);
            continue;
        }
        if (err == ESP_ERR_TIMEOUT) {
            twai_status_info_t st;
            if (twai_get_status_info(&st) == ESP_OK && st.state == TWAI_STATE_BUS_OFF) {
                ESP_LOGW(TAG, "bus-off - kurtarma baslatiliyor");
                twai_initiate_recovery();
            }
            continue;
        }
        ESP_LOGW(TAG, "twai_receive: %s", esp_err_to_name(err));
    }
}

static esp_err_t twai_install(uint32_t kbps)
{
    if (s_running) return ESP_OK;
    /* Pinleri sıfırla: ekran SPI'sinin GPIO matrisindeki çıkış bağlantısı koparılır (TX'i
     * TWAI zaten üstüne yazar; RX için bu şart, yoksa her LVGL flush RX'e çöp basar). */
    gpio_reset_pin((gpio_num_t)PIN_CAN_TX);
    gpio_reset_pin((gpio_num_t)PIN_CAN_RX);
#if PIN_CAN_STBY >= 0
    gpio_reset_pin((gpio_num_t)PIN_CAN_STBY);
    gpio_set_direction((gpio_num_t)PIN_CAN_STBY, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_CAN_STBY, 0);    /* transceiver aktif (STBY=LOW) */
#endif
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT((gpio_num_t)PIN_CAN_TX, (gpio_num_t)PIN_CAN_RX, TWAI_MODE_NORMAL);
    g.tx_queue_len = 0;
    g.rx_queue_len = 64;
    twai_timing_config_t t = timing_for(kbps);
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    esp_err_t err = twai_driver_install(&g, &t, &f);
    if (err != ESP_OK) { ESP_LOGE(TAG, "driver_install: %s", esp_err_to_name(err)); return err; }
    err = twai_start();
    if (err != ESP_OK) { ESP_LOGE(TAG, "twai_start: %s", esp_err_to_name(err)); twai_driver_uninstall(); return err; }
    if (xTaskCreatePinnedToCore(twai_rx_task, "twai_rx", 3072, NULL, CAN_TASK_PRIORITY, &s_task, CAN_TASK_CORE) != pdPASS) {
        ESP_LOGE(TAG, "alici gorev olusturulamadi"); twai_stop(); twai_driver_uninstall(); return ESP_FAIL;
    }
    s_running = true;
    ESP_LOGI(TAG, "init OK (%lu kbit/s, TX kapali, tx=%d rx=%d%s)", (unsigned long)kbps, PIN_CAN_TX, PIN_CAN_RX,
             CAN_SHARES_DISPLAY_PIN ? ", ekran D-hatlari devralindi" : "");
    return ESP_OK;
}

static void twai_boot_task(void* arg)
{
    (void)arg;
#if CAN_SHARES_DISPLAY_PIN
    /* Ekran init'ini bekle (UI task, Core 0). En fazla 20 sn; sonra yine kur. */
    for (int i = 0; i < 200 && !co5300_is_ready(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    if (!co5300_is_ready()) ESP_LOGW(TAG, "ekran init bekleme asildi, pinler yine de devraliniyor");
    vTaskDelay(pdMS_TO_TICKS(200));
#endif
    esp_err_t e = twai_install(s_kbps);
    if (e != ESP_OK) ESP_LOGE(TAG, "baslatilamadi: %s", esp_err_to_name(e));
    s_booting = false;
    vTaskDelete(NULL);
}

esp_err_t can_twai_start(uint32_t kbps)
{
    if (s_running || s_booting) return ESP_OK;
    if (PIN_CAN_TX < 0 || PIN_CAN_RX < 0) {
        ESP_LOGE(TAG, "PIN_CAN_TX/RX tanimsiz - build flag ile verin");
        return ESP_ERR_INVALID_STATE;
    }
    s_kbps = kbps; s_booting = true;
    /* Kurulum ayrı görevde: ekran paylaşımı varsa init'i bekler, yoksa hemen kurar. */
    if (xTaskCreatePinnedToCore(twai_boot_task, "twai_boot", 3072, NULL, CAN_TASK_PRIORITY, NULL, CAN_TASK_CORE) != pdPASS) {
        s_booting = false; return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t can_twai_stop(void)
{
    if (!s_running) return ESP_OK;
    if (s_task) { vTaskDelete(s_task); s_task = NULL; }
    s_booting = false;
    twai_stop(); twai_driver_uninstall(); s_running = false;
    return ESP_OK;
}

bool can_twai_running(void) { return s_running; }

int can_twai_bus_state(void)
{
    if (!s_running) return -1;
    twai_status_info_t st;
    if (twai_get_status_info(&st) != ESP_OK) return -1;
    switch (st.state) {
        case TWAI_STATE_STOPPED:    return 0;
        case TWAI_STATE_RUNNING:    return 1;
        case TWAI_STATE_BUS_OFF:    return 2;
        case TWAI_STATE_RECOVERING: return 3;
        default:                    return -1;
    }
}

#else   /* ENABLE_CAN == 0 ya da tracker yok — donanım yok, servis `can inject` ile beslenir */

esp_err_t can_twai_start(uint32_t kbps) { (void)kbps; return ESP_ERR_NOT_SUPPORTED; }
esp_err_t can_twai_stop(void)           { return ESP_OK; }
bool      can_twai_running(void)        { return false; }
int       can_twai_bus_state(void)      { return -1; }

#endif
