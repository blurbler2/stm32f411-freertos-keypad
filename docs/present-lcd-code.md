# Presenting the LCD code

Notes for presenting the `lcd1602` driver: what to show, what to say, the numbers to know and the
questions that may come up. The full reference (wiring, API, configuration) is in
[lcd1602-library.md](lcd1602-library.md). Line numbers refer to commit `7fae595`.

> **The message in one sentence:** we turned the provided LCD1602 library, which busy-waits about
> 6 ms per character, into a driver that works under FreeRTOS: one gatekeeper task owns the
> display, no other task ever waits for it, only changed characters are sent, and the I2C
> transfer runs via DMA, protected by a mutex, while every time of the HD44780 timing diagram
> is met.

## 1. Suggested structure (about 12 minutes)

| # | Part | Time | What to show |
|---|---|---|---|
| 1 | Hardware and wiring | 1-2 min | Board, level shifter, LCD; the wiring diagram |
| 2 | Basics from the lecture | 2-3 min | LCD, HD44780, PCF8574 (section 3) |
| 3 | From LCD1602 to code that works under FreeRTOS | 3 min | The problem/solution table (section 4) |
| 4 | Code walkthrough | 4 min | Seven places in the code (section 5) |
| 5 | Demo | 2 min | Keys, LCD, log, unplugging the display (section 6) |
| 6 | Summary and questions | 1 min | Section 9 |

## 2. Hardware and wiring

