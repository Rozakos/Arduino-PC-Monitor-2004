# AGENTS.md

Guidance for AI agents working on this repo. Read this before editing
`src/main.cpp`.

## What this is

PlatformIO firmware for an Arduino Nano driving up to 4× **20×4 I²C LCDs** as a
PC stats monitor. A host-side Python GUI streams stats over USB serial; the
firmware parses them and renders them. Single translation unit:
[`src/main.cpp`](src/main.cpp).

## Build / verify

```sh
pio run -e nanoatmega168          # default target, MUST keep fitting (see limits)
pio run -e nanoatmega328          # 328P, old bootloader
pio run -e nanoatmega328new       # 328P, optiboot
pio run -e nanoatmega168 -t upload
pio device monitor                # 115200 baud
```

On Windows the CLI lives at `%USERPROFILE%\.platformio\penv\Scripts\pio.exe`
(not on PATH). **Always build `nanoatmega168` after any change** and check the
size report — this is the binding constraint.

## Hard constraints (ATmega168P = default target)

- **14336 B flash, 1024 B SRAM.** Build fails the link if either is exceeded.
- Current usage ≈ 11.3 KB flash / **746 B** static RAM. The remaining RAM is
  shared by the heap (`new` LCD objects) and the stack, so **static RAM is the
  scarce resource** — be very conservative adding globals/buffers.
- **The link succeeding is NOT enough.** This firmware once linked fine at
  **794 B** static but **hung during `LiquidCrystal_I2C::init()`** (boot reached
  `BOOT` but never `FOUND_LCD`/`READY`) — a runtime **stack/heap collision**:
  with an LCD present, `init()`'s call stack peaked and overran the heap. The
  fix was freeing ~48 B by shrinking the serial **TX** ring to 16 via
  `build_flags = -DSERIAL_TX_BUFFER_SIZE=16` in `platformio.ini` (`[env]`).
  **Symptom to recognise:** boots clean with the LCD *unplugged* (reaches
  `READY`) but hangs after `BOOT` with it plugged in → you're out of RAM, not
  broken on the bus. Keep a healthy static-RAM margin (target ≤ ~760 B).

### Rules that keep it fitting — do not casually break these

1. **No `printf` family.** No `sprintf`/`snprintf`/`sscanf`. On AVR their format
   strings live in `.data` (RAM), and the code is large. Earlier this firmware
   used them and overflowed the 168P by ~2.2 KB flash / ~214 B RAM. Use the
   hand-rolled helpers instead:
   - Parsing: `findField()` + `getLong()`.
   - Formatting: `uAppend()`, `uAppendW()`, and the `fmt*` functions.
2. **String literals cost RAM.** A plain `"..."` literal is copied to `.data`.
   For serial output use the `F()` macro (keeps it in flash). For building LCD
   text, prefer the integer helpers; short labels are assembled char-by-char in
   `renderBlock()` precisely to avoid `.data` literals.
3. **Parse the serial line in place.** Do not copy `rxLine` into a scratch
   buffer. The parser tolerates unknown/trailing fields, so there is no
   tail-stripping step.
4. **Avoid floats** entirely (no FP printf, no `float` math). Fixed-point only
   (temps are ×10 integers, usage is per-mille).
5. Keep debug serial output minimal — only the protocol tokens below.

If you genuinely need more room, the cheapest lever is lowering `MAX_LCDS`.

## Architecture map (`src/main.cpp`, top to bottom)

- **Config/defines:** `COLS=20`, `ROWS=4`, page IDs `PG_CPU/GPU/RAM/NET`,
  custom-char slots, `MAX_LCDS=4`, `MAX_PAGES=4`.
- **Globals:** LCD registry (`lcds[]`, `lcdAddrs[]`, `numLcds`), page assignment
  (`lcdPages`, `lcdPageCount`, `lcdViewIndex`, `enabledPages`, `pageMask`,
  `pageInterval`), the stats variables, and the `rxLine[140]` serial buffer.
- `scanAndInitLcds()` — I²C probe, `new LiquidCrystal_I2C`, register custom
  chars, print `FOUND_LCD=`.
