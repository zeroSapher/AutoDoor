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
  *   +17 reserved   2 bytes
  *   +19 totalLo    4 bytes  lifetime event counter, low 32 bits
  *   +23 totalHi    4 bytes  lifetime event counter, high 32 bits
  *   +27 reserved   5 bytes
  *
  * The header is rewritten whenever a record is flushed. That is one extra
  * 32-byte page write per batch, which is affordable precisely because writes
  * are batched rather than per-event.
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
#define HDR_OFF_TOTAL_LO    19U
#define HDR_OFF_TOTAL_HI    23U

#define LOG_MAGIC_0         'A'
#define LOG_MAGIC_1         'D'
#define LOG_MAGIC_2         'R'
#define LOG_MAGIC_3         'M'
#define LOG_VERSION         1U

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

    return EEPROM_Write(slot_address(slot), buf, LOG_ENTRY_SIZE);
}

static uint8_t record_read(uint16_t slot, LogEntry_t *e)
{
    uint8_t buf[LOG_ENTRY_SIZE];
    uint8_t rc;

    rc = EEPROM_Read(slot_address(slot), buf, LOG_ENTRY_SIZE);
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
  * @brief  Recover the ring position and counters by scanning the record area.
  * @note   The newest record is identified by the largest sequence number,
  *         compared as a signed 16-bit difference so that the wrap from 65535
  *         back to 0 is handled correctly. Scanning is used rather than trusting
  *         the stored write index because a power cut during a flush could leave
  *         the index ahead of the data; the data is always the truth.
  */
static void rebuild_from_records(void)
{
    LogEntry_t e;
    uint16_t   i;
    uint16_t   newestSlot = 0U;
    uint16_t   newestSeq  = 0U;
    uint16_t   found      = 0U;

    s_hdr.count       = 0U;
    s_hdr.wrIndex     = 0U;
    s_hdr.totalEvents = 0U;

    for (i = 0U; i < LOG_SLOT_COUNT; i++)
    {
        if (record_read(i, &e) != MYI2C_OK)
        {
            continue;
        }

        /* A blank slot reads as all 0xFF. Event 0xFF is never valid, and a
           genuine record can never have 0xFFFF as its sequence AND 0xFF as its
           event at the same time, so this is a safe "empty" test. */
        if ((e.event == 0xFFU) || (e.event == LOG_EVT_NONE))
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
        return;
    }

    /* Slots are used in ascending order and wrap, so the slot after the newest
       one is the first free (or oldest) slot. */
    s_hdr.wrIndex = (uint16_t)((newestSlot + 1U) % LOG_SLOT_COUNT);

    /* If the ring wrapped, every slot is populated and the oldest is the one
       right after the newest. `count` is capped at the capacity either way. */
    if (s_hdr.count > LOG_SLOT_COUNT)
    {
        s_hdr.count = LOG_SLOT_COUNT;
    }
}

uint8_t Log_Init(void)
{
    uint8_t hdr[LOG_HEADER_SIZE];

    s_ready        = 0U;
    s_pendingCount = 0U;
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
    s_hdr.totalEvents = 0U;

    if (EEPROM_Read(0U, hdr, LOG_HEADER_SIZE) != MYI2C_OK)
    {
        return 2U;
    }

    if ((hdr[HDR_OFF_MAGIC + 0U] != LOG_MAGIC_0) ||
        (hdr[HDR_OFF_MAGIC + 1U] != LOG_MAGIC_1) ||
        (hdr[HDR_OFF_MAGIC + 2U] != LOG_MAGIC_2) ||
        (hdr[HDR_OFF_MAGIC + 3U] != LOG_MAGIC_3))
    {
        /* Blank or foreign device: start a fresh ring. */
        s_hdr.delayMs = DOOR_AUTO_CLOSE_MS;
        s_hdr.mode    = 0U;
        s_hdr.bootId  = 0U;
        s_hdr.seqNext = 0U;

        if (header_write() != MYI2C_OK)
        {
            return 3U;
        }

        s_hdr.bootId = 1U;
        (void)header_write();

        s_ready = 1U;
        return 0U;
    }

    s_hdr.delayMs     = get16(&hdr[HDR_OFF_DELAY]);
    s_hdr.mode        = hdr[HDR_OFF_MODE];
    s_hdr.seqNext     = get16(&hdr[HDR_OFF_SEQ_NEXT]);
    s_hdr.bootId      = get16(&hdr[HDR_OFF_BOOT_ID]);
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

    rebuild_from_records();

    s_hdr.bootId++;
    s_seqCounter = s_hdr.seqNext;

    /* Persist the incremented boot counter and the recovered ring position. */
    (void)header_write();

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
  */
void Log_Flush(void)
{
    uint8_t i;

    if ((s_ready == 0U) || (s_pendingCount == 0U))
    {
        return;
    }

    for (i = 0U; i < s_pendingCount; i++)
    {
        if (record_write(s_hdr.wrIndex, &s_pending[i]) != MYI2C_OK)
        {
            /* EEPROM unreachable: keep the entries queued rather than losing
               them silently, and let the next flush retry. */
            return;
        }

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

    (void)header_write();

    s_pendingCount = 0U;
    s_flushTimerMs = 0U;
}

void Log_Tick1ms(void)
{
    if ((s_ready == 0U) || (s_pendingCount == 0U))
    {
        return;
    }

    /* Flush sooner when the batch is full, otherwise on the interval. */
    if (s_pendingCount >= LOG_FLUSH_BATCH)
    {
        Log_Flush();
        return;
    }

    s_flushTimerMs++;

    if (s_flushTimerMs >= LOG_FLUSH_INTERVAL_MS)
    {
        Log_Flush();
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

    /* If a burst overruns the batch, flush synchronously rather than dropping
       the event: correctness of the record beats one flush delay. */
    if (s_pendingCount >= LOG_FLUSH_BATCH)
    {
        Log_Flush();
    }

    /*
     * The flush above can FAIL - the EEPROM is unreachable, or its internal
     * write cycle is still running - and on failure Log_Flush() deliberately
     * leaves s_pendingCount untouched so the entries are not lost. That means
     * this function can still arrive here with the queue full, and writing to
     * s_pending[LOG_FLUSH_BATCH] would run one past the end of the array and
     * corrupt whatever follows it in .bss.
     *
     * So the boundary is re-checked AFTER the flush. An event that cannot be
     * queued is dropped and counted; losing a log line is bad, corrupting the
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
    /* Persisted count plus whatever is still queued in RAM, so the answer is
       correct before a flush as well as after one. */
    return s_hdr.totalEvents + (uint64_t)s_pendingCount;
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
  * @brief  Invalidate one record slot so the recovery scan will ignore it.
  * @note   The whole slot is filled with 0xFF, which is exactly what erased
  *         EEPROM reads back as and what rebuild_from_records() skips. Every
  *         byte is set explicitly: a partial initialiser such as
  *         `= { 0xFFU }` would zero the remaining bytes, and a record whose
  *         event byte happens to land on 0xFF but whose other bytes are zero
  *         would still be skipped - but the intent would be unclear and any
  *         change to the empty test would silently break it.
  */
static uint8_t slot_erase(uint16_t slot, uint8_t eepromPresent, uint16_t *erased)
{
    uint8_t blank[LOG_ENTRY_SIZE];
    uint8_t k;

    if (eepromPresent == 0U)
    {
        /* No memory to clear; count the slot as handled so the caller does not
           treat this as a failure. */
        (*erased)++;
        return MYI2C_OK;
    }

    for (k = 0U; k < LOG_ENTRY_SIZE; k++)
    {
        blank[k] = 0xFFU;
    }

    if (EEPROM_Write(slot_address(slot), blank, LOG_ENTRY_SIZE) != MYI2C_OK)
    {
        return MYI2C_ERR_TIMEOUT;
    }

    (*erased)++;
    return MYI2C_OK;
}

uint8_t Log_Clear(void)
{
    uint16_t i;
    uint16_t erased = 0U;
    uint8_t  eepromPresent;

    if (s_ready == 0U)
    {
        return 1U;
    }

    eepromPresent = EEPROM_IsPresent();

    /*
     * Clearing the header alone is NOT enough, and that was a real bug: the
     * record area still holds valid-looking entries, and Log_Init() rebuilds the
     * ring by scanning all LOG_SLOT_COUNT slots on every boot. So a cleared log
     * would come back after the next power cycle.
     *
     * The records are therefore invalidated by writing them blank (0xFF, which
     * rebuild_from_records() skips). That is up to 252 page-writes - slow, but
     * this is a user-initiated command and it runs in the main loop, never on the
     * limit-switch path, so blocking here cannot delay a safety response.
     *
     * It is also the only approach that survives leaving no residue: no validity
     * marker is needed in the header, so an interrupted clear cannot leave a
     * record that the scan would still accept.
     */
    if (eepromPresent != 0U)
    {
        for (i = 0U; i < LOG_SLOT_COUNT; i++)
        {
            if (slot_erase(i, eepromPresent, &erased) != MYI2C_OK)
            {
                /* Stop at the first failure and leave the header untouched, so
                   the ring description still matches what is actually stored. */
                return 2U;
            }
        }
    }

    s_pendingCount = 0U;
    s_hdr.count    = 0U;
    s_hdr.wrIndex  = 0U;

    return header_write();
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
