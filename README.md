# TeensyDSL v7.2

> **A fully self-contained, hardware-scriptable HID automation engine running on a Teensy 4.1.**
> Point it at any computer. No drivers. No software. No configuration. Just plug in, browse to your script, and watch it execute.

```
╔══════════════════════════════════════════════════════════════════╗
║           TeensyDSL v7 — Stability + Abstraction                ║
║     Tokenizer · Dispatch Table · Validator · Rate Limiter       ║
╠══════════════════════════════════════════════════════════════════╣
║  Board:  Teensy 4.1 (IMXRT1062, 600 MHz Cortex-M7)             ║
║  USB:    Serial + Keyboard + Mouse + Joystick                   ║
╚══════════════════════════════════════════════════════════════════╝
```

---

## What Is This?

TeensyDSL is a **custom Domain Specific Language (DSL) interpreter** burned onto a Teensy 4.1 microcontroller. Scripts written in the TeensyDSL language are stored on an SD card. When you plug the device into any USB host, it enumerates as a **native HID keyboard** — completely transparent to the OS, no drivers, no permissions, no install.

You navigate menus using five physical buttons and a 128×64 OLED display, select the target OS profile and script file, validate it on-device, then execute. The interpreter runs your script line by line, injecting keystrokes, combos, and timing sequences into the target machine exactly as programmed.

Built as a **solo embedded systems project**, TeensyDSL demonstrates a complete language design pipeline on constrained hardware: tokenizer → validator → dispatch table → cooperative execution — all inside 256 KB of SRAM and 1 MB of Flash, with a hardware watchdog watching over everything.

---

## Why It Was Built

Repetitive automation tasks — setting up developer environments, running reproducible test sequences, provisioning machines, CTF challenges — share one common frustration: you need the target machine to do the work before you have access to run anything on it. Existing solutions either require software on the target, a specific OS, or a commercial device with a locked ecosystem.

TeensyDSL was created to answer a simple question: **what if the automation engine lived entirely in the USB cable?**

The result is a device that:
- Works on **any OS** (Windows, macOS, Ubuntu, Gnome profiles built-in)
- Requires **zero installation** on the target machine
- Scripts are **plain text files** editable in any editor
- The entire device is **open, auditable firmware** — every byte of behavior is in this one `.ino` file

---

## Target Hardware

| Component | Details |
|---|---|
| **MCU** | PJRC Teensy 4.1 — NXP IMXRT1062, 600 MHz ARM Cortex-M7 |
| **USB** | Set to `Serial + Keyboard + Mouse + Joystick` in Arduino IDE |
| **SD Card** | Teensy 4.1 built-in SDIO slot — **no SPI, no CS pin**, uses `BUILTIN_SDCARD` |
| **Display** | Adafruit SSD1306 OLED, 128×64 px, I²C at `0x3C` (SDA→18, SCL→19) |
| **Buttons** | 5 tactile switches: UP (3), DOWN (4), SELECT (5), BACK (6), CHECKPOINT (7) |
| **LED** | Onboard LED on pin 13 — used for checkpoint blink alerts |

### Libraries Required

| Library | Source |
|---|---|
| `SD` | Teensy built-in |
| `Keyboard` | Teensy built-in |
| `Wire` | Teensy built-in |
| `Adafruit_SSD1306` + `Adafruit_GFX` | Arduino Library Manager |
| `Watchdog_t4` | Arduino Library Manager (optional but recommended) |

---

## Architecture

TeensyDSL is not a simple "type these keys" macro device. It's a layered interpreter with a real architecture:

```
 SD Card (.txt scripts)
        │
        ▼
  ┌─────────────┐
  │  File Loader │  ← line-by-line reader, 512 lines × 256 chars max
  └──────┬──────┘
         │
         ▼
  ┌─────────────┐
  │  Pre-Scanner │  ← first pass: registers all LABEL and FUNCTION definitions
  └──────┬──────┘
         │
         ▼
  ┌──────────────┐
  │   Validator  │  ← static analysis: syntax, arity, jump targets, call targets
  └──────┬───────┘
         │
         ▼
  ┌──────────────────┐
  │  Tokenizer       │  ← single-pass, quoted-string-aware, case-insensitive dispatch
  │  + Dispatch Table│  ← 50+ commands, O(n) lookup, arity checked before dispatch
  └──────┬───────────┘
         │
         ▼
  ┌─────────────────────────────────────────┐
  │  Cooperative Execution Loop             │
  │  One instruction per loop() tick        │
  │  Buttons + OLED updated every tick      │
  │  Hardware watchdog fed every tick       │
  │  HID rate-limiter enforced per event    │
  └─────────────────────────────────────────┘
```

### State Machine

The device runs a 7-state finite state machine:

