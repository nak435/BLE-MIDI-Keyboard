/*
    BLE-MIDI-Keyboard  -  M5StickS3

    Connects a BLE HID keyboard and makes the M5StickS3 appear to a PC over
    USB-C as a "USB Audio device (MIDI class)". BLE keyboard events are
    translated to USB-MIDI (note-on / note-off / CC), turning an ordinary BLE
    keyboard into a MIDI keyboard controller.

    States:
      SCAN/LIST : BLE scan running; named devices are listed.
                  BTN-A = move cursor (one way, wraps)   BTN-B = select
                  BTN-A long press = rescan   BTN-B long press = forget all bonds
      WAIT      : Keeps an asynchronous connection request pending for the
                  bonded device, so it reconnects automatically the moment the
                  keyboard wakes and advertises again (no action needed on the
                  M5StickS3). Once the link is up it encrypts with Just Works
                  and subscribes to the HID service.
                  BTN-A = cancel -> scan   BTN-B = retry now
      PLAY      : Translates keys to USB-MIDI and shows the state on screen.
                  BTN-B = disconnect -> scan (stops auto-reconnect)
      FAILED    : BTN-A = scan   BTN-B = re-arm auto-connect for the same device

    For boards without a screen (e.g. a bare ESP32-S3 Dev board), the on-screen
    text and button guide are also printed to Serial, and the app can be driven
    from Serial input:
      a / b  ... BTN-A / BTN-B press (short)
      A / B  ... BTN-A / BTN-B long press
      ?      ... reprint the current state and guide
    The M5StickS3 physical buttons and Serial input are OR'd (either works).

    Key mapping:
      1. Piano keys (keydown = note-on, keyup = note-off; velocity variable,
         default 98)
         A  W  S  E  D  F  T  G  Y  H  U  J  K   O    L   P    ;   '
         C  C# D  D# E  F  F# G  G# A  A# B  C+1 C#+1 D+1 D#+1 E+1 F+1   (offset 0..17)
      2. Octave (handled on keydown; held notes are retriggered after a change)
         1..0,-  -> base note 0,12,24,36,48(default),60,72,84,96,108,120
                    each key spans base..base+17; notes above 127 are ignored
         Z  -> one octave down (-12, floor 0)
         X  -> one octave up   (+12, ceiling 120)
      3. Sustain pedal
         TAB -> CC64=127 on keydown, CC64=0 on keyup
      4. Velocity (handled on keydown)
         C  -> -5 (floor 1)      V  -> +5 (ceiling 127)      default 98

    Board options USB Mode = "USB-OTG (TinyUSB)" and USB CDC On Boot: Enabled
    are required.

    Note: the US-layout ' key is the : key on a JIS keyboard; both send
    keycode 0x34, so the mapping is identical.
*/

#if ARDUINO_USB_MODE
#error "Set USB Mode to 'USB-OTG (TinyUSB)'  (default_board_options: ...,USBMode=default)"
#endif

#include <M5Unified.h> // On a bare ESP32-S3 Dev board, comment this out and comment out every line that then fails to compile.

#include <NimBLEDevice.h>
#include <Preferences.h>
#include "USB.h"
#include "USBMIDI.h"

SET_USB_MIDI_DEVICE_NAME("M5StickS3 BLE-MIDI")

// ---------------- config ----------------
#define SCREEN_ROTATION     0
#define VELOCITY_DEFAULT    98      // 既定ベロシティ
#define MIDI_CHANNEL        1
#define OCTAVE_BASE_DEFAULT 48      // 既定は key '5'
#define N_PIANO_KEYS        18      // semitone offsets 0..17
#define LONGPRESS_MS        700

static const NimBLEUUID UUID_HID_SERVICE((uint16_t)0x1812);
static const NimBLEUUID UUID_PROTOCOL_MODE((uint16_t)0x2A4E);

// ---------------- USB MIDI ----------------
USBMIDI MIDI;

// ---------------- display ----------------
LGFX_Sprite canvas(&M5.Display);
struct { int W, H, headerH, footerH, bodyY, bodyH; } L;

void layout_init() {
    L.W = canvas.width();  L.H = canvas.height();
    L.headerH = 20;  L.footerH = 26;
    L.bodyY = L.headerH;  L.bodyH = L.H - L.headerH - L.footerH;
}

// ---------------- app state ----------------
enum AppState { ST_SCAN, ST_LIST, ST_WAIT, ST_PLAY, ST_FAILED };
volatile AppState g_state = ST_SCAN;
String g_statusMsg;
Preferences g_prefs;

// ---------------- scan results ----------------
struct DevInfo {
    NimBLEAddress addr;
    String        name;
    int           rssi;
    bool          isHID;
};
static const int  MAX_DEV = 24;
DevInfo           g_dev[MAX_DEV];
int               g_devN = 0, g_sel = 0, g_scrollTop = 0;
SemaphoreHandle_t g_devMutex;
volatile bool     g_scanning = false;

