# BLE-MIDI-Keyboard (M5StickS3)

（日本語: [README_JP.md](README_JP.md)）

Turn an ordinary **BLE HID keyboard** into a **USB-MIDI keyboard controller**.

The M5StickS3 connects to a BLE keyboard as a HID host, and at the same time
enumerates on the PC over USB-C as a **"USB Audio device (MIDI class)"**. Key
presses on the BLE keyboard are translated into USB-MIDI messages
(note-on / note-off / CC), so any DAW or soft-synth on the PC sees a standard
MIDI input named **`M5StickS3 BLE-MIDI`**.

```
 BLE keyboard  ──BLE HID──▶  M5StickS3 (ESP32-S3)  ──USB-MIDI──▶  PC / DAW
```

---

## Hardware

| Part | Notes |
|------|-------|
| **M5StickS3** (ESP32-S3, 135×240 LCD) | primary target; buttons A/B used for the UI |
| A **BLE** (Bluetooth Low Energy) HID keyboard | must expose HID over GATT (service `0x1812`). Classic-Bluetooth-only keyboards do **not** work — the ESP32-S3 has no Classic BT radio |
| USB-C cable to the PC | carries USB-MIDI + a USB CDC serial console |

A bare **ESP32-S3 Dev board** (no screen) also works — see
[Headless operation](#headless-operation).

---

## Build & flash

### Board options (required)

| Option | Value |
|--------|-------|
| Board | `M5StickS3` (`m5stack:esp32:m5stack_sticks3`) |
| USB Mode | **USB-OTG (TinyUSB)** |
| USB CDC On Boot | **Enabled** |
| Partition Scheme | `8M with spiffs` (`default_8MB`) or larger |

`USB-OTG (TinyUSB)` is mandatory: the built-in `USBMIDI` class only exists in
that mode. The sketch has a `#error` guard that fires if `ARDUINO_USB_MODE`
indicates Hardware-CDC mode.

### Libraries

| Library | Source |
|---------|--------|
| **NimBLE-Arduino** (>= 2.5) | Library Manager / `arduino-cli lib install NimBLE-Arduino` |
| `M5Unified`, `USB` / `USBMIDI`, `Preferences` | bundled with the M5Stack ESP32 core |

### arduino-cli

`sketch.yaml` already pins the FQBN and options:

```bash
arduino-cli compile --profile default        # or:
arduino-cli compile --fqbn "m5stack:esp32:m5stack_sticks3:PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=cdc" .
arduino-cli upload  --fqbn "m5stack:esp32:m5stack_sticks3:PartitionScheme=default_8MB,USBMode=default,CDCOnBoot=cdc" -p /dev/cu.usbmodemXXXX .
```

After flashing, the device shows up on the PC as a composite USB device:
a serial port (115200) **and** a MIDI port `M5StickS3 BLE-MIDI`.

---

## First use (pairing)

1. Power the M5StickS3. With no stored bond it starts in **SCAN**.
2. Put the BLE keyboard into pairing mode (or just wake it so it advertises).
3. **BTN-A** moves the cursor through the list (one direction, wraps).
   Only devices that advertise a name are listed; `HID` (green) marks a
   keyboard-class device.
4. **BTN-B** selects the highlighted device. It connects and bonds using
   **Just Works** pairing (no passkey). The bond and the device name are saved
   to NVS.

> Keyboards that force a numeric passkey are not supported by the Just Works
> flow and will fail to bond (see the serial log).

---

## Auto-reconnect

BLE keyboards sleep quickly when idle and drop the link. This firmware makes
reconnection **automatic** — no button press needed:

* On boot, if a bond exists, the app goes straight to **WAIT** and arms an
  **asynchronous connection with no timeout**. The BLE controller holds the
  connection attempt pending and connects the instant the keyboard advertises
  again (e.g. when you press any key to wake it).
* If the keyboard sleeps during **PLAY**, the app returns to **WAIT** by itself
  and re-arms — just start typing again.
* Bonded keyboards that use a rotating random address are resolved via the
  stored IRK, so the pending connect still matches.

Keeping the keyboard awake from the ESP32-S3 side is **not** attempted: keyboard
firmware sleeps on *key* inactivity and terminates the link on sleep, so
central-side traffic cannot prevent it.

### State machine

| State | Meaning | BTN-A | BTN-B |
|-------|---------|-------|-------|
| **SCAN / LIST** | scanning, device list shown | move cursor (wraps) · *long*: rescan | select · *long*: forget all bonds |
| **WAIT** | async auto-connect armed for the bonded device | cancel → SCAN | retry now |
| **PLAY** | translating keys to USB-MIDI | — | disconnect → SCAN (stops auto-reconnect) |
| **FAILED** | connect / setup failed | → SCAN | re-arm WAIT for the same device |

Post-connect setup (encrypt + discover + subscribe) is retried up to 5 times
before dropping to **FAILED**.

---

## Key mapping

![](us-keyboard.jpeg)

### 1. Piano keys

`keydown` → note-on, `keyup` → note-off. Velocity is variable (see §4),
default **98**.

| Key | `A` | `W` | `S` | `E` | `D` | `F` | `T` | `G` | `Y` | `H` | `U` | `J` | `K` | `O` | `L` | `P` | `;` | `'` |
|-----|----|----|----|----|----|----|----|----|----|----|----|----|-----|------|-----|------|-----|-----|
| Note | C | C# | D | D# | E | F | F# | G | G# | A | A# | B | C+1 | C#+1 | D+1 | D#+1 | E+1 | F+1 |
| Offset | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 | 14 | 15 | 16 | 17 |

MIDI note played = **octave base + offset**. Held notes are re-sounded at the
new pitch when the octave changes.

> The US-layout `'` key is the `:` key on a JIS keyboard — same keycode
> (`0x34`), same mapping.

### 2. Octave

Handled on `keydown`.

| Key | `1` | `2` | `3` | `4` | `5` | `6` | `7` | `8` | `9` | `0` | `-` |
|-----|----|----|----|----|-----|-----|-----|-----|-----|-----|-----|
| Base note | 0 | 12 | 24 | 36 | **48** | 60 | 72 | 84 | 96 | 108 | 120 |

`5` (base 48) is the default. Each key covers `base … base+17`; resulting notes
above **127** are dropped.

| Key | Action |
|-----|--------|
| `Z` | octave **down** (base −12, floor 0) |
| `X` | octave **up** (base +12, ceiling 120) |

### 3. Sustain pedal

| Key | keydown | keyup |
|-----|---------|-------|
| `TAB` | CC64 = 127 (sustain on) | CC64 = 0 (sustain off) |

### 4. Velocity

Handled on `keydown`. Applies to subsequent note-ons (default **98**).

| Key | Action |
|-----|--------|
| `C` | velocity −5 (floor 1) |
| `V` | velocity +5 (ceiling 127) |

All MIDI is sent on **channel 1**.

---

## Serial console

The USB CDC serial port (115200) mirrors the on-screen guidance and streams key
events, e.g.:

```
========================================
[WAIT] auto-connect armed for My Keyboard  [10:05:aa:3f:0a:28]
  connects automatically as soon as the keyboard wakes up
  a  BTN-A press = cancel -> scan
  b  BTN-B press = retry now
========================================
[   42500] DOWN A      (0x04)  ON  C3 48
[   43120] DOWN Z      (0x1D)  OCT C2 (36)
[   44300] DOWN TAB    (0x2B)  SUSTAIN on
[   45010] DOWN V      (0x19)  VEL 103
```

## Headless operation

For a screen-less board, the app is fully operable from the serial console:

| Input | Action |
|-------|--------|
| `a` / `b` | BTN-A / BTN-B **press** |
| `A` / `B` | BTN-A / BTN-B **long press** |
| `?` | reprint current state + guide (and device list while scanning) |

Physical buttons and serial input are OR'd, so both work at once.

To build for a bare ESP32-S3 Dev board, comment out `#include <M5Unified.h>`
and then comment out every line that fails to compile (all `M5.*` / `canvas.*`
display and button code). The serial console remains as the full UI.

---

## Limitations

* **BLE HID only.** Classic Bluetooth keyboards are not supported.
* **Just Works pairing only.** Keyboards that require a passkey will not bond.
* **Boot-style report parsing**: an 8-byte report `[mods][rsv][k0..k5]`.
  Consumer/media keys and NKRO reports are ignored.
* During each (re)connect there is a ~1–2 s pause (pairing + service discovery)
  while the display and serial console are unresponsive.

---

## Files

| File | |
|------|--|
| `BLE-MIDI-Keyboard.ino` | the sketch |
| `sketch.yaml` | arduino-cli profile: FQBN, board options, port |
| `README.md` | this file |
