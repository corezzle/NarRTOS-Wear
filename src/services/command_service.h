#pragma once
/**
 * @brief Command Service — cihazı dışarıdan metin komutlarıyla sürme.
 *
 * Faz 1 taşıyıcı: USB seri (UART0 konsolu — log çıkan hattın RX'i).
 * Satır tabanlı komutlar; her komut mevcut iç API'lere köprüdür:
 *
 *   help                       komut listesi
 *   info                       sürüm/uptime/heap/wifi/aktif app (JSON)
 *   app launch <isim|id>       uygulama aç   (EVENT_APP_SWITCH ile, thread-safe)
 *   app back                   geri          (lv_async_call ile LVGL task'ında)
 *   app current                aktif uygulama
 *   app list                   isim -> id tablosu
 *   swipe left|right|up|down   jest enjeksiyonu (g_swipe_dir)
 *   brightness <0-255>         ekran parlaklığı
 *   wake                       ekranı uyandır (aktivite tetikle)
 *   reboot                     yeniden başlat
 *
 * Yanıtlar "[cmd]" önekiyle seriye yazılır (loglardan ayırt etmek için).
 * İleride aynı parser BLE characteristic'ine ve HTTP'ye bağlanacak —
 * taşıyıcıdan bağımsız çekirdek: command_service_exec().
 *
 * Şimdilik yalnızca LCD128 board varyantında etkin (AMOLED build'ı
 * değişmesin kuralı) — kararlılık kanıtlanınca ortaklaştırılır.
 */

#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/** UART okuyucu görevini başlat (boot'ta bir kez, UI hazır olduktan sonra). */
esp_err_t command_service_init(void);

/** Yanıt satırı alıcısı — taşıyıcı kendi kanalına yazar (seri printf,
 *  BLE notify, ...). line "[cmd] ..." önekli, NUL-sonlu, newline'sız. */
typedef void (*cmd_reply_fn_t)(const char* line, void* ctx);

/** Tek bir komut satırını çalıştır — taşıyıcıdan bağımsız çekirdek.
 *  Yanıtlar reply(line, ctx) ile teslim edilir. Farklı taşıyıcılardan
 *  eşzamanlı çağrılara karşı içeride serileştirilir (mutex).
 *  @param line NUL-sonlu komut satırı (değiştirilebilir buffer olmalı). */
void command_service_exec_ex(char* line, cmd_reply_fn_t reply, void* ctx);

/** exec_ex'in seri (printf) yanıtlı kısayolu — USB taşıyıcısı kullanır. */
void command_service_exec(char* line);

/** "Son sonuç" (2026-11-09): uzun süren ölçümlerin (mic nabiz) tek satırlık özeti. BLE'de ayrı bir READ
 *  karakteristiğinden (NarRTOS CMD …0004) her an okunur; Log'a bakmak gerekmez. Boşsa "-" döner. */
void        command_service_set_result(const char* text);
const char* command_service_get_result(void);

#ifdef __cplusplus
}
#endif