Wiring diagram and checklist: [lcd1602-library.md, section 2](lcd1602-library.md#2-wiring-with-the-level-shifter-pegelwandler).

What to say:

- The LCD (TC2004A-01, 20 x 4, HD44780) needs 5 V for a good contrast, the STM32 works with 3 V.
  The PCF8574T module on the back of the LCD turns 4 wires (5V, GND, SDA, SCL) into the 8 parallel
  LCD signals.
- The level shifter (Pegelwandler) sits between them: **LV = 3V** of the board, **HV = 5V**,
  **GND connected on both sides**. SDA (PB7) and SCL (PB8) each go through one channel.
- A PCF8574 at 5 V needs at least 3.5 V for a logic high (0.7 × VDD), so a 3.3 V bus would be out of
  spec. The level shifter gives each side its own levels.
- We use **PB7/PB8** and not PB6/PB9 from the slides, because on the STM32F411E-DISCO PB6/PB9
  are the I2C bus of the on-board audio chip and accelerometer.

## 3. Basic concepts from the lecture

### 3.1 How an LCD works (slides 3-4)

- An LCD does not emit light, it **controls light**: in a TN cell the liquid crystal between two
  crossed polarisers turns the light by 90°. A voltage untwists the molecules and the pixel turns
  dark. The light comes from the backlight.
- What counts is the **RMS voltage** (Effektivwert). A DC voltage destroys the liquid crystal, so
  the display is driven with AC.
- **Multiplexing** saves connections: 2 x 16 characters with 5 x 8 dots would need 1281 lines
  statically, with 8 commons only 168. The commons (rows) are activated one after another.
- The price is contrast: an OFF pixel also gets some voltage. With 16 commons (HD44780, two lines)
  an ON pixel has only **29 % more RMS voltage** than an OFF pixel (Alt-Pleshko limit,
  D_max = √(5/3) ≈ 1.29, bias 1/5). That is why there is a **contrast trimmer** and why the module
  runs at **5 V**.

### 3.2 The HD44780 controller (slides 5-12)

- The controller sits on the LCD board and receives **commands** (RS = 0) and **data** (RS = 1).
- **R/W** is tied to "write": the busy flag is never read, instead the driver relies on the
  execution times (37 µs per instruction, 1.52 ms for Clear/Home).
- **E** (Enable): when writing, the controller takes the data on the **falling edge** of E.
- **4-bit mode**: every byte is sent as two nibbles, the high nibble first. That saves 4 data lines.
- The timing is in **nanoseconds**: t_AS 140 ns, t_PWH 450 ns, t_DSW 195 ns, t_H 10 ns (slide 10).
  A `HAL_Delay(1)` waits more than 2000 times the required E pulse width.
- **Init**: wait 40-50 ms after power-up, three times `0x3`, then `0x2` for 4-bit mode, then the
  function set `0x28`.
- **DDRAM addresses** of the 20x4 display: `0x00 / 0x40 / 0x14 / 0x54`. Internally there are two
  lines of 40 characters, "folded" onto four rows: after the 20th character of row 1 comes row 3.

### 3.3 The I2C module PCF8574 (slides 22-26)

- An I/O expander without registers: **every byte appears at once on P7..P0**.
- Upper nibble = LCD data D7..D4, lower nibble = backlight, E, R/W, RS.
- The backlight is just a bit: it must be set in **every** byte.
- After power-up all outputs are high, also E.
- Address `0x27` (PCF8574T) or `0x3F` (PCF8574AT); the HAL wants it shifted left: `0x4E`.
- At 100 kHz one byte takes **90 µs**. That is far longer than every time in the timing diagram,
  so the E pulse and the execution time come for free. Only Clear/Home (2 ms) and the init need
  real waiting.
- Slide 26: 4 bytes per character, **7.6 ms per row**, **30 ms for the whole display**.

### 3.4 The driver under FreeRTOS (slides 27-30)

| Problem with the simple driver (slide 27) | Consequence under FreeRTOS | Solution |
|---|---|---|
| `HAL_Delay()` busy-waits | The task occupies the CPU, lower-priority tasks stop | `vTaskDelay(pdMS_TO_TICKS(ms) + 1)` |
| `HAL_I2C_Master_Transmit()` polls | 7.6 ms of CPU time wasted per row | DMA (or interrupt) + semaphore |
| Cursor and text are two steps | Task B writes in between: "Zeichensalat" | Gatekeeper task |
| LCD and sensors share I2C1 | `HAL_BUSY` or broken transfers | Mutex around every transfer |

- **DMA + semaphore** (slide 28), six steps: take the mutex, start the DMA, `xSemaphoreTake()`
  (the task blocks, the CPU is free), the DMA sends, the interrupt gives the semaphore
  (`xSemaphoreGiveFromISR()` + `portYIELD_FROM_ISR()`), release the mutex. The transfer takes just
  as long, but the time belongs to the other tasks.
- **Gatekeeper** (slide 29): only one task writes to the display. The others send messages into
  a queue with timeout 0 and never block.
- **One queue + frame buffer instead of four queues** (slide 30): one waiting point, any position
  and length, the newest value wins, refresh 5 times per second with only the changes, automatic
  re-initialisation after errors.
- Trick from the lecture: **send only the characters that changed**. One changed character costs
  about 1 ms on the bus instead of 30 ms for the whole display.

### 3.5 Portability (slide 31)

Manufacturer code only in the port. The core (protocol, bus logic, tasks) stays unchanged when
the driver moves to another MCU, e.g. from STM32 to NXP.

## 4. From the provided library LCD1602 to RTOS-ready code (betriebssystemtauglicher Code mit FreeRTOS)

### 4.1 Starting point: how `LCD1602.c` sends a character

Every nibble goes through this function (`LCD1602.c`, line 87):

```c
static void LCD_SendCmd_8bit( uint8_t bt ){
	LCD_WriteByte( portLcd |= 0x04 );   // E = 1
	HAL_Delay( 1 );                     // busy-waits 1-2 ms
	LCD_WriteByte( portLcd | bt );      // data with E = 1
	LCD_WriteByte( portLcd &=~ 0x04 );  // E = 0, but the data bits are 0 here
	HAL_Delay( 1 );                     // busy-waits 1-2 ms
}
```

Every `LCD_WriteByte()` is its own blocking `HAL_I2C_Master_Transmit()` with address and one
byte. One character = 1 byte for RS + 2 nibbles × 3 transfers = **7 I2C transfers and 4 ×
`HAL_Delay(1)`**, about **6 ms per character** with the CPU busy all the time (slide 21). A full
20x4 screen takes about half a second.

### 4.2 What we changed

| # | Provided LCD1602 | Consequence under FreeRTOS | `lcd1602` | Where |
|---|---|---|---|---|
| 1 | `HAL_Delay(1)` twice per nibble | About 6 ms busy waiting per character, lower-priority tasks stop | No waiting between the bytes: the 90 µs per I2C byte cover all times of the diagram. `vTaskDelay()` only for Clear and init | [`encode_byte()`](../Core/Src/lcd1602.c#L190), [`port_delay_ms()`](../Core/Src/lcd1602.c#L148) |
| 2 | One blocking transfer per byte | CPU polls the I2C flags, address byte repeated 7 times per character | One transfer per row, sent by DMA; the task sleeps on a semaphore | [`dma_transfer()`](../Core/Src/lcd1602.c#L261), [`bus_write()`](../Core/Src/lcd1602.c#L283) |
| 3 | No protection of the I2C bus | Collisions with other I2C devices (`HAL_BUSY`, broken transfers) | Mutex around every transfer, can be shared with other drivers | [`bus_write()`](../Core/Src/lcd1602.c#L283), [`lcd1602_start()`](../Core/Src/lcd1602.c#L514) |
| 4 | Every task may call `LCD_SetCursor()` + `LCD_PrintString()` | Two steps: another task can write in between ("Zeichensalat") | Gatekeeper task owns the display; the other tasks only send messages (timeout 0) | [`lcd1602_print()`](../Core/Src/lcd1602.c#L552), [`lcd1602_task()`](../Core/Src/lcd1602.c#L474) |
| 5 | Always rewrites whole strings | Up to 30 ms of bus time for the display | Frame buffer: every 200 ms only the changed range of each row | [`refresh()`](../Core/Src/lcd1602.c#L385) |
| 6 | `LCD_Init()` in `main()` with `HAL_Delay()` | Before the scheduler starts there is no tick (slide 27); no recovery after errors | Init in the gatekeeper task with `vTaskDelay()`, again after every bus error | [`display_init()`](../Core/Src/lcd1602.c#L335) |
| 7 | Sends `0x38` after switching to 4-bit mode (line 236) | DL = 1 switches the controller back to 8-bit mode; works only by accident | Sends `0x28` | [`display_init()`](../Core/Src/lcd1602.c#L335) |
| 8 | Data bits are cleared in the byte in which E falls (see 4.1) | Data hold time t_H = 0 instead of ≥ 10 ns | Bytes 3 and 5 keep the data while E falls | [`encode_byte()`](../Core/Src/lcd1602.c#L190) |
| 9 | Hardware settings spread over the code (`hi2c3`, address, GPIO variant, Cyrillic font) | Porting means editing the whole driver | Hardware only in section 1 of the header and the port section | [`lcd1602.h`](../Core/Inc/lcd1602.h#L81), [port section](../Core/Src/lcd1602.c#L73) |
| 10 | Global variable `portLcd` | Shared state, not safe with several tasks | State only inside the gatekeeper task | [`lcd1602.c`](../Core/Src/lcd1602.c#L50) |
| 11 | Return values of the HAL ignored | A missing display is not noticed and nothing is retried | Status codes, I2C reset after a timeout, retry every 2 s, log message | [`handle_status()`](../Core/Src/lcd1602.c#L466) |

What stayed the same: the PCF8574 bit layout, the 4-bit protocol, the row offsets
`0x00 / 0x40 / 0x14 / 0x54`, the init idea (three times `0x3`, then `0x2`).

Left out because the application does not need them: custom characters (CGRAM), cursor and blink,
display shift, the Cyrillic font and the direct 4-bit GPIO variant. They could be added as further
message types.

### 4.3 Why 5 bytes per character instead of 4 (slide 26)

In the 4-byte sequence of slide 26, E rises in the **same byte** in which RS and the data are set.
The PCF8574 switches all outputs at the same moment, so t_AS ≈ 0 instead of ≥ 140 ns. Most
displays accept this, but it is outside the data sheet. `lcd1602` sends one extra byte first,
with RS and data valid and E still low, so t_AS = 90 µs:

| | After `START 0x4E` | Bytes per character | Whole row |
|---|---|---|---|
| Slide 26 | `0x4D 0x49 0x1D 0x19` | 4 | 7.6 ms |
| `lcd1602` | `0x49 0x4D 0x49 0x1D 0x19` | 5 | 9.5 ms |

This costs 25 % more bus time, which hardly matters because only the changes are sent. The PC
test checks t_AS and t_H for every byte.

### 4.4 Before and after

| | Provided LCD1602 | `lcd1602` |
|---|---|---|
| One character | About 6 ms, the CPU busy-waits | 0.45 ms on the bus, a few µs of CPU time with DMA |
| One row | About 120 ms busy waiting | One transfer, 9.5 ms on the bus, CPU free |
| Whole 20x4 display | About 0.5 s | 38 ms if everything changed, usually about 1 ms (only changes) |
| Calling task | Blocks until the text is out | Returns at once (queue, timeout 0) |
| Several tasks write | Text can get mixed up | Impossible: only the gatekeeper writes |
| Other I2C devices | Collisions | Mutex |
| Display unplugged | Errors ignored | Detected, retried every 2 s, content restored |
| Timing diagram | t_H not met (data cleared with E) | All times met, checked by the test |
| Porting | HAL calls everywhere | HAL only in the port section |

## 5. Code walkthrough

Show these places in this order:

1. **Configuration** – [`lcd1602.h`, section 1](../Core/Inc/lcd1602.h#L81).
   *"The only hardware-dependent part: I2C handle, interrupts, address, display size. To port the
   driver, this section and the port section change, nothing else."*
2. **API and use in the application** – [`lcd1602.h`, section 2](../Core/Inc/lcd1602.h#L131),
   [`app.c` line 205](../Core/Src/app.c#L205) (`lcd1602_start()`), [`App_Display()`](../Core/Src/app.c#L123).
   *"The KeyTask only calls `lcd1602_write_line()`. That copies the text into the queue with
   timeout 0 and returns at once: the task never waits for the display."*
3. **Timing diagram** – [`encode_byte()`](../Core/Src/lcd1602.c#L190) with the comment above it
   (from line 154). *"Each I2C byte is one step of the timing diagram: RS first, then E high,
   then E low with the data still there. Because one byte takes 90 µs, we need no delay between
   the bytes."*
4. **DMA + semaphore + mutex** – [`dma_transfer()`](../Core/Src/lcd1602.c#L261),
   [`bus_write()`](../Core/Src/lcd1602.c#L283) and the interrupt side
   [`signal_from_isr()`](../Core/Src/lcd1602.c#L220). The comments `step 1` to `step 6` are the
   six steps of slide 28. *"While the DMA sends, the task is blocked on the semaphore and the CPU
   runs the other tasks. The interrupt gives the semaphore and ignores transfers of other drivers
   on the same I2C peripheral."*
5. **Gatekeeper and frame buffer** – [`refresh()`](../Core/Src/lcd1602.c#L385) and
   [`lcd1602_task()`](../Core/Src/lcd1602.c#L474). *"frame is what should be shown, shown is what
   is on the glass. Every 200 ms we compare them and send per row only the range from the first to
   the last changed character, with the cursor command, in one transfer."*
6. **Init** – [`display_init()`](../Core/Src/lcd1602.c#L335). *"Runs in the task, so the waits
   use `vTaskDelay()`. Note `0x28`: the original sends `0x38` and switches back to 8-bit mode."*
7. **Test** – [`sim_port_write()`](../tests/host/test_lcd1602.c#L76) in the PC test.
   *"Every byte goes into a simulated PCF8574 + HD44780 that checks t_AS and t_H. If E rises in
   the same byte as RS, the test fails."*

## 6. Demo

Before the presentation:

- Check the wiring with the checklist in [lcd1602-library.md](lcd1602-library.md#checklist-before-switching-on).
- Build and flash (README, "Build and run"). Adjust the contrast.
- Optional: USB-UART adapter on PA2 (115200 baud) for the log; logic analyser on PB7/PB8 (LV side,
  3 V) with I2C decoder.

| Step | Do | Expected | Say |
|---|---|---|---|
| 1 | Switch on | Row 1 `Keypad demo`, row 2 `Press a key`, row 4 `I2C: DMA` (or `I2C: blocking` without the CubeMX DMA settings) | "Row 4 shows whether the driver found the DMA settings" |
| 2 | Press `1`..`4` | LED toggles at once, row 2 shows `Key 1 PRESS` / `RELEASE` | "The KeyTask never waits for the display" |
| 3 | Hold `D` | Running light keeps stepping, row 2 shows `HOLD` / `REPEAT` | "The display updates do not slow down the other tasks" |
| 4 | Analyser on SDA/SCL, press a key | A short burst: `0x4E`, 5 bytes cursor command, then 5 bytes per changed character | "Only the changed characters, one transfer per row" |
| 5 | Pull SDA, press a key | Log `LCD not found` | "The failed transfer is detected" |
| 6 | Plug SDA back in | Within 2 s log `LCD found ...`, the current text is back | "Automatic re-initialisation; texts sent meanwhile were kept" |

The driver only notices a missing display when it tries to send something, so press a key in
step 5.

## 7. Numbers to remember

| | Value |
|---|---|
| I2C byte at 100 kHz | 90 µs (8 bits + ACK) |
| Bytes per character | 5 (slide 26: 4) |
| One changed character incl. cursor command | about 1 ms |
| Whole row | 9.5 ms (slide 26: 7.6 ms) |
| Whole display | 38 ms (slide 26: 30 ms) |
| Original library | about 6 ms per character, busy waiting |
| Refresh period | 200 ms (5 times per second) |
| HD44780 execution time | 37 µs, Clear/Home 1.52 ms |
| Power-up wait | 40-50 ms |
| Timing diagram | t_AS 140 ns, t_PWH 450 ns, t_DSW 195 ns, t_H 10 ns |
| PCF8574 high level at 5 V | ≥ 3.5 V (0.7 × VDD) |
| Address | 0x27 (HAL: 0x4E) |
| Interrupt priority for `...FromISR` | 5 or higher (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`) |

## 8. Questions you may get

**Why a level shifter, if PB7/PB8 are 5 V tolerant?**
It would work without it, because I2C is open-drain and the pins are FT. The level shifter is the
clean solution: the STM32 never sees 5 V, the PCF8574 sees full 5 V levels, and it would also work
on pins that are not 5 V tolerant.

**Why a mutex, when the LCD is the only I2C device?**
Slide 29: sensors will use the same bus. The mutex makes the driver ready for that, and other
drivers can take the same mutex (`lcd1602_bus_mutex()`).

**Gatekeeper and mutex: isn't one enough?**
They protect different things. The gatekeeper protects the display (cursor command and text belong
together, only one writer). The mutex protects the bus against other devices.

**Why a mutex and not a binary semaphore for the bus?**
Priority inheritance: if a high-priority sensor task waits for the bus while the low-priority LCD
task holds it, the LCD task temporarily gets the high priority, so medium-priority tasks cannot
delay it (no priority inversion).

**With DMA the task still waits on the semaphore. What did DMA gain?**
CPU time. The task is blocked, not busy: during the 9.5 ms of a row the scheduler runs the other
tasks. With the polling HAL function, the CPU would spend these 9.5 ms checking flags.

**How did you get rid of the 6 ms per character?**
No millisecond delays between the bytes: one I2C byte takes 90 µs, which is longer than every time
in the timing diagram and the 37 µs execution time. Only Clear/Home and the init need real waiting.
On top of that, DMA frees the CPU and only changed characters are sent.

**Why 5 bytes per character and not 4 like on slide 26?**
See 4.3: with 4 bytes RS changes in the same byte in which E rises (t_AS = 0).

**What happens when the queue is full?**
The message is dropped and the function returns `LCD1602_ERR_QUEUE_FULL`. The sender never waits.
The next message brings the newest text anyway.

**Why every 200 ms and not at once?**
Several changes within 200 ms are combined into one transfer, so the bus load has an upper limit.
5 updates per second are enough for the eye (slide 30). The text appears at most 0.2 s late.

**Why `vTaskDelay(pdMS_TO_TICKS(ms) + 1)`?**
`vTaskDelay(1)` waits until the next tick, which can come almost at once. With + 1 the delay is at
least the requested time (slide 27).

**Why is the display initialised in the task and not in `main()`?**
Before the scheduler starts there is no tick, so `vTaskDelay()` does not work (slide 27). The
task can also initialise the display again after an error.

**Why priority 5 for the I2C and DMA interrupts?**
Interrupts that call `xSemaphoreGiveFromISR()` must have a priority number of at least
`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` (5). A smaller number would interrupt the kernel in
its critical sections.

**What does `portYIELD_FROM_ISR()` do?**
If the semaphore woke a task with a higher priority than the interrupted one, the scheduler switches
to it directly when the interrupt ends, not only at the next tick.

**What happens if the display is unplugged?**
The next transfer fails (no ACK) or times out. The driver marks the display as not ready, logs
`LCD not found`, resets the I2C peripheral after a timeout and tries again every 2 s. After the new
init the whole frame is sent again.

**What if the DMA is not set up in CubeMX?**
`lcd1602_start()` checks the DMA channel and the I2C interrupts. If something is missing, the
driver uses blocking I2C: only the gatekeeper task waits, the other tasks still never wait. Row 4
and the log say why.

**Why static allocation?**
No heap needed, the memory is known at link time, and creating the objects cannot fail at run time.

**How would you port the driver to NXP?**
Change section 1 of `lcd1602.h` and the port functions (vendor HAL calls). The timing diagram, the
bus logic with mutex and semaphore, the gatekeeper task and the API stay the same.

**How did you test without the board?**
A PC test runs the real driver code with mocked FreeRTOS and HAL. A simulated PCF8574 + HD44780
decodes every byte and checks the timing diagram, the init and the display content. Two deliberate
bugs (E with RS in one byte, `0x38`) make the test fail.

**Why does `refresh()` also send unchanged characters between two changes?**
Slide 29: one transfer per row. Every extra cursor command costs 5 bytes, as much as one
character. For a gap of one or two unchanged characters, sending them is just as cheap, and one
range per row keeps the code simple.

## 9. Summary: what we learned

1. **An LCD controls light.** The RMS voltage counts, DC destroys it, and multiplexing limits the
   contrast: hence the contrast trimmer and the 5 V supply.
2. **HD44780:** 4-bit mode, high nibble first, takes the data on the falling edge of E, init with
   three times `0x3`. The timing is in nanoseconds, not milliseconds.
3. **PCF8574:** every I2C byte sets all 8 LCD signals. At 100 kHz a byte takes 90 µs, so the
   timing comes for free; the backlight bit must be in every byte.
4. **FreeRTOS:** no busy waiting (`vTaskDelay`, DMA with semaphore), one gatekeeper owns the
   display, a mutex protects the shared bus, a frame buffer sends only the changes.
5. **Portability:** vendor code only in the port; the core, bus logic and tasks stay unchanged.

## 10. German terms for the presentation

| English | German |
|---|---|
| Level shifter | Pegelwandler |
| Busy waiting | aktives Warten |
| Frame buffer / what should be shown / what is shown | Bildspeicher / Soll-Bild / Ist-Bild |
| Mixed-up characters | Zeichensalat |
| Gatekeeper task | Gatekeeper-Task |
| RTOS-ready code | betriebssystemtauglicher Code |
| Falling edge | fallende Flanke |
| Execution time | Ausführungszeit |
| RMS voltage | Effektivwert |
| Contrast trimmer | Kontrast-Poti |
| Shared resource | geteilte Ressource |
| Priority inversion / inheritance | Prioritätsinversion / Prioritätsvererbung |
