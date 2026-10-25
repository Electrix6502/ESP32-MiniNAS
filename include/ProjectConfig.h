#pragma once
#include <stdint.h>

// Central configuration. Enter your WLAN, a free IP and the SD pins here.
namespace Config {
constexpr char WIFI_SSID[] = "ssid";
constexpr char WIFI_PASSWORD[] = "password";
constexpr char HOSTNAME[] = "espnas";
constexpr uint8_t IP[4] = {192, 168, 178, 15};
constexpr uint8_t NETMASK[4] = {255, 255, 255, 0};
constexpr uint8_t GATEWAY[4] = {192, 168, 178, 1};
constexpr uint8_t DNS[4] = {192, 168, 178, 1};
constexpr uint16_t HTTP_PORT = 80;
constexpr uint16_t SMB_PORT = 445;
constexpr char FLASH_SHARE[] = "flash";
constexpr char SD_SHARE[] = "sd";

enum class SdBus { SPI, SDMMC_1BIT, SDMMC_4BIT };
constexpr SdBus SD_BUS = SdBus::SPI;
// -1: not assigned. Firmware builds; SD remains unavailable.
constexpr int SD_SCK = 12;
constexpr int SD_MISO = 13;
constexpr int SD_MOSI = 11;
constexpr int SD_CS = 14;
constexpr int SD_CLK = -1;
constexpr int SD_CMD = -1;
constexpr int SD_D0 = -1;
constexpr int SD_D1 = -1;
constexpr int SD_D2 = -1;
constexpr int SD_D3 = -1;
// 20 MHz is a reliable baseline for short SPI wiring and is twice the
// previous throughput ceiling. Reduce to 10 MHz if the card becomes unstable.
constexpr uint32_t SD_SPI_HZ = 40000000;
constexpr uint32_t SDMMC_KHZ = 10000;

// Activity LEDs, active HIGH.
constexpr int LED_WRITE_PIN = 2;
constexpr int LED_READ_PIN = 1;
constexpr int LED_READY_PIN = 7;
constexpr uint32_t LED_ACTIVITY_MS = 120;

// File managers commonly keep one connection for directory browsing and
// open additional ones for writes, metadata and thumbnails. Two connections
// make the share appear read-only once both volumes are open.
constexpr unsigned SMB_MAX_CLIENTS = 5;
constexpr unsigned SMB_MAX_HANDLES = 12;
// Fewer, larger SMB requests substantially reduce protocol overhead on SPI SD.
constexpr uint32_t SMB_IO_SIZE = 32768;
constexpr uint32_t SMB_TASK_STACK = 16384;
constexpr uint32_t WIFI_RETRY_MS = 10000;
// Keep false: failed mount must not erase existing files.
constexpr bool FORMAT_FLASH_ON_MOUNT_FAILURE = false;
} // namespace Config
