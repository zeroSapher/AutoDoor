/**
  ******************************************************************************
  * @file    Log.c
  * @brief   Persistent event log: RAM queue backed by the AT24C32 EEPROM.
  *
  * HEADER LAYOUT (32 bytes at address 0)
  * -------------------------------------
  *   +0  magic      4 bytes  'A','D','R','M'  - identifies an initialised unit
  *   +4  version    2 bytes
  *   +6  count      2 bytes  records currently valid (0..LOG_SLOT_COUNT)
  *   +8  wrIndex    2 bytes  next slot to write
  *   +10 delayMs    2 bytes  auto-close delay, persisted
  *   +12 mode       1 byte   0 = AUTO, 1 = MANUAL
  *   +13 seqNext    2 bytes  next record sequence number
  *   +15 bootId     2 bytes  power-cycle counter
  *   +17 generation 2 bytes  erase generation (see CLEARING below)
  *   +19 totalLo    4 bytes  lifetime event counter, low 32 bits
  *   +23 totalHi    4 bytes  lifetime event counter, high 32 bits
  *   +27 reserved   5 bytes
  *
  * The header is rewritten whenever a record is flushed. That is one extra
  * 32-byte page write per batch, which is affordable precisely because writes
  * are batched rather than per-event. It is also exactly one AT24C32 page, so a
  * header write is a single page write and cannot be split.
  *
  * CLEARING (why there is a generation field)
  * -----------------------------------------
  * Log_Clear() has to make every stored record invisible, including after the
  * next power-up - rebuild_from_records() reconstructs the ring by scanning the
  * record area, so simply zeroing the header is not enough.
  *
  * The first attempt physically rewrote all 252 slots blank. That is wrong on
  * two counts: it blocks the main loop for well over a second (during which the
  * door state machine does not run at all, including the level-triggered
  * reversal safety net), and it burns 252 EEPROM write cycles per clear.
  *
  * Instead every record carries a copy of the header's generation counter, and a
  * record only counts when the two match. Clearing therefore becomes "increment
  * the generation, reset count/wrIndex, write the header" - one 32-byte page
  * write that atomically invalidates every existing record. A power cut during
  * the clear leaves the old generation in place, so the outcome is either
  * "cleared" or "not cleared", never a half-erased ring.
  *
  * The counter is 16-bit. After 65535 clears it would repeat, which would
  * resurrect records that old, so the wrap is handled by physically erasing the
  * record area exactly once per 65535 clears. The resurrection hazard is bounded
  * and the cost is paid once, not once per clear.
  *
  * WHERE THE EEPROM IS TOUCHED (important)
  * ---------------------------------------
  * EEPROM writes block for ~5 ms each. The 1 ms SysTick handler therefore only
  * ever *requests* a flush; the actual writes happen in Log_Process(), called
  * from the main loop. Doing those writes in SysTick cost 45 ms of interrupt
  * context per batch: it stalled the UART (one byte per 87 us at 115200, so
  * hundreds of dropped received characters) and it could interleave a second
  * EEPROM transaction with one already in progress in the main loop, corrupting
  * both. Log_Add() and Log_Tick1ms() never touch the bus at all.
  ******************************************************************************
  */

#include "Log.h"
#include "EEPROM.h"
#include "MyI2C.h"      /* MYI2C_OK - the EEPROM layer's result codes */
#include "Delay.h"      /* g_msTick - the shared millisecond time base */
#include "main.h"
#include <string.h>

/*===========================================================================*/
/*  Header field offsets                                                     */
/*===========================================================================*/

#define HDR_OFF_MAGIC       0U
#define HDR_OFF_VERSION     4U
#define HDR_OFF_COUNT       6U
#define HDR_OFF_WRINDEX     8U
#define HDR_OFF_DELAY       10U
#define HDR_OFF_MODE        12U
#define HDR_OFF_SEQ_NEXT    13U
#define HDR_OFF_BOOT_ID     15U
#define HDR_OFF_GENERATION  17U
#define HDR_OFF_TOTAL_LO    19U
#define HDR_OFF_TOTAL_HI    23U

#define LOG_MAGIC_0         'A'
#define LOG_MAGIC_1         'D'
#define LOG_MAGIC_2         'R'
#define LOG_MAGIC_3         'M'

/* 2 since records gained the generation field (offset 13..14 of the record,
   previously reserved and written as zero). A version-1 header is rebuilt from
   scratch: its records read back as generation 0 while the new header starts at
   generation 1, so they are ignored rather than misinterpreted. Nothing has been
   deployed, so this costs nobody a log. */
#define LOG_VERSION         2U

/* Generation written on a freshly initialised unit. Never 0, so that records
   left over from a version-1 layout (generation 0) are never accepted. */
#define LOG_GENERATION_INIT 1U