// ---------------- connection ----------------
NimBLEClient*  g_client = nullptr;
DevInfo        g_target;
bool           g_haveTarget = false;     // g_target is a known/bonded device
volatile bool  g_connected = false, g_disconnected = false, g_connFail = false;
volatile int   g_discReason = 0;
int            g_finalFails = 0;          // consecutive post-connect setup failures

// ---------------- MIDI translation state ----------------
// piano keys indexed by semitone offset 0..17
int16_t           g_sounding[N_PIANO_KEYS];   // MIDI note currently on for that key, or -1
int16_t           g_octaveBase = OCTAVE_BASE_DEFAULT;
int               g_velocity   = VELOCITY_DEFAULT;
bool              g_sustain     = false;   // TAB held -> CC64
uint32_t          g_midiEvents = 0;
char              g_lastMidi[24] = "-";
SemaphoreHandle_t g_midiMutex;

// ================= helpers =================
const char* keyName(uint8_t u) {
    static char b[4];
    if (u >= 0x04 && u <= 0x1D) { b[0] = 'A' + (u - 0x04); b[1] = 0; return b; }
    if (u >= 0x1E && u <= 0x26) { b[0] = '1' + (u - 0x1E); b[1] = 0; return b; }
    switch (u) {
        case 0x27: return "0";     case 0x2D: return "-";
        case 0x28: return "Enter"; case 0x29: return "Esc";
        case 0x2A: return "Bksp";  case 0x2B: return "Tab";
        case 0x2C: return "Space"; case 0x33: return ";";
        case 0x34: return "'";
        case 0xE1: return "LShift"; case 0xE5: return "RShift";
    }
    return "?";
}

// MIDI note number -> name, e.g. 60 -> "C4"
String midiNoteName(int n) {
    static const char* nm[12] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
    if (n < 0 || n > 127) return String("--");
    return String(nm[n % 12]) + String(n / 12 - 1);
}

// HID usage -> semitone offset 0..17, or -1
int8_t pianoOffset(uint8_t u) {
    switch (u) {
        case 0x04: return 0;   // A  C
        case 0x1A: return 1;   // W  C#
        case 0x16: return 2;   // S  D
        case 0x08: return 3;   // E  D#
        case 0x07: return 4;   // D  E
        case 0x09: return 5;   // F  F
        case 0x17: return 6;   // T  F#
        case 0x0A: return 7;   // G  G
        case 0x1C: return 8;   // Y  G#
        case 0x0B: return 9;   // H  A
        case 0x18: return 10;  // U  A#
        case 0x0D: return 11;  // J  B
        case 0x0E: return 12;  // K  C+1
        case 0x12: return 13;  // O  C#+1
        case 0x0F: return 14;  // L  D+1
        case 0x13: return 15;  // P  D#+1
        case 0x33: return 16;  // ;  E+1
        case 0x34: return 17;  // '  F+1
        default:   return -1;
    }
}

// digit keys -> absolute octave base note, or -1  (default is '5' = 48)
int16_t octaveBaseForKey(uint8_t u) {
    switch (u) {
        case 0x1E: return 0;    case 0x1F: return 12;   case 0x20: return 24;
        case 0x21: return 36;   case 0x22: return 48;   case 0x23: return 60;
        case 0x24: return 72;   case 0x25: return 84;   case 0x26: return 96;
        case 0x27: return 108;  case 0x2D: return 120;
        default:   return -1;
    }
}

// Z = octave down (-1), X = octave up (+1), else 0
int8_t octaveShiftForKey(uint8_t u) {
    if (u == 0x1D) return -1;   // Z
    if (u == 0x1B) return 1;    // X
    return 0;
}

// C = velocity -5, V = velocity +5, else 0
int8_t velocityDeltaForKey(uint8_t u) {
    if (u == 0x06) return -5;   // C
    if (u == 0x19) return 5;    // V
    return 0;
}

// ================= MIDI actions (called from BLE task) =================
void midiAllOff() {
    xSemaphoreTake(g_midiMutex, portMAX_DELAY);
    for (int o = 0; o < N_PIANO_KEYS; o++) {
        if (g_sounding[o] >= 0) { MIDI.noteOff(g_sounding[o], 0, MIDI_CHANNEL); g_sounding[o] = -1; }
    }
    if (g_sustain) { MIDI.controlChange(64, 0, MIDI_CHANNEL); g_sustain = false; }
    strcpy(g_lastMidi, "all off");
    xSemaphoreGive(g_midiMutex);
}

void midiResetState() {
    xSemaphoreTake(g_midiMutex, portMAX_DELAY);
    for (int o = 0; o < N_PIANO_KEYS; o++) g_sounding[o] = -1;
    g_octaveBase = OCTAVE_BASE_DEFAULT;
    g_velocity   = VELOCITY_DEFAULT;
    g_sustain    = false;
    g_midiEvents = 0;
    strcpy(g_lastMidi, "-");
    xSemaphoreGive(g_midiMutex);
}

