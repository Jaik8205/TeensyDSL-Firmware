/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║           TeensyDSL v6 — Stability + Abstraction                ║
 * ║     Tokenizer · Dispatch Table · Validator · Rate Limiter       ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  ARDUINO IDE:                                                    ║
 * ║  Board    → Teensy 4.1 (recommended) or 4.0                     ║
 * ║  USB Type → "Serial + Keyboard + Mouse + Joystick"              ║
 * ║  Libraries: SD, SPI, Keyboard, Wire  (Teensy built-in)          ║
 * ║             Adafruit_SSD1306 + GFX  (Library Manager)           ║
 * ║             Watchdog_t4             (Library Manager, optional) ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  WIRING:                                                         ║
 * ║  OLED SDA/SCL  → 18/19   SD CS → 10   LED → 13                ║
 * ║  BTN_UP        → pin 3 → GND                                   ║
 * ║  BTN_DOWN      → pin 4 → GND                                   ║
 * ║  BTN_SELECT    → pin 5 → GND                                   ║
 * ║  BTN_BACK      → pin 6 → GND                                   ║
 * ║  BTN_CHECKPOINT→ pin 7 → GND                                   ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  SD CARD LAYOUT:                                                 ║
 * ║  /windows/  /mac/  /ubuntu/  /gnome/  (auto-created at boot)   ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  v6 ARCHITECTURAL CHANGES:                                       ║
 * ║  1. Tokenizer   — single-pass, quoted-string-aware, typed       ║
 * ║  2. Dispatch table — array of {name, minArgs, handler_fn}       ║
 * ║                   replaces the 40-branch if-chain               ║
 * ║  3. Script validator — runs before execution, shows result on   ║
 * ║                   OLED, blocks run on syntax errors             ║
 * ║  4. Overflow guards — GUARD_BOUNDS macro on every array write   ║
 * ║  5. HID rate limiter — min 2ms between reports, burst cap 50/s  ║
 * ║  6. Hardware watchdog — chip resets if software hangs entirely  ║
 * ║  7. Error tiers — parse errors warn+skip, fatal errors halt     ║
 * ╚══════════════════════════════════════════════════════════════════╝
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// Optional hardware watchdog — requires Watchdog_t4 library
#define ENABLE_WATCHDOG  1
#if ENABLE_WATCHDOG
  #include <Watchdog_t4.h>
  WDT_T4<WDT1> hwWatchdog;
#endif

// ─────────────────────────────────────────────────────────────────────
// ░░  PIN CONFIGURATION  ░░
// ─────────────────────────────────────────────────────────────────────

#define PIN_BTN_UP          3
#define PIN_BTN_DOWN        4
#define PIN_BTN_SELECT      5
#define PIN_BTN_BACK        6
#define PIN_BTN_CHECKPOINT  7
#define PIN_SD_CS          10
#define PIN_LED            13

// ─────────────────────────────────────────────────────────────────────
// ░░  OLED CONFIGURATION  ░░
// ─────────────────────────────────────────────────────────────────────

#define OLED_W            128
#define OLED_H             64
#define OLED_ADDR        0x3C
#define OLED_CONTENT_ROWS   6   // rows 1–6 (row 0 = header, row 7 = footer)
#define OLED_COLS          21   // ≈128 / 6px per char
#define OLED_REFRESH_MS    33   // 30 Hz

Adafruit_SSD1306 oled(OLED_W, OLED_H, &Wire, -1);
bool oledReady = false;
unsigned long lastOledRefresh = 0;

// ─────────────────────────────────────────────────────────────────────
// ░░  INTERPRETER LIMITS  ░░
// ─────────────────────────────────────────────────────────────────────

#define MAX_LINES          512
#define MAX_LINE_LEN       256
#define MAX_VARS            64
#define MAX_LABELS          64
#define MAX_FUNCTIONS       32
#define CALL_STACK_DEPTH    32
#define MAX_MEMORY         256
#define MAX_STEPS       100000UL
#define DEFAULT_DELAY_INIT   5
#define STARTUP_DELAY_MS  1500
#define COMBO_HOLD_MIN_MS   20

// ─────────────────────────────────────────────────────────────────────
// ░░  HID RATE LIMITER  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Prevents a runaway script from spamming the USB HID interface.
 * Two limits enforced:
 *   Rate  — minimum 2ms between any two HID reports
 *   Burst — maximum 50 events per 100ms window
 *   If burst exceeded → forced 50ms cooldown before continuing
 */
#define HID_RATE_MIN_MS      2   // minimum ms between HID events
#define HID_BURST_MAX       50   // max events per burst window
#define HID_BURST_WINDOW_MS 100  // burst window width in ms

static unsigned long _hidLastTime   = 0;
static int           _hidBurstCount = 0;
static unsigned long _hidBurstStart = 0;

// ─────────────────────────────────────────────────────────────────────
// ░░  TOKENIZER  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Replaces the fragile chained getToken() calls from v5.
 * Single pass over the line, produces a TokenList.
 * Quoted strings ("hello world") become one token.
 * tok[0] is force-uppercased for case-insensitive dispatch.
 */
#define MAX_TOKENS      12
#define MAX_TOKEN_LEN  128

struct TokenList {
    char tok[MAX_TOKENS][MAX_TOKEN_LEN];
    int  count;
    bool overflow;   // true if line had more than MAX_TOKENS tokens
};

static void tokenize(const char* src, TokenList& tl) {
    tl.count    = 0;
    tl.overflow = false;
    memset(tl.tok, 0, sizeof(tl.tok));

    const char* p = src;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        if (tl.count >= MAX_TOKENS) { tl.overflow = true; break; }

        char* dst = tl.tok[tl.count];
        int   len = 0;

        if (*p == '"') {
            p++;
            while (*p && *p != '"' && len < MAX_TOKEN_LEN - 1) dst[len++] = *p++;
            if (*p == '"') p++;
        } else {
            while (*p && *p != ' ' && *p != '\t' && len < MAX_TOKEN_LEN - 1)
                dst[len++] = *p++;
        }
        dst[len] = '\0';
        tl.count++;
    }

    // Uppercase command token only — args remain case-preserved
    if (tl.count > 0)
        for (int i = 0; tl.tok[0][i]; i++)
            tl.tok[0][i] = toupper((uint8_t)tl.tok[0][i]);
}

// Safe argument accessor — returns "" if index out of range
static inline const char* arg(const TokenList& tl, int idx) {
    return (idx >= 0 && idx < tl.count) ? tl.tok[idx] : "";
}

// ─────────────────────────────────────────────────────────────────────
// ░░  BOUNDS GUARD MACROS  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * GUARD_BOUNDS — checks an index against a max, logs error, and returns
 * from the calling function if out of range. Used in every array write.
 */
#define GUARD_BOUNDS(idx, maxVal, context)                               \
    do {                                                                  \
        if ((int)(idx) < 0 || (int)(idx) >= (int)(maxVal)) {            \
            char _gb[56];                                                 \
            snprintf(_gb, sizeof(_gb), "OOB " context ": %d (max %d)",  \
                     (int)(idx), (int)(maxVal));                         \
            dbgErr(_gb);                                                  \
            return;                                                       \
        }                                                                 \
    } while (0)

// Same but returns false instead of void
#define GUARD_BOUNDS_F(idx, maxVal, context)                             \
    do {                                                                  \
        if ((int)(idx) < 0 || (int)(idx) >= (int)(maxVal)) {            \
            char _gb[56];                                                 \
            snprintf(_gb, sizeof(_gb), "OOB " context ": %d (max %d)",  \
                     (int)(idx), (int)(maxVal));                         \
            dbgErr(_gb);                                                  \
            return false;                                                 \
        }                                                                 \
    } while (0)

// ─────────────────────────────────────────────────────────────────────
// ░░  COMMAND DISPATCH TABLE STRUCTURES  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Each entry maps a command name (uppercase) to a handler function
 * and declares how many tokens are minimally required (including cmd).
 * The dispatcher checks minToks before calling the handler —
 * syntax errors are caught uniformly without code in each handler.
 *
 * Handler signature: void handler(const TokenList& tl, int lineIdx)
 * Handlers that modify PC (JMP, CALL...) set currentLine directly.
 * The outer loop detects the change and does not auto-increment.
 */
typedef void (*CmdHandler)(const TokenList& tl, int lineIdx);

struct CmdEntry {
    const char* name;      // uppercase command name
    uint8_t     minToks;   // minimum tokens required (1 = cmd only, 2 = cmd + 1 arg...)
    CmdHandler  handler;
};

// ─────────────────────────────────────────────────────────────────────
// ░░  VALIDATION REPORT  ░░
// ─────────────────────────────────────────────────────────────────────

struct ValidationReport {
    int errors;    // syntax / semantic — block execution
    int warnings;  // unknown commands, style — allow execution
};

// ─────────────────────────────────────────────────────────────────────
// ░░  APPLICATION STATE MACHINE  ░░
// ─────────────────────────────────────────────────────────────────────

enum AppState {
    ST_BOOT,
    ST_OS_MENU,
    ST_FILE_MENU,
    ST_VALIDATE_RESULT,
    ST_RUNNING,
    ST_CHECKPOINT,
    ST_HALTED
};

AppState appState       = ST_BOOT;
int      osMenuCursor   = 0;
int      osMenuScroll   = 0;
int      fileMenuCursor = 0;
int      fileMenuScroll = 0;
int      selectedOS     = -1;

#define NUM_OS              4
#define MAX_FILES_PER_OS   32
#define MAX_FILENAME_LEN   32

char  fileList[MAX_FILES_PER_OS][MAX_FILENAME_LEN];
int   fileCount = 0;

const char* osNames[]   = { "Windows", "macOS", "Ubuntu", "Gnome" };
const char* osFolders[] = { "/windows", "/mac", "/ubuntu", "/gnome" };

char currentScriptName[MAX_FILENAME_LEN] = "";
char checkpointMsg[MAX_LINE_LEN]         = "";
bool waitingForCheckpoint                = false;
char haltMsg[64]                         = "Script complete.";

ValidationReport lastValidation = {0, 0};
// autorun countdown — if 0 errors + 0 warnings, auto-starts execution
unsigned long validateAutoRunAt = 0;

// ─────────────────────────────────────────────────────────────────────
// ░░  LOG RING BUFFER  ░░
// ─────────────────────────────────────────────────────────────────────

