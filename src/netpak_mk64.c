/**
 * netpak_mk64.c — NetPak64 device driver, libultra port for mk64.
 *
 * A from-scratch reimplementation of libnetpak's netpak.c against the same
 * register-level device contract (netpak-spec.md v0.9), using mk64's libultra
 * primitives instead of libdragon's. The ch1 reliable-channel logic and the
 * mailbox command protocol are ported verbatim; only the hardware access layer
 * (register IO, PI DMA, cache, timing) is rewritten.
 *
 * KEY PORTING NOTE (spec §5.2): both libdragon (dma_read/dma_write) AND mk64's
 * libultra (osPiRawReadIo / osPiRawStartDma) mangle their PI address into the
 * cart-ROM range by OR-ing `osRomBase` (0x10000000). The device lives at
 * Domain-2 physical 0x05F00000, so those helpers would silently hit
 * 0x15F00000. This driver therefore talks to the PI hardware registers
 * directly and reads/writes device registers through KSEG1 at the true
 * physical address (IO_READ / IO_WRITE, which use PHYS_TO_K1).
 *
 * Milestone 0 runs POLL MODE only: netpak_poll() drains the device and pumps
 * ch1 timers once per frame from the game thread. Because every entry point
 * (poll / recv / send) runs on that single thread, no interrupt-level critical
 * sections are needed yet; the cart-IRQ receive path is a later increment.
 */
#include <ultra64.h>
#include <PR/rcp.h>       /* IO_READ / IO_WRITE (PHYS_TO_K1 uncached access) */
#include <macros.h>       /* ALIGNED16 */
#include <string.h>
#include "netpak.h"

/* --- PI hardware registers (physical addresses) --------------------------- */
#define NP_PI_DRAM_ADDR_REG 0x04600000
#define NP_PI_CART_ADDR_REG 0x04600004
#define NP_PI_RD_LEN_REG    0x04600008
#define NP_PI_WR_LEN_REG    0x0460000C
#define NP_PI_STATUS_REG    0x04600010
#define NP_PI_STATUS_BUSY   0x1
#define NP_PI_STATUS_IOBUSY 0x2

/* Dom2 timing registers + spec-pinned values (spec §2.1, PROVISIONAL). */
#define NP_PI_BSD_DOM2_LAT_REG 0x04600024
#define NP_PI_BSD_DOM2_PWD_REG 0x04600028
#define NP_PI_BSD_DOM2_PGS_REG 0x0460002C
#define NP_PI_BSD_DOM2_RLS_REG 0x04600030

/* --- Register map (netpak-spec.md §3) ------------------------------------- */
#define NP_REG_MAGIC       0x0000
#define NP_REG_VERSION     0x0004
#define NP_REG_STATUS      0x000C
#define NP_REG_IRQ_STATUS  0x0010
#define NP_REG_IRQ_MASK    0x0014
#define NP_REG_CMD         0x0018
#define NP_REG_CMD_STATUS  0x001C
#define NP_REG_ARG0        0x0020
#define NP_REG_RES0        0x0030
#define NP_REG_RES1        0x0034
#define NP_REG_TX_LEN_DST  0x0040
#define NP_REG_TX_DOORBELL 0x0048
#define NP_REG_RX_LEN_SRC  0x0050
#define NP_REG_RX_CONSUME  0x0058
#define NP_REG_RX_COUNT    0x005C
#define NP_REG_EPOCH       0x0060
#define NP_REG_CMD_DATA    0x0800
#define NP_REG_TX_BUF      0x1000
#define NP_REG_RX_WIN      0x2000

#define NP_MAGIC_VALUE 0x4E503634U /* "NP64" */

/* STATUS bits (spec §3.2) */
#define NP_STATUS_TX_READY (1u << 3)
/* IRQ bits (spec §3.3) */
#define NP_IRQ_RX_READY    (1u << 0)
#define NP_IRQ_TX_DONE     (1u << 1)
#define NP_IRQ_EVENT       (1u << 2)
#define NP_IRQ_LINK_CHANGE (1u << 3)

/* --- Low-level hardware access -------------------------------------------- */
/* Wait for the PI bus to go idle. Every register access and DMA must clear
 * this first: a PI write holds the bus busy for hundreds of cycles, during
 * which a read returns the write latch (not the register) and a second write
 * is dropped (spec §5.2). */
