#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_chip_info.h"

static const char *TAG = "NEX";

void app_main(void)
{
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    ESP_LOGI(TAG, "NEX Firmware");
    ESP_LOGI(TAG, "ESP-IDF: %s",
             esp_get_idf_version());

    ESP_LOGI(TAG, "CPU cores: %d",
             chip_info.cores);

    ESP_LOGI(TAG,
             "Hardware initialization pending");

    printf("\nWelcome to NEX.\n");
}