void midiHandleKey(bool down, uint8_t usage) {
    int8_t  off  = pianoOffset(usage);
    int16_t absB = octaveBaseForKey(usage);
    int8_t  oshf = octaveShiftForKey(usage);
    int8_t  vdlt = velocityDeltaForKey(usage);
    bool    tab  = (usage == 0x2B);
    if (off < 0 && absB < 0 && oshf == 0 && vdlt == 0 && !tab) return;

    xSemaphoreTake(g_midiMutex, portMAX_DELAY);

    if (off >= 0) {         // ---- 鍵盤 ----
        if (down) {
            if (g_sounding[off] < 0) {
                int n = g_octaveBase + off;
                if (n <= 127) {
                    MIDI.noteOn(n, g_velocity, MIDI_CHANNEL);
                    g_sounding[off] = n;
                    snprintf(g_lastMidi, sizeof(g_lastMidi), "ON  %s %d",
                             midiNoteName(n).c_str(), n);
                    g_midiEvents++;
                }
            }
        } else {
            if (g_sounding[off] >= 0) {
                int n = g_sounding[off];
                MIDI.noteOff(n, 0, MIDI_CHANNEL);
                g_sounding[off] = -1;
                snprintf(g_lastMidi, sizeof(g_lastMidi), "OFF %s %d",
                         midiNoteName(n).c_str(), n);
                g_midiEvents++;
            }
        }
    } else if (absB >= 0 || oshf != 0) {        // ---- オクターブ変更 ----
        if (down) {
            int16_t nb;
            if (absB >= 0) nb = absB;
            else {
                nb = g_octaveBase + (oshf < 0 ? -12 : 12);
                if (nb < 0)   nb = 0;           // Z: 下限 0
                if (nb > 120) nb = 120;         // X: 上限 120
            }
            if (nb != g_octaveBase) {
                g_octaveBase = nb;              // オクターブを反映してから鳴らし直す
                for (int o = 0; o < N_PIANO_KEYS; o++) {
                    if (g_sounding[o] >= 0) {
                        MIDI.noteOff(g_sounding[o], 0, MIDI_CHANNEL);
                        int nn = nb + o;
                        if (nn <= 127) { MIDI.noteOn(nn, g_velocity, MIDI_CHANNEL); g_sounding[o] = nn; }
                        else           { g_sounding[o] = -1; }
                    }
                }
                snprintf(g_lastMidi, sizeof(g_lastMidi), "OCT %s (%d)",
                         midiNoteName(nb).c_str(), nb);
                g_midiEvents++;
            }
        }
    } else if (tab) {                           // ---- サステイン (TAB) ----
        if (down && !g_sustain) {
            g_sustain = true;  MIDI.controlChange(64, 127, MIDI_CHANNEL);
            strcpy(g_lastMidi, "SUSTAIN on");  g_midiEvents++;
        } else if (!down && g_sustain) {
            g_sustain = false; MIDI.controlChange(64, 0, MIDI_CHANNEL);
            strcpy(g_lastMidi, "SUSTAIN off"); g_midiEvents++;
        }
    } else if (down && vdlt != 0) {             // ---- ベロシティ (C / V) ----
        int v = g_velocity + vdlt;
        if (v < 1)   v = 1;
        if (v > 127) v = 127;
        g_velocity = v;
        snprintf(g_lastMidi, sizeof(g_lastMidi), "VEL %d", g_velocity);
        g_midiEvents++;
    }

    xSemaphoreGive(g_midiMutex);
}

// ================= BLE callbacks =================
class ScanCB : public NimBLEScanCallbacks {
    void onResult(const NimBLEAdvertisedDevice* d) override {
        bool hid = d->isAdvertisingService(UUID_HID_SERVICE);
        if ((d->getAppearance() & 0xFFC0) == 0x03C0) hid = true;
        String name = d->getName().c_str();
        NimBLEAddress a = d->getAddress();

        xSemaphoreTake(g_devMutex, portMAX_DELAY);
        int idx = -1;
        for (int i = 0; i < g_devN; i++)
            if (g_dev[i].addr == a) { idx = i; break; }
        if (idx < 0 && name.length() && g_devN < MAX_DEV) idx = g_devN++;  // named only
        if (idx >= 0) {
            g_dev[idx].addr  = a;
            g_dev[idx].rssi  = d->getRSSI();
            g_dev[idx].isHID = g_dev[idx].isHID || hid;
            if (name.length()) g_dev[idx].name = name;
        }
        xSemaphoreGive(g_devMutex);
    }
    void onScanEnd(const NimBLEScanResults&, int) override { g_scanning = false; }
};