static void np_pi_wait(void) {
    while (IO_READ(NP_PI_STATUS_REG) & (NP_PI_STATUS_BUSY | NP_PI_STATUS_IOBUSY)) {
        ;
    }
}

/* Uncached 32-bit device register read/write at the TRUE physical address —
 * IO_READ/IO_WRITE go through PHYS_TO_K1, NOT osRomBase, so Domain-2 addresses
 * are hit correctly (see file header). */
static u32 np_read(u32 off) {
    np_pi_wait();
    return IO_READ(NETPAK_BASE_PHYS + off);
}

static void np_write(u32 off, u32 v) {
    np_pi_wait();
    IO_WRITE(NETPAK_BASE_PHYS + off, v);
}

/* One PI DMA to/from a device buffer window. Drives the PI registers directly
 * (bypassing osRomBase) so cart_off is decoded in Domain 2. dir is OS_READ
 * (device -> RDRAM) or OS_WRITE (RDRAM -> device). Blocks until complete. */
static void np_dma(s32 dir, u32 cart_off, void *dram, u32 len) {
    np_pi_wait();
    IO_WRITE(NP_PI_DRAM_ADDR_REG, osVirtualToPhysical(dram));
    IO_WRITE(NP_PI_CART_ADDR_REG, (NETPAK_BASE_PHYS + cart_off) & 0x1FFFFFFF);
    if (dir == OS_READ) {
        IO_WRITE(NP_PI_WR_LEN_REG, len - 1); /* cart -> RDRAM */
    } else {
        IO_WRITE(NP_PI_RD_LEN_REG, len - 1); /* RDRAM -> cart */
    }
    np_pi_wait();
}

/* microseconds of emulated time (spec's get_ticks_us() analogue). Equivalent
 * to OS_CYCLES_TO_USEC with OS_CPU_COUNTER = 46875000 (that macro lives in the
 * monolithic PR/os.h, which ultra64.h does not pull in). */
static u64 np_ticks_us(void) {
    return (osGetTime() * 64ull) / 3000ull;
}

/* --- Driver state --------------------------------------------------------- */
#define NP_RING_DEPTH 16
static netpak_pkt_t np_ring[NP_RING_DEPTH];
static volatile u32 np_ring_head, np_ring_tail; /* tail-head = fill */
static volatile u32 np_rx_dropped;
static bool np_irq_mode;
static bool np_present; /* cached: device detected + initialized */

/* 16-byte-aligned bounce buffers. DMA needs 8-byte RDRAM alignment / even
 * lengths, but osInvalDCache/osWritebackDCache operate at 16-byte cache-line
 * granularity — the stricter master. Pad every transfer to 16 (spec §5.2). */
static u8 np_dma_buf[NETPAK_MTU]   ALIGNED16;
static u8 np_dma_txbuf[NETPAK_MTU] ALIGNED16;

static u32 np_pad16(u32 n) { return (n + 15) & ~15u; }

/* ==========================================================================
 * ch1 — reliable-ordered channel (netpak-spec.md §8.5: end-to-end in the
 * library, the relay stays dumb). Ported verbatim from the reference driver:
 * per-peer sliding window, cumulative acks, fixed RTO, in-order delivery
 * through netpak_recv(). All static memory; u16 seq with wraparound compare.
 * ========================================================================== */
#define CH1_WINDOW    4
#define CH1_HDR       4      /* u16 seq, u16 ack (big-endian) */
#define CH1_MAX_DATA  (NETPAK_MTU - CH1_HDR)
#define CH1_RTO_US    100000 /* 100 ms emulated */
#define CH1_ACK_US    20000  /* standalone-ack delay */
#define CH1_MAX_RETRY 64     /* then assume the peer is gone; drop */

typedef struct {
    bool used;
    u16  seq;
    u16  len;
    u64  next_send_us;
    u32  tries;
    u8   data[CH1_MAX_DATA];
} ch1_slot_t;

typedef struct {
    bool used;
    u8   peer;
    u16  tx_next;    /* next seq to assign */
    u16  rx_expect;  /* next in-order seq we want (cumulative ack) */
    bool ack_pending;
    u64  ack_due_us;
    ch1_slot_t txw[CH1_WINDOW];
    ch1_slot_t rxb[CH1_WINDOW]; /* out-of-order hold */
} ch1_peer_t;

