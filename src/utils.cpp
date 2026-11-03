#include "utils.h"
#include "common_log.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_mac.h"
#include <cstring>
#include <cstdio>

#define TAG "UTILS"

void corrupt_partition(const esp_partition_t *partition) {
    uint8_t dummy_data[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // Overwrite the magic byte 0xE9
    // A small write to offset 0 will make the partition unbootable by the ESP32 bootloader
    if (esp_partition_write(partition, 0, dummy_data, sizeof(dummy_data)) != ESP_OK) {
        INFO("Failed to corrupt partition header!");
    } else {
        INFO("Partition header corrupted successfully.");
    }
}

bool try_restore_app0_boot_partition() {
    const esp_partition_t *app0 = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!app0) {
        WARN("Could not find app0 (OTA_0) partition!");
        return false;
    }
    esp_app_desc_t app0_desc;
    if (esp_ota_get_partition_description(app0, &app0_desc) != ESP_OK) {
        WARN("app0 partition does not contain a valid image; keeping current boot partition.");
        return false;
    }
    if (esp_ota_set_boot_partition(app0) != ESP_OK) {
        WARN("Failed to set boot partition back to app0!");
        return false;
    }
    return true;
}

void print_hash(const char *prefix, const uint8_t *hash) {
    printf(prefix);
    for (int i = 0; i < 32; i++) printf("%02x", hash[i]);
    printf("\r\n");
}

char *getDeviceName() {
    static char name[20];
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(name, sizeof(name), "Meshtastic_%02X%02X", mac[4], mac[5]);
    return name;
}
