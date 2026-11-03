#include "eth_app.h"
#include "common_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "lwip/inet.h"
#include <cstring>

#define TAG "ETH_APP"

#ifndef ETH_MODULE_TYPE
#define ETH_MODULE_TYPE ETH_MODULE_W5500
#endif

#ifndef ETH_MOSI_PIN
#define ETH_MOSI_PIN 11
#endif

#ifndef ETH_MISO_PIN
#define ETH_MISO_PIN 12
#endif

#ifndef ETH_SCLK_PIN
#define ETH_SCLK_PIN 13
#endif

#ifndef ETH_CS_PIN
#define ETH_CS_PIN 14
#endif

#ifndef ETH_INT_PIN
#define ETH_INT_PIN 10
#endif

#ifndef ETH_RST_PIN
#define ETH_RST_PIN 9
#endif

#ifndef ETH_SPI_HOST
#define ETH_SPI_HOST SPI3_HOST
#endif

#ifndef ETH_SPI_CLOCK_MHZ
#define ETH_SPI_CLOCK_MHZ 20
#endif

#ifndef ETH_CONNECT_TIMEOUT_SEC
#define ETH_CONNECT_TIMEOUT_SEC 45
#endif

static const int BIT_CONNECTED = BIT0;
static const int BIT_DISCONNECTED = BIT1;
static EventGroupHandle_t eth_event_group = NULL;
static esp_netif_t *eth_netif = NULL;
static esp_eth_handle_t eth_handle = NULL;
static bool s_is_connected = false;
static bool s_is_static = false;

static void eth_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
    if (event_base == ETH_EVENT) {
        switch (event_id) {
            case ETHERNET_EVENT_CONNECTED:
                INFO("Ethernet Link UP");
                if (s_is_static) {
                    s_is_connected = true;
                    xEventGroupSetBits(eth_event_group, BIT_CONNECTED);
                }
                break;
            case ETHERNET_EVENT_DISCONNECTED:
                WARN("Ethernet Link DOWN");
                s_is_connected = false;
                xEventGroupClearBits(eth_event_group, BIT_CONNECTED);
                xEventGroupSetBits(eth_event_group, BIT_DISCONNECTED);
                break;
            case ETHERNET_EVENT_START:
                INFO("Ethernet Started");
                break;
            case ETHERNET_EVENT_STOP:
                INFO("Ethernet Stopped");
                s_is_connected = false;
                break;
            default:
                break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        INFO("Ethernet Got IP: " IPSTR ", Mask: " IPSTR ", GW: " IPSTR,
             IP2STR(&event->ip_info.ip),
             IP2STR(&event->ip_info.netmask),
             IP2STR(&event->ip_info.gw));
        s_is_connected = true;
        xEventGroupSetBits(eth_event_group, BIT_CONNECTED);
    }
}

static void drive_cs_pin_high(int pin) {
    if (pin < 0) return;
    gpio_config_t io_conf = {};
    io_conf.pin_bit_mask = (1ULL << pin);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);
    gpio_set_level((gpio_num_t)pin, 1);
}

static void deselect_shared_spi_devices(void) {
    drive_cs_pin_high(ETH_CS_PIN);
#ifdef LORA_CS_PIN
    drive_cs_pin_high(LORA_CS_PIN);
#endif
#ifdef TFT_CS_PIN
    drive_cs_pin_high(TFT_CS_PIN);
#endif
#ifdef SD_CS_PIN
    drive_cs_pin_high(SD_CS_PIN);
#endif
}

static void hardware_reset_module(void) {
#if (ETH_RST_PIN >= 0)
    gpio_config_t rst_gpio_cfg = {};
    rst_gpio_cfg.pin_bit_mask = (1ULL << ETH_RST_PIN);
    rst_gpio_cfg.mode = GPIO_MODE_OUTPUT;
    rst_gpio_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    rst_gpio_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    rst_gpio_cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&rst_gpio_cfg);

    INFO("Resetting Ethernet module on GPIO %d...", ETH_RST_PIN);
    gpio_set_level((gpio_num_t)ETH_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    gpio_set_level((gpio_num_t)ETH_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(100)); // Allow oscillator and internal PLL to stabilize
#endif
}

bool eth_connect(const nvs_config_t *config) {
    eth_event_group = xEventGroupCreate();
    s_is_connected = false;

    // 1. Ensure CS lines on the SPI bus are deselected (HIGH) and pulse hardware reset
    deselect_shared_spi_devices();
    hardware_reset_module();

    // 2. Install GPIO ISR handler service for INT pin
    gpio_install_isr_service(0);

    // 3. Initialize SPI bus
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = ETH_MOSI_PIN;
    buscfg.miso_io_num = ETH_MISO_PIN;
    buscfg.sclk_io_num = ETH_SCLK_PIN;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;

    INFO("Initializing SPI%d (MOSI:%d, MISO:%d, SCLK:%d)...",
         ETH_SPI_HOST + 1, ETH_MOSI_PIN, ETH_MISO_PIN, ETH_SCLK_PIN);
    ESP_ERROR_CHECK(spi_bus_initialize(ETH_SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));

    // 4. Configure SPI device
    // Leave command_bits=0 and address_bits=0 so ESP-IDF's per-chip MAC driver
    // (W5500=16/8, DM9051=1/7, KSZ8851SNL=variable) configures its required frame format.
    spi_device_interface_config_t devcfg = {};
    devcfg.mode = 0;
    devcfg.clock_speed_hz = ETH_SPI_CLOCK_MHZ * 1000 * 1000;
    devcfg.spics_io_num = ETH_CS_PIN;
    devcfg.queue_size = 20;

    // 5. Initialize MAC and PHY for selected module
    esp_eth_mac_t *mac = NULL;
    esp_eth_phy_t *phy = NULL;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    // Hardware reset was already performed in hardware_reset_module()
    phy_config.reset_gpio_num = -1;

#if CONFIG_ETH_SPI_ETHERNET_W5500
    if (ETH_MODULE_TYPE == ETH_MODULE_W5500) {
        INFO("Configuring W5500 SPI Ethernet module...");
        eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
        w5500_config.int_gpio_num = ETH_INT_PIN;
        mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);
        phy = esp_eth_phy_new_w5500(&phy_config);
    }