```
ST_BOOT → ST_OS_MENU → ST_FILE_MENU → ST_VALIDATE_RESULT → ST_RUNNING
                                                                  │
                                               ST_CHECKPOINT ←───┤
                                               ST_HALTED    ←───┘
```

Every state has its own OLED renderer and input handler. The OLED refreshes at **30 Hz**, completely independent of script execution speed.

---

## The DSL — Language Reference

Scripts are plain `.txt` files stored in `/windows/`, `/mac/`, `/ubuntu/`, or `/gnome/` on the SD card. These folders are **auto-created at boot** if missing.

### Syntax Rules

- One instruction per line
- Command names are **case-insensitive** (uppercased internally by tokenizer)
- String arguments can be **quoted** to include spaces: `STRING "hello world"`
- Comments: `REM ...`, `# ...`, or `// ...`
- Labels: `myLabel:` or `LABEL myLabel`
- Functions: `FUNCTION myFunc` ... `END_FUNCTION`

---

### Complete Command Set

#### HID Text Injection

| Command | Syntax | Description |
|---|---|---|
| `STRING` | `STRING <text>` | Types text at full speed via scan-code injection |
| `STRINGLN` | `STRINGLN <text>` | Types text then presses ENTER |
| `SLOWPRINT` | `SLOWPRINT <text>` | Types with human-like random delays (8–16ms/char + micro-pauses) |
| `FASTPRINT` | `FASTPRINT <text>` | Types quickly with jitter (2–6ms/char) |

All text injection uses **explicit US-layout HID scan codes** via `safeTypeChar()`, handling the full printable ASCII set including all shift-modified symbols. No `Keyboard.print()` character guessing.

#### Key Control

| Command | Syntax | Description |
|---|---|---|
| `PRESS` / `HOLD` | `PRESS <key>` | Holds a key down |
| `RELEASE` | `RELEASE <key>` | Releases a held key |
| `TAP` | `TAP <key>` | Press + release with 50ms hold |
| `COMBO` | `COMBO <key1> <key2> ...` | Simultaneous multi-key chord (up to 8 keys), held for `20 + random(0,30)` ms |
| `RELEASEALL` | `RELEASEALL` | Releases all keys immediately |
| `SAFE_RELEASE` | `SAFE_RELEASE` | Releases all keys + 50ms settle delay |

Supported key names include: `CTRL`, `SHIFT`, `ALT`, `GUI`/`WIN`/`CMD`/`SUPER`, `ENTER`, `ESC`, `TAB`, `BACKSPACE`, `DELETE`, `INSERT`, `HOME`, `END`, `PAGE_UP`, `PAGE_DOWN`, `CAPS_LOCK`, `NUM_LOCK`, `SCROLL_LOCK`, `PRINT_SCREEN`, `PAUSE`, arrow keys, `F1`–`F12`, numpad keys (`NUM0`–`NUM9`, `NUM_ENTER`, `NUM_PLUS`, etc.), and all right-side modifier variants (`RCTRL`, `RSHIFT`, `RALT`, `RGUI`).

#### Timing

| Command | Syntax | Description |
|---|---|---|
| `DELAY` | `DELAY <ms>` | Precise non-blocking delay (feeds watchdog + updates OLED during wait) |
| `DEFAULT_DELAY` | `DEFAULT_DELAY <ms>` | Sets the inter-instruction jitter base delay |
| `WAIT_READY` | `WAIT_READY` | 500ms settle wait |
| `WAIT_STABLE` | `WAIT_STABLE` | Random 500–1000ms wait (simulates human hesitation) |

#### Variables

| Command | Syntax | Description |
|---|---|---|
| `SET` | `SET <var> <value>` | Assigns integer or string to a named variable (64 variables max) |
| `CLR` | `CLR <var>` | Resets variable to 0 |
| `INC` / `DEC` | `INC <var>` | Increments / decrements by 1 |
| `PRINT` / `ECHO` | `PRINT <var\|literal>` | Logs value to OLED log buffer + Serial |

#### Arithmetic

| Command | Syntax | Description |
|---|---|---|
| `ADD` | `ADD <dest> <a> <b>` | `dest = a + b` |
| `SUB` | `SUB <dest> <a> <b>` | `dest = a - b` |
| `MUL` | `MUL <dest> <a> <b>` | `dest = a * b` |
| `DIV` | `DIV <dest> <a> <b>` | `dest = a / b` (halts on divide-by-zero) |
| `CMP` | `CMP <a> <b>` | Sets `__CMP__` to -1 / 0 / 1 |

Operands can be **variable names or integer literals** — `resolveValue()` handles both transparently.

#### Control Flow