static ch1_peer_t np_ch1[8];
static u32 np_ch1_epoch;

/* seq a "before" b in wraparound space */
static bool ch1_before(u16 a, u16 b) { return (s16)(a - b) < 0; }

static ch1_peer_t *ch1_state(u8 peer, bool alloc) {
    s32 i;
    for (i = 0; i < 8; i++) {
        if (np_ch1[i].used && np_ch1[i].peer == peer) {
            return &np_ch1[i];
        }
    }
    if (!alloc) {
        return NULL;
    }
    for (i = 0; i < 8; i++) {
        if (!np_ch1[i].used) {
            bzero(&np_ch1[i], sizeof(np_ch1[i]));
            np_ch1[i].used = true;
            np_ch1[i].peer = peer;
            return &np_ch1[i];
        }
    }
    return NULL;
}

static void ch1_reset_all(void) {
    bzero(np_ch1, sizeof(np_ch1));
}

/* Raw ch1 datagram out: [seq, ack, data...]. Forward declaration. */
static s32 ch1_transmit(u8 peer, u16 seq, u16 ack, const void *data, u16 len);
/* Delivers one in-order ch1 payload into the game ring. Forward declaration. */
static void np_ring_push(u8 src, u8 ch, const u8 *data, u16 len);

static void ch1_send_ack(ch1_peer_t *st) {
    ch1_transmit(st->peer, 0, st->rx_expect, NULL, 0);
    st->ack_pending = false;
}

/* Pump timers: retransmits + standalone acks. Called from netpak_poll() and
 * the ch1 send spin. */
static void ch1_pump(void) {
    u64 now = np_ticks_us();
    u32 e;
    s32 p, i;

    /* A loadstate/reset/reconnect invalidates every window; peers re-handshake
     * at the app level (README §5). */
    e = netpak_epoch();
    if (e != np_ch1_epoch) {
        np_ch1_epoch = e;
        ch1_reset_all();
        return;
    }

    for (p = 0; p < 8; p++) {
        ch1_peer_t *st = &np_ch1[p];
        if (!st->used) {
            continue;
        }
        for (i = 0; i < CH1_WINDOW; i++) {
            ch1_slot_t *s = &st->txw[i];
            if (!s->used || now < s->next_send_us) {
                continue;
            }
            if (s->tries >= CH1_MAX_RETRY) {
                s->used = false;
                continue;
            }
            ch1_transmit(st->peer, s->seq, st->rx_expect, s->data, s->len);
            st->ack_pending = false; /* ack piggybacked */
            s->tries++;
            s->next_send_us = now + CH1_RTO_US;
        }
        if (st->ack_pending && now >= st->ack_due_us) {
            ch1_send_ack(st);
        }
    }
}

/* Peer acked everything before `ack`: release those TX slots. */
static void ch1_process_ack(ch1_peer_t *st, u16 ack) {
    s32 i;
    for (i = 0; i < CH1_WINDOW; i++) {
        if (st->txw[i].used && ch1_before(st->txw[i].seq, ack)) {
            st->txw[i].used = false;
        }
    }
}

/* Inbound ch1 datagram (from np_drain). Handles ack, ordering, delivery. */
static void ch1_ingest(u8 src, const u8 *pkt, u16 len) {
    ch1_peer_t *st;
    u16 seq, ack, dlen;
    s32 i;

    if (len < CH1_HDR) {
        return;
    }
    st = ch1_state(src, true);
    if (!st) {
        return;
    }

    seq = (u16)((pkt[0] << 8) | pkt[1]);
    ack = (u16)((pkt[2] << 8) | pkt[3]);
    ch1_process_ack(st, ack);

    dlen = len - CH1_HDR;
    if (dlen == 0) {
        return; /* standalone ack */
    }

    if (ch1_before(seq, st->rx_expect)) { /* duplicate of delivered */
        st->ack_pending = true;
        st->ack_due_us = np_ticks_us() + CH1_ACK_US;
        return;
    }

    if (seq == st->rx_expect) {
        bool progressed = true;
        np_ring_push(src, 1, pkt + CH1_HDR, dlen);
        st->rx_expect++;
        /* Drain any buffered successors now in order. */
        while (progressed) {
            progressed = false;
            for (i = 0; i < CH1_WINDOW; i++) {
                ch1_slot_t *s = &st->rxb[i];
                if (s->used && s->seq == st->rx_expect) {
                    np_ring_push(src, 1, s->data, s->len);
                    s->used = false;
                    st->rx_expect++;
                    progressed = true;
                }
            }
        }
    } else {
        /* Out of order: hold it (dedup; drop if hold buffer full — the
         * retransmit recovers it later). */
        bool have = false;
        for (i = 0; i < CH1_WINDOW; i++) {
            if (st->rxb[i].used && st->rxb[i].seq == seq) {
                have = true;
            }
        }
        if (!have) {
            for (i = 0; i < CH1_WINDOW; i++) {
                ch1_slot_t *s = &st->rxb[i];
                if (!s->used) {
                    s->used = true;
                    s->seq = seq;
                    s->len = dlen > CH1_MAX_DATA ? CH1_MAX_DATA : dlen;
                    memcpy(s->data, pkt + CH1_HDR, s->len);
                    break;
                }
            }
        }
    }
    st->ack_pending = true;
    st->ack_due_us = np_ticks_us() + CH1_ACK_US;
}

