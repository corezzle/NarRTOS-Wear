/**
 * @file i2c_bus.cpp
 * @brief Paylaşımlı I2C bus implementasyonu.
 *
 * Tek I2C_NUM_0 bus, tüm çevre birimleri bu bus üzerinde:
 *   FT3168  (0x38) — UI Task / Core 0
 *   ICM42670 (0x68) — Sensor Task / Core 1
 *   AXP2101 (0x34) — Power Task  / Core 1
 *   PCF85063 (0x51) — Sensor Task / Core 1
 *
 * FreeRTOS mutex ile çakışma önlenir.
 * Timeout: 10ms (I2C hung bus koruması).
 */

#include "i2c_bus.h"
#include "../core/os_config.h"

#include <driver/i2c.h>
#include <driver/gpio.h>
#include <esp_rom_sys.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

static const char* TAG = "I2CBus";

#define I2C_TIMEOUT_MS  50

static SemaphoreHandle_t s_mutex = NULL;

// ─── i2c_bus_init ─────────────────────────────────────────────────────────────

/* Takılı kalmış hat kurtarma (Session 46g): reset bir okumanın ortasına denk
 * gelirse bir slave SDA'yı aşağıda tutar ve hat güç kesilene kadar ölür (NAR
 * kartında esptool RTS reseti sonrası PMIC dahil hiçbir çip cevap vermedi).
 * Standart çözüm: SDA aşağıdayken SCL'ye 9 darbe, sonra STOP. */
static void i2c_bus_recover(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_I2C_SDA) | (1ULL << PIN_I2C_SCL),
        .mode         = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
    gpio_set_level((gpio_num_t)PIN_I2C_SDA, 1);
    gpio_set_level((gpio_num_t)PIN_I2C_SCL, 1);
    esp_rom_delay_us(10);

    bool sda = gpio_get_level((gpio_num_t)PIN_I2C_SDA);
    bool scl = gpio_get_level((gpio_num_t)PIN_I2C_SCL);
    if (sda && scl) return;                       /* hat serbest */

    ESP_LOGW(TAG, "I2C hatti takili (SDA=%d SCL=%d) — kurtarma", sda, scl);
    int pulses = 0;
    for (int i = 0; i < 9 && !gpio_get_level((gpio_num_t)PIN_I2C_SDA); i++) {
        gpio_set_level((gpio_num_t)PIN_I2C_SCL, 0); esp_rom_delay_us(5);
        gpio_set_level((gpio_num_t)PIN_I2C_SCL, 1); esp_rom_delay_us(5);
        pulses++;
    }
    /* STOP: SCL yüksekken SDA düşük → yüksek */
    gpio_set_level((gpio_num_t)PIN_I2C_SDA, 0); esp_rom_delay_us(5);
    gpio_set_level((gpio_num_t)PIN_I2C_SCL, 1); esp_rom_delay_us(5);
    gpio_set_level((gpio_num_t)PIN_I2C_SDA, 1); esp_rom_delay_us(10);
    ESP_LOGW(TAG, "I2C kurtarma: %d darbe, sonuc SDA=%d SCL=%d", pulses,
             gpio_get_level((gpio_num_t)PIN_I2C_SDA), gpio_get_level((gpio_num_t)PIN_I2C_SCL));
}


#ifndef BOARD_VARIANT_LCD128
/* ─── PMIC kör yazma (Session 46s) ────────────────────────────────────────────
 * Bazı NAR kartlarında AXP2101 açılışta LDO'ların bir kısmını (ALDO4 = IMU VDD) KAPALI
 * başlatıyor. Beslemesiz çip SDA/SCL'yi ~2.0 V'a kırpıyor; ESP32 bunu lojik-1 saymadığı
 * için sürücü hiçbir işlem yapamıyor (PMIC dahil 0 cihaz) → rayı normal yoldan açmak da
 * imkânsız. AXP2101 ise 2.0 V'u lojik-1 kabul ediyor: pinler elle sürülüp ACK BEKLENMEDEN
 * 0x34: 0x90=0xFF (tüm LDO'lar açık) yazılır; hat 3 V'a döner. Yalnız PMIC normal yoldan
 * cevap vermiyorsa çağrılır. */
