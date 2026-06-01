#include <Arduino.h>

/*
 * PC Monitor -- Arduino Nano (ATmega168P / 328P, 2004-only)
 * Multi-LCD 2004 I2C support, runtime-configured via Python GUI.
 * Built with PlatformIO. Default env: nanoatmega168 (1 KB SRAM, 14 KB flash).
 *
 * Wiring:  A4=SDA  A5=SCL  5V=VCC  GND=GND
 * Set unique I2C addresses via A0/A1/A2 jumpers on each backpack.
 *
 * LAYOUT (20x4, "hybrid"):
 *   Each screen shows TWO metrics stacked, each as a 2-row block:
 *     row N   : text label  (usage / temp / mem / speed)
 *     row N+1 : full-width smooth bar
 *   An LCD groups its assigned pages into consecutive pairs and
 *   rotates through the pairs. Odd page out -> bottom block cleared.
 *
 * GRAPHICS:
 *   Smooth sub-character bars. 4 custom chars give 1/5..4/5 partial
 *   cells, the built-in solid block (0xFF) is a full cell, space is
 *   empty. Bars therefore fill column-by-column at 1/5-cell res.
 *
 * 168P DIET: no printf/scanf (hand-rolled parse + format), no debug
 *   output, line parsed in place. Keeps flash + RAM within 168P limits.
 *
 * GPU label logic:
 *   IF temp has decimals -> Usage% + decimal temp (e.g. 50.5C)
 *   ELSE                 -> Usage% + integer temp (e.g. 50C)
 *   VRAM ratio is shown in the middle of the label row when it fits.
 */

#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── Limits ────────────────────────────────────────────────────────────────────
#define MAX_LCDS   4
#define MAX_PAGES  4

// ── Display size ──────────────────────────────────────────────────────────────
#define COLS       20
#define ROWS        4

// ── Page IDs ──────────────────────────────────────────────────────────────────
#define PG_CPU  0
#define PG_GPU  1
#define PG_RAM  2
#define PG_NET  3

// ── Custom char slots ───────────────────────────────────────────────────────--
//   Slots 1..6 = segmented bar cells (L/M/R, full/empty).  Slot 7 = degree.
//   (Slot 0 deliberately unused — registering a char on slot 0 during init
//    wedged the LCD and hung the boot; slots 1..7 are safe.)
#define DEG     7

// ── Defaults ──────────────────────────────────────────────────────────────────
#define BAUD                 115200
#define LED_PIN              LED_BUILTIN
#define DEFAULT_PAGE_INTERVAL 3000
#define DEFAULT_PAGE_MASK     0x0F

// ── Bar widths ────────────────────────────────────────────────────────────────
#define BAR_FULL   COLS
#define BAR_HALF  (COLS / 2)


// ════════════════════════════════════════════════════════════════════════════
//  LCD REGISTRY
// ════════════════════════════════════════════════════════════════════════════
LiquidCrystal_I2C* lcds[MAX_LCDS];
uint8_t  lcdAddrs[MAX_LCDS];
uint8_t  numLcds = 0;


// ════════════════════════════════════════════════════════════════════════════
//  PAGE ASSIGNMENT
// ════════════════════════════════════════════════════════════════════════════
uint8_t  lcdPages[MAX_LCDS][MAX_PAGES];
uint8_t  lcdPageCount[MAX_LCDS];
uint8_t  lcdViewIndex[MAX_LCDS];        // which pair (view) is showing
uint8_t  enabledPages[MAX_PAGES];
uint8_t  numEnabled   = 0;
uint8_t  pageMask     = DEFAULT_PAGE_MASK;
uint32_t pageInterval = DEFAULT_PAGE_INTERVAL;


// ════════════════════════════════════════════════════════════════════════════
//  SERIAL BUFFER
// ════════════════════════════════════════════════════════════════════════════
char     rxLine[140];
uint8_t  rxLen     = 0;
bool     lineReady = false;


