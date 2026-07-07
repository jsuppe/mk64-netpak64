/**
 * netpak_sc64.c — SC64 USB transport backend for the NetPak64 driver.
 *
 * Emulates the NetPak64 register file (netpak-spec.md v0.9) in RDRAM and
 * keeps it coherent with a PC-side bridge daemon over the SC64's USB link.
 * The bridge owns the real device FSM (command mailbox, peer table, relay
 * UDP connection); this side is deliberately thin:
 *
 *   - CMD_DATA / ARG0 / TX_BUF writes stage locally.
 *   - A CMD write ships a 'C' frame (opcode + staged bytes) and parks
 *     CMD_STATUS on BUSY; the bridge's 'R' response completes it. The
 *     driver's np_cmd() poll loop pumps USB ingest via CMD_STATUS reads.
 *   - TX_DOORBELL ships a 'T' frame built from TX_LEN_DST + the staged
 *     TX buffer. USB writes are synchronous, so TX_READY is always set.
 *   - Inbound 'X' frames land in a local RX ring exposed through
 *     RX_COUNT / RX_LEN_SRC / RX_WIN / RX_CONSUME with drop-oldest
 *     overflow, exactly like the device (spec §4).
 *   - 'W' / 'S' frames carry LINK_UP/SESSION status, EPOCH, and the
 *     launch room code + player name registers (0x64 / 0x6C).
 *
 * Wire format (USB datatype 0x40, both directions, big-endian):
 *   { u8 op; u8 a; u16 len; u8 payload[len]; }  frames may be batched.
 *   N64->PC: 'H' hello(a=proto), 'C' cmd(a=opcode, arg0+cmd_data[16]),
 *            'T' tx(a=dst, ch + data)
 *   PC->N64: 'W' welcome(a=node, status+epoch+room[8]+name[16]),
 *            'R' cmdres(a=code, err+pad3+res0+res1+cmd_data),
 *            'X' rx(a=src, ch + data), 'S' status(status+epoch)
 */
#include <ultra64.h>
#include <string.h>
#include "netpak.h"
#include "netpak_sc64.h"
#include "usb.h"

#define NPU_DATATYPE 0x40 /* UNFLoader datatype claimed for netpak frames */

#define NPU_HELLO   'H'
#define NPU_WELCOME 'W'
#define NPU_CMD     'C'
#define NPU_CMDRES  'R'
#define NPU_TX      'T'
#define NPU_RX      'X'
#define NPU_STATUS  'S'

/* Register offsets + bit values duplicated from netpak_mk64.c — both are the
 * one spec contract (netpak-spec.md §3); keep in sync. */
#define NPS_REG_MAGIC       0x0000
#define NPS_REG_VERSION     0x0004
#define NPS_REG_STATUS      0x000C
#define NPS_REG_IRQ_STATUS  0x0010
#define NPS_REG_IRQ_MASK    0x0014
#define NPS_REG_CMD         0x0018
#define NPS_REG_CMD_STATUS  0x001C
#define NPS_REG_ARG0        0x0020
#define NPS_REG_RES0        0x0030
#define NPS_REG_RES1        0x0034
#define NPS_REG_TX_LEN_DST  0x0040
#define NPS_REG_TX_DOORBELL 0x0048
#define NPS_REG_RX_LEN_SRC  0x0050
#define NPS_REG_RX_CONSUME  0x0058
#define NPS_REG_RX_COUNT    0x005C
#define NPS_REG_EPOCH       0x0060
#define NPS_REG_ROOM        0x0064 /* ..0x0068: launch room code, 8 bytes  */
#define NPS_REG_NAME        0x006C /* ..0x0078: player name, 16 bytes      */
#define NPS_REG_CMD_DATA    0x0800
#define NPS_REG_TX_BUF      0x1000
#define NPS_REG_RX_WIN      0x2000

#define NPS_MAGIC_VALUE   0x4E503634U /* "NP64" */
#define NPS_SPEC_VERSION  0x00000009U /* v0.9: major hi16 = 0 */

