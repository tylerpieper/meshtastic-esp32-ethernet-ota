#pragma once
#include "esp_partition.h"

void corrupt_partition(const esp_partition_t *partition);
bool try_restore_app0_boot_partition();
void print_hash(const char *prefix, const uint8_t *hash);
char *getDeviceName();