/*
 * The struct and the wire format are NOT the same thing.
 *
 * LogEntry_t is padded to 20 bytes by the compiler: 4-byte alignment of
 * timestampMs leaves 2 bytes after `seq`, and 2-byte alignment of durationMs
 * leaves 1 byte after `mode`. The EEPROM record is 16 bytes of explicitly packed
 * fields (see the offsets in record_write/record_read). sizeof(LogEntry_t) must
 * therefore never be used as the record stride, as an EEPROM_Write length, or as
 * a memcpy size - doing so would write 20-byte records over 16-byte slots.
 *
 * WHAT THESE ASSERTS COVER, AND WHAT THEY DO NOT - an earlier version of this
 * comment claimed more than the code did, which is worse than claiming nothing:
 *
 *   - COVERED: the sum of the struct's stored field sizes still equals
 *     LOG_ENTRY_SIZE. Widening or narrowing a stored field fails the build. The
 *     sum is written in terms of sizeof(member), not of literal numbers, so it
 *     actually tracks the struct; the first version used literals and could not
 *     fail for any edit to the struct at all.
 *   - COVERED: the struct is still larger than the wire record, so the warnings
 *     above remain true.
 *   - NOT COVERED: the byte offsets in record_write()/record_read(), and the
 *     pairing of a field with its offset. Those are literals, and no C construct
 *     checks them. Transposing two same-width fields there (say writing
 *     e->doorState to byte 8 and e->event to byte 9) compiles cleanly and is
 *     invisible here. tools/check-log-layout.py is the guard for that: it parses
 *     the offsets and the member names and cross-checks the write against the
 *     read. Do not read these asserts as covering it.
 */
_Static_assert((sizeof(((LogEntry_t *)0)->seq) +
                sizeof(((LogEntry_t *)0)->timestampMs) +
                sizeof(((LogEntry_t *)0)->bootId) +
                sizeof(((LogEntry_t *)0)->event) +
                sizeof(((LogEntry_t *)0)->doorState) +
                sizeof(((LogEntry_t *)0)->mode) +
                sizeof(((LogEntry_t *)0)->durationMs) +
                2U  /* generation, kept in reserved[0..1] */ +
                1U  /* reserved[2] */) == LOG_ENTRY_SIZE,
               "the stored fields no longer fill LOG_ENTRY_SIZE - update the "
               "offsets in record_write()/record_read() and LOG_VERSION");

_Static_assert(sizeof(LogEntry_t) > LOG_ENTRY_SIZE,
               "LogEntry_t now fits the wire record exactly, so the byte-packing "
               "and the sizeof() warnings above are misleading - recheck them");

/*===========================================================================*/
/*  Internal state                                                           */
/*===========================================================================*/

typedef struct
{
    uint16_t count;
    uint16_t wrIndex;
    uint16_t delayMs;
    uint8_t  mode;
    uint16_t seqNext;
    uint16_t bootId;
    uint16_t generation;    /* bumped by Log_Clear(); records must match */
    uint64_t totalEvents;   /* 64-bit so the lifetime counter never wraps */
} LogHeader_t;

static LogHeader_t s_hdr;
static uint8_t     s_ready = 0U;

/* Pending records waiting to be written. Sized to the flush batch so a burst of
   events cannot overflow it; if it ever did, the oldest pending entry would be
   dropped rather than corrupting the buffer. */
static LogEntry_t  s_pending[LOG_FLUSH_BATCH];
static uint8_t     s_pendingCount = 0U;
static uint16_t    s_droppedEvents = 0U;   /* events lost because the queue stayed full */
static uint16_t    s_flushTimerMs = 0U;

/* Number of entries at the head of s_pending[] that are already in the EEPROM.
   A flush that fails part way through must resume *after* the committed prefix:
   retrying from zero would rewrite those records into fresh slots, duplicating
   them in the ring and counting them twice in the lifetime total. */
static uint8_t     s_flushProgress = 0U;

/* Set by Log_Add()/Log_Tick1ms() (possibly from an interrupt) and consumed by
   Log_Process() in the main loop, which is the only place that touches the bus. */
static volatile uint8_t s_flushRequest = 0U;
static uint8_t          s_flushing = 0U;

/* Last flush attempt failed (EEPROM unreachable or still in its write cycle).
   While this is set, the "queue is full, flush now" fast path is disabled so
   retries are paced by LOG_FLUSH_INTERVAL_MS instead. Without it a full queue
   plus a dead EEPROM would attempt a write on every main-loop iteration, and
   each failed attempt costs a full EEPROM_WRITE_TIMEOUT_MS of busy polling -
   the door state machine would be starved by a bus that is not answering. */
static uint8_t          s_flushFailed = 0U;

/* The records are in the EEPROM but the header describing them is not. The ring
   state in RAM (count, wrIndex, totalEvents) is ahead of what is stored, so the
   header has to be written again before the next boot can see it. Set whenever a
   header write fails - during a flush, or at the end of Log_Init - and cleared
   by the first success.
   
   This exists because dropping the batch on a header failure (which is correct:
   the records must not be rewritten) would otherwise leave the header stale
   until some LATER batch happened to flush successfully. Until then a power cut
   costs the lifetime counter everything in that window, even though every record
   survived - the header page is the most-written page in the device, one write
   per batch, so it is also the most likely one to fail. */
static uint8_t          s_headerDirty = 0U;

static uint16_t    s_seqCounter   = 0U;
/*===========================================================================*/
/*  Little-endian helpers                                                    */
/*===========================================================================*/
/* Explicit byte packing rather than struct-punning: the on-EEPROM format must
   not depend on the compiler's padding or alignment choices. */

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