/* --- RX ring + device drain ----------------------------------------------- */
static void np_ring_push(u8 src, u8 ch, const u8 *data, u16 len) {
    netpak_pkt_t *slot;
    u32 fill = np_ring_tail - np_ring_head;
    if (fill >= NP_RING_DEPTH) { /* drop-oldest, matching the device */
        np_ring_head++;
        np_rx_dropped++;
    }
    slot = &np_ring[np_ring_tail % NP_RING_DEPTH];
    slot->src = src;
    slot->ch = ch;
    slot->len = len;
    memcpy(slot->data, data, len);
    np_ring_tail++;
}

/* Drain every queued device-side packet into the RDRAM ring. */
static void np_drain(void) {
    u32 iters = 0;
    while (np_read(NP_REG_RX_COUNT) > 0) {
        u32 lensrc;
        u16 len;
        u8 src, ch;

        if (++iters > 64) { /* defensive: a spin here means a bus-level bug */
            break;
        }
        lensrc = np_read(NP_REG_RX_LEN_SRC);
        len = (u16)(lensrc >> 16);
        if (len > NETPAK_MTU) {
            len = NETPAK_MTU;
        }

        if (len) {
            /* device -> RDRAM. Invalidate around the DMA so no stale/dirty
             * cache lines survive over the freshly-DMA'd data. */
            osInvalDCache(np_dma_buf, np_pad16(len));
            np_dma(OS_READ, NP_REG_RX_WIN, np_dma_buf, np_pad16(len));
            osInvalDCache(np_dma_buf, np_pad16(len));
        }

        src = (u8)((lensrc >> 8) & 0xFF);
        ch = (u8)(lensrc & 0xFF);
        if (ch == 1) {
            ch1_ingest(src, np_dma_buf, len);
        } else {
            np_ring_push(src, ch, np_dma_buf, len);
        }

        np_write(NP_REG_RX_CONSUME, 1); /* pop head, expose next */
    }
    /* Ack the level sources we just serviced (W1C). */
    np_write(NP_REG_IRQ_STATUS, NP_IRQ_RX_READY | NP_IRQ_TX_DONE);
}

/* --- Public API ----------------------------------------------------------- */

bool netpak_detect(void) {
    if (np_read(NP_REG_MAGIC) != NP_MAGIC_VALUE) {          /* step 1 */
        return false;
    }
    if ((np_read(NP_REG_VERSION) >> 16) != 0) {             /* step 2 */
        return false;
    }
    np_write(NP_REG_CMD_DATA, 0xC0FFEE42U);                 /* step 3 */
    if (np_read(NP_REG_CMD_DATA) != 0xC0FFEE42U) {
        return false;
    }
    return true;
}

u32 netpak_version(void) {
    return np_read(NP_REG_VERSION);
}

bool netpak_present(void) {
    return np_present;
}

void netpak_debug_poke(u32 v) {
    np_write(NP_REG_ARG0, v); /* ARG0 (0x0020): a real register the trace logs */
}

/* Read the launch room code (from ares' NP64_ROOM env) exposed at 0x0064/0x0068,
 * 8 bytes big-endian. Writes a NUL-terminated string to out[8]; out[0]=='\0'
 * means no code was provided. Lets the online menu pre-fill the join code. */
