# Den LCD-Code präsentieren

> Englische Version: [present-lcd-code.md](present-lcd-code.md)

Notizen für die Präsentation des Treibers `lcd1602`: was du zeigen und sagen kannst, die Zahlen,
die du kennen solltest, und Fragen, die kommen können. Die vollständige Referenz (Verkabelung,
API, Konfiguration) steht in [lcd1602-library.md](lcd1602-library.md) (Englisch). Die
Zeilennummern beziehen sich auf Commit `7fae595`.

> **Die Kernaussage in einem Satz:** Wir haben die mitgelieferte Bibliothek LCD1602, die pro
> Zeichen etwa 6 ms aktiv wartet, zu einem Treiber umgebaut, der unter FreeRTOS funktioniert: Ein
> Gatekeeper-Task besitzt das Display, kein anderer Task wartet je darauf, es werden nur geänderte
> Zeichen gesendet, und die I2C-Übertragung läuft per DMA, geschützt durch einen Mutex. Dabei wird
> jede Zeit aus dem Zeitdiagramm des HD44780 eingehalten.

## 1. Vorgeschlagener Ablauf (ca. 12 Minuten)

| # | Teil | Zeit | Was du zeigst |
|---|---|---|---|
| 1 | Hardware und Verkabelung | 1–2 min | Board, Pegelwandler, LCD; den Schaltplan |
| 2 | Grundlagen aus der Vorlesung | 2–3 min | LCD, HD44780, PCF8574 (Abschnitt 3) |
| 3 | Von LCD1602 zu betriebssystemtauglichem Code | 3 min | Die Tabelle Problem/Lösung (Abschnitt 4) |
| 4 | Code-Durchgang | 4 min | Sieben Stellen im Code (Abschnitt 5) |
| 5 | Demo | 2 min | Tasten, LCD, Log, Display abstecken (Abschnitt 6) |
| 6 | Zusammenfassung und Fragen | 1 min | Abschnitt 9 |

## 2. Hardware und Verkabelung

