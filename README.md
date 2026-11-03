# Meshtastic ESP32 Ethernet + BLE Unified OTA Bootloader

[![OTA Build](https://img.shields.io/badge/Build-PlatformIO%20%2F%20ESP--IDF-blue.svg)](https://platformio.org/)
[![License](https://img.shields.io/badge/License-GPLv3-green.svg)](LICENSE)

A specialized, lightweight recovery and update bootloader partition (`app1`) for Meshtastic ESP32 hardware, featuring **Wired Ethernet (W5500, DM9051, KSZ8851SNL)** as the primary update transport, paired with **Bluetooth Low Energy (NimBLE)** as an offline recovery fallback.

Forked from [`meshtastic/esp32-unified-ota`](https://github.com/meshtastic/esp32-unified-ota).

---

## Why Ethernet OTA?

1. **Massive Flash Space Savings**:
   * Stripping the closed-source Espressif Wi-Fi stack eliminates over **200 KB** of binary bloat.
   * Total binary footprint is **~475 KB**, leaving **~180 KB of free headroom** under the 640 KB (`0x0A0000`) partition ceiling.
2. **Zero NVS Credential Handshake Needed**:
   * Unlike Wi-Fi, Ethernet requires no SSID or password to be passed through NVS. It acquires a network configuration via DHCP over the wire on boot.
3. **Static IP & DHCP Support**:
   * Supports dynamic DHCP acquisition or pre-configured Static IP (`ip`, `gw`, `netmask`, `dns`) read directly from the `"MeshtasticOTA"` NVS namespace.
4. **Dual Transport Recovery (Ethernet + BLE)**:
   * Listens on **TCP port 3232** for high-speed wired local updates.
   * Concurrently exposes the Meshtastic BLE OTA GATT service. If the Ethernet cable is disconnected or DHCP times out, BLE is immediately ready for recovery updates.
5. **Anti-Bricking Failsafe**:
   * If neither Ethernet nor BLE establishes an active update session within the timeout window, the bootloader safely restores the boot pointer to `app0` (main Meshtastic firmware) and reboots.

---

## Supported Hardware & Pinouts

| Environment Target | Target Board / MCU | Ethernet Chip | MOSI | MISO | SCLK | CS | INT | RST | SPI Host |
| :--- | :--- | :---: | :---: | :---: | :---: | :---: | :---: | :---: | :---: |
| `station-g3-waveshare-eth-ota` | **Station G3 + Waveshare ESP32-S3-ETH** | W5500 | 11 | 12 | 13 | 14 | 10 | 9 | `SPI3_HOST` |
| `t-eth-elite-ota` | **LilyGO T-ETH-Elite** (ESP32-S3) | W5500 | 21 | 47 | 48 | 45 | 14 | -1 | `SPI3_HOST` |
| `t-connect-pro-ota` | **LilyGO T-Connect-Pro** (ESP32-S3) | W5500 | 11 | 12 | 13 | 10 | 9 | 48 | `SPI3_HOST` |
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
  6. Client streams binary chunks (up to 1024 bytes per chunk).
  7. On completion, device calculates final SHA-256 of the flashed partition:
     * If valid: Device responds `OK\n`, sets `app0` as boot partition, and reboots.
     * If invalid: Device corrupts the corrupt partition header, responds `ERR Hash Mismatch\n`, and aborts.

### 2. Bluetooth Low Energy (BLE Fallback)
* **Service UUID**: `4FAFC201-1FB5-459E-8FCC-C5C9C331914B`
* **OTA Characteristic (Write)**: `62ec0272-3ec5-11eb-b378-0242ac130005`
* **TX Characteristic (Notify)**: `62ec0272-3ec5-11eb-b378-0242ac130003`

---

## License

This project is licensed under the GNU General Public License v3.0 (GPL-3.0) — see [LICENSE](LICENSE) for details.
