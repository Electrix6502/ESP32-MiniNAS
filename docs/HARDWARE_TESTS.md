# Offene Abnahme am tatsächlichen Board

Ein physischer ESP32, eine SD-Karte und deine Verdrahtung sind hier nicht
verfügbar. Diese Punkte sind **noch zu prüfen**:

1. Kaltstart/USB-Upload; 16.777.216 Bytes Flash und 8.388.608 Bytes PSRAM im
   Monitor bestätigen. Bei anderer Speicheranbindung Boardprofil anpassen.
2. Statische WLAN-Adresse nach Kaltstart und Router-Neustart prüfen.
3. Nach Upload All Webseite und `/api/status` direkt per IP laden.
4. Ohne SD müssen Flashfreigabe und Webseite weiterarbeiten.
5. SD-Pins für die tatsächliche Anbindung eintragen. FAT32-Karte vor dem
   Einschalten einsetzen. Lesen/Schreiben/Umbenennen/Löschen auf `sd` prüfen.
6. Nicht-FAT32-Karte muss abgewiesen werden; es darf keine automatische
   Formatierung stattfinden.
7. Binärdateien unterschiedlicher Größe auf beide Freigaben schreiben,
   zurücklesen und SHA-256 vergleichen. Viele kleine Dateien mitprüfen.
8. Zwei SMB-Clients und HTTP parallel betreiben. Konkurrierende schreibende
   Handles müssen abgewiesen werden. Schreiben auf eine Webdatei muss für diese
   Datei vorübergehend HTTP 503 verursachen.
9. Volles LittleFS bzw. volle SD: Fehler melden, fremde Dateien erhalten,
   anschließend nach Freigeben von Speicher wieder schreiben können.
10. Reiner Firmware-Upload muss vorhandene LittleFS-Dateien erhalten. LittleFS-
    Image-Upload muss die Datenpartition vollständig durch `data/` ersetzen.
11. 24-Stunden-Lauf, Wiederverbindungen, Speicherverbrauch, offene Handles und
    tatsächlichen Durchsatz prüfen.
12. Windows-Explorer mit erlaubtem Gastzugriff, Linux und macOS-Finder getrennt
    testen. Die Hosttests belegen keine vollständige Dateimanager-Kompatibilität.

Kein zugesicherter Hotplug, keine FAT32-Reparatur und keine Garantie für
beliebige Stromunterbrechungen während Schreibvorgängen.
