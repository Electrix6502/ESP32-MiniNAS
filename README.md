# ESP32-MiniNAS

Ein kleines WLAN-NAS auf Basis eines **ESP32-S3 mit 16 MiB Flash und 8 MiB
PSRAM**. Die Firmware stellt den internen Flash und eine FAT32-SD-Karte als
SMB-Gastfreigaben bereit. Eine Weboberfläche zeigt den Speicher- und
Systemstatus an.

Das Projekt verwendet Arduino und PlatformIO. Ein passendes Boardprofil für
QSPI-Flash und OPI-PSRAM ist enthalten, beispielsweise für ein
ESP32-S3-WROOM-1-N16R8.

## Funktionen

- SMB2/3 über TCP-Port 445: Dateien lesen, schreiben, auflisten, umbenennen und
  löschen sowie Verzeichnisse verwalten.
- Zwei Freigaben: `flash` für die 12-MiB-LittleFS-Partition und `sd` für die SD-Karte.
- Weboberfläche mit Speicherbelegung, WLAN-Signal, Laufzeit, Heap/PSRAM und
  SMB-Verbindungen; Statusdaten unter `/api/status`.
- Statische IPv4-Adresse und mDNS-Hostname `espnas.local`.
- SD-Anbindung per SPI, SDMMC mit einem oder vier Datenbits.
- LEDs für Betriebsbereitschaft sowie Lese- und Schreibaktivität.

## Voraussetzungen

