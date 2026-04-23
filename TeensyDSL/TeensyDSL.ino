/*
 * ╔══════════════════════════════════════════════════════════════════╗
 * ║               TeensyDSL — Complete Interpreter                  ║
 * ║         For Teensy 3.x / 4.x with USB HID support               ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  ARDUINO IDE SETUP:                                              ║
 * ║  Board   → Teensy 4.0 (or your model)                           ║
 * ║  USB Type → Keyboard + Mouse + Joystick                         ║
 * ║  Libraries needed: SD (built-in), Keyboard (Teensy built-in)    ║
 * ╠══════════════════════════════════════════════════════════════════╣
 * ║  SCRIPT FILE: place "script.dsl" on the SD card root            ║
 * ║  OR send via Serial at 115200 baud (end script with "###")      ║
 * ╚══════════════════════════════════════════════════════════════════╝
 *
 * SUPPORTED COMMANDS:
 *   Variables : SET, CLR, GET
 *   Math      : ADD, SUB, MUL, DIV, INC, DEC
 *   Logic     : CMP, IF <reg> <op> <val> JMP <label>
 *   Flow      : JMP, CALL, RET, HALT
 *   Loop      : LOOP <reg> <label>
 *   Memory    : STORE <addr> <reg>, LOAD <reg> <addr>
 *   Timing    : DELAY, DEFAULT_DELAY, WAIT_READY
 *   Debug     : PRINT, ECHO
 *   HID Keys  : PRESS, HOLD, RELEASE, TAP, RELEASEALL, COMBO
 *   Injection : SLOWPRINT, FASTPRINT, STRING, STRINGLN
 *   Structure : LABEL, FUNCTION/END_FUNCTION, REM, #, //
 */

#include <Arduino.h>
#include <SD.h>
#include <SPI.h>

// ─────────────────────────────────────────────────────────────────────
// ░░  CONFIGURATION  ░░
// ─────────────────────────────────────────────────────────────────────

#define SD_CS_PIN          10        // SD chip-select pin (adjust for your board)
#define SCRIPT_FILE        "script.dsl"

#define MAX_LINES          512       // Max lines in script
#define MAX_LINE_LEN       256       // Max characters per line
#define MAX_VARS           64        // Max variables
#define MAX_LABELS         64        // Max labels
#define MAX_FUNCTIONS      32        // Max named functions
#define CALL_STACK_DEPTH   32        // Max nested CALL depth
#define MAX_MEMORY         256       // LOAD/STORE address space

#define STARTUP_DELAY_MS   1500      // Wait after USB enumerate
#define DEFAULT_DELAY_INIT 5         // Default inter-command delay (ms)

// ─────────────────────────────────────────────────────────────────────
// ░░  DATA STRUCTURES  ░░
// ─────────────────────────────────────────────────────────────────────

struct Variable {
    char    name[32];
    int32_t intVal;
    char    strVal[128];
    bool    isString;
};

struct Label {
    char name[32];
    int  lineIndex;
};

struct Function {
    char name[32];
    int  lineIndex;   // line index of the FUNCTION declaration
};

// ─────────────────────────────────────────────────────────────────────
// ░░  GLOBAL STATE  ░░
// ─────────────────────────────────────────────────────────────────────

char*    scriptLines[MAX_LINES];
int      totalLines   = 0;

Variable variables[MAX_VARS];
int      varCount     = 0;

Label    labels[MAX_LABELS];
int      labelCount   = 0;

Function functions[MAX_FUNCTIONS];
int      funcCount    = 0;

int      callStack[CALL_STACK_DEPTH];
int      stackTop     = 0;

int32_t  memArray[MAX_MEMORY];

int      currentLine  = 0;
int      defaultDelay = DEFAULT_DELAY_INIT;
bool     halted       = false;

// ─────────────────────────────────────────────────────────────────────
// ░░  STRING UTILITIES  ░░
// ─────────────────────────────────────────────────────────────────────

void trimWhitespace(char* s) {
    // Strip leading whitespace
    int start = 0;
    while (s[start] == ' ' || s[start] == '\t') start++;
    if (start > 0) memmove(s, s + start, strlen(s) - start + 1);
    // Strip trailing whitespace + CR/LF
    int len = strlen(s);
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' ||
                       s[len-1] == '\r' || s[len-1] == '\n')) {
        s[--len] = '\0';
    }
}

