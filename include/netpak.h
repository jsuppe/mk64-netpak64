/**
 * netpak.h — NetPak64 network device driver, libultra port for mk64.
 *
 * The upstream driver (github.com/jsuppe/libnetpak) targets libdragon; it
 * cannot be linked into a libultra ROM. This is a from-scratch reimplementation
 * of the SAME register-level device contract (netpak-spec.md v0.9) using mk64's
 * own libultra primitives. The public API below is kept identical to
 * libnetpak's so netcode written against either is portable.
 *
 * Milestone 0 scope: probe, init (Dom2 timing + poll-mode receive), ch0
 * datagram send/recv, the mailbox command set (sessions/peers/time), and the
 * reliable ch1 channel. The cart-line IRQ receive path is deferred — poll mode
 * (netpak_poll() once per frame) is the supported path for now.
 */
#ifndef NETPAK_H
#define NETPAK_H

#include <ultra64.h>

/** Device window base as a physical PI bus address (netpak-spec.md §2). */
#define NETPAK_BASE_PHYS 0x05F00000U

/** Payload MTU (netpak-spec.md §3). */
#define NETPAK_MTU 1024

/** Broadcast destination node id (netpak-spec.md §3, §8.5). */
#define NETPAK_BROADCAST 0xFF

typedef struct {
    u8  src;               /**< source node id */
    u8  ch;                /**< channel (0 = unreliable datagram) */
    u16 len;               /**< payload length in bytes */
    u8  data[NETPAK_MTU];  /**< payload */
} netpak_pkt_t;

/** Hardened three-step probe (spec §2.2). Safe on a stock console. */
bool netpak_detect(void);

/** Raw VERSION register (major hi / minor lo); valid after detect. */
u32 netpak_version(void);

/** Cached result of the last successful netpak_init() — true if the device is
 *  present and initialized. Cheap; no bus access. */
bool netpak_present(void);

/** Debug: write a 32-bit value to the device CMD_DATA scratch register so it
 *  shows up in ares' NP64_TRACE_IO log (`io wW 0800 <- v`). A poor-man's printf
 *  that needs no ISViewer/is_debug plumbing. */
void netpak_debug_poke(u32 v);

/** Print a string to ares' IS-Viewer (mirrored to stdout with ARES_ISV=1) via a
 *  direct cartridge-domain write — a headless printf channel that works where
 *  the stock osSyncPrintf does not. No-op on real hardware without an IS-Viewer. */
void netpak_isv_print(const char *s);

/** Read the launch room code (ares NP64_ROOM env) into out[8], NUL-terminated.
 *  out[0]=='\0' if none was provided. Used to pre-fill the online join code. */
void netpak_get_room_code(char out[8]);

/**
 * Initialize the device: program the spec-pinned Dom2 bus timings (§2.1),
 * clear device IRQ state, and select the receive path.
 *
 * @param use_irq true is reserved for the deferred cart-IRQ path; milestone 0
 *                always runs poll mode regardless (call netpak_poll() per frame)
 * @return 0 on success, -1 if the device is not present.
 */
s32 netpak_init(bool use_irq);

/** Switch receive path at runtime (poll-only in milestone 0). */
void netpak_set_irq_mode(bool use_irq);

/**
 * Send one datagram on channel `ch` to node `dst` (0xFF = broadcast).
 * @return 0 on success, -1 on timeout waiting for TX_READY, -2 bad length.
 */
s32 netpak_send(u8 dst, u8 ch, const void *buf, s32 len);

/**
 * Non-blocking receive from the driver's RDRAM ring.
 * @return 0 and fills *pkt, or -1 if no packet is pending.
 */
s32 netpak_recv(netpak_pkt_t *pkt);

/** Poll-mode pump: drain device RX into the RDRAM ring + run ch1 timers.
 *  Call once per frame. */
void netpak_poll(void);

/** Packets dropped because the driver RDRAM ring overflowed (drop-oldest). */
u32 netpak_rx_dropped(void);

/**
 * Session-discontinuity counter (EPOCH register, netpak-spec.md §3, §7).
 * Increments on emulator loadstate, console soft reset, and relay reconnect.
 * Poll once per frame; on change, run your netcode's resync path.
 */
u32 netpak_epoch(void);

/** STATUS register bits (netpak-spec.md §3.2). */
#define NETPAK_STATUS_LINK_UP  (1u << 0)
#define NETPAK_STATUS_SESSION  (1u << 1)
#define NETPAK_STATUS_RX_AVAIL (1u << 2)
#define NETPAK_STATUS_TX_READY (1u << 3)

/** Raw STATUS register. Poll LINK_UP|SESSION after init when using a relay
 *  backend — the config-driven room join completes asynchronously. */
u32 netpak_status(void);

/* --- Sessions & peers (mailbox commands, netpak-spec.md §5) --------------- */

typedef struct {
    u8   node_id;
    u32  rtt_us;    /* 0 in v0.9 — per-peer RTT is a later refinement */
    char name[16];  /* NUL-terminated */
} netpak_peer_t;

/** Create a new room. Blocks (polling) until the relay responds or times out.
 *  On success writes the 6-char join code + NUL into code_out and returns the
 *  assigned node id (>= 0); negative errno otherwise. */
s32 netpak_session_create(char code_out[8]);

/** Join a room by 6-char code (join-or-create in v0.9). Returns node id or
 *  negative errno. */
s32 netpak_session_join(const char *code);

/** Leave the current room (stays connected to the relay). 0 or -errno. */
s32 netpak_session_leave(void);

/** Fill up to max entries; returns peer count (not counting yourself) or
 *  negative errno (-NETPAK_ERR_NOTJOINED when not in a session). */
s32 netpak_peers(netpak_peer_t *out, s32 max);

/** Relay-anchored clock: microseconds since the relay started (u64), shared
 *  by all room members. Returns 0 on success, -errno otherwise. */
s32 netpak_time(u64 *relay_us);

/** Last relay RTT sample in microseconds (0 until the transport has one). */
u32 netpak_rtt_us(void);

/** Errno values a mailbox command can return, negated (spec §3.4.1). */
#define NETPAK_ERR_BUSY      2
#define NETPAK_ERR_NOTJOINED 3
#define NETPAK_ERR_ALREADY   4
#define NETPAK_ERR_TIMEOUT   6
#define NETPAK_ERR_RELAY     8

#endif /* NETPAK_H */