// ════════════════════════════════════════════════════════════════════════════
//  STATS — ramMB/ramTotalMB are unsigned int (0-65535 → up to 64 GB)
// ════════════════════════════════════════════════════════════════════════════
int  cpu10  = 0, ram10  = 0, gpu10  = 0;
int  dlKB   = 0, ulKB   = 0;
unsigned int ramMB = 0, ramTotalMB = 0;
int  vramMB = 0;
int  vramTotalMB = -1;
int  temp10 = -1, gput10 = -1;
int  linkSpeedMbps = 1000;
bool gotData = false;


// ════════════════════════════════════════════════════════════════════════════
//  TIMING
// ════════════════════════════════════════════════════════════════════════════
uint32_t lastPageFlip = 0;
uint32_t lastHB       = 0;
bool     ledState     = false;


// ════════════════════════════════════════════════════════════════════════════
//  I2C SCAN & LCD INIT
// ════════════════════════════════════════════════════════════════════════════

// Free a wedged I2C bus: if a slave is holding SDA low, clock SCL up to 9
// times to let it finish its byte, then issue a STOP. Run BEFORE Wire.begin().
void i2cBusRecover() {
  pinMode(SDA, INPUT_PULLUP);
  pinMode(SCL, INPUT_PULLUP);
  delayMicroseconds(10);
  if (digitalRead(SDA) == HIGH) return;          // bus already free

  for (uint8_t i = 0; i < 9 && digitalRead(SDA) == LOW; i++) {
    pinMode(SCL, OUTPUT); digitalWrite(SCL, LOW); delayMicroseconds(5);
    pinMode(SCL, INPUT_PULLUP);                   delayMicroseconds(5);
  }
  // STOP condition: SDA low -> high while SCL high
  pinMode(SDA, OUTPUT); digitalWrite(SDA, LOW); delayMicroseconds(5);
  pinMode(SCL, INPUT_PULLUP);                   delayMicroseconds(5);
  pinMode(SDA, INPUT_PULLUP);                   delayMicroseconds(5);
}

bool probeAddress(uint8_t addr) {
  Wire.beginTransmission(addr);
  return (Wire.endTransmission() == 0);
}

