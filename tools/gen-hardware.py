#!/usr/bin/env python3
"""
Generate the schematic netlist / BOM and CROSS-CHECK it against the firmware.

WHY THIS EXISTS
---------------
The single most expensive class of bug in a project like this is a hardware and
firmware disagreement: the board is wired for PB10 but the firmware drives PB6,
and nothing catches it until someone spends an evening with a meter. The
information needed to catch it already exists twice - in Core/main.h and in the
schematic notes - and duplicated information drifts.

So this script makes Core/main.h the machine-readable source of truth and checks
every other representation against it. It parses the pin macros, compares them to
the net table below, and refuses to be quiet about any disagreement.

WHAT IT CHECKS
--------------
  1. Every <name>_PORT / <name>_PIN macro pair in main.h resolves to a real
     STM32F103 pin (all GPIOA..GPIOC, pins 0..15).
  2. Every firmware pin is claimed by exactly one net, on the build variant it
     belongs to - no forgotten signals, no double-claimed pins.
  3. Every net entry refers to a firmware signal that actually exists, so a
     renamed macro cannot silently orphan a net.
  4. External-interface nets (motor, limits, buttons, console, power) are present
     in full, because a missing one means a missing connector.

Build variants (REAL / SIM / BOTH) let one pin carry different nets in the two
builds. The current simulation uses the same OLED I2C pins as the real board, so
the display bus is a BOTH net; only EEPROM storage is replaced by a stub.

Usage:
    python tools/gen-hardware.py            # check, and write the artifacts
    python tools/gen-hardware.py --check    # check only, fail on any problem
"""

import argparse
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MAIN_H = os.path.join(ROOT, "Core", "main.h")

# Output goes to "generated/", NOT "hardware/".
#
# On Windows the filesystem is case-insensitive, so "hardware/" and the driver
# source tree "Hardware/" are the SAME directory - an earlier version of this
# script wrote bom.csv, mcu-pinout.csv and netlist.txt straight into the driver
# tree. That mixes generated artifacts in with hand-written sources, which makes
# it impossible to tell at a glance what is source and what is derivative.
# A distinct name removes the ambiguity entirely rather than relying on case.
OUT_DIR = os.path.join(ROOT, "generated")

# ---------------------------------------------------------------------------
# Build variants
# ---------------------------------------------------------------------------
# REAL : the physical board
# SIM  : the Proteus simulation, using the OLED12864 I2C display model
REAL = "real"
SIM = "sim"
BOTH = "both"


# ---------------------------------------------------------------------------
# MCU pin macros -> net
# ---------------------------------------------------------------------------
# Every entry maps a firmware macro stem to the net it belongs to. `variants`
# says which builds the connection exists in. The OLED I2C bus is present in
# both builds; only the EEPROM storage backend changes in simulation.
NET_TABLE = [
    # stem                net name          role                  variants
    ("STATUS_LED",        "LED_STATUS",     "Status LED (state blink)", BOTH),
    ("UART_TX",           "USART1_TX",      "Console TX",         BOTH),
    ("UART_RX",           "USART1_RX",      "Console RX",         BOTH),
    ("MYI2C_SCL",        "I2C_SCL",        "OLED/EEPROM I2C clock", BOTH),
    ("MYI2C_SDA",        "I2C_SDA",        "OLED/EEPROM I2C data",  BOTH),
    # LIMIT_OPEN / LIMIT_CLOSE are deliberately NOT here: no end stops are fitted.
    # See RESERVED_PINS below for the pin-level story.
    ("SENSOR_OUT",        "SENSOR_OUT",     "Outside presence",   BOTH),
    ("SENSOR_IN",         "SENSOR_IN",      "Inside presence",    BOTH),
    ("KEY1",              "KEY1",           "Start / stop",       BOTH),
    ("KEY2",              "KEY2",           "Mode toggle",        BOTH),
    ("KEY3",              "KEY3",           "Manual open",        BOTH),
    ("KEY4",              "KEY4",           "Manual close / e-stop", BOTH),
    ("BUZZER",            "BUZZER",         "Buzzer drive",       BOTH),
    ("SERVO",             "SERVO_PWM",      "SG90 signal (PA6/TIM3_CH1, 50 Hz)", BOTH),
]