class ClientCB : public NimBLEClientCallbacks {
    void onConnect(NimBLEClient*) override { g_connected = true; }
    void onDisconnect(NimBLEClient*, int reason) override {
        g_connected = false; g_disconnected = true; g_discReason = reason;
        Serial.printf("BLE disconnected, reason=%d\n", reason);
    }
    void onConnectFail(NimBLEClient*, int reason) override {
        g_connFail = true; g_discReason = reason;
        Serial.printf("BLE connect fail, reason=%d\n", reason);
    }
    void onPassKeyEntry(NimBLEConnInfo& ci) override { NimBLEDevice::injectPassKey(ci, 0); }
    void onConfirmPasskey(NimBLEConnInfo& ci, uint32_t) override {
        NimBLEDevice::injectConfirmPasskey(ci, true);
    }
    void onAuthenticationComplete(NimBLEConnInfo& ci) override {
        Serial.printf("Auth: bonded=%s encrypted=%s\n",
                      ci.isBonded() ? "yes" : "no", ci.isEncrypted() ? "yes" : "no");
    }
};

static ScanCB   g_scanCB;
static ClientCB g_clientCB;

// ================= HID notification =================
uint8_t g_hidPrev[8] = {0};      // last boot report; cleared on every new connection

static bool inReport(const uint8_t* r, uint8_t k) {
    for (int i = 2; i < 8; i++) if (r[i] == k) return true;
    return false;
}

void handleKeyEvent(bool down, uint8_t usage) {
    midiHandleKey(down, usage);

    // show the resulting MIDI action for mapped keys (note / octave / sustain / velocity)
    bool mapped = pianoOffset(usage) >= 0 || octaveBaseForKey(usage) >= 0 ||
                  octaveShiftForKey(usage) != 0 || velocityDeltaForKey(usage) != 0 ||
                  usage == 0x2B;
    char eff[24] = "";
    if (mapped && (down || pianoOffset(usage) >= 0)) {
        xSemaphoreTake(g_midiMutex, portMAX_DELAY);
        strncpy(eff, g_lastMidi, sizeof(eff) - 1);
        xSemaphoreGive(g_midiMutex);
    }
    Serial.printf("[%8lu] %-4s %-6s (0x%02X)  %s\n", (unsigned long)millis(),
                  down ? "DOWN" : "UP", keyName(usage), usage, eff);
}

void onHidNotify(NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (len < 8) return;
    uint8_t cur[8];
    memcpy(cur, data, 8);

    for (int b = 0; b < 8; b++) {                       // modifier bits
        bool nowb = cur[0] & (1 << b), wasb = g_hidPrev[0] & (1 << b);
        if (nowb && !wasb) handleKeyEvent(true,  0xE0 + b);
        if (!nowb && wasb) handleKeyEvent(false, 0xE0 + b);
    }
    for (int i = 2; i < 8; i++)                          // new key presses
        if (cur[i] >= 0x04 && !inReport(g_hidPrev, cur[i])) handleKeyEvent(true, cur[i]);
    for (int i = 2; i < 8; i++)                          // releases
        if (g_hidPrev[i] >= 0x04 && !inReport(cur, g_hidPrev[i])) handleKeyEvent(false, g_hidPrev[i]);

    memcpy(g_hidPrev, cur, 8);
}

// ================= connection =================
// 非同期接続を「タイムアウト無限」で開始する。コントローラは相手が
// アドバタイズを再開するまで initiating 状態を保持し、目覚めた瞬間に
// 自動で接続して onConnect を呼ぶ（= ESP32-S3 側の再接続操作は不要）。
bool beginConnect() {
    if (g_client) { NimBLEDevice::deleteClient(g_client); g_client = nullptr; }
    g_connected = g_disconnected = g_connFail = false;

    g_client = NimBLEDevice::createClient();
    g_client->setClientCallbacks(&g_clientCB, false);
    g_client->setConnectionParams(12, 12, 0, 200);
    g_client->setConnectTimeout(0x7FFFFFFF);          // BLE_HS_FOREVER: 期限なし

    Serial.printf("auto-connect armed for %s [%s]\n",
                  g_target.name.c_str(), g_target.addr.toString().c_str());
    if (!g_client->connect(g_target.addr, true, /*asyncConnect=*/true, true)) {
        g_statusMsg = "connect() start failed";
        NimBLEDevice::deleteClient(g_client);
        g_client = nullptr;
        return false;
    }
    return true;
}

// リンク確立後（onConnect 済み）に呼ぶ: 暗号化 + サービス探索 + 購読。ブロッキング。
bool finishConnect() {
    if (!g_client || !g_client->isConnected()) { g_statusMsg = "link dropped";   return false; }
    if (!g_client->secureConnection())         { g_statusMsg = "pairing failed"; return false; }

    NimBLERemoteService* svc = g_client->getService(UUID_HID_SERVICE);
    if (!svc) { g_statusMsg = "no HID service (0x1812)"; return false; }

    NimBLERemoteCharacteristic* pm = svc->getCharacteristic(UUID_PROTOCOL_MODE);
    if (pm && pm->canWriteNoResponse()) { uint8_t boot = 0; pm->writeValue(&boot, 1, false); }

    // clear translation state BEFORE subscribing so a report that arrives the
    // instant we subscribe is not wiped afterwards (would leave a stuck note)
    memset(g_hidPrev, 0, sizeof(g_hidPrev));
    midiResetState();

    int subs = 0;
    for (auto* c : svc->getCharacteristics(true)) {
        if ((c->canNotify() || c->canIndicate()) && c->subscribe(true, onHidNotify, true)) {
            subs++;
            Serial.printf("subscribed %s\n", c->getUUID().toString().c_str());
        }
    }
    if (subs == 0) { g_statusMsg = "no notify characteristic"; return false; }

    // remember this device so we can auto-reconnect without re-pairing next time.
    // boot reads these NVS keys (not getNumBonds()) to decide whether to auto-connect.
    g_haveTarget = true;
    g_prefs.putString("addr", String(g_target.addr.toString().c_str()));
    g_prefs.putUChar("atype", g_target.addr.getType());
    if (g_target.name.length()) g_prefs.putString("name", g_target.name);
    return true;
}

