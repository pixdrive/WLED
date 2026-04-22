# PixDrive SD-Karten Erweiterung für WLED

## Voraussetzungen

- WLED Controller mit PixDrive Firmware
- SD-Karten Board (z.B. Zedfy ZW-SD + C012)
- microSD-Karte (FAT32 formatiert)
- `.wled` Animationsdateien (erstellt mit PixDrive Studio)

## SD-Karten Board anschließen

### Zedfy ZW-SD + C012

Das Zedfy ZW-SD Board wird über SPI an den ESP32 angeschlossen. Bei Verwendung mit dem C012 Controller sind die GPIOs bereits vorkonfiguriert:

| Signal | GPIO | Beschreibung        |
|--------|------|---------------------|
| CS     | 33   | Chip Select         |
| SCK    | 27   | Clock               |
| MISO   | 26   | Data Out (SD → ESP) |
| MOSI   | 25   | Data In (ESP → SD)  |

### Andere SD-Karten Boards

Die GPIO-Belegung kann in den WLED Einstellungen angepasst werden:

1. WLED Web-Oberfläche öffnen
2. **Einstellungen** → **Usermods**
3. Unter **SD Card** die GPIO-Pins entsprechend der eigenen Verkabelung anpassen
4. Speichern und Neustart abwarten

## SD-Karten Manager verwenden

Der PixDrive SD-Karten Manager ist über den Browser erreichbar:

```
http://<WLED-IP>/pixdrive
```

### Dateien hochladen

1. `/pixdrive` im Browser aufrufen
2. `.wled` Datei per Drag & Drop in die Upload-Zone ziehen oder Datei über den Button auswählen
3. Die Datei wird automatisch auf Integrität geprüft (CRC32)
4. Nach erfolgreichem Upload erscheint die Datei in der Dateiliste

### Dateien verwalten

Die Dateiliste zeigt alle `.wled` Dateien auf der SD-Karte mit folgenden Informationen:

- Dateiname
- Dateigröße
- Anzahl LEDs und Frames
- FPS (Bilder pro Sekunde)
- RGB/RGBW Modus

Dateien können über den Löschen-Button einzeln entfernt werden.

## PixDrive Effekt verwenden

Die Wiedergabe von `.wled` Animationen erfolgt über den WLED Effekt **PixDrive**.

### Einrichtung

1. WLED Web-Oberfläche öffnen
2. Ein **Segment** auswählen oder erstellen
3. In der Effektliste den Effekt **PixDrive** auswählen
4. Im **Segment-Namen** den Dateipfad der gewünschten Animation eintragen:
   ```
   /pixdrive/meine_animation.wled
   ```
5. Die Animation startet automatisch

### Mehrere Animationen gleichzeitig

Jedes WLED Segment kann eine eigene Animation abspielen. So lassen sich mehrere LED-Streifen unabhängig voneinander mit verschiedenen Animationen bespielen:

1. Für jeden LED-Streifen ein eigenes Segment anlegen
2. Jedem Segment den Effekt **PixDrive** zuweisen
3. In jedem Segment-Namen einen anderen Dateipfad eintragen

### Wiedergabe steuern

Die Animation läuft in einer Endlosschleife mit der in der `.wled` Datei hinterlegten FPS-Rate. Über die WLED Oberfläche kann:

- **Speed**: Wiedergabegeschwindigkeit anpassen
- **Brightness**: Helligkeit regeln
- Segment ein-/ausschalten → startet/stoppt die Wiedergabe