void scanAndInitLcds() {
  // Segmented bar cells: left/mid/right, full/empty (rounded ends).
  byte cLF[8] = { 0b01111,0b11111,0b11111,0b11111,0b11111,0b11111,0b01111,0b00000 };
  byte cLE[8] = { 0b01111,0b10000,0b10000,0b10000,0b10000,0b10000,0b01111,0b00000 };
  byte cMF[8] = { 0b11110,0b11110,0b11110,0b11110,0b11110,0b11110,0b11110,0b00000 };
  byte cME[8] = { 0b11110,0b00000,0b00000,0b00000,0b00000,0b00000,0b11110,0b00000 };
  byte cRF[8] = { 0b11110,0b11111,0b11111,0b11111,0b11111,0b11111,0b11110,0b00000 };
  byte cRE[8] = { 0b11110,0b00001,0b00001,0b00001,0b00001,0b00001,0b11110,0b00000 };

  // Degree symbol (slot 7)
  byte cDegree[8] = {
    0b00110,0b01001,0b01001,0b00110,0b00000,0b00000,0b00000,0b00000
  };

  const uint8_t cand[] = {
    0x27,0x26,0x25,0x24,0x23,0x22,0x21,0x20,
    0x3F,0x3E,0x3D,0x3C,0x3B,0x3A,0x39,0x38
  };

  numLcds = 0;
  for (uint8_t i = 0; i < 16 && numLcds < MAX_LCDS; i++) {
    uint8_t addr = cand[i];
    if (!probeAddress(addr)) continue;

    lcdAddrs[numLcds] = addr;
    lcds[numLcds] = new LiquidCrystal_I2C(addr, COLS, ROWS);
    if (!lcds[numLcds]) continue;

    lcds[numLcds]->init();
    lcds[numLcds]->backlight();

    lcds[numLcds]->createChar(1, cLF);
    lcds[numLcds]->createChar(2, cLE);
    lcds[numLcds]->createChar(3, cMF);
    lcds[numLcds]->createChar(4, cME);
    lcds[numLcds]->createChar(5, cRF);
    lcds[numLcds]->createChar(6, cRE);
    lcds[numLcds]->createChar(DEG, cDegree);
    lcds[numLcds]->clear();

    Serial.print(F("FOUND_LCD=0x"));
    Serial.println(addr, HEX);
    numLcds++;
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  PAGE DISTRIBUTION
// ════════════════════════════════════════════════════════════════════════════

void distributePages() {
  for (uint8_t i = 0; i < MAX_LCDS; i++) { lcdPageCount[i] = 0; lcdViewIndex[i] = 0; }
  if (numLcds == 0 || numEnabled == 0) return;

  if (numEnabled <= numLcds) {
    for (uint8_t i = 0; i < numEnabled; i++) {
      lcdPages[i][0] = enabledPages[i]; lcdPageCount[i] = 1;
    }
  } else {
    uint8_t fixed = numLcds - 1;
    for (uint8_t i = 0; i < fixed; i++) {
      lcdPages[i][0] = enabledPages[i]; lcdPageCount[i] = 1;
    }
    uint8_t lastIdx = numLcds - 1;
    uint8_t rem     = numEnabled - fixed;
    for (uint8_t i = 0; i < rem; i++) lcdPages[lastIdx][i] = enabledPages[fixed + i];
    lcdPageCount[lastIdx] = rem;
  }
}

void rebuildEnabledPages(uint8_t mask) {
  if (mask == 0) mask = 1;
  pageMask = mask; numEnabled = 0;
  for (uint8_t i = 0; i < MAX_PAGES; i++)
    if (mask & (1 << i)) enabledPages[numEnabled++] = i;
  distributePages();
}

void applyPageOrder(const char* p) {
  numEnabled = 0;
  uint8_t newMask = 0;
  while (*p && numEnabled < MAX_PAGES) {
    while (*p && (*p < '0' || *p > '9')) {
      if (*p == ';') { p = ""; break; }   // stop at field separator
      p++;
    }
    if (!*p) break;
    uint8_t id = 0;
    while (*p >= '0' && *p <= '9') id = id * 10 + (*p++ - '0');
    if (*p == ',') p++;
    if (id < MAX_PAGES) {
      enabledPages[numEnabled++] = id;
      newMask |= (1 << id);
    }
  }
  if (numEnabled == 0) { enabledPages[0] = 0; numEnabled = 1; newMask = 1; }
  pageMask = newMask;
  distributePages();
}


// ════════════════════════════════════════════════════════════════════════════
//  KEY=VALUE PARSER (in place, boundary-aware)
//    Matches KEY only when preceded by start-of-string or ';' and
//    followed by '='. So "RAM" won't match inside "VRAM"/"RAMU"/"RAMT",
//    and "PG" won't match inside "PGORD".
// ════════════════════════════════════════════════════════════════════════════

const char* findField(const char* s, const char* key) {
  uint8_t kl = strlen(key);
  const char* p = s;
  while ((p = strstr(p, key)) != NULL) {
    bool boundary = (p == s) || (*(p - 1) == ';');
    if (boundary && p[kl] == '=') return p + kl + 1;
    p++;
  }
  return NULL;
}

long getLong(const char* s, const char* key, long def) {
  const char* p = findField(s, key);
  if (!p) return def;
  bool neg = false;
  if (*p == '-') { neg = true; p++; }
  if (*p < '0' || *p > '9') return def;
  long v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10 + (*p++ - '0');
  return neg ? -v : v;
}


// ════════════════════════════════════════════════════════════════════════════
//  BAR DRAWING — segmented cells with rounded ends (slots 1..6, slot 0 unused)
// ════════════════════════════════════════════════════════════════════════════

void drawSmoothBar(LiquidCrystal_I2C* lcd, uint8_t row, uint8_t col,
                   uint8_t width, int val10) {
  // val10 is permille (0..1000). Each cell is full or empty.
  int filled = constrain((int)((long)val10 * width / 1000), 0, (int)width);
  lcd->setCursor(col, row);
  for (int i = 0; i < width; i++) {
    bool f = (i < filled);
    if      (i == 0)         lcd->write(byte(f ? 1 : 2));   // left end
    else if (i == width - 1) lcd->write(byte(f ? 5 : 6));   // right end
    else                     lcd->write(byte(f ? 3 : 4));   // middle
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  HAND-ROLLED FORMATTERS (no printf)
// ════════════════════════════════════════════════════════════════════════════

// Append unsigned value, return new end pointer.
char* uAppend(char* p, unsigned long v) {
  char tmp[11]; uint8_t n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
  while (n) *p++ = tmp[--n];
  return p;
}

// Append unsigned value right-justified to at least `width` (space padded).
char* uAppendW(char* p, unsigned long v, uint8_t width) {
  char tmp[11]; uint8_t n = 0;
  if (v == 0) tmp[n++] = '0';
  while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
  for (uint8_t i = n; i < width; i++) *p++ = ' ';
  while (n) *p++ = tmp[--n];
  return p;
}

// "  0%" .. "100%"  (4 chars)
void fmtPct4(char* b, int v) {
  int pct = v / 10;
  if (pct < 0) pct = 0;
  if (pct > 999) pct = 999;
  char* p = uAppendW(b, (unsigned long)pct, 3);
  *p++ = '%'; *p = '\0';
}

// Decimal temp (e.g. "50.5°C")
void fmtTempDec(char* b, int t) {
  char* p = b;
  if (t < 0) { *p++=' '; *p++='-'; *p++='-'; *p++='.'; *p++='-'; *p='\0'; return; }
  p = uAppendW(p, (unsigned long)(t / 10), 2);
  *p++ = '.';
  *p++ = '0' + (t % 10);
  *p++ = DEG; *p++ = 'C'; *p = '\0';
}

// Integer temp (e.g. "50°C")
void fmtTempInt(char* b, int t) {
  char* p = b;
  if (t < 0) { *p++=' '; *p++='-'; *p++='-'; *p++=' '; *p='\0'; return; }
  p = uAppendW(p, (unsigned long)(t / 10), 2);
  *p++ = DEG; *p++ = 'C'; *p = '\0';
}

void fmtMemRatio(char* b, unsigned int used, unsigned int total) {
  char* p = b;
  if (total >= 1024) {
    p = uAppend(p, used / 1024);
    *p++ = '.';
    p = uAppend(p, (unsigned long)((used % 1024) * 10 / 1024));
    *p++ = '/';
    p = uAppend(p, total / 1024);
    *p++ = 'G';
  } else {
    p = uAppend(p, used);
    *p++ = '/';
    p = uAppend(p, total);
    *p++ = 'M';
  }
  *p = '\0';
}

void fmtVramRatio(char* b, int used, int total) {
  char* p = b;
  int ug = used / 1024;
  int tg = total / 1024;
  if (tg >= 10) {
    p = uAppend(p, (unsigned long)ug); *p++ = '/';
    p = uAppend(p, (unsigned long)tg); *p++ = 'G';
  } else if (tg > 0) {
    p = uAppend(p, (unsigned long)ug); *p++ = '.';
    p = uAppend(p, (unsigned long)((used % 1024) * 10 / 1024)); *p++ = '/';
    p = uAppend(p, (unsigned long)tg); *p++ = 'G';
  } else {
    p = uAppend(p, (unsigned long)used); *p++ = '/';
    p = uAppend(p, (unsigned long)total); *p++ = 'M';
  }
  *p = '\0';
}

// Network speed, ~5 chars: "1234K" / " 1.2M" / "1234M"
void fmtSpeed5(char* b, int kb) {
  char* p = b;
  if (kb < 0) kb = 0;
  if (kb >= 10000) {
    p = uAppendW(p, (unsigned long)(kb / 1024), 4); *p++ = 'M';
  } else if (kb >= 1000) {
    int m = (int)((long)kb * 10 / 1024);
    p = uAppend(p, (unsigned long)(m / 10)); *p++ = '.';
    *p++ = '0' + (m % 10); *p++ = 'M';
  } else {
    p = uAppendW(p, (unsigned long)kb, 4); *p++ = 'K';
  }
  *p = '\0';
}

int kbToPermille(int kb) {
  if (linkSpeedMbps <= 0) return 0;
  return (int)constrain((long)kb*1000L/((long)linkSpeedMbps*125L), 0L, 1000L);
}

// Left-align `left`, right-align `right`, optionally center `mid` in the gap
// (dropped if it doesn't fit). Result is exactly `cols` chars + NUL.
void composeRow(char* row, const char* left, const char* mid,
                const char* right, uint8_t cols) {
  for (uint8_t i = 0; i < cols; i++) row[i] = ' ';
  row[cols] = '\0';

  uint8_t ll = strlen(left), rl = strlen(right), ml = strlen(mid);

  for (uint8_t i = 0; i < ll && i < cols; i++) row[i] = left[i];
  if (rl <= cols)
    for (uint8_t i = 0; i < rl; i++) row[cols - rl + i] = right[i];

  if (ml > 0) {
    uint8_t gapStart = ll + 1;                                 // 1 space after left
    uint8_t gapEnd   = (rl + 1 <= cols) ? cols - rl - 1 : 0;   // 1 space before right
    if (gapStart < gapEnd && ml <= (gapEnd - gapStart)) {
      uint8_t mstart = gapStart + ((gapEnd - gapStart) - ml) / 2;
      for (uint8_t i = 0; i < ml; i++) row[mstart + i] = mid[i];
    }
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  BLOCK RENDERER — one metric = label row (top) + bar row (top+1)
// ════════════════════════════════════════════════════════════════════════════

void clearRows(LiquidCrystal_I2C* lcd, uint8_t startRow, uint8_t count) {
  for (uint8_t r = 0; r < count; r++) {
    lcd->setCursor(0, startRow + r);
    for (uint8_t c = 0; c < COLS; c++) lcd->write(' ');
  }
}

void renderBlock(LiquidCrystal_I2C* lcd, uint8_t top, uint8_t pageId) {
  char row[COLS + 1];

  switch (pageId) {

    case PG_CPU: {
      char tmp[10], left[10];
      char* p = left; *p++='C'; *p++='P'; *p++='U'; *p++=' '; *p='\0';
      fmtPct4(left + 4, cpu10);
      fmtTempDec(tmp, temp10);
      composeRow(row, left, "", tmp, COLS);
      lcd->setCursor(0, top); lcd->print(row);
      drawSmoothBar(lcd, top + 1, 0, BAR_FULL, cpu10);
      break;
    }

    case PG_GPU: {
      char tmp[10], vram[12], left[10];
      bool hasDecimal = (gput10 >= 0) && (gput10 % 10 != 0);

      left[0]='G'; left[1]='P'; left[2]='U'; left[3]=' ';
      fmtPct4(left + 4, gpu10);

      if (hasDecimal) fmtTempDec(tmp, gput10);
      else            fmtTempInt(tmp, gput10);

      if (vramTotalMB > 0) fmtVramRatio(vram, vramMB, vramTotalMB);
      else                 vram[0] = '\0';

      composeRow(row, left, vram, tmp, COLS);
      lcd->setCursor(0, top); lcd->print(row);
      drawSmoothBar(lcd, top + 1, 0, BAR_FULL, gpu10);
      break;
    }

    case PG_RAM: {
      char mem[16], left[10];
      left[0]='R'; left[1]='A'; left[2]='M'; left[3]=' ';
      fmtPct4(left + 4, ram10);
      fmtMemRatio(mem, ramMB, ramTotalMB);
      composeRow(row, left, "", mem, COLS);
      lcd->setCursor(0, top); lcd->print(row);
      drawSmoothBar(lcd, top + 1, 0, BAR_FULL, ram10);
      break;
    }

    case PG_NET: {
      char left[8], right[8];
      left[0]='D'; left[1]='L';  fmtSpeed5(left + 2, dlKB);
      right[0]='U'; right[1]='L'; fmtSpeed5(right + 2, ulKB);
      composeRow(row, left, "", right, COLS);
      lcd->setCursor(0, top); lcd->print(row);
      drawSmoothBar(lcd, top + 1, 0,        BAR_HALF, kbToPermille(dlKB));
      drawSmoothBar(lcd, top + 1, BAR_HALF, BAR_HALF, kbToPermille(ulKB));
      break;
    }
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  VIEW (pair of blocks) RENDERER
// ════════════════════════════════════════════════════════════════════════════

void showView(uint8_t lcdIdx) {
  LiquidCrystal_I2C* lcd = lcds[lcdIdx];
  uint8_t cnt = lcdPageCount[lcdIdx];
  if (!cnt) return;

  uint8_t views  = (cnt + 1) / 2;
  uint8_t v      = lcdViewIndex[lcdIdx] % views;
  uint8_t topIdx = v * 2;

  renderBlock(lcd, 0, lcdPages[lcdIdx][topIdx]);
  if (topIdx + 1 < cnt) renderBlock(lcd, 2, lcdPages[lcdIdx][topIdx + 1]);
  else                  clearRows(lcd, 2, 2);
}


// ════════════════════════════════════════════════════════════════════════════
//  WAITING SCREEN
// ════════════════════════════════════════════════════════════════════════════

void showWaiting(LiquidCrystal_I2C* lcd) {
  lcd->setCursor(0,0); lcd->print(F("                    "));
  lcd->setCursor(0,1); lcd->print(F("   Waiting for PC   "));
  lcd->setCursor(0,2); lcd->print(F("        ...         "));
  lcd->setCursor(0,3); lcd->print(F("                    "));
}


// ════════════════════════════════════════════════════════════════════════════
//  REFRESH & ROTATE
// ════════════════════════════════════════════════════════════════════════════

void refreshAll() {
  for (uint8_t i = 0; i < numLcds; i++) {
    if (!lcdPageCount[i]) continue;
    if (!gotData) showWaiting(lcds[i]);
    else          showView(i);
  }
}

void rotatePages() {
  for (uint8_t i = 0; i < numLcds; i++) {
    uint8_t views = (lcdPageCount[i] + 1) / 2;
    if (views < 2) continue;
    lcdViewIndex[i] = (lcdViewIndex[i] + 1) % views;
    if (gotData) showView(i);
  }
}


// ════════════════════════════════════════════════════════════════════════════
//  SETUP
// ════════════════════════════════════════════════════════════════════════════

void setup() {
  pinMode(LED_PIN, OUTPUT);
  Serial.begin(BAUD);
  Serial.println(F("BOOT"));
  i2cBusRecover();                  // unstick a wedged bus before using Wire
  Wire.begin();
  Wire.setWireTimeout(25000, true); // 25 ms timeout, reset TWI on timeout — never hard-hang
  scanAndInitLcds();
  rebuildEnabledPages(DEFAULT_PAGE_MASK);

  for (uint8_t i = 0; i < numLcds; i++)
    if (lcdPageCount[i]) showWaiting(lcds[i]);

  Serial.println(F("READY"));
  lastPageFlip = millis();
}


// ════════════════════════════════════════════════════════════════════════════
//  LOOP
// ════════════════════════════════════════════════════════════════════════════

void loop() {
  uint32_t now = millis();

  if (now - lastHB >= 500) {
    lastHB = now; ledState = !ledState;
    digitalWrite(LED_PIN, ledState);
  }

  if (gotData && (now - lastPageFlip >= pageInterval)) {
    lastPageFlip = now; rotatePages();
  }

  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') {
      if (rxLen > 0 && rxLine[rxLen-1] == '\r') rxLen--;
      rxLine[rxLen] = '\0'; lineReady = true; break;
    } else {
      if (rxLen < sizeof(rxLine)-1) rxLine[rxLen++] = c;
      else rxLen = 0;
    }
  }

  if (!lineReady) return;
  lineReady = false;

  // ── IDENT command ──────────────────────────────────────────────────────
  if (strncmp(rxLine, "IDENT", 5) == 0) {
    char buf[21];
    for (uint8_t i = 0; i < numLcds; i++) {
      lcds[i]->clear();
      // "     Screen N"
      char* p = buf;
      for (uint8_t s = 0; s < 5; s++) *p++ = ' ';
      *p++='S'; *p++='c'; *p++='r'; *p++='e'; *p++='e'; *p++='n'; *p++=' ';
      p = uAppend(p, (unsigned long)(i + 1)); *p = '\0';
      lcds[i]->setCursor(0, 1); lcds[i]->print(buf);
      // "     Addr 0xHH"
      p = buf;
      for (uint8_t s = 0; s < 5; s++) *p++ = ' ';
      *p++='A'; *p++='d'; *p++='d'; *p++='r'; *p++=' '; *p++='0'; *p++='x';
      uint8_t hi = lcdAddrs[i] >> 4, lo = lcdAddrs[i] & 0x0F;
      *p++ = hi < 10 ? '0' + hi : 'A' + hi - 10;
      *p++ = lo < 10 ? '0' + lo : 'A' + lo - 10;
      *p = '\0';
      lcds[i]->setCursor(0, 2); lcds[i]->print(buf);
    }
    delay(3000);
    gotData = false;
    rxLen = 0; lineReady = false;
    Serial.println(F("OK IDENT"));
    return;
  }

  // ── Data line: parsed in place, key=value, trailing fields ignored ──────
  if (findField(rxLine, "CPU")) {
    cpu10       = (int)getLong(rxLine, "CPU",  0);
    ram10       = (int)getLong(rxLine, "RAM",  0);
    gpu10       = (int)getLong(rxLine, "GPU",  0);
    dlKB        = (int)getLong(rxLine, "DL",   0);
    ulKB        = (int)getLong(rxLine, "UL",   0);
    ramMB       = (unsigned int)getLong(rxLine, "RAMU",  0);
    ramTotalMB  = (unsigned int)getLong(rxLine, "RAMT",  0);
    vramMB      = (int)getLong(rxLine, "VRAM",  0);
    vramTotalMB = (int)getLong(rxLine, "VRAMT", -1);
    temp10      = (int)getLong(rxLine, "TEMP", -1);
    gput10      = (int)getLong(rxLine, "GPUT", -1);

    long ls = getLong(rxLine, "LSPD", 0);
    if (ls > 0) linkSpeedMbps = (int)ls;

    long pd = getLong(rxLine, "PDLY", -1);
    if (pd >= 500 && pd <= 60000) pageInterval = (uint32_t)pd;

    const char* pgord = findField(rxLine, "PGORD");
    if (pgord) {
      applyPageOrder(pgord);
    } else {
      long pg = getLong(rxLine, "PG", -1);
      if (pg >= 0 && (uint8_t)pg != pageMask) rebuildEnabledPages((uint8_t)pg);
    }

    gotData = true;
    refreshAll();
    Serial.println(F("OK"));
  } else {
    Serial.println(F("ERR"));
  }

  rxLen = 0;
}