#define NPS_STATUS_TX_READY (1u << 3)
#define NPS_STATUS_RX_AVAIL (1u << 2)

#define NPS_CMD_BUSY 0x01

#define NPS_CMD_DATA_SIZE 0x100 /* LIST_PEERS worst case: 8 peers x 24 B */
#define NPS_RING_DEPTH 16       /* matches the device's RX ring (spec §4) */

typedef struct {
    u16 len;
    u8 src;
    u8 ch;
    u8 data[NETPAK_MTU];
} nps_pkt_t;

/* --- shadow state (netbss is NOLOAD: everything is seeded in detect) ------ */
static u32 nps_link_seen;  /* a 'W' has arrived: bridge + relay are alive */
static u32 nps_net_status; /* LINK_UP|SESSION bits owned by the bridge */
static u32 nps_epoch;
static u32 nps_cmd_status;
static u32 nps_res0, nps_res1;
static u32 nps_arg0;
static u32 nps_tx_lendst;
static u8 nps_cmd_data[NPS_CMD_DATA_SIZE];
static u8 nps_room[8];
static u8 nps_name[16];
static u8 nps_txstage[NETPAK_MTU];

static nps_pkt_t nps_ring[NPS_RING_DEPTH];
static u32 nps_rhead, nps_rsize;

/* one inbound USB transfer: 4-byte frame header + the largest payload */
static u8 nps_scratch[4 + 1 + NETPAK_MTU + 64];
static u8 nps_txframe[4 + 1 + NETPAK_MTU];

static u64 nps_ticks_us(void) {
    return (osGetTime() * 64ull) / 3000ull;
}

static u32 nps_be32(const u8 *p) {
    return ((u32) p[0] << 24) | ((u32) p[1] << 16) | ((u32) p[2] << 8) | p[3];
}

/* --- outbound ------------------------------------------------------------- */

static void nps_send_frame(u8 op, u8 a, const u8 *payload, u16 len) {
    nps_txframe[0] = op;
    nps_txframe[1] = a;
    nps_txframe[2] = (u8)(len >> 8);
    nps_txframe[3] = (u8) len;
    if (len) {
        memcpy(nps_txframe + 4, payload, len);
    }
    usb_write(NPU_DATATYPE, nps_txframe, 4 + len);
}

/* --- inbound -------------------------------------------------------------- */

static void nps_ring_push(u8 src, u8 ch, const u8 *data, u16 len) {
    nps_pkt_t *slot;
    if (len > NETPAK_MTU) {
        len = NETPAK_MTU;
    }
    if (nps_rsize == NPS_RING_DEPTH) { /* drop-oldest (spec §4) */
        nps_rhead = (nps_rhead + 1) % NPS_RING_DEPTH;
        nps_rsize--;
    }
    slot = &nps_ring[(nps_rhead + nps_rsize) % NPS_RING_DEPTH];
    slot->src = src;
    slot->ch = ch;
    slot->len = len;
    if (len) {
        memcpy(slot->data, data, len);
    }
    nps_rsize++;
}

static void nps_dispatch(u8 op, u8 a, const u8 *p, u16 len) {
    switch (op) {
        case NPU_WELCOME:
            if (len >= 32) {
                nps_net_status = nps_be32(p);
                nps_epoch = nps_be32(p + 4);
                memcpy(nps_room, p + 8, 8);
                memcpy(nps_name, p + 16, 16);
            }
            nps_link_seen = 1;
            break;
        case NPU_STATUS:
            if (len >= 8) {
                nps_net_status = nps_be32(p);
                nps_epoch = nps_be32(p + 4);
            }
            break;
        case NPU_CMDRES:
            /* payload: err(1) pad(3) res0(4) res1(4) cmd_data(k) */
            if (len >= 12) {
                u16 k = len - 12;
                nps_res0 = nps_be32(p + 4);
                nps_res1 = nps_be32(p + 8);
                if (k > NPS_CMD_DATA_SIZE) {
                    k = NPS_CMD_DATA_SIZE;
                }
                if (k) {
                    memcpy(nps_cmd_data, p + 12, k);
                }
                /* code in a, errno in payload[0] -> CMD_STATUS layout §3.4 */
                nps_cmd_status = (u32) a | ((u32) p[0] << 8);
            }
            break;
        case NPU_RX:
            if (len >= 1) {
                nps_ring_push(a, p[0], p + 1, (u16)(len - 1));
            }
            break;
        default:
            break;
    }
}

