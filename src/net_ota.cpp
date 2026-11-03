#include "net_ota.h"
#include "common_log.h"
#include "ota_processor.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_app_desc.h"
#include "lwip/sockets.h"
#include <cstring>
#include "utils.h"

#define TAG "NET_OTA"
#define OTA_PORT 3232
#define BROADCAST_INTERVAL_SEC 1

#ifndef OTA_IDLE_TIMEOUT_SEC
#define OTA_IDLE_TIMEOUT_SEC 120
#endif

#ifndef OTA_SOCKET_RECV_TIMEOUT_SEC
#define OTA_SOCKET_RECV_TIMEOUT_SEC 30
#endif

// File-scope statics to keep main task stack usage low on ESP32-S3
static int udp_sock = -1;
static int listen_sock = -1;
static struct sockaddr_in broadcast_addr;
static char discovery_msg[64];
static uint8_t rx_buffer[1024];
static OtaProcessor otaProcessor;

void start_network_ota_process(const nvs_config_t *config) {
    INFO("Starting Network Listener...");

    udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (udp_sock < 0) FAIL("Failed to create UDP socket");
    int broadcast = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_BROADCAST, &broadcast, sizeof(broadcast));

    memset(&broadcast_addr, 0, sizeof(broadcast_addr));
    broadcast_addr.sin_family = AF_INET;
    broadcast_addr.sin_port = htons(OTA_PORT);
    broadcast_addr.sin_addr.s_addr = htonl(INADDR_BROADCAST);

    char* devName = getDeviceName();
    const esp_app_desc_t *app_desc = esp_app_get_description();
    snprintf(discovery_msg, sizeof(discovery_msg), "%s %s", devName, app_desc->version);

    listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) FAIL("Failed to create TCP socket");
    int reuse = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(OTA_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) FAIL("TCP Bind failed");
    if (listen(listen_sock, 1) < 0) FAIL("TCP Listen failed");

    INFO("Listening on TCP port %d (idle timeout: %ds)", OTA_PORT, OTA_IDLE_TIMEOUT_SEC);

    // Set the expected hash from NVS
    otaProcessor.setNvramExpectedHash(config->ota_hash);
    otaProcessor.setAckEnabled(true);

    while (true) {
        int client_sock = -1;
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        uint32_t idle_seconds = 0;

        INFO("Waiting for client...");

        while (client_sock < 0) {
            sendto(udp_sock, discovery_msg, strlen(discovery_msg), 0, (struct sockaddr *)&broadcast_addr, sizeof(broadcast_addr));
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_sock, &readfds);
            struct timeval tv = { .tv_sec = BROADCAST_INTERVAL_SEC, .tv_usec = 0 };
            int activity = select(listen_sock + 1, &readfds, NULL, NULL, &tv);
            if (activity > 0 && FD_ISSET(listen_sock, &readfds)) {
                client_sock = accept(listen_sock, (struct sockaddr *)&client_addr, &client_addr_len);
                if (client_sock >= 0) {
                    INFO("Client connected from %s", inet_ntoa(client_addr.sin_addr));
                }
            } else {
                idle_seconds += BROADCAST_INTERVAL_SEC;
                if (!otaProcessor.isFlashErased() && idle_seconds >= OTA_IDLE_TIMEOUT_SEC) {
                    WARN("No OTA client connected within %u seconds. Restoring app0 and rebooting...", (unsigned int)idle_seconds);
                    try_restore_app0_boot_partition();
                    vTaskDelay(pdMS_TO_TICKS(500));
                    esp_restart();
                }
            }
            vTaskDelay(10 / portTICK_PERIOD_MS);
        }

        // Prevent a stalled TCP connection from blocking recv() forever
        struct timeval rcv_timeout = { .tv_sec = OTA_SOCKET_RECV_TIMEOUT_SEC, .tv_usec = 0 };
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_timeout, sizeof(rcv_timeout));

        otaProcessor.reset();
        otaProcessor.setSender([client_sock](const char* data, size_t len) {
            send(client_sock, data, len, 0);
        });

        while (true) {
            int len = recv(client_sock, rx_buffer, sizeof(rx_buffer), 0);
            if (len <= 0) {
                INFO("Client disconnected or timed out");
                break;
            }
            otaProcessor.process(rx_buffer, len);

            if (otaProcessor.isRebootRequired()) {
                #ifdef USE_WIFI_OTA
                if (config->ssid[0] != '\0') {
                    INFO("Marking NVRAM as updated for WiFi recovery.");
                    nvs_mark_updated();
                }
                #endif
                INFO("Reboot flag detected. Restarting in 2 seconds...");
                vTaskDelay(2000 / portTICK_PERIOD_MS); // Wait for ACK flush
                closesocket(client_sock);
                esp_restart();
            }
        }

        closesocket(client_sock);
        otaProcessor.reset();
    }
}