void netpak_get_room_code(char out[8]) {
    u32 w0, w1;
    if (!np_present) {
        out[0] = '\0';
        return;
    }
    w0 = np_read(0x0064);
    w1 = np_read(0x0068);
    out[0] = (char) ((w0 >> 24) & 0xFF);
    out[1] = (char) ((w0 >> 16) & 0xFF);
    out[2] = (char) ((w0 >> 8) & 0xFF);
    out[3] = (char) (w0 & 0xFF);
    out[4] = (char) ((w1 >> 24) & 0xFF);
    out[5] = (char) ((w1 >> 16) & 0xFF);
    out[6] = '\0'; /* room codes are 6 chars */
    out[7] = '\0';
}

/* Read the current player name exposed at 0x006C-0x0078 (16 bytes big-endian,
 * NUL-padded): the last SET_IDENTITY, else the launch identity (NP64_NAME or
 * the emulator-persisted name). */
void netpak_get_name(char out[16]) {
    s32 w;
    if (!np_present) {
        out[0] = '\0';
        return;
    }
    for (w = 0; w < 16; w += 4) {
        u32 v = np_read(0x006C + (u32) w);
        out[w + 0] = (char) ((v >> 24) & 0xFF);
        out[w + 1] = (char) ((v >> 16) & 0xFF);
        out[w + 2] = (char) ((v >> 8) & 0xFF);
        out[w + 3] = (char) (v & 0xFF);
    }
    out[15] = '\0';
}

/* Direct write to ares' IS-Viewer at the cartridge domain (phys 0x13FF0000),
 * bypassing libultra's osEPiWriteIo (whose osRomBase mangling keeps the stock
 * osSyncPrintf from ever reaching ares). Matches ares' flush protocol: bytes to
 * the data buffer (0x20+), then a word to PUT (0x14) whose low half lands on the
 * 0x16 flush trigger. ares mirrors the chars to stdout when run with ARES_ISV=1.
 * A clean headless printf channel for the debug harness. */
#define NP_ISV_BASE 0x13FF0000u
void netpak_isv_print(const char* s) {
    static s32 inited = 0;
    u32 i, j, len = 0;

    while (s[len] != '\0' && len < 0x400u) {
        len++;
    }
    if (len == 0) {
        return;
    }
    if (!inited) {
        np_pi_wait();
        IO_WRITE(NP_ISV_BASE + 0x00, 0x49533634u); /* magic 'IS64' */
        inited = 1;
    }
    np_pi_wait();
    IO_WRITE(NP_ISV_BASE + 0x04, 0); /* GET = 0 (ares reads [GET, count)) */
    for (i = 0; i < len; i += 4) {
        u32 w = 0;
        for (j = 0; j < 4; j++) {
            u8 c = (i + j < len) ? (u8) s[i + j] : (u8) 0;
            w |= ((u32) c) << ((3 - j) * 8); /* big-endian byte order */
        }
        np_pi_wait();
        IO_WRITE(NP_ISV_BASE + 0x20 + i, w);
    }
    np_pi_wait();
    IO_WRITE(NP_ISV_BASE + 0x14, len & 0xFFFFu); /* PUT word; low half -> 0x16 flush */
}

s32 netpak_init(bool use_irq) {
    np_present = false;
    if (!netpak_detect()) {
        return -1;
    }

    /* Spec-pinned Dom2 bus timings (spec §2.1): software-owned. */
    np_pi_wait();
    IO_WRITE(NP_PI_BSD_DOM2_LAT_REG, 0xFF);
    IO_WRITE(NP_PI_BSD_DOM2_PWD_REG, 0xFF);
    IO_WRITE(NP_PI_BSD_DOM2_PGS_REG, 0x0F);
    IO_WRITE(NP_PI_BSD_DOM2_RLS_REG, 0x03);

    np_ring_head = np_ring_tail = 0;
    np_rx_dropped = 0;
    ch1_reset_all();
    np_ch1_epoch = netpak_epoch();

    /* Clear any stale device IRQ state, then select the receive path. */
    np_write(NP_REG_IRQ_MASK, 0);
    np_write(NP_REG_IRQ_STATUS, 0xFFFFFFFFu); /* W1C all */
    netpak_set_irq_mode(use_irq);
    np_present = true;
    return 0;
}