#define LOG_LINES     24
#define LOG_LINE_LEN  64

char     logBuffer[LOG_LINES][LOG_LINE_LEN];
int      logHead       = 0;
int      logTotal      = 0;
int      logScrollOff  = 0;
bool     logAutoScroll = true;

static void addLog(const char* msg) {
    Serial.println(msg);
    strncpy(logBuffer[logHead], msg, LOG_LINE_LEN - 1);
    logBuffer[logHead][LOG_LINE_LEN - 1] = '\0';
    logHead  = (logHead + 1) % LOG_LINES;
    logTotal++;
    if (logAutoScroll) logScrollOff = 0;
}

static void dbgLine(int idx, const char* text) {
    char buf[LOG_LINE_LEN];
    snprintf(buf, sizeof(buf), "[%lums L%d] %s", millis(), idx, text);
    addLog(buf);
}
static void dbgWarn(const char* m) {
    char buf[LOG_LINE_LEN]; snprintf(buf, sizeof(buf), "[%lums WARN] %s", millis(), m); addLog(buf);
}
static void dbgErr(const char* m) {
    char buf[LOG_LINE_LEN]; snprintf(buf, sizeof(buf), "[%lums ERR] %s",  millis(), m); addLog(buf);
}
static void dbgInfo(const char* m) {
    char buf[LOG_LINE_LEN]; snprintf(buf, sizeof(buf), "[%lums DSL] %s",  millis(), m); addLog(buf);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  INTERPRETER GLOBALS  ░░
// ─────────────────────────────────────────────────────────────────────

char     scriptLines[MAX_LINES][MAX_LINE_LEN];
int      totalLines    = 0;

struct Variable  { char name[32]; int32_t intVal; char strVal[128]; bool isString; };
struct LabelDef  { char name[32]; int lineIndex; };
struct FuncDef   { char name[32]; int lineIndex; };

Variable variables[MAX_VARS];  int varCount      = 0;
LabelDef labels[MAX_LABELS];   int labelCount    = 0;
FuncDef  functions[MAX_FUNCTIONS]; int funcCount = 0;
int      callStack[CALL_STACK_DEPTH];
int      stackTop      = 0;
int32_t  memArray[MAX_MEMORY];
int      currentLine   = 0;
int      defaultDelay  = DEFAULT_DELAY_INIT;
bool     halted        = false;
bool     debugMode     = false;
unsigned long stepCount = 0;

// ─────────────────────────────────────────────────────────────────────
// ░░  BUTTON HANDLER  ░░
// ─────────────────────────────────────────────────────────────────────

#define DEBOUNCE_MS  50
#define BTN_COUNT     5

const int btnPins[BTN_COUNT]  = { PIN_BTN_UP, PIN_BTN_DOWN, PIN_BTN_SELECT, PIN_BTN_BACK, PIN_BTN_CHECKPOINT };
enum BtnIdx { BTN_UP=0, BTN_DOWN=1, BTN_SEL=2, BTN_BACK=3, BTN_CHK=4 };

bool btnState[BTN_COUNT]      = {};
bool btnLastRaw[BTN_COUNT]    = {};
bool btnJustPressed[BTN_COUNT]= {};
unsigned long btnLastChange[BTN_COUNT] = {};

static void initButtons() {
    for (int i = 0; i < BTN_COUNT; i++) {
        pinMode(btnPins[i], INPUT_PULLUP);
        btnState[i] = btnLastRaw[i] = HIGH;
    }
}

static void handleButtons() {
    unsigned long now = millis();
    for (int i = 0; i < BTN_COUNT; i++) {
        btnJustPressed[i] = false;
        bool raw = digitalRead(btnPins[i]);
        if (raw != btnLastRaw[i]) { btnLastChange[i] = now; btnLastRaw[i] = raw; }
        if (now - btnLastChange[i] >= DEBOUNCE_MS && raw != btnState[i]) {
            btnState[i] = raw;
            if (raw == LOW) btnJustPressed[i] = true;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────
// ░░  SMART DELAY + JITTER  ░░
// ─────────────────────────────────────────────────────────────────────

void maybeUpdateOLED();  // forward declaration

static void smartDelay(unsigned long ms) {
    if (!ms) return;
    unsigned long until = millis() + ms;
    while (millis() < until) {
        handleButtons();
        maybeUpdateOLED();
        #if ENABLE_WATCHDOG
            hwWatchdog.feed();
        #endif
        if (halted) return;
        delayMicroseconds(500);
    }
}

static void jitterDelay(int base = -1) {
    if (base < 0) base = defaultDelay;
    int ms = base + (int)random(2, 7);
    if (debugMode) {
        char b[32]; snprintf(b, sizeof(b), "[DEBUG] delay %dms", ms); addLog(b);
    }
    smartDelay(ms);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  HID RATE GUARD  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Call before every Keyboard.press() / Keyboard.print() / etc.
 * Enforces minimum inter-event spacing and per-second burst cap.
 * A runaway FASTPRINT loop cannot saturate the USB HID pipe.
 */
static void hidRateGuard() {
    unsigned long now = millis();

    // Minimum inter-event interval
    long gap = (long)(now - _hidLastTime);
    if (gap < HID_RATE_MIN_MS) {
        delayMicroseconds((HID_RATE_MIN_MS - gap) * 1000);
    }

    // Burst window management
    if (now - _hidBurstStart > HID_BURST_WINDOW_MS) {
        _hidBurstStart = now; _hidBurstCount = 0;
    }
    _hidBurstCount++;
    if (_hidBurstCount > HID_BURST_MAX) {
        char b[48]; snprintf(b, sizeof(b), "HID burst cap (%d/100ms) — cooldown", HID_BURST_MAX);
        dbgWarn(b);
        smartDelay(50);  // forced cooldown
        _hidBurstStart = millis(); _hidBurstCount = 0;
    }

    _hidLastTime = millis();
}

// ─────────────────────────────────────────────────────────────────────
// ░░  OLED RENDERER  ░░
// ─────────────────────────────────────────────────────────────────────

static void oledRow(int row, const char* text, bool invert = false) {
    int y = row * 8;
    if (invert) {
        oled.fillRect(0, y, OLED_W, 8, SSD1306_WHITE);
        oled.setTextColor(SSD1306_BLACK);
    } else {
        oled.setTextColor(SSD1306_WHITE);
    }
    oled.setCursor(0, y);
    char buf[OLED_COLS + 1]; strncpy(buf, text, OLED_COLS); buf[OLED_COLS] = '\0';
    oled.print(buf);
    if (invert) oled.setTextColor(SSD1306_WHITE);
}

static void renderOSMenu() {
    oled.clearDisplay();
    oledRow(0, "  SELECT TARGET OS  ", true);
    for (int i = 0; i < OLED_CONTENT_ROWS && i < NUM_OS; i++) {
        int oi = osMenuScroll + i;
        if (oi >= NUM_OS) break;
        char line[OLED_COLS + 1];
        snprintf(line, sizeof(line), "%c %s", (oi == osMenuCursor) ? '>' : ' ', osNames[oi]);
        oledRow(i + 1, line);
    }
    oledRow(7, "\x18\x19=nav  \x10=sel  \x11=back");
    oled.display();
}

static void renderFileMenu() {
    oled.clearDisplay();
    char header[OLED_COLS + 1];
    snprintf(header, sizeof(header), " %s / %d files", osNames[selectedOS], fileCount);
    oledRow(0, header, true);
    if (fileCount == 0) {
        oledRow(2, "  (no .dsl files)");
        char p[OLED_COLS + 1]; snprintf(p, sizeof(p), "  %s/", osFolders[selectedOS]);
        oledRow(3, "  Put scripts in:"); oledRow(4, p);
    } else {
        for (int i = 0; i < OLED_CONTENT_ROWS && i < fileCount; i++) {
            int fi = fileMenuScroll + i;
            if (fi >= fileCount) break;
            char line[OLED_COLS + 1];
            snprintf(line, sizeof(line), "%c %s", (fi == fileMenuCursor) ? '>' : ' ', fileList[fi]);
            oledRow(i + 1, line);
        }
    }
    oledRow(7, "\x18\x19=nav  \x10=run  \x11=back");
    oled.display();
}

/*
 * Validation result screen.
 * Shows error/warning counts and lets user choose to run or cancel.
 * If 0 errors + 0 warnings: shows countdown to auto-run.
 * If errors > 0: SELECT disabled — script has syntax problems.
 */
static void renderValidateResult() {
    oled.clearDisplay();

    bool hasErrors   = lastValidation.errors   > 0;
    bool hasWarnings = lastValidation.warnings  > 0;

    if (hasErrors) {
        oledRow(0, "  ! ERRORS FOUND !  ", true);
    } else if (hasWarnings) {
        oledRow(0, "  WARNINGS FOUND   ", true);
    } else {
        oledRow(0, "  VALIDATION OK    ", true);
    }

    char line[OLED_COLS + 1];
    snprintf(line, sizeof(line), "  Errors:   %d", lastValidation.errors);
    oledRow(2, line);
    snprintf(line, sizeof(line), "  Warnings: %d", lastValidation.warnings);
    oledRow(3, line);

    if (hasErrors) {
        oledRow(5, "  Cannot run.");
        oledRow(6, "  Fix errors first.");
        oledRow(7, "  \x11=back            ");
    } else if (hasWarnings) {
        oledRow(5, "  May have issues.");
        oledRow(7, "  \x10=run   \x11=cancel ");
    } else {
        // Auto-run countdown
        long msLeft = (long)(validateAutoRunAt - millis());
        if (msLeft > 0) {
            snprintf(line, sizeof(line), "  Auto-run in %lds  ", msLeft / 1000 + 1);
            oledRow(5, line);
        } else {
            oledRow(5, "  Running...");
        }
        oledRow(7, "  \x10=run   \x11=cancel ");
    }
    oled.display();
}

static void renderRunning() {
    oled.clearDisplay();
    char header[OLED_COLS + 1];
    snprintf(header, sizeof(header), "\x10 %s", currentScriptName);
    oledRow(0, header, true);

    int available = min(logTotal, LOG_LINES);
    int viewable  = min(available, OLED_CONTENT_ROWS);
    int clamped   = constrain(logScrollOff, 0, max(0, available - viewable));

    for (int row = 0; row < viewable; row++) {
        int fromNewest = clamped + (viewable - 1 - row);
        int bufIdx = ((logHead - 1 - fromNewest) % LOG_LINES + LOG_LINES) % LOG_LINES;
        oledRow(row + 1, logBuffer[bufIdx]);
    }

    if (logScrollOff > 0) {
        char f[OLED_COLS + 1]; snprintf(f, sizeof(f), "\x18\x19scroll[+%d] \x11=stop", logScrollOff);
        oledRow(7, f);
    } else {
        oledRow(7, "\x18\x19=scroll  \x11=stop");
    }
    oled.display();
}

static void renderCheckpoint() {
    oled.clearDisplay();
    oledRow(0, "  ! CHECKPOINT !   ", true);
    const char* p = checkpointMsg;
    for (int row = 1; row <= 5 && *p; row++) {
        char lb[OLED_COLS + 1]; int i = 0;
        while (*p && i < OLED_COLS) lb[i++] = *p++;
        lb[i] = '\0'; oledRow(row, lb);
    }
    oledRow(6, "  Press [CHK] btn  ");
    oledRow(7, "\x11=abort            ");
    oled.display();
}

static void renderHalted() {
    oled.clearDisplay();
    oledRow(0, "     FINISHED      ", true);
    oledRow(2, haltMsg);
    oledRow(4, currentScriptName);
    oledRow(6, "  \x10=file  \x11=OS menu");
    oled.display();
}

void maybeUpdateOLED() {
    if (!oledReady) return;
    unsigned long now = millis();
    if (now - lastOledRefresh < OLED_REFRESH_MS) return;
    lastOledRefresh = now;
    oled.setTextSize(1);
    switch (appState) {
        case ST_OS_MENU:         renderOSMenu();          break;
        case ST_FILE_MENU:       renderFileMenu();        break;
        case ST_VALIDATE_RESULT: renderValidateResult();  break;
        case ST_RUNNING:         renderRunning();         break;
        case ST_CHECKPOINT:      renderCheckpoint();      break;
        case ST_HALTED:          renderHalted();          break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────
// ░░  STRING UTILITIES  ░░
// ─────────────────────────────────────────────────────────────────────

static void trimWhitespace(char* s) {
    int start = 0;
    while (s[start]==' '||s[start]=='\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s) - start + 1);
    int len = strlen(s);
    while (len > 0 && (s[len-1]==' '||s[len-1]=='\t'||s[len-1]=='\r'||s[len-1]=='\n')) s[--len]='\0';
}

static bool strStartsWith(const char* s, const char* p) { return strncmp(s, p, strlen(p)) == 0; }

static bool strEqCI(const char* a, const char* b) {
    while (*a && *b) { if (tolower(*a) != tolower(*b)) return false; a++; b++; }
    return *a == '\0' && *b == '\0';
}

// ─────────────────────────────────────────────────────────────────────
// ░░  VARIABLE MANAGEMENT  ░░
// ─────────────────────────────────────────────────────────────────────

static int findVar(const char* n) {
    for (int i = 0; i < varCount; i++) if (strcmp(variables[i].name, n) == 0) return i;
    return -1;
}
static int getOrCreateVar(const char* n) {
    int i = findVar(n); if (i >= 0) return i;
    GUARD_BOUNDS_F(varCount, MAX_VARS, "var table");
    strncpy(variables[varCount].name, n, 31); variables[varCount].name[31] = '\0';
    variables[varCount].intVal = 0; variables[varCount].strVal[0] = '\0'; variables[varCount].isString = false;
    return varCount++;
}
static int32_t getVarInt(const char* n) { int i = findVar(n); return i >= 0 ? variables[i].intVal : 0; }
static void setVarInt(const char* n, int32_t v) {
    int i = getOrCreateVar(n); if (i < 0) return;
    GUARD_BOUNDS(i, MAX_VARS, "setVarInt");
    variables[i].intVal = v; variables[i].isString = false;
}
static void setVarStr(const char* n, const char* s) {
    int i = getOrCreateVar(n); if (i < 0) return;
    GUARD_BOUNDS(i, MAX_VARS, "setVarStr");
    strncpy(variables[i].strVal, s, 127); variables[i].strVal[127] = '\0';
    variables[i].intVal = atoi(s); variables[i].isString = true;
}
static int32_t resolveValue(const char* t) {
    if (!t || !*t) return 0;
    bool isNum = true; int start = (t[0] == '-') ? 1 : 0;
    if (!t[start]) isNum = false;
    for (int i = start; t[i]; i++) if (!isdigit((uint8_t)t[i])) { isNum = false; break; }
    return isNum ? (int32_t)atol(t) : getVarInt(t);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  LABEL & FUNCTION MANAGEMENT  ░░
// ─────────────────────────────────────────────────────────────────────

static void registerLabel(const char* n, int idx) {
    if (labelCount >= MAX_LABELS) { dbgErr("Label table full"); return; }
    for (int i = 0; i < labelCount; i++)
        if (strcmp(labels[i].name, n) == 0) { labels[i].lineIndex = idx; return; }
    strncpy(labels[labelCount].name, n, 31); labels[labelCount].name[31] = '\0';
    labels[labelCount].lineIndex = idx; labelCount++;
}
static int findLabel(const char* n) {
    for (int i = 0; i < labelCount; i++) if (strcmp(labels[i].name, n) == 0) return labels[i].lineIndex;
    return -1;
}
static void registerFunction(const char* n, int idx) {
    if (funcCount >= MAX_FUNCTIONS) { dbgErr("Function table full"); return; }
    strncpy(functions[funcCount].name, n, 31); functions[funcCount].name[31] = '\0';
    functions[funcCount].lineIndex = idx; funcCount++;
}
static int findFunction(const char* n) {
    for (int i = 0; i < funcCount; i++) if (strcmp(functions[i].name, n) == 0) return functions[i].lineIndex;
    return -1;
}
static void stripParens(char* n) { char* p = strchr(n, '('); if (p) *p = '\0'; }

static void preScan() {
    labelCount = 0; funcCount = 0;
    for (int i = 0; i < totalLines; i++) {
        char line[MAX_LINE_LEN];
        strncpy(line, scriptLines[i], MAX_LINE_LEN - 1); line[MAX_LINE_LEN - 1] = '\0';
        trimWhitespace(line);
        if (strStartsWith(line, "LABEL ")) {
            char nm[64]; const char* p = line + 6;
            while (*p == ' ') p++;
            int j = 0; while (*p && *p != ' ' && j < 63) nm[j++] = *p++;
            nm[j] = '\0'; if (j) registerLabel(nm, i);
        } else {
            int len = strlen(line);
            if (len > 1 && line[len-1] == ':') {
                char nm[64]; strncpy(nm, line, len - 1); nm[len-1] = '\0';
                bool ok = true; for (int j = 0; nm[j]; j++) if (nm[j]==' '||nm[j]=='\t'){ok=false;break;}
                if (ok) registerLabel(nm, i);
            }
        }
        if (strStartsWith(line, "FUNCTION ")) {
            TokenList tl; tokenize(line, tl);
            if (tl.count >= 2) { char nm[64]; strncpy(nm, tl.tok[1], 63); stripParens(nm); registerFunction(nm, i); }
        }
    }
    char buf[48]; snprintf(buf, sizeof(buf), "Pre-scan: %d labels, %d funcs", labelCount, funcCount);
    dbgInfo(buf);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  HID KEY MAPPING  ░░
// ─────────────────────────────────────────────────────────────────────

static int resolveKey(const char* k) {
    if (!k || !*k) return 0;
    if (strEqCI(k,"CTRL")||strEqCI(k,"CONTROL"))    return MODIFIERKEY_CTRL;
    if (strEqCI(k,"SHIFT"))                          return MODIFIERKEY_SHIFT;
    if (strEqCI(k,"ALT"))                            return MODIFIERKEY_ALT;
    if (strEqCI(k,"GUI")||strEqCI(k,"WIN")||strEqCI(k,"WINDOWS")||strEqCI(k,"CMD")||strEqCI(k,"SUPER")) return MODIFIERKEY_GUI;
    if (strEqCI(k,"RCTRL")||strEqCI(k,"RIGHT_CTRL"))   return MODIFIERKEY_RIGHT_CTRL;
    if (strEqCI(k,"RSHIFT")||strEqCI(k,"RIGHT_SHIFT")) return MODIFIERKEY_RIGHT_SHIFT;
    if (strEqCI(k,"RALT")||strEqCI(k,"RIGHT_ALT"))     return MODIFIERKEY_RIGHT_ALT;
    if (strEqCI(k,"RGUI")||strEqCI(k,"RIGHT_GUI"))     return MODIFIERKEY_RIGHT_GUI;
    if (strEqCI(k,"ENTER")||strEqCI(k,"RETURN"))  return KEY_RETURN;
    if (strEqCI(k,"ESC")||strEqCI(k,"ESCAPE"))    return KEY_ESC;
    if (strEqCI(k,"BACKSPACE")||strEqCI(k,"BS"))  return KEY_BACKSPACE;
    if (strEqCI(k,"TAB"))   return KEY_TAB;   if (strEqCI(k,"SPACE")) return KEY_SPACE;
    if (strEqCI(k,"DELETE")||strEqCI(k,"DEL"))    return KEY_DELETE;
    if (strEqCI(k,"INSERT")||strEqCI(k,"INS"))    return KEY_INSERT;
    if (strEqCI(k,"HOME"))  return KEY_HOME;  if (strEqCI(k,"END"))   return KEY_END;
    if (strEqCI(k,"PAGE_UP")||strEqCI(k,"PAGEUP"))     return KEY_PAGE_UP;
    if (strEqCI(k,"PAGE_DOWN")||strEqCI(k,"PAGEDOWN")) return KEY_PAGE_DOWN;
    if (strEqCI(k,"CAPS_LOCK")||strEqCI(k,"CAPSLOCK")) return KEY_CAPS_LOCK;
    if (strEqCI(k,"NUM_LOCK")||strEqCI(k,"NUMLOCK"))   return KEY_NUM_LOCK;
    if (strEqCI(k,"SCROLL_LOCK")||strEqCI(k,"SCROLLLOCK")) return KEY_SCROLL_LOCK;
    if (strEqCI(k,"PRINT_SCREEN")||strEqCI(k,"PRTSC")) return KEY_PRINTSCREEN;
    if (strEqCI(k,"PAUSE"))  return KEY_PAUSE;
    if (strEqCI(k,"UP")||strEqCI(k,"UP_ARROW"))     return KEY_UP;
    if (strEqCI(k,"DOWN")||strEqCI(k,"DOWN_ARROW")) return KEY_DOWN;
    if (strEqCI(k,"LEFT")||strEqCI(k,"LEFT_ARROW")) return KEY_LEFT;
    if (strEqCI(k,"RIGHT")||strEqCI(k,"RIGHT_ARROW")) return KEY_RIGHT;
    if (strEqCI(k,"F1"))  return KEY_F1;  if (strEqCI(k,"F2"))  return KEY_F2;
    if (strEqCI(k,"F3"))  return KEY_F3;  if (strEqCI(k,"F4"))  return KEY_F4;
    if (strEqCI(k,"F5"))  return KEY_F5;  if (strEqCI(k,"F6"))  return KEY_F6;
    if (strEqCI(k,"F7"))  return KEY_F7;  if (strEqCI(k,"F8"))  return KEY_F8;
    if (strEqCI(k,"F9"))  return KEY_F9;  if (strEqCI(k,"F10")) return KEY_F10;
    if (strEqCI(k,"F11")) return KEY_F11; if (strEqCI(k,"F12")) return KEY_F12;
    if (strEqCI(k,"NUM0")) return KEYPAD_0; if (strEqCI(k,"NUM1")) return KEYPAD_1;
    if (strEqCI(k,"NUM2")) return KEYPAD_2; if (strEqCI(k,"NUM3")) return KEYPAD_3;
    if (strEqCI(k,"NUM4")) return KEYPAD_4; if (strEqCI(k,"NUM5")) return KEYPAD_5;
    if (strEqCI(k,"NUM6")) return KEYPAD_6; if (strEqCI(k,"NUM7")) return KEYPAD_7;
    if (strEqCI(k,"NUM8")) return KEYPAD_8; if (strEqCI(k,"NUM9")) return KEYPAD_9;
    if (strEqCI(k,"NUM_ENTER")) return KEYPAD_ENTER;
    if (strEqCI(k,"NUM_PLUS"))  return KEYPAD_PLUS;
    if (strEqCI(k,"NUM_MINUS")) return KEYPAD_MINUS;
    if (strEqCI(k,"NUM_STAR"))  return KEYPAD_ASTERIX;
    if (strEqCI(k,"NUM_SLASH")) return KEYPAD_SLASH;
    if (strEqCI(k,"NUM_DOT"))   return KEYPAD_PERIOD;
    if (k[1] == '\0') return (int)(uint8_t)k[0];
    char m[48]; snprintf(m, sizeof(m), "Unknown key: %s", k); dbgWarn(m);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  SAFE CHAR TYPING — US-layout explicit HID scan codes  ░░
// ─────────────────────────────────────────────────────────────────────

static bool safeTypeChar(char c) {
    uint8_t key = 0; bool shift = false;
    if (c>='a'&&c<='z')       { key=KEY_A+(c-'a'); }
    else if (c>='A'&&c<='Z')  { key=KEY_A+(c-'A'); shift=true; }
    else if (c>='1'&&c<='9')  { key=KEY_1+(c-'1'); }
    else if (c=='0')   key=KEY_0;     else if (c==' ')  key=KEY_SPACE;
    else if (c=='\n')  key=KEY_RETURN;else if (c=='\t') key=KEY_TAB;
    else if (c=='-')   key=KEY_MINUS; else if (c=='=')  key=KEY_EQUAL;
    else if (c=='[')   key=KEY_LEFT_BRACE;  else if (c==']') key=KEY_RIGHT_BRACE;
    else if (c=='\\')  key=KEY_BACKSLASH;   else if (c==';') key=KEY_SEMICOLON;
    else if (c=='\'')  key=KEY_QUOTE;       else if (c=='`') key=KEY_TILDE;
    else if (c==',')   key=KEY_COMMA;       else if (c=='.') key=KEY_PERIOD;
    else if (c=='/')   key=KEY_SLASH;
    else if (c=='!')  {key=KEY_1;          shift=true;} else if (c=='@'){key=KEY_2;shift=true;}
    else if (c=='#')  {key=KEY_3;          shift=true;} else if (c=='$'){key=KEY_4;shift=true;}
    else if (c=='%')  {key=KEY_5;          shift=true;} else if (c=='^'){key=KEY_6;shift=true;}
    else if (c=='&')  {key=KEY_7;          shift=true;} else if (c=='*'){key=KEY_8;shift=true;}
    else if (c=='(')  {key=KEY_9;          shift=true;} else if (c==')'){key=KEY_0;shift=true;}
    else if (c=='_')  {key=KEY_MINUS;      shift=true;} else if (c=='+'){key=KEY_EQUAL;shift=true;}
    else if (c=='{')  {key=KEY_LEFT_BRACE; shift=true;} else if (c=='}'){key=KEY_RIGHT_BRACE;shift=true;}
    else if (c=='|')  {key=KEY_BACKSLASH;  shift=true;} else if (c==':'){key=KEY_SEMICOLON;shift=true;}
    else if (c=='"')  {key=KEY_QUOTE;      shift=true;} else if (c=='~'){key=KEY_TILDE;shift=true;}
    else if (c=='<')  {key=KEY_COMMA;      shift=true;} else if (c=='>'){key=KEY_PERIOD;shift=true;}
    else if (c=='?')  {key=KEY_SLASH;      shift=true;}
    else {
        char m[40]; snprintf(m, sizeof(m), "Layout-unsafe 0x%02X fallback", (uint8_t)c);
        dbgWarn(m); Keyboard.print(c); return false;
    }
    hidRateGuard();
    if (shift) Keyboard.press(MODIFIERKEY_SHIFT);
    Keyboard.press(key);
    delay(random(3, 7));      // tiny hold — too short for smartDelay overhead
    Keyboard.release(key);
    if (shift) Keyboard.release(MODIFIERKEY_SHIFT);
    return true;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  HID INJECTION HELPERS  ░░
// ─────────────────────────────────────────────────────────────────────

static void slowPrint(const char* t) {
    int n = 0;
    for (int i = 0; t[i] && !halted; i++) {
        if (debugMode) { char b[28]; snprintf(b,sizeof(b),"[DBG] SLW:'%c'",t[i]); addLog(b); }
        else safeTypeChar(t[i]);
        delay(random(8, 16));
        if (++n % 20 == 0) smartDelay(random(20, 41));
    }
}

static void fastPrint(const char* t) {
    int n = 0;
    for (int i = 0; t[i] && !halted; i++) {
        if (debugMode) { char b[28]; snprintf(b,sizeof(b),"[DBG] FST:'%c'",t[i]); addLog(b); }
        else safeTypeChar(t[i]);
        delay(random(2, 6));
        if (++n % 15 == 0) smartDelay(random(10, 21));
    }
}

static void waitStable() {
    int ms = 500 + (int)random(200, 500);
    char b[32]; snprintf(b, sizeof(b), "WAIT_STABLE %dms", ms); dbgInfo(b);
    smartDelay(ms);
}

static void safeRelease() {
    if (debugMode) { addLog("[DEBUG] SAFE_RELEASE"); return; }
    Keyboard.releaseAll(); smartDelay(50);
}

static void performReset() {
    dbgInfo("RESET — clearing interpreter state");
    Keyboard.releaseAll();
    varCount = 0; stackTop = 0; stepCount = 0; currentLine = 0; halted = false;
    memset(memArray, 0, sizeof(memArray));
    dbgInfo("RESET — restarting from line 0");
}

static void runCheckpoint(const char* msg) {
    strncpy(checkpointMsg, msg, sizeof(checkpointMsg) - 1);
    waitingForCheckpoint = true; appState = ST_CHECKPOINT;
    char lb[LOG_LINE_LEN]; snprintf(lb, sizeof(lb), "[CHECKPOINT] %s", msg); addLog(lb);
    addLog(">>> Waiting for CHK button <<<");
    for (int i = 0; i < 3; i++) { digitalWrite(PIN_LED, HIGH); smartDelay(150); digitalWrite(PIN_LED, LOW); smartDelay(150); }
    while (waitingForCheckpoint && !halted) {
        handleButtons(); maybeUpdateOLED();
        if (btnJustPressed[BTN_CHK]) { waitingForCheckpoint = false; addLog("[CHECKPOINT] Resuming."); }
        if (btnJustPressed[BTN_BACK]) { halted = true; strncpy(haltMsg, "Aborted at checkpoint.", sizeof(haltMsg)); addLog("[CHECKPOINT] Aborted."); }
        delayMicroseconds(500);
    }
    if (!halted) appState = ST_RUNNING;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  COMMAND HANDLER FORWARD DECLARATIONS  ░░
// ─────────────────────────────────────────────────────────────────────

static void cmdDebug     (const TokenList& tl, int li);
static void cmdReset     (const TokenList& tl, int li);
static void cmdCheckpoint(const TokenList& tl, int li);
static void cmdHalt      (const TokenList& tl, int li);
static void cmdPrint     (const TokenList& tl, int li);
static void cmdSet       (const TokenList& tl, int li);
static void cmdClr       (const TokenList& tl, int li);
static void cmdInc       (const TokenList& tl, int li);
static void cmdDec       (const TokenList& tl, int li);
static void cmdArith     (const TokenList& tl, int li);  // ADD SUB MUL DIV
static void cmdCmp       (const TokenList& tl, int li);
static void cmdIf        (const TokenList& tl, int li);
static void cmdJmp       (const TokenList& tl, int li);
static void cmdLoop      (const TokenList& tl, int li);
static void cmdCall      (const TokenList& tl, int li);
static void cmdStore     (const TokenList& tl, int li);
static void cmdLoad      (const TokenList& tl, int li);
static void cmdDelay     (const TokenList& tl, int li);
static void cmdDefDelay  (const TokenList& tl, int li);
static void cmdWaitReady (const TokenList& tl, int li);
static void cmdWaitStable(const TokenList& tl, int li);
static void cmdSafeRel   (const TokenList& tl, int li);
static void cmdRelAll    (const TokenList& tl, int li);
static void cmdHidKey    (const TokenList& tl, int li);  // PRESS HOLD RELEASE TAP
static void cmdCombo     (const TokenList& tl, int li);
static void cmdString    (const TokenList& tl, int li);  // STRING STRINGLN
static void cmdSlowPrint (const TokenList& tl, int li);
static void cmdFastPrint (const TokenList& tl, int li);
static void cmdRetry     (const TokenList& tl, int li);
static void cmdPinMode   (const TokenList& tl, int li);
static void cmdPinWrite  (const TokenList& tl, int li);
static void cmdPinRead   (const TokenList& tl, int li);
static void cmdAnalogRead(const TokenList& tl, int li);
static void cmdSwitchWait(const TokenList& tl, int li);
static void cmdTone      (const TokenList& tl, int li);
static void cmdOled      (const TokenList& tl, int li);  // OLED_CLEAR OLED_PRINT OLED_VAR
static void cmdModeCheck (const TokenList& tl, int li);

// ─────────────────────────────────────────────────────────────────────
// ░░  COMMAND DISPATCH TABLE  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Name must be UPPERCASE (tokenizer already uppercases tok[0]).
 * minToks = minimum number of tokens including command name.
 *   1 = command only (e.g. HALT)
 *   2 = command + 1 arg
 *   etc.
 * The dispatcher enforces minToks before calling the handler.
 * Aliases (GOTO=JMP, etc.) are separate entries pointing to same handler.
 */
static const CmdEntry CMD_TABLE[] = {
    // Control flow
    { "DEBUG",        2, cmdDebug      },
    { "RESET",        1, cmdReset      },
    { "CHECKPOINT",   2, cmdCheckpoint },
    { "HALT",         1, cmdHalt       },
    { "REM",          1, cmdHalt       },  // REM: handled as no-op via cmdHalt returning immediately
    // Variables
    { "PRINT",        2, cmdPrint      },
    { "ECHO",         2, cmdPrint      },
    { "SET",          3, cmdSet        },
    { "CLR",          2, cmdClr        },
    { "INC",          2, cmdInc        },
    { "DEC",          2, cmdDec        },
    // Math
    { "ADD",          4, cmdArith      },
    { "SUB",          4, cmdArith      },
    { "MUL",          4, cmdArith      },
    { "DIV",          4, cmdArith      },
    // Logic
    { "CMP",          3, cmdCmp        },
    { "IF",           6, cmdIf         },
    // Flow
    { "JMP",          2, cmdJmp        },
    { "GOTO",         2, cmdJmp        },
    { "LOOP",         3, cmdLoop       },
    { "CALL",         2, cmdCall       },
    // Memory
    { "STORE",        3, cmdStore      },
    { "LOAD",         3, cmdLoad       },
    // Timing
    { "DELAY",        2, cmdDelay      },
    { "DEFAULT_DELAY",2, cmdDefDelay   },
    { "DEFAULTDELAY", 2, cmdDefDelay   },
    { "WAIT_READY",   1, cmdWaitReady  },
    { "WAITREADY",    1, cmdWaitReady  },
    { "WAIT_STABLE",  1, cmdWaitStable },
    { "WAITSTABLE",   1, cmdWaitStable },
    // HID
    { "SAFE_RELEASE", 1, cmdSafeRel    },
    { "SAFERELEASE",  1, cmdSafeRel    },
    { "RELEASEALL",   1, cmdRelAll     },
    { "PRESS",        2, cmdHidKey     },
    { "HOLD",         2, cmdHidKey     },
    { "RELEASE",      2, cmdHidKey     },
    { "TAP",          2, cmdHidKey     },
    { "COMBO",        2, cmdCombo      },
    // Text injection
    { "STRING",       2, cmdString     },
    { "STRINGLN",     2, cmdString     },
    { "SLOWPRINT",    2, cmdSlowPrint  },
    { "FASTPRINT",    2, cmdFastPrint  },
    // Reliability
    { "RETRY",        3, cmdRetry      },
    // GPIO
    { "PIN_MODE",     3, cmdPinMode    },
    { "PIN_WRITE",    3, cmdPinWrite   },
    { "PIN_READ",     3, cmdPinRead    },
    { "ANALOG_READ",  3, cmdAnalogRead },
    { "SWITCH_WAIT",  3, cmdSwitchWait },
    { "TONE",         4, cmdTone       },
    // OLED
    { "OLED_CLEAR",   1, cmdOled       },
    { "OLED_PRINT",   3, cmdOled       },
    { "OLED_VAR",     3, cmdOled       },
    // System
    { "MODE_CHECK",   2, cmdModeCheck  },
};
static const int N_CMDS = (int)(sizeof(CMD_TABLE) / sizeof(CMD_TABLE[0]));

// ─────────────────────────────────────────────────────────────────────
// ░░  DISPATCHER  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Looks up tok[0] in CMD_TABLE, checks minToks, calls handler.
 * Returns true if command was found (whether it succeeded or not).
 * Returns false for unknown command (caller logs warning).
 *
 * ERROR RECOVERY POLICY:
 *   minToks check failure → log error + SKIP (not halt)
 *   Unknown command       → log warning + SKIP
 *   Fatal logic errors    → handler sets halted=true
 */
static bool dispatchCommand(const TokenList& tl, int lineIdx) {
    for (int i = 0; i < N_CMDS; i++) {
        if (strcmp(tl.tok[0], CMD_TABLE[i].name) == 0) {
            if (tl.count < CMD_TABLE[i].minToks) {
                // Syntax error — skip line, log error, continue execution
                char m[72];
                snprintf(m, sizeof(m), "L%d: %s needs %d arg(s), got %d — skipping",
                         lineIdx, tl.tok[0],
                         CMD_TABLE[i].minToks - 1, tl.count - 1);
                dbgErr(m);
                return true;  // found but skipped — don't emit "unknown cmd" warning
            }
            CMD_TABLE[i].handler(tl, lineIdx);
            return true;
        }
    }
    return false;  // not found
}

// ─────────────────────────────────────────────────────────────────────
// ░░  COMMAND HANDLER IMPLEMENTATIONS  ░░
// ─────────────────────────────────────────────────────────────────────

// Returns early for REM (no-op) and DEBUG-as-comment patterns
static void cmdDebug(const TokenList& tl, int li) {
    (void)li;
    if      (strEqCI(arg(tl,1), "ON"))  { debugMode = true;  addLog("[DSL] Debug ON"); }
    else if (strEqCI(arg(tl,1), "OFF")) { debugMode = false; addLog("[DSL] Debug OFF"); }
    else dbgWarn("DEBUG expects ON or OFF");
}
// REM shares cmdHalt handler — override: REM is a no-op
// We handle REM before dispatch by checking tok[0]=="REM" in executeLine.

static void cmdReset(const TokenList& tl, int li) { (void)tl; (void)li; performReset(); }

static void cmdCheckpoint(const TokenList& tl, int li) {
    (void)li; runCheckpoint(arg(tl, 1));
}

static void cmdHalt(const TokenList& tl, int li) {
    (void)tl; (void)li;
    strncpy(haltMsg, "Script halted.", sizeof(haltMsg));
    dbgInfo("HALT reached."); halted = true;
}

static void cmdPrint(const TokenList& tl, int li) {
    (void)li;
    const char* tok = arg(tl, 1);
    int idx = findVar(tok);
    char out[LOG_LINE_LEN];
    if (idx >= 0) {
        if (variables[idx].isString) snprintf(out, sizeof(out), "%s", variables[idx].strVal);
        else                         snprintf(out, sizeof(out), "%ld", (long)variables[idx].intVal);
    } else {
        strncpy(out, tok, sizeof(out) - 1);
    }
    addLog(out);
}

static void cmdSet(const TokenList& tl, int li) {
    (void)li;
    const char* val = arg(tl, 2);
    bool isNum = true; int start = (val[0] == '-') ? 1 : 0;
    if (!val[start]) isNum = false;
    for (int i = start; val[i]; i++) if (!isdigit((uint8_t)val[i])) { isNum = false; break; }
    if (isNum) setVarInt(arg(tl, 1), (int32_t)atol(val));
    else       setVarStr(arg(tl, 1), val);
}
static void cmdClr(const TokenList& tl, int li) { (void)li; setVarInt(arg(tl,1), 0); }
static void cmdInc(const TokenList& tl, int li) { (void)li; setVarInt(arg(tl,1), getVarInt(arg(tl,1)) + 1); }
static void cmdDec(const TokenList& tl, int li) { (void)li; setVarInt(arg(tl,1), getVarInt(arg(tl,1)) - 1); }

static void cmdArith(const TokenList& tl, int li) {
    (void)li;
    int32_t va = resolveValue(arg(tl,2)), vb = resolveValue(arg(tl,3)), res = 0;
    char c0 = tl.tok[0][0];
    if      (c0 == 'A') res = va + vb;
    else if (c0 == 'S') res = va - vb;
    else if (c0 == 'M') res = va * vb;
    else {
        if (vb == 0) { dbgErr("DIV by zero"); halted = true; return; }
        res = va / vb;
    }
    setVarInt(arg(tl,1), res);
}

static void cmdCmp(const TokenList& tl, int li) {
    (void)li;
    int32_t va = resolveValue(arg(tl,1)), vb = resolveValue(arg(tl,2));
    setVarInt("__CMP__", (va < vb) ? -1 : (va > vb) ? 1 : 0);
}

static void cmdIf(const TokenList& tl, int li) {
    (void)li;
    // IF reg op val JMP label   → tok 0..5
    int32_t lhs = resolveValue(arg(tl,1)), rhs = resolveValue(arg(tl,3));
    const char* op = arg(tl, 2); bool cond = false;
    if      (strcmp(op,"==")==0||strEqCI(op,"EQ"))  cond=(lhs==rhs);
    else if (strcmp(op,"!=")==0||strEqCI(op,"NEQ")) cond=(lhs!=rhs);
    else if (strcmp(op,">")==0 ||strEqCI(op,"GT"))  cond=(lhs>rhs);
    else if (strcmp(op,"<")==0 ||strEqCI(op,"LT"))  cond=(lhs<rhs);
    else if (strcmp(op,">=")==0||strEqCI(op,"GTE")) cond=(lhs>=rhs);
    else if (strcmp(op,"<=")==0||strEqCI(op,"LTE")) cond=(lhs<=rhs);
    if (cond && strEqCI(arg(tl,4), "JMP")) {
        int dest = findLabel(arg(tl, 5));
        if (dest < 0) {
            char m[56]; snprintf(m, sizeof(m), "IF JMP: label '%s' not found", arg(tl,5));
            dbgErr(m); halted = true; return;
        }
        currentLine = dest;
    }
}

static void cmdJmp(const TokenList& tl, int li) {
    (void)li;
    int dest = findLabel(arg(tl, 1));
    if (dest < 0) {
        char m[48]; snprintf(m, sizeof(m), "JMP: label '%s' not found", arg(tl,1));
        dbgErr(m); halted = true; return;
    }
    currentLine = dest;
}

static void cmdLoop(const TokenList& tl, int li) {
    (void)li;
    int32_t cnt = getVarInt(arg(tl, 1));
    if (cnt > 0) {
        setVarInt(arg(tl, 1), cnt - 1);
        int dest = findLabel(arg(tl, 2));
        if (dest < 0) {
            char m[48]; snprintf(m, sizeof(m), "LOOP: label '%s' not found", arg(tl,2));
            dbgErr(m); halted = true; return;
        }
        currentLine = dest;
    }
}

// callFunction is defined later (needs executeLine forward declaration)
void callFunction(const char* name, int returnLine);

static void cmdCall(const TokenList& tl, int li) {
    char nm[64]; strncpy(nm, arg(tl, 1), 63); stripParens(nm);
    callFunction(nm, li + 1);
}

static void cmdStore(const TokenList& tl, int li) {
    (void)li;
    int addr = (int)resolveValue(arg(tl, 1));
    GUARD_BOUNDS(addr, MAX_MEMORY, "STORE");
    memArray[addr] = getVarInt(arg(tl, 2));
}
static void cmdLoad(const TokenList& tl, int li) {
    (void)li;
    int addr = (int)resolveValue(arg(tl, 2));
    GUARD_BOUNDS(addr, MAX_MEMORY, "LOAD");
    setVarInt(arg(tl, 1), memArray[addr]);
}

static void cmdDelay(const TokenList& tl, int li) {
    (void)li;
    int ms = (int)resolveValue(arg(tl, 1));
    if (debugMode) { char b[28]; snprintf(b,sizeof(b),"[DEBUG] DELAY %dms",ms); addLog(b); }
    smartDelay(ms);
}
static void cmdDefDelay(const TokenList& tl, int li) {
    (void)li; defaultDelay = max(1, (int)resolveValue(arg(tl, 1)));
}
static void cmdWaitReady (const TokenList& tl, int li) { (void)tl;(void)li; smartDelay(500); }
static void cmdWaitStable(const TokenList& tl, int li) { (void)tl;(void)li; waitStable(); }
static void cmdSafeRel   (const TokenList& tl, int li) { (void)tl;(void)li; safeRelease(); }
static void cmdRelAll    (const TokenList& tl, int li) {
    (void)tl;(void)li;
    if (debugMode) { addLog("[DEBUG] RELEASEALL"); return; }
    Keyboard.releaseAll();
}

static void cmdHidKey(const TokenList& tl, int li) {
    (void)li;
    int k = resolveKey(arg(tl, 1));
    if (!k) return;
    char c0 = tl.tok[0][0]; // P=PRESS/HOLD, R=RELEASE, T=TAP
    if (c0 == 'R') {  // RELEASE
        if (debugMode) { char b[32]; snprintf(b,sizeof(b),"[DBG] REL %s",arg(tl,1)); addLog(b); return; }
        hidRateGuard(); Keyboard.release(k);
    } else if (c0 == 'T') {  // TAP
        if (debugMode) { char b[32]; snprintf(b,sizeof(b),"[DBG] TAP %s",arg(tl,1)); addLog(b); return; }
        hidRateGuard(); Keyboard.press(k); delay(50+random(0,20)); Keyboard.release(k);
    } else {  // PRESS / HOLD
        if (debugMode) { char b[32]; snprintf(b,sizeof(b),"[DBG] PRESS %s",arg(tl,1)); addLog(b); return; }
        hidRateGuard(); Keyboard.press(k);
    }
}

static void cmdCombo(const TokenList& tl, int li) {
    (void)li;
    int keys[8]; int kc = 0;
    for (int i = 1; i < tl.count && kc < 8; i++) {
        int k = resolveKey(tl.tok[i]); if (k) keys[kc++] = k;
    }
    if (debugMode) { char b[32]; snprintf(b,sizeof(b),"[DBG] COMBO %d keys",kc); addLog(b); return; }
    hidRateGuard();
    for (int i = 0; i < kc; i++) Keyboard.press(keys[i]);
    smartDelay(COMBO_HOLD_MIN_MS + random(0, 30));
    safeRelease();
}

static void cmdString(const TokenList& tl, int li) {
    (void)li;
    const char* s = arg(tl, 1);
    if (debugMode) { char b[LOG_LINE_LEN]; snprintf(b,sizeof(b),"[DBG] STRING: %s",s); addLog(b); return; }
    for (int i = 0; s[i] && !halted; i++) safeTypeChar(s[i]);
    if (tl.tok[0][6] == 'L') {  // STRINGLN has 'L' at index 6
        hidRateGuard(); Keyboard.press(KEY_RETURN); delay(30); Keyboard.release(KEY_RETURN);
    }
}
static void cmdSlowPrint(const TokenList& tl, int li) {
    (void)li;
    if (debugMode) { char b[LOG_LINE_LEN]; snprintf(b,sizeof(b),"[DBG] SLOWPRINT: %s",arg(tl,1)); addLog(b); return; }
    slowPrint(arg(tl, 1));
}
static void cmdFastPrint(const TokenList& tl, int li) {
    (void)li;
    if (debugMode) { char b[LOG_LINE_LEN]; snprintf(b,sizeof(b),"[DBG] FASTPRINT: %s",arg(tl,1)); addLog(b); return; }
    fastPrint(arg(tl, 1));
}

static void cmdRetry(const TokenList& tl, int li) {
    (void)li;
    int attempts = max(1, (int)resolveValue(arg(tl, 1)));
    // Rebuild sub-command string from tokens 2 onwards
    char subcmd[MAX_LINE_LEN] = "";
    for (int i = 2; i < tl.count; i++) {
        if (i > 2) strncat(subcmd, " ", MAX_LINE_LEN - strlen(subcmd) - 1);
        // Re-quote token if it was originally a quoted string (heuristic: contains spaces?)
        // For simplicity we just concat as-is; quoted strings were already extracted
        strncat(subcmd, tl.tok[i], MAX_LINE_LEN - strlen(subcmd) - 1);
    }
    for (int attempt = 0; attempt < attempts && !halted; attempt++) {
        char b[32]; snprintf(b, sizeof(b), "[RETRY] %d/%d", attempt + 1, attempts); addLog(b);
        if (totalLines < MAX_LINES) {
            strncpy(scriptLines[totalLines], subcmd, MAX_LINE_LEN - 1);
            scriptLines[totalLines][MAX_LINE_LEN - 1] = '\0';
            int sl = currentLine;
            // executeLine forward declaration used here
            extern void executeLine(int);
            executeLine(totalLines);
            currentLine = sl;
        }
        if (attempt < attempts - 1) {
            int p = 100 + random(0, 200);
            snprintf(b, sizeof(b), "[RETRY] pause %dms", p); addLog(b);
            smartDelay(p);
        }
    }
}

static void cmdPinMode(const TokenList& tl, int li) {
    (void)li;
    int pin = (int)resolveValue(arg(tl, 1));
    const char* m = arg(tl, 2);
    if      (strEqCI(m,"INPUT"))        pinMode(pin, INPUT);
    else if (strEqCI(m,"OUTPUT"))       pinMode(pin, OUTPUT);
    else if (strEqCI(m,"INPUT_PULLUP")) pinMode(pin, INPUT_PULLUP);
    else { char b[48]; snprintf(b,sizeof(b),"PIN_MODE: unknown mode '%s'",m); dbgWarn(b); }
}
static void cmdPinWrite(const TokenList& tl, int li) {
    (void)li;
    int pin = (int)resolveValue(arg(tl, 1));
    int val = (strEqCI(arg(tl,2),"HIGH")||strcmp(arg(tl,2),"1")==0) ? HIGH : LOW;
    if (debugMode) { char b[28]; snprintf(b,sizeof(b),"[DBG] PIN%d=%s",pin,arg(tl,2)); addLog(b); return; }
    digitalWrite(pin, val);
}
static void cmdPinRead(const TokenList& tl, int li) {
    (void)li;
    setVarInt(arg(tl,1), digitalRead((int)resolveValue(arg(tl,2))));
}
static void cmdAnalogRead(const TokenList& tl, int li) {
    (void)li;
    const char* ps = arg(tl, 2);
    int pin = (ps[0]=='A'||ps[0]=='a') ? (int)resolveValue(ps+1) : (int)resolveValue(ps);
    setVarInt(arg(tl, 1), analogRead(pin));
}
static void cmdSwitchWait(const TokenList& tl, int li) {
    (void)li;
    int pin    = (int)resolveValue(arg(tl, 1));
    int target = (strEqCI(arg(tl,2),"HIGH")||strcmp(arg(tl,2),"1")==0) ? HIGH : LOW;
    unsigned long deadline = millis() + 30000;
    while (digitalRead(pin) != target && millis() < deadline) {
        handleButtons(); maybeUpdateOLED(); if (halted) return; delayMicroseconds(500);
    }
    if (millis() >= deadline) dbgWarn("SWITCH_WAIT: timed out after 30s");
}
static void cmdTone(const TokenList& tl, int li) {
    (void)li;
    int pin  = (int)resolveValue(arg(tl,1));
    int freq = (int)resolveValue(arg(tl,2));
    int dur  = (int)resolveValue(arg(tl,3));
    if (debugMode) { char b[40]; snprintf(b,sizeof(b),"[DBG] TONE %d %dHz %dms",pin,freq,dur); addLog(b); return; }
    tone(pin, freq, dur); smartDelay(dur);
}

static void cmdOled(const TokenList& tl, int li) {
    (void)li;
    if (!oledReady) return;
    char c1 = tl.tok[0][5];  // 'C'=CLEAR, 'P'=PRINT, 'V'=VAR
    if (c1 == 'C') {
        if (debugMode) { addLog("[DBG] OLED_CLEAR"); return; }
        oled.clearDisplay(); oled.display(); return;
    }
    int row = (int)resolveValue(arg(tl, 1));
    GUARD_BOUNDS(row, OLED_ROWS, "OLED row");
    if (c1 == 'P') {
        const char* tx = arg(tl, 2);
        if (debugMode) { char b[LOG_LINE_LEN]; snprintf(b,sizeof(b),"[DBG] OLED[%d]: %s",row,tx); addLog(b); return; }
        oled.fillRect(0, row*8, OLED_W, 8, SSD1306_BLACK);
        oled.setCursor(0, row*8); oled.setTextColor(SSD1306_WHITE); oled.print(tx); oled.display();
    } else {  // VAR
        int vi = findVar(arg(tl, 2)); char disp[24];
        if (vi >= 0) {
            if (variables[vi].isString) snprintf(disp,sizeof(disp),"%.22s",variables[vi].strVal);
            else snprintf(disp,sizeof(disp),"%s=%ld",arg(tl,2),(long)variables[vi].intVal);
        } else snprintf(disp,sizeof(disp),"?%s",arg(tl,2));
        if (debugMode) { char b[LOG_LINE_LEN]; snprintf(b,sizeof(b),"[DBG] OLEDV[%d]: %s",row,disp); addLog(b); return; }
        oled.fillRect(0,row*8,OLED_W,8,SSD1306_BLACK);
        oled.setCursor(0,row*8); oled.setTextColor(SSD1306_WHITE); oled.print(disp); oled.display();
    }
}

static void cmdModeCheck(const TokenList& tl, int li) {
    (void)li;
    // 0 = HID, 1 = storage (storage mode never reaches scripts, so always 0 here)
    setVarInt(arg(tl, 1), 0);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  SCRIPT VALIDATOR  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * Runs after preScan(), before execution starts.
 * Checks every line for:
 *   - Minimum token count (syntax)
 *   - Known command name (style)
 *   - CALL / JMP / LOOP / IF JMP target existence (semantics)
 *
 * ERROR TIERS:
 *   Errors   (ValidationReport.errors)   → block execution until fixed
 *   Warnings (ValidationReport.warnings) → allow execution, user confirms
 *
 * Output goes to log buffer (visible on OLED + Serial).
 */
static bool isStructuralLine(const char* line) {
    if (!line || !*line) return true;
    if (strStartsWith(line, "REM ") || strcmp(line,"REM")==0) return true;
    if (strStartsWith(line, "#") || strStartsWith(line, "//")) return true;
    if (strStartsWith(line, "LABEL ")) return true;
    if (strStartsWith(line, "FUNCTION ") || strStartsWith(line,"END_FUNCTION") || strStartsWith(line,"ENDFUNCTION")) return true;
    int len = strlen(line);
    if (len > 1 && line[len-1] == ':') return true;
    return false;
}

static ValidationReport validateScript() {
    ValidationReport r = {0, 0};
    addLog("─── Validating script ───────────");

    for (int i = 0; i < totalLines; i++) {
        char line[MAX_LINE_LEN];
        strncpy(line, scriptLines[i], MAX_LINE_LEN - 1); line[MAX_LINE_LEN - 1] = '\0';
        trimWhitespace(line);
        if (isStructuralLine(line)) continue;

        static TokenList tl;
        tokenize(line, tl);
        if (tl.count == 0) continue;

        if (tl.overflow) {
            char m[56]; snprintf(m,sizeof(m),"L%d: >%d tokens — check syntax",i,MAX_TOKENS);
            dbgWarn(m); r.warnings++;
        }

        // ── Syntax: check minimum token count via CMD_TABLE ──────
        bool found = false;
        for (int c = 0; c < N_CMDS; c++) {
            if (strcmp(tl.tok[0], CMD_TABLE[c].name) == 0) {
                found = true;
                if (tl.count < CMD_TABLE[c].minToks) {
                    char m[72];
                    snprintf(m, sizeof(m), "L%d: %s needs %d arg(s), got %d",
                             i, tl.tok[0], CMD_TABLE[c].minToks - 1, tl.count - 1);
                    dbgErr(m); r.errors++;
                }
                break;
            }
        }

        if (!found) {
            // RET/RETURN are valid but not in CMD_TABLE (handled by callFunction)
            if (strcmp(tl.tok[0],"RET")==0||strcmp(tl.tok[0],"RETURN")==0) { /* OK */ }
            else {
                char m[48]; snprintf(m,sizeof(m),"L%d: unknown cmd '%s'",i,tl.tok[0]);
                dbgWarn(m); r.warnings++;
            }
        }

        // ── Semantics: check jump/call targets exist ─────────────
        if (strcmp(tl.tok[0],"JMP")==0||strcmp(tl.tok[0],"GOTO")==0) {
            if (tl.count >= 2 && findLabel(tl.tok[1]) < 0) {
                char m[56]; snprintf(m,sizeof(m),"L%d: JMP target '%s' not found",i,tl.tok[1]);
                dbgErr(m); r.errors++;
            }
        }
        if (strcmp(tl.tok[0],"CALL")==0) {
            if (tl.count >= 2) {
                char nm[64]; strncpy(nm,tl.tok[1],63); stripParens(nm);
                if (findFunction(nm) < 0) {
                    char m[56]; snprintf(m,sizeof(m),"L%d: CALL target '%s' not found",i,nm);
                    dbgErr(m); r.errors++;
                }
            }
        }
        if (strcmp(tl.tok[0],"IF")==0 && tl.count >= 6) {
            if (strcmp(tl.tok[4],"JMP")==0 && findLabel(tl.tok[5]) < 0) {
                char m[56]; snprintf(m,sizeof(m),"L%d: IF JMP target '%s' not found",i,tl.tok[5]);
                dbgErr(m); r.errors++;
            }
        }
        if (strcmp(tl.tok[0],"LOOP")==0 && tl.count >= 3) {
            if (findLabel(tl.tok[2]) < 0) {
                char m[56]; snprintf(m,sizeof(m),"L%d: LOOP target '%s' not found",i,tl.tok[2]);
                dbgErr(m); r.errors++;
            }
        }
    }

    char summary[LOG_LINE_LEN];
    snprintf(summary, sizeof(summary), "Validation: %d error(s), %d warning(s)",
             r.errors, r.warnings);
    addLog(summary);
    addLog("─────────────────────────────────");
    return r;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  SCRIPT LOADING  ░░
// ─────────────────────────────────────────────────────────────────────

static void clearScript() {
    totalLines = 0; varCount = 0; stackTop = 0; stepCount = 0;
    currentLine = 0; halted = false;
    memset(memArray, 0, sizeof(memArray));
    for (int i = 0; i < MAX_LINES; i++) scriptLines[i][0] = '\0';
}

static bool loadFromSD(const char* path) {
    if (!SD.begin(PIN_SD_CS)) { dbgErr("SD init failed"); return false; }
    File f = SD.open(path);
    if (!f) { char m[64]; snprintf(m,sizeof(m),"File not found: %s",path); dbgErr(m); return false; }
    clearScript();
    char buf[MAX_LINE_LEN]; int len = 0;
    while (f.available() && totalLines < MAX_LINES) {
        char c = f.read();
        if (c == '\n' || !f.available()) {
            if (!f.available() && c != '\n' && len < MAX_LINE_LEN - 1) buf[len++] = c;
            buf[len] = '\0'; trimWhitespace(buf);
            if (len > 0 && strlen(buf) > 0) {
                strncpy(scriptLines[totalLines], buf, MAX_LINE_LEN - 1);
                scriptLines[totalLines][MAX_LINE_LEN-1] = '\0'; totalLines++;
            }
            len = 0;
        } else if (c != '\r') { if (len < MAX_LINE_LEN-1) buf[len++] = c; }
    }
    f.close();
    char m[48]; snprintf(m,sizeof(m),"Loaded %d lines from %s",totalLines,path); dbgInfo(m);
    return totalLines > 0;
}

static bool loadFromSerial() {
    addLog("[Serial] Send script, end with '###'");
    clearScript();
    unsigned long timeout = millis() + 30000;
    while (millis() < timeout && totalLines < MAX_LINES) {
        if (!Serial.available()) { smartDelay(10); continue; }
        char buf[MAX_LINE_LEN];
        int len = Serial.readBytesUntil('\n', buf, MAX_LINE_LEN - 1);
        buf[len] = '\0'; trimWhitespace(buf);
        if (strcmp(buf, "###") == 0) break;
        if (strlen(buf) > 0) {
            strncpy(scriptLines[totalLines], buf, MAX_LINE_LEN - 1);
            scriptLines[totalLines][MAX_LINE_LEN-1] = '\0'; totalLines++;
            timeout = millis() + 5000;
        }
    }
    char m[36]; snprintf(m,sizeof(m),"Serial: loaded %d lines",totalLines); dbgInfo(m);
    return totalLines > 0;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  MAIN INTERPRETER — executeLine()  ░░
// ─────────────────────────────────────────────────────────────────────
/*
 * In v6 executeLine() is now very thin:
 *   1. Get line text
 *   2. Tokenize it (single pass)
 *   3. Skip structural lines by token[0]
 *   4. Dispatch to handler via CMD_TABLE
 *
 * This means adding a new command in future requires:
 *   - Write a static cmdXxx() handler function
 *   - Add one entry to CMD_TABLE
 *   That's it. No touching executeLine().
 */

// Static token list lives in .bss — reused every call, safe for
// single-threaded cooperative execution (callFunction awaits return).
static TokenList _tl;

void executeLine(int lineIdx) {
    if (lineIdx < 0 || lineIdx >= totalLines) return;

    char line[MAX_LINE_LEN];
    strncpy(line, scriptLines[lineIdx], MAX_LINE_LEN - 1); line[MAX_LINE_LEN - 1] = '\0';
    trimWhitespace(line);
    if (!line[0]) return;

    if (debugMode) dbgLine(lineIdx, line);

    // Skip structural lines early — no need to tokenize them
    if (isStructuralLine(line)) return;
    // RET/RETURN inside a function body are caught by callFunction's loop
    // and never reach executeLine when called from the main scheduler
    if (strStartsWith(line, "RET")) return;

    tokenize(line, _tl);
    if (_tl.count == 0) return;

    // REM as command (no trailing space case handled here)
    if (strcmp(_tl.tok[0], "REM") == 0) return;

    if (!dispatchCommand(_tl, lineIdx)) {
        char m[56]; snprintf(m, sizeof(m), "Unknown cmd: %s", _tl.tok[0]); dbgWarn(m);
    }
}

// ─────────────────────────────────────────────────────────────────────
// ░░  FUNCTION EXECUTOR  ░░
// ─────────────────────────────────────────────────────────────────────

void callFunction(const char* name, int returnLine) {
    int dest = findFunction(name);
    if (dest < 0) {
        char m[48]; snprintf(m,sizeof(m),"CALL: func '%s' not found",name); dbgErr(m);
        halted = true; return;
    }
    if (stackTop >= CALL_STACK_DEPTH) { dbgErr("CALL: stack overflow"); halted = true; return; }
    callStack[stackTop++] = returnLine;
    currentLine = dest + 1;

    while (currentLine < totalLines && !halted) {
        handleButtons(); maybeUpdateOLED();
        #if ENABLE_WATCHDOG
            hwWatchdog.feed();
        #endif

        char fline[MAX_LINE_LEN];
        strncpy(fline, scriptLines[currentLine], MAX_LINE_LEN - 1); fline[MAX_LINE_LEN-1] = '\0';
        trimWhitespace(fline);

        if (strStartsWith(fline,"END_FUNCTION")||strStartsWith(fline,"ENDFUNCTION")||
            strStartsWith(fline,"RET")||strStartsWith(fline,"RETURN")) break;

        int saved = currentLine;
        executeLine(currentLine);

        if (++stepCount > MAX_STEPS) { dbgErr("Watchdog: infinite loop in function"); halted = true; break; }
        if (currentLine == saved) currentLine++;
        if (!halted) jitterDelay();
    }

    if (stackTop <= 0) { dbgErr("Stack underflow on RET"); halted = true; return; }
    currentLine = callStack[--stackTop];
    currentLine--;  // main loop will +1 after return
}

// ─────────────────────────────────────────────────────────────────────
// ░░  FILE BROWSER  ░░
// ─────────────────────────────────────────────────────────────────────

static void loadFileList(int osIdx) {
    fileCount = 0;
    memset(fileList, 0, sizeof(fileList));
    if (!SD.begin(PIN_SD_CS)) { dbgErr("SD init failed in file browser"); return; }
    SD.mkdir(osFolders[osIdx]);
    File dir = SD.open(osFolders[osIdx]);
    if (!dir) { char m[48]; snprintf(m,sizeof(m),"Folder not found: %s",osFolders[osIdx]); dbgWarn(m); return; }
    while (fileCount < MAX_FILES_PER_OS) {
        File entry = dir.openNextFile();
        if (!entry) break;
        if (!entry.isDirectory()) {
            const char* nm = entry.name();
            int nmLen = strlen(nm);
            if (nmLen > 4 && strEqCI(nm + nmLen - 4, ".dsl")) {
                strncpy(fileList[fileCount], nm, MAX_FILENAME_LEN - 1);
                fileList[fileCount][MAX_FILENAME_LEN - 1] = '\0';
                fileCount++;
            }
        }
        entry.close();
    }
    dir.close();
    char m[48]; snprintf(m,sizeof(m),"%d .dsl files in %s",fileCount,osFolders[osIdx]); dbgInfo(m);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  STATE MACHINE — menu navigation  ░░
// ─────────────────────────────────────────────────────────────────────

static void handleMenuInput() {
    switch (appState) {

        case ST_OS_MENU:
            if (btnJustPressed[BTN_UP]) {
                osMenuCursor = max(0, osMenuCursor - 1);
                if (osMenuCursor < osMenuScroll) osMenuScroll = osMenuCursor;
            }
            if (btnJustPressed[BTN_DOWN]) {
                osMenuCursor = min(NUM_OS - 1, osMenuCursor + 1);
                if (osMenuCursor >= osMenuScroll + OLED_CONTENT_ROWS)
                    osMenuScroll = osMenuCursor - OLED_CONTENT_ROWS + 1;
            }
            if (btnJustPressed[BTN_SEL]) {
                selectedOS = osMenuCursor;
                fileMenuCursor = 0; fileMenuScroll = 0;
                loadFileList(selectedOS);
                appState = ST_FILE_MENU;
            }
            break;

        case ST_FILE_MENU:
            if (btnJustPressed[BTN_BACK]) { appState = ST_OS_MENU; }
            if (fileCount == 0) break;
            if (btnJustPressed[BTN_UP]) {
                fileMenuCursor = max(0, fileMenuCursor - 1);
                if (fileMenuCursor < fileMenuScroll) fileMenuScroll = fileMenuCursor;
            }
            if (btnJustPressed[BTN_DOWN]) {
                fileMenuCursor = min(fileCount - 1, fileMenuCursor + 1);
                if (fileMenuCursor >= fileMenuScroll + OLED_CONTENT_ROWS)
                    fileMenuScroll = fileMenuCursor - OLED_CONTENT_ROWS + 1;
            }
            if (btnJustPressed[BTN_SEL]) {
                char fullPath[64];
                snprintf(fullPath, sizeof(fullPath), "%s/%s",
                         osFolders[selectedOS], fileList[fileMenuCursor]);
                strncpy(currentScriptName, fileList[fileMenuCursor], sizeof(currentScriptName) - 1);

                // Clear logs for fresh run
                logHead = 0; logTotal = 0; logScrollOff = 0; logAutoScroll = true;
                memset(logBuffer, 0, sizeof(logBuffer));
                char lm[64]; snprintf(lm,sizeof(lm),"Loading: %s",currentScriptName); addLog(lm);

                if (!loadFromSD(fullPath)) {
                    snprintf(haltMsg, sizeof(haltMsg), "Load failed: %s", currentScriptName);
                    appState = ST_HALTED; break;
                }

                // Pre-scan for labels/functions, then validate
                preScan();
                lastValidation = validateScript();

                appState = ST_VALIDATE_RESULT;
                // Auto-run timer — only fires if 0 errors AND 0 warnings
                if (lastValidation.errors == 0 && lastValidation.warnings == 0)
                    validateAutoRunAt = millis() + 2000;
                else
                    validateAutoRunAt = 0;  // no auto-run
            }
            break;

        case ST_VALIDATE_RESULT:
            if (btnJustPressed[BTN_BACK]) { appState = ST_FILE_MENU; }
            // SELECT blocked if errors present
            if (btnJustPressed[BTN_SEL] && lastValidation.errors == 0) {
                stepCount = 0; halted = false;
                strncpy(haltMsg, "Script complete.", sizeof(haltMsg));
                appState = ST_RUNNING;
            }
            // Auto-run countdown
            if (validateAutoRunAt > 0 && millis() >= validateAutoRunAt) {
                validateAutoRunAt = 0;
                stepCount = 0; halted = false;
                strncpy(haltMsg, "Script complete.", sizeof(haltMsg));
                appState = ST_RUNNING;
            }
            break;

        case ST_HALTED:
            if (btnJustPressed[BTN_SEL] || btnJustPressed[BTN_UP] ||
                btnJustPressed[BTN_DOWN] || btnJustPressed[BTN_CHK]) {
                appState = ST_FILE_MENU;
            }
            if (btnJustPressed[BTN_BACK]) { appState = ST_OS_MENU; }
            break;

        default: break;
    }
}

static void handleRunningInput() {
    if (btnJustPressed[BTN_BACK]) {
        halted = true; Keyboard.releaseAll();
        snprintf(haltMsg, sizeof(haltMsg), "Stopped by user.");
        addLog("[FORCE STOP] Script aborted.");
    }
    // Log scrolling
    if (btnJustPressed[BTN_UP]) {
        logScrollOff++; logAutoScroll = false;
        int avail = min(logTotal, LOG_LINES);
        logScrollOff = constrain(logScrollOff, 0, max(0, avail - OLED_CONTENT_ROWS));
    }
    if (btnJustPressed[BTN_DOWN]) {
        logScrollOff = max(0, logScrollOff - 1);
        if (logScrollOff == 0) logAutoScroll = true;
    }
}

// ─────────────────────────────────────────────────────────────────────
// ░░  SETUP  ░░
// ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    pinMode(PIN_LED, OUTPUT);
    initButtons();
    delay(STARTUP_DELAY_MS);
    randomSeed(analogRead(A0));

    #if ENABLE_WATCHDOG
        WDT_timings_t wdtCfg;
        wdtCfg.trigger = 5;   // interrupt after 5s of no feed
        wdtCfg.timeout = 10;  // reset after 10s of no feed
        hwWatchdog.begin(wdtCfg);
    #endif

    Serial.println("╔══════════════════════════════╗");
    Serial.println("║   TeensyDSL v6               ║");
    Serial.println("║   Tokenizer + Validator       ║");
    Serial.println("╚══════════════════════════════╝");

    Wire.begin();
    if (oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
        oledReady = true;
        oled.clearDisplay(); oled.setTextSize(1); oled.setTextColor(SSD1306_WHITE);
        oled.setCursor(0,0); oled.println("TeensyDSL v6"); oled.println("Booting..."); oled.display();
    } else {
        Serial.println("[OLED] Not found at 0x3C.");
    }

    if (!SD.begin(PIN_SD_CS)) Serial.println("[SD] Warning: init failed.");
    else { for (int i = 0; i < NUM_OS; i++) SD.mkdir(osFolders[i]); }

    appState = ST_OS_MENU;
    addLog("[DSL] v6 ready — select OS + script");
}

// ─────────────────────────────────────────────────────────────────────
// ░░  LOOP — cooperative multitasking scheduler  ░░
// ─────────────────────────────────────────────────────────────────────

void loop() {
    // Feed hardware watchdog — must happen at least every 5s
    #if ENABLE_WATCHDOG
        hwWatchdog.feed();
    #endif

    // 1. Read buttons
    handleButtons();

    // 2. Route input to correct handler
    if (appState == ST_RUNNING || appState == ST_CHECKPOINT) {
        handleRunningInput();
    } else {
        handleMenuInput();
    }

    // 3. Execute one interpreter step
    if (appState == ST_RUNNING && !halted) {
        if (currentLine >= totalLines) {
            dbgInfo("Script complete.");
            halted = true; appState = ST_HALTED;
        } else if (++stepCount > MAX_STEPS) {
            dbgErr("Watchdog: infinite loop in main body");
            halted = true;
            snprintf(haltMsg, sizeof(haltMsg), "Watchdog triggered.");
            appState = ST_HALTED;
        } else {
            int saved = currentLine;
            executeLine(currentLine);
            if (!halted && currentLine == saved) currentLine++;
            if (!halted) jitterDelay();
        }

        if (halted && appState == ST_RUNNING) {
            Keyboard.releaseAll();
            appState = ST_HALTED;
        }
    }

    // 4. Update OLED at 30Hz
    maybeUpdateOLED();
}