// 接続待機/接続を中止してクライアントを破棄する
void abortConnect() {
    if (g_client) {
        g_client->cancelConnect();
        NimBLEDevice::deleteClient(g_client);
        g_client = nullptr;
    }
}

void startScan() {
    xSemaphoreTake(g_devMutex, portMAX_DELAY);
    g_devN = 0; g_sel = 0; g_scrollTop = 0;
    xSemaphoreGive(g_devMutex);

    NimBLEScan* s = NimBLEDevice::getScan();
    s->clearResults();
    s->setScanCallbacks(&g_scanCB, false);
    s->setActiveScan(true);
    s->setInterval(60);
    s->setWindow(45);
    s->setMaxResults(0);
    g_scanning = true;
    s->start(0, false);
    g_state = ST_SCAN;
    g_statusMsg = "";
}

// forget the remembered keyboard and go back to scanning. The saved target in
// NVS (g_prefs) is what boot uses, so removing it is authoritative even if the
// NimBLE bond store misbehaves; deleteAllBonds() is best-effort on top.
void forgetAll() {
    abortConnect();
    NimBLEDevice::getScan()->stop();
    int before = NimBLEDevice::getNumBonds();
    bool ok    = NimBLEDevice::deleteAllBonds();
    int after  = NimBLEDevice::getNumBonds();
    g_prefs.remove("name");
    g_prefs.remove("addr");
    g_prefs.remove("atype");
    g_haveTarget = false;
    Serial.printf("forget: deleteAllBonds()=%s  bonds %d -> %d ; NVS target cleared\n",
                  ok ? "ok" : "FAIL", before, after);
    startScan();
}

// ================= UI =================
void drawHeader(const char* title, uint16_t col = YELLOW) {
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(col, BLACK);
    canvas.drawString(title, 2, 3);
    int mv = M5.Power.getBatteryVoltage();
    if (mv > 1000) {
        canvas.setTextDatum(top_right);
        canvas.setTextColor(WHITE, BLACK);
        canvas.drawString(String(mv / 1000.0f, 2) + "V", L.W - 2, 3);
    }
    canvas.drawFastHLine(0, L.headerH - 2, L.W, ORANGE);
}

void drawFooter(const char* txt) {
    int fy = L.H - L.footerH;
    canvas.drawFastHLine(0, fy, L.W, ORANGE);
    canvas.setTextSize(1);
    canvas.setTextDatum(top_left);
    canvas.setTextColor(YELLOW, BLACK);
    canvas.drawString(txt, 2, fy + 4);
}

void drawList() {
    canvas.clear(BLACK);
    char h[32];
    snprintf(h, sizeof(h), "Keyboards %s", g_scanning ? "(scan..)" : "");
    drawHeader(h);

    xSemaphoreTake(g_devMutex, portMAX_DELAY);
    int n = g_devN;
    DevInfo snap[MAX_DEV];
    for (int i = 0; i < n; i++) snap[i] = g_dev[i];
    if (g_sel >= n) g_sel = n ? n - 1 : 0;
    xSemaphoreGive(g_devMutex);

    const int rowH = 22;
    int visible = L.bodyH / rowH;
    if (g_sel < g_scrollTop) g_scrollTop = g_sel;
    if (g_sel >= g_scrollTop + visible) g_scrollTop = g_sel - visible + 1;

    if (n == 0) {
        canvas.setTextDatum(middle_center);
        canvas.setTextColor(DARKGREY, BLACK);
        canvas.drawString("searching...", L.W / 2, L.bodyY + L.bodyH / 2);
    }
    for (int i = 0; i < visible && (g_scrollTop + i) < n; i++) {
        int di = g_scrollTop + i, y = L.bodyY + i * rowH;
        bool cur = (di == g_sel);
        uint16_t bg = cur ? canvas.color565(0, 70, 0) : (uint16_t)BLACK;
        canvas.fillRect(0, y, L.W, rowH, bg);
        if (cur) canvas.drawRect(0, y, L.W, rowH, GREEN);

        String nm = snap[di].name;
        if (nm.length() == 0) nm = snap[di].addr.toString().c_str();
        canvas.setTextDatum(top_left);
        canvas.setTextColor(snap[di].isHID ? GREEN : WHITE, bg);
        while (nm.length() > 3 && canvas.textWidth(nm) > L.W - 34) nm.remove(nm.length() - 1);
        canvas.drawString(nm, 3, y + 2);
        canvas.setTextColor(snap[di].isHID ? GREEN : DARKGREY, bg);
        canvas.drawString(snap[di].isHID ? "HID" : "   ", 3, y + 12);
        canvas.setTextDatum(top_right);
        canvas.setTextColor(DARKGREY, bg);
        canvas.drawString(String(snap[di].rssi) + "dBm", L.W - 3, y + 12);
    }
    drawFooter("A:next  B:select");
}

