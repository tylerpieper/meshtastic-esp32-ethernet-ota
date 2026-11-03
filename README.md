# Meshtastic ESP32 Ethernet + BLE Unified OTA Bootloader

[![OTA Build](https://img.shields.io/badge/Build-PlatformIO%20%2F%20ESP--IDF-blue.svg)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-GPLv3-green.svg)](LICENSE)

A specialized, lightweight recovery and update bootloader partition (`app1`) for Meshtastic ESP32 hardware, featuring **Wired Ethernet (W5500, DM9051, KSZ8851SNL)** and **Bluetooth Low Energy (NimBLE)** update transports.

Forked from [`meshtastic/esp32-unified-ota`](https://github.com/meshtastic/esp32-unified-ota).

---

## Key Features

1. **Massive Flash Space Savings**:
   * Stripping the Espressif Wi-Fi stack (`esp_wifi` / `wpa_supplicant`) saves over **200 KB** of flash.
   * Total binary footprint is well under the 640 KB (`0x0A0000`) `app1` partition ceiling.
2. **Consistent MAC Address & DHCP Lease Preservation**:
   * Reads the ESP32 factory Ethernet MAC (`esp_read_mac(mac, ESP_MAC_ETH)`) and programs it into the external SPI Ethernet chip via `esp_eth_ioctl(..., ETH_CMD_S_MAC_ADDR, mac)`—matching Meshtastic firmware (`ETH.begin` / `SharedBusEthernet::begin`) so your DHCP server assigns the exact same IP address during OTA.
3. **Static IP & DHCP Support**:
   * Acquires an IP via DHCP by default, or applies a static IPv4 configuration (`ip` / `static_ip`, `gateway` / `static_gw`, `subnet` / `static_mask`, `dns` / `static_dns`) read from the `"MeshtasticOTA"` NVS namespace.
4. **Client-Driven Transport Selection (Ethernet or BLE)**:
   * When OTA is triggered by a network client (`method = 2` / `3`), boots into **Ethernet OTA** on TCP port `3232`.
   * When OTA is triggered by a Bluetooth client (`method = 1`), boots into **BLE OTA** via NimBLE GATT.
5. **Two-Stage Anti-Bricking Failsafe**:
   * **Pre-Erase Standby**: On boot, points the boot partition back to `app0` (if `app0` contains a valid image). If Ethernet link/DHCP fails (`45s` timeout) or no client connects and begins an update within the idle window (`120s` timeout), the bootloader automatically reboots back into `app0` (main Meshtastic firmware).
   * **Erase/Write Protection**: Immediately before `esp_ota_begin()` erases `app0`, locks the boot partition to `app1` (`OTA_1`). If a TCP connection stalls (`30s` `SO_RCVTIMEO`) or drops mid-transfer, the bootloader resets the socket and remains in `app1` so the client can reconnect and complete the flash.

---

## Supported Hardware & Pinouts

| Environment Target | Target Board / MCU | Ethernet Chip | MOSI | MISO | SCLK | CS | INT | RST | SPI Host |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `station-g3-waveshare-eth-ota` | **Station G3 + Waveshare ESP32-S3-ETH** | W5500 | 11 | 12 | 13 | 14 | 10 | 9 | `SPI3_HOST` |
| `t-eth-elite-ota` | **LilyGO T-ETH-Elite** (ESP32-S3) | W5500 | 21 | 47 | 48 | 45 | 14 | -1 | `SPI3_HOST` |
| `t-connect-pro-ota` | **LilyGO T-Connect-Pro** (ESP32-S3, shared SPI) | W5500 | 11 | 13 | 12 | 10 | 9 | 48 | `SPI2_HOST` |
| `esp32s3-dm9051-ota` | **Generic ESP32-S3** | DM9051 | 11 | 12 | 13 | 14 | 10 | 9 | `SPI3_HOST` |
| `esp32s3-ksz8851-ota` | **Generic ESP32-S3** | KSZ8851SNL | 11 | 12 | 13 | 14 | 10 | 9 | `SPI3_HOST` |

---

## Flash Partition Layout (16 MB Flash)

Matches standard Meshtastic `default_16MB.csv`:

```
+------------+------------+---------------+-----------------------------------------------+
| Partition  | Offset     | Size          | Purpose                                       |
+------------+------------+---------------+-----------------------------------------------+
| nvs        | 0x009000   | 0x005000 (20K)| NVS ("MeshtasticOTA" metadata & SHA-256 hash) |
| otadata    | 0x00E000   | 0x002000 (8K) | Boot partition selector                       |
| app0       | 0x010000   | 0x640000 (6MB)| Primary Meshtastic Firmware                   |
| app1       | 0x650000   | 0x640000 (6MB)| Dedicated Ethernet+BLE OTA Recovery Loader    |
| spiffs     | 0xC90000   | 0x360000(3.3M)| Web interface files                           |
+------------+------------+---------------+-----------------------------------------------+
```

---

## Building with PlatformIO

### Prerequisites
* PlatformIO Core 6.2+
* Python 3.10+

### Build Targets

Build the bootloader binary for your board:

```powershell
# Station G3 + Waveshare ESP32-S3-ETH
pio run -e station-g3-waveshare-eth-ota

# LilyGO T-ETH-Elite
pio run -e t-eth-elite-ota

# LilyGO T-Connect-Pro
pio run -e t-connect-pro-ota

# Generic DM9051 / KSZ8851SNL
pio run -e esp32s3-dm9051-ota
pio run -e esp32s3-ksz8851-ota
```

The resulting binary will be in `.pio/build/<environment>/firmware.bin`.

---

## Flashing to Hardware

Flash the generated `firmware.bin` directly to the `app1` (`ota_1`) partition:

### 16 MB Boards (Station G3 / Waveshare ESP32-S3-ETH):
```powershell
esptool.py --chip esp32s3 -p COM_PORT write_flash 0x650000 .pio/build/station-g3-waveshare-eth-ota/firmware.bin
```

### 4 MB Boards:
```powershell
esptool.py -p COM_PORT write_flash 0x260000 .pio/build/<environment>/firmware.bin
```

---

## Protocol Specification

### 1. Network Discovery & Handshake (Ethernet TCP)
* **UDP Broadcast**: Device broadcasts its presence on UDP port `3232` every 1 second:
  `"<device_name> <version>"`
* **TCP Connection**: Client connects to device IP on TCP port `3232`.
* **Handshake**:
  1. Client sends: `VERSION\n`
  2. Device responds: `OK <hw_ver> <fw_ver> <reboot_count> <git_hash>\n`
  3. Client sends: `OTA <size_in_bytes> <sha256_hex_string>\n`
  4. Device validates SHA-256 against pre-stored NVS hash.
  5. Device responds: `ERASING\n` then `OK\n`.
  6. Client streams binary chunks (up to 1024 bytes per chunk), receiving `ACK\n` per chunk.
  7. On completion, device calculates final SHA-256 of the flashed partition:
     * If valid: Device responds `OK\n`, sets `app0` as boot partition, and reboots.
     * If invalid: Device corrupts the target partition header, responds `ERR Hash Mismatch\n`, and aborts.

### 2. Bluetooth Low Energy (BLE OTA)
* **Service UUID**: `4FAFC201-1FB5-459E-8FCC-C5C9C331914B`
* **OTA Characteristic (Write)**: `62ec0272-3ec5-11eb-b378-0242ac130005`
* **TX Characteristic (Notify)**: `62ec0272-3ec5-11eb-b378-0242ac130003`

---

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0) — see [LICENSE](LICENSE) for details.