/* Drain every pending USB transfer; parse the netpak frames inside. */
static void nps_pump(void) {
    u32 header;
    s32 guard = 8;
    while (guard-- > 0 && (header = usb_poll()) != 0) {
        int size = (int) USBHEADER_GETSIZE(header);
        if (USBHEADER_GETTYPE(header) != NPU_DATATYPE || size <= 0
            || size > (int) sizeof(nps_scratch)) {
            usb_purge();
            continue;
        }
        usb_read(nps_scratch, size);
        {
            int off = 0;
            while (off + 4 <= size) {
                u8 op = nps_scratch[off];
                u8 a = nps_scratch[off + 1];
                u16 plen = (u16)(((u16) nps_scratch[off + 2] << 8) | nps_scratch[off + 3]);
                if (off + 4 + plen > size) {
                    break; /* truncated frame: discard the tail */
                }
                nps_dispatch(op, a, nps_scratch + off + 4, plen);
                off += 4 + plen;
            }
        }
    }
}

/* --- public backend API ---------------------------------------------------- */

s32 np_sc64_detect(void) {
    s32 tries;

    if (!usb_initialize()) {
        return -1;
    }
    if (usb_getcart() != CART_SC64) {
        return -2;
    }

    /* netbss is NOLOAD — nothing here can rely on zero-initialization. */
    nps_link_seen = 0;
    nps_net_status = 0;
    nps_epoch = 0;
    nps_cmd_status = 0;
    nps_res0 = nps_res1 = 0;
    nps_arg0 = 0;
    nps_tx_lendst = 0;
    nps_rhead = nps_rsize = 0;
    bzero(nps_cmd_data, sizeof(nps_cmd_data));
    bzero(nps_room, sizeof(nps_room));
    bzero(nps_name, sizeof(nps_name));

    /* The bridge answers any HELLO with WELCOME, so a bridge started before
     * the console powers on is caught on the first try. */
    for (tries = 0; tries < 3; tries++) {
        u64 deadline = nps_ticks_us() + 1000000; /* 1 s per try */
        nps_send_frame(NPU_HELLO, 1, NULL, 0);
        while (!nps_link_seen && nps_ticks_us() < deadline) {
            nps_pump();
        }
        if (nps_link_seen) {
            return 0;
        }
    }
    return -3;
}

u32 np_sc64_read(u32 off) {
    if (off >= NPS_REG_CMD_DATA && off + 3 < NPS_REG_CMD_DATA + NPS_CMD_DATA_SIZE) {
        return nps_be32(nps_cmd_data + (off - NPS_REG_CMD_DATA));
    }
    if (off >= NPS_REG_ROOM && off <= NPS_REG_ROOM + 4) {
        return nps_be32(nps_room + (off - NPS_REG_ROOM));
    }
    if (off >= NPS_REG_NAME && off <= NPS_REG_NAME + 12) {
        return nps_be32(nps_name + (off - NPS_REG_NAME));
    }
    switch (off) {
        case NPS_REG_MAGIC:
            return NPS_MAGIC_VALUE;
        case NPS_REG_VERSION:
            return NPS_SPEC_VERSION;
        case NPS_REG_STATUS:
            nps_pump();
            return nps_net_status | NPS_STATUS_TX_READY
                 | (nps_rsize ? NPS_STATUS_RX_AVAIL : 0);
        case NPS_REG_CMD_STATUS:
            nps_pump(); /* np_cmd()'s poll loop drives ingest through here */
            return nps_cmd_status;
        case NPS_REG_RX_COUNT:
            nps_pump(); /* np_drain()'s loop condition drives ingest here */
            return nps_rsize;
        case NPS_REG_RX_LEN_SRC: {
            const nps_pkt_t *h;
            if (nps_rsize == 0) {
                return 0;
            }
            h = &nps_ring[nps_rhead];
            return ((u32) h->len << 16) | ((u32) h->src << 8) | h->ch;
        }
        case NPS_REG_EPOCH:
            nps_pump();
            return nps_epoch;
        case NPS_REG_RES0:
            return nps_res0;
        case NPS_REG_RES1:
            return nps_res1;
        case NPS_REG_ARG0:
            return nps_arg0;
        default:
            return 0;
    }
}

