#pragma once
/**
 * @file weather_app.h
 * @brief "Hava Durumu" uygulaması — Apple Watch tarzı detaylı hava ekranı.
 *
 * Veri kaynağı: weather_service (Open-Meteo + IP geolocation).
 * UI: gradient mavi zemin, üstte şehir + büyük sıcaklık, altta üç halka
 * widget (UV, Rüzgâr, Nem).
 */

#include "../../core/app_manager.h"

const app_interface_t* weather_app_get_interface(void);