void netpak_set_irq_mode(bool use_irq) {
    /* Milestone 0: cart-IRQ receive is not wired yet, so we always run poll
     * mode. Keep IRQ_MASK clear regardless of the requested mode. */
    (void)use_irq;
    np_write(NP_REG_IRQ_MASK, 0);
    np_irq_mode = false;
}

/* Raw datagram out — the ch0 path, also the wire leg of ch1. */
static s32 np_send_raw(u8 dst, u8 ch, const void *buf, s32 len) {
    u64 deadline;

    if (len < 0 || len > NETPAK_MTU) {
        return -2;
    }

    /* Wait for the TX slot (previous packet drains at the next device tick;
     * bound the spin generously in emulated time). */
    deadline = np_ticks_us() + 100000; /* 100 ms */
    while (!(np_read(NP_REG_STATUS) & NP_STATUS_TX_READY)) {
        if (np_ticks_us() > deadline) {
            return -1;
        }
    }

    if (len) {
        memcpy(np_dma_txbuf, buf, len);
        osWritebackDCache(np_dma_txbuf, np_pad16(len));
        np_dma(OS_WRITE, NP_REG_TX_BUF, np_dma_txbuf, np_pad16(len));
    }
    np_write(NP_REG_TX_LEN_DST,
             ((u32)len << 16) | ((u32)dst << 8) | ch);
    np_write(NP_REG_TX_DOORBELL, 1);
    return 0;
}

static s32 ch1_transmit(u8 peer, u16 seq, u16 ack, const void *data, u16 len) {
    static u8 pkt[NETPAK_MTU];
    pkt[0] = (u8)(seq >> 8);
    pkt[1] = (u8)seq;
    pkt[2] = (u8)(ack >> 8);
    pkt[3] = (u8)ack;
    if (len) {
        memcpy(pkt + CH1_HDR, data, len);
    }
    return np_send_raw(peer, 1, pkt, CH1_HDR + len);
}

s32 netpak_send(u8 dst, u8 ch, const void *buf, s32 len) {
    ch1_peer_t *st;
    ch1_slot_t *slot;
    u64 deadline;
    u16 seq, ack;
    s32 i;

    if (ch != 1) {
        return np_send_raw(dst, ch, buf, len);
    }

    /* ch1: reliable-ordered, unicast only (spec §8.5 lives in the library). */
    if (dst == NETPAK_BROADCAST) {
        return -2;
    }
    if (len < 0 || len > CH1_MAX_DATA) {
        return -2;
    }

    st = ch1_state(dst, true);
    if (!st) {
        return -2;
    }

    /* Find a free window slot, pumping while we wait (bounded spin). */
    slot = NULL;
    deadline = np_ticks_us() + 200000;
    for (;;) {
        for (i = 0; i < CH1_WINDOW && !slot; i++) {
            if (!st->txw[i].used) {
                slot = &st->txw[i];
            }
        }
        if (slot) {
            break;
        }
        netpak_poll(); /* drains acks; pumps retransmits */
        if (np_ticks_us() > deadline) {
            return -1; /* window stayed full */
        }
    }

    slot->used = true;
    slot->seq = st->tx_next++;
    slot->len = (u16)len;
    slot->tries = 1;
    slot->next_send_us = np_ticks_us() + CH1_RTO_US;
    memcpy(slot->data, buf, len);
    seq = slot->seq;
    ack = st->rx_expect;
    st->ack_pending = false; /* piggybacked */

    return ch1_transmit(dst, seq, ack, buf, (u16)len);
}

s32 netpak_recv(netpak_pkt_t *pkt) {
    netpak_pkt_t *slot;
    if (np_ring_head == np_ring_tail) {
        return -1;
    }
    slot = &np_ring[np_ring_head % NP_RING_DEPTH];
    memcpy(pkt, slot, sizeof(netpak_pkt_t));
    np_ring_head++;
    return 0;
}

void netpak_poll(void) {
    np_drain(); /* poll mode owns draining */
    ch1_pump(); /* ch1 timers run every frame */
}

u32 netpak_rx_dropped(void) {
    return np_rx_dropped;
}

u32 netpak_epoch(void) {
    return np_read(NP_REG_EPOCH);
}

u32 netpak_status(void) {
    return np_read(NP_REG_STATUS);
}

/* --- Mailbox helper (netpak-spec.md §5): issue CMD, poll to completion ----- */
#define NP_CMD_OK  0x02
#define NP_CMD_ERR 0x03