| Command | Syntax | Description |
|---|---|---|
| `LABEL` / `name:` | `LABEL loop_start` | Defines a jump target (registered at pre-scan) |
| `JMP` / `GOTO` | `JMP <label>` | Unconditional jump |
| `IF` | `IF <a> <op> <b> JMP <label>` | Conditional jump. Operators: `==`, `!=`, `>`, `<`, `>=`, `<=` (or `EQ`, `NEQ`, `GT`, `LT`, `GTE`, `LTE`) |
| `LOOP` | `LOOP <counter_var> <label>` | Decrements counter; jumps to label if > 0 |
| `FUNCTION` | `FUNCTION myFunc` ... `END_FUNCTION` | Defines a named subroutine |
| `CALL` | `CALL myFunc` | Pushes return address onto call stack (depth 32) and jumps to function |
| `RET` / `RETURN` | `RET` | Pops call stack and returns |
| `HALT` | `HALT` | Terminates script execution |

#### Memory

| Command | Syntax | Description |
|---|---|---|
| `STORE` | `STORE <addr> <var>` | Writes variable value to memory cell 0–255 |
| `LOAD` | `LOAD <var> <addr>` | Reads memory cell into variable |

#### Reliability

| Command | Syntax | Description |
|---|---|---|
| `RETRY` | `RETRY <n> <command...>` | Executes an inline command up to n times with random 100–300ms backoff between attempts |
| `CHECKPOINT` | `CHECKPOINT <message>` | Pauses execution, displays message on OLED, blinks LED 3×, waits for physical button press before continuing |

#### GPIO / Hardware

| Command | Syntax | Description |
|---|---|---|
| `PIN_MODE` | `PIN_MODE <pin> INPUT\|OUTPUT\|INPUT_PULLUP` | Sets a GPIO pin mode |
| `PIN_WRITE` | `PIN_WRITE <pin> HIGH\|LOW` | Digital write |
| `PIN_READ` | `PIN_READ <var> <pin>` | Digital read into variable |
| `ANALOG_READ` | `ANALOG_READ <var> <pin>` | Analog read (supports `A0` style pin names) |
| `SWITCH_WAIT` | `SWITCH_WAIT <pin> HIGH\|LOW` | Blocks until pin reaches target state (30s timeout) |
| `TONE` | `TONE <pin> <freq> <duration_ms>` | Plays a tone on a digital pin |

#### OLED Control (from script)

| Command | Syntax | Description |
|---|---|---|
| `OLED_CLEAR` | `OLED_CLEAR` | Clears the display |
| `OLED_PRINT` | `OLED_PRINT <row> <text>` | Prints text to a specific row (0–7) |
| `OLED_VAR` | `OLED_VAR <row> <var>` | Displays a variable's name and value on a row |

#### System / Debug

| Command | Syntax | Description |
|---|---|---|
| `DEBUG` | `DEBUG ON\|OFF` | Enables dry-run mode — all HID actions are logged but not sent |
| `RESET` | `RESET` | Clears all variables, call stack, and memory; restarts from line 0 |
| `MODE_CHECK` | `MODE_CHECK <var>` | Writes 0 (HID mode) into variable — reserved for mode-aware scripts |

---

## Example Scripts

### Open a terminal on Ubuntu and run a command

```
REM Ubuntu quick-terminal script
COMBO CTRL ALT t
WAIT_STABLE
STRING "echo Hello from TeensyDSL"
TAP ENTER
DELAY 1000
HALT
```

### Login sequence with retry

```
COMBO GUI r
WAIT_READY
STRING "cmd"
TAP ENTER
WAIT_STABLE
RETRY 3 STRINGLN "whoami"
HALT
```

### Loop with counter and conditional

```
SET counter 5
loopTop:
  OLED_VAR 3 counter
  COMBO GUI d
  DELAY 800
  DEC counter
  IF counter > 0 JMP loopTop
HALT
```

### Function call with checkpoint

```
FUNCTION typeCredentials
  SLOWPRINT "admin"
  TAP TAB
  SLOWPRINT "p@ssw0rd"
  TAP ENTER
END_FUNCTION

CHECKPOINT "Connect to target machine now"
WAIT_STABLE
CALL typeCredentials
HALT
```

---

## Safety & Reliability Systems

This firmware was designed with multiple independent safety layers:

**Hardware Watchdog** — The Teensy 4.1's `WDT_T4` hardware watchdog is configured with a 5-second interrupt and 10-second reset. If the firmware hangs (deadlock, infinite wait) the chip resets automatically. The watchdog is fed inside every `smartDelay()` call and in the main `loop()`.

**Software Step Limiter** — A 100,000-step counter detects infinite loops in the script itself and halts execution cleanly before the watchdog fires.

**HID Rate Limiter** — A dual-gate system enforces a minimum 2ms gap between HID reports and a burst cap of 50 events per 100ms window. A runaway `FASTPRINT` loop cannot saturate the USB HID pipe.

