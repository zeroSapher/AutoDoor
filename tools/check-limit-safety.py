#!/usr/bin/env python3
"""Check the two invariants that the limit/motor-cut path depends on.

SCOPE - READ THIS FIRST
-----------------------
This guard covers the OPTIONAL variant built with `tools\\build.ps1 -WithLimits`
(`-DAUTODOOR_NO_LIMITS=0`). The shipping default fits NO limit switches:
`AUTODOOR_NO_LIMITS` is 1 by default, `Hardware/Limit.c` touches no pin, position
is a timed estimate from DOOR_TRAVEL_MS, and the whole EXTI/fast-stop path below
is not compiled in.

So a PASS here does not say anything about the shipping firmware - that code is
not part of the default build. The limits below still matter because they are the
only guard on the path that reappears the moment someone fits end stops and
builds with -WithLimits, and that path has already been broken twice.

WHY THIS EXISTS
---------------
This path has now been broken twice by REMOVING A GUARD that looked redundant:

  1. PA0/PB0 and PA1/PB1 shared EXTI0/EXTI1, so the limit switches' interrupt was
     stolen by the sensors. The guard that read the pin was doing source
     disambiguation - which mattered, because without it the handler ran for the
     wrong input entirely.
  2. When the pins got distinct lines, that guard was deleted as "no longer
     needed". It had a SECOND job nobody wrote down: the limit lines are
     configured Rising_Falling, so it also restricted the motor cut to the ASSERT
     edge. Deleting it made every RELEASE edge cut the motor, which stranded the
     door a few millimetres off the switch - nothing restarts the motor except
     begin_open/begin_close - and the travel watchdog faulted it five seconds
     later. The build that was default AT THE TIME (limit switches fitted) could
     not open or close the door at all.

Neither break produced a compiler warning, and both would have been invisible to
every other check in tools/. A comment saying "do not remove this" had already
failed once, so the invariants are checked here instead.

This is a TEXTUAL check on the source, not a proof about the program. It asserts
that the two guards are still present and that the decision has not moved back
into the interrupt handler. It cannot tell whether the guards are CORRECT - only
that someone did not quietly delete them. Say so rather than implying more.

The source it reads is compiled only when -WithLimits is passed, so the check
keeps working either way: it reads text, not a build.

Run:  python tools/check-limit-safety.py
Exit: 0 = invariants present, 1 = one is missing or the parser went blind
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IT_C = ROOT / "Core" / "stm32f10x_it.c"
LIMIT_C = ROOT / "Hardware" / "Limit" / "Limit.c"

problems = []


def read_text(path):
    try:
        return path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        print(f"cannot read {path}: {exc}")
        sys.exit(1)


def body_of(text, signature):
    start = text.find(signature)
    if start < 0:
        return None
    end = text.find("\n}", start)
    if end < 0:
        return None
    return text[start:end]


it_c = read_text(IT_C)
limit_c = read_text(LIMIT_C)

# ---------------------------------------------------------------------------
# 1. The EXTI handlers must not cut the motor themselves.
# ---------------------------------------------------------------------------
# The cut decision needs the pin level AND the motor direction. Putting it in the
# handler is how the release edge got cut the second time.
for line_name, fn in (("EXTI0_IRQHandler", "void EXTI0_IRQHandler(void)"),
                      ("EXTI1_IRQHandler", "void EXTI1_IRQHandler(void)")):
    body = body_of(it_c, fn)
    if body is None:
        problems.append(f"{line_name} not found in {IT_C.name} - the parser is blind")
        continue

    # Strip comments so prose about the old bug does not count as code.
    code = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)

    if "Motor_EmergencyStop" in code:
        problems.append(
            f"{line_name} calls Motor_EmergencyStop() directly. The limit lines are "
            f"Rising_Falling, so this handler also runs on the RELEASE edge and would "
            f"cut the motor as the door leaves the switch. The decision belongs in "
            f"Limit_IrqHandler(), which can see the pin level and the motor direction.")
    if "Limit_IrqHandler" not in code:
        problems.append(f"{line_name} no longer calls Limit_IrqHandler()")

# ---------------------------------------------------------------------------
# 2. Limit_IrqHandler must keep both guards.
# ---------------------------------------------------------------------------
handler = body_of(limit_c, "uint8_t Limit_IrqHandler(uint8_t openEdge)")
if handler is None:
    problems.append(f"Limit_IrqHandler not found in {LIMIT_C.name} - the parser is blind")
else:
    code = re.sub(r"/\*.*?\*/", "", handler, flags=re.S)
    code = re.sub(r"//[^\n]*", "", code)

    # Guard A: the ASSERT edge only. Active-low readback, so asserted means the
    # pin reads low; a HIGH pin is the release edge and must not cut.
    if not re.search(r"GPIO_ReadInputDataBit\s*\(", code):
        problems.append(
            "Limit_IrqHandler() no longer reads the limit pin. Without the level "
            "test the RELEASE edge (pin high) cuts the motor and the door cannot "
            "leave a limit.")

    if "Bit_RESET" not in code:
        problems.append(
            "Limit_IrqHandler() no longer compares the pin against Bit_RESET. The "
            "readback is active LOW (Limit.c passes activeLevel 0), so a test "
            "against Bit_SET would invert the whole thing.")

    # Guard B: not while driving away. A departing NO contact bounces, so the pin
    # dips low again for a few milliseconds; without this the cut re-triggers for
    # the whole departure.
    if "Motor_GetDir" not in code:
        problems.append(
            "Limit_IrqHandler() no longer consults Motor_GetDir(). Without the "
            "direction test, contact bounce while the door departs re-cuts the "
            "motor it just started.")

    # The motor cut must still actually happen, or the guard check above is
    # meaningless.
    if "Motor_EmergencyStop" not in code:
        problems.append(
            "Limit_IrqHandler() no longer calls Motor_EmergencyStop() at all - the "
            "fast-stop layer the -WithLimits variant depends on is gone.")

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
print("limit/motor-cut invariants (-WithLimits variant only; the shipping "
      "default fits no limit switches):")
print("  EXTI0/EXTI1 handlers  : delegate to Limit_IrqHandler, no direct cut")
print("  Limit_IrqHandler      : pin-level guard (Bit_RESET) + direction guard")
print("  (textual presence check only - it cannot tell whether the guards are right)")

if problems:
    print("\nFAILED:")
    for p in problems:
        print(f"  - {p}")
    sys.exit(1)

print("\nBoth guards are present in both places.")
sys.exit(0)