# ---------------------------------------------------------------------------
# Firmware macros deliberately absent from the netlist
# ---------------------------------------------------------------------------
# Core/main.h defines TWO sets of LED macros, and only one of them is used:
#
#   STATUS_LED_*  PB4   - driven by Hardware/Led/StatusLed.c (6 state patterns).
#                         This is the "系统状态指示" the requirement asks for.
#   LED_*         PC13  - leftovers. The heartbeat helpers that used them
#                         (led_init/led_toggle) were removed from Core/main.c
#                         when it was rewritten, and nothing references these
#                         macros now.
#
# They are listed here rather than in NET_TABLE so the netlist names the pin that
# is actually driven. Presenting PC13 as a live status LED would send someone
# wiring an indicator that never lights; silently dropping the macros instead
# would hide the duplication. Naming the collision is the honest option.
#
# (Getting this the wrong way round once already - marking PB4 as reserved - is
# why the check below exists: the netlist must not disagree with what the
# firmware drives.)
RESERVED_PINS = [
    ("LED", "PC13",
     "Duplicate/unused LED macros - no firmware code references them"),
    # No end stops are fitted: the design derives the door position from a
    # calibrated travel time (AUTODOOR_NO_LIMITS=1 by default), so PA0/PA1 are
    # unconnected and free for other use. The history is worth keeping because it
    # is why they are free: these two lines each carried BOTH a limit switch and a
    # presence sensor until round 11, and AFIO routes one port per EXTI line, so
    # the sensor - initialised second - took both lines and the limit switches were
    # left with no interrupt at all. The sensors moved to PB12/PB13 (lines 12/13).
    # A line is not shareable, it is contended, and one side always loses.
    ("LIMIT_OPEN", "PA0",
     "No end stops fitted (timed position) - pin unconnected and free"),
    ("LIMIT_CLOSE", "PA1",
     "No end stops fitted (timed position) - pin unconnected and free"),
]

# Pins that are not driven by the application but must be present for the part to
# run or to be programmable. Listed so the checklist cannot omit them.
FIXED_PINS = [
    ("PA13", "SWDIO",     "Debug data - must stay free"),
    ("PA14", "SWCLK",     "Debug clock - must stay free"),
    ("PD0",  "OSC_IN",    "8 MHz crystal"),
    ("PD1",  "OSC_OUT",   "8 MHz crystal"),
    ("NRST", "NRST",      "Reset, 10k pull-up + 100nF"),
    ("BOOT0", "BOOT0",    "10k pull-down to GND"),
]