static uint32_t get32(const uint8_t *p)
{
    return ((uint32_t)p[0]) |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/*===========================================================================*/
/*  Header and record access                                                 */
/*===========================================================================*/

static uint16_t slot_address(uint16_t index)
{
    return (uint16_t)(LOG_HEADER_SIZE + (index * LOG_ENTRY_SIZE));
}

/* Defined below; used by initialise_fresh() for the generation wrap. */
static uint8_t erase_all_slots(void);

static uint8_t header_write(void)
{
    uint8_t buf[LOG_HEADER_SIZE];

    memset(buf, 0, sizeof(buf));

    buf[HDR_OFF_MAGIC + 0U] = LOG_MAGIC_0;
    buf[HDR_OFF_MAGIC + 1U] = LOG_MAGIC_1;
    buf[HDR_OFF_MAGIC + 2U] = LOG_MAGIC_2;
    buf[HDR_OFF_MAGIC + 3U] = LOG_MAGIC_3;

    put16(&buf[HDR_OFF_VERSION],  LOG_VERSION);
    put16(&buf[HDR_OFF_COUNT],    s_hdr.count);
    put16(&buf[HDR_OFF_WRINDEX],  s_hdr.wrIndex);
    put16(&buf[HDR_OFF_DELAY],    s_hdr.delayMs);
    buf[HDR_OFF_MODE] = s_hdr.mode;
    put16(&buf[HDR_OFF_SEQ_NEXT], s_hdr.seqNext);
    put16(&buf[HDR_OFF_BOOT_ID],  s_hdr.bootId);
    put16(&buf[HDR_OFF_GENERATION], s_hdr.generation);
    put32(&buf[HDR_OFF_TOTAL_LO], s_hdr.totalEvents & 0xFFFFFFFFUL);
    put32(&buf[HDR_OFF_TOTAL_HI], (uint32_t)(s_hdr.totalEvents >> 32));

    return EEPROM_Write(0U, buf, LOG_HEADER_SIZE);
}

static uint8_t record_write(uint16_t slot, const LogEntry_t *e)
{
    uint8_t buf[LOG_ENTRY_SIZE];

    memset(buf, 0, sizeof(buf));

    put16(&buf[0],  e->seq);
    put32(&buf[2],  e->timestampMs);
    put16(&buf[6],  e->bootId);
    buf[8]  = e->event;
    buf[9]  = e->doorState;
    buf[10] = e->mode;
    put16(&buf[11], e->durationMs);
    put16(&buf[13], s_hdr.generation);   /* record validity stamp */

    return EEPROM_Write(slot_address(slot), buf, LOG_ENTRY_SIZE);
}

static uint8_t record_read(uint16_t slot, LogEntry_t *e)
{
    uint8_t buf[LOG_ENTRY_SIZE];
    uint8_t rc = MYI2C_ERR_NACK;
    uint8_t attempt;

    /*
     * A failed read used to be treated as "empty slot" by every caller, which
     * silently shortened the recovered record count and could point wrIndex back
     * into the middle of the ring. An AT24C32 read failing is overwhelmingly
     * transient - a glitch on the bus, or the tail of a write cycle - so retry
     * before drawing any conclusion. A slot that still will not read after the
     * retries is skipped by the caller, which is the least-wrong option
     * available: the data is genuinely not readable.
     */
    for (attempt = 0U; attempt < 3U; attempt++)
    {
        rc = EEPROM_Read(slot_address(slot), buf, LOG_ENTRY_SIZE);
        if (rc == MYI2C_OK)
        {
            break;
        }
        Delay_ms(1U);
    }

    if (rc != MYI2C_OK)
    {
        return rc;
    }

    e->seq         = get16(&buf[0]);
    e->timestampMs = get32(&buf[2]);
    e->bootId      = get16(&buf[6]);
    e->event       = buf[8];
    e->doorState   = buf[9];
    e->mode        = buf[10];
    e->durationMs  = get16(&buf[11]);
    e->reserved[0] = buf[13];
    e->reserved[1] = buf[14];
    e->reserved[2] = buf[15];

    return MYI2C_OK;
}

/*===========================================================================*/
/*  Initialisation                                                           */
/*===========================================================================*/

/**
  * @brief  Recover the ring position and count by scanning the record area.
  * @param  newestSeqOut  Receives the highest sequence number found (only valid
  *                       when the return value is non-zero).
  * @return 1 when at least one valid record was found, 0 when the ring is empty.
  * @note   The newest record is identified by the largest sequence number,
  *         compared as a signed 16-bit difference so that the wrap from 65535
  *         back to 0 is handled correctly. Scanning is used rather than trusting
  *         the stored write index because a power cut during a flush could leave
  *         the index ahead of the data; the data is always the truth.
  * @note   This function deliberately does NOT touch s_hdr.totalEvents. It used
  *         to zero it, which silently destroyed the lifetime counter on every
  *         boot: Log_Init() loads the counter from the header, called this, and
  *         then wrote the zeroed value straight back. The counter is owned by
  *         Log_Init() and Log_Flush(); a ring rebuild has no business resetting
  *         it.
  */
static uint8_t rebuild_from_records(uint16_t *newestSeqOut)
{
    LogEntry_t e;
    uint16_t   i;
    uint16_t   newestSlot = 0U;
    uint16_t   newestSeq  = 0U;
    uint16_t   found      = 0U;

    s_hdr.count   = 0U;
    s_hdr.wrIndex = 0U;

    for (i = 0U; i < LOG_SLOT_COUNT; i++)
    {
        uint16_t recGen;

        if (record_read(i, &e) != MYI2C_OK)
        {
            continue;
        }

        /* A blank slot reads as all 0xFF. Event 0xFF is never valid, so this is
           a safe "empty" test on its own, and it also covers the generation
           bytes of a blank slot (0xFFFF) whatever the header says. */
        if ((e.event == 0xFFU) || (e.event == LOG_EVT_NONE))
        {
            continue;
        }

        /* A record only counts if it belongs to the current generation. This is
           what makes Log_Clear() take effect across a power cycle. */
        recGen = (uint16_t)((uint16_t)e.reserved[0] | ((uint16_t)e.reserved[1] << 8));
        if (recGen != s_hdr.generation)
        {
            continue;
        }

        if (found == 0U)
        {
            newestSeq  = e.seq;
            newestSlot = i;
            found      = 1U;
            s_hdr.count = 1U;
        }
        else
        {
            /* Signed 16-bit difference: > 0 means e.seq is newer. */
            if ((int16_t)(e.seq - newestSeq) > 0)
            {
                newestSeq  = e.seq;
                newestSlot = i;
            }
            s_hdr.count++;
        }
    }

    if (found == 0U)
    {
        s_hdr.count   = 0U;
        s_hdr.wrIndex = 0U;
    }
    else
    {
        /* Slots are used in ascending order and wrap, so the slot after the
           newest one is the first free (or oldest) slot. */
        s_hdr.wrIndex = (uint16_t)((newestSlot + 1U) % LOG_SLOT_COUNT);

        /* If the ring wrapped, every slot is populated and the oldest is the one
           right after the newest. `count` is capped at the capacity either way. */
        if (s_hdr.count > LOG_SLOT_COUNT)
        {
            s_hdr.count = LOG_SLOT_COUNT;
        }
    }

    /* Sanity floor: the unit has recorded at least as many events as survive in
       the ring. A corrupted or truncated header would otherwise report
       "CNT=37 TOTAL=0", which reads as a firmware fault. */
    if (s_hdr.totalEvents < (uint64_t)s_hdr.count)
    {
        s_hdr.totalEvents = (uint64_t)s_hdr.count;
    }

    if (newestSeqOut != 0)
    {
        *newestSeqOut = newestSeq;
    }

    return (uint8_t)((found != 0U) ? 1U : 0U);
}

/**
  * @brief  Write a brand-new header over a blanked record area.
  * @param  oldHdr  A readable header from an older record layout, or 0 for a
  *                 blank/foreign device. When non-0 the configuration and the
  *                 lifetime counters are carried over: only the record layout is
  *                 version-specific, so a firmware upgrade should reset the log
  *                 and nothing else.
  * @return 0 on success, non-zero if the ring could not be blanked or the header
  *         page could not be written.
  * @note   The record area is blanked UNCONDITIONALLY, and the generation is then
  *         simply LOG_GENERATION_INIT.
  *
  *         Two earlier versions of this tried to be cleverer - "old generation +
  *         1", then "max(header, highest stamp found in the ring) + 1" - and both
  *         were wrong for the same reason: they assumed something could be known
  *         about the ring that cannot be. The header can lag the stamps (a clear
  *         whose header write failed, followed by record writes that succeeded),
  *         and any scan of the ring is incomplete by construction, because a slot
  *         that fails every read attempt hides its stamp. Either way the computed
  *         generation can land exactly on a stamp that is physically present, and
  *         the next scan then counts those records as live.
  *
  *         Blanking removes the question. With every slot reading 0xFF the empty
  *         test in rebuild_from_records() skips them whatever the generation is,
  *         so no arithmetic over unknown data is needed at all. It costs 252 page
  *         writes (~1.3 s) and happens only when the header is unreadable or the
  *         record layout changed - i.e. once per device, or once per firmware
  *         version bump, before the main loop is even running. That is a cheap
  *         price for not having to reason about unreadable slots.
  */
static uint8_t initialise_fresh(const uint8_t *oldHdr)
{
    uint16_t preservedDelay = DOOR_AUTO_CLOSE_MS;
    uint8_t  preservedMode  = 0U;
    uint16_t preservedSeq   = 0U;
    uint64_t preservedTotal = 0ULL;
    uint16_t firstBootId    = 1U;

    if (oldHdr != 0)
    {
        preservedDelay = get16(&oldHdr[HDR_OFF_DELAY]);
        preservedMode  = oldHdr[HDR_OFF_MODE];
        preservedSeq   = get16(&oldHdr[HDR_OFF_SEQ_NEXT]);
        preservedTotal = (((uint64_t)get32(&oldHdr[HDR_OFF_TOTAL_HI])) << 32) |
                         (uint64_t)get32(&oldHdr[HDR_OFF_TOTAL_LO]);
        firstBootId    = (uint16_t)(get16(&oldHdr[HDR_OFF_BOOT_ID]) + 1U);

        /* Clamp exactly as the normal load path does - an old header deserves
           no more trust than a current one. */
        if ((preservedDelay < DOOR_AUTO_CLOSE_MIN_MS) ||
            (preservedDelay > DOOR_AUTO_CLOSE_MAX_MS))
        {
            preservedDelay = DOOR_AUTO_CLOSE_MS;
        }
        if (preservedMode > 1U)
        {
            preservedMode = 0U;
        }
    }

    /*
     * If this fails part way the ring is a mix of blank and live slots, which is
     * exactly the state that makes Log_Get() return erased slots as records - so
     * do not pretend the log is usable. Reporting it as unavailable routes the
     * application down the existing degradation path, where the door still runs
     * and the console says plainly what is missing.
     */
    if (erase_all_slots() != 0U)
    {
        return 1U;
    }

    s_hdr.count       = 0U;
    s_hdr.wrIndex     = 0U;
    s_hdr.delayMs     = preservedDelay;
    s_hdr.mode        = preservedMode;
    s_hdr.seqNext     = preservedSeq;
    s_hdr.bootId      = firstBootId;
    s_hdr.generation  = LOG_GENERATION_INIT;
    s_hdr.totalEvents = preservedTotal;

    s_seqCounter = s_hdr.seqNext;

    if (header_write() != MYI2C_OK)
    {
        return 1U;
    }

    s_ready = 1U;
    return 0U;
}

uint8_t Log_Init(void)
{
    uint8_t  hdr[LOG_HEADER_SIZE];
    uint8_t  magicOk;
    uint8_t  haveRecord;
    uint16_t newestSeq = 0U;

    s_ready        = 0U;
    s_pendingCount = 0U;
    s_flushProgress = 0U;
    s_flushRequest = 0U;
    s_flushing     = 0U;
    s_flushFailed  = 0U;
    s_headerDirty  = 0U;
    s_flushTimerMs = 0U;

    if (EEPROM_IsPresent() == 0U)
    {
        return 1U;
    }

    /* Defaults, used when the header is missing or unrecognised. */
    s_hdr.count       = 0U;
    s_hdr.wrIndex     = 0U;
    s_hdr.delayMs     = DOOR_AUTO_CLOSE_MS;
    s_hdr.mode        = 0U;
    s_hdr.seqNext     = 0U;
    s_droppedEvents   = 0U;
    s_hdr.bootId      = 0U;
    s_hdr.generation  = LOG_GENERATION_INIT;
    s_hdr.totalEvents = 0U;

    if (EEPROM_Read(0U, hdr, LOG_HEADER_SIZE) != MYI2C_OK)
    {
        return 2U;
    }

    magicOk = (uint8_t)((hdr[HDR_OFF_MAGIC + 0U] == LOG_MAGIC_0) &&
                        (hdr[HDR_OFF_MAGIC + 1U] == LOG_MAGIC_1) &&
                        (hdr[HDR_OFF_MAGIC + 2U] == LOG_MAGIC_2) &&
                        (hdr[HDR_OFF_MAGIC + 3U] == LOG_MAGIC_3));

    /*
     * A blank or foreign device, or one written by an older record layout, gets
     * a fresh ring: the record area is blanked and the header rewritten. See the
     * note on initialise_fresh() for why "blank it" beats any attempt to compute
     * a generation that avoids the stamps already present.
     *
     * Only the record layout is version-specific, so when the magic is intact
     * the configuration and the lifetime counters are carried over.
     */
    if ((magicOk == 0U) || (get16(&hdr[HDR_OFF_VERSION]) != LOG_VERSION))
    {
        if (initialise_fresh((magicOk != 0U) ? hdr : 0) != 0U)
        {
            return 3U;
        }
        return 0U;
    }

    s_hdr.delayMs     = get16(&hdr[HDR_OFF_DELAY]);
    s_hdr.mode        = hdr[HDR_OFF_MODE];
    s_hdr.seqNext     = get16(&hdr[HDR_OFF_SEQ_NEXT]);
    s_hdr.bootId      = get16(&hdr[HDR_OFF_BOOT_ID]);
    s_hdr.generation  = get16(&hdr[HDR_OFF_GENERATION]);
    s_hdr.totalEvents = (((uint64_t)get32(&hdr[HDR_OFF_TOTAL_HI])) << 32) |
                        (uint64_t)get32(&hdr[HDR_OFF_TOTAL_LO]);

    /* Clamp a corrupted delay rather than trusting it. */
    if ((s_hdr.delayMs < DOOR_AUTO_CLOSE_MIN_MS) ||
        (s_hdr.delayMs > DOOR_AUTO_CLOSE_MAX_MS))
    {
        s_hdr.delayMs = DOOR_AUTO_CLOSE_MS;
    }
    if (s_hdr.mode > 1U)
    {
        s_hdr.mode = 0U;
    }
    /* Generation 0 is never written by design, so reading it back means the
       field is corrupt. Flooring it keeps the impossible value from being
       persisted again on the header write below. */
    if (s_hdr.generation == 0U)
    {
        s_hdr.generation = LOG_GENERATION_INIT;
    }

    haveRecord = rebuild_from_records(&newestSeq);

    /*
     * Do not simply carry the header's seqNext forward. A flush writes the
     * records first and the header last, so a power cut in between (or a failed
     * header page) leaves records on the EEPROM that the header knows nothing
     * about - including their sequence numbers. Restarting from the stale header
     * value would reissue sequence numbers that are already in the ring, and the
     * recovery scan identifies the newest record by comparing sequence numbers,
     * so duplicates make the ring order ambiguous. Take whichever is newer.
     */
    if ((haveRecord != 0U) &&
        ((int16_t)((uint16_t)(newestSeq + 1U) - s_hdr.seqNext) > 0))
    {
        s_hdr.seqNext = (uint16_t)(newestSeq + 1U);
    }

    s_hdr.bootId++;
    s_seqCounter = s_hdr.seqNext;

    /*
     * Persist the incremented boot counter, the recovered ring position, the
     * loaded lifetime counter (which rebuild_from_records() leaves alone) and the
     * corrected sequence number.
     *
     * A failure here is not fatal - the RAM state is already correct and the door
     * runs - but it must not be forgotten either. s_headerDirty makes Log_Flush()
     * write the header again even when nothing is queued, so the EEPROM stops
     * describing the previous boot as soon as the bus recovers.
     */
    if (header_write() != MYI2C_OK)
    {
        s_headerDirty = 1U;
    }

    s_ready = 1U;
    return 0U;
}

/*===========================================================================*/
/*  Flushing                                                                 */
/*===========================================================================*/

/**
  * @brief  Write every pending record, then the header.
  * @note   The header goes last on purpose: if power fails part way through, the
  *         header still describes the previous state and the recovery scan finds
  *         the records that did land. Writing the header first would advertise
  *         records that were never stored.
  * @note   MAIN LOOP ONLY. Every call performs tens of milliseconds of blocking
  *         I2C, and the bus is shared with the display and (potentially) with a
  *         transaction the caller is already in the middle of. Interrupt context
  *         must go through Log_RequestFlush()/Log_Process() instead. s_flushing
  *         is a guard, not a licence: it turns a programming error into a no-op
  *         rather than into bus corruption.
  * @return 0 when everything pending reached the EEPROM, non-zero otherwise.
  */
uint8_t Log_Flush(void)
{
    uint8_t i;
    uint8_t rc;

    if (s_ready == 0U)
    {
        s_flushFailed = 1U;
        return 1U;
    }

    /*
     * Nothing queued is NOT the same as nothing to do: the records may all be
     * stored already with only their header outstanding (s_headerDirty). That
     * case falls through to the header write below with an empty loop, which is
     * exactly the retry needed. Returning early here - as this function used to -
     * left the header stale until some later batch happened to flush.
     */
    if ((s_pendingCount == 0U) && (s_headerDirty == 0U))
    {
        s_flushProgress = 0U;
        s_flushTimerMs  = 0U;
        s_flushFailed   = 0U;
        return 0U;
    }

    if (s_flushing != 0U)
    {
        return 1U;      /* already inside a flush; see the note above */
    }

    s_flushing = 1U;

    /* Resume after the entries that already landed. Starting at 0 would rewrite
       them into fresh slots - duplicate records in the ring, and a lifetime
       counter inflated by the retry. */
    for (i = s_flushProgress; i < s_pendingCount; i++)
    {
        if (record_write(s_hdr.wrIndex, &s_pending[i]) != MYI2C_OK)
        {
            /* EEPROM unreachable: keep the entries queued rather than losing
               them silently, and let the next flush resume from here. Anything
               already written stays written, and the header is not touched -
               the next boot's scan recovers those records on its own. */
            s_flushing    = 0U;
            s_flushFailed = 1U;
            return 1U;
        }

        s_flushProgress = (uint8_t)(i + 1U);

        s_hdr.wrIndex = (uint16_t)((s_hdr.wrIndex + 1U) % LOG_SLOT_COUNT);

        if (s_hdr.count < LOG_SLOT_COUNT)
        {
            s_hdr.count++;
        }
        /* Once the ring is full the oldest slot is simply overwritten, so
           `count` stays at capacity. The lifetime counter keeps rising either
           way, so "how many events has this door ever seen" stays answerable
           after the ring has wrapped. */
        s_hdr.totalEvents++;
    }

    /*
     * Reaching here means either there was something to write (and it is now
     * stored) or there was a stale header to retry. Either way the queue has been
     * dealt with, so it is emptied BEFORE the header write: if the header write
     * fails, those records must not be written a second time, and leaving them
     * queued would also pin a full queue forever and make Log_Add() drop every
     * new event. s_headerDirty is what remembers that the header still owes them.
     */
    s_pendingCount  = 0U;
    s_flushProgress = 0U;

    rc = header_write();

    if (rc == MYI2C_OK)
    {
        s_headerDirty = 0U;
        s_flushFailed = 0U;
        s_flushTimerMs = 0U;
    }
    else
    {
        s_headerDirty = 1U;
        s_flushFailed = 1U;
    }

    s_flushing = 0U;
    return (uint8_t)((rc == MYI2C_OK) ? 0U : 1U);
}

/**
  * @brief  Ask for a flush to happen in the main loop.
  * @note   Safe from any context, including an interrupt: it only sets a flag.
  */
void Log_RequestFlush(void)
{
    s_flushRequest = 1U;
    s_flushTimerMs = 0U;
}

/**
  * @brief  Perform a requested flush. Call once per main-loop iteration.
  */
void Log_Process(void)
{
    if (s_flushRequest == 0U)
    {
        return;
    }

    /* Cleared before the attempt, not after: a failed flush must not be retried
       on every iteration. Hammering an unreachable EEPROM at loop speed would
       spend the whole CPU inside I2C timeouts. The 1 ms tick re-requests it once
       the interval elapses, which bounds the retry rate. */
    s_flushRequest = 0U;

    (void)Log_Flush();
}

void Log_Tick1ms(void)
{
    if (s_ready == 0U)
    {
        return;
    }

    if ((s_pendingCount == 0U) && (s_headerDirty == 0U))
    {
        s_flushTimerMs = 0U;
        return;
    }

    /*
     * Request sooner when the batch is full, otherwise on the interval - but
     * only use the fast path when the previous attempt actually worked. After a
     * failure the timer below paces the retries, so an EEPROM that stops
     * answering cannot turn every main-loop iteration into a 20 ms timeout.
     * Nothing is written here either way: see the note on Log_Flush().
     */
    if ((s_pendingCount >= LOG_FLUSH_BATCH) && (s_flushFailed == 0U))
    {
        Log_RequestFlush();
        return;
    }

    s_flushTimerMs++;

    if (s_flushTimerMs >= LOG_FLUSH_INTERVAL_MS)
    {
        Log_RequestFlush();
    }
}

/*===========================================================================*/
/*  Recording                                                                */
/*===========================================================================*/

void Log_Add(LogEvent_t event, uint8_t doorState, uint8_t mode, uint16_t durationMs)
{
    LogEntry_t *e;

    if (s_ready == 0U)
    {
        return;
    }

    /*
     * If a burst overruns the batch, ask for a flush rather than performing one
     * here. This function is documented as never blocking and never touching the
     * EEPROM, and until now it did not honour that: it called Log_Flush(), which
     * is tens of milliseconds of I2C. Log_Process() runs on the same main-loop
     * iteration, so the request is served immediately and the drop path below
     * stays effectively unreachable.
     */
    if ((s_pendingCount >= LOG_FLUSH_BATCH) && (s_flushFailed == 0U))
    {
        Log_RequestFlush();
    }

    /*
     * The queue can still be full here, because a flush is now asynchronous and
     * may also have failed earlier (EEPROM unreachable, or its internal write
     * cycle still running - Log_Flush() deliberately keeps the entries queued so
     * they are not lost). Writing to s_pending[LOG_FLUSH_BATCH] would run one
     * past the end of the array and corrupt whatever follows it in .bss.
     *
     * So the boundary is checked BEFORE the write, always. An event that cannot
     * be queued is dropped and counted; losing a log line is bad, corrupting the
     * door state is worse.
     */
    if (s_pendingCount >= LOG_FLUSH_BATCH)
    {
        s_droppedEvents++;
        return;
    }

    e = &s_pending[s_pendingCount];

    e->seq         = s_seqCounter++;
    e->timestampMs = g_msTick;
    e->bootId      = s_hdr.bootId;
    e->event       = (uint8_t)event;
    e->doorState   = doorState;
    e->mode        = mode;
    e->durationMs  = durationMs;
    e->reserved[0] = 0U;
    e->reserved[1] = 0U;
    e->reserved[2] = 0U;

    /* Keep the persisted "next sequence" in step, so a power cut cannot cause
       sequence numbers to be reused on the next boot. */
    s_hdr.seqNext = s_seqCounter;

    s_pendingCount++;
}

uint16_t Log_DroppedCount(void)
{
    return s_droppedEvents;
}

/*===========================================================================*/
/*  Queries                                                                  */
/*===========================================================================*/

uint16_t Log_Count(void)
{
    return s_hdr.count;
}

uint64_t Log_TotalEvents(void)
{
    /*
     * Persisted count plus whatever is queued but not yet written, so the answer
     * is correct before a flush as well as after one. The subtraction matters
     * after a *partially* failed flush: the entries up to s_flushProgress are
     * already counted in totalEvents, and counting the whole queue would report
     * them twice until the retry completes.
     */
    uint8_t uncommitted = (uint8_t)(s_pendingCount - s_flushProgress);

    return s_hdr.totalEvents + (uint64_t)uncommitted;
}

uint8_t Log_Get(uint16_t index, LogEntry_t *out)
{
    uint16_t slot;
    uint16_t oldest;

    if ((s_ready == 0U) || (out == 0) || (index >= s_hdr.count))
    {
        return 1U;
    }

    /* The oldest retained record is `count` slots behind the write index. */
    oldest = (uint16_t)((s_hdr.wrIndex + LOG_SLOT_COUNT - s_hdr.count) % LOG_SLOT_COUNT);
    slot   = (uint16_t)((oldest + index) % LOG_SLOT_COUNT);

    return record_read(slot, out);
}

/**
  * @brief  Physically blank every record slot.
  * @note   Only used when the generation counter wraps, which takes 65535 clear
  *         commands. The whole slot is filled with 0xFF, which is exactly what
  *         erased EEPROM reads back as and what rebuild_from_records() skips.
  *         Every byte is set explicitly: a partial initialiser such as
  *         `= { 0xFFU }` would zero the remaining bytes, and a record whose event
  *         byte happens to land on 0xFF but whose other bytes are zero would
  *         still be skipped - but the intent would be unclear and any change to
  *         the empty test would silently break it.
  * @return 0 on success, non-zero at the first slot that could not be written.
  */
static uint8_t erase_all_slots(void)
{
    uint8_t blank[LOG_ENTRY_SIZE];
    uint8_t k;
    uint16_t i;

    for (k = 0U; k < LOG_ENTRY_SIZE; k++)
    {
        blank[k] = 0xFFU;
    }

    for (i = 0U; i < LOG_SLOT_COUNT; i++)
    {
        if (EEPROM_Write(slot_address(i), blank, LOG_ENTRY_SIZE) != MYI2C_OK)
        {
            return 1U;
        }
    }

    return 0U;
}

uint8_t Log_Clear(void)
{
    if (s_ready == 0U)
    {
        return 1U;
    }

    /*
     * Clearing the header alone is NOT enough - the record area still holds
     * valid-looking entries and Log_Init() rebuilds the ring by scanning all
     * LOG_SLOT_COUNT slots on every boot, so a cleared log came back after the
     * next power cycle.
     *
     * The first fix rewrote all 252 slots blank. That worked, but it was the
     * wrong trade twice over:
     *
     *   - It blocked the main loop for over a second (252 writes, each waiting
     *     out a ~5 ms internal cycle). Door_Update() does not run during that
     *     time, which includes the level-triggered reversal safety net - so a
     *     person stepping into the doorway while the door was closing would not
     *     be seen. The old comment claimed this "cannot delay a safety response"
     *     because it is not on the limit-switch path; that reasoning was wrong,
     *     because the limit ISR is not the only safety layer.
     *   - It spent 252 EEPROM write cycles per clear on the same page.
     *
     * Now a record only counts when its generation stamp matches the header's, so
     * invalidating every record is a single 32-byte page write - the header page,
     * written atomically. A power cut during the clear leaves the old generation,
     * so the result is either "cleared" or "not cleared", never a half-erased
     * ring.
     */
    s_hdr.generation++;

    if (s_hdr.generation == 0U)
    {
        /*
         * 65535 clears have happened and the counter would repeat, which would
         * resurrect records that old. Blank the record area once and restart the
         * counter. This is the only path that still pays the full erase, and it
         * is rare enough to be worth the pause; the pause itself is unavoidable,
         * because there is no way to invalidate 252 records that already carry
         * the generation we are about to reuse.
         */
        uint16_t newestSeq = 0U;

        if (erase_all_slots() != 0U)
        {
            /*
             * The erase stopped part way through, which is the one place in this
             * function where the ring is left inconsistent: some slots are now
             * blank while s_hdr.count and s_hdr.wrIndex still describe the old,
             * full ring. Log_Get() walks `count` slots back from wrIndex, so with
             * a full ring every index would land on an erased slot and be printed
             * as a record with event 0xFF ("UNKNOWN") and sequence 0xFFFF.
             *
             * So rescan and let the EEPROM say what actually survives. The
             * generation is put back BEFORE the scan so the surviving records -
             * which still carry it - are the ones counted.
             */
            s_hdr.generation = 0xFFFFU;
            (void)rebuild_from_records(&newestSeq);
            s_headerDirty = 1U;

            return 2U;
        }

        s_hdr.generation = LOG_GENERATION_INIT;
    }

    /*
     * Anything still queued belongs to the log being erased, but it has already
     * been counted by Log_TotalEvents(): dropping the queue without folding it in
     * first would make TOTAL go BACKWARDS, which contradicts it being a lifetime
     * counter. The events are lost (that is what clearing means) - the count of
     * how many this unit has ever seen is not.
     */
    s_hdr.totalEvents += (uint64_t)(s_pendingCount - s_flushProgress);

    s_pendingCount  = 0U;
    s_flushProgress = 0U;
    s_flushRequest  = 0U;

    s_hdr.count   = 0U;
    s_hdr.wrIndex = 0U;

    {
        uint8_t rc = header_write();

        /*
         * Keep the retry-pacing flag honest: a successful clear proves the bus
         * works, a failed one is exactly the condition the backoff exists for.
         * A failed clear also leaves the EEPROM header describing the previous
         * generation while RAM has moved on, so the header is marked dirty and
         * Log_Flush() will publish the new generation - and, with it, make the
         * clear actually take effect - as soon as the bus recovers.
         *
         * The return is normalised to the documented 0/1 contract, NOT passed
         * through raw. header_write() returns a MYI2C_* code, and
         * MYI2C_ERR_TIMEOUT happens to be 2 - the value Log.h and Cmd.c reserve
         * for "the generation-wrap erase was incomplete". Returning rc directly
         * meant an ordinary header-write timeout reported "clear incomplete, 0
         * records remain", which is actively misleading: the clear DID succeed in
         * RAM and no erase was attempted. 2 is now returned only by the erase
         * path above.
         */
        s_flushFailed = (uint8_t)((rc == MYI2C_OK) ? 0U : 1U);
        s_headerDirty = (uint8_t)((rc == MYI2C_OK) ? 0U : 1U);

        return (uint8_t)((rc == MYI2C_OK) ? 0U : 1U);
    }
}

/*===========================================================================*/
/*  Persisted settings                                                       */
/*===========================================================================*/

uint16_t Log_GetAutoCloseMs(void)
{
    return s_hdr.delayMs;
}

uint8_t Log_SetAutoCloseMs(uint16_t ms)
{
    if ((ms < DOOR_AUTO_CLOSE_MIN_MS) || (ms > DOOR_AUTO_CLOSE_MAX_MS))
    {
        return 1U;
    }

    s_hdr.delayMs = ms;

    return header_write();
}

uint8_t Log_GetPersistedMode(void)
{
    return s_hdr.mode;
}

uint8_t Log_SetPersistedMode(uint8_t mode)
{
    if (mode > 1U)
    {
        return 1U;
    }

    s_hdr.mode = mode;

    return header_write();
}

uint16_t Log_GetBootId(void)
{
    return s_hdr.bootId;
}

/*===========================================================================*/
/*  Names                                                                    */
/*===========================================================================*/

const char *Log_EventName(uint8_t event)
{
    switch (event)
    {
        case LOG_EVT_ENTER:        return "ENTER_IN";
        case LOG_EVT_EXIT:         return "EXIT_OUT";
        case LOG_EVT_OPEN_START:   return "OPEN_START";
        case LOG_EVT_CLOSE_START:  return "CLOSE_START";
        case LOG_EVT_OPEN_DONE:    return "OPEN_DONE";
        case LOG_EVT_REVERSE:      return "REVERSE";
        case LOG_EVT_CLOSE_DONE:   return "CLOSE_DONE";
        case LOG_EVT_ESTOP:        return "ESTOP";
        case LOG_EVT_FAULT_OPEN:   return "FAULT_OPEN_TIMEOUT";
        case LOG_EVT_FAULT_CLOSE:  return "FAULT_CLOSE_TIMEOUT";
        case LOG_EVT_MODE_CHANGE:  return "MODE_CHANGE";
        case LOG_EVT_PARAM_CHANGE: return "PARAM_CHANGE";
        case LOG_EVT_LIMIT_FAULT:  return "FAULT_LIMIT";
        case LOG_EVT_SYSTEM_START: return "SYSTEM_START";
        case LOG_EVT_SYSTEM_STOP:  return "SYSTEM_STOP";
        default:                   return "UNKNOWN";
    }
}
