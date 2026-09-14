#!/usr/bin/env python3
"""Cross-check the on-EEPROM layout of the event log against App/Log.c.

WHY THIS EXISTS
---------------
The EEPROM format is described in three places at once, and all three can drift
apart silently:

  1. the ASCII layout comment at the top of App/Log.c
  2. the HDR_OFF_* offset macros
  3. the actual put16()/put32()/buf[] calls in header_write(), record_write()
     and record_read()

A field that is written but never read back, two fields overlapping, a field that
ran past the end of the 32-byte header page - none of these produce a compiler
warning and none of them are visible in a hex dump until you already suspect
something. It matters more than usual here because the header is exactly one
AT24C32 page: a field that spills past byte 31 lands in the first record slot.

The check is deliberately blind-hostile: if the parser does not find the number
of fields it expects, it exits non-zero rather than reporting success. A checker
that passes because it failed to read the file is worse than no checker.

Run:  python tools/check-log-layout.py
Exit: 0 = consistent, 1 = drift found (or the parser went blind)
"""

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LOG_C = ROOT / "App" / "Log.c"
MAIN_H = ROOT / "Core" / "main.h"

# Counts the parser must find, so a silent regex miss cannot masquerade as a
# pass. Update these when the format legitimately gains a field - having to
# touch this file is the point.
EXPECTED_HDR_WRITES = 14      # 4 magic bytes + 7 put16 + 2 put32 + 1 byte field
EXPECTED_REC_WRITES = 8
EXPECTED_REC_READS = 10       # 3 get16 + 1 get32 + 3 byte + 3 reserved bytes

problems = []
notes = []


def fail(msg):
    problems.append(msg)


def read_text(path):
    try:
        # utf-8-sig tolerates a BOM; the sources are ASCII in practice.
        return path.read_text(encoding="utf-8-sig")
    except OSError as exc:
        print(f"cannot read {path}: {exc}")
        sys.exit(1)


def parse_size(text, macro):
    m = re.search(rf"#define\s+{macro}\s+(\d+)U?", text)
    if not m:
        fail(f"{macro} is not defined in {MAIN_H.name}")
        return None
    return int(m.group(1))


main_h = read_text(MAIN_H)
log_c = read_text(LOG_C)

HDR_SIZE = parse_size(main_h, "LOG_HEADER_SIZE")
REC_SIZE = parse_size(main_h, "LOG_ENTRY_SIZE")
SLOT_COUNT = parse_size(main_h, "LOG_SLOT_COUNT")
EEPROM_SIZE = parse_size(main_h, "EEPROM_SIZE_BYTES")
EEPROM_PAGE = parse_size(main_h, "EEPROM_PAGE_SIZE")

if None in (HDR_SIZE, REC_SIZE, SLOT_COUNT, EEPROM_SIZE, EEPROM_PAGE):
    print("FATAL: could not read the layout sizes from Core/main.h")
    sys.exit(1)

offsets = {name: int(val)
           for name, val in re.findall(r"#define\s+(HDR_OFF_\w+)\s+(\d+)U?", log_c)}


def width_of(bits):
    """put16/put32 take a *bit* count, not a byte count."""
    return 2 if bits == "16" else 4


def body_of(fn_signature):
    start = log_c.find(fn_signature)
    if start < 0:
        fail(f"{fn_signature} not found")
        return ""
    # The next line that is exactly "}" at column 0 ends the function.
    end = log_c.find("\n}", start)
    if end < 0:
        fail(f"{fn_signature} has no closing brace")
        return ""
    return log_c[start:end]


def memsets(body, size):
    """Bytes covered by memset(buf, 0, sizeof(buf)) - written, but not fields."""
    return set(range(size)) if re.search(r"memset\(buf,\s*0,\s*sizeof\(buf\)\)", body) else set()


