#include "HttpService.h"
#include "ProjectConfig.h"
#include "Storage.h"
#include "SmbServer.h"
#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>
#include <WebServer.h>
#include <esp_timer.h>
namespace {
WebServer http(Config::HTTP_PORT);
String quote(const String &value) {
    String out;
    out += char(34);
    for (size_t i = 0; i < value.length(); ++i) {
        unsigned char c = value[i];
        if (c == 34 || c == 92) {
            out += char(92);
            out += char(c);
        } else if (c >= 32)
            out += char(c);
    }
    out += char(34);
    return out;
}
String number(uint64_t n) {
    char b[24];
    snprintf(b, sizeof(b), "%llu", static_cast<unsigned long long>(n));
    return b;
}
void status() {
    StorageGuard guard;
    String json = "{\"hostname\":" + quote(Config::HOSTNAME);
    json += ",\"ip\":" + quote(WiFi.localIP().toString());
    json += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
    json += ",\"rssi\":" + String(WiFi.RSSI());
    json += ",\"uptimeSeconds\":" + number(esp_timer_get_time() / 1000000ULL);
    json += ",\"heapFree\":" + number(ESP.getFreeHeap());
    json += ",\"psramSize\":" + number(ESP.getPsramSize());
    json += ",\"psramFree\":" + number(ESP.getFreePsram());
    json += ",\"smbReady\":" + String(SmbServer::ready() ? "true" : "false");
    json += ",\"smbClients\":" + String(SmbServer::clients());
    json += ",\"smbHandles\":" + String(SmbServer::handles());
    json += ",\"sdStatus\":" + quote(Storage::sdStatus());
    json += ",\"volumes\":[";
    for (unsigned v = 0; v < 2; ++v) {
        if (v)
            json += ',';
        json += "{\"name\":" + quote(Storage::volumes[v].name.c_str());
        json += ",\"ready\":" + String(Storage::volumes[v].ready ? "true" : "false");
        json += ",\"filesystem\":" + quote(v == 0 ? "LittleFS" : "FAT32");
        json += ",\"total\":" + number(Storage::total(v));
        json += ",\"used\":" + number(Storage::used(v)) + "}";
    }
    json += "]}";
    http.sendHeader("Cache-Control", "no-store");
    http.send(200, "application/json; charset=utf-8", json);
}
int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
bool decode(const String &raw, std::string &out) {
    if (raw.length() > 480)
        return false;
    for (size_t i = 0; i < raw.length(); ++i) {
        char c = raw[i];
        if (c == '%') {
            if (i + 2 >= raw.length())
                return false;
            int a = hex(raw[i + 1]), b = hex(raw[i + 2]);
            if (a < 0 || b < 0)
                return false;
            c = char(a * 16 + b);
            i += 2;
        }
        if (!c)
            return false;
        out += c;
    }
    return true;
}
const char *mime(const String &p) {
    if (p.endsWith(".html"))
        return "text/html; charset=utf-8";
    if (p.endsWith(".css"))
        return "text/css; charset=utf-8";
    if (p.endsWith(".js"))
        return "application/javascript; charset=utf-8";
    if (p.endsWith(".json"))
        return "application/json";
    if (p.endsWith(".svg"))
        return "image/svg+xml";
    if (p.endsWith(".png"))
        return "image/png";
    if (p.endsWith(".jpg") || p.endsWith(".jpeg"))
        return "image/jpeg";
    if (p.endsWith(".ico"))
        return "image/x-icon";
    if (p.endsWith(".woff2"))
        return "font/woff2";
    return "application/octet-stream";
}
void staticFile() {
    if (http.method() != HTTP_GET && http.method() != HTTP_HEAD) {
        http.send(405, "text/plain", "GET oder HEAD erforderlich");
        return;
    }
    StorageGuard guard;
    if (!Storage::volumes[0].ready) {
        http.send(503, "text/plain",
                  "LittleFS nicht verfuegbar. Bitte Upload LittleFS ausfuehren.");
        return;
    }
    std::string uri;
    if (!decode(http.uri(), uri)) {
        http.send(400, "text/plain", "Ungueltiger Pfad");
        return;
    }
    if (uri.empty() || uri.back() == '/')
        uri += "index.html";
    std::string resolved;
    if (!Storage::resolve(0, uri.c_str(), resolved)) {
        http.send(400, "text/plain", "Ungueltiger Pfad");
        return;
    }
    std::string absolute =
        Storage::volumes[0].root + "/www" + resolved.substr(Storage::volumes[0].root.size());
    if (SmbServer::writeOpen(absolute)) {
        http.sendHeader("Retry-After", "1");
        http.send(503, "text/plain", "Datei wird gerade geschrieben");
        return;
    }
    String fileName = absolute.substr(Storage::volumes[0].root.size()).c_str();
    File file = LittleFS.open(fileName, "r");
    if (!file || file.isDirectory()) {
        http.send(404, "text/plain", "Datei nicht gefunden");
        return;
    }
    http.sendHeader("Cache-Control", "no-cache");
    http.sendHeader("X-Content-Type-Options", "nosniff");
    if (http.method() == HTTP_HEAD) {
        http.setContentLength(file.size());
        http.send(200, mime(fileName), "");
    } else
        http.streamFile(file, mime(fileName));
    file.close();
}
} // namespace
namespace HttpService {
void begin() {
    http.on("/api/status", HTTP_GET, status);
    http.onNotFound(staticFile);
    http.begin();
}
void loop() {
    http.handleClient();
}
} // namespace HttpService
