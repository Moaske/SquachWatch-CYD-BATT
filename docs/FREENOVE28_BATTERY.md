# Freenove 2.8" (FNK0114B) on a battery

This fork's `[env:freenove28]` build runs SquachWatch on the **Freenove ESP32
Display 2.8"** (FNK0114B_2P8, ESP32-32E, ST7789, resistive touch) from a
single Li-ion cell, charged and protected by an **Adafruit bq25185** charger
board, with an on/off switch.

## Wiring

```
                         Adafruit bq25185 board
  Li-ion 3.7 V  ───────► JST (BAT)
  2200 mAh               USB-C / VIN 5-18 V  ◄──── charger / solar
                         EN ──┐
                         GND ─┴─ bridged: 3.3 V buck and its LED off
                         4.5V (+) ───────── SWITCH ─────────┐
                         GND  (−) ───────────────────────┐  │
                                                         │  │
                         Freenove FNK0114B               │  │
                         battery connector (MX1.25)      │  │
                           pin 1  BAT+  ◄────────────────┼──┘
                           pin 2  GND   ◄────────────────┘
```

1. **Cell → Adafruit JST.** Check the polarity against the board's + / −
   marks: cells from other shops often have the plug wired the other way.
2. **Adafruit 4.5V (+) → switch → Freenove BAT+ (pin 1).**
3. **Adafruit GND (−) → Freenove GND (pin 2).** No switch in this one.
4. **Adafruit EN → GND.** The 3.3 V output is not used; left on, it and its
   green LED drain the cell even with the switch off.
5. **Charge through the Adafruit's USB-C (or VIN).** The Freenove's own USB
   port is for flashing, with the switch **off**.

Use the **4.5V load output, not the 3V output.** It is a power-path output:
4.5 V whenever the charger has input power, the cell's own voltage otherwise.
The 3.3 V output would sit under the Freenove's own regulator, and the
battery reading would be a constant 3.3 V.

### Why it works

On the FNK0114B, BAT+ feeds the board through Q3 (SL2305) into its ME6217
3.3 V regulator, and through a 100K/100K divider into **GPIO34**. The board's
own charger (a TP4054) also sits on BAT+; it tolerates up to 11 V there and
does nothing above 4.2 V, so the 4.5 V output leaves it idle.

With the switch off the charger still fills the cell (faster, with no board
to run), and the whole thing draws a few µA.

**Keep the switch off while the Freenove is on its own USB port.** Otherwise
its TP4054 can push up to 300 mA back through the Adafruit's output into the
cell, around the bq25185's protections.

## What the firmware shows

| At BAT+ (GPIO34 × 2) | Meaning | BATTERY row (Settings → SYSTEM) |
|---|---|---|
| above 4.35 V | the charger has input power (charging or full) | `EXT POWER` |
| 3.50 – 4.2 V | on the cell | `73% 3.92V` |
| under 3.50 V (30 s) | nearly flat | `LOW 4% 3.47V`, a toast, and Squachy says so |
| under 3.38 V (15 s) | flat | BATTERY EMPTY screen, then deep sleep |
| under 2.5 V | nothing on BAT+ | `NONE` |

- **EXT POWER** is the whole story while the charger is plugged in: the
  bq25185 holds its output at 4.5 V, so the cell's own voltage is not visible
  until it is unplugged. Plugging in and unplugging show a toast.
- **The percent** is estimated from the voltage under the board's own load,
  and 0% is where the Freenove's regulator starts to give up (~3.4 V), not
  where the cell is empty. It only goes down while on the cell, so a WiFi
  burst does not make it wander.
- **The flat shutdown** happens before the regulator drops out (the ESP32
  would otherwise brown out and reboot until the bq25185 cuts the cell off at
  3.0 V). The board then sleeps, waking every five minutes to look: once the
  charger is on (or the cell reads 3.7 V or more) it boots normally. Switching
  it off and on, or the reset button, starts it at once.
- **CHARGE MODE** is hidden on this build: the charger does that job with the
  board switched off.
- **UPDATE CHECK** starts off: this build is never published on
  squachwatch.com, so there is nothing newer there to find.

### On the console (USB serial, 2000000 baud)

- `BATT` — the reading now: the BATTERY row, the filtered and raw mV.
- `BATTLOG` — every battery sample in the black box, newest first: one every
  ten minutes, at boot, and whenever the power source or the screen changes.
  A full discharge in here is what refines the percent curve.
- `RUNTIME` — how long each of the last eight boots ran.

A `[batt]` line is printed at boot and with every sample.

### Known limits

- **No charging vs. full.** The bq25185's STAT2 pin would tell the two apart
  (low while charging); wiring it to GPIO35 on the P4 connector with a 10K
  pull-up to 3.3 V would be the way, and the firmware does not read it yet.
- **On the Freenove's own USB with the switch off**, GPIO34 reads whatever
  the Freenove's TP4054 puts on an empty BAT+ (around 4.2 V), so the row shows
  a full-looking cell that is not there.
- **Calibration.** The divider is taken as exactly ×2. If a multimeter on
  BAT+ disagrees with `BATT` by more than a few tens of mV, the build flag
  `-DBOARD_BATT_X1000=` trims it (2000 = ×2.000).

## Building and flashing

Every push to the `freenove28` branch runs **Freenove 2.8 battery build** in
the Actions tab. The run's artifact, `freenove28-<version>`, holds:

| File | Where | When |
|---|---|---|
| `freenove28-FULL-merged-0x0.bin` | address `0x0` | the first install, or a rescue: **erases settings** |
| `freenove28-UPDATE-app-0x10000.bin` | address `0x10000` | every update after that: keeps settings, touch calibration and the black box |
| `parts/` | as named | bootloader, partition table and otadata, for esptool by hand |

With [Espressif's browser flasher](https://espressif.github.io/esptool-js/):
battery switch **off**, USB into the Freenove, **Connect**, add the file with
its address, **Program**.

Bluetooth and WiFi updates do not apply to this build: they need the
upstream maker's signature, and his releases have no `freenove28` image.

## Pins, for reference

From Freenove's schematic (`2.8inch_ESP32-32E_Display_Schematic.pdf`):

| What | GPIO |
|---|---|
| TFT (ST7789, HSPI) | MOSI 13, MISO 12, SCLK 14, CS 15, DC 2, backlight 21 |
| Touch (XPT2046) | CLK 25, CS 33, DIN 32, DOUT 39, IRQ 36 |
| SD card | CS 5, MOSI 23, MISO 19, SCLK 18 |
| RGB LED (common anode, lit low) | R 22, G 16, B 17 |
| Audio amplifier | in 26, enable 4 |
| Battery divider (100K/100K) | 34 |
| P4 connector | 3.3 V, **35**, (none), GND |
| P3 connector | SPI: MOSI 23, MISO 19, CLK 18, CS 27 |
| BOOT button | 0 |