void drawWait() {
    canvas.clear(BLACK);
    drawHeader(g_connected ? "Linking..." : "Waiting", CYAN);
    canvas.setTextDatum(middle_center);
    String nm = g_target.name.length() ? g_target.name : String(g_target.addr.toString().c_str());
    while (nm.length() > 3 && canvas.textWidth(nm) > L.W - 8) nm.remove(nm.length() - 1);
    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("auto-connect to", L.W / 2, L.bodyY + L.bodyH / 2 - 20);
    canvas.setTextColor(CYAN, BLACK);
    canvas.drawString(nm, L.W / 2, L.bodyY + L.bodyH / 2 - 4);
    canvas.setTextColor(DARKGREY, BLACK);
    canvas.drawString("wake the keyboard", L.W / 2, L.bodyY + L.bodyH / 2 + 16);
    drawFooter("A:cancel  B:retry");
}

void drawPlay() {
    xSemaphoreTake(g_midiMutex, portMAX_DELAY);
    int16_t base = g_octaveBase;
    bool    sus  = g_sustain;
    int     vel  = g_velocity;
    uint32_t ev  = g_midiEvents;
    char last[24]; strncpy(last, g_lastMidi, sizeof(last));
    int16_t snd[N_PIANO_KEYS]; memcpy(snd, g_sounding, sizeof(snd));
    xSemaphoreGive(g_midiMutex);

    canvas.clear(BLACK);
    drawHeader("BLE-MIDI", GREEN);

    int y = L.bodyY + 3;
    canvas.setTextDatum(top_left);
    canvas.setTextSize(1);

    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("octave base", 3, y); y += 11;
    canvas.setTextSize(2);
    canvas.setTextColor(CYAN, BLACK);
    canvas.drawString(midiNoteName(base) + "  (" + String(base) + ")", 6, y); y += 20;

    canvas.setTextSize(1);
    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("sustain:", 3, y);
    canvas.setTextColor(sus ? GREEN : DARKGREY, BLACK);
    canvas.drawString(sus ? "ON" : "off", 60, y); y += 12;
    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("velocity:", 3, y);
    canvas.setTextColor(CYAN, BLACK);
    canvas.drawString(String(vel), 60, y); y += 14;

    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("notes:", 3, y); y += 11;
    String held;
    for (int o = 0; o < N_PIANO_KEYS; o++) if (snd[o] >= 0) { held += midiNoteName(snd[o]); held += " "; }
    canvas.setTextColor(GREEN, BLACK);
    canvas.drawString(held.length() ? held : String("-"), 6, y); y += 15;

    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("last:", 3, y); y += 11;
    canvas.setTextColor(ORANGE, BLACK);
    canvas.drawString(last, 6, y); y += 14;
    canvas.setTextColor(DARKGREY, BLACK);
    canvas.drawString("events " + String(ev), 3, y);

    drawFooter("B: disconnect");
}

void drawFailed() {
    canvas.clear(BLACK);
    drawHeader("Disconnected", RED);
    canvas.setTextDatum(middle_center);
    canvas.setTextColor(RED, BLACK);
    canvas.drawString(g_statusMsg.length() ? g_statusMsg : String("connection lost"),
                      L.W / 2, L.bodyY + L.bodyH / 2 - 8);
    canvas.setTextColor(WHITE, BLACK);
    canvas.drawString("reason " + String(g_discReason), L.W / 2, L.bodyY + L.bodyH / 2 + 8);
    drawFooter(g_haveTarget ? "A:scan   B:reconnect" : "A: back to scan");
}

// ================= Serial mirror / headless control =================
// 画面を持たない ESP32-S3 Dev などのために、画面の案内文とボタン A/B の
// ガイドを Serial にも出力する。さらに Serial 入力でも操作できる:
//   a / b  = ボタン A / B のクリック
//   A / B  = ボタン A / B の長押し
//   ?      = 現在の状態と操作ガイドを再表示

volatile bool g_sClickA = false, g_sClickB = false, g_sHoldA = false, g_sHoldB = false;