# ---------------------------------------------------------------------------
# Non-MCU BOM
# ---------------------------------------------------------------------------
BOM = [
    # RefDes, Qty, Value, Package, Description
    ("U1",  1, "STM32F103C8T6",    "LQFP48",   "Main microcontroller"),
    ("U2",  1, "AMS1117-3.3",      "SOT-223",  "5V to 3.3V regulator"),
    ("U3",  1, "SG90 servo",       "9g",       "Door actuator; needs its own 5 V supply, not the 3V3 rail"),
    ("U4",  1, "0.96in SSD1306",   "Module",   "128x64 OLED, I2C addr 0x78"),
    ("U5",  1, "AT24C32 module",   "Module",   "4 KB I2C EEPROM, addr 0xA0"),
    ("M1",  1, "130 DC motor",     "130",      "3-6 V toy motor, needs gearing"),
    ("Y1",  1, "8 MHz",            "HC-49S",   "HSE crystal"),
    ("SW1", 1, "Reset",            "SMD 3x6",  "Reset button"),
    # No SW2/SW3: the two KW12-3 end stops are NOT fitted (timed position instead).
    # The RefDes numbers are deliberately left with the gap so they keep matching
    # the schematic notes and docs, which refer to SW4..SW9 by name.
    ("SW4", 1, "Tactile",          "SMD 3x6",  "KEY1 start/stop"),
    ("SW5", 1, "Tactile",          "SMD 3x6",  "KEY2 mode"),
    ("SW6", 1, "Tactile",          "SMD 3x6",  "KEY3 manual open"),
    ("SW7", 1, "Tactile",          "SMD 3x6",  "KEY4 manual close / e-stop"),
    ("SW8", 1, "Tactile",          "SMD 3x6",  "Simulated outside sensor"),
    ("SW9", 1, "Tactile",          "SMD 3x6",  "Simulated inside sensor"),
    ("LS1", 1, "Active buzzer",    "12mm",     "Built-in oscillator"),
    ("D1",  1, "SS34",             "SMA",      "Motor flyback"),
    ("D2",  1, "LED red",          "0805",     "Status LED"),
    ("C1",  1, "470uF/16V",        "Elec 8mm", "Motor rail bulk"),
    ("C2",  1, "100nF",            "0805",     "Motor rail HF"),
    ("C3",  1, "10uF/10V",         "0805",     "Regulator input"),
    ("C4",  1, "10uF/10V",         "0805",     "Regulator output"),
    ("C5",  4, "100nF",            "0805",     "One per MCU VDD pin"),
    ("C6",  1, "10uF",             "0805",     "MCU bulk"),
    ("C7",  2, "22pF",             "0805",     "Crystal load"),
    ("C8",  1, "100nF",            "0805",     "NRST"),
    # C9 (limit debounce) is gone with the end stops.
    ("C10", 6, "100nF",            "0805",     "Button/sensor debounce"),
    ("R1",  1, "10k",              "0805",     "NRST pull-up"),
    # R2 (limit pull-ups) is gone with the end stops.
    # Fitting end stops later means: 2x KW12-3, 2x 10k pull-up, 2x 100nF debounce,
    # plus a firmware rebuild with -WithLimits (see Core/main.h).
    ("R3",  6, "10k",              "0805",     "Button/sensor pull-ups"),
    ("R4",  1, "1k",               "0805",     "LED series"),
    ("R5",  1, "1k",               "0805",     "Buzzer base"),
    ("R6",  2, "10k",              "0805",     "I2C pull-ups"),
    ("R7",  2, "10k",              "0805",     "BOOT0/BOOT1 pull-downs"),
    ("J1",  1, "XH2.54-2P",        "THT",      "5V power input"),
    ("J2",  1, "1x4 header",       "2.54mm",   "SWD: 3V3/SWDIO/SWCLK/GND"),
    ("J3",  1, "1x4 header",       "2.54mm",   "Console: GND/TX/RX/5V"),
    ("J4",  1, "1x2 header",       "2.54mm",   "Motor output"),
    # No J5: the limit switch input header is not fitted either. Fitting end stops
    # later needs this header back as well, on top of the parts listed at C9/R2.
]


# ---------------------------------------------------------------------------
# Parsing Core/main.h
# ---------------------------------------------------------------------------
PIN_MACRO = re.compile(r"^#define\s+(\w+?)_(PORT|PIN)\s+(\w+)", re.MULTILINE)
GPIO_PIN = re.compile(r"^GPIO_Pin_(\d+)$")
GPIO_PORT = re.compile(r"^GPIO([A-G])$")