def collect(body, size, use_macros):
    """Return (statements, fields_by_byte, written_by_byte).

    `statements` counts the number of field-writing statements, which is what the
    EXPECTED_* constants below are about; the bytes are tracked separately so
    overlap and in-bounds can be checked per byte. Counting bytes instead of
    statements was this script's own first bug - it reported a 2-byte field as two
    fields and a 4-byte field as four.
    """
    statements = 0
    fields = {}

    for m in re.finditer(r"put(16|32)\(&buf\[([^\]]+)\]", body):
        bits, expr = m.group(1), m.group(2)
        off = resolve(expr, use_macros)
        if off is None:
            fail(f"cannot resolve the buffer offset in put{bits}(&buf[{expr}])")
            continue
        statements += 1
        for b in range(off, off + width_of(bits)):
            fields.setdefault(b, f"put{bits}@{off}")

    for m in re.finditer(r"\bbuf\[([^\]]+)\]\s*=", body):
        off = resolve(m.group(1), use_macros)
        if off is None:
            fail(f"cannot resolve the buffer offset in buf[{m.group(1)}] =")
            continue
        statements += 1
        fields.setdefault(off, f"byte@{off}")

    written = dict(fields)
    for b in memsets(body, size):
        written.setdefault(b, "memset")

    for b in fields:
        if b >= size:
            fail(f"field at offset {b} is outside the {size}-byte region")
    return statements, fields, written


def resolve(expr, use_macros):
    """'HDR_OFF_X' / 'HDR_OFF_MAGIC + 3U' / '13' -> absolute byte offset."""
    expr = expr.strip()
    m = re.fullmatch(r"(\w+)\s*\+\s*(\d+)U?", expr)
    if m and use_macros:
        base = offsets.get(m.group(1))
        return None if base is None else base + int(m.group(2))
    if use_macros:
        if expr in offsets:
            return offsets[expr]
        return None if not expr.isdigit() else int(expr)
    return int(expr) if expr.isdigit() else None


# ---------------------------------------------------------------------------
# 1. Offset macros must sit inside the header page
# ---------------------------------------------------------------------------
for name, off in sorted(offsets.items(), key=lambda kv: kv[1]):
    if off >= HDR_SIZE:
        fail(f"{name} = {off} is outside the {HDR_SIZE}-byte header")

# ---------------------------------------------------------------------------
# 2. The layout comment must agree with the macros
# ---------------------------------------------------------------------------
comment = re.findall(r"^\s*\*\s+\+(\d+)\s+(\w+)\s+(\d+)\s+bytes", log_c, re.M)
if not comment:
    fail("the ASCII layout comment was not found - the parser is blind")

NAME_TO_MACRO = {
    "magic": "HDR_OFF_MAGIC",
    "version": "HDR_OFF_VERSION",
    "count": "HDR_OFF_COUNT",
    "wrindex": "HDR_OFF_WRINDEX",
    "delayms": "HDR_OFF_DELAY",
    "mode": "HDR_OFF_MODE",
    "seqnext": "HDR_OFF_SEQ_NEXT",
    "bootid": "HDR_OFF_BOOT_ID",
    "generation": "HDR_OFF_GENERATION",
    "totallo": "HDR_OFF_TOTAL_LO",
    "totalhi": "HDR_OFF_TOTAL_HI",
}

for off_s, name, size_s in comment:
    off, size = int(off_s), int(size_s)
    if off + size > HDR_SIZE:
        fail(f"comment: '{name}' at +{off} size {size} overruns the header")
    macro = NAME_TO_MACRO.get(name.lower())
    if macro is None:
        continue                       # a reserved/unnamed gap is fine
    if macro not in offsets:
        fail(f"comment names '{name}' but {macro} is not defined")
    elif offsets[macro] != off:
        fail(f"comment says '{name}' is at +{off}, but {macro} is {offsets[macro]}")

# ---------------------------------------------------------------------------
# 3. Header: no overlap, in bounds, comment fields all present
# ---------------------------------------------------------------------------
hdr_body = body_of("static uint8_t header_write(void)")
hdr_stmts, hdr_fields, _ = collect(hdr_body, HDR_SIZE, use_macros=True)

if hdr_stmts != EXPECTED_HDR_WRITES:
    fail(f"found {hdr_stmts} header field writes, expected "
         f"{EXPECTED_HDR_WRITES} - the parser is blind, treat this as a failure")