bool strStartsWith(const char* s, const char* prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

bool strEqCI(const char* a, const char* b) {   // case-insensitive equality
    while (*a && *b) {
        if (tolower((uint8_t)*a) != tolower((uint8_t)*b)) return false;
        a++; b++;
    }
    return *a == '\0' && *b == '\0';
}

/*
 * getToken() — pulls the next whitespace-delimited token (or quoted string)
 * from `src` into `buf`. Returns pointer past the consumed token.
 */

const char* getToken(const char* src, char* buf, int bufLen) {
    while (*src == ' ' || *src == '\t') src++;   // skip leading spaces

    if (*src == '"') {
        src++;                                    // skip opening quote
        int i = 0;
        while (*src && *src != '"' && i < bufLen - 1) buf[i++] = *src++;
        buf[i] = '\0';
        if (*src == '"') src++;                  // skip closing quote
    } else {
        int i = 0;
        while (*src && *src != ' ' && *src != '\t' && i < bufLen - 1)
            buf[i++] = *src++;
        buf[i] = '\0';
    }
    return src;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  VARIABLE MANAGEMENT  ░░
// ─────────────────────────────────────────────────────────────────────

int findVar(const char* name) {
    for (int i = 0; i < varCount; i++)
        if (strcmp(variables[i].name, name) == 0) return i;
    return -1;
}

int getOrCreateVar(const char* name) {
    int idx = findVar(name);
    if (idx >= 0) return idx;
    if (varCount >= MAX_VARS) {
        Serial.println("[ERR] Variable table full");
        return -1;
    }
    strncpy(variables[varCount].name, name, 31);
    variables[varCount].name[31] = '\0';
    variables[varCount].intVal   = 0;
    variables[varCount].strVal[0]= '\0';
    variables[varCount].isString = false;
    return varCount++;
}

int32_t getVarInt(const char* name) {
    int idx = findVar(name);
    return (idx >= 0) ? variables[idx].intVal : 0;
}

void setVarInt(const char* name, int32_t val) {
    int idx = getOrCreateVar(name);
    if (idx < 0) return;
    variables[idx].intVal   = val;
    variables[idx].isString = false;
}

void setVarStr(const char* name, const char* str) {
    int idx = getOrCreateVar(name);
    if (idx < 0) return;
    strncpy(variables[idx].strVal, str, 127);
    variables[idx].strVal[127] = '\0';
    variables[idx].intVal      = atoi(str);
    variables[idx].isString    = true;
}

/*
 * resolveValue() — if token looks like an integer, return it;
 * otherwise look it up as a variable name.
 */
int32_t resolveValue(const char* token) {
    if (!token || !*token) return 0;
    // Leading minus is okay for negative literals
    bool isNum = true;
    int  start = (token[0] == '-') ? 1 : 0;
    if (token[start] == '\0') isNum = false;
    for (int i = start; token[i]; i++) {
        if (!isdigit((uint8_t)token[i])) { isNum = false; break; }
    }
    return isNum ? (int32_t)atol(token) : getVarInt(token);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  LABEL & FUNCTION MANAGEMENT  ░░
// ─────────────────────────────────────────────────────────────────────

void registerLabel(const char* name, int lineIdx) {
    if (labelCount >= MAX_LABELS) return;
    // Check duplicate
    for (int i = 0; i < labelCount; i++)
        if (strcmp(labels[i].name, name) == 0) { labels[i].lineIndex = lineIdx; return; }
    strncpy(labels[labelCount].name, name, 31);
    labels[labelCount].name[31]   = '\0';
    labels[labelCount].lineIndex  = lineIdx;
    labelCount++;
}

int findLabel(const char* name) {
    for (int i = 0; i < labelCount; i++)
        if (strcmp(labels[i].name, name) == 0) return labels[i].lineIndex;
    return -1;
}

void registerFunction(const char* name, int lineIdx) {
    if (funcCount >= MAX_FUNCTIONS) return;
    strncpy(functions[funcCount].name, name, 31);
    functions[funcCount].name[31]   = '\0';
    functions[funcCount].lineIndex  = lineIdx;
    funcCount++;
}

int findFunction(const char* name) {
    for (int i = 0; i < funcCount; i++)
        if (strcmp(functions[i].name, name) == 0) return functions[i].lineIndex;
    return -1;
}

// Strip trailing parentheses from a function name token, e.g. "blink()" → "blink"
void stripParens(char* name) {
    char* p = strchr(name, '(');
    if (p) *p = '\0';
}

/*
 * preScan() — first pass over all lines to register
 * LABEL declarations and FUNCTION definitions.
 */
void preScan() {
    labelCount = 0;
    funcCount  = 0;

    for (int i = 0; i < totalLines; i++) {
        char line[MAX_LINE_LEN];
        strncpy(line, scriptLines[i], MAX_LINE_LEN - 1);
        line[MAX_LINE_LEN - 1] = '\0';
        trimWhitespace(line);

        // "LABEL name"
        if (strStartsWith(line, "LABEL ")) {
            char name[64];
            getToken(line + 6, name, sizeof(name));
            if (strlen(name) > 0) registerLabel(name, i);
        }
        // "name:" shorthand
        else {
            int len = strlen(line);
            if (len > 1 && line[len - 1] == ':') {
                char name[64];
                strncpy(name, line, len - 1);
                name[len - 1] = '\0';
                // Only register if it looks like a plain identifier
                bool ok = true;
                for (int j = 0; name[j]; j++)
                    if (name[j] == ' ' || name[j] == '\t') { ok = false; break; }
                if (ok) registerLabel(name, i);
            }
        }

        // "FUNCTION name()"
        if (strStartsWith(line, "FUNCTION ")) {
            char name[64];
            getToken(line + 9, name, sizeof(name));
            stripParens(name);
            if (strlen(name) > 0) registerFunction(name, i);
        }
    }

    Serial.print("[DSL] Labels: ");   Serial.println(labelCount);
    Serial.print("[DSL] Functions: "); Serial.println(funcCount);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  HID KEY MAPPING  ░░
// ─────────────────────────────────────────────────────────────────────

/*
 * Teensy USB HID uses integer key codes from <keylayouts.h> (included
 * automatically by the Teensy Keyboard library).
 * MODIFIERKEY_* values are sent via Keyboard.press() just like regular keys.
 */
int resolveKey(const char* k) {
    if (!k || !*k) return 0;

    // ── Modifiers ──────────────────────────────────────────────────
    if (strEqCI(k, "CTRL")  || strEqCI(k, "CONTROL"))      return MODIFIERKEY_CTRL;
    if (strEqCI(k, "SHIFT"))                                return MODIFIERKEY_SHIFT;
    if (strEqCI(k, "ALT"))                                  return MODIFIERKEY_ALT;
    if (strEqCI(k, "GUI")   || strEqCI(k, "WIN")    ||
        strEqCI(k, "WINDOWS") || strEqCI(k, "CMD")  ||
        strEqCI(k, "SUPER"))                                return MODIFIERKEY_GUI;
    if (strEqCI(k, "RCTRL") || strEqCI(k, "RIGHT_CTRL"))   return MODIFIERKEY_RIGHT_CTRL;
    if (strEqCI(k, "RSHIFT")|| strEqCI(k, "RIGHT_SHIFT"))  return MODIFIERKEY_RIGHT_SHIFT;
    if (strEqCI(k, "RALT")  || strEqCI(k, "RIGHT_ALT"))    return MODIFIERKEY_RIGHT_ALT;
    if (strEqCI(k, "RGUI")  || strEqCI(k, "RIGHT_GUI"))    return MODIFIERKEY_RIGHT_GUI;

    // ── Special Keys ───────────────────────────────────────────────
    if (strEqCI(k, "ENTER")  || strEqCI(k, "RETURN"))      return KEY_RETURN;
    if (strEqCI(k, "ESC")    || strEqCI(k, "ESCAPE"))      return KEY_ESC;
    if (strEqCI(k, "BACKSPACE") || strEqCI(k, "BS"))       return KEY_BACKSPACE;
    if (strEqCI(k, "TAB"))                                  return KEY_TAB;
    if (strEqCI(k, "SPACE"))                                return (int)' ';
    if (strEqCI(k, "DELETE") || strEqCI(k, "DEL"))         return KEY_DELETE;
    if (strEqCI(k, "INSERT") || strEqCI(k, "INS"))         return KEY_INSERT;
    if (strEqCI(k, "HOME"))                                 return KEY_HOME;
    if (strEqCI(k, "END"))                                  return KEY_END;
    if (strEqCI(k, "PAGE_UP")  || strEqCI(k, "PAGEUP"))    return KEY_PAGE_UP;
    if (strEqCI(k, "PAGE_DOWN")|| strEqCI(k, "PAGEDOWN"))  return KEY_PAGE_DOWN;
    if (strEqCI(k, "CAPS_LOCK")|| strEqCI(k, "CAPSLOCK"))  return KEY_CAPS_LOCK;
    if (strEqCI(k, "NUM_LOCK") || strEqCI(k, "NUMLOCK"))   return KEY_NUM_LOCK;
    if (strEqCI(k, "SCROLL_LOCK") || strEqCI(k, "SCROLLLOCK")) return KEY_SCROLL_LOCK;
    if (strEqCI(k, "PRINT_SCREEN") || strEqCI(k, "PRTSC")) return KEY_PRINTSCREEN;
    if (strEqCI(k, "PAUSE"))                                return KEY_PAUSE;

    // ── Arrow Keys ─────────────────────────────────────────────────
    if (strEqCI(k, "UP")    || strEqCI(k, "UP_ARROW"))     return KEY_UP;
    if (strEqCI(k, "DOWN")  || strEqCI(k, "DOWN_ARROW"))   return KEY_DOWN;
    if (strEqCI(k, "LEFT")  || strEqCI(k, "LEFT_ARROW"))   return KEY_LEFT;
    if (strEqCI(k, "RIGHT") || strEqCI(k, "RIGHT_ARROW"))  return KEY_RIGHT;

    // ── Function Keys ──────────────────────────────────────────────
    if (strEqCI(k, "F1"))  return KEY_F1;
    if (strEqCI(k, "F2"))  return KEY_F2;
    if (strEqCI(k, "F3"))  return KEY_F3;
    if (strEqCI(k, "F4"))  return KEY_F4;
    if (strEqCI(k, "F5"))  return KEY_F5;
    if (strEqCI(k, "F6"))  return KEY_F6;
    if (strEqCI(k, "F7"))  return KEY_F7;
    if (strEqCI(k, "F8"))  return KEY_F8;
    if (strEqCI(k, "F9"))  return KEY_F9;
    if (strEqCI(k, "F10")) return KEY_F10;
    if (strEqCI(k, "F11")) return KEY_F11;
    if (strEqCI(k, "F12")) return KEY_F12;

    // ── Numpad ─────────────────────────────────────────────────────
    if (strEqCI(k, "NUM0")) return KEYPAD_0;
    if (strEqCI(k, "NUM1")) return KEYPAD_1;
    if (strEqCI(k, "NUM2")) return KEYPAD_2;
    if (strEqCI(k, "NUM3")) return KEYPAD_3;
    if (strEqCI(k, "NUM4")) return KEYPAD_4;
    if (strEqCI(k, "NUM5")) return KEYPAD_5;
    if (strEqCI(k, "NUM6")) return KEYPAD_6;
    if (strEqCI(k, "NUM7")) return KEYPAD_7;
    if (strEqCI(k, "NUM8")) return KEYPAD_8;
    if (strEqCI(k, "NUM9")) return KEYPAD_9;
    if (strEqCI(k, "NUM_ENTER"))  return KEYPAD_ENTER;
    if (strEqCI(k, "NUM_PLUS"))   return KEYPAD_PLUS;
    if (strEqCI(k, "NUM_MINUS"))  return KEYPAD_MINUS;
    if (strEqCI(k, "NUM_STAR"))   return KEYPAD_ASTERIX;
    if (strEqCI(k, "NUM_SLASH"))  return KEYPAD_SLASH;
    if (strEqCI(k, "NUM_DOT"))    return KEYPAD_PERIOD;

    // ── Single ASCII character ─────────────────────────────────────
    if (k[1] == '\0') return (int)(uint8_t)k[0];

    // Unknown
    Serial.print("[WARN] Unknown key: "); Serial.println(k);
    return 0;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  SCRIPT LOADING  ░░
// ─────────────────────────────────────────────────────────────────────

void freeScript() {
    for (int i = 0; i < totalLines; i++) {
        if (scriptLines[i]) { free(scriptLines[i]); scriptLines[i] = nullptr; }
    }
    totalLines = 0;
}

bool loadFromSD() {
    if (!SD.begin(SD_CS_PIN)) {
        Serial.println("[SD] Init failed.");
        return false;
    }
    File f = SD.open(SCRIPT_FILE);
    if (!f) {
        Serial.print("[SD] File not found: "); Serial.println(SCRIPT_FILE);
        return false;
    }
    totalLines = 0;
    char buf[MAX_LINE_LEN];
    int  len = 0;

    while (f.available() && totalLines < MAX_LINES) {
        char c = f.read();
        if (c == '\n' || !f.available()) {
            if (!f.available() && c != '\n') buf[len++] = c;
            buf[len] = '\0';
            trimWhitespace(buf);
            if (len > 0 && strlen(buf) > 0) {
                scriptLines[totalLines] = (char*)malloc(strlen(buf) + 1);
                if (scriptLines[totalLines]) strcpy(scriptLines[totalLines], buf);
                totalLines++;
            }
            len = 0;
        } else if (c != '\r') {
            if (len < MAX_LINE_LEN - 1) buf[len++] = c;
        }
    }
    f.close();
    Serial.print("[SD] Loaded "); Serial.print(totalLines); Serial.println(" lines.");
    return totalLines > 0;
}

bool loadFromSerial() {
    Serial.println("[Serial] Send script. End with a line containing only '###'");
    totalLines = 0;
    unsigned long timeout = millis() + 30000;   // 30-second wait

    while (millis() < timeout && totalLines < MAX_LINES) {
        if (!Serial.available()) { delay(10); continue; }
        char buf[MAX_LINE_LEN];
        int  len = Serial.readBytesUntil('\n', buf, MAX_LINE_LEN - 1);
        buf[len] = '\0';
        trimWhitespace(buf);
        if (strcmp(buf, "###") == 0) break;
        if (strlen(buf) > 0) {
            scriptLines[totalLines] = (char*)malloc(strlen(buf) + 1);
            if (scriptLines[totalLines]) strcpy(scriptLines[totalLines], buf);
            totalLines++;
            timeout = millis() + 5000;   // reset timeout on each received line
        }
    }
    Serial.print("[Serial] Loaded "); Serial.print(totalLines); Serial.println(" lines.");
    return totalLines > 0;
}

// ─────────────────────────────────────────────────────────────────────
// ░░  FUNCTION EXECUTOR (helper used by both CALL and implicit calls)  ░░
// ─────────────────────────────────────────────────────────────────────

// Forward declarations
void executeLine(int lineIdx);

/*
 * callFunction() — pushes return address, runs the function body,
 * pops on END_FUNCTION or RET.  Leaves currentLine at the instruction
 * AFTER the original CALL (via the saved return address).
 */
void callFunction(const char* name, int returnToLine) {
    int dest = findFunction(name);
    if (dest < 0) {
        Serial.print("[ERR] CALL: function '"); Serial.print(name); Serial.println("' not found");
        return;
    }
    if (stackTop >= CALL_STACK_DEPTH) {
        Serial.println("[ERR] CALL: stack overflow");
        return;
    }
    callStack[stackTop++] = returnToLine;
    currentLine = dest + 1;   // start executing from line after FUNCTION declaration

    while (currentLine < totalLines && !halted) {
        char fline[MAX_LINE_LEN];
        strncpy(fline, scriptLines[currentLine], MAX_LINE_LEN - 1);
        fline[MAX_LINE_LEN - 1] = '\0';
        trimWhitespace(fline);

        if (strStartsWith(fline, "END_FUNCTION") ||
            strStartsWith(fline, "ENDFUNCTION")  ||
            strStartsWith(fline, "RET")           ||
            strStartsWith(fline, "RETURN")) {
            break;
        }

        int saved = currentLine;
        executeLine(currentLine);
        if (currentLine == saved) currentLine++;
        delay(defaultDelay);
    }

    currentLine = callStack[--stackTop];  // restore return address
    currentLine--;                         // main loop will +1 after return
}

// ─────────────────────────────────────────────────────────────────────
// ░░  MAIN INTERPRETER — executeLine()  ░░
// ─────────────────────────────────────────────────────────────────────

void executeLine(int lineIdx) {
    if (lineIdx < 0 || lineIdx >= totalLines) return;

    char line[MAX_LINE_LEN];
    strncpy(line, scriptLines[lineIdx], MAX_LINE_LEN - 1);
    line[MAX_LINE_LEN - 1] = '\0';
    trimWhitespace(line);

    int lineLen = strlen(line);
    if (lineLen == 0) return;

    // ── Skip comment / structural lines ──────────────────────────
    if (strStartsWith(line, "REM ")    || strcmp(line, "REM") == 0) return;
    if (strStartsWith(line, "#"))                                    return;
    if (strStartsWith(line, "//"))                                   return;
    if (strStartsWith(line, "LABEL "))                               return;
    if (lineLen > 1 && line[lineLen - 1] == ':')                    return;
    if (strStartsWith(line, "FUNCTION "))                            return;
    if (strStartsWith(line, "END_FUNCTION") ||
        strStartsWith(line, "ENDFUNCTION"))                          return;

    // ── Tokenise command ─────────────────────────────────────────
    char cmd[64];
    const char* rest = getToken(line, cmd, sizeof(cmd));
    while (*rest == ' ' || *rest == '\t') rest++;   // skip spaces after cmd

    // ════════════════════════════════════════════════════════════
    // 1. VARIABLES
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "SET") == 0 || strcmp(cmd, "VAR") == 0) {
        char varName[32], valTok[128];
        rest = getToken(rest, varName, sizeof(varName));
        while (*rest == ' ' || *rest == '\t' || *rest == '=') rest++;
        rest = getToken(rest, valTok, sizeof(valTok));

        int srcIdx = findVar(valTok);
        if (srcIdx >= 0) {
            // Copy from another variable
            if (variables[srcIdx].isString) setVarStr(varName, variables[srcIdx].strVal);
            else                            setVarInt(varName, variables[srcIdx].intVal);
        } else if (valTok[0] == '"') {
            setVarStr(varName, valTok);      // quoted literal (quotes stripped by getToken)
        } else {
            // Try integer; if non-numeric treat as string
            bool isNum = true;
            int  start = (valTok[0] == '-') ? 1 : 0;
            if (valTok[start] == '\0') isNum = false;
            for (int i = start; valTok[i]; i++)
                if (!isdigit((uint8_t)valTok[i])) { isNum = false; break; }
            if (isNum) setVarInt(varName, atol(valTok));
            else       setVarStr(varName, valTok);
        }
        return;
    }

    if (strcmp(cmd, "CLR") == 0) {
        char varName[32];
        getToken(rest, varName, sizeof(varName));
        setVarInt(varName, 0);
        return;
    }

    // GET is an alias for PRINT a variable (handled below in PRINT section)

    // ════════════════════════════════════════════════════════════
    // 2. ARITHMETIC
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "ADD") == 0) {
        char n[32], v[64];
        rest = getToken(rest, n, sizeof(n));
        getToken(rest, v, sizeof(v));
        setVarInt(n, getVarInt(n) + resolveValue(v));
        return;
    }
    if (strcmp(cmd, "SUB") == 0) {
        char n[32], v[64];
        rest = getToken(rest, n, sizeof(n));
        getToken(rest, v, sizeof(v));
        setVarInt(n, getVarInt(n) - resolveValue(v));
        return;
    }
    if (strcmp(cmd, "MUL") == 0) {
        char n[32], v[64];
        rest = getToken(rest, n, sizeof(n));
        getToken(rest, v, sizeof(v));
        setVarInt(n, getVarInt(n) * resolveValue(v));
        return;
    }
    if (strcmp(cmd, "DIV") == 0) {
        char n[32], v[64];
        rest = getToken(rest, n, sizeof(n));
        getToken(rest, v, sizeof(v));
        int32_t d = resolveValue(v);
        if (d == 0) { Serial.println("[ERR] DIV by zero"); return; }
        setVarInt(n, getVarInt(n) / d);
        return;
    }
    if (strcmp(cmd, "INC") == 0) {
        char n[32]; getToken(rest, n, sizeof(n));
        setVarInt(n, getVarInt(n) + 1);
        return;
    }
    if (strcmp(cmd, "DEC") == 0) {
        char n[32]; getToken(rest, n, sizeof(n));
        setVarInt(n, getVarInt(n) - 1);
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 3. COMPARISON
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "CMP") == 0) {
        char t1[64], t2[64];
        rest = getToken(rest, t1, sizeof(t1));
        getToken(rest, t2, sizeof(t2));
        int32_t v1 = resolveValue(t1);
        int32_t v2 = resolveValue(t2);
        // Result accessible via CMP_RESULT variable
        int32_t result = (v1 < v2) ? -1 : (v1 > v2) ? 1 : 0;
        setVarInt("CMP_RESULT", result);
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 4. CONTROL FLOW — IF
    //    Syntax:  IF <tok1> <op> <tok2> JMP <label>
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "IF") == 0) {
        char t1[64], op[8], t2[64], jmpKw[8], labelName[64];
        rest = getToken(rest, t1,        sizeof(t1));
        rest = getToken(rest, op,        sizeof(op));
        rest = getToken(rest, t2,        sizeof(t2));
        rest = getToken(rest, jmpKw,     sizeof(jmpKw));
        rest = getToken(rest, labelName, sizeof(labelName));

        int32_t v1 = resolveValue(t1);
        int32_t v2 = resolveValue(t2);
        bool cond  = false;

        if      (strcmp(op, "==") == 0) cond = (v1 == v2);
        else if (strcmp(op, "!=") == 0) cond = (v1 != v2);
        else if (strcmp(op, ">")  == 0) cond = (v1 >  v2);
        else if (strcmp(op, "<")  == 0) cond = (v1 <  v2);
        else if (strcmp(op, ">=") == 0) cond = (v1 >= v2);
        else if (strcmp(op, "<=") == 0) cond = (v1 <= v2);
        else {
            Serial.print("[ERR] IF: unknown op '"); Serial.print(op); Serial.println("'");
        }

        if (cond) {
            int dest = findLabel(labelName);
            if (dest >= 0) currentLine = dest;
            else { Serial.print("[ERR] IF JMP: label not found: "); Serial.println(labelName); }
        }
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 4b. CONTROL FLOW — JMP / GOTO
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "JMP")    == 0 ||
        strcmp(cmd, "GOTO")   == 0 ||
        strcmp(cmd, "JUMPTO") == 0) {
        char labelName[64];
        getToken(rest, labelName, sizeof(labelName));
        int dest = findLabel(labelName);
        if (dest >= 0) currentLine = dest;
        else { Serial.print("[ERR] JMP: label not found: "); Serial.println(labelName); }
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 4c. CALL / RET / HALT
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "CALL") == 0) {
        char fname[64];
        getToken(rest, fname, sizeof(fname));
        stripParens(fname);
        callFunction(fname, currentLine + 1);
        return;
    }

    if (strcmp(cmd, "RET")    == 0 ||
        strcmp(cmd, "RETURN") == 0) {
        if (stackTop > 0) {
            currentLine = callStack[--stackTop];
            currentLine--;   // main loop will +1
        } else {
            halted = true;   // bare RET at top level → stop
        }
        return;
    }

    if (strcmp(cmd, "HALT") == 0 ||
        strcmp(cmd, "STOP") == 0) {
        halted = true;
        Serial.println("[DSL] HALT");
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 5. LOOP
    //    LOOP <counter_var> <label>
    //    Decrements counter; jumps if counter != 0
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "LOOP") == 0) {
        char varName[32], labelName[64];
        rest = getToken(rest, varName,   sizeof(varName));
        rest = getToken(rest, labelName, sizeof(labelName));
        int32_t val = getVarInt(varName) - 1;
        setVarInt(varName, val);
        if (val != 0) {
            int dest = findLabel(labelName);
            if (dest >= 0) currentLine = dest;
            else { Serial.print("[ERR] LOOP: label not found: "); Serial.println(labelName); }
        }
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 6. MEMORY
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "STORE") == 0) {
        char addrTok[32], varName[32];
        rest = getToken(rest, addrTok,  sizeof(addrTok));
        rest = getToken(rest, varName,  sizeof(varName));
        int addr = (int)resolveValue(addrTok);
        if (addr >= 0 && addr < MAX_MEMORY) memArray[addr] = getVarInt(varName);
        else Serial.println("[ERR] STORE: address out of range");
        return;
    }

    if (strcmp(cmd, "LOAD") == 0) {
        char varName[32], addrTok[32];
        rest = getToken(rest, varName,  sizeof(varName));
        rest = getToken(rest, addrTok,  sizeof(addrTok));
        int addr = (int)resolveValue(addrTok);
        if (addr >= 0 && addr < MAX_MEMORY) setVarInt(varName, memArray[addr]);
        else Serial.println("[ERR] LOAD: address out of range");
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 7. TIMING
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "DELAY") == 0 || strcmp(cmd, "WAIT") == 0) {
        char tok[32]; getToken(rest, tok, sizeof(tok));
        delay((unsigned long)resolveValue(tok));
        return;
    }

    if (strcmp(cmd, "DEFAULT_DELAY") == 0) {
        char tok[32]; getToken(rest, tok, sizeof(tok));
        defaultDelay = (int)resolveValue(tok);
        Serial.print("[DSL] Default delay set to "); Serial.println(defaultDelay);
        return;
    }

    if (strcmp(cmd, "WAIT_READY") == 0) {
        Serial.println("[DSL] WAIT_READY: pausing 1000ms");
        delay(1000);
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 8. DEBUG / PRINT
    // ════════════════════════════════════════════════════════════

    if (strcmp(cmd, "PRINT") == 0 ||
        strcmp(cmd, "ECHO")  == 0 ||
        strcmp(cmd, "GET")   == 0) {

        if (rest[0] == '"') {
            char buf[256]; getToken(rest, buf, sizeof(buf));
            Serial.println(buf);
        } else {
            char tok[64]; getToken(rest, tok, sizeof(tok));
            int idx = findVar(tok);
            if (idx >= 0) {
                if (variables[idx].isString) Serial.println(variables[idx].strVal);
                else                         Serial.println(variables[idx].intVal);
            } else {
                // Print literal text
                Serial.println(tok);
            }
        }
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 9. HID — KEY COMMANDS
    // ════════════════════════════════════════════════════════════

    // PRESS <key>  — tap once
    if (strcmp(cmd, "PRESS") == 0) {
        char tok[32]; getToken(rest, tok, sizeof(tok));
        int k = resolveKey(tok);
        if (k) {
            Keyboard.press(k);
            delay(40 + random(20));
            Keyboard.release(k);
        }
        return;
    }

    // HOLD <key>  — hold without releasing
    if (strcmp(cmd, "HOLD") == 0) {
        char tok[32]; getToken(rest, tok, sizeof(tok));
        int k = resolveKey(tok);
        if (k) Keyboard.press(k);
        return;
    }

    // RELEASE <key>  — release a held key
    if (strcmp(cmd, "RELEASE") == 0) {
        char tok[32]; getToken(rest, tok, sizeof(tok));
        int k = resolveKey(tok);
        if (k) Keyboard.release(k);
        return;
    }

    // TAP <key>  — quick press+release with slight jitter
    if (strcmp(cmd, "TAP") == 0) {
        char tok[32]; getToken(rest, tok, sizeof(tok));
        int k = resolveKey(tok);
        if (k) {
            Keyboard.press(k);
            delay(25 + random(25));
            Keyboard.release(k);
        }
        return;
    }

    // RELEASEALL  — release every held key
    if (strcmp(cmd, "RELEASEALL") == 0) {
        Keyboard.releaseAll();
        return;
    }

    // COMBO <key1> [key2] [key3] ...  — press all simultaneously
    if (strcmp(cmd, "COMBO") == 0) {
        int  keys[10];
        int  kc = 0;
        const char* p = rest;
        while (*p && kc < 10) {
            char tok[32];
            p = getToken(p, tok, sizeof(tok));
            while (*p == ' ' || *p == '\t') p++;
            if (!*tok) break;
            int k = resolveKey(tok);
            if (k) {
                Keyboard.press(k);
                keys[kc++] = k;
                delay(5 + random(5));
            }
        }
        delay(60 + random(30));
        for (int i = kc - 1; i >= 0; i--) {
            Keyboard.release(keys[i]);
            delay(5 + random(5));
        }
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 10. HID — TEXT INJECTION
    // ════════════════════════════════════════════════════════════

    /*
     * resolveText() — helper lambda-like inline: extracts the string to type.
     * If arg is quoted, use literal; otherwise look up as variable, else use raw.
     */
    auto resolveText = [&](const char* arg, char* outBuf, int outLen) {
        if (arg[0] == '"') {
            getToken(arg, outBuf, outLen);
        } else {
            char varName[32];
            getToken(arg, varName, sizeof(varName));
            int idx = findVar(varName);
            if (idx >= 0 && variables[idx].isString) {
                strncpy(outBuf, variables[idx].strVal, outLen - 1);
                outBuf[outLen - 1] = '\0';
            } else if (idx >= 0) {
                // int var — convert to string
                snprintf(outBuf, outLen, "%ld", (long)variables[idx].intVal);
            } else {
                strncpy(outBuf, arg, outLen - 1);
                outBuf[outLen - 1] = '\0';
            }
        }
    };

    // SLOWPRINT "text"  — ~100–150 WPM with human-like jitter
    if (strcmp(cmd, "SLOWPRINT") == 0) {
        char buf[256];
        resolveText(rest, buf, sizeof(buf));
        for (int i = 0; buf[i]; i++) {
            Keyboard.print(buf[i]);
            delay(8 + random(7));   // 8–14 ms per char ≈ 100–150 WPM
        }
        return;
    }

    // FASTPRINT "text"  — ~300–400 WPM, still with tiny jitter
    if (strcmp(cmd, "FASTPRINT") == 0) {
        char buf[256];
        resolveText(rest, buf, sizeof(buf));
        for (int i = 0; buf[i]; i++) {
            Keyboard.print(buf[i]);
            delay(2 + random(3));   // 2–4 ms per char ≈ 300–400 WPM
        }
        return;
    }

    // STRING "text"   — same as FASTPRINT
    if (strcmp(cmd, "STRING") == 0) {
        char buf[256];
        resolveText(rest, buf, sizeof(buf));
        for (int i = 0; buf[i]; i++) {
            Keyboard.print(buf[i]);
            delay(2 + random(3));
        }
        return;
    }

    // STRINGLN "text"  — FASTPRINT + ENTER
    if (strcmp(cmd, "STRINGLN") == 0) {
        char buf[256];
        resolveText(rest, buf, sizeof(buf));
        for (int i = 0; buf[i]; i++) {
            Keyboard.print(buf[i]);
            delay(2 + random(3));
        }
        delay(30);
        Keyboard.press(KEY_RETURN);
        delay(30 + random(20));
        Keyboard.release(KEY_RETURN);
        return;
    }

    // ════════════════════════════════════════════════════════════
    // 11. IMPLICIT FUNCTION CALL  —  name()
    // ════════════════════════════════════════════════════════════

    {
        // If cmd ends with '(' or rest starts with ')', treat as function call
        char possibleFunc[64];
        strncpy(possibleFunc, cmd, sizeof(possibleFunc) - 1);
        possibleFunc[sizeof(possibleFunc) - 1] = '\0';
        stripParens(possibleFunc);

        if (findFunction(possibleFunc) >= 0) {
            callFunction(possibleFunc, currentLine + 1);
            return;
        }
    }

    // ════════════════════════════════════════════════════════════
    // 12. UNKNOWN COMMAND
    // ════════════════════════════════════════════════════════════

    Serial.print("[WARN] Unknown command at line "); Serial.print(lineIdx + 1);
    Serial.print(": "); Serial.println(cmd);
}

// ─────────────────────────────────────────────────────────────────────
// ░░  MAIN RUN LOOP  ░░
// ─────────────────────────────────────────────────────────────────────

void runScript() {
    currentLine  = 0;
    halted       = false;
    stackTop     = 0;
    varCount     = 0;
    memset(memArray, 0, sizeof(memArray));

    Serial.println("[DSL] ── Script start ──");
    unsigned long startMs = millis();

    while (currentLine < totalLines && !halted) {
        int saved = currentLine;
        executeLine(currentLine);
        if (currentLine == saved) currentLine++;   // advance only if no jump happened
        delay(defaultDelay);
    }

    unsigned long elapsed = millis() - startMs;
    Serial.print("[DSL] ── Script complete in "); Serial.print(elapsed); Serial.println(" ms ──");
}

// ─────────────────────────────────────────────────────────────────────
// ░░  ARDUINO ENTRY POINTS  ░░
// ─────────────────────────────────────────────────────────────────────

void setup() {
    Serial.begin(115200);
    delay(STARTUP_DELAY_MS);   // Give USB host time to enumerate HID device

    Serial.println("╔══════════════════════════════╗");
    Serial.println("║  TeensyDSL Interpreter v1.0  ║");
    Serial.println("╚══════════════════════════════╝");

    Keyboard.begin();
    randomSeed(analogRead(0));   // Seed RNG from floating ADC pin

    bool loaded = loadFromSD();
    if (!loaded) {
        Serial.println("[INFO] SD not available. Falling back to serial...");
        loaded = loadFromSerial();
    }

    if (!loaded) {
        Serial.println("[ERR] No script loaded. Halting.");
        return;
    }

    preScan();
    runScript();
    freeScript();
}

void loop() {
    // Idle after script completes.
    // Optional: poll Serial for a new script and re-run.
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'R' || c == 'r') {
            Serial.println("[INFO] Reload requested via serial...");
            if (loadFromSD()) {
                preScan();
                runScript();
                freeScript();
            }
        }
    }
    delay(50);
}

/*
 * ════════════════════════════════════════════════════════════════════════
 * EXAMPLE script.dsl
 * ════════════════════════════════════════════════════════════════════════
 *
 * # TeensyDSL example script
 * DEFAULT_DELAY 10
 * WAIT_READY
 *
 * # Open Run dialog (Windows)
 * COMBO GUI r
 * DELAY 800
 *
 * FASTPRINT "notepad"
 * PRESS ENTER
 * DELAY 1200
 *
 * SET greeting "Hello from TeensyDSL!"
 * SLOWPRINT greeting
 * PRESS ENTER
 *
 * # Loop example: type a number 5 times
 * SET count 5
 * LABEL loop_start
 *   IF count == 0 JMP loop_end
 *   FASTPRINT "Line: "
 *   PRINT count
 *   DEC count
 *   PRESS ENTER
 *   DELAY 200
 *   JMP loop_start
 * LABEL loop_end
 *
 * # Function example
 * CALL say_done
 * HALT
 *
 * FUNCTION say_done()
 *   DELAY 300
 *   SLOWPRINT "Script complete."
 *   PRESS ENTER
 * END_FUNCTION
 * ════════════════════════════════════════════════════════════════════════
 */
