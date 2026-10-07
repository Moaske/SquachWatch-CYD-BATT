// SquachWatch-CYD — the battery on a board that reads it through a divider.
// See board_battery.h for what the reading means, and
// docs/FREENOVE28_BATTERY.md for the wiring it was written against.
#include "board_battery.h"

#if defined(BOARD_BATT_PIN)

#include "blackbox.h"
#include "clock.h"

namespace BoardBattery {

namespace {

// Thresholds, in mV at BAT+.
//
// EXT: the BQ25185 holds SYS at 4.5 V (+-2%) with input power; a Li-ion cell
// tops out at 4.2 V. Halfway-ish, with hysteresis so ADC noise at the edge
// cannot flicker it.
const uint16_t EXT_ON_MV    = 4350;
const uint16_t EXT_OFF_MV   = 4280;
// NONE: nothing on BAT+ -- the divider pulls the pin to ground.
const uint16_t PRESENT_MV   = 2500;
// LOW and CRITICAL. The board's own 3.3 V regulator (ME6217, fed from BAT+
// through Q3) starts to drop out a little under 3.4 V at the board's load,
// and the BQ25185 disconnects the cell at 3.0 V. Between the two the ESP32
// browns out and reboots over and over; CRITICAL shuts down cleanly first.
const uint16_t LOW_MV       = 3500;
const uint16_t LOW_REARM_MV = 3600;   // a LOW is said once per discharge
const uint16_t CRITICAL_MV  = 3380;
const uint32_t LOW_HOLD_MS      = 30000;   // a WiFi burst sags the cell for a moment; a real low stays
const uint32_t CRITICAL_HOLD_MS = 15000;

const uint32_t SAMPLE_MS    = 1000;

// BOARD USB ("AC"): the battery switch off and the Freenove on its own USB.
// Then nothing is on BAT+ but the board's own charger (TP4054) and the
// 200K divider. With no cell to charge it ends its cycle at once (the 20 uA
// the divider draws is far under its termination current), the 10 uF on
// BAT+ sags through the divider to its recharge threshold in tens of
// milliseconds, and it starts again: a sawtooth of roughly 4.05-4.2 V, many
// times a second. A cell, or the BQ25185's SYS, is steady to a few tens of
// mV. So a reading near the top that keeps swinging is "no cell, board USB".
// Sampled once per loop pass (four reads averaged), judged once a second.
const uint16_t USB_SWING_ON_MV  = 120;   // the swing within one second that means sawtooth
const uint16_t USB_SWING_OFF_MV = 60;
const uint16_t USB_TOP_MV       = 4000;  // and only near the top: a cell cannot be both low and charged
const uint8_t  USB_ON_SECONDS   = 3;
const uint8_t  USB_OFF_SECONDS  = 5;
const uint32_t LOG_MS       = 600000;      // a black box sample every ten minutes, like the watch

// Estimated charge from the voltage at BAT+ *under the board's own load*
// (~100-150 mA, screen on, radios scanning), which sits a little under a
// resting cell's. 0% is where the regulator starts to give up, not where the
// cell is empty: the last few percent of a Li-ion are below 3.4 V and this
// board cannot use them anyway. Rough, and meant to be refined from a real
// discharge in the black box (BATTLOG).
const uint16_t CURVE_MV[]  = { 3400, 3500, 3600, 3680, 3730, 3770, 3810, 3860, 3930, 4010, 4100, 4150 };
const uint8_t  CURVE_PCT[] = {    0,    3,    8,   17,   27,   38,   48,   59,   70,   82,   95,  100 };

uint16_t s_mv      = 0;       // filtered
uint16_t s_rawMv   = 0;       // last sample, unfiltered
uint8_t  s_pct     = 0;       // as shown: on the cell it only goes down (see sample())
bool     s_ext     = false;
bool     s_present = false;
bool     s_lowSaid = false;
bool     s_critSaid = false;
uint32_t s_lastAt = 0, s_lowSince = 0, s_critSince = 0, s_logAt = 0;
bool     s_screenOn = true;
Event    s_event = Event::NONE;
bool     s_begun = false;
bool     s_usb   = false;     // board USB, no cell: see USB_SWING_ON_MV
uint16_t s_winMin = 0xFFFF, s_winMax = 0, s_lastSwing = 0;
uint8_t  s_usbOnRun = 0, s_usbOffRun = 0;

// One quick reading per loop pass, for the swing: four reads, not sixteen,
// so it costs well under 100 us a frame.
void fastSample() {
    uint32_t sum = 0;
    for (int i = 0; i < 4; i++) sum += analogReadMilliVolts(BOARD_BATT_PIN);
    const uint16_t mv = (uint16_t)((sum / 4) * BOARD_BATT_X1000 / 1000);
    if (mv < s_winMin) s_winMin = mv;
    if (mv > s_winMax) s_winMax = mv;
}

// Once a second: was that second's swing the board charger's sawtooth?
void judgeUsb() {
    if (s_winMax == 0) return;   // no fast samples this second
    const uint16_t swing = s_winMax - s_winMin;
    s_lastSwing = swing;
    const bool saw = !s_ext && s_winMax >= USB_TOP_MV && swing >= USB_SWING_ON_MV;
    const bool calm = s_ext || s_winMax < USB_TOP_MV - 50 || swing < USB_SWING_OFF_MV;
    if (!s_usb) {
        s_usbOnRun = saw ? (uint8_t)(s_usbOnRun + 1) : 0;
        if (s_usbOnRun >= USB_ON_SECONDS) { s_usb = true; s_usbOffRun = 0; }
    } else {
        s_usbOffRun = calm ? (uint8_t)(s_usbOffRun + 1) : 0;
        if (s_usbOffRun >= USB_OFF_SECONDS) { s_usb = false; s_usbOnRun = 0; }
    }
    s_winMin = 0xFFFF; s_winMax = 0;
}

uint16_t readMv() {
    // Sixteen reads, averaged: the ADC is noisy at the millivolt level and
    // this costs well under a millisecond.
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(BOARD_BATT_PIN);
    return (uint16_t)((sum / 16) * BOARD_BATT_X1000 / 1000);
}

uint8_t curvePct(uint16_t mv) {
    const uint8_t n = sizeof CURVE_MV / sizeof CURVE_MV[0];
    if (mv <= CURVE_MV[0]) return 0;
    for (uint8_t i = 1; i < n; i++)
        if (mv <= CURVE_MV[i])
            return (uint8_t)(CURVE_PCT[i-1] + (uint32_t)(CURVE_PCT[i] - CURVE_PCT[i-1]) * (mv - CURVE_MV[i-1]) / (CURVE_MV[i] - CURVE_MV[i-1]));
    return 100;
}

void post(Event e) {
    // CRITICAL is never overwritten by anything less urgent before main.cpp takes it.
    if (s_event == Event::FLAT) return;
    s_event = e;
}

void sample(uint32_t now, bool first) {
    const uint16_t raw = readMv();
    s_rawMv = raw;
    // A step of more than a quarter volt is the power source changing (the
    // charger's input coming or going), not noise: take it at once rather
    // than filtering towards it for ten seconds.
    if (first || (raw > s_mv ? raw - s_mv : s_mv - raw) > 250) s_mv = raw;
    else s_mv = (uint16_t)((s_mv * 3u + raw) / 4u);

    s_present = s_mv >= PRESENT_MV;
    const bool wasExt = s_ext;
    if (!s_ext && s_mv >= EXT_ON_MV) s_ext = true;
    else if (s_ext && s_mv < EXT_OFF_MV) s_ext = false;

    // The shown percent: on the cell it only falls, so the brief recovery
    // after a WiFi burst does not make it wander up and down. It is allowed
    // up again after external power (or at boot), and by a clear jump
    // (a fresh cell, say).
    const uint8_t est = curvePct(s_mv);
    if (first || wasExt || est < s_pct || est > s_pct + 10) s_pct = est;

    if (first) {
        s_logAt = now;
        return;   // no events for the reading that seeds the filter
    }

    if (s_ext != wasExt) {
        post(s_ext ? Event::EXT_ON : Event::EXT_OFF);
        s_lowSince = s_critSince = 0;
        if (s_ext) { s_lowSaid = false; s_critSaid = false; }
        noteSample(BlackBox::BATT_WHY_USB);
        return;
    }
    if (s_ext || s_usb || !s_present) { s_lowSince = s_critSince = 0; return; }

    if (s_mv >= LOW_REARM_MV) s_lowSaid = false;
    if (s_mv < LOW_MV) { if (!s_lowSince) s_lowSince = now; } else s_lowSince = 0;
    if (s_mv < CRITICAL_MV) { if (!s_critSince) s_critSince = now; } else s_critSince = 0;

    if (s_critSince && !s_critSaid && now - s_critSince >= CRITICAL_HOLD_MS) {
        s_critSaid = true;
        post(Event::FLAT);
    } else if (s_lowSince && !s_lowSaid && now - s_lowSince >= LOW_HOLD_MS) {
        s_lowSaid = true;
        post(Event::LOW_BATTERY);
    }
}

}  // namespace

void begin() {
    analogSetPinAttenuation(BOARD_BATT_PIN, ADC_11db);   // 0..~3.1 V at the pin: 4.5 V at BAT+ is 2.25 V
    const uint32_t now = millis();
    sample(now, true);
    s_lastAt = now;
    s_begun = true;
    // Booted onto a cell that is already too flat to run on: say so at once
    // rather than after the hold, or the board browns out before it gets there.
    if (s_present && !s_ext && s_mv < CRITICAL_MV) { s_critSaid = true; post(Event::FLAT); }
    Serial.printf("[batt] %s  %u mV at BAT+ (pin %d, x%u.%03u)\n",
                  !s_present ? "no battery" : s_ext ? "external power" : "on battery",
                  (unsigned)s_mv, (int)BOARD_BATT_PIN,
                  (unsigned)(BOARD_BATT_X1000 / 1000), (unsigned)(BOARD_BATT_X1000 % 1000));
    noteSample(BlackBox::BATT_WHY_BOOT);
}

void tick(uint32_t now, bool screenOn) {
    if (!s_begun) return;
    if (screenOn != s_screenOn) {
        s_screenOn = screenOn;
        noteSample(BlackBox::BATT_WHY_SCREEN);
    }
    fastSample();
    if (now - s_lastAt < SAMPLE_MS) return;
    s_lastAt = now;
    judgeUsb();
    sample(now, false);
    if (now - s_logAt >= LOG_MS) noteSample(BlackBox::BATT_WHY_TIMER);
}

Event takeEvent() {
    const Event e = s_event;
    s_event = Event::NONE;
    return e;
}

bool     present() { return s_present; }
bool     ext()     { return s_ext; }
bool     boardUsb(){ return s_usb; }
uint16_t mv()      { return s_mv; }
uint8_t  pct()     { return s_pct; }

void noteSample(uint8_t why) {
    s_logAt = millis();
    BlackBox::BattRecord r;
    memset(&r, 0, sizeof r);
    r.mv       = s_mv;
    r.pct      = s_ext ? 0 : s_pct;
    r.why      = why;
    r.epoch    = Clock::trusted() ? Clock::nowEpoch() : 0;
    r.upSec    = millis() / 1000u;
    r.cpuMhz10 = (uint8_t)(getCpuFrequencyMhz() / 10);
    if (s_ext || s_usb) r.flags |= BlackBox::BATT_USB;   // external power: the charger's input, or the board's own USB
    if (s_screenOn) r.flags |= BlackBox::BATT_SCREEN_ON;
    r.flags |= BlackBox::BATT_RADIOS_ON;             // no duty cycle on this board: always listening
    BlackBox::noteBattery(r);
    static const char* const WHY[] = { "timer", "boot", "power", "screen", "radio reset", "self-heal" };
    Serial.printf("[batt] %u mV  %s  screen %s  up %lu s  (%s)\n", (unsigned)s_mv,
                  !s_present ? "no battery" : s_ext ? "external power" : s_usb ? "board USB, no cell" : "on battery",
                  s_screenOn ? "on" : "dim", (unsigned long)r.upSec, WHY[why < 6 ? why : 0]);
}

void printNow() {
    char line[24];
    boardBatteryLine(line, sizeof line);
    Serial.printf("[batt] %s  (filtered %u mV, last raw %u mV, swing %u mV/s, %s)\n", line, (unsigned)s_mv,
                  (unsigned)s_rawMv, (unsigned)s_lastSwing,
                  !s_present ? "nothing on BAT+" : s_ext ? "external power" : s_usb ? "board USB, no cell" : "on the cell");
}

void printLog() {
    Serial.println("[battlog] newest first: boot  up(s)  epoch  mV  %  flags(ext/-/screen/radio)  cpu  why");
    BlackBox::forEachBattery([](const BlackBox::BattRecord& r, void*) {
        Serial.printf("[battlog] %u  %lu  %lu  %u  %u  %c-%c%c  %u  %u\n", (unsigned)r.boot,
                      (unsigned long)r.upSec, (unsigned long)r.epoch, (unsigned)r.mv, (unsigned)r.pct,
                      (r.flags & BlackBox::BATT_USB) ? 'E' : '-',
                      (r.flags & BlackBox::BATT_SCREEN_ON) ? 'S' : '-', (r.flags & BlackBox::BATT_RADIOS_ON) ? 'R' : '-',
                      (unsigned)r.cpuMhz10 * 10, (unsigned)r.why);
        return true;
    }, nullptr);
}

}  // namespace BoardBattery

void boardBatteryLine(char* out, size_t n) {
    using namespace BoardBattery;
    const uint16_t v = mv();
    if (!present())      snprintf(out, n, "NONE");
    else if (ext())      snprintf(out, n, "EXT POWER");
    else if (boardUsb()) snprintf(out, n, "AC");   // no cell at all: a percent would be made up
    else if (v < LOW_MV) snprintf(out, n, "LOW %u%% %u.%02uV", (unsigned)pct(), (unsigned)(v / 1000), (unsigned)(v % 1000 / 10));
    else                 snprintf(out, n, "%u%% %u.%02uV", (unsigned)pct(), (unsigned)(v / 1000), (unsigned)(v % 1000 / 10));
}

#endif  // BOARD_BATT_PIN