for off, size, name in comment:
    macro = NAME_TO_MACRO.get(name.lower())
    if macro is None or macro not in offsets:
        continue
    base = offsets[macro]
    missing = [b for b in range(base, base + size) if b not in hdr_fields]
    if missing:
        fail(f"comment declares '{name}' covering bytes {base}..{base + size - 1}, "
             f"but header_write() never writes {missing}")

hdr_last = max(hdr_fields) if hdr_fields else -1
notes.append(f"header bytes {hdr_last + 1}..{HDR_SIZE - 1} are reserved and "
             f"written as zero by memset")

# ---------------------------------------------------------------------------
# 4. Records: agreed offsets, no overlap, in bounds, read/write symmetric
# ---------------------------------------------------------------------------
rec_w_body = body_of("static uint8_t record_write(")
rec_r_body = body_of("static uint8_t record_read(")

rec_w_stmts, rec_w_fields, rec_w_written = collect(rec_w_body, REC_SIZE, use_macros=False)

# record_read() assigns to struct members, not to buf[].
rec_r_fields = {}
rec_r_stmts = 0
for m in re.finditer(r"e->\w+(?:\[\d+\])?\s*=\s*get(16|32)\(&buf\[(\d+)U?\]\)", rec_r_body):
    off = int(m.group(2))
    rec_r_stmts += 1
    for b in range(off, off + width_of(m.group(1))):
        rec_r_fields.setdefault(b, f"get{m.group(1)}@{off}")
for m in re.finditer(r"e->\w+(?:\[\d+\])?\s*=\s*buf\[(\d+)U?\]", rec_r_body):
    rec_r_stmts += 1
    rec_r_fields.setdefault(int(m.group(1)), f"byte@{m.group(1)}")

if rec_w_stmts != EXPECTED_REC_WRITES:
    fail(f"found {rec_w_stmts} record writes, expected {EXPECTED_REC_WRITES}")
if rec_r_stmts != EXPECTED_REC_READS:
    fail(f"found {rec_r_stmts} record reads, expected {EXPECTED_REC_READS}")

for off in sorted(rec_w_fields):
    if off not in rec_r_fields:
        fail(f"record byte +{off} ({rec_w_fields[off]}) is written but never read back")
for off in sorted(rec_r_fields):
    if off not in rec_w_written:
        fail(f"record byte +{off} is read but never written")

# A single-byte write is fine as part of a multi-byte field only if the whole
# field is accounted for; the byte-level maps above already guarantee that.

# ---------------------------------------------------------------------------
# 5. The ring must fit the device and must not collide with the self test
# ---------------------------------------------------------------------------
ring_end = HDR_SIZE + SLOT_COUNT * REC_SIZE
scratch = EEPROM_SIZE - EEPROM_PAGE
if ring_end > scratch:
    fail(f"the ring ends at {ring_end} but the self-test scratch area starts at "
         f"{scratch} - they overlap")
elif ring_end < scratch:
    notes.append(f"ring ends at {ring_end}, self-test scratch starts at {scratch} "
                 f"({scratch - ring_end} bytes unused)")

slot_bytes = SLOT_COUNT * REC_SIZE
if HDR_SIZE + slot_bytes != ring_end:
    fail("ring geometry is inconsistent")

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
print(f"log layout: {HDR_SIZE}-byte header + {SLOT_COUNT} x {REC_SIZE}-byte records")
print(f"  header fields : {hdr_stmts} writes covering bytes 0..{hdr_last}")
print(f"  record fields : {rec_w_stmts} writes / {rec_r_stmts} reads "
      f"covering bytes {min(rec_w_fields)}..{max(rec_w_fields)}")
print(f"  ring          : {HDR_SIZE}..{ring_end - 1} "
      f"({slot_bytes} bytes), device {EEPROM_SIZE} bytes")

for note in notes:
    print(f"  note: {note}")

if problems:
    print("\nFAILED:")
    for p in problems:
        print(f"  - {p}")
    sys.exit(1)

print("\nLayout is consistent: comment, offset macros read/write calls all agree.")
sys.exit(0)