void logDeviceList() {
    xSemaphoreTake(g_devMutex, portMAX_DELAY);
    int n = g_devN, sel = g_sel;
    DevInfo snap[MAX_DEV];
    for (int i = 0; i < n; i++) snap[i] = g_dev[i];
    xSemaphoreGive(g_devMutex);

    Serial.printf("-- devices (%d) %s --\n", n, g_scanning ? "[scanning]" : "");
    for (int i = 0; i < n; i++)
        Serial.printf("  %c [%d] %-20s %-3s %ddBm\n",
                      i == sel ? '>' : ' ', i,
                      snap[i].name.length() ? snap[i].name.c_str()
                                            : snap[i].addr.toString().c_str(),
                      snap[i].isHID ? "HID" : "", snap[i].rssi);
    if (n == 0) Serial.println("  (none yet - press a key on the keyboard to wake it)");
}

void logGuide() {
    Serial.println();
    Serial.println("========================================");
    switch (g_state) {
    case ST_SCAN:
    case ST_LIST:
        Serial.println("[SCAN] BLE keyboards - named devices only");
        Serial.println("  a  BTN-A press      = move cursor (wraps)");
        Serial.println("  b  BTN-B press      = select");
        Serial.println("  A  BTN-A long press = rescan");
        Serial.println("  B  BTN-B long press = forget all bonds");
        break;
    case ST_WAIT:
        Serial.printf ("[WAIT] auto-connect armed for %s  [%s]\n",
                       g_target.name.c_str(), g_target.addr.toString().c_str());
        Serial.println("  connects automatically as soon as the keyboard wakes up");
        Serial.println("  (no action needed - just use the keyboard)");
        Serial.println("  a  BTN-A press      = cancel -> scan");
        Serial.println("  b  BTN-B press      = retry now");
        Serial.println("  B  BTN-B long press = forget this keyboard");
        break;
    case ST_PLAY: {
        xSemaphoreTake(g_midiMutex, portMAX_DELAY);
        int b = g_octaveBase, v = g_velocity; bool s = g_sustain;
        xSemaphoreGive(g_midiMutex);
        Serial.println("[PLAY] translating BLE keys to USB-MIDI");
        Serial.printf ("  octave base = %s (%d)   velocity = %d   sustain = %s\n",
                       midiNoteName(b).c_str(), b, v, s ? "ON" : "off");
        Serial.println("  keys : A W S E D F T G Y H U J K O L P ; '  = C..F(+1 oct)");
        Serial.println("         1..0,- = octave base | Z/X = octave down/up");
        Serial.println("         TAB = sustain | C/V = velocity -/+");
        Serial.println("  b  BTN-B press      = disconnect -> scan (stops auto-reconnect)");
        break;
    }
    case ST_FAILED:
        Serial.printf ("[FAILED] %s  (reason %d)\n",
                       g_statusMsg.length() ? g_statusMsg.c_str() : "connection lost",
                       g_discReason);
        if (g_haveTarget) {
            Serial.println("  b  BTN-B press      = reconnect same device");
            Serial.println("  a  BTN-A press      = scan");
            Serial.println("  B  BTN-B long press = forget this keyboard");
        } else {
            Serial.println("  a  BTN-A press      = back to scan");
        }
        break;
    }
    Serial.println("  (serial: lower = press, UPPER = long press, ? = reprint)");
    Serial.println("========================================");
}

void pollSerialCmd() {
    while (Serial.available()) {
        switch (Serial.read()) {
            case 'a': g_sClickA = true; break;
            case 'b': g_sClickB = true; break;
            case 'A': g_sHoldA  = true; break;
            case 'B': g_sHoldB  = true; break;
            case '?':
                logGuide();
                if (g_state == ST_SCAN || g_state == ST_LIST) logDeviceList();
                break;
            default: break;
        }
    }
}

inline bool btnA()     { bool s = g_sClickA; g_sClickA = false; return M5.BtnA.wasClicked() || s; }
inline bool btnB()     { bool s = g_sClickB; g_sClickB = false; return M5.BtnB.wasClicked() || s; }
inline bool btnAHold() { bool s = g_sHoldA;  g_sHoldA  = false; return M5.BtnA.wasHold()   || s; }
inline bool btnBHold() { bool s = g_sHoldB;  g_sHoldB  = false; return M5.BtnB.wasHold()   || s; }

// ================= setup / loop =================
void setup() {
    Serial.begin(115200);
    Serial.setTxTimeoutMs(0);

    MIDI.begin();
    USB.begin();

    delay(1000);
    Serial.println("BLE-MIDI-Keyboard (M5StickS3)");

    M5.begin();
    M5.Power.setExtOutput(true);
    M5.Display.setRotation(SCREEN_ROTATION);
    M5.BtnA.setHoldThresh(LONGPRESS_MS);
    M5.BtnB.setHoldThresh(LONGPRESS_MS);

    canvas.setColorDepth(8);
    canvas.createSprite(M5.Display.width(), M5.Display.height());
    layout_init();

    g_devMutex  = xSemaphoreCreateMutex();
    g_midiMutex = xSemaphoreCreateMutex();
    midiResetState();

    NimBLEDevice::init("M5StickS3-BLEMIDI");
    NimBLEDevice::setPower(3);
    NimBLEDevice::setSecurityAuth(true, false, true);
    NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

    g_prefs.begin("blemidi", false);
    // 起動時の自動接続可否は「前回接続成功して NVS に保存した相手」の有無で決める。
    // getNumBonds() に依存しないので、フルイレース後は必ずスキャンから始まる。
    String savedAddr = g_prefs.getString("addr", String());
    Serial.printf("bonded devices: %d ; saved target: %s\n",
                  NimBLEDevice::getNumBonds(),
                  savedAddr.length() ? savedAddr.c_str() : "(none)");
    if (savedAddr.length()) {
        uint8_t atype = g_prefs.getUChar("atype", 0);
        g_target.addr  = NimBLEAddress(std::string(savedAddr.c_str()), atype);
        g_target.name  = g_prefs.getString("name", String("(saved)"));
        g_target.isHID = true;
        g_haveTarget   = true;
        g_state = ST_WAIT;
        Serial.printf("known keyboard %s [%s] - auto-connecting\n",
                      g_target.name.c_str(), g_target.addr.toString().c_str());
    } else {
        startScan();
    }
}

