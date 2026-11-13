#pragma once
/**
 * @file store_app.h
 * @brief Store / mağaza uygulaması — v1.2.4 (2026-05-19).
 *
 * Launcher'da Settings/Gallery/Alarms gibi tile olarak görünür. İçeride
 * 2 tab var:
 *   - Apps  : Saatte yüklü built-in uygulamaların read-only browser'ı
 *             (Alarm, Timer, Weather, Heart Rate, Steps, Gallery, Notify).
 *             v1.2.4'te disable/uninstall yok — tüm app'ler "Installed".
 *   - Faces : Watchface marketplace — backend henüz hazır değil, stub.
 *
 * Mevcut sistem'e dokunmama prensipi:
 *   - Yeni dosyalar: src/apps/store/store_app.h, store_app.cpp
 *   - os_config.h: APP_ID_STORE = 11 (yeni satır)
 *   - app_manager.cpp: store_app_get_interface() kaydı (1 satır)
 *   - launcher_app.cpp: 5. tile (1 make_icon çağrısı)
 *   Mevcut diğer kodlara dokunulmuyor.
 */

#include "../../core/app_manager.h"

const app_interface_t* store_app_get_interface(void);
