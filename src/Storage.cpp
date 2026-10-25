#include "Storage.h"
#include "ProjectConfig.h"
#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SD.h>
#include <SD_MMC.h>
#include <SPI.h>
#include <ff.h>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
namespace {
SemaphoreHandle_t storageMutex = nullptr;
SPIClass sdSPI(FSPI);
String sdState = "Pins nicht konfiguriert";
bool spiMode = true;
} // namespace
StorageGuard::StorageGuard() {
    if (storageMutex)
        xSemaphoreTakeRecursive(storageMutex, portMAX_DELAY);
}
StorageGuard::~StorageGuard() {
    if (storageMutex)
        xSemaphoreGiveRecursive(storageMutex);
}
namespace Storage {
Volume volumes[2] = {{Config::FLASH_SHARE, "/littlefs", false, false},
                     {Config::SD_SHARE, "/sd", false, true}};
void begin() {
    storageMutex = xSemaphoreCreateRecursiveMutex();
    if (!storageMutex) {
        Serial.println("FATAL: Storage-Mutex fehlt");
        abort();
    }
    volumes[0].ready =
        LittleFS.begin(Config::FORMAT_FLASH_ON_MOUNT_FAILURE, "/littlefs", 16, "flashfs");
    Serial.printf("LittleFS: %s\n",
                  volumes[0].ready ? "bereit" : "Mount fehlgeschlagen; Upload LittleFS ausfuehren");
    bool mounted = false;
    spiMode = Config::SD_BUS == Config::SdBus::SPI;
    if (spiMode) {
        if (Config::SD_SCK < 0 || Config::SD_MISO < 0 || Config::SD_MOSI < 0 || Config::SD_CS < 0)
            return;
        sdSPI.begin(Config::SD_SCK, Config::SD_MISO, Config::SD_MOSI, Config::SD_CS);
        mounted = SD.begin(Config::SD_CS, sdSPI, Config::SD_SPI_HZ, "/sd", 16, false);
    } else {
        if (Config::SD_CLK < 0 || Config::SD_CMD < 0 || Config::SD_D0 < 0)
            return;
        bool oneBit = Config::SD_BUS == Config::SdBus::SDMMC_1BIT;
        if (!oneBit && (Config::SD_D1 < 0 || Config::SD_D2 < 0 || Config::SD_D3 < 0))
            return;
        bool pins = oneBit ? SD_MMC.setPins(Config::SD_CLK, Config::SD_CMD, Config::SD_D0)
                           : SD_MMC.setPins(Config::SD_CLK, Config::SD_CMD, Config::SD_D0,
                                            Config::SD_D1, Config::SD_D2, Config::SD_D3);
        if (!pins) {
            sdState = "Ungueltige SDMMC-Pins";
            return;
        }
        mounted = SD_MMC.begin("/sd", oneBit, false, Config::SDMMC_KHZ, 16);
    }
    if (!mounted) {
        sdState = "Keine Karte oder Mount fehlgeschlagen";
        return;
    }
    // This firmware mounts exactly one FatFs volume; LittleFS uses its own VFS.
    FATFS *fs = nullptr;
    DWORD freeClusters = 0;
    bool fat32 = f_getfree("0:", &freeClusters, &fs) == FR_OK && fs && fs->fs_type == FS_FAT32;
    if (!fat32) {
        if (spiMode)
            SD.end();
        else
            SD_MMC.end();
        sdState = "Karte ist nicht FAT32";
        return;
    }
    volumes[1].ready = true;
    volumes[1].fat32 = true;
    sdState = "FAT32 bereit";
    Serial.println("SD: FAT32 bereit");
}
uint64_t total(unsigned v) {
    StorageGuard l;
    if (v > 1 || !volumes[v].ready)
        return 0;
    return v == 0 ? LittleFS.totalBytes() : (spiMode ? SD.totalBytes() : SD_MMC.totalBytes());
}
uint64_t used(unsigned v) {
    StorageGuard l;
    if (v > 1 || !volumes[v].ready)
        return 0;
    return v == 0 ? LittleFS.usedBytes() : (spiMode ? SD.usedBytes() : SD_MMC.usedBytes());
}
const char *sdStatus() {
    return sdState.c_str();
}
} // namespace Storage