Schaltplan und Checkliste: [lcd1602-library.md, Abschnitt 2](lcd1602-library.md#2-wiring-with-the-level-shifter-pegelwandler).

Was du sagen kannst:

- Das LCD (TC2004A-01, 20 × 4, HD44780) braucht 5 V für einen guten Kontrast, der STM32 arbeitet
  mit 3 V. Das PCF8574T-Modul hinten am LCD macht aus 4 Leitungen (5V, GND, SDA, SCL) die
  8 parallelen LCD-Signale.
- Dazwischen sitzt der Pegelwandler: **LV = 3V** vom Board, **HV = 5V**, **GND auf beiden Seiten
  verbunden**. SDA (PB7) und SCL (PB8) laufen über je einen Kanal.
- Ein PCF8574 an 5 V braucht für „High“ mindestens 3,5 V (0,7 × VDD), ein 3,3-V-Bus wäre also
  außerhalb der Spezifikation. Der Pegelwandler gibt jeder Seite ihre eigenen Pegel.
- Wir verwenden **PB7/PB8** und nicht PB6/PB9 wie auf den Folien, weil PB6/PB9 auf dem
  STM32F411E-DISCO der I2C-Bus des Audio-Chips und des Beschleunigungssensors sind.

## 3. Grundlagen aus der Vorlesung

### 3.1 Wie ein LCD funktioniert (Folien 3–4)

- Ein LCD leuchtet nicht selbst, es **steuert Licht**: In einer TN-Zelle dreht der Flüssigkristall
  zwischen zwei gekreuzten Polarisatoren das Licht um 90°. Eine Spannung richtet die Moleküle aus,
  und das Pixel wird dunkel. Das Licht kommt von der Hintergrundbeleuchtung.
- Entscheidend ist der **Effektivwert** der Spannung. Gleichspannung zerstört den Flüssigkristall,
  deshalb wird mit Wechselspannung angesteuert.
- **Multiplexbetrieb** spart Anschlüsse: 2 × 16 Zeichen mit 5 × 8 Punkten bräuchten statisch
  1281 Leitungen, mit 8 Commons nur 168. Die Commons (Zeilen) werden nacheinander aktiviert.
- Der Preis ist Kontrast: Auch ein AUS-Pixel bekommt etwas Spannung ab. Bei 16 Commons (HD44780,
  zweizeilig) hat ein EIN-Pixel nur **29 % mehr Effektivspannung** als ein AUS-Pixel (Grenze nach
  Alt-Pleshko, D_max = √(5/3) ≈ 1,29, Bias 1/5). Deshalb gibt es ein **Kontrast-Poti**, und
  deshalb läuft das Modul mit **5 V**.

### 3.2 Der Controller HD44780 (Folien 5–12)

- Der Controller sitzt auf der LCD-Platine und bekommt **Befehle** (RS = 0) und **Daten** (RS = 1).
- **R/W** liegt fest auf „Schreiben“: Das Busy-Flag wird nie gelesen, stattdessen hält der Treiber
  die Ausführungszeiten ein (37 µs pro Befehl, 1,52 ms für Clear/Home).
- **E** (Enable): Beim Schreiben übernimmt der Controller die Daten mit der **fallenden Flanke**
  von E.
- **4-Bit-Modus**: Jedes Byte wird als zwei Nibbles gesendet, das obere zuerst. Das spart
  4 Datenleitungen.
- Das Timing liegt im **Nanosekundenbereich**: t_AS 140 ns, t_PWH 450 ns, t_DSW 195 ns, t_H 10 ns
  (Folie 10). Ein `HAL_Delay(1)` wartet über 2000-mal so lange wie der nötige E-Puls.
- **Initialisierung**: nach dem Einschalten 40–50 ms warten, dreimal `0x3`, dann `0x2` für den
  4-Bit-Modus, dann Function Set `0x28`.
- **DDRAM-Adressen** des 20×4-Displays: `0x00 / 0x40 / 0x14 / 0x54`. Intern gibt es zwei Zeilen
  à 40 Zeichen, „gefaltet“ auf vier Zeilen: Nach dem 20. Zeichen von Zeile 1 kommt Zeile 3.

### 3.3 Das I2C-Modul PCF8574 (Folien 22–26)

- Ein Port-Expander ohne Register: **Jedes Byte erscheint sofort an P7..P0**.
- Oberes Nibble = LCD-Daten D7..D4, unteres Nibble = Licht, E, R/W, RS.
- Das Licht ist nur ein Bit: Es muss in **jedem** Byte gesetzt sein.
- Nach dem Einschalten sind alle Ausgänge High, auch E.
- Adresse `0x27` (PCF8574T) oder `0x3F` (PCF8574AT); die HAL will sie um eins nach links
  geschoben: `0x4E`.
- Bei 100 kHz dauert ein Byte **90 µs**. Das ist viel länger als jede Zeit im Zeitdiagramm,
  deshalb ergeben sich E-Puls und Ausführungszeit von selbst. Nur Clear/Home (2 ms) und die
  Initialisierung brauchen echte Wartezeiten.
- Folie 26: 4 Bytes pro Zeichen, **7,6 ms pro Zeile**, **30 ms für das ganze Display**.

### 3.4 Der Treiber unter FreeRTOS (Folien 27–30)

| Problem des einfachen Treibers (Folie 27) | Folge unter FreeRTOS | Lösung |
|---|---|---|
| `HAL_Delay()` wartet aktiv | Der Task belegt die CPU, niedriger priorisierte Tasks stehen still | `vTaskDelay(pdMS_TO_TICKS(ms) + 1)` |
| `HAL_I2C_Master_Transmit()` pollt | 7,6 ms CPU-Zeit pro Zeile verschwendet | DMA (oder Interrupt) + Semaphore |
| Cursor und Text sind zwei Schritte | Task B schreibt dazwischen: „Zeichensalat“ | Gatekeeper-Task |
| LCD und Sensoren teilen sich I2C1 | `HAL_BUSY` oder zerstörte Übertragungen | Mutex um jede Übertragung |

- **DMA + Semaphore** (Folie 28), sechs Schritte: Mutex holen, DMA starten, `xSemaphoreTake()`
  (der Task blockiert, die CPU ist frei), der DMA überträgt, der Interrupt gibt die Semaphore frei
  (`xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()`), Mutex freigeben. Die Übertragung dauert
  genauso lange, aber die Zeit gehört den anderen Tasks.
- **Gatekeeper** (Folie 29): Nur ein Task schreibt aufs Display. Die anderen schicken Nachrichten
  mit Timeout 0 in eine Queue und blockieren nie.
- **Eine Queue + Bildspeicher statt vier Queues** (Folie 30): ein Wartepunkt, beliebige Position
  und Länge, der neueste Wert gewinnt, Refresh 5-mal pro Sekunde mit nur den Änderungen,
  automatische Neuinitialisierung nach Fehlern.
- Trick aus der Vorlesung: **nur die geänderten Zeichen senden**. Ein geändertes Zeichen kostet
  etwa 1 ms auf dem Bus statt 30 ms für das ganze Display.

#### Die vier Begriffe einfach erklärt

**`vTaskDelay(pdMS_TO_TICKS(ms) + 1)`: schlafen statt aktiv warten.**
`vTaskDelay(n)` legt den Task für `n` Ticks schlafen; in der Zeit arbeitet die CPU die anderen
Tasks ab. `HAL_Delay()` schaut dagegen ständig auf die Uhr und blockiert die CPU. Ein Tick ist der
Herzschlag von FreeRTOS (hier 1000 pro Sekunde, also 1 Tick = 1 ms), und `pdMS_TO_TICKS()` rechnet
Millisekunden in Ticks um. Das `+ 1` ist nötig, weil der nächste Tick fast sofort kommen kann:
`vTaskDelay(1)` dauert irgendwo zwischen 0 und 1 ms, mit `+ 1` schläft der Task mindestens so lange
wie gewünscht.
*Bild:* eine Uhr, die nur zur vollen Minute klickt. Um 10:00:59 heißt „weck mich nach 1 Klick“
schon nach einer Sekunde, deshalb sagt man „2 Klicks“, um sicher eine ganze Minute zu schlafen.

**Semaphore: das Signal „fertig“.**
Einer gibt sie, der andere wartet darauf. Der Task startet die DMA-Übertragung und ruft
`xSemaphoreTake()` auf: Er schläft, ohne CPU-Zeit zu verbrauchen. Wenn das letzte Byte draußen
ist, ruft der Interrupt `xSemaphoreGiveFromISR()` auf, und der Task wacht auf. Eine einfache
Variable als Merker müsste man in einer Schleife abfragen, und das wäre wieder aktives Warten.
*Bild:* der Pager im Selbstbedienungsrestaurant. Man setzt sich und bekommt ein Signal, wenn das
Essen fertig ist, statt alle zehn Sekunden an der Theke zu fragen.

**Gatekeeper-Task: der Einzige, der aufs Display schreibt.**
Die anderen Tasks legen ihren Text in eine Queue und machen sofort weiter. Aufs LCD schreiben sind
zwei Schritte (Cursor setzen, Text schreiben); täten das zwei Tasks gleichzeitig, könnte einer
dazwischenspringen, und die Zeichen landen an der falschen Stelle. Mit nur einem Besitzer werden
die Nachrichten der Reihe nach abgearbeitet. In unserem Code ist das `lcd1602_task()`.
*Bild:* Nur eine Person schreibt auf das Whiteboard; alle anderen geben Zettel ab.

**Mutex: der Schlüssel zu einem gemeinsamen Betriebsmittel.**
Nur der Task mit dem Schlüssel darf den I2C-Bus benutzen; die anderen warten, bis er zurückgegeben
wird (`xSemaphoreTake(mutex)` vor und `xSemaphoreGive(mutex)` nach jeder Übertragung). Anders als
die Semaphore oben, die ein Signal vom Interrupt an den Task ist, nimmt und gibt derselbe Task den
Mutex. Ein Mutex hat außerdem Prioritätsvererbung: Hält ein unwichtiger Task den Schlüssel,
während ein wichtiger darauf wartet, bekommt der unwichtige vorübergehend eine höhere Priorität
und wird schnell fertig.
*Bild:* der Schlüssel zum einzigen Badezimmer.

**Gatekeeper und Mutex zusammen:** Der Gatekeeper schützt das Display (wer schreibt was, in welcher
Reihenfolge), der Mutex schützt den Bus, den auch andere Geräte benutzen können.

### 3.5 Portierbarkeit (Folie 31)

Herstellercode nur im Port. Der Kern (Protokoll, Bus-Logik, Tasks) bleibt unverändert, wenn der
Treiber auf einen anderen Mikrocontroller wandert, z. B. von STM32 zu NXP.

## 4. Von der mitgelieferten Bibliothek LCD1602 zu betriebssystemtauglichem Code mit FreeRTOS

### 4.1 Ausgangspunkt: wie `LCD1602.c` ein Zeichen sendet

Jedes Nibble läuft durch diese Funktion (`LCD1602.c`, Zeile 87):

```c
static void LCD_SendCmd_8bit( uint8_t bt ){
	LCD_WriteByte( portLcd |= 0x04 );   // E = 1
	HAL_Delay( 1 );                     // wartet aktiv 1–2 ms
	LCD_WriteByte( portLcd | bt );      // Daten mit E = 1
	LCD_WriteByte( portLcd &=~ 0x04 );  // E = 0, aber die Datenbits sind hier 0
	HAL_Delay( 1 );                     // wartet aktiv 1–2 ms
}
```

Jedes `LCD_WriteByte()` ist ein eigenes blockierendes `HAL_I2C_Master_Transmit()` mit Adresse und
einem Byte. Ein Zeichen = 1 Byte für RS + 2 Nibbles × 3 Übertragungen = **7 I2C-Übertragungen und
4 × `HAL_Delay(1)`**, also etwa **6 ms pro Zeichen**, in denen die CPU die ganze Zeit wartet
(Folie 21). Ein volles 20×4-Display dauert etwa eine halbe Sekunde.

### 4.2 Was wir geändert haben

| # | Mitgelieferte LCD1602 | Folge unter FreeRTOS | `lcd1602` | Wo |
|---|---|---|---|---|
| 1 | `HAL_Delay(1)` zweimal pro Nibble | Etwa 6 ms aktives Warten pro Zeichen, niedriger priorisierte Tasks stehen still | Keine Wartezeit zwischen den Bytes: Die 90 µs pro I2C-Byte decken alle Zeiten des Diagramms ab. `vTaskDelay()` nur für Clear und Init | [`encode_byte()`](../Core/Src/lcd1602.c#L190), [`port_delay_ms()`](../Core/Src/lcd1602.c#L148) |
| 2 | Eine blockierende Übertragung pro Byte | Die CPU pollt die I2C-Flags, das Adressbyte wird pro Zeichen 7-mal gesendet | Eine Übertragung pro Zeile, gesendet per DMA; der Task schläft auf einer Semaphore | [`dma_transfer()`](../Core/Src/lcd1602.c#L261), [`bus_write()`](../Core/Src/lcd1602.c#L283) |
| 3 | Kein Schutz des I2C-Busses | Kollisionen mit anderen I2C-Geräten (`HAL_BUSY`, zerstörte Übertragungen) | Mutex um jede Übertragung, mit anderen Treibern teilbar | [`bus_write()`](../Core/Src/lcd1602.c#L283), [`lcd1602_start()`](../Core/Src/lcd1602.c#L514) |
| 4 | Jeder Task darf `LCD_SetCursor()` + `LCD_PrintString()` aufrufen | Zwei Schritte: Ein anderer Task kann dazwischen schreiben („Zeichensalat“) | Der Gatekeeper-Task besitzt das Display; die anderen Tasks schicken nur Nachrichten (Timeout 0) | [`lcd1602_print()`](../Core/Src/lcd1602.c#L552), [`lcd1602_task()`](../Core/Src/lcd1602.c#L474) |
| 5 | Schreibt immer ganze Strings neu | Bis zu 30 ms Buszeit für das Display | Bildspeicher: alle 200 ms nur der geänderte Bereich jeder Zeile | [`refresh()`](../Core/Src/lcd1602.c#L385) |
| 6 | `LCD_Init()` in `main()` mit `HAL_Delay()` | Vor dem Scheduler-Start gibt es keinen Tick (Folie 27); keine Wiederherstellung nach Fehlern | Init im Gatekeeper-Task mit `vTaskDelay()`, erneut nach jedem Busfehler | [`display_init()`](../Core/Src/lcd1602.c#L335) |
| 7 | Sendet nach der Umschaltung auf 4 Bit `0x38` (Zeile 236) | DL = 1 schaltet den Controller zurück in den 8-Bit-Modus; funktioniert nur zufällig | Sendet `0x28` | [`display_init()`](../Core/Src/lcd1602.c#L335) |
| 8 | Datenbits werden in dem Byte gelöscht, in dem E fällt (siehe 4.1) | Haltezeit t_H = 0 statt ≥ 10 ns | Bytes 3 und 5 halten die Daten, während E fällt | [`encode_byte()`](../Core/Src/lcd1602.c#L190) |
| 9 | Hardware-Einstellungen im ganzen Code verteilt (`hi2c3`, Adresse, GPIO-Variante, kyrillischer Font) | Portieren heißt den ganzen Treiber ändern | Hardware nur in Abschnitt 1 des Headers und im Port-Teil | [`lcd1602.h`](../Core/Inc/lcd1602.h#L81), [Port-Teil](../Core/Src/lcd1602.c#L73) |
| 10 | Globale Variable `portLcd` | Gemeinsamer Zustand, nicht sicher bei mehreren Tasks | Zustand nur im Gatekeeper-Task | [`lcd1602.c`](../Core/Src/lcd1602.c#L50) |
| 11 | Rückgabewerte der HAL ignoriert | Ein fehlendes Display wird nicht bemerkt, nichts wird wiederholt | Statuscodes, I2C-Reset nach einem Timeout, neuer Versuch alle 2 s, Log-Meldung | [`handle_status()`](../Core/Src/lcd1602.c#L466) |

Gleich geblieben sind: die Bitbelegung des PCF8574, das 4-Bit-Protokoll, die Zeilen-Offsets
`0x00 / 0x40 / 0x14 / 0x54` und die Idee der Initialisierung (dreimal `0x3`, dann `0x2`).

Weggelassen, weil die Anwendung sie nicht braucht: eigene Zeichen (CGRAM), Cursor und Blinken,
Display-Shift, der kyrillische Font und die direkte 4-Bit-GPIO-Variante. Sie ließen sich als
weitere Nachrichtentypen ergänzen.

### 4.3 Warum 5 Bytes pro Zeichen statt 4 (Folie 26)

In der 4-Byte-Folge von Folie 26 steigt E **im selben Byte**, in dem RS und die Daten gesetzt
werden. Der PCF8574 schaltet alle Ausgänge im selben Moment, also ist t_AS ≈ 0 statt ≥ 140 ns.
Das betrifft die Stellen, an denen RS wechselt, z. B. vom Cursor-Befehl (RS = 0) zum ersten
Zeichen (RS = 1). Die meisten Displays verkraften das, aber es liegt außerhalb des Datenblatts.
`lcd1602` schickt vorher ein zusätzliches Byte, in dem RS und die Daten schon gültig sind und E
noch low ist. Damit ist t_AS = 90 µs:

| | Nach `START 0x4E` | Bytes pro Zeichen | Ganze Zeile |
|---|---|---|---|
| Folie 26 | `0x4D 0x49 0x1D 0x19` | 4 | 7,6 ms |
| `lcd1602` | `0x49 0x4D 0x49 0x1D 0x19` | 5 | 9,5 ms |

Das kostet 25 % mehr Buszeit. Das fällt kaum ins Gewicht, weil nur die Änderungen gesendet
werden. Der PC-Test prüft t_AS und t_H für jedes Byte.

### 4.4 Vorher und nachher

| | Mitgelieferte LCD1602 | `lcd1602` |
|---|---|---|
| Ein Zeichen | Etwa 6 ms, die CPU wartet aktiv | 0,45 ms auf dem Bus, mit DMA wenige µs CPU-Zeit |
| Eine Zeile | Etwa 120 ms aktives Warten | Eine Übertragung, 9,5 ms auf dem Bus, CPU frei |
| Ganzes 20×4-Display | Etwa 0,5 s | 38 ms, wenn sich alles geändert hat; meist etwa 1 ms (nur Änderungen) |
| Aufrufender Task | Blockiert, bis der Text draußen ist | Kehrt sofort zurück (Queue, Timeout 0) |
| Mehrere Tasks schreiben | Text kann durcheinandergeraten | Unmöglich: Nur der Gatekeeper schreibt |
| Andere I2C-Geräte | Kollisionen | Mutex |
| Display abgesteckt | Fehler werden ignoriert | Wird erkannt, neuer Versuch alle 2 s, Inhalt wird wiederhergestellt |
| Zeitdiagramm | t_H nicht eingehalten (Daten mit E gelöscht) | Alle Zeiten eingehalten, vom Test geprüft |
| Portieren | HAL-Aufrufe überall | HAL nur im Port-Teil |

## 5. Code-Durchgang

Zeig diese Stellen in dieser Reihenfolge:

1. **Konfiguration** – [`lcd1602.h`, Abschnitt 1](../Core/Inc/lcd1602.h#L81).
   *„Der einzige hardwareabhängige Teil: I2C-Handle, Interrupts, Adresse, Displaygröße. Zum
   Portieren ändern sich dieser Abschnitt und der Port-Teil, sonst nichts.“*
2. **API und Verwendung in der Anwendung** – [`lcd1602.h`, Abschnitt 2](../Core/Inc/lcd1602.h#L131),
   [`app.c` Zeile 205](../Core/Src/app.c#L205) (`lcd1602_start()`), [`App_Display()`](../Core/Src/app.c#L123).
   *„Der KeyTask ruft nur `lcd1602_write_line()` auf. Das kopiert den Text mit Timeout 0 in die
   Queue und kehrt sofort zurück: Der Task wartet nie auf das Display.“*
3. **Zeitdiagramm** – [`encode_byte()`](../Core/Src/lcd1602.c#L190) mit dem Kommentar darüber (ab
   Zeile 154). *„Jedes I2C-Byte ist ein Schritt des Zeitdiagramms: zuerst RS, dann E hoch, dann E
   runter, während die Daten noch anliegen. Weil ein Byte 90 µs dauert, brauchen wir keine
   Wartezeit zwischen den Bytes.“*
4. **DMA + Semaphore + Mutex** – [`dma_transfer()`](../Core/Src/lcd1602.c#L261),
   [`bus_write()`](../Core/Src/lcd1602.c#L283) und die Interrupt-Seite
   [`signal_from_isr()`](../Core/Src/lcd1602.c#L220). Die Kommentare `step 1` bis `step 6` sind
   die sechs Schritte von Folie 28. *„Während der DMA sendet, blockiert der Task auf der Semaphore,
   und die CPU führt die anderen Tasks aus. Der Interrupt gibt die Semaphore frei und ignoriert
   Übertragungen anderer Treiber am selben I2C-Peripheriegerät.“*
5. **Gatekeeper und Bildspeicher** – [`refresh()`](../Core/Src/lcd1602.c#L385) und
   [`lcd1602_task()`](../Core/Src/lcd1602.c#L474). *„frame ist das Soll-Bild, shown das Ist-Bild.
   Alle 200 ms vergleichen wir die beiden und senden pro Zeile nur den Bereich vom ersten bis zum
   letzten geänderten Zeichen, zusammen mit dem Cursor-Befehl, in einer Übertragung.“*
6. **Init** – [`display_init()`](../Core/Src/lcd1602.c#L335). *„Läuft im Task, deshalb verwenden
   die Wartezeiten `vTaskDelay()`. Achtung `0x28`: Das Original sendet `0x38` und schaltet damit
   zurück in den 8-Bit-Modus.“*
7. **Test** – [`sim_port_write()`](../tests/host/test_lcd1602.c#L76) im PC-Test.
   *„Jedes Byte geht in einen simulierten PCF8574 + HD44780, der t_AS und t_H prüft. Steigt E im
   selben Byte, in dem RS wechselt, schlägt der Test fehl.“*

## 6. Demo

Vor der Präsentation:

- Die Verkabelung mit der Checkliste in [lcd1602-library.md](lcd1602-library.md#checklist-before-switching-on)
  prüfen.
- Bauen und flashen (README, „Build and run“). Den Kontrast einstellen.
- Optional: USB-UART-Adapter an PA2 (115200 Baud) für das Log; Logic Analyser an PB7/PB8
  (LV-Seite, 3 V) mit I2C-Decoder.

| Schritt | Tun | Erwartet | Sagen |
|---|---|---|---|
| 1 | Einschalten | Zeile 1 `Keypad demo`, Zeile 2 `Press a key`, Zeile 4 `I2C: DMA` (oder `I2C: blocking` ohne die DMA-Einstellungen in CubeMX) | „Zeile 4 zeigt, ob der Treiber die DMA-Einstellungen gefunden hat“ |
| 2 | `1`..`4` drücken | Die LED schaltet sofort, Zeile 2 zeigt `Key 1 PRESS` / `RELEASE` | „Der KeyTask wartet nie auf das Display“ |
| 3 | `D` gedrückt halten | Das Lauflicht läuft weiter, Zeile 2 zeigt `HOLD` / `REPEAT` | „Die Display-Updates bremsen die anderen Tasks nicht“ |
| 4 | Analyser an SDA/SCL, eine Taste drücken | Ein kurzer Burst: `0x4E`, 5 Bytes Cursor-Befehl, dann 5 Bytes pro geändertem Zeichen | „Nur die geänderten Zeichen, eine Übertragung pro Zeile“ |
| 5 | SDA abziehen, eine Taste drücken | Log `LCD not found` | „Die fehlgeschlagene Übertragung wird erkannt“ |
| 6 | SDA wieder anstecken | Innerhalb von 2 s Log `LCD found ...`, der aktuelle Text ist wieder da | „Automatische Neuinitialisierung; Texte von zwischendurch wurden aufbewahrt“ |

Der Treiber merkt ein fehlendes Display erst, wenn er etwas senden will. Drück deshalb in
Schritt 5 eine Taste.

## 7. Zahlen zum Merken

| | Wert |
|---|---|
| I2C-Byte bei 100 kHz | 90 µs (8 Bit + ACK) |
| Bytes pro Zeichen | 5 (Folie 26: 4) |
| Ein geändertes Zeichen inkl. Cursor-Befehl | etwa 1 ms |
| Ganze Zeile | 9,5 ms (Folie 26: 7,6 ms) |
| Ganzes Display | 38 ms (Folie 26: 30 ms) |
| Originalbibliothek | etwa 6 ms pro Zeichen, aktives Warten |
| Refresh-Periode | 200 ms (5-mal pro Sekunde) |
| Ausführungszeit HD44780 | 37 µs, Clear/Home 1,52 ms |
| Wartezeit nach dem Einschalten | 40–50 ms |
| Zeitdiagramm | t_AS 140 ns, t_PWH 450 ns, t_DSW 195 ns, t_H 10 ns |
| High-Pegel des PCF8574 an 5 V | ≥ 3,5 V (0,7 × VDD) |
| Adresse | 0x27 (HAL: 0x4E) |
| Interrupt-Priorität für `...FromISR` | 5 oder höher (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`) |

## 8. Mögliche Fragen

**Warum ein Pegelwandler, wenn PB7/PB8 5-V-tolerant sind?**
Es würde auch ohne gehen, weil I2C Open-Drain ist und die Pins FT sind. Der Pegelwandler ist die
saubere Lösung: Der STM32 sieht nie 5 V, der PCF8574 sieht volle 5-V-Pegel, und es funktioniert
auch an Pins, die nicht 5-V-tolerant sind.

**Warum ein Mutex, wenn das LCD das einzige I2C-Gerät ist?**
Folie 29: Sensoren werden denselben Bus verwenden. Mit dem Mutex ist der Treiber dafür bereit, und
andere Treiber können denselben Mutex nehmen (`lcd1602_bus_mutex()`).

**Gatekeeper und Mutex: Reicht nicht eins von beiden?**
Sie schützen verschiedene Dinge. Der Gatekeeper schützt das Display (Cursor-Befehl und Text
gehören zusammen, nur ein Schreiber). Der Mutex schützt den Bus gegen andere Geräte.

**Warum ein Mutex und keine binäre Semaphore für den Bus?**
Prioritätsvererbung: Wenn ein hoch priorisierter Sensor-Task auf den Bus wartet, während der
niedrig priorisierte LCD-Task ihn hält, bekommt der LCD-Task vorübergehend die hohe Priorität.
So können Tasks mittlerer Priorität ihn nicht aufhalten (keine Prioritätsinversion).

**Mit DMA wartet der Task trotzdem auf die Semaphore. Was bringt DMA dann?**
CPU-Zeit. Der Task ist blockiert, nicht beschäftigt: Während der 9,5 ms einer Zeile führt der
Scheduler die anderen Tasks aus. Mit der pollenden HAL-Funktion würde die CPU diese 9,5 ms mit dem
Abfragen von Flags verbringen.

**Wie seid ihr die 6 ms pro Zeichen losgeworden?**
Keine Millisekunden-Wartezeiten zwischen den Bytes: Ein I2C-Byte dauert 90 µs, das ist länger als
jede Zeit im Zeitdiagramm und als die 37 µs Ausführungszeit. Nur Clear/Home und die Init brauchen
echte Wartezeiten. Dazu kommt, dass DMA die CPU entlastet und nur geänderte Zeichen gesendet
werden.

**Warum 5 Bytes pro Zeichen und nicht 4 wie auf Folie 26?**
Siehe 4.3: Mit 4 Bytes ändert sich RS im selben Byte, in dem E steigt (t_AS = 0).

**Was passiert, wenn die Queue voll ist?**
Die Nachricht wird verworfen, und die Funktion gibt `LCD1602_ERR_QUEUE_FULL` zurück. Der Sender
wartet nie. Die nächste Nachricht bringt den neuesten Text sowieso.

**Warum alle 200 ms und nicht sofort?**
Mehrere Änderungen innerhalb von 200 ms werden zu einer Übertragung zusammengefasst, so hat die
Buslast eine Obergrenze. 5 Aktualisierungen pro Sekunde reichen fürs Auge (Folie 30). Der Text
erscheint höchstens 0,2 s später.

**Warum `vTaskDelay(pdMS_TO_TICKS(ms) + 1)`?**
`vTaskDelay(1)` wartet bis zum nächsten Tick, und der kann fast sofort kommen. Mit + 1 dauert die
Wartezeit mindestens so lange wie verlangt (Folie 27).

**Warum wird das Display im Task initialisiert und nicht in `main()`?**
Vor dem Scheduler-Start gibt es keinen Tick, also funktioniert `vTaskDelay()` nicht (Folie 27).
Außerdem kann der Task das Display nach einem Fehler erneut initialisieren.

**Warum Priorität 5 für die I2C- und DMA-Interrupts?**
Interrupts, die `xSemaphoreGiveFromISR()` aufrufen, brauchen eine Prioritätsnummer von mindestens
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (5). Mit einer kleineren Nummer würden sie den
Kernel in seinen kritischen Abschnitten unterbrechen.

**Was macht `portYIELD_FROM_ISR()`?**
Wenn die Semaphore einen Task geweckt hat, der eine höhere Priorität hat als der unterbrochene,
wechselt der Scheduler direkt am Ende des Interrupts zu ihm und nicht erst beim nächsten Tick.

**Was passiert, wenn das Display abgesteckt wird?**
Die nächste Übertragung schlägt fehl (kein ACK) oder läuft in einen Timeout. Der Treiber markiert
das Display als nicht bereit, schreibt `LCD not found` ins Log, setzt das I2C-Peripheriegerät nach
einem Timeout zurück und versucht es alle 2 s neu. Nach der neuen Initialisierung wird der ganze
Bildspeicher noch einmal gesendet.

**Was, wenn DMA in CubeMX nicht eingerichtet ist?**
`lcd1602_start()` prüft den DMA-Kanal und die I2C-Interrupts. Fehlt etwas, verwendet der Treiber
blockierendes I2C: Nur der Gatekeeper-Task wartet, die anderen Tasks warten weiterhin nie. Zeile 4
und das Log sagen, warum.

**Warum statische Speicherbelegung?**
Kein Heap nötig, der Speicher ist beim Linken bekannt, und das Anlegen der Objekte kann zur
Laufzeit nicht fehlschlagen.

**Wie würdet ihr den Treiber auf NXP portieren?**
Abschnitt 1 von `lcd1602.h` und die Port-Funktionen ändern (Aufrufe der Hersteller-HAL). Das
Zeitdiagramm, die Bus-Logik mit Mutex und Semaphore, der Gatekeeper-Task und die API bleiben gleich.

**Wie habt ihr ohne Board getestet?**
Ein PC-Test führt den echten Treibercode mit nachgebildetem FreeRTOS und nachgebildeter HAL aus. Ein
simulierter PCF8574 + HD44780 dekodiert jedes Byte und prüft das Zeitdiagramm, die Init und den
Displayinhalt. Zwei absichtlich eingebaute Fehler (E und RS im selben Byte, `0x38`) lassen den
Test fehlschlagen.

**Warum sendet `refresh()` auch unveränderte Zeichen zwischen zwei Änderungen?**
Folie 29: eine Übertragung pro Zeile. Jeder zusätzliche Cursor-Befehl kostet 5 Bytes, so viel wie
ein Zeichen. Bei einer Lücke von ein oder zwei unveränderten Zeichen ist das Mitsenden genauso
billig, und ein Bereich pro Zeile hält den Code einfach.

## 9. Zusammenfassung: was wir gelernt haben

1. **Ein LCD steuert Licht.** Der Effektivwert zählt, Gleichspannung zerstört es, und der
   Multiplexbetrieb begrenzt den Kontrast: daher das Kontrast-Poti und die 5-V-Versorgung.
2. **HD44780:** 4-Bit-Modus, oberes Nibble zuerst, Übernahme mit der fallenden Flanke von E, Init
   mit dreimal `0x3`. Das Timing liegt im Nanosekunden-, nicht im Millisekundenbereich.
3. **PCF8574:** Jedes I2C-Byte setzt alle 8 LCD-Signale. Bei 100 kHz dauert ein Byte 90 µs,
   deshalb ergibt sich das Timing von selbst; das Licht-Bit muss in jedem Byte stehen.
4. **FreeRTOS:** kein aktives Warten (`vTaskDelay`, DMA mit Semaphore), ein Gatekeeper besitzt
   das Display, ein Mutex schützt den gemeinsamen Bus, ein Bildspeicher sendet nur die Änderungen.
5. **Portierbarkeit:** Herstellercode nur im Port; Kern, Bus-Logik und Tasks bleiben unverändert.

## 10. Begriffe Deutsch – Englisch (wie im Code und im Datenblatt)

| Deutsch | Englisch |
|---|---|
| Pegelwandler | level shifter |
| Aktives Warten | busy waiting |
| Bildspeicher / Soll-Bild / Ist-Bild | frame buffer / frame / shown |
| Zeichensalat | mixed-up characters |
| Gatekeeper-Task | gatekeeper task |
| Betriebssystemtauglicher Code | RTOS-ready code |
| Fallende Flanke | falling edge |
| Ausführungszeit | execution time |
| Effektivwert | RMS voltage |
| Kontrast-Poti | contrast trimmer |
| Geteilte Ressource | shared resource |
| Prioritätsinversion / Prioritätsvererbung | priority inversion / priority inheritance |