- **Page distribution:** `distributePages()`, `rebuildEnabledPages(mask)`,
  `applyPageOrder(str)`.
- **Parser:** `findField()` (boundary-aware key match), `getLong()`.
- **Rendering:** `drawSmoothBar()` (segmented bar — name is legacy, see below),
  the `fmt*` formatters, `composeRow()`, `renderBlock()` (one metric = 2 rows),
  `showView()` (a pair of blocks), `showWaiting()`, `refreshAll()`,
  `rotatePages()`.
- `setup()` / `loop()` — heartbeat LED, page rotation timer, serial line
  assembly, then `IDENT` handling and the data-line parse.

## Key behaviors / gotchas

- **Custom character slots:** `1–6` = segmented bar cells (left/mid/right ×
  full/empty), `7` = degree symbol (`#define DEG 7`). **Slot 0 is deliberately
  unused.** 8 slots exist (0–7); don't exceed. (Slot 0 was suspected during the
  boot-hang debugging and avoided; the real cause turned out to be RAM, but the
  1..7 layout was kept.)
- **I²C robustness:** `i2cBusRecover()` runs before `Wire.begin()` to clock out
  a slave stuck holding SDA; `Wire.setWireTimeout(25000, true)` ensures a wedged
  bus can never hard-hang the scan. Keep both.
- **`findField` is boundary-aware:** a key matches only at string start or right
  after `;`, and must be followed by `=`. This is load-bearing — it stops `RAM`
  matching inside `VRAM`/`RAMU`/`RAMT`, and `PG` matching inside `PGORD`. Keep
  this logic if you touch the parser.
- **Layout = "hybrid":** each screen shows two metric blocks; each block is a
  label row + a full-width bar row. LCDs rotate through *pairs* of their
  assigned pages (`showView` steps `lcdViewIndex`).
- **NET block** is special: one label (`DL`/`UL`) plus a split bar row (DL =
  left half, UL = right half), scaled by `LSPD` via `kbToPermille()`.
- **GPU label** logic: decimal GPU temp → `usage% + decimal temp`; else
  `usage% + integer temp` with VRAM ratio centered when it fits.
- **Units:** usage `CPU/RAM/GPU` is per-mille (÷10 → %); temps `TEMP/GPUT` are
  °C×10 with `<0` meaning unknown; mem/VRAM in MB; `DL/UL` in KB/s.
- `drawSmoothBar()` keeps its name from the smooth-bar era but now renders the
  segmented rounded-end style. Rename only if you also update call sites.

## Serial protocol (authoritative summary)

Host→device data line: `CPU=…;RAM=…;GPU=…;DL=…;UL=…;RAMU=…;RAMT=…;VRAM=…;VRAMT=…;TEMP=…;GPUT=…;LSPD=…;PG=…;PDLY=…;PGORD=0,2,1,3`.
All optional except `CPU` (gate). `PGORD` overrides `PG`. `PDLY` clamped
500–60000 ms. Command `IDENT` shows each screen's number + I²C address.

Device→host tokens: `BOOT`, `FOUND_LCD=0x<addr>`, `READY`, `OK`, `ERR`,
`OK IDENT`. Treat these as a stable contract with the host GUI — don't rename.

See [README.md](README.md) for the full field table.

## History (why the code looks the way it does)

1. Ported from a 1602 (16×2) firmware to 2004 (20×4).
2. Layout reworked to the hybrid two-metric-per-screen design.
3. Bars were briefly smooth sub-character bars, then reverted to the original
   segmented rounded-end style (per user preference) — hence the legacy
   `drawSmoothBar` name.
4. Put on a "diet" to fit the ATmega168P: removed `printf`/`scanf`, hand-rolled
   parse + format, in-place line parsing, stripped debug output.
5. Segmented bars (7 custom chars) then linked fine but **hung at boot on the
   168P** — a runtime stack/heap collision at ~794 B static RAM, not a bar bug.
   Diagnosed by: clean boot with LCD unplugged vs. hang with it plugged in.
   Fixed by adding `i2cBusRecover()` + `Wire.setWireTimeout()` (bus safety) and,
   the actual fix, `-DSERIAL_TX_BUFFER_SIZE=16` to free ~48 B (→ 746 B static).
