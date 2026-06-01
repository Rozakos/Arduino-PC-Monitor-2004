# Arduino PC Monitor — 2004 LCD

Firmware for an **Arduino Nano** that drives one to four **20×4 (2004) I²C
character LCDs** as a live PC stats display (CPU, GPU, RAM, network). A
companion Python GUI on the host sends stats over USB serial; the firmware
parses them and renders bars + readouts, rotating through pages.

This is the 2004 successor to the original 1602 firmware — same serial
protocol, redesigned layout for the larger screen.

## Hardware

- **MCU:** Arduino Nano, **ATmega168P** (default) or **ATmega328P**.
- **Displays:** up to 4× 20×4 HD44780 LCDs on PCF8574 I²C backpacks.
- **Wiring:** `A4 = SDA`, `A5 = SCL`, `5V = VCC`, `GND = GND`.
- Give each backpack a **unique I²C address** via its `A0/A1/A2` jumpers.
  The firmware auto-scans the common address ranges (`0x20–0x27`, `0x38–0x3F`).

## Display layout

20×4 lets each screen show **two metrics at once** ("hybrid" layout). Each
metric is a **2-row block**:

```
Row 0:  CPU  45%        52.5°C     <- label: usage / temp / mem / speed
Row 1:  [#################   ]     <- full-width bar
Row 2:  GPU  90% 7.5/8G   61°C     <- second metric label
Row 3:  [###################  ]    <- its bar
```

- Each LCD groups its assigned pages into consecutive **pairs** and rotates
  through the pairs every `PDLY` ms. An odd page out leaves the bottom block
  blank.
- **NET** is a single block: the label shows `DL` and `UL`, and the bar row is
  split — download on the left half, upload on the right half (scaled to the
  link speed `LSPD`).
- **GPU** label adapts: if the GPU temperature has a decimal it shows
  `usage% + decimal temp`; otherwise `usage% + integer temp`, with the VRAM
  ratio inserted in the middle when it fits.
- Bars use segmented custom characters with rounded ends.

## Pages

| ID | Page | Bit (in `PG` mask) |
|----|------|--------------------|
| 0  | CPU  | `0x01` |
| 1  | GPU  | `0x02` |
| 2  | RAM  | `0x04` |
| 3  | NET  | `0x08` |

When more pages are enabled than there are LCDs, the extra pages all land on
the last LCD and rotate there; otherwise each LCD gets one page.

## Build & flash (PlatformIO)

Environments are defined in [platformio.ini](platformio.ini). Default is the
168P.

```sh
# 168P (default) — build + upload
pio run -t upload

# 328P (old bootloader)
pio run -e nanoatmega328 -t upload

# 328P (new / optiboot bootloader)
pio run -e nanoatmega328new -t upload

# serial monitor (115200 baud)
pio device monitor
```

### Memory footprint

The 168P is tight (1 KB SRAM, 14 KB usable flash). Current usage:

| Target | Flash | SRAM (static) |
|--------|-------|---------------|
| ATmega168P | ~10.9 KB / 14 KB (76%) | ~794 B / 1024 B (78%) |
| ATmega328P | ~10.9 KB / 30 KB (35%) | ~794 B / 2048 B (38%) |

The ~230 B of free RAM on the 168P is shared between the heap (4 LCD objects,
~70 B) and the stack. If you run 4 displays and hit instability, lower
`MAX_LCDS` or move those units to a 328P.

## Serial protocol

115200 baud, newline-terminated lines.

### Host → device

A stats line is a `KEY=value;KEY=value;…` list. All fields are optional except
`CPU` (its presence marks the line as a valid data line); unknown/trailing
fields are ignored.

| Key | Meaning | Units |
|------|---------|-------|
| `CPU` `RAM` `GPU` | usage | per-mille (0–1000; shown as `value/10` %) |
| `DL` `UL` | network down/up | KB/s |
| `RAMU` `RAMT` | RAM used / total | MB |
| `VRAM` `VRAMT` | VRAM used / total | MB (`VRAMT` ≤ 0 → unknown) |
| `TEMP` | CPU temperature | °C × 10 (e.g. `525` = 52.5°C; `<0` = unknown) |
| `GPUT` | GPU temperature | °C × 10 |
| `LSPD` | link speed (scales NET bars) | Mbps |
| `PDLY` | page rotation interval | ms (clamped 500–60000) |
| `PG` | enabled-pages bitmask | see Pages table |
| `PGORD` | explicit page order | e.g. `PGORD=0,2,1,3` (overrides `PG`) |

Example:

```
CPU=453;RAM=602;GPU=900;DL=1240;UL=256;RAMU=9830;RAMT=16384;VRAM=7680;VRAMT=8192;TEMP=525;GPUT=610;LSPD=1000;PG=15;PDLY=3000
```

The special command `IDENT` makes each LCD briefly show its screen number and
I²C address (useful for figuring out which physical screen is which).

### Device → host

`BOOT` on start, `FOUND_LCD=0x<addr>` per detected display, `READY` once
initialised, then `OK` (line accepted) / `ERR` (no valid data) per line, and
`OK IDENT` after an `IDENT`.

## Repository layout

```
platformio.ini   PlatformIO project / build environments
src/main.cpp      firmware
```