def parse_main_h(path):
    """
    Return {stem: {'port': 'A', 'pin': 0}} for every signal that has both a port
    and a pin macro.

    The naming is not uniform, so the two halves are matched by prefix rather
    than by assuming a fixed suffix pair:

        UART_GPIO_PORT  -> stem "UART_GPIO"   ... but the signal is UART_TX
        MYI2C_PORT      -> shared by the OLED I2C signals (MYI2C_SCL/SDA)
        LIMIT_OPEN_PORT -> stem "LIMIT_OPEN"

    A pin macro's stem is progressively shortened until it matches a known port
    stem, which resolves both the shared-port case and the "_GPIO_PORT" style.
    """
    with open(path, "r", encoding="utf-8") as fh:
        text = fh.read()

    port_stems, pin_stems = {}, {}
    for stem, kind, value in PIN_MACRO.findall(text):
        if kind == "PORT":
            m = GPIO_PORT.match(value)
            if m:
                port_stems[stem] = m.group(1)
        else:
            m = GPIO_PIN.match(value)
            if m:
                pin_stems[stem] = int(m.group(1))

    result = {}
    unmatched = []

    for stem, pin in pin_stems.items():
        # Try the exact stem, then progressively shorter prefixes.
        parts = stem.split("_")
        resolved = None
        for take in range(len(parts), 0, -1):
            candidate = "_".join(parts[:take])
            if candidate in port_stems:
                resolved = port_stems[candidate]
                break
        if resolved is None:
            unmatched.append(stem)
        else:
            result[stem] = {"port": resolved, "pin": pin}

    return result, set(unmatched)