void loop() {
    M5.update();
    pollSerialCmd();

    static bool waitInit = false;                       // ST_WAIT: async connect armed?
    static AppState prevState = (AppState)0xFF;
    static int lastListN = -1, lastListSel = -1;
    if (g_state != prevState) {
        prevState = g_state;
        lastListN = -1; lastListSel = -1;               // force device-list reprint
        waitInit = false;                               // re-arm ST_WAIT on entry
        if (g_state == ST_WAIT) g_finalFails = 0;       // fresh attempt, clear fail count
        logGuide();
    }

    switch (g_state) {

    case ST_SCAN:
    case ST_LIST: {
        xSemaphoreTake(g_devMutex, portMAX_DELAY);
        int n = g_devN;
        xSemaphoreGive(g_devMutex);

        if (n != lastListN || g_sel != lastListSel) {   // echo the list to Serial on change
            lastListN = n; lastListSel = g_sel;
            logDeviceList();
        }

        if (btnA() && n > 0) g_sel = (g_sel + 1) % n;   // BTN-A: advance cursor (wrap)
        if (btnAHold()) startScan();                    // BTN-A hold: rescan
        if (btnBHold()) {                               // BTN-B hold: forget remembered keyboard
            forgetAll();
        } else if (btnB() && n > 0) {                   // BTN-B: select
            xSemaphoreTake(g_devMutex, portMAX_DELAY);
            g_target = g_dev[g_sel];
            xSemaphoreGive(g_devMutex);
            NimBLEDevice::getScan()->stop();
            g_scanning = false;
            Serial.printf("selected: %s [%s]\n",
                          g_target.name.c_str(), g_target.addr.toString().c_str());
            g_state = ST_WAIT;
        }
        drawList();
        break;
    }

    case ST_WAIT: {
        if (!waitInit) {                                 // arm the async (forever) connect once
            waitInit = true;
            if (!beginConnect()) { g_state = ST_FAILED; break; }
        }
        if (g_connected) {                               // link up -> pair + discover + subscribe
            if (finishConnect()) {                       // finishConnect() already reset MIDI state
                g_finalFails = 0;
                Serial.println("connected - translating keys to USB-MIDI");
                g_state = ST_PLAY;
            } else {
                Serial.printf("post-connect setup failed: %s\n", g_statusMsg.c_str());
                abortConnect();
                if (g_haveTarget && ++g_finalFails < 5) waitInit = false;  // retry
                else g_state = ST_FAILED;
            }
            break;
        }
        if (g_connFail) {                               // hard failure (addr/bond problem)
            g_connFail = false;
            abortConnect();
            g_statusMsg = "auto-connect failed";
            g_state = ST_FAILED;
            break;
        }
        if (btnBHold()) { forgetAll(); break; }               // forget remembered keyboard
        if (btnA()) { abortConnect(); startScan(); break; }   // cancel
        if (btnB()) { abortConnect(); waitInit = false; }     // retry now
        drawWait();
        break;
    }

    case ST_PLAY:
        if (g_disconnected) {                           // keyboard slept / went away
            midiAllOff();
            Serial.println("keyboard disconnected - re-arming auto-connect");
            if (g_haveTarget) { g_state = ST_WAIT; }    // auto-reconnect, no button needed
            else {
                g_statusMsg = "keyboard disconnected";
                abortConnect();
                g_state = ST_FAILED;
            }
            break;
        }
        if (btnB() && g_client) {                       // user wants out -> stop auto-reconnect
            midiAllOff();
            g_client->disconnect();
            delay(50);
            abortConnect();
            startScan();
            break;
        }
        drawPlay();
        break;

    case ST_FAILED:
        if (btnBHold()) {                               // forget remembered keyboard
            forgetAll();
        } else if (btnB() && g_haveTarget) {           // retry same device (auto-wait)
            g_state = ST_WAIT;
        } else if (btnA()) {
            startScan();
        }
        drawFailed();
        break;
    }

    canvas.pushSprite(0, 0);
    delay(15);
}