- ESP32-S3 mit 16 MiB QSPI-Flash und 8 MiB OPI-PSRAM sowie USB-Verbindung.
- 2,4-GHz-WLAN und eine freie bzw. reservierte IP-Adresse im eigenen Netz.
- Optional: FAT32-formatierte SD-Karte und passende SD-Beschaltung.
- [PlatformIO](https://platformio.org/) als CLI oder als Erweiterung für VS Code.
  Beim ersten Build werden die benötigten Pakete heruntergeladen.

## Einrichtung

1. Repository klonen oder herunterladen und den Projektordner in VS Code öffnen.
2. In [`include/ProjectConfig.h`](include/ProjectConfig.h) `WIFI_SSID` und
   `WIFI_PASSWORD` eintragen. `IP`, `NETMASK`, `GATEWAY` und `DNS` an das eigene
   Netzwerk anpassen. Aktuell ist `192.168.178.15` voreingestellt.
3. SD-Pins und LED-Pins mit der tatsächlichen Verdrahtung abgleichen.
4. Im PlatformIO-Terminal Firmware und Dateisystem bauen und aufspielen:

   ```sh
   pio run
   pio run -t upload
   pio run -t uploadfs
   pio device monitor
   ```

Der serielle Monitor verwendet **115200 Baud**. Falls PlatformIO den USB-Port
nicht eindeutig erkennt, in [`platformio.ini`](platformio.ini) `upload_port`
auf den passenden Gerätepfad bzw. COM-Port setzen.

### Aktuelle Pinbelegung

Die folgenden Werte stammen aus `include/ProjectConfig.h`; sie sind keine
universelle Belegung für jedes ESP32-S3-Board.

| Funktion | Konfiguration | GPIO |
|---|---|---:|
| SD-Takt (SPI) | `SD_SCK` | 12 |
| SD-Daten zum ESP32 | `SD_MISO` | 13 |
| SD-Daten zur Karte | `SD_MOSI` | 11 |
| SD-Chip-Select | `SD_CS` | 14 |
| Schreib-LED | `LED_WRITE_PIN` | 2 |
| Lese-LED | `LED_READ_PIN` | 1 |
| Bereitschafts-LED | `LED_READY_PIN` | 7 |

Standardmäßig ist `SdBus::SPI` mit **40 MHz** eingestellt. Bei instabiler
SD-Verbindung den Takt reduzieren und die Verdrahtung prüfen. Die LEDs werden
im aktuellen Treiber **aktiv LOW** angesteuert.

Für `SdBus::SDMMC_1BIT` müssen `SD_CLK`, `SD_CMD` und `SD_D0` gesetzt werden;
für `SdBus::SDMMC_4BIT` zusätzlich `SD_D1`, `SD_D2` und `SD_D3`. Die erforderlichen
Pull-ups und die Pinbelegung des konkreten Boards beachten. GPIOs für Flash,
PSRAM oder USB dürfen nicht mit der Beschaltung kollidieren.

Die SD-Karte muss FAT32 verwenden; FAT12, FAT16 und exFAT werden abgewiesen.
Die Firmware formatiert die Karte nicht. Ohne nutzbare SD-Karte bleiben
Flashfreigabe und Webserver verfügbar. Die Karte nur bei ausgeschaltetem Gerät
wechseln; Hotplug ist nicht implementiert.

## Zugriff im Netzwerk

Mit der aktuell voreingestellten IP-Adresse:

| Dienst | Adresse |
|---|---|
| Weboberfläche | `http://192.168.178.15/` |
| Status-API | `http://192.168.178.15/api/status` |
| Flash unter Windows | `\\192.168.178.15\flash` |
| SD unter Windows | `\\192.168.178.15\sd` |
| Flash unter macOS/Linux | `smb://192.168.178.15/flash` |
| SD unter macOS/Linux | `smb://192.168.178.15/sd` |

Bei mDNS-Unterstützung ist auch `espnas.local` erreichbar. Die SMB-Freigaben
direkt mit ihrem Namen öffnen: Freigabenauflistung über `\\IP` und automatische
Erkennung in der Windows-Netzwerkumgebung sind nicht implementiert.

**Die Freigaben erlauben Gastzugriff ohne Passwortprüfung.** Jeder erreichbare
Client kann Dateien lesen, verändern und löschen. Das Gerät daher nur in einem
vertrauenswürdigen lokalen Netz betreiben. WLAN-Zugangsdaten werden weiterhin
für die Verbindung zum Router benötigt.

SMB-Signierung und -Verschlüsselung werden für die Gastverbindungen nicht
angeboten. Clients, die Gastzugriff sperren oder Signierung verlangen, können
deshalb nicht zugreifen.

## Weboberfläche und Flash-Dateien

[`data/www/`](data/www/) enthält die Weboberfläche. Beim normalen Build erzeugt
[`scripts/build_images.py`](scripts/build_images.py) auch das LittleFS-Image.
Die Images liegen unter `.pio/build/esp32s3_n16r8/`.

- `pio run -t upload` aktualisiert die Firmware und erhält die Flash-Dateien.
- `pio run -t uploadfs` ersetzt die gesamte LittleFS-Partition durch `data/`.
  Dabei gehen Dateien verloren, die nur auf dem Gerät vorhanden sind.

Die Webseite lässt sich alternativ direkt über die Freigabe `flash` im Ordner
`www` bearbeiten. Dafür ist kein Firmware-Upload notwendig. Während eine Datei
per SMB zum Schreiben geöffnet ist, liefert HTTP für diese Datei den Status 503.

## Grenzen

- Aktuell fünf SMB-Clients, zwölf offene Handles und 32-KiB-Transferblöcke;
  konfigurierbar in `ProjectConfig.h`.
- Maximale Dateigröße: **2 GiB minus 1 Byte**, auch auf der SD-Karte.
- Kein SMB1, keine Windows-ACLs, Byte-Range-Locks, Oplocks/Leases oder Change
  Notifications.
- Keine Firmware-OTA-Slots. Die Partitionierung ist in
  [`partitions.csv`](partitions.csv) festgelegt.
- Die nutzbare Flash-Kapazität liegt wegen Dateisystem-Metadaten unter 12 MiB.

## Tests und Projektstruktur

Optionale Hosttests mit Python, GCC/G++ und Node.js:

```sh
python -m pip install -r tests/requirements.txt
python scripts/test_native.py --sanitize
node scripts/test_webui.cjs
```

Den dokumentierten Prüfstand enthält [`docs/TEST_REPORT.md`](docs/TEST_REPORT.md).
Für die Prüfung am Board siehe [`docs/HARDWARE_TESTS.md`](docs/HARDWARE_TESTS.md).
Hosttests ersetzen keinen Test mit der tatsächlichen Hardware und SD-Karte.

| Pfad | Inhalt |
|---|---|
| `src/` | Firmware, Speicherzugriff, SMB- und HTTP-Dienst |
| `include/` | Konfiguration und Schnittstellen |
| `data/www/` | Weboberfläche im LittleFS-Image |
| `boards/` | PlatformIO-Boardprofil |
| `lib/libsmb2/` | Mitgelieferte, angepasste SMB-Bibliothek |
| `scripts/`, `tests/` | Build-Helfer und Hosttests |

## Lizenz

Der Projektcode steht unter **LGPL-2.1-or-later**, siehe [`LICENSE`](LICENSE)
und [`LICENSES/`](LICENSES/). Lizenzhinweise und lokale Änderungen der enthaltenen
SMB-Bibliothek befinden sich unter [`lib/libsmb2/`](lib/libsmb2/), insbesondere
in [`PATCHES.md`](lib/libsmb2/PATCHES.md).
