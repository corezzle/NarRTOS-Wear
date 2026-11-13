#pragma once
#include <esp_err.h>
#include <time.h>

esp_err_t pcf85063_init(void);
esp_err_t pcf85063_get_time(struct tm* out);
esp_err_t pcf85063_set_time(const struct tm* t);
