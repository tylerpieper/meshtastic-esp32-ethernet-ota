#pragma once

#include "nvs_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ETH_MODULE_W5500 = 0,
    ETH_MODULE_DM9051 = 1,
    ETH_MODULE_KSZ8851SNL = 2,
} eth_module_type_t;

/**
 * @brief Initialize SPI Ethernet interface and connect to network.
 * Supports W5500, DM9051, and KSZ8851SNL modules with DHCP and Static IP.
 * 
 * @param config Pointer to NVS configuration
 * @return true if connected and IP obtained/assigned, false on timeout/failure.
 */
bool eth_connect(const nvs_config_t *config);

/**
 * @brief Check if Ethernet link is physically UP and IP is valid.
 */
bool is_eth_connected(void);

#ifdef __cplusplus
}
#endif