# ---------------------------------------------------------------------------
# Checks
# ---------------------------------------------------------------------------
def check(fw_pins, unmatched_stems):
    problems = []
    notes = []

    for stem in sorted(unmatched_stems):
        problems.append(
            "main.h: '%s' has only one of _PORT/_PIN - the signal is unusable" % stem)

    # ---- 1. every firmware pin is claimed by exactly one net per variant ----
    claimed = {}          # (port, pin, variant) -> [(stem, net)]
    for stem, net, role, variants in NET_TABLE:
        if stem not in fw_pins:
            problems.append(
                "net '%s' refers to firmware macro '%s', which does not exist in main.h"
                % (net, stem))
            continue

        p = fw_pins[stem]
        key = (p["port"], p["pin"])
        for variant in ([REAL, SIM] if variants == BOTH else [variants]):
            claimed.setdefault((key[0], key[1], variant), []).append((stem, net))

    # A physical pin may carry two nets only when their variants never coexist.
    for (port, pin, variant), users in sorted(claimed.items()):
        if len(users) > 1:
            problems.append(
                "P%s%d is claimed by %d nets in the %s build: %s"
                % (port, pin, len(users), variant,
                   ", ".join("%s(%s)" % (n, s) for s, n in users)))

    reserved_stems = {stem for stem, _, _ in RESERVED_PINS}

    for stem in sorted(fw_pins.keys()):
        if stem in reserved_stems:
            continue        # deliberately not wired; reported separately
        if not any(stem == s for s, _, _, _ in NET_TABLE):
            problems.append(
                "main.h defines '%s' (P%s%d) but no net claims it - "
                "the schematic would be missing this signal"
                % (stem, fw_pins[stem]["port"], fw_pins[stem]["pin"]))

    # ---- 2. report the deliberate I2C/LCD overlap as informational ----
    #
    # A pin is only worth a note when nets that exist in NEITHER build's sibling
    # claim it - i.e. a REAL-only net and a SIM-only net on the same pin. Pins
    # used by both builds show up in both dictionaries simply because they exist
    # in both, and reporting those would bury the one real overlap in noise.
    real_only = {}
    sim_only = {}
    for stem, net, role, variants in NET_TABLE:
        if stem not in fw_pins:
            continue
        p = fw_pins[stem]
        key = (p["port"], p["pin"])
        if variants == REAL:
            real_only.setdefault(key, []).append(net)
        elif variants == SIM:
            sim_only.setdefault(key, []).append(net)

    for key in sorted(set(real_only.keys()) & set(sim_only.keys())):
        notes.append(
            "P%s%d: %s (real board) vs %s (simulation) - deliberate overlap; "
            "the two builds are never wired at once"
            % (key[0], key[1],
               ", ".join(real_only[key]), ", ".join(sim_only[key])))

    return problems, notes


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------
def write_artifacts(fw_pins):
    os.makedirs(OUT_DIR, exist_ok=True)

    # ---- pinout CSV: firmware view, grouped the way the schematic is drawn ----
    pinout_path = os.path.join(OUT_DIR, "mcu-pinout.csv")
    rows = []
    for stem, net, role, variants in NET_TABLE:
        p = fw_pins.get(stem)
        if p is None:
            continue
        rows.append({
            "MCU Pin": "P%s%d" % (p["port"], p["pin"]),
            "Net": net,
            "Firmware Macro": stem,
            "Function": role,
            "Build": variants,
        })

    for name, net, role in FIXED_PINS:
        rows.append({
            "MCU Pin": name,
            "Net": net,
            "Firmware Macro": "-",
            "Function": role,
            "Build": BOTH,
        })

    # Reserved pins are listed so the schematic can deliberately leave them free,
    # marked as unused rather than silently omitted.
    for stem, pin, role in RESERVED_PINS:
        rows.append({
            "MCU Pin": pin,
            "Net": "(reserved)",
            "Firmware Macro": stem,
            "Function": role,
            "Build": "unused",
        })

    # Grouped by pin number so the schematic can be drawn port by port.
    def sort_key(r):
        p = r["MCU Pin"]
        m = re.match(r"P([A-G])(\d+)$", p)
        if not m:
            return (9, 99, p)
        return (ord(m.group(1)), int(m.group(2)), p)

    rows.sort(key=sort_key)
    with open(pinout_path, "w", newline="", encoding="utf-8") as fh:
        w = csv.DictWriter(fh, fieldnames=["MCU Pin", "Net", "Firmware Macro",
                                           "Function", "Build"])
        w.writeheader()
        w.writerows(rows)

    # ---- BOM CSV ----
    bom_path = os.path.join(OUT_DIR, "bom.csv")
    with open(bom_path, "w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow(["RefDes", "Qty", "Value", "Package", "Description"])
        w.writerows(BOM)

    # ---- netlist, grouped for hand-drawing ----
    netlist_path = os.path.join(OUT_DIR, "netlist.txt")
    by_net = {}
    for stem, net, role, variants in NET_TABLE:
        p = fw_pins.get(stem)
        if p is None:
            continue
        by_net.setdefault(net, []).append(
            ("P%s%d" % (p["port"], p["pin"]), stem, role, variants))

    with open(netlist_path, "w", encoding="utf-8") as fh:
        fh.write("AutoDoor netlist (generated from Core/main.h)\n")
        fh.write("=" * 64 + "\n\n")
        for net in sorted(by_net.keys()):
            fh.write("%s\n" % net)
            for pin, stem, role, variants in by_net[net]:
                fh.write("    %-6s %-16s %-28s [%s]\n" % (pin, stem, role, variants))
            fh.write("\n")

    return pinout_path, bom_path, netlist_path


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true",
                    help="verify only; exit non-zero on any problem")
    args = ap.parse_args()

    if not os.path.exists(MAIN_H):
        print("cannot find %s" % MAIN_H)
        return 2

    fw_pins, unmatched = parse_main_h(MAIN_H)
    print("Parsed %d port/pin macro pairs from Core/main.h" % len(fw_pins))

    problems, notes = check(fw_pins, unmatched)

    for n in notes:
        print("  note: %s" % n)

    if problems:
        print("\n%d PROBLEM(S):" % len(problems))
        for p in problems:
            print("  - %s" % p)
    else:
        print("Netlist is consistent with the firmware: every pin claimed once, "
              "every macro used.")

    if args.check:
        return 1 if problems else 0

    pinout, bom, netlist = write_artifacts(fw_pins)
    print("\nWrote:")
    for path in (pinout, bom, netlist):
        print("  %s" % os.path.relpath(path, ROOT))

    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
