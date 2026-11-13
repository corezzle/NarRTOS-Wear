#pragma once
#include "../../core/app_manager.h"

/**
 * @brief HQ Message — Genel Müdürlük mesaj ekranı.
 *
 * Watchface'ten SOLA swipe ile açılır (v1.2.9). Ortada yuvarlak görsel
 * placeholder'ı, altında Genel Müdürlük'ten gelen mesajın gösterileceği
 * metin alanı bulunur. Görsel ve mesaj içeriği ileride BLE/OTA üzerinden
 * beslenecek; şimdilik statik placeholder gösterilir.
 */
const app_interface_t* hq_message_app_get_interface(void);