void np_sc64_write(u32 off, u32 v) {
    if (off >= NPS_REG_CMD_DATA && off + 3 < NPS_REG_CMD_DATA + NPS_CMD_DATA_SIZE) {
        u8 *d = nps_cmd_data + (off - NPS_REG_CMD_DATA);
        d[0] = (u8)(v >> 24);
        d[1] = (u8)(v >> 16);
        d[2] = (u8)(v >> 8);
        d[3] = (u8) v;
        return;
    }
    switch (off) {
        case NPS_REG_CMD: {
            u8 body[20];
            body[0] = (u8)(nps_arg0 >> 24);
            body[1] = (u8)(nps_arg0 >> 16);
            body[2] = (u8)(nps_arg0 >> 8);
            body[3] = (u8) nps_arg0;
            memcpy(body + 4, nps_cmd_data, 16);
            nps_cmd_status = NPS_CMD_BUSY;
            nps_send_frame(NPU_CMD, (u8)(v & 0xFF), body, sizeof(body));
            break;
        }
        case NPS_REG_ARG0:
            nps_arg0 = v;
            break;
        case NPS_REG_TX_LEN_DST:
            nps_tx_lendst = v;
            break;
        case NPS_REG_TX_DOORBELL: {
            u16 len = (u16)(nps_tx_lendst >> 16);
            u8 dst = (u8)((nps_tx_lendst >> 8) & 0xFF);
            static u8 body[1 + NETPAK_MTU];
            if (len > NETPAK_MTU) {
                len = NETPAK_MTU;
            }
            body[0] = (u8)(nps_tx_lendst & 0xFF); /* channel */
            if (len) {
                memcpy(body + 1, nps_txstage, len);
            }
            nps_send_frame(NPU_TX, dst, body, (u16)(1 + len));
            break;
        }
        case NPS_REG_RX_CONSUME:
            if (nps_rsize) {
                nps_rhead = (nps_rhead + 1) % NPS_RING_DEPTH;
                nps_rsize--;
            }
            break;
        default:
            /* IRQ_MASK / IRQ_STATUS and the rest: no device IRQ line over
             * USB — poll mode only, writes are accepted and ignored. */
            break;
    }
}

void np_sc64_dma(s32 dir, u32 cart_off, void *dram, u32 len) {
    if (dir == OS_WRITE && cart_off == NPS_REG_TX_BUF) {
        if (len > NETPAK_MTU) {
            len = NETPAK_MTU;
        }
        memcpy(nps_txstage, dram, len);
        return;
    }
    if (dir == OS_READ && cart_off == NPS_REG_RX_WIN) {
        const nps_pkt_t *h = nps_rsize ? &nps_ring[nps_rhead] : NULL;
        u32 have = h ? h->len : 0;
        u32 n = (len < have) ? len : have;
        if (n) {
            memcpy(dram, h->data, n);
        }
        if (len > n) { /* padded read past the packet: zero-fill like RX_WIN */
            bzero((u8 *) dram + n, len - n);
        }
        /* The caller invalidates the destination after a "DMA"; push our CPU
         * writes to RDRAM first so that invalidate doesn't discard them. */
        osWritebackDCache(dram, len);
        return;
    }
}