**Bounds Guards** — The `GUARD_BOUNDS` and `GUARD_BOUNDS_F` macros protect every array write in the interpreter (variables, labels, functions, call stack, memory array). Out-of-bounds access logs an error and returns instead of corrupting memory.

**Pre-execution Validator** — Before a script runs, a static analysis pass checks every line for argument count errors, unknown commands, and unresolved jump/call targets. The result is shown on the OLED. Scripts with syntax errors **cannot be run** until fixed. Scripts with only warnings can be run after user acknowledgment.

**Two-Tier Error System** — Parse errors (wrong arity, bad syntax) are errors that block execution. Unknown commands are warnings that allow execution. Fatal runtime errors (bad jump target, divide by zero) halt execution immediately.

**Non-Blocking `smartDelay()`** — All waits poll buttons, refresh the OLED, and feed the watchdog. The device is never unresponsive during a `DELAY`, even for multi-second waits.

**Debug Dry-Run Mode** — `DEBUG ON` suppresses all HID output and logs every action instead, letting you trace a script's behavior on the OLED without sending a single keystroke to the host.

---

## OLED UI

The 128×64 OLED renders at 30 Hz across six distinct screens:

| Screen | Description |
|---|---|
| **OS Menu** | Scrollable list of 4 OS profiles (Windows / macOS / Ubuntu / Gnome) |
| **File Menu** | Scrollable list of `.txt` scripts found in the selected OS folder |
| **Validation Result** | Error/warning counts. Auto-runs after 2s countdown if perfectly clean. |
| **Running** | Script name in header, live scrollable log buffer (6 visible lines, 24 total), real-time timestamp on every entry |
| **Checkpoint** | Full-screen halt with custom message, LED blink alert, waiting for CHK button |
| **Finished / Halted** | Final status message and script name |

Button mapping across all screens:
- `▲ / ▼` — Navigate menus, scroll logs
- `▶ (SELECT)` — Confirm selection, run script
- `◀ (BACK)` — Go back, abort
- `CHK (CHECKPOINT)` — Resume from checkpoint halt

---

## Interpreter Limits

| Parameter | Limit |
|---|---|
| Max script lines | 512 |
| Max line length | 256 characters |
| Max variables | 64 (int32 or string) |
| Max labels | 64 |
| Max functions | 32 |
| Call stack depth | 32 frames |
| Memory array | 256 × int32 cells |
| Max script steps | 100,000 (infinite loop guard) |
| Max tokens per line | 12 |
| Max token length | 128 characters |
| Log buffer | 24 lines × 64 characters |
| Files per OS folder | 32 |

---

## Build & Flash

1. Install [Teensyduino](https://www.pjrc.com/teensy/td_download.html)
2. Open `TeensyDSL_v7_2.ino` in Arduino IDE
3. Select **Board → Teensy 4.1**
4. Select **USB Type → Serial + Keyboard + Mouse + Joystick**
5. Install `Adafruit_SSD1306`, `Adafruit_GFX`, and `Watchdog_t4` via Library Manager
6. Upload

On first boot, the firmware creates `/windows/`, `/mac/`, `/ubuntu/`, and `/gnome/` directories on the SD card automatically.

---

## Wiring

```
Teensy 4.1
├── Pin 18 (SDA) ──────── OLED SDA
├── Pin 19 (SCL) ──────── OLED SCL
├── Pin 3  ──── BTN_UP ─── GND
├── Pin 4  ─── BTN_DOWN ── GND
├── Pin 5  ── BTN_SELECT ─ GND
├── Pin 6  ─── BTN_BACK ── GND
├── Pin 7  ─ BTN_CHECKPOINT GND
├── Pin 13 ──────────────── Onboard LED
└── Built-in SDIO slot ─── SD Card (no external wiring needed)
```

All buttons use `INPUT_PULLUP` — connect one leg to the pin, the other to GND. Software debounce is handled at 50ms.

---

## Project Context

TeensyDSL was created as a personal project exploring the intersection of embedded systems, language design, and hardware security tooling. It deliberately avoids abstractions that hide the hardware — every decision from the tokenizer design to the watchdog configuration is made with the constraints of a microcontroller in mind.

The firmware is a single self-contained `.ino` file by design. No build system, no dependencies beyond library installs, no external toolchain. Flash it and it works.

**Potential extensions in future versions:**
- LLM-to-DSL script generation pipeline (feed a task description, get a `.txt` script back)
- Mouse movement and click commands
- USB mass storage mode toggle (switch between HID and storage device)
- I²C/SPI peripheral commands for sensor-triggered automation
- Wi-Fi variant using ESP32-S3 as a wireless execution target

---

## License

MIT — do whatever you want with it. If you build something cool, a star on the repo is appreciated.

---

<p align="center">
  <i>Built on a Teensy 4.1 · 600 MHz · 1 MB Flash · A lot of late nights</i>
</p>
