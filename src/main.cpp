#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include "ProjectConfig.h"
#include "Storage.h"
#include "SmbServer.h"
#include "HttpService.h"
#include "StatusLeds.h"
namespace {
bool servicesStarted = false, wasConnected = false;
uint32_t lastReconnect = 0;
IPAddress address(const uint8_t (&a)[4]) {
    return IPAddress(a[0], a[1], a[2], a[3]);
}
} // namespace
void setup() {
    Serial.begin(115200);
    StatusLeds::begin();
    delay(2500);
    StatusLeds::setReady(true);
    Serial.printf("\nESP32-MiniNAS | Flash: %u | PSRAM: %u\n", ESP.getFlashChipSize(),
                  ESP.getPsramSize());
    if (ESP.getFlashChipSize() != 16 * 1024 * 1024)
        Serial.println("WARN: 16 MiB Flash nicht erkannt");
    if (ESP.getPsramSize() < 8 * 1024 * 1024)
        Serial.println("WARN: 8 MiB PSRAM nicht erkannt; Boardprofil pruefen");
    Storage::begin();
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(Config::HOSTNAME);
    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    if (!WiFi.config(address(Config::IP), address(Config::GATEWAY), address(Config::NETMASK),
                     address(Config::DNS))) {
        Serial.println("FATAL: Statische IP-Konfiguration fehlgeschlagen");
        while (true)
            delay(1000);
    }
    WiFi.begin(Config::WIFI_SSID, Config::WIFI_PASSWORD);
    Serial.printf("Verbinde WLAN %s, statische IP %s\n", Config::WIFI_SSID,
                  address(Config::IP).toString().c_str());
}
void loop() {
    bool connected = WiFi.status() == WL_CONNECTED;
    if (connected && !wasConnected) {
        Serial.printf("WLAN verbunden: http://%s/\n", WiFi.localIP().toString().c_str());
        if (!servicesStarted) {
            HttpService::begin();
            SmbServer::begin();
            servicesStarted = true;
            StatusLeds::setReady(true);
        }
        MDNS.end();
        if (MDNS.begin(Config::HOSTNAME)) {
            MDNS.addService("http", "tcp", Config::HTTP_PORT);
            MDNS.addService("smb", "tcp", Config::SMB_PORT);
        }
    }
    if (!connected && uint32_t(millis() - lastReconnect) >= Config::WIFI_RETRY_MS) {
        lastReconnect = millis();
        WiFi.reconnect();
        Serial.println("WLAN: erneuter Verbindungsversuch");
    }
    wasConnected = connected;
    StatusLeds::setReady(connected && servicesStarted);
    StatusLeds::loop();
    if (connected && servicesStarted)
        HttpService::loop();
    delay(2);
}
