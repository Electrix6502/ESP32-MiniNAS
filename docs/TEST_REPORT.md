# Prüfbericht · 22. Oktober 2026

Die aufgeführten Prüfungen beziehen sich auf die bereitgestellte Projektfassung.

## SMB-Hosttests

**Bestanden** mit AddressSanitizer und UndefinedBehaviorSanitizer. Der tatsächliche
Adapter `src/SmbServer.cpp` und dieselbe vendorte Bibliothek wie im ESP32-Build
wurden auf Linux kompiliert. Client: Impacket 0.13.1, echte TCP-Verbindungen auf
Loopback. Als Speicher dienen temporäre POSIX-Verzeichnisse, keine ESP-Hardware.

Für jeden Dialekt **SMB 2.0.2, 2.1, 3.0, 3.0.2 und 3.1.1** geprüft:

- Gastanmeldung.
- Verzeichnis anlegen.
- Binärdatei mit 1.048.591 Bytes schreiben und bytegleich zurücklesen.
- Datei umbenennen und Verzeichnis auflisten.
- Beide Freigaben unabhängig verwenden; Speichertrennung prüfen.
- Pfadüberschreitung mit `../` abweisen.
- Dateien und Verzeichnisse löschen.
- Wiederholtes LOGOFF und neue Client-Verbindungen.

Zusätzlich mit SMB 3.1.1:

- Beliebige mitgesendete Zugangsdaten werden als Gast behandelt.
- 160 lange Unicode-Dateinamen über mehrere Verzeichnis-Antwortblöcke.
- Zwei gleichzeitige Clients.
- Konkurrierender Schreibzugriff wird mit `STATUS_SHARING_VIOLATION` abgewiesen.
- Dateigröße über SET_INFO ändern und tatsächlichen Dateistand prüfen.

Die Leak-Prüfung ist standardmäßig deaktiviert, weil sie Zugriff auf `/proc`
benötigt. Adresszugriffs- und UB-Prüfungen bleiben aktiv; bei entsprechenden
Meldungen schlägt der Test fehl.

## Weboberfläche und Projektdateien

JavaScript-Syntax und JSON-Konfigurationen sind geprüft. Der UI-Test mit
simulierter Status-API bestätigt Statusanzeige, korrekten UNC-Freigabepfad und
den Zustand bei fehlender SD. Das ist keine Prüfung im Browser am echten ESP.

## Offene Hardwaretests

Ein realer ESP32, SD-Karte und Verdrahtung standen nicht zur Verfügung.
WLAN-Durchsatz, Dauerbetrieb, reales Mounten von Flash/SD sowie Windows-Explorer
und macOS-Finder sind noch zu prüfen; siehe `HARDWARE_TESTS.md`.
SMB1/CIFS ist nicht implementiert. Die Hosttests sind keine vollständige
SMB-Konformitätsprüfung.

## Reproduktion

```sh
python -m pip install -r tests/requirements.txt
python scripts/test_native.py --sanitize
node scripts/test_webui.cjs
pio run
```

## ESP32-Build und LittleFS-Image

Der normale PlatformIO-Build ist erfolgreich, mit deaktivierter Telemetrie.
Firmware laut Linker: **910.681 Bytes** von 4.128.768 Bytes; statischer RAM:
**49.712 Bytes** von 327.680 Bytes. Dynamische Netzwerkpuffer und Task-Stacks
sind darin nicht enthalten.

Das LittleFS-Image wird beim normalen Build erzeugt, hat genau **12.582.912
Bytes**, wurde wieder entpackt und mit allen drei Webdateien bytegenau verglichen.
Die erzeugte Partitionstabelle bestätigt `flashfs` bei `0x400000`, Größe
`0xC00000`, Ende bei 16 MiB.

## Entpacktes Projekt

Das ZIP wurde in einen neuen Projektordner entpackt und dort mit leerem
Projekt-Build-Cache erneut erfolgreich gebaut. PlatformIO erzeugte Firmware
und LittleFS-Image gemeinsam. Die bereits installierte System-Toolchain wurde
wiederverwendet. Das Quellarchiv enthält keine Build-Caches oder temporären
Compilerdateien.