/* Async commands complete a device tick after a relay round-trip (real time);
 * poll generously in emulated time (spec §5.2 driver notes). */
static s32 np_cmd(u8 opcode) {
    u64 deadline;
    np_write(NP_REG_CMD, opcode);
    deadline = np_ticks_us() + 30ull * 1000 * 1000;
    for (;;) {
        u32 cs = np_read(NP_REG_CMD_STATUS);
        u8 code = cs & 0xFF;
        if (code == NP_CMD_OK) {
            return 0;
        }
        if (code == NP_CMD_ERR) {
            return -(s32)((cs >> 8) & 0xFF);
        }
        if (np_ticks_us() > deadline) {
            return -6; /* TIMEOUT */
        }
    }
}

s32 netpak_session_create(char code_out[8]) {
    s32 i;
    s32 rc = np_cmd(0x02); /* SESSION_CREATE */
    if (rc != 0) {
        return rc;
    }
    for (i = 0; i < 8; i += 4) {
        u32 w = np_read(NP_REG_CMD_DATA + i);
        code_out[i + 0] = (char)(w >> 24);
        code_out[i + 1] = (char)(w >> 16);
        code_out[i + 2] = (char)(w >> 8);
        code_out[i + 3] = (char)w;
    }
    code_out[7] = '\0';
    return (s32)np_read(NP_REG_RES0);
}

s32 netpak_session_join(const char *code) {
    u8 buf[8];
    s32 i;
    bzero(buf, sizeof(buf));
    for (i = 0; i < 6 && code[i]; i++) {
        buf[i] = (u8)code[i];
    }
    np_write(NP_REG_CMD_DATA,
             ((u32)buf[0] << 24) | ((u32)buf[1] << 16) | ((u32)buf[2] << 8) | buf[3]);
    np_write(NP_REG_CMD_DATA + 4,
             ((u32)buf[4] << 24) | ((u32)buf[5] << 16));
    i = np_cmd(0x03); /* SESSION_JOIN */
    if (i != 0) {
        return i;
    }
    return (s32)np_read(NP_REG_RES0);
}

s32 netpak_session_leave(void) {
    return np_cmd(0x04);
}

s32 netpak_set_name(const char *name) {
    u8 buf[16];
    s32 i;
    bzero(buf, sizeof(buf));
    for (i = 0; i < 15 && name[i]; i++) {
        buf[i] = (u8) name[i];
    }
    for (i = 0; i < 16; i += 4) {
        np_write(NP_REG_CMD_DATA + (u32) i, ((u32) buf[i] << 24) | ((u32) buf[i + 1] << 16) |
                                                ((u32) buf[i + 2] << 8) | buf[i + 3]);
    }
    return np_cmd(0x01); /* SET_IDENTITY (completes locally; relay notified async) */
}

s32 netpak_peers(netpak_peer_t *out, s32 max) {
    s32 count, n, i, w;
    s32 rc = np_cmd(0x05); /* LIST_PEERS */
    if (rc != 0) {
        return rc;
    }
    count = (s32)np_read(NP_REG_RES0);
    n = count < max ? count : max;
    for (i = 0; i < n; i++) {
        u32 base = NP_REG_CMD_DATA + (u32)i * 24;
        out[i].node_id = (u8)(np_read(base) >> 24);
        out[i].rtt_us = np_read(base + 4);
        for (w = 0; w < 16; w += 4) {
            u32 v = np_read(base + 8 + (u32)w);
            out[i].name[w + 0] = (char)(v >> 24);
            out[i].name[w + 1] = (char)(v >> 16);
            out[i].name[w + 2] = (char)(v >> 8);
            out[i].name[w + 3] = (char)v;
        }
        out[i].name[15] = '\0';
    }
    return count;
}

s32 netpak_time(u64 *relay_us) {
    s32 rc = np_cmd(0x07); /* GET_TIME */
    if (rc != 0) {
        return rc;
    }
    *relay_us = ((u64)np_read(NP_REG_RES0) << 32) | np_read(NP_REG_RES1);
    return 0;
}

u32 netpak_rtt_us(void) {
    s32 rc = np_cmd(0x06); /* PING */
    if (rc != 0) {
        return 0;
    }
    return np_read(NP_REG_RES0);
}