/* 2026-10-30 (yeni SiP, dev kit): hat 0,8-1,5 V'a kelepçelenince open-drain
 * sürüş + pull-up ile SCL AXP2101'in lojik-1 eşiğine bile çıkamıyor, kör yazma
 * geçmiyor. Pad testi push-pull sürüşün hattı 1'e çıkarabildiğini gösterdi
 * (H→1 ok). Bu yüzden kör yazma PUSH-PULL sürüşle yapılır; yalnız ACK
 * yuvasında SDA girişe alınır (çekişme yok) ve ACK okunup loglanır. Önce
 * ALDO1 gerilimi 3,3 V (0x92=0x1C), sonra LDO'lar açık (0x90=0xFF).
 *
 * 2026-10-30 akşam (Emin ölçümü): A3V3 3,3 V, SCL direncin iki yanında 3,3 V,
 * SDA direnç öncesi 3,3 V / sonrası 0,8 V → SDA'yı bir köle pini diyot eşiğinde
 * kilitliyor; SCL sağlam. Hasar yalnız kölelerin sürdüğü hatta. 40 mA
 * (GPIO_DRIVE_CAP_3) push-pull, beslemesiz çipi ESD diyotu üzerinden besler ve
 * çekişmede köle pinini zorlar; iki ES7210 art arda aynı tabloya düştü. Sürüş
 * bu yüzden GPIO_DRIVE_CAP_0'a (~5 mA, tipik pin mutlak sınırı altında)
 * indirildi: çekişme ve parazit besleme artık yıkıcı değil. Bu akım kelepçeyi
 * yenemezse çözüm donanımda (ALDO1 açılış varsayılanı / ES7210 besleme), 40 mA'ya
 * geri DÖNÜLMEZ. */
static int s_blind_ack_fail = 0;

static void blind_bit(int sda_level)
{
    gpio_set_level((gpio_num_t)PIN_I2C_SDA, sda_level); esp_rom_delay_us(100);
    gpio_set_level((gpio_num_t)PIN_I2C_SCL, 1);         esp_rom_delay_us(200);
    gpio_set_level((gpio_num_t)PIN_I2C_SCL, 0);         esp_rom_delay_us(100);
}

static void blind_byte(uint8_t b)
{
    const gpio_num_t sda = (gpio_num_t)PIN_I2C_SDA, scl = (gpio_num_t)PIN_I2C_SCL;
    for (int i = 7; i >= 0; i--) blind_bit((b >> i) & 1);
    /* ACK yuvası: SDA'yı bırak, oku */
    gpio_set_direction(sda, GPIO_MODE_INPUT); gpio_set_pull_mode(sda, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(100);
    gpio_set_level(scl, 1); esp_rom_delay_us(150);
    int ack = gpio_get_level(sda);                 /* 0 = ACK */
    esp_rom_delay_us(50);
    gpio_set_level(scl, 0); esp_rom_delay_us(50);
    gpio_set_direction(sda, GPIO_MODE_OUTPUT); gpio_set_level(sda, 1); esp_rom_delay_us(50);
    if (ack) s_blind_ack_fail++;
}

static void blind_write(uint8_t reg, uint8_t val)
{
    const gpio_num_t sda = (gpio_num_t)PIN_I2C_SDA, scl = (gpio_num_t)PIN_I2C_SCL;
    gpio_set_level(sda, 0); esp_rom_delay_us(200); gpio_set_level(scl, 0); esp_rom_delay_us(100);   /* START */
    blind_byte(0x34 << 1); blind_byte(reg); blind_byte(val);
    gpio_set_level(sda, 0); esp_rom_delay_us(100); gpio_set_level(scl, 1); esp_rom_delay_us(200);
    gpio_set_level(sda, 1); esp_rom_delay_us(2000);                                                   /* STOP */
}

/* Tek kör yazma denemesi. Önce hatlar push-pull 50 ms yüksekte tutulur:
 * beslemesiz çiplerin ESD diyotları üzerinden hayalet rayları şarj olur,
 * kelepçe zayıflar. ACK sayısı kelepçeli hatta güvenilmez (SDA zaten 0
 * okunur → sahte ACK); bu yüzden başarı çağıran tarafta PMIC'ten gerçek
 * okumayla doğrulanır, deneme sayısı oradan yönetilir. */
static void pmic_blind_enable_ldos(int deneme)
{
    const gpio_num_t sda = (gpio_num_t)PIN_I2C_SDA, scl = (gpio_num_t)PIN_I2C_SCL;
    gpio_reset_pin(sda); gpio_reset_pin(scl);
    /* CAP_0 (~5 mA): köle pini / ESD diyotu için güvenli sınır, bkz. üstteki not */
    gpio_set_direction(sda, GPIO_MODE_OUTPUT); gpio_set_drive_capability(sda, GPIO_DRIVE_CAP_0);
    gpio_set_direction(scl, GPIO_MODE_OUTPUT); gpio_set_drive_capability(scl, GPIO_DRIVE_CAP_0);
    gpio_set_level(sda, 1); gpio_set_level(scl, 1);
    vTaskDelay(pdMS_TO_TICKS(50));                   /* ön şarj */
    /* takılı slave için 9 saat darbesi (push-pull) + STOP */
    for (int i = 0; i < 9; i++) { gpio_set_level(scl, 0); esp_rom_delay_us(100); gpio_set_level(scl, 1); esp_rom_delay_us(100); }
    gpio_set_level(sda, 0); esp_rom_delay_us(100); gpio_set_level(sda, 1); esp_rom_delay_us(500);
    s_blind_ack_fail = 0;
    blind_write(0x92, 0x1C);                          /* ALDO1 = 3,3 V */
    blind_write(0x90, 0xFF);                          /* LDO'lar açık */
    ESP_LOGW(TAG, "kor yazma (push-pull 5 mA) %d: ACK hatasi %d/6 (kelepcede ACK guvenilmez)", deneme, s_blind_ack_fail);
    gpio_reset_pin(sda); gpio_reset_pin(scl);
    vTaskDelay(pdMS_TO_TICKS(300));                  /* raylar otursun, hat yükselsin */
}
#endif

esp_err_t i2c_bus_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex);

    i2c_bus_recover();

    i2c_config_t cfg = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = PIN_I2C_SDA,
        .scl_io_num       = PIN_I2C_SCL,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master = {
            .clk_speed    = I2C_MASTER_FREQ_HZ,
        },
        .clk_flags        = 0,
    };

    esp_err_t ret = i2c_param_config(I2C_NUM_0, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(ret));
        return ret;
    }


