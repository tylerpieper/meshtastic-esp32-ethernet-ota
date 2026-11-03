// System Includes
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"

// Project Includes
#include "common_log.h"
#include "nvs_config.h"
#include "net_ota.h"

#ifdef USE_WIFI_OTA
#include "wifi_app.h"
#endif

#ifdef USE_ETH_OTA
#include "eth_app.h"
#endif

#ifdef USE_BLE_OTA
#include "ble_ota.h"
#endif

#include "utils.h"

#define TAG "MAIN"

#define NO_REBOOT_OTA 0
#define OTA_BLE 1
#define OTA_WIFI 2
#define OTA_ETH 3

extern "C" void app_main(void) {

    esp_netif_init();
    esp_event_loop_create_default();

    // Point boot partition back to app0 while in pre-erase standby so any reset,
    // connection failure, or idle timeout cleanly returns to Meshtastic firmware.
    // OtaProcessor::handleOtaStart() locks boot back to OTA_1 right before erasing app0.
    try_restore_app0_boot_partition();

    // Keep the NVS namespace "MeshtasticOTA" constant so configuration is shared
    nvs_init_custom("MeshtasticOTA");

    const esp_app_desc_t *app_desc = esp_app_get_description();
    INFO("\n\n//\\ E S H T /\\ S T / C\n\n");
    printf("OTA Loader v%s (%s)\r\n", app_desc->version, app_desc->project_name);
    printf("Compiled on: %s %s\r\n", app_desc->date, app_desc->time);

    nvs_config_t config;
    nvs_read_config(&config);

    print_hash("Expecting firmware with hash: ", config.ota_hash);

    // If BLE is disabled at compile time, force network mode
    #ifndef USE_BLE_OTA
    if (config.method == OTA_BLE) {
        INFO("Config requested BLE, but this firmware is Network-Only.");
        #ifdef USE_ETH_OTA
        config.method = OTA_ETH;
        #else
        config.method = OTA_WIFI;
        #endif
    }
    #endif

    // Network clients request OTA_WIFI (2) or OTA_ETH (3). On Ethernet-enabled builds
    // without Wi-Fi, route network OTA requests (and uninitialized method 0) to Ethernet.
    #if defined(USE_ETH_OTA) && !defined(USE_WIFI_OTA)
    if (config.method == OTA_WIFI || config.method == NO_REBOOT_OTA) {
        config.method = OTA_ETH;
    }
    #endif

    if (config.method == OTA_ETH) {
        #ifdef USE_ETH_OTA
        INFO("Mode: Ethernet OTA");
        if (eth_connect(&config)) {
            start_network_ota_process(&config);
        } else {
            WARN("Ethernet connection failed or timed out. Rebooting to main app (app0)...");
            try_restore_app0_boot_partition();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        #else
        FAIL("Ethernet Mode requested but not compiled in.");
        #endif
    } else if (config.method == OTA_WIFI) {
        #ifdef USE_WIFI_OTA
        INFO("Mode: WiFi OTA");
        INFO("Connecting to SSID: %s", config.ssid);
        wifi_connect(&config);
        start_network_ota_process(&config);
        #else
        FAIL("WiFi Mode requested but not compiled in.");
        #endif
    } else {
        #ifdef USE_BLE_OTA
        INFO("Mode: BLE OTA");
        xTaskCreate(ble_ota_task, "ble_ota_task", 8192, NULL, 5, NULL);
        #else
        FAIL("BLE Mode requested but not compiled in.");
        #endif
    }
}
