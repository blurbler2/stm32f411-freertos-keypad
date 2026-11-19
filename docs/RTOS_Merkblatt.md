# RTOS-Merkblatt – Echtzeit-Betriebssysteme auf dem STM32

Zusammenfassung zu *Ausgewählte Kapitel der Informatik* (FH Campus Wien, Paulis/Fischer, WS 2026).
Grundlage sind die Foliensätze vom 16.09., 15.10., 20.10. und 05.11.2026 sowie die Codebeispiele aus der LVA.

> Mit **(Ergänzung)** markierte Stellen stehen so nicht im Folientext. Entweder waren die Folien nur Bilder, oder es ist Zusatzwissen, das beim Verstehen hilft.

---

## Inhalt

1. [Warum ein RTOS?](#1-warum-ein-rtos)
2. [Echtzeit: Begriffe](#2-echtzeit-begriffe)
3. [Tasks](#3-tasks)
4. [Scheduler und Scheduling](#4-scheduler-und-scheduling)
5. [Überblick: Kommunikation und Synchronisation](#5-überblick-kommunikation-und-synchronisation)
6. [Semaphore](#6-semaphore)
7. [RTOS und Interrupts](#7-rtos-und-interrupts)
8. [Queues, Stream- und Message-Buffer](#8-queues-stream--und-message-buffer)
9. [Event Flags](#9-event-flags)
10. [Mutex](#10-mutex)
11. [Probleme: Deadlock, Prioritätsinversion, Spinlock, Starvation](#11-probleme-deadlock-prioritätsinversion-spinlock-starvation)
12. [Software-Timer](#12-software-timer)
13. [Nebenläufigkeit modellieren](#13-nebenläufigkeit-modellieren)
14. [FreeRTOS in der STM32CubeIDE](#14-freertos-in-der-stm32cubeide)
15. [Trace mit dem Logikanalysator](#15-trace-mit-dem-logikanalysator)
16. [API-Spickzettel: CMSIS-RTOS v2 und natives FreeRTOS](#16-api-spickzettel-cmsis-rtos-v2-und-natives-freertos)
17. [Dos and Don'ts](#17-dos-and-donts)
18. [Selbsttest: typische Fragen](#18-selbsttest-typische-fragen)

---

## 1. Warum ein RTOS?

### Bare-Metal (Superloop)

Bisher liefen alle µC-Programme „bare metal“: eine Hauptschleife in `main()` plus Interrupts, Timer und DMA. Andere Namen dafür sind *Superloop*, *Bare-bone* und *Foreground/Background* (Vordergrund = ISR, Hintergrund = Hauptschleife).

| | Bare-Metal | RTOS |
|---|---|---|
| Struktur | einfach | Aufteilung in Tasks |
| Geschwindigkeit | sehr schnell, kein Overhead | etwas Overhead (Kontextwechsel, API-Aufrufe) |
| Komplexes Timing | wird schnell unübersichtlich | klares, definiertes Timing-Verhalten |
| Prioritäten | nur über Interrupt-Prioritäten | feingranulare Ausführungsregeln pro Task |
| Datenaustausch | globale Variablen, Flags | Queues, Semaphore, Mutex (sicher, vom OS verwaltet) |
| Geteilte Ressourcen | selbst absichern | bessere Verwaltung, weniger Gefahr von Blockaden |
| Speicher | minimal | ca. 6–10 kB Flash, ca. 0,5 kB RAM plus ein Stack pro Task |

### Was ist ein Betriebssystem?

Definition nach DIN 44300: *„Die Programme eines digitalen Rechensystems, die zusammen mit den Eigenschaften dieser Rechenanlage die Basis der möglichen Betriebsarten des Rechensystems bilden und die insbesondere die Abwicklung von Programmen steuern und überwachen.“*

Aufgaben eines OS:
- Komplexität der Maschine verbergen (Abstraktion)
- Benutzerschnittstelle (Shell) und normierte Programmierschnittstelle (API) bereitstellen
- Ressourcen verwalten: Prozessor, Speicher, Geräte, Rechenzeit
- Schutzstrategien bei der Ressourcenvergabe
- Prozesse koordinieren

### Hauptkomponenten eines RTOS

| Komponente | Aufgabe |
|---|---|
| **Tasks** | Funktionen, die in einer Endlosschleife laufen |
| **Scheduler** | entscheidet, welche Task wann läuft, und führt den Wechsel durch |
| **Software-Timer** | zeitliche Komponente des OS, periodische oder einmalige Callbacks |
| **Queues** | Datenaustausch zwischen Tasks bzw. zwischen ISR und Task |
| **Semaphore** | Synchronisation von Tasks |
| **Mutex** | Sperre für kritische Codebereiche bzw. geteilte Ressourcen |

---

## 2. Echtzeit: Begriffe

**Echtzeit heißt nicht „schnell“, sondern „rechtzeitig“.** Das Ergebnis muss innerhalb einer vorher festgelegten Zeitschranke vorliegen. Wie groß diese Schranke ist (µs, ms, Stunden), hängt nur vom Problem ab.

```
Ereignis                Reaktion (Code läuft)          Zeitschranke (Deadline)
   |------------------------------>|--------------------------|
   t0                         Latenz                         t_max
```

### Arten von Echtzeit

| Art | Was bei Überschreitung passiert | Beispiel (Ergänzung) |
|---|---|---|
| **Harte Echtzeit** | gilt als **Versagen**; das richtige Ergebnis muss *immer* rechtzeitig kommen | Airbag, ABS, Motorsteuerung |
| **Feste Echtzeit** | kein unmittelbarer Schaden, aber das verspätete Ergebnis ist **nutzlos** und wird verworfen | ein Video-Frame, der zu spät kommt |
| **Weiche Echtzeit** | Richtwert; ein akzeptabler **Mittelwert** oder ein statistisches Kriterium reicht, Überschreitung im Toleranzbereich ist ok | Audio-Streaming mit Puffer, UI |

### Anforderungen an ein Echtzeitbetriebssystem

- **Vorhersagbare Reaktionszeiten** auf Ereignisse
- **Vorhersagbares Zeitverhalten von Systemaufrufen**, garantiert auch im ungünstigsten Fall (Worst Case)
- **Präemptives Multitasking**: Ein Timer-Interrupt unterbricht die laufende Task nach ihrer Zeitscheibe. Der Kontext wird gesichert, der Scheduler wählt die nächste rechenbereite Task, stellt deren Kontext wieder her und startet sie.

### Latenz

- Latenz = Zeit vom Ereignis bis zur Reaktion. Für Echtzeit zählt die **maximale** Latenz, nicht der Durchschnitt.
- OSADL misst Latenzen von (Echtzeit-)Linux-Systemen über lange Zeit („Latency Plots“, Realtime QA Farm).
- **Messung in der LVA, STM32F411:** Eine niedrig priorisierte Task läuft, ein Ereignis weckt eine hoch priorisierte Task. Die Latenz bis zur hoch priorisierten Task betrug **ca. 9 µs**.

---

## 3. Tasks

### Was ist eine Task?

Eine C-Funktion mit Endlosschleife und eigenem Stack und eigener Priorität:

```c
void MyTask(void *argument)
{
    /* Code hier: läuft genau einmal beim Start der Task (Initialisierung) */

    for (;;)
    {
        /* Code hier: läuft immer wieder, wenn die Task "Running" ist */
        osDelay(100);            /* blockiert -> andere Tasks kommen dran */
    }

    /* Code hier: wird nie erreicht */
}
```

Eine Task darf **nie** einfach aus der Funktion zurückkehren. Wenn sie fertig ist, muss sie sich selbst löschen (Ergänzung: `vTaskDelete(NULL)` bzw. `osThreadExit()`).

### Bestandteile einer Task

| Teil | Wo | Inhalt |
|---|---|---|
| Programmcode | Flash | die Task-Funktion |
| **Stack** | RAM | Rücksprungadressen, Parameter, lokale Variablen, Zwischenergebnisse, gesicherte Register |
| **TCB** (Task Control Block) | RAM | Stackpointer, Priorität, aktueller Zustand, … |

Stack + Priorität + TCB-Daten zusammen bilden den **Task-Kontext**.
**Kontextwechsel (Context Switch)** = Kontext der angehaltenen Task speichern und Kontext der weiterlaufenden Task wiederherstellen.

### Die vier Task-Zustände

| Zustand | Bedeutung |
|---|---|
| **Running** | läuft gerade; pro CPU-Kern genau **eine** Task |
| **Ready** | bereit, wartet nur darauf, dass der Scheduler sie auswählt |
| **Blocked** | wartet auf ein Ereignis: Zeitablauf (`osDelay`), Semaphor, Queue, Event Flag, … Verbraucht **keine CPU-Zeit**. Tritt das Ereignis ein (oder der Timeout läuft ab), wird sie Ready. |
| **Suspended** | per API-Aufruf angehalten; nur ein weiterer API-Aufruf macht sie wieder Ready (kein Timeout) |

```
                         Scheduler wählt aus
                     +--------------------------+
                     |                          v
  erzeugt -->  +---------+                +-----------+
               |  READY  |                |  RUNNING  |
               +---------+                +-----------+
                 ^  ^  ^   verdrängt oder       |  |
                 |  |  |   Zeitscheibe vorbei   |  |
                 |  |  +-----------------------+  |
                 |  |                              |  wartet auf Ereignis
                 |  |  Ereignis tritt ein          |  (osDelay, Semaphor,
                 |  |  oder Timeout                v   Queue, Event Flag)
                 |  |                        +-----------+
                 |  +------------------------|  BLOCKED  |
                 |                           +-----------+
                 |  Resume
                 |                           +-----------+
                 +---------------------------| SUSPENDED |<-- Suspend
                                             +-----------+    (aus jedem Zustand)
```

### Zeitgesteuert vs. ereignisgesteuert

| | Zeitgesteuert | Ereignisgesteuert |
|---|---|---|
| Prinzip | Task wacht periodisch auf und prüft (pollt) | Task blockiert, bis ein Ereignis sie weckt |
| Code | `osDelay(n)` in der Schleife, dann Status abfragen | `osSemaphoreAcquire(..., osWaitForever)` oder `xQueueReceive(..., portMAX_DELAY)` |
| Reaktionszeit | bis zu einer Periode Verzögerung | sofort (Latenz im µs-Bereich) |
| CPU-Last | Aufwachen auch ohne Grund | nur bei tatsächlichem Ereignis |

**Merksatz:** Nicht pollen, sondern auf Ereignisse warten.

**`osDelay` vs. `osDelayUntil` (Ergänzung):** `osDelay(100)` wartet 100 ms *ab jetzt*. Die Periode wird dadurch um die Laufzeit der Task länger und driftet. `osDelayUntil` bzw. `vTaskDelayUntil` wartet bis zu einem *absoluten* Zeitpunkt und ergibt eine exakte Periode. Das ist besser für Abtastung und Scanning (das Keypad-Projekt nutzt deshalb `vTaskDelayUntil`).

---

## 4. Scheduler und Scheduling

Der **Scheduler** ist der Algorithmus, der aus allen **Ready**-Tasks die nächste auswählt. Kriterien sind Priorität, Zeitscheibe (Time Slice), Preemption und Idle.

### Präemptiv vs. kooperativ

| | Präemptiv | Kooperativ |
|---|---|---|
| Wer gibt die CPU ab? | Das OS verdrängt: eine höher priorisierte Task unterbricht eine niedriger priorisierte sofort | Die Task muss die CPU **selbst** abgeben (Delay, Warten, `taskYIELD`) |
| Gefahr | niedrig priorisierte Tasks **verhungern** (Starvation) | eine Task behält die CPU zu lange, andere kommen nicht oder zu spät dran |
| In der LVA | **Standard, nicht verändern** | **vermeiden!** |

### Round Robin

- Tasks **gleicher Priorität** bekommen zyklisch abwechselnd eine Zeitscheibe (in FreeRTOS: ein Tick = 1 ms bei `configTICK_RATE_HZ = 1000`).
- Mit dem Logikanalysator sichtbar: Drei Tasks gleicher Priorität, die nie blockieren, wechseln sich jede Millisekunde ab.

### Empfehlungen aus der LVA gegen Starvation

1. **Alle Tasks gleiche Priorität**, damit funktioniert Multitasking am besten.
2. **Nicht pollen**, sondern auf Ereignisse warten (blockieren).
3. **Höhere Priorität nur für interruptgesteuerte Tasks**, die meist blockiert sind und nur kurz laufen.
4. Das OS hilft mit **Priority Inheritance** (beim Mutex).

### Rate Monotonic Scheduling (RMS, Liu/Layland 1973)

**Regel:** Feste Prioritäten nach Periode. **Je kürzer die Periode, desto höher die Priorität.**

**Planbarkeitstest (Ergänzung, die Formel stand nur als Bild auf den Folien):**

$$U = \sum_{i=1}^{n} \frac{C_i}{T_i} \le n\left(2^{1/n} - 1\right)$$

- $C_i$ = Rechenzeit (Worst Case) von Task i, $T_i$ = Periode von Task i, $U$ = CPU-Auslastung

| Anzahl Tasks n | Grenze für U |
|---|---|
| 1 | 100 % |
| 2 | 82,8 % |
| 3 | 78,0 % |
| 4 | 75,7 % |
| ∞ | ln 2 ≈ **69,3 %** |

- Liegt U unter der Grenze, werden **garantiert** alle Deadlines eingehalten. Darüber ist es möglich, aber nicht garantiert (genauer prüfen).
- Faustregel von der Folie: Unter **69,3 %** CPU-Last hält RMS alle Deadlines. Die restlichen 30,7 % können nicht-echtzeitkritische Tasks niedriger Priorität bekommen. Zufällige periodische Task-Sets schaffen es meist bis ca. 85 %, das ist aber nicht garantiert.

**Rechenbeispiel (Ergänzung):** Task A: C = 1 ms, T = 4 ms; Task B: C = 2 ms, T = 10 ms
U = 1/4 + 2/10 = 0,45 = 45 % ≤ 82,8 % → planbar. Task A bekommt die höhere Priorität.

**Annahmen von RMS** (die in der Praxis oft nicht stimmen, daher die kritische Frage auf der Folie *„Do you believe this?“*):
- keine geteilten Ressourcen (keine Semaphore, Queues, kein Busy-Wait)
- Deadline = Periode
- statische Prioritäten, nach RMS-Regel vergeben
- Kontextwechsel kosten keine Zeit

---

## 5. Überblick: Kommunikation und Synchronisation

| Mechanismus | Zweck | Daten? | Aus ISR? | Kurz gesagt |
|---|---|---|---|---|
| **Binärer Semaphor** | Synchronisation („Ereignis ist eingetreten“) | nein | Give: ja | Signal von ISR oder Task an Task |
| **Zählender Semaphor** | mehrere Ereignisse zählen oder n gleiche Ressourcen verwalten | nein | Give: ja | Zähler statt 0/1 |
| **Mutex** | wechselseitiger Ausschluss bei geteilter Ressource | nein | **nein** | Token mit Besitzer und Priority Inheritance |
| **Queue** | Daten übertragen (FIFO, Kopie) | ja, feste Größe | ja (FromISR) | viele Schreiber, viele Leser |
| **Stream Buffer** | Bytestrom | ja | ja | 1 Schreiber, 1 Leser |
| **Message Buffer** | Nachrichten variabler Länge | ja | ja | 1 Schreiber, 1 Leser |
| **Event Flags** | auf mehrere Ereignisse warten (UND/ODER) | Bits | Set: ja | bis 24 Bits, Broadcast an mehrere Tasks |
| **Task Notification** | direktes Signal an eine bestimmte Task | 32-Bit-Wert | ja | schnellste Variante, Empfänger-Id nötig |

**Entscheidungshilfe (Ergänzung):**
- Ich muss **Daten** weitergeben → **Queue**
- Ich muss nur **signalisieren**, dass etwas passiert ist (z. B. aus einer ISR) → **binärer Semaphor**
- Zwei Tasks greifen auf **dieselbe Ressource** zu (UART, I2C, Display) → **Mutex**, oder besser: eine eigene IO-Task mit Queue
- Eine Task wartet auf **mehrere** Ereignisse gleichzeitig → **Event Flags**

---

## 6. Semaphore

- Dienen der **Synchronisation** mit Ereignissen von anderen Tasks oder Interrupts.
- Intern im Prinzip eine **Queue mit Länge 1 und Datengröße 0**.
- Eine wartende Task ist **blocked** und verbraucht keine CPU-Zeit.
- **Take** (nehmen, `osSemaphoreAcquire`) und **Give** (freigeben, `osSemaphoreRelease`).
- **Interrupts dürfen Semaphore nur freigeben (give), nie nehmen.**

### Arten

| Art | Verhalten |
|---|---|
| **Binär** | 0 oder 1. Mehrfaches Give vor einem Take zählt nur einmal |
| **Zählend** | Zähler von 0 bis Maximum. Jedes Give erhöht, jedes Take verringert. Kein Ereignis geht verloren |
| **Mutex** | binärer Semaphor mit Besitzer und Prioritätsvererbung, nur für wechselseitigen Ausschluss (siehe Kapitel 10) |

### Funktionen (CMSIS-RTOS v2)

```c
osStatus_t osSemaphoreAcquire(osSemaphoreId_t id, uint32_t timeout); /* take */
osStatus_t osSemaphoreRelease(osSemaphoreId_t id);                   /* give */
```

- `timeout` in Ticks (bei 1000 Hz = ms) oder `osWaitForever` (`0xFFFFFFFF`)
- Rückgabewert sagt, ob erfolgreich (`osOK`) oder Timeout (`osErrorTimeout`) → **immer prüfen!**
- `osSemaphoreRelease()` erkennt selbst, ob es aus einer ISR oder einer Task aufgerufen wird.

### Typisches Muster: ISR weckt Task

```c
/* Task: wartet blockiert, bis die ISR das Ereignis meldet */
void StartTask02(void *argument)
{
    for (;;)
    {
        osSemaphoreAcquire(myBinarySem01Handle, osWaitForever);
        HAL_GPIO_TogglePin(LD6_GPIO_Port, LD6_Pin);
    }
}

/* ISR-Callback: nur freigeben, sonst nichts Aufwändiges */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    osSemaphoreRelease(myBinarySem01Handle);
}
```

### Stolperfalle: Semaphor ist beim Start schon gegeben

**Frage aus der LVA:** Warum leuchtet die blaue LED schon beim Systemstart?

**Antwort:** CubeMX erzeugt `osSemaphoreNew(1, 1, ...)`. Der zweite Parameter `initial_count` ist **1**, der Semaphor ist also von Anfang an gegeben und die Task läuft sofort einmal durch.

**Abhilfe:** Im USER-CODE-Bereich einmal nehmen, **nicht** in `osSemaphoreNew()` ändern (das wird bei jeder Codegenerierung überschrieben):

```c
/* USER CODE BEGIN RTOS_SEMAPHORES */
osSemaphoreAcquire(myBinarySem01Handle, 1);
/* USER CODE END RTOS_SEMAPHORES */
```

(Ergänzung: Beim nativen FreeRTOS ist das umgekehrt. `xSemaphoreCreateBinary()` startet **leer**, da tritt das Problem nicht auf.)

---

## 7. RTOS und Interrupts

### Prioritätsregel (wichtigste Prüfungsfrage!)

- Beim Cortex-M gilt: **kleinere Zahl = höhere Priorität** (0 ist die höchste).
- Ein Interrupt, der RTOS-Funktionen aufruft (`...FromISR`, `osSemaphoreRelease` usw.), muss eine **niedrigere** Priorität haben als `configMAX_SYSCALL_INTERRUPT_PRIORITY`. Der eingestellte **Zahlenwert muss also größer oder gleich 5** sein (Default 5, muss normalerweise nicht geändert werden).
- Interrupts mit **höherer** Priorität (Zahl 0 bis 4) dürfen **keine** RTOS-Funktionen aufrufen, sonst **undefiniertes Verhalten**. Dafür werden sie nie vom RTOS verzögert.
- Verschachtelung (Nesting) von RTOS-Interrupts ist möglich.

```
Priorität (Zahl):   0   1   2   3   4 | 5   6   ...   15
                    <- nie vom RTOS  -> | <- dürfen RTOS-API (FromISR) nutzen ->
                       verzögert, KEINE |    SysTick/PendSV laufen auf 15
                       RTOS-Aufrufe!    |
```

### Regeln für ISRs

- ISRs **so kurz wie möglich** halten, sonst stören sie das Timing des RTOS.
- **Generelles Muster:** Die Task erledigt die eigentliche Arbeit und wartet auf einen Semaphor. Die ISR gibt nur den Semaphor frei, sobald das Ereignis eingetreten ist („Deferred Interrupt Processing“).
- In der ISR nie blockieren, kein `osDelay`, kein Warten auf Queue oder Semaphor.
- Natives FreeRTOS: in der ISR immer die `...FromISR`-Variante plus `portYIELD_FROM_ISR` verwenden:

```c
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    if (GPIO_Pin == GPIO_PIN_0) {
        xSemaphoreGiveFromISR(IRQSemaphore, &xHigherPriorityTaskWoken);
        portYIELD_FROM_ISR(xHigherPriorityTaskWoken);  /* sofort zur geweckten Task wechseln */
    }
}
```

`portYIELD_FROM_ISR` sorgt dafür, dass nach der ISR **direkt** die geweckte Task läuft, wenn sie eine höhere Priorität hat, und nicht erst beim nächsten Tick.

- Hinweis aus der LVA: Callback-Funktion in `freertos.c` platzieren, wenn die RTOS-Funktionen in `main.c` nicht bekannt sind (bzw. die passenden Header einbinden).

---

## 8. Queues, Stream- und Message-Buffer

### Queue-Grundlagen

- Speichert eine feste Anzahl von Nachrichten, arbeitet als **FIFO** (die erste eingegangene Nachricht kommt zuerst wieder heraus).
- Alle Elemente haben **dieselbe Datenstruktur und feste Länge**.
- Daten werden **per Kopie** übertragen. Der Sender kann deshalb eine lokale Variable verwenden.
- Operationen laufen **unterbrechungsfrei** ab (Critical Sections).

### Blockierverhalten

| Situation | Was passiert |
|---|---|
| Lesen aus **leerer** Queue | Leser wird **blocked**, bis eine Nachricht kommt oder der Timeout abläuft |
| Schreiben in **volle** Queue | Schreiber wird **blocked**, bis Platz frei wird oder der Timeout abläuft |
| Mehrere Tasks warten | die mit **höchster Priorität** zuerst, bei gleicher Priorität die, die **am längsten wartet** |

Timeout entweder fest vorgegeben oder `osWaitForever`. Bei Timeout 0 gibt es kein Warten, die Funktion meldet sofort Erfolg oder Fehler.

### Funktionen (CMSIS-RTOS v2)

```c
osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size, const osMessageQueueAttr_t *attr);
osStatus_t osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr, uint8_t msg_prio, uint32_t timeout);
osStatus_t osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr, uint8_t *msg_prio, uint32_t timeout);
uint32_t   osMessageQueueGetCount(osMessageQueueId_t mq_id);   /* wie viele Nachrichten sind drin? */
```

- `msg_prio` wird ignoriert: beim Put immer 0, beim Get einen Pointer auf eine Dummy-Variable übergeben.
- Rückgabewert auf `osOK` prüfen.

### Struct als Nachricht (Beispiel vom Whiteboard)

```c
struct QMessage {
    uint8_t SenderID;   /* 1 = Turn_On-Task, 2 = Turn_Off-Task */
    uint8_t Zahl;       /* welche LED (0..3)                   */
};

myQueue = xQueueCreate(10, sizeof(struct QMessage));   /* Größe des STRUCTS, nicht des Pointers! */

struct QMessage xTx = { .SenderID = 1, .Zahl = 2 };
xQueueSend(myQueue, &xTx, 0);                           /* Inhalt wird kopiert */

struct QMessage xRx;
if (xQueueReceive(myQueue, &xRx, pdMS_TO_TICKS(1010)) == pdPASS) { /* auswerten */ }
```

> Achtung (Ergänzung): Im Kursbeispiel `RTOSQueue2` steht `xQueueCreate(10, sizeof(struct QMessage *))`, also die Größe eines **Pointers** (4 Byte). Das funktioniert dort nur zufällig, weil das Struct 2 Byte groß ist. Richtig ist `sizeof(struct QMessage)`.

### Message Queue vs. Mail Queue

- Die Unterscheidung stammt aus **CMSIS-RTOS v1**: Eine Message Queue überträgt nur einen 32-Bit-Wert (Integer oder Pointer). Eine Mail Queue überträgt Datenblöcke aus einem Memory Pool, durch die Queue wandert nur der Pointer und der Empfänger muss den Block mit `osMailFree` freigeben.
- **FreeRTOS kennt nur einen Queue-Typ** mit frei wählbarer, fester Elementgröße (int, Pointer oder ganzes struct, per Kopie).
- **CMSIS-RTOS v2** hat keine Mail Queue mehr. `osMessageQueueNew` nimmt beliebig große Nachrichten fester Größe auf.
- Will man große Daten vermeiden, kann man bewusst Pointer in die Queue legen. Dann muss man aber selbst klären, wem der Speicher gehört und wie lange er gültig ist.

### Stream Buffer und Message Buffer (seit FreeRTOS V10)

| Mechanismus | Datenmodell | Teilnehmer | Typischer Einsatz |
|---|---|---|---|
| **Queue** | Elemente fester Größe | beliebig viele Schreiber und Leser | Nachrichten, Befehle, Messwerte |
| **Message Buffer** | Nachrichten variabler Länge (jede wird ganz oder gar nicht gelesen) | **1 Schreiber, 1 Leser** | Textzeilen, Pakete |
| **Stream Buffer** | Bytestrom ohne Nachrichtengrenzen | **1 Schreiber, 1 Leser** | UART-Empfang: ISR schreibt Bytes, Task verarbeitet |

- Stream Buffer: `xStreamBufferCreate(size, triggerLevel)`. Der **Trigger Level** legt fest, wie viele Bytes mindestens im Puffer sein müssen, bevor die wartende Leser-Task Ready wird.
- Message Buffer: Vor jede Nachricht wird intern ein Längenfeld (`size_t`, meist 4 Byte) gestellt. Das muss man bei der Puffergröße mitrechnen.
- Bei mehreren Schreibern oder Lesern muss man den Zugriff selbst absichern (z. B. mit einer Critical Section).

### Wichtigste Anwendung: IO-Task mit Queue

Peripherie (Display, SD-Karte, UART) darf **nicht** von allen Tasks direkt angesprochen werden, sonst kommt alles bunt gemischt durcheinander.

```
Task 1 --\
Task 2 ---+--> [ Queue ] --> IO-Task (einzige Task mit Zugriff) --> Display
Task 3 --/
```

- **Je Peripheriekomponente eine IO-Task und eine Queue.**
- Tasks schreiben ihre Ausgaben in die Queue, gegebenenfalls mit Zusatzinfo (z. B. Zeile und Position am Display).
- Die IO-Task wartet blockiert auf die Queue und verarbeitet jede Nachricht.
- **Achtung, Queue voll:** Der Sender muss den Rückgabewert prüfen und dann warten, wiederholen oder die Nachricht verwerfen.
- Genau so ist die Display-Manager-Aufgabe aus dem zweiten LVA-Teil aufgebaut.

### Zeitverhalten (Übungsfrage)

Im Beispiel sendet Task 1 alle 250 ms eine LED-Nummer, Task 2 schaut alle 70 ms nach.
- **Sender schneller als Empfänger** → die Queue füllt sich, bis sie voll ist. Danach scheitert `Put` nach seinem Timeout und Nachrichten gehen verloren (oder der Sender blockiert bei langem Timeout).
- **Empfänger schneller als Sender** → die Queue ist meist leer und der Empfänger findet oft nichts. Das ist harmlos, aber Polling mit `GetCount` verschwendet CPU-Zeit. Besser ist es, mit Timeout direkt in `Get` zu blockieren.
- Mit `osDelay(170 * LED_Number)` hängt die Sendeperiode von der zufälligen LED-Nummer ab (170 bis 680 ms). Jede LED bleibt so lange an, bis die nächste Nachricht kommt, also unterschiedlich lange. Das Verhalten wird unregelmäßig (Ergänzung zur Interpretation).

---

## 9. Event Flags

- Synchronisation einer Task mit **mehreren** Ereignissen.
- Bis zu **24 Events** pro Gruppe, dargestellt als **Bitmaske** (z. B. `0x50` = Bits 6 und 4).
- Tasks warten auf **irgendeines** (`osFlagsWaitAny`, Default) oder **alle** (`osFlagsWaitAll`) der Bits einer Maske.
- Andere Tasks und Interrupts setzen die Bits.
- `osFlagsNoClear`: Die Bits, auf die gewartet wurde, werden nach dem Aufwachen **nicht** gelöscht.

**Vorteile gegenüber mehreren Semaphoren:**
- braucht weniger RAM
- bereits gesetzte Events können wieder gelöscht werden
- **Broadcast**: Mehrere Tasks können auf dieselben Bits warten und werden alle gleichzeitig geweckt

**Beispiel aus der LVA:**
1. Task 1 wartet auf Maske `0x51` mit „alle“ → blocked
2. Task 2 setzt periodisch `0x01`, das reicht noch nicht
3. Der EXTI-Interrupt setzt `0x50` → jetzt sind alle Bits von `0x51` gesetzt → Task 1 wird Ready
4. Wartet eine Task 3 auf dieselben Events, wird sie gleichzeitig ebenfalls Ready (Broadcast)

```c
osEventFlagsSet(evtHandle, 0x50);                                         /* setzen (auch aus ISR) */
osEventFlagsWait(evtHandle, 0x51, osFlagsWaitAll, osWaitForever);         /* warten */
```

---

## 10. Mutex

### Problem: geteilte Ressource

Zwei Tasks senden gleichzeitig über eine UART → **Buchstabensalat**:

```
Task 1: "ABCDEFGH..."     Ausgabe: "ABCabcDdefEF..."
Task 2: "abcdefgh..."
```

Die Ausgabe einer Task wird vom Scheduler mitten im String unterbrochen (Beispiel `SharedResource.c`).

### Mutex = MUTual EXclusion (wechselseitiger Ausschluss)

- Spezielle Form eines binären Semaphors, gedacht für **alleinigen Zugriff** auf eine Ressource.
- Funktioniert wie ein **Token**: Die Task wartet auf Freigabe, besitzt den Mutex dann eine Zeit lang und **muss ihn selbst wieder zurückgeben**.
- **Kann nicht aus Interrupts freigegeben werden.**
- Hat **Priority Inheritance** (Prioritätsvererbung).

```c
osMutexAcquire(uartMutexHandle, osWaitForever);
HAL_UART_Transmit(&huart2, msg, len, 100);       /* kritischer Abschnitt: kurz halten! */
osMutexRelease(uartMutexHandle);
```

### Mutex vs. binärer Semaphor (häufige Prüfungsfrage)

| | Binärer Semaphor | Mutex |
|---|---|---|
| Zweck | **Synchronisation** (Signal: „Ereignis ist da“) | **Schutz** einer geteilten Ressource |
| Besitzer | keinen; Task A kann nehmen, Task B oder eine ISR kann geben | **die Task, die ihn genommen hat**, und nur sie gibt ihn zurück |
| Aus ISR freigeben | ja | **nein** |
| Priority Inheritance | **nein** | **ja** |
| Startzustand (natives FreeRTOS) | leer | frei |
| Typisch | ISR meldet „Taste gedrückt“ | UART, I2C-Bus, Display |

Beispiel für den Unterschied: Eine Task nimmt einen Semaphor, um auf ein I/O-Ereignis zu warten. Die Rx-Task (oder ISR), die das Byte empfängt, gibt ihn frei. Mit einem Mutex geht das nicht, weil nur der Besitzer ihn freigeben darf.

### Priority Inheritance

Wartet eine **hoch** priorisierte Task auf einen Mutex, den eine **niedrig** priorisierte Task hält, hebt das RTOS die Priorität des Besitzers **vorübergehend** an. So kann er seinen kritischen Abschnitt schnell beenden und den Mutex freigeben. Danach fällt er auf seine ursprüngliche Priorität zurück. Semaphore kennen keine Priority Inheritance.

### Übungsbeispiel Mutex (Interpretation, Ergänzung)

Drei Tasks mit Priorität *Normal*, alle mit `HAL_Delay(500)` (das ist **Busy-Waiting**, die Tasks blockieren also nie):
- Task02 senkt nach vier Durchläufen **innerhalb des kritischen Abschnitts** seine eigene Priorität auf *Low*. Dadurch wird sie sofort verdrängt, **während sie den Mutex noch hält**.
- **Mit Mutex:** Task03 will den Mutex → Priority Inheritance hebt Task02 kurz auf *Normal* → Task02 gibt den Mutex frei → Task03 läuft weiter. Task02 verhungert danach auf *Low*, weil Task03 und Task04 nie blockieren (HAL_Delay).
- **Mit Semaphor:** keine Vererbung. Task02 hält den Semaphor auf *Low* und kommt nie wieder dran. Task03 wartet ewig. **Nur noch Task04 läuft** (nur ihre LED blinkt). Das ist **Prioritätsinversion**.
- Lehre: `HAL_Delay` gehört nicht in Tasks (stattdessen `osDelay`). Und Mutex statt Semaphor für geteilte Ressourcen.

---

## 11. Probleme: Deadlock, Prioritätsinversion, Spinlock, Starvation

### Deadlock (Verklemmung)

Zwei Tasks brauchen jeweils die Ressourcen A **und** B:
- Task 1 hält A und wartet auf B
- Task 2 hält B und wartet auf A
- → keine kann weiter. **Das löst sich nicht von selbst auf.**

```
Task 1 --hält--> [A] <--wartet-- Task 2
Task 1 --wartet--> [B] <--hält-- Task 2
```

- Das OS kann Deadlocks kaum erkennen oder verhindern (Erkennung ist sehr aufwändig).
- **Schon beim Systemdesign vermeiden**, z. B. (Ergänzung):
  - Ressourcen **immer in derselben Reihenfolge** anfordern (alle zuerst A, dann B)
  - mit **Timeout** statt `osWaitForever` warten und bei Timeout alles freigeben
  - möglichst nie mehrere Mutexe gleichzeitig halten
  - IO-Task mit Queue statt geteilter Ressource

### Prioritätsinversion (Ergänzung, auf den Folien nur als Stichwort)

Eine hoch priorisierte Task wartet indirekt auf eine **mittel** priorisierte:

```
Prio hoch:    H wartet auf Ressource ...............(blockiert)..........
Prio mittel:  ............ M läuft und läuft und läuft ...
Prio niedrig: L hält Ressource, wird von M verdrängt, kann sie nicht freigeben
```

1. L (niedrig) nimmt die Ressource.
2. H (hoch) will sie auch → blockiert und wartet auf L.
3. M (mittel) wird Ready und verdrängt L, weil M wichtiger ist als L.
4. Ergebnis: **H wartet auf M**, obwohl M nichts mit der Ressource zu tun hat.

**Lösung:** Mutex mit **Priority Inheritance**. L erbt die Priorität von H, kann nicht von M verdrängt werden, beendet den kritischen Abschnitt und gibt frei. (Berühmtes Beispiel: Mars Pathfinder 1997.)

### Spinlock / Busy-Waiting (Ergänzung, auf den Folien nur als Stichwort)

Eine Task wartet **aktiv** in einer Schleife, statt zu blockieren:

```c
while (!flag) { }                          /* Spinlock: verbrennt CPU-Zeit */
for (int i = 0; i < 8000000; i++);         /* Warteschleife aus dem ersten Beispiel */
HAL_Delay(500);                            /* ebenfalls Busy-Wait! */
```

- Verschwendet CPU-Zeit. Tasks gleicher oder niedrigerer Priorität kommen nur noch über die Zeitscheibe bzw. gar nicht dran.
- **Lösung:** blockierende RTOS-Funktionen verwenden: `osDelay`, Semaphor, Queue.

### Starvation (Verhungern)

Eine niedrig priorisierte Task kommt nie dran, weil höher priorisierte Tasks immer Ready sind (sie blockieren nie). Lösung: gleiche Prioritäten, nicht pollen, höher priorisierte Tasks müssen blockieren.

---

## 12. Software-Timer

- Standardkomponente jedes RTOS. Ruft **Callback-Funktionen** periodisch oder einmalig auf.
- Die Callbacks laufen im Kontext der **Timer Service Task** (Daemon Task). Deshalb gilt:
  - **niemals blockieren**: kein `osDelay()`
  - auf Queues und Semaphore **nur mit Timeout 0** zugreifen
- Nicht extrem präzise (Auflösung = Tick). Gedacht für periodische Funktionen und Verzögerungen, als Ergänzung zu den Hardware-Timern.

| Typ | Verhalten |
|---|---|
| **Periodisch** | Callback wird nach jedem Ablauf wieder aufgerufen; **einmal starten reicht** |
| **One Pulse (One-Shot)** | Callback genau einmal; muss **manuell neu gestartet** werden |

**Achtung:** Wird ein Timer neu gestartet, **bevor** er abgelaufen ist, beginnt die Zeit von vorne. Der Callback wird dann nicht aufgerufen.

```c
osTimerStart(myTimer01Handle, 1000);   /* periodisch: einmal vor der Schleife */

for (;;) {
    osTimerStart(myTimer02Handle, 1000);   /* One-Shot: jedes Mal neu */
    osDelay(2000);
}

void Callback01(void *argument) {          /* kurz, nicht blockierend */
    HAL_GPIO_TogglePin(LD4_GPIO_Port, LD4_Pin);
}
```

---

## 13. Nebenläufigkeit modellieren

### Begriffe

- **Sequentiell:** Innerhalb einer Task laufen die Anweisungen nacheinander ab. Gleiche Eingaben liefern immer dasselbe Ergebnis (determiniert).
- **Nebenläufig (concurrent):** Aktivitäten, die **nicht** in einer festen Reihenfolge laufen müssen. Beispiel: Task Pi wartet auf Daten (z. B. USART-Interrupt), in der Zeit nutzt Task Pj die CPU → *quasi-parallel*. Die CPU selbst arbeitet trotzdem nacheinander.
- Die **Peripherie** eines µC arbeitet echt parallel (Timer, DMA, UART …).
- Nebenläufigkeit verbessert meist das Echtzeitverhalten, weil technische Teilprozesse Ereignisse erzeugen, die Tasks von *blocked* nach *ready* bringen.

### Beispiel CNC-Bohrmaschine

- Tisch in X und Y bewegen: **nebenläufig**, beide Achsen gleichzeitig.
- Z-Achse (Bohren) erst, **nachdem X und Y fertig sind**: das ist eine Synchronisation (z. B. Event Flags mit „alle“).

### Kooperation und Konkurrenz

- **Konkurrenz:** Tasks konkurrieren um geteilte Ressourcen (CPU, UART, Bus).
- **Kooperation:** Sie müssen zusammenarbeiten, damit es keine Zugriffsprobleme gibt, denn für den technischen Prozess zählt das gemeinsame Ziel → **Inter-Task-Kommunikation**.
- Das RTOS übernimmt Locking und Unlocking über API-Aufrufe (Supervisor Call, SVC). Das erzeugt **Overhead**. Manchmal wird stattdessen Shared Memory verwendet, das ist dann aber **nicht** vom RTOS geschützt.

### Petri-Netze

Grafische Modellierung nebenläufiger Abläufe:
- **Stellen** (Kreise) mit **Marken** (Tokens, Punkte) = Zustände bzw. Ressourcen
- **Transitionen** (Balken) = Ereignisse bzw. Aktionen. Eine Transition „schaltet“, wenn alle Eingangsstellen eine Marke haben. Dann nimmt sie die Marken weg und legt neue in die Ausgangsstellen.
- Ein **Semaphor** entspricht genau einer Stelle mit Marken: Give legt eine Marke hinein, Take nimmt eine heraus.

### Erzeuger-Verbraucher (Producer-Consumer)

Beispiel aus der LVA: Eine Task wird jede Sekunde Ready und erzeugt einen Messwert per ADC (Erzeuger). Eine andere Task wartet auf diesen Wert und zeigt ihn am Display an (Verbraucher).
→ In RTOS als **Queue** umgesetzt: Der Erzeuger macht Put, der Verbraucher blockiert auf Get.

---

## 14. FreeRTOS in der STM32CubeIDE

### Konfigurationsschritte

1. Board oder MCU auswählen. Bei Board-Auswahl sind LEDs und Taster schon mit Namen konfiguriert. Nicht verwendete Komponenten deaktivieren (spart Strom, gibt Pins frei). Beim Discovery-Board RCC von *Bypass* auf *Disable* ändern bzw. HSI nutzen.
2. Takt einstellen (in der LVA: HSI + PLL auf 100 MHz).
3. **System Core → SYS → Timebase Source: NICHT SysTick**, sondern einen anderen Timer. Der SysTick gehört FreeRTOS. Gute Wahl sind TIM6/TIM7 (am F411 nicht vorhanden → z. B. TIM11).
4. **Middleware → FREERTOS** aktivieren, Interface **CMSIS_V2**.
5. FPU-Unterstützung nur aktivieren, wenn sie wirklich gebraucht wird (sie kostet zusätzliche Zeit und Speicher beim Taskwechsel).
6. `USE_PREEMPTION` = Enabled (Default, **nicht ändern**).
7. **`USE_APPLICATION_TASK_TAG` aktivieren**, damit die Trace-Hook-Makros funktionieren.
8. `USE_MUTEXES` für Mutex-Beispiele aktivieren.
9. Tasks, Queues, Semaphore und Timer in den Tabs anlegen oder selbst im Code erzeugen.
10. Empfohlen: *Generate peripheral initialization as a pair of .c/.h files*. Dann liegt der Task-Code in `freertos.c` statt in `main.c`.

### Wichtige Parameter in `FreeRTOSConfig.h`

| Parameter | Typischer Wert | Bedeutung |
|---|---|---|
| `configUSE_PREEMPTION` | 1 | präemptiver Scheduler |
| `configTICK_RATE_HZ` | 1000 | 1 Tick = 1 ms |
| `configMAX_PRIORITIES` | 56 (CMSIS v2) | Anzahl Prioritätsstufen |
| `configMINIMAL_STACK_SIZE` | 128 | Words (× 4 = Bytes!) |
| `configTOTAL_HEAP_SIZE` | 15360 | Heap für alle dynamisch erzeugten Tasks, Queues usw. |
| `configMAX_SYSCALL_INTERRUPT_PRIORITY` | 5 (Library-Wert) | Grenze für ISRs mit RTOS-Aufrufen |
| `configUSE_MUTEXES` | 1 | Mutex verfügbar |
| `configUSE_APPLICATION_TASK_TAG` | 1 | für Trace-Hooks |

### USER CODE

Nur zwischen `/* USER CODE BEGIN ... */` und `/* USER CODE END ... */` schreiben. Alles andere wird beim nächsten *Generate Code* überschrieben.

### Speicher

- **Stackgröße bei `xTaskCreate` in Words**, nicht Bytes: 128 = 512 Byte. Bei CMSIS v2 (`osThreadAttr_t.stack_size`) dagegen in **Bytes** (deshalb `128 * 4`).
- Tasks mit `printf`/`sprintf` brauchen deutlich mehr Stack (256 Words und mehr).
- Stack oder Heap zu klein führt zu **undefiniertem Verhalten des gesamten Systems**. Warnungen der CubeIDE beachten.

---

## 15. Trace mit dem Logikanalysator

Ziel: sehen, **welche Task wann läuft**.

1. Jede Task bekommt eine Nummer (Tag):
   ```c
   vTaskSetApplicationTaskTag(NULL, (void *)1);   /* in der Task selbst */
   ```
2. In `FreeRTOSConfig.h` (USER CODE Defines) die Kernel-Hooks auf eigene Funktionen umleiten:
   ```c
   #define traceTASK_SWITCHED_IN()  SetOutput(0, (int)pxCurrentTCB->pxTaskTag)
   #define traceTASK_SWITCHED_OUT() ResetOutput(0, (int)pxCurrentTCB->pxTaskTag)
   ```
3. `SetOutput`/`ResetOutput` setzen bzw. löschen den GPIO-Pin, der zur Tag-Nummer gehört.
4. Logikanalysator an diese Pins anschließen. Ein Pin ist **high, solange seine Task läuft**.

Damit sieht man z. B. Round Robin (Wechsel jede ms), Preemption durch eine höher priorisierte Task und die Latenz nach einem Interrupt.

---

## 16. API-Spickzettel: CMSIS-RTOS v2 und natives FreeRTOS

Die Folien verwenden meist die **CMSIS-v2-API** (`os...`), die Codebeispiele in der LVA oft das **native FreeRTOS** (`x...`, `v...`). Beides funktioniert gleichzeitig, weil CMSIS v2 nur eine Hülle um FreeRTOS ist.

| Aufgabe | CMSIS-RTOS v2 | Natives FreeRTOS |
|---|---|---|
| Task erzeugen | `osThreadNew(func, arg, &attr)` | `xTaskCreate(func, "Name", stackWords, arg, prio, &handle)` |
| Warten (relativ) | `osDelay(ticks)` | `vTaskDelay(ticks)` |
| Warten (absolut, exakte Periode) | `osDelayUntil(ticks)` | `vTaskDelayUntil(&lastWake, period)` |
| ms in Ticks | (Ticks = ms bei 1 kHz) | `pdMS_TO_TICKS(ms)` |
| Ewig warten | `osWaitForever` | `portMAX_DELAY` |
| Priorität ändern | `osThreadSetPriority(id, prio)` | `vTaskPrioritySet(handle, prio)` |
| Binärer Semaphor | `osSemaphoreNew(1, 0, NULL)` (**initial 0!**) | `xSemaphoreCreateBinary()` (startet leer) |
| Zählender Semaphor | `osSemaphoreNew(max, init, NULL)` | `xSemaphoreCreateCounting(max, init)` |
| Take | `osSemaphoreAcquire(id, timeout)` | `xSemaphoreTake(h, timeout)` |
| Give (Task) | `osSemaphoreRelease(id)` | `xSemaphoreGive(h)` |
| Give (ISR) | `osSemaphoreRelease(id)` (erkennt ISR selbst) | `xSemaphoreGiveFromISR(h, &woken)` + `portYIELD_FROM_ISR(woken)` |
| Mutex | `osMutexNew(NULL)` | `xSemaphoreCreateMutex()` |
| Mutex nehmen/geben | `osMutexAcquire` / `osMutexRelease` | `xSemaphoreTake` / `xSemaphoreGive` |
| Queue erzeugen | `osMessageQueueNew(count, size, NULL)` | `xQueueCreate(count, size)` |
| Senden | `osMessageQueuePut(id, &msg, 0, timeout)` | `xQueueSend(h, &msg, timeout)` |
| Senden aus ISR | `osMessageQueuePut(id, &msg, 0, 0)` | `xQueueSendFromISR(h, &msg, &woken)` |
| Empfangen | `osMessageQueueGet(id, &msg, NULL, timeout)` | `xQueueReceive(h, &msg, timeout)` |
| Füllstand | `osMessageQueueGetCount(id)` | `uxQueueMessagesWaiting(h)` |
| Event Flags erzeugen | `osEventFlagsNew(NULL)` | `xEventGroupCreate()` |
| Bits setzen | `osEventFlagsSet(id, bits)` | `xEventGroupSetBits(h, bits)` (ISR: `...FromISR`) |
| Auf Bits warten | `osEventFlagsWait(id, bits, osFlagsWaitAll, timeout)` | `xEventGroupWaitBits(h, bits, clear, waitAll, timeout)` |
| SW-Timer erzeugen | `osTimerNew(cb, osTimerPeriodic, arg, NULL)` | `xTimerCreate("T", period, autoReload, id, cb)` |
| SW-Timer starten | `osTimerStart(id, ticks)` | `xTimerStart(h, 0)` |
| Scheduler starten | `osKernelStart()` | `vTaskStartScheduler()` |

### Rückgabewerte (CMSIS v2) – immer prüfen!

| Wert | Bedeutung |
|---|---|
| `osOK` (0) | erfolgreich |
| `osError` (-1) | unspezifischer Fehler |
| `osErrorTimeout` (-2) | nicht innerhalb des Timeouts geschafft |
| `osErrorResource` (-3) | Ressource nicht verfügbar |
| `osErrorParameter` (-4) | falscher Parameter |
| `osErrorNoMemory` (-5) | kein Speicher mehr (Heap zu klein) |
| `osErrorISR` (-6) | Funktion darf nicht aus einer ISR aufgerufen werden |

Natives FreeRTOS: `pdPASS`/`pdTRUE` = Erfolg, `pdFAIL`/`pdFALSE` bzw. `errQUEUE_FULL` = Fehler.

---

## 17. Dos and Don'ts

### Do

- ✅ Für die HAL-Zeitbasis einen **anderen Timer als SysTick** verwenden (Warnung der CubeIDE beachten)
- ✅ **Rückgabewerte** von RTOS-Funktionen prüfen
- ✅ ISR-Priorität **≥ 5** (Zahlenwert), wenn die ISR RTOS-Funktionen aufruft; das gilt auch für HAL-ISRs und DMA-Kanäle
- ✅ ISRs kurz halten, Arbeit per Semaphor oder Queue an eine Task übergeben
- ✅ Auf Ereignisse warten statt pollen
- ✅ Gleiche Prioritäten für normale Tasks; höher nur für ereignisgesteuerte Tasks, die meistens blockiert sind
- ✅ Eine IO-Task pro Peripherie, gespeist durch eine Queue
- ✅ Mutex für geteilte Ressourcen, binären Semaphor für Signale
- ✅ Stack- und Heap-Warnungen ernst nehmen

### Don't

- ❌ `HAL_Delay()` oder Warteschleifen in Tasks (Busy-Wait) → `osDelay()` verwenden
- ❌ RTOS-Funktionen aus ISRs mit Priorität 0–4
- ❌ Mutex aus einer ISR freigeben
- ❌ In Software-Timer-Callbacks blockieren (`osDelay`, Warten auf Queue oder Semaphor mit Timeout ≠ 0)
- ❌ Kooperatives Scheduling
- ❌ Mehrere Tasks direkt auf dieselbe Peripherie zugreifen lassen
- ❌ Initialwert eines Semaphors in `osSemaphoreNew()` ändern (wird überschrieben), stattdessen im USER CODE einmal `Acquire`

### Fehlersuche

| Symptom | Mögliche Ursache |
|---|---|
| Task läuft nie | wartet auf ein Ereignis, das nie kommt (Semaphor, Mutex, Queue); oder Priorität zu niedrig und andere Tasks blockieren nie |
| Task läuft einmal sofort beim Start | Semaphor wurde mit Initialwert 1 erzeugt |
| Unerwartete Timeouts | Timeout zu kurz gewählt, Rückgabewert nicht geprüft |
| Interrupt hat keine oder seltsame Wirkung | ISR-Priorität zu hoch (Zahl < 5) bei RTOS-Aufruf |
| One-Shot-Timer-Callback kommt nie | Timer wird vor Ablauf immer wieder neu gestartet |
| Timer-Callbacks zur falschen Zeit | Priorität der Timer-Task zu niedrig |
| HAL-Timeouts funktionieren nicht | HAL und RTOS teilen sich denselben Timer |
| Absturz, HardFault, „zufälliges“ Verhalten | Stack oder Heap zu klein |
| Buchstabensalat auf UART oder Display | geteilte Ressource ohne Mutex oder IO-Task |

---

## 18. Selbsttest: typische Fragen

**1. Was ist der Unterschied zwischen harter, fester und weicher Echtzeit?**
Hart: Überschreitung = Versagen, Ergebnis muss immer rechtzeitig sein. Fest: kein Schaden, aber ein verspätetes Ergebnis ist wertlos. Weich: Zeitvorgabe ist ein Richtwert, ein guter Mittelwert reicht.

**2. Heißt Echtzeit „möglichst schnell“?**
Nein. Echtzeit heißt *vorhersagbar innerhalb einer festen Zeitschranke*, auch im ungünstigsten Fall. Die Schranke kann auch Stunden betragen.

**3. Welche vier Zustände kann eine Task haben, und was ist der Unterschied zwischen Blocked und Suspended?**
Running, Ready, Blocked, Suspended. Blocked wartet auf ein Ereignis oder einen Timeout und wird automatisch wieder Ready. Suspended wird nur durch einen expliziten API-Aufruf (Resume) wieder Ready.

**4. Was ist ein TCB, was ist ein Kontextwechsel?**
Der Task Control Block ist die Verwaltungsstruktur einer Task (Stackpointer, Priorität, Zustand). Beim Kontextwechsel wird der Kontext der alten Task gesichert und der der neuen wiederhergestellt.

**5. Präemptiv vs. kooperativ – Vor- und Nachteile?**
Präemptiv: Wichtige Tasks kommen sofort dran, aber niedrige können verhungern. Kooperativ: einfacher, aber eine Task kann die CPU blockieren. In der LVA: präemptiv verwenden, kooperativ vermeiden.

**6. Wie vermeidet man Starvation?**
Gleiche Prioritäten, nicht pollen, sondern blockierend auf Ereignisse warten, hohe Priorität nur für kurze, ereignisgesteuerte Tasks.

**7. Drei Tasks: C/T = 1/5, 2/10, 3/20 ms. Ist das nach RMS sicher planbar?**
U = 0,2 + 0,2 + 0,15 = 0,55. Die Grenze für n = 3 ist 0,78 → ja, planbar. Prioritäten: Periode 5 ms höchste, 20 ms niedrigste.

**8. Warum darf ein Interrupt mit Priorität 2 kein `xSemaphoreGiveFromISR` aufrufen?**
Er liegt über `configMAX_SYSCALL_INTERRUPT_PRIORITY` (5). Das RTOS sperrt in seinen kritischen Abschnitten nur Interrupts mit Zahlenwert 5 bis 15. Ein Interrupt mit Priorität 2 bleibt dabei aktiv und könnte das RTOS mitten in einer Listenoperation unterbrechen → undefiniertes Verhalten.

**9. Wozu `portYIELD_FROM_ISR`?**
Wenn die ISR eine höher priorisierte Task geweckt hat, wird direkt nach der ISR zu ihr gewechselt und nicht erst beim nächsten Tick. Das senkt die Latenz.

**10. Binärer Semaphor vs. Mutex?**
Semaphor = Signal, kein Besitzer, aus ISR freigebbar, keine Prioritätsvererbung. Mutex = Schutz einer Ressource, hat einen Besitzer, nur dieser gibt frei, nicht aus ISR, mit Priority Inheritance.

**11. Binärer vs. zählender Semaphor?**
Binär merkt sich nur „mindestens ein Ereignis“, mehrere Gives vor einem Take gehen verloren. Zählend zählt jedes Give mit.

**12. Was ist Prioritätsinversion und wie wird sie gelöst?**
Eine hohe Task wartet auf eine Ressource, die eine niedrige hält, und eine mittlere Task verdrängt die niedrige → die hohe wartet indirekt auf die mittlere. Lösung: Mutex mit Priority Inheritance.

**13. Was ist ein Deadlock, und wie verhindert man ihn?**
Zwei Tasks halten je eine Ressource und warten auf die der anderen. Er löst sich nicht von selbst. Vermeiden durch feste Anforderungsreihenfolge, Timeouts, nicht mehrere Ressourcen gleichzeitig halten, IO-Task mit Queue.

**14. Was passiert, wenn eine Task in eine volle Queue schreiben will?**
Sie blockiert bis zum Timeout. Danach meldet die Funktion einen Fehler (`osErrorTimeout`/`errQUEUE_FULL`) und die Nachricht ist nicht in der Queue. Rückgabewert prüfen!

**15. Queue, Stream Buffer, Message Buffer – wann was?**
Queue: feste Elementgröße, beliebig viele Teilnehmer. Message Buffer: variable Nachrichtenlänge, 1 zu 1. Stream Buffer: Bytestrom (UART), 1 zu 1.

**16. Warum sollen nicht alle Tasks direkt aufs Display schreiben?**
Ausgaben würden sich mischen (geteilte Ressource). Lösung: eine Display-Task, die als einzige zugreift und ihre Texte über eine Queue bekommt.

**17. Wofür sind Event Flags besser als Semaphore?**
Wenn auf mehrere Ereignisse gewartet wird (UND/ODER), für Broadcast an mehrere Tasks, und sie brauchen weniger RAM als mehrere Semaphore.

**18. Was darf man in einem Software-Timer-Callback nicht?**
Blockieren: kein `osDelay`, Queue- und Semaphor-Zugriffe nur mit Timeout 0. Der Callback läuft in der Timer-Service-Task und würde alle anderen Timer aufhalten.

**19. Warum braucht die HAL einen anderen Timer als SysTick?**
Der SysTick gehört dem RTOS-Scheduler. Teilen sich HAL und RTOS den Timer, werden `HAL_Delay` und alle HAL-Timeouts vom RTOS beeinflusst.

**20. Warum ist `HAL_Delay()` in einer Task schlecht?**
Es ist Busy-Waiting: Die Task bleibt Ready bzw. Running und verbraucht CPU-Zeit. Tasks gleicher Priorität kommen nur über die Zeitscheibe dran, niedrigere gar nicht. `osDelay()` dagegen blockiert und gibt die CPU frei.

---

*Quellen: Foliensätze „Echtzeit-Betriebssysteme für µC“ (Paulis/Fischer, V1.2.2), „OS4C – Betriebssysteme für Microcontroller“, „OS4C – Realtime Operatingsystems“ und „Inter-Task Communication“ (Fischer), LVA-Codebeispiele. Weiterführend: UM1722 (ST), FreeRTOS-Buch auf freertos.org, Brian Amos: „Hands-On RTOS with Microcontrollers“.*