#endif

#if CONFIG_ETH_SPI_ETHERNET_DM9051
    if (ETH_MODULE_TYPE == ETH_MODULE_DM9051) {
        INFO("Configuring DM9051 SPI Ethernet module...");
        eth_dm9051_config_t dm9051_config = ETH_DM9051_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
        dm9051_config.int_gpio_num = ETH_INT_PIN;
        mac = esp_eth_mac_new_dm9051(&dm9051_config, &mac_config);
        phy = esp_eth_phy_new_dm9051(&phy_config);
    }
#endif

#if CONFIG_ETH_SPI_ETHERNET_KSZ8851SNL
    if (ETH_MODULE_TYPE == ETH_MODULE_KSZ8851SNL) {
        INFO("Configuring KSZ8851SNL SPI Ethernet module...");
        eth_ksz8851snl_config_t ksz_config = ETH_KSZ8851SNL_DEFAULT_CONFIG(ETH_SPI_HOST, &devcfg);
        ksz_config.int_gpio_num = ETH_INT_PIN;
        mac = esp_eth_mac_new_ksz8851snl(&ksz_config, &mac_config);
        phy = esp_eth_phy_new_ksz8851snl(&phy_config);
    }
#endif

    if (!mac || !phy) {
        WARN("Failed to create Ethernet MAC/PHY instance for module type %d", (int)ETH_MODULE_TYPE);
        return false;
    }

    // 6. Install driver
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&eth_config, &eth_handle) != ESP_OK || !eth_handle) {
        WARN("Failed to install Ethernet driver");
        return false;
    }

    // 7. Program ESP32 Ethernet MAC into the external SPI Ethernet chip
    // Matches Meshtastic firmware (ETH.begin / SharedBusEthernet::begin) so DHCP assigns the same IP lease.
    uint8_t mac_addr[6] = {0};
    if (esp_read_mac(mac_addr, ESP_MAC_ETH) == ESP_OK) {
        ESP_ERROR_CHECK(esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr));
        INFO("Ethernet MAC: %02x:%02x:%02x:%02x:%02x:%02x",
             mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
    }

    // 8. Attach to Netif
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    eth_netif = esp_netif_new(&netif_cfg);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, esp_eth_new_netif_glue(eth_handle)));

    // 9. Register Event Handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &eth_event_handler, NULL, NULL));

    // 10. Configure Static IP vs DHCP
    if (config && config->has_static_ip && config->static_ip != 0) {
        s_is_static = true;
        INFO("Applying Static IP configuration...");
        ESP_ERROR_CHECK(esp_netif_dhcpc_stop(eth_netif));

        esp_netif_ip_info_t ip_info;
        ip_info.ip.addr = config->static_ip;
        ip_info.gw.addr = config->static_gw;
        ip_info.netmask.addr = config->static_mask ? config->static_mask : inet_addr("255.255.255.0");
        ESP_ERROR_CHECK(esp_netif_set_ip_info(eth_netif, &ip_info));

        if (config->static_dns != 0) {
            esp_netif_dns_info_t dns_info;
            dns_info.ip.u_addr.ip4.addr = config->static_dns;
            dns_info.ip.type = ESP_IPADDR_TYPE_V4;
            ESP_ERROR_CHECK(esp_netif_set_dns_info(eth_netif, ESP_NETIF_DNS_MAIN, &dns_info));
        }

        INFO("Static IP set to: " IPSTR ", GW: " IPSTR,
             IP2STR(&ip_info.ip), IP2STR(&ip_info.gw));
    } else {
        s_is_static = false;
        INFO("Starting DHCP client...");
        ESP_ERROR_CHECK(esp_netif_dhcpc_start(eth_netif));
    }

    // 11. Start Ethernet Driver
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    INFO("Waiting up to %d seconds for Ethernet network link & IP...", ETH_CONNECT_TIMEOUT_SEC);
    EventBits_t bits = xEventGroupWaitBits(eth_event_group,
                                           BIT_CONNECTED,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(ETH_CONNECT_TIMEOUT_SEC * 1000));

    if (bits & BIT_CONNECTED) {
        INFO("Ethernet connected successfully!");
        return true;
    } else {
        WARN("Ethernet connection timed out (no cable or DHCP lease).");
        return false;
    }
}

bool is_eth_connected(void) {
    return s_is_connected;
}