#ifndef BOARD_VARIANT_LCD128
    /* PMIC cevap vermiyorsa hat kırpılmış olabilir → LDO'ları körlemesine aç, sürücüyü yeniden kur */
    {
        uint8_t reg = 0x03, id = 0;
        if (i2c_master_write_read_device(I2C_NUM_0, 0x34, &reg, 1, &id, 1, pdMS_TO_TICKS(50)) != ESP_OK) {
            ESP_LOGW(TAG, "PMIC cevap vermiyor — hat kirpilmis olabilir, LDO'lar korlemesine aciliyor (push-pull, 0x92=1C 0x90=FF)");
            bool ok = false;
            for (int d = 1; d <= 8 && !ok; d++) {
                i2c_driver_delete(I2C_NUM_0);
                pmic_blind_enable_ldos(d);
                i2c_param_config(I2C_NUM_0, &cfg);
                i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
                ok = i2c_master_write_read_device(I2C_NUM_0, 0x34, &reg, 1, &id, 1, pdMS_TO_TICKS(50)) == ESP_OK;
                ESP_LOGW(TAG, "kor yazma %d sonrasi PMIC: %s", d, ok ? "CEVAP VERIYOR (hat kurtarildi)" : "hala cevap yok");
            }
        }
    }
#endif

    ESP_LOGI(TAG, "I2C bus init OK (SDA=%d, SCL=%d, %dkHz)",
        PIN_I2C_SDA, PIN_I2C_SCL, I2C_MASTER_FREQ_HZ / 1000);
    return ESP_OK;
}

// ─── i2c_bus_write ───────────────────────────────────────────────────────────

esp_err_t i2c_bus_write(uint8_t addr, const uint8_t* data, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_write_to_device(
        I2C_NUM_0, addr, data, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    xSemaphoreGive(s_mutex);
    return ret;
}

// ─── i2c_bus_read_reg ────────────────────────────────────────────────────────

esp_err_t i2c_bus_read_reg(uint8_t addr, uint8_t reg, uint8_t* out_buf, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t ret = i2c_master_write_read_device(
        I2C_NUM_0, addr, &reg, 1, out_buf, len, pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    xSemaphoreGive(s_mutex);
    return ret;
}

// ─── i2c_bus_write_reg ───────────────────────────────────────────────────────

esp_err_t i2c_bus_write_reg(uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_bus_write(addr, buf, sizeof(buf));
}

// ─── i2c_bus_rmw_reg ─────────────────────────────────────────────────────────

esp_err_t i2c_bus_rmw_reg(uint8_t addr, uint8_t reg, uint8_t mask, uint8_t value)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    uint8_t cur = 0;
    esp_err_t ret = i2c_master_write_read_device(
        I2C_NUM_0, addr, &reg, 1, &cur, 1, pdMS_TO_TICKS(I2C_TIMEOUT_MS));

    if (ret == ESP_OK) {
        cur = (cur & ~mask) | (value & mask);
        uint8_t buf[2] = { reg, cur };
        ret = i2c_master_write_to_device(
            I2C_NUM_0, addr, buf, sizeof(buf), pdMS_TO_TICKS(I2C_TIMEOUT_MS));
    }

    xSemaphoreGive(s_mutex);
    return ret;
}

// ─── teşhis yardımcıları ─────────────────────────────────────────────────────

void i2c_bus_lock(void)   { if (s_mutex) xSemaphoreTake(s_mutex, portMAX_DELAY); }
void i2c_bus_unlock(void) { if (s_mutex) xSemaphoreGive(s_mutex); }

esp_err_t i2c_bus_reattach_pins(void)
{
    return i2c_set_pin(I2C_NUM_0, PIN_I2C_SDA, PIN_I2C_SCL,
                       GPIO_PULLUP_ENABLE, GPIO_PULLUP_ENABLE, I2C_MODE_MASTER);
}
