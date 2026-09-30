// SquachWatch-CYD — the battery on a board that reads it through a divider.
//
// Built only when the environment defines BOARD_BATT_PIN (so far just
// [env:freenove28], the Freenove FNK0114B 2.8"). The pin sees the board's
// BAT+ through a 100K/100K divider; what sits on BAT+ decides what the
// reading means:
//
//   a bare Li-ion cell      -> the cell's voltage
//   a BQ25185 power-path    -> the cell's voltage on battery, and a steady
//   charger's SYS output       4.5 V whenever the charger has input power
//                              (charging or full). A Li-ion cell never reads
//                              above 4.2 V, so anything over ~4.35 V is
//                              "external power", not a cell.
//
// No fuel gauge and no charge-status line: a voltage, a percent estimated
// from it under the board's own load, and "external power" when the reading
// is above anything a cell can show. See docs/FREENOVE28_BATTERY.md.
#pragma once
#include <Arduino.h>

#if defined(BOARD_BATT_PIN)

// The divider's ratio, x1000. 100K/100K on the FNK0114B: 2000. A board whose
// reading is off against a multimeter can trim it with -DBOARD_BATT_X1000=...
#ifndef BOARD_BATT_X1000
#define BOARD_BATT_X1000 2000
#endif

namespace BoardBattery {

enum class Event : uint8_t {
    NONE,
    EXT_ON,     // external power arrived (the charger's SYS went to 4.5 V)
    EXT_OFF,    // back on the cell
    LOW_BATTERY,  // under LOW_MV on the cell for a while: once per discharge
    FLAT,         // under CRITICAL_MV on the cell: shut down now
};

void     begin();                                        // setup(), before anything reads it
void     tick(uint32_t now, bool screenOn);              // loop(): samples once a second
Event    takeEvent();                                    // the latest event, once

bool     present();   // something is on BAT+ at all
bool     ext();       // external power (reading above any cell)
uint16_t mv();        // the filtered reading at BAT+, in mV
uint8_t  pct();       // estimated charge, 0..100; meaningless while ext()

void     printNow();  // BATT on the console
void     printLog();  // BATTLOG on the console: the black box's samples, newest first
void     noteSample(uint8_t why);   // one sample into the black box now

}  // namespace BoardBattery

// The SYSTEM page's BATTERY row: "73% 3.92V", "EXT POWER", "LOW 4% 3.47V" or
// "NONE". Same name and shape as the Freenove S3's, so ui_settings.cpp treats
// both boards alike.
void boardBatteryLine(char* out, size_t n);

#endif  // BOARD_BATT_PIN
