/**
 * net_race.c — NetPak64 racing netcode, milestone A + interpolation.
 *
 * Own kart = player slot 0 (simulated + rendered locally, untouched). Each
 * frame we broadcast a compact snapshot of slot 0 on ch0 (unreliable,
 * newest-wins). Inbound snapshots are mapped by sender node id to puppet slots
 * 1..3 and applied at the per-player update seam (func_80028E70) so the remote
 * kart's transform comes straight off the wire.
 *
 * Puppets are INTERPOLATED, not snapped: each frame we linearly extrapolate
 * from the two most recent snapshots over their measured arrival interval. This
 * is unit-free (works in whatever units pos/yaw are), self-tuning to the real
 * packet rate, adds no fixed latency, and coasts smoothly through a dropped
 * packet (the extrapolation parameter simply runs past 1.0, clamped).
 *
 * In loopback the device echoes our own broadcast, so a single instance shows
 * slot 1 shadowing slot 0 — a full end-to-end test of the tx/rx/puppet path.
 * With a relay + a second instance, slot 1 is the other player.
 */
#include <ultra64.h>
#include <string.h>
#include "common_structs.h"
#include "defines.h"
#include "kart_dma.h" /* load_kart_palette (character sync) */
#include "netpak.h"
#include "net_race.h"
#include "objects.h" /* gObjectList — Path A start-state pinpoint diagnostic */
#include "camera.h"  /* Camera, camera1, func_8001EE98 — lockstep local chase-cam */
#include "code_800029B0.h" /* D_800DC5EC (course renderer wrapper) — segment culling */
#include "path.h"          /* gTrackPaths / gNearestPathPointByPlayerId — waypoint autodrive */

extern u16 atan2s(f32, f32);            /* racing/math_util.h */
extern u16 gPathCountByPathIndex[];     /* points per path (path.h D_801645C8) */
extern hud_player playerHUD[];          /* itemOverride — forced-item test */

extern f32 gCourseTimer;
extern u16 D_80152300[]; /* per-camera mode array written by the camera follow */

/* Boundary symbols of cpu_vehicles_camera_path.c's per-CPU state block (all its
 * .bss laid out contiguously): unk_..._pad is the first, cpu_ItemStrategy the
 * last (CpuItemStrategyData[NUM_PLAYERS], 0x10 each => 0x80 bytes). Declared as
 * byte arrays here purely to take their linked addresses; the real types live in
 * cpu_vehicles_camera_path.h. Used to zero the block at lockstep race entry so
 * the CPU sim starts identical on every console (see net_lockstep_reset). */
extern u8 unk_cpu_vehicles_camera_path_pad[];
extern u8 cpu_ItemStrategy[];
#define LS_CPU_STATE_END (cpu_ItemStrategy + 8 * 0x10) /* NUM_PLAYERS * sizeof(CpuItemStrategyData) */

extern Player gPlayers[];
extern struct Controller gControllers[];
extern s32 gGamestate;
extern s32 gGlobalTimer;
extern s32 gMenuSelection;
extern s8  gMainMenuSelection;
extern void osSyncPrintf(const char* fmt, ...); /* declared in PR/os.h (not via ultra64.h) */

/* Temporary headless test: auto-navigate the menus to the mode-select screen
 * and pick ONLINE, so Increment 1 can be verified without a controller. Set to
 * 0 to disable. Pokes gMenuSelection (0xE0..) + a 0xFF..F success marker to
 * NP64_TRACE_IO. */
#define NET_MENU_TEST 0
#define NET_DETECTOR_SELFTEST 0 /* 1 = poison one sim mid-race to prove 0x7D fires */
#define NET_ITEM_PROBE 0        /* 1 = per-console item grants (DIVERGENT by design) */ /* 1 = poison one sim mid-race to prove 0x7D fires */
/* NET_DIAG: keep the render/position forensics pokes in HUMAN builds so a
 * player's own session (launcher sets NP64_TRACE_IO=1) captures the evidence
 * for on-device-only defects. Pokes are single register writes — negligible
 * without tracing. No autopilot, no sim impact. */
#define NET_DIAG 1

/* RETIRED (task #34): the RenderSave/camera-array render-write isolation.
 * It predates RENDER PACING, which locks render:sim 1:1 and makes render-rate
 * writes schedule-deterministic by construction — campaign verified 3/3
 * bit-identical with this OFF. Worse, the isolation itself was the "host
 * black screen / invisible karts" bug: restoring animFrameSelector after
 * every render permanently desynced the kart sprite streaming cache
 * (gLastAnimFrameSelector in render_player.c compares against values the
 * restore kept rewinding), starving kart textures — garbled HUD glyphs,
 * invisible karts, and a fully void 3D scene on the console whose camera
 * watches kart 0. Kept compiled-out for reference / emergency re-enable. */
#define NET_RENDER_ISOLATION 0

/* Determinism probe: run an identical OFFLINE race on two instances with
 * identical scripted inputs and NO networking, hashing all 8 karts' sim state
 * each frame. If the two hash streams match frame-for-frame, MK64-on-ares is
 * deterministic → lockstep (shared inputs) is viable. Navigator targets GRAND
 * PRIX (not ONLINE) and net_race_frame emits (frame,hash) instead of tx/rx. */
#define NET_DETERMINISM_TEST 0

static void net_race_menu_test(void) {
#if NET_MENU_TEST
    static u32 tick;
    static s32 downs;
    u16 press = 0;

    if (!netpak_present() || gGamestate == RACING) {
        return;
    }
    if ((tick++ % 18) != 0) {
        return; /* one action per ~18 frames so fades settle */
    }
    netpak_debug_poke(0xE0000000u | (u32)(gMenuSelection & 0xFF));
    switch (gMenuSelection) {
        case 8:  /* LOGO_INTRO_MENU */
        case 10: /* START_MENU */
            press = START_BUTTON;
            break;
        case 9: { /* CONTROLLER_PAK_MENU: move SELECT_RECORD -> END (R), then A to exit */
            static s32 pakStep;
            if (pakStep == 0) { press = R_JPAD; pakStep = 1; }
            else { press = A_BUTTON; pakStep = 0; }
            break;
        }
        case 11: /* MAIN_MENU */
#if NET_DETERMINISM_TEST
            press = A_BUTTON; /* step player -> mode(GP) -> CC class -> OK with defaults */
#else
            if (gMainMenuSelection == 3) {        /* PLAYER_SELECT: confirm 1P */
                press = A_BUTTON;
            } else if (gMainMenuSelection == 4) { /* MODE_SELECT: down to ONLINE then confirm */
                static s32 dGap;
                if (downs < 2) { /* alternate press/release so each D is an edge */
                    if ((dGap++ & 1) == 0) { press = D_JPAD; downs++; } else { press = 0; }
                }
                else { press = A_BUTTON; }
            } else if (gMainMenuSelection == 5) { /* MODE_SUB_SELECT: the CC
                    class sub-menu ONLINE now routes through (v27) — confirm
                    the default 50cc and continue to the online screen */
                press = A_BUTTON;
            }
#endif
            break;
        case 12: { /* CHARACTER_SELECT_MENU: pick a per-instance character (cursor
                    * right node-id times), then confirm + OK — distinct picks
                    * exercise the character-sync path the way real players do. */
            extern s32 net_menu_node_id(void);
            static s32 chMoves;
            s32 myNode = net_menu_node_id();
            if (myNode > 0 && chMoves < myNode && chMoves < 7) {
                press = R_JPAD;
                chMoves++;
            } else {
                press = A_BUTTON;
            }
            break;
        }
        case 13: /* COURSE_SELECT_MENU */
#if NET_DETERMINISM_TEST
            press = A_BUTTON;                     /* GP: cup -> OK -> launch */
#endif
            break;
        case NETWORK_VS_MENU: { /* join the preset room, then START. Instances still
                                 * carrying the default identity ("player") first walk
                                 * the NAME editor — exercises rename end-to-end without
                                 * touching harness/bot instances named via NP64_NAME. */
            static s32 onlineStep;
            static s32 doRename = -1;
            static s32 isAlice;
            if (doRename < 0) {
                char nm[16];
                netpak_get_name(nm);
                doRename = (nm[0] == 'p' && nm[1] == 'l' && nm[2] == 'a' && nm[3] == 'y' &&
                            nm[4] == 'e' && nm[5] == 'r' && nm[6] == '\0');
                isAlice = (nm[0] == 'a' && nm[1] == 'l' && nm[2] == 'i' && nm[3] == 'c' &&
                           nm[4] == 'e' && nm[5] == '\0');
            }
            netpak_debug_poke(0xF0000000u | (u32)(onlineStep & 0xFF));
            if (doRename) {
                switch (onlineStep) {
                    case 1: press = D_JPAD;        break; /* cursor HOST -> JOIN */
                    case 2: press = D_JPAD;        break; /* JOIN -> NAME */
                    case 3: press = A_BUTTON;      break; /* open the name editor */
                    case 4: press = U_JPAD;        break; /* PLAYER -> QLAYER */
                    case 5: press = A_BUTTON;      break; /* save -> SET_IDENTITY */
                    case 6: press = U_JPAD;        break; /* cursor NAME -> JOIN */
                    case 7: press = A_BUTTON;      break; /* JOIN -> code entry (pre-filled) */
                    case 8: press = A_BUTTON;      break; /* confirm code -> OM_JOINED */
                    case 13: press = START_BUTTON; break; /* self-start -> character select */
                }
            } else {
                switch (onlineStep) {
                    case 1: press = D_JPAD;        break; /* cursor HOST -> JOIN */
                    case 2: press = A_BUTTON;      break; /* JOIN -> code entry (pre-filled) */
                    case 3: press = A_BUTTON;      break; /* confirm code -> OM_JOINED */
                    default:
                        /* With explicit join (v4+) the room fills over ~10-30s as
                         * staggered instances walk the menus — starting blind put
                         * one kart in a race of CPUs. Only "alice" starts, once
                         * the room is full (8) or after a generous fallback; the
                         * barrier's latecomer adoption covers any straggler. Bot
                         * instances (bot1..7) therefore never self-start a
                         * human's race. */
                        if (isAlice && onlineStep >= 8 && (onlineStep % 4) == 0) {
                            if (net_menu_player_count() >= 8 || onlineStep >= 150) {
                                press = START_BUTTON;
                            }
                        }
                        break;
                }
            }
            onlineStep++;
            break;
        }
    }
    if (press) {
        gControllers[0].buttonPressed |= press;
        gControllers[0].button |= press;
        gControllers[0].stickPressed |= press;
    }
#endif
}

/* Demo aid: with ares' Input/Driver=None the local kart never accelerates, so
 * a networked race just sits on the start grid. With this enabled, exactly ONE
 * of two paired instances auto-drives forward after the countdown (chosen by
 * the peer's node id parity), so the other instance visibly watches the remote
 * kart drive off under network control. Set to 0 for real controller input.
 *
 * DISABLED pending a fix: the menu-bypass launch (netpak_launch_race) doesn't
 * cleanly complete mk64's race-intro state machine (start_race()/D_800DC510),
 * so player 0 stays flagged PLAYER_START_SEQUENCE and input routes to the
 * start-sequence handler instead of the driving path — the kart won't throttle.
 * Diagnosed by poking gPlayers[0].type (0xE200) to a device register visible
 * in NP64_TRACE_IO. Fix = drive the intro state machine properly (see task). */
#define NET_DEMO_AUTODRIVE 0
#define NET_AUTODRIVE_AFTER_FRAMES 45 /* just after start_race() forces GO */

/* Input overlay (test builds): last inputs the autodrive injected for the LOCAL
 * kart, drawn as a stick bar + button lights over the race view (cam_pop). Each
 * console shows its OWN injected inputs — makes 'are both karts driven by the
 * same inputs?' answerable at a glance. */
static s8  sAdOverlayStick;
static u16 sAdOverlayBtn;

/* --- Wire format (ch0 payload; opaque to the relay) ----------------------- */
/* Both peers run the identical ROM, so a raw struct is safe: same field
 * layout, same big-endian byte order. Kept small — 36 bytes, well under the
 * 1024 B ch0 MTU, comfortable at 30 Hz for 2-4 players (spec §7 budget). */
#define NETKART_TAG 0x4B /* 'K' */
#define NETKART_VER 2

typedef struct {
    u8  tag;
    u8  ver;
    u16 seq;         /* newest-wins, wraparound compare */
    u16 characterId; /* carried for later; not applied in milestone A */
    s16 rotY;        /* rotation[1] (yaw) — drives the billboard sprite */
    f32 posX, posY, posZ;
    f32 velX, velY, velZ;
    f32 speed;
    u32 effects;     /* Player.effects — star sparkle, boost, mini-turbo, spinout,
                      * squish/shrink; drives the puppet's visual state */
} NetKartSnapshot; /* 40 bytes */

/* --- ch1 event wire format (reliable-ordered) ----------------------------- */
/* ch1 is unicast + reliable + in-order per peer (spec §10). Discrete race
 * events that must not be missed or reordered — lap completions, finish/
 * placement — go here, sent to every known peer. */
#define NETEV_TAG    0x45 /* 'E' */
#define NETEV_LAP    1     /* crossed the line onto a new lap */
#define NETEV_FINISH 2     /* finished the race */
#define NETEV_HIT    3     /* "your kart was hit by my item" (unicast to victim) */

typedef struct {
    u8  tag;  /* NETEV_TAG */
    u8  type; /* NETEV_* */
    u8  node; /* sender node id (also available as pkt.src) */
    u8  pad;
    s16 lap;  /* lap count at the time of the event (LAP/FINISH) */
    s16 rank; /* current placement (LAP/FINISH) */
    u32 bits; /* NETEV_HIT: trigger bitmask to apply to the victim's kart */
} NetEvent; /* 12 bytes */

/* Item-hit triggers we replicate: green/red/blue shell, banana, fake item box,
 * star bump, and lightning. All are player-caused (one player's item strikes
 * another), so forwarding them to the victim is correct. Deliberately excludes
 * course-hazard triggers (thwomp, etc.) — those are simulated identically on
 * each console and must not be double-applied. */
#define NET_HIT_MASK (HIGH_TUMBLE_TRIGGER | LOW_TUMBLE_TRIGGER | \
                      HIT_BANANA_TRIGGER | VERTICAL_TUMBLE_TRIGGER | \
                      HIT_BY_STAR_TRIGGER | LIGHTNING_STRIKE_TRIGGER)

/* --- Replication state ---------------------------------------------------- */
#define NET_MAX_SLOTS 8      /* slot 0 = local; 1..7 = remote puppets (room cap) */
#define NET_ALPHA_MAX 2.0f   /* clamp extrapolation to 2 intervals on packet loss */

typedef struct {
    bool valid;
    u16  seq;

    /* Two most recent snapshots + their arrival times (us), for interpolation. */
    bool havePrev;
    f32  prevPos[3];
    s16  prevYaw;
    u32  prevTime;
    f32  curPos[3];
    s16  curYaw;
    u32  curTime;
    f32  speed;
    u16  characterId;
    u32  effects; /* remote's Player.effects — replicated to the puppet's visuals */

    /* Render transform computed once per frame, consumed by the puppet apply. */
    f32  renderPos[3];
    s16  renderYaw;

    /* Latest race-flow state received over ch1 (for standings/HUD later). */
    s16  lap;
    s16  rank;
    bool finished;
} RemoteSlot;

static RemoteSlot sRemote[NUM_PLAYERS];
static s16        sSrcSlot[256]; /* sender node id -> puppet slot, -1 = unknown */
static u8         sPeerNode[NET_MAX_SLOTS]; /* puppet slot -> peer node id */
static bool       sPeerUsed[NET_MAX_SLOTS];
static s32        sNextSlot;     /* next puppet slot to hand out (1..) */
static u16        sTxSeq;
static s32        sPrevGamestate;
static u32        sNetEpoch;

/* Local race-flow state, to detect changes worth broadcasting on ch1. */
static s16        sLastLap;
static bool       sSentFinish;
static u32        sRaceFrames; /* rendered frames since entering the race */

/* Auto-start latch (separate from per-race reset so we never re-trigger). */
static bool       sAutoStarted;
static u32        sLinkUpUs;

#define NET_AUTOSTART_BOOT_FRAMES 150       /* let boot/logo finish (~5 s) */
#define NET_AUTOSTART_LOOPBACK_US 3000000u  /* LINK_UP-with-no-SESSION grace */

/* microseconds of emulated time (matches the driver's clock), truncated to
 * u32. Only ever used for small frame-interval deltas via modular subtraction,
 * so the ~71-minute wrap is harmless — and staying 32-bit keeps the alpha math
 * off the 64-bit-to-float libgcc path. */
static u32 net_now_us(void) {
    return (u32)((osGetTime() * 64ull) / 3000ull);
}

void net_race_reset(void) {
    s32 i;
    bzero(sRemote, sizeof(sRemote));
    bzero(sPeerNode, sizeof(sPeerNode));
    bzero(sPeerUsed, sizeof(sPeerUsed));
    for (i = 0; i < 256; i++) {
        sSrcSlot[i] = -1;
    }
    sNextSlot = 1;
    sTxSeq = 0;
    sLastLap = -1;
    sSentFinish = false;
}

/* Map a sender node id to a stable puppet slot, allocating on first sight. */
static s32 net_race_src_slot(u8 src) {
    s16 slot = sSrcSlot[src];
    if (slot >= 0) {
        return slot;
    }
    if (sNextSlot >= NET_MAX_SLOTS) {
        return -1; /* room fuller than we can puppet; ignore extras */
    }
    slot = (s16)sNextSlot++;
    sSrcSlot[src] = slot;
    sPeerNode[slot] = src; /* remember the node id so we can ch1-unicast to it */
    sPeerUsed[slot] = true;
    return slot;
}

/* Reliable broadcast: ch1 is unicast-only (spec §11), so send one copy to each
 * known peer. Peers are learned from inbound ch0 snapshots. */
static void net_event_send_all(const NetEvent* ev) {
    s32 i;
    for (i = 1; i < NET_MAX_SLOTS; i++) {
        if (sPeerUsed[i]) {
            netpak_send(sPeerNode[i], 1, ev, sizeof(*ev));
        }
    }
}

/* Reliable unicast to one peer node. */
static void net_event_send_one(u8 node, const NetEvent* ev) {
    netpak_send(node, 1, ev, sizeof(*ev));
}

/* Handle a ch1 race-flow event from a peer. */
static void net_event_ingest(u8 src, const u8* data, u16 len) {
    NetEvent ev;
    s32 slot;
    RemoteSlot* r;

    if (len < sizeof(NetEvent)) {
        return;
    }
    memcpy(&ev, data, sizeof(ev));
    if (ev.tag != NETEV_TAG) {
        return;
    }

    /* A hit reported by a peer whose item struck our kart on their screen:
     * apply the reaction to our own kart (slot 0) and let our normal
     * apply_triggers pipeline play it out (spin/tumble, sound, camera). Masked
     * to known item triggers so a corrupt packet can't inject arbitrary bits. */
    if (ev.type == NETEV_HIT) {
        gPlayers[0].triggers |= (ev.bits & NET_HIT_MASK);
        osSyncPrintf("netpak: got HIT from node %d (bits %08x)\n", src, ev.bits);
        return;
    }

    slot = net_race_src_slot(src);
    if (slot < 0) {
        return;
    }
    r = &sRemote[slot];
    r->lap = ev.lap;
    r->rank = ev.rank;
    if (ev.type == NETEV_FINISH) {
        r->finished = true;
        osSyncPrintf("netpak: peer node %d FINISHED (place %d)\n", src, ev.rank);
    } else {
        osSyncPrintf("netpak: peer node %d reached lap %d (place %d)\n", src, ev.lap, ev.rank);
    }
}

static void net_race_tx(void) {
    NetKartSnapshot s;
    Player* me = &gPlayers[0];

    s.tag = NETKART_TAG;
    s.ver = NETKART_VER;
    s.seq = sTxSeq++;
    s.characterId = me->characterId;
    s.rotY = me->rotation[1];
    s.posX = me->pos[0];
    s.posY = me->pos[1];
    s.posZ = me->pos[2];
    s.velX = me->velocity[0];
    s.velY = me->velocity[1];
    s.velZ = me->velocity[2];
    s.speed = me->speed;
    s.effects = me->effects;

    netpak_send(NETPAK_BROADCAST, 0, &s, sizeof(s));
}

static void net_race_rx(void) {
    static netpak_pkt_t pkt; /* 1 KB — keep off the stack */
    NetKartSnapshot s;
    s32 slot;
    RemoteSlot* r;
    u32 now;

    while (netpak_recv(&pkt) == 0) {
        if (pkt.ch == 1) {
            net_event_ingest(pkt.src, pkt.data, pkt.len); /* reliable race events */
            continue;
        }
        if (pkt.ch != 0) {
            continue;
        }
        if (pkt.len < sizeof(NetKartSnapshot)) {
            continue;
        }
        memcpy(&s, pkt.data, sizeof(s)); /* copy out; pkt.data may be unaligned */
        if (s.tag != NETKART_TAG || s.ver != NETKART_VER) {
            continue;
        }
        slot = net_race_src_slot(pkt.src);
        if (slot < 0) {
            continue;
        }
        r = &sRemote[slot];
        if (r->valid && (s16)(s.seq - r->seq) <= 0) {
            continue; /* older/duplicate — newest-wins */
        }

        now = net_now_us();
        if (!r->valid) {
            /* First snapshot: seed both endpoints so interpolation is a no-op
             * until the second arrives. */
            r->prevPos[0] = r->curPos[0] = s.posX;
            r->prevPos[1] = r->curPos[1] = s.posY;
            r->prevPos[2] = r->curPos[2] = s.posZ;
            r->prevYaw = r->curYaw = s.rotY;
            r->prevTime = r->curTime = now;
            r->havePrev = false;
        } else {
            /* Slide the window: current becomes previous, new becomes current. */
            r->prevPos[0] = r->curPos[0];
            r->prevPos[1] = r->curPos[1];
            r->prevPos[2] = r->curPos[2];
            r->prevYaw = r->curYaw;
            r->prevTime = r->curTime;
            r->curPos[0] = s.posX;
            r->curPos[1] = s.posY;
            r->curPos[2] = s.posZ;
            r->curYaw = s.rotY;
            r->curTime = now;
            r->havePrev = true;
        }
        r->speed = s.speed;
        r->characterId = s.characterId;
        r->effects = s.effects;
        r->valid = true;
        r->seq = s.seq;
    }
}

/* Compute each puppet's render transform for this frame by extrapolating from
 * its two most recent snapshots. alpha = fraction of a snapshot interval that
 * has elapsed since the latest snapshot; render = cur + (cur - prev) * alpha. */
static void net_race_interpolate(void) {
    u32 now = net_now_us();
    s32 i;

    for (i = 1; i < NUM_PLAYERS; i++) {
        RemoteSlot* r = &sRemote[i];
        f32 alpha;
        u32 interval;
        s16 dYaw;

        if (!r->valid) {
            continue;
        }
        if (!r->havePrev) {
            r->renderPos[0] = r->curPos[0];
            r->renderPos[1] = r->curPos[1];
            r->renderPos[2] = r->curPos[2];
            r->renderYaw = r->curYaw;
            continue;
        }

        interval = r->curTime - r->prevTime; /* modular u32 subtraction */
        if (interval == 0) {
            alpha = 0.0f;
        } else {
            alpha = (f32)(s32)(now - r->curTime) / (f32)(s32)interval;
            if (alpha < 0.0f) {
                alpha = 0.0f;
            }
            if (alpha > NET_ALPHA_MAX) {
                alpha = NET_ALPHA_MAX;
            }
        }

        r->renderPos[0] = r->curPos[0] + (r->curPos[0] - r->prevPos[0]) * alpha;
        r->renderPos[1] = r->curPos[1] + (r->curPos[1] - r->prevPos[1]) * alpha;
        r->renderPos[2] = r->curPos[2] + (r->curPos[2] - r->prevPos[2]) * alpha;

        /* Yaw: extrapolate along the short way around the circle (s16 wraps). */
        dYaw = (s16)(r->curYaw - r->prevYaw);
        r->renderYaw = (s16)(r->curYaw + (s16)((f32)dYaw * alpha));
    }
}

/* Watch the local kart's lap counter and reliably announce each lap completion
 * and the finish (lapCount reaches 3) to every peer over ch1. lapCount starts
 * at -1 and increments on each finish-line crossing (race_logic.c:543). */
static void net_race_events_tx(void) {
    Player* me = &gPlayers[0];
    s16 lap = me->lapCount;
    NetEvent ev;

    if (lap == sLastLap) {
        return;
    }
    sLastLap = lap;
    if (lap < 1) {
        return; /* -1/0 = still on the first lap; nothing completed yet */
    }

    ev.tag = NETEV_TAG;
    ev.node = 0; /* our own id is unknown here; peers key off pkt.src */
    ev.pad = 0;
    ev.lap = lap;
    ev.rank = me->currentRank;
    if (lap >= 3 && !sSentFinish) {
        ev.type = NETEV_FINISH;
        sSentFinish = true;
        osSyncPrintf("netpak: local FINISH (place %d)\n", me->currentRank);
    } else {
        ev.type = NETEV_LAP;
        osSyncPrintf("netpak: local lap %d (place %d)\n", lap, me->currentRank);
    }
    net_event_send_all(&ev);
}

/* Character sync: give each puppet its peer's driver. Kart body textures, size
 * and wheels re-DMA every frame off live characterId (kart_dma.c), so setting
 * the field is enough for those; only the body PALETTE needs an explicit reload
 * (both double-buffer indices), and only when the character actually changes. */
static void net_race_apply_characters(void) {
    s32 i;
    for (i = 1; i < NUM_PLAYERS; i++) {
        RemoteSlot* r = &sRemote[i];
        if (!r->valid) {
            continue;
        }
        if (r->characterId > BOWSER) {
            continue; /* guard against a corrupt id indexing character tables */
        }
        if (gPlayers[i].characterId != r->characterId) {
            gPlayers[i].characterId = r->characterId;
            load_kart_palette(&gPlayers[i], (s8)i, 0, 0);
            load_kart_palette(&gPlayers[i], (s8)i, 0, 1);
            osSyncPrintf("netpak: puppet slot %d -> character %d\n", i, r->characterId);
        }
    }
}

/* Item-hit replication (target-authority). When one of OUR item actors strikes
 * a remote player's puppet in our world, the engine's per-kart collision has
 * already set that puppet slot's trigger bits. Report the hit to the victim so
 * their own kart reacts, then consume the bits (the puppet's visible reaction
 * comes back to us through their position snapshots — no local actor spawning). */
static void net_race_scan_hits(void) {
    s32 i;
    for (i = 1; i < NUM_PLAYERS; i++) {
        s32 hit;
        if (!sRemote[i].valid || !sPeerUsed[i]) {
            continue;
        }
        hit = gPlayers[i].triggers & NET_HIT_MASK;
        if (hit) {
            NetEvent ev;
            ev.tag = NETEV_TAG;
            ev.type = NETEV_HIT;
            ev.node = 0;
            ev.pad = 0;
            ev.lap = 0;
            ev.rank = 0;
            ev.bits = (u32)hit;
            net_event_send_one(sPeerNode[i], &ev);
            gPlayers[i].triggers &= ~hit; /* consume so it fires exactly once */
            osSyncPrintf("netpak: my item hit puppet slot %d (bits %08x) -> node %d\n",
                         i, hit, sPeerNode[i]);
        }
    }
}

/* Demo auto-drive: force the local kart to hold accelerate once the race is
 * running. Asymmetric — only the instance whose peer has an odd node id drives,
 * so its partner sits still and watches the remote (networked) kart pull away.
 * Called from the game loop AFTER read_controllers() so it isn't overwritten. */
void net_race_autodrive(void) {
    net_race_menu_test(); /* temp: headless menu navigation to verify ONLINE entry */
#if NET_DEMO_AUTODRIVE
    {
    bool drive = false;
    s32 i;

    if (!netpak_present() || gGamestate != RACING) {
        return;
    }
    /* TEMP diag: player-0 state (type + speed*10) so we can see if slot 0 is a
     * drivable PLAYER_HUMAN and whether input actually moves it. */
    if ((sRaceFrames % 32) == 0) {
        netpak_debug_poke(0xA0000000u | (u32)((u16)gPlayers[0].type));
        netpak_debug_poke(0xB0000000u | (u32)((s32)(gPlayers[0].speed * 10.0f) & 0xFFFF));
    }
    /* Only drive once staging + the countdown are fully done, i.e. the kart is
     * actually under player control. Forcing input during PLAYER_STAGING fights
     * the game's auto-drive-to-grid and leaves the kart stuck mid-staging. */
    if (gPlayers[0].type & (PLAYER_STAGING | PLAYER_START_SEQUENCE)) {
        return;
    }
    (void)i;
    drive = true; /* TEST: drive to verify the race is drivable post-staging */
    if (!drive) {
        return;
    }
    /* Waypoint autodrive v5. v4's eager reverse-arc recovery physically
     * clipped karts THROUGH wall corners (position logs showed a bot exiting
     * the course and driving kilometers off-map in a straight line). v5 is
     * deliberately conservative: steer at a speed-scaled lookahead, ease the
     * throttle in corners, pull back toward the nearest waypoint when far off
     * the line, and only ever reverse in a single short straight burst after
     * a long genuine pin — never repeatedly, never during the launch. */
    {
        s32 me = 0;
        Player* p;
        u16 cnt;
        s32 steer = 0;
        s32 wantA = 1;
        s32 wantB = 0;
        static u32 sAdStuck;
        static u32 sAdRevLeft;
        static u32 sAdRevCooldown;
        static u16 sAdLastPt;
        static s32 sAdXtrack;
        static s32 sAdPace;
#if NET_LOCKSTEP
        me = net_lockstep_local_slot();
#endif
        p = &gPlayers[me];
        cnt = gPathCountByPathIndex[gPlayerPathIndex];
        if (cnt != 0) {
            u16 near = gNearestPathPointByPlayerId[me];
            TrackPathPoint* np_ = &gTrackPaths[gPlayerPathIndex][near % cnt];
            f32 ndx = np_->posX - p->pos[0];
            f32 ndz = np_->posZ - p->pos[2];
            f32 nd2 = ndx * ndx + ndz * ndz;
            u16 tgt;
            s16 diff;

            /* PACE-CAR STRATEGY: the CPU karts lap perfectly, so prefer to
             * chase the nearest CPU that's 8..60 path points ahead — its
             * position is always on a drivable line, so walls and hairpins are
             * dodged by construction. Waypoint pursuit is the fallback. */
            {
                s32 k;
                s32 bestd = 9999;
                s32 best = -1;
                for (k = 0; k < 8; k++) {
                    s32 d;
                    if ((gPlayers[k].type & PLAYER_CPU) != PLAYER_CPU) {
                        continue;
                    }
                    d = (s32) (((u32) gNearestPathPointByPlayerId[k] + (u32) cnt - (u32) near) % cnt);
                    if (d >= 8 && d <= 60 && d < bestd) {
                        bestd = d;
                        best = k;
                    }
                }
                sAdPace = best;
            }
            if (sAdPace >= 0) {
                tgt = atan2s(gPlayers[sAdPace].pos[0] - p->pos[0],
                             gPlayers[sAdPace].pos[2] - p->pos[2]);
                diff = (s16) (tgt - p->rotation[1]);
            } else if (nd2 > 150.0f * 150.0f) {
                /* far off the line (escaped/knocked out): aim straight back at
                 * the nearest waypoint until close again */
                tgt = atan2s(ndx, ndz);
                diff = (s16) (tgt - p->rotation[1]);
            } else {
                /* pure pursuit with adaptive lookahead: far when aligned, near
                 * when off-heading — a long lookahead at a hairpin aims ACROSS
                 * it through the inner wall (both bots wedged at Luigi's first
                 * hairpin, path index ~79, before this). */
                s32 ahead = 4 + (s32) (p->speed * 4.0f); /* cruise ~2.0 -> ~12 */
                TrackPathPoint* tp;
                if (ahead > 14) {
                    ahead = 14;
                }
                tp = &gTrackPaths[gPlayerPathIndex][((u32) near + (u32) ahead) % cnt];
                tgt = atan2s(tp->posX - p->pos[0], tp->posZ - p->pos[2]);
                diff = (s16) (tgt - p->rotation[1]);
                if (diff > DEGREES(30) || diff < -DEGREES(30)) {
                    tp = &gTrackPaths[gPlayerPathIndex][((u32) near + 3u) % cnt];
                    tgt = atan2s(tp->posX - p->pos[0], tp->posZ - p->pos[2]);
                    diff = (s16) (tgt - p->rotation[1]);
                }
            }

            /* cross-track correction (Stanley-lite): add steering proportional
             * to the signed lateral offset from the path, so the kart is pulled
             * back to the centerline BEFORE it wall-hugs. Sign from the cross
             * product of the local path direction and the offset vector. */
            {
                TrackPathPoint* np2 = &gTrackPaths[gPlayerPathIndex][((u32) near + 1u) % cnt];
                f32 dirx = np2->posX - np_->posX;
                f32 dirz = np2->posZ - np_->posZ;
                f32 offx = -ndx; /* kart - nearest point */
                f32 offz = -ndz;
                f32 cross = dirx * offz - dirz * offx;
                s32 pull = (s32) (cross * 0.15f);
                if (pull > 30) {
                    pull = 30;
                }
                if (pull < -30) {
                    pull = -30;
                }
                sAdXtrack = (sAdPace >= 0) ? 0 : pull;
            }

            /* throttle: full when aligned, eased in corners so the kart can
             * rotate. When pacing a CPU the target is drivable by construction
             * and close-follow makes diff swing, so commit harder to the gas. */
            if (sAdPace >= 0) {
                if (diff > DEGREES(80) || diff < -DEGREES(80)) {
                    wantA = (sRaceFrames & 3) != 0;
                }
            } else {
                if (diff > DEGREES(35) || diff < -DEGREES(35)) {
                    wantA = (sRaceFrames & 3) != 0;
                }
                if (diff > DEGREES(80) || diff < -DEGREES(80)) {
                    wantA = (sRaceFrames & 1);
                }
            }
            /* hairpin overspeed: dab the brake while badly off-heading at pace */
            if (sAdPace < 0 && (diff > DEGREES(55) || diff < -DEGREES(55)) && p->speed > 1.5f && (sRaceFrames & 7) < 2) {
                wantA = 0;
                wantB = 1;
            }

            steer = diff / 60 - sAdXtrack; /* pull sign VERIFIED by A/B: flipping it dropped lap coverage 9->6 */
            if (steer > 75) {
                steer = 75;
            }
            if (steer < -75) {
                steer = -75;
            }
            if (steer > 0 && steer < 14) {
                steer = (diff > DEGREES(1)) ? 14 : 0;
            }
            if (steer < 0 && steer > -14) {
                steer = (diff < -DEGREES(1)) ? -14 : 0;
            }

            /* last-resort unpin. NOTE: speed is NOT a usable pin signal — a
             * kart grinding a wall at full throttle still reads cruise speed.
             * The reliable signal is the path index not advancing. One short
             * STRAIGHT reverse (arcs clip through walls), long cooldown. */
            if (sAdRevCooldown != 0) {
                sAdRevCooldown--;
            }
            if (near != sAdLastPt || sRaceFrames < 300) {
                sAdLastPt = near;
                sAdStuck = 0;
            } else if (++sAdStuck > 150 && sAdRevLeft == 0 && sAdRevCooldown == 0) {
                sAdRevLeft = 45;
                sAdRevCooldown = 450;
                sAdStuck = 0;
            }
            if (sAdRevLeft != 0) {
                sAdRevLeft--;
                wantA = 0;
                wantB = 1;
                steer = 0; /* straight back — arcs are how v4 clipped through walls */
            }
        }

        if (wantA) {
            gControllers[0].button |= A_BUTTON;
            gControllers[0].buttonPressed |= A_BUTTON;
        }
        if (wantB) {
            gControllers[0].button |= B_BUTTON;
        }
        gControllers[0].rawStickX = (s8) steer;
        sAdOverlayStick = (s8) steer; /* input overlay (drawn in cam_pop) */
        sAdOverlayBtn = (u16) ((wantA ? A_BUTTON : 0) | (wantB ? B_BUTTON : 0));

        /* state-machine telemetry: [rev|offline|A|B]<<16 | speed*100 */
        if ((sRaceFrames & 7) == 0) {
            netpak_debug_poke(0x5F000000u |
                              (((sAdRevLeft ? 8u : 0u) |
                                (wantA ? 2u : 0u) | (wantB ? 1u : 0u)) << 16) |
                              ((u32) (p->speed * 100.0f) & 0xFFFF));
        }

        /* progress telemetry: lap + path index, decodable from the trace */
        if ((sRaceFrames & 63) == 0) {
            netpak_debug_poke(0x5E000000u | (((u32) (p->lapCount + 1) & 0xF) << 16) | (u16) gNearestPathPointByPlayerId[me]);
        }

        /* Per-client kart-position log (user request): every console records
         * ALL 8 karts' x/z each 32 frames — tags 0x80+kart (x, s16) and
         * 0x90+kart (z, s16) — plus the course waypoints ONCE at race start
         * (tags 0x9E/0x9F pairs, in path order). netpak/kartplot.py turns a
         * console's NP64_TRACE_IO log into an SVG of trails over the track. */
        {
            static u32 sPosDumped;
            s32 k;
            if (!sPosDumped && cnt != 0) {
                u32 w;
                sPosDumped = 1;
                for (w = 0; w < cnt; w++) {
                    TrackPathPoint* wp = &gTrackPaths[gPlayerPathIndex][w];
                    netpak_debug_poke(0x9E000000u | (u16) (s16) wp->posX);
                    netpak_debug_poke(0x9F000000u | (u16) (s16) wp->posZ);
                }
            }
            if ((sRaceFrames & 31) == 0) {
                for (k = 0; k < 8; k++) {
                    netpak_debug_poke(((0x80u + (u32) k) << 24) | (u16) (s16) gPlayers[k].pos[0]);
                    netpak_debug_poke(((0x90u + (u32) k) << 24) | (u16) (s16) gPlayers[k].pos[2]);
                }
            }
        }

        /* Scripted item use: when the local kart holds an item, press Z in a short
         * per-slot staggered window (so screenshots can catch each player's item
         * going off separately). Poke 0x65 [item<<8|slot] as the screenshot cue. */
        {
            static u32 adFrames;
            u32 phase = adFrames % 400u;
            adFrames++;
            if (p->currentItemCopy != ITEM_NONE && phase >= (u32) (me * 40) && phase < (u32) (me * 40 + 4)) {
                gControllers[0].button |= Z_TRIG;
                netpak_debug_poke(0x59000000u | (((u32) (u16) p->currentItemCopy & 0xFFu) << 8) | (u32) me);
            }
        }
    }
    }
#endif
}

/* --- Debug harness ------------------------------------------------------- */
/* Structured state dump over osSyncPrintf. With ares built with the ISViewer
 * mirror and run with ARES_ISV=1, these lines land in the ares log — a readable
 * timeline instead of blind register-poking. Grep for "NPDBG". */
#define NET_DEBUG 0
#if NET_DEBUG
extern s16 gCurrentCourseId;
extern s32 gModeSelection;
extern s32 gScreenModeSelection;
/* Structured state dump over the register-poke channel (NP64_TRACE_IO logs each
 * write to reg 0x0020). Tagged 32-bit words the decoder script turns into
 * readable lines — a reliable headless state timeline. Tags (high byte):
 *   0x51 gamestate: (gs<<16)|(mode<<8)|course
 *   0x52 in-race A: frame (24-bit)
 *   0x53 in-race B: (stg<<17)|(go<<16)|type16
 *   0x54 in-race C: (course<<16)|speed10
 *   0x5F STUCK assert */
static void net_race_debug_state(void) {
    static s32 lastGs = -1;
    static u16 lastType = 0xFFFF;
    s32 gs = (s32) gGamestate;
    u16 t = (u16) gPlayers[0].type;

    if (gs != lastGs) {
        netpak_debug_poke(0x51000000u | (((u32) gs & 0xFF) << 16) |
                          (((u32) gModeSelection & 0xFF) << 8) |
                          ((u32) gCurrentCourseId & 0xFF));
    }
    if (gs == RACING && ((t != lastType) || (sRaceFrames % 16) == 0)) {
        u32 stg = (t & PLAYER_STAGING) ? 1u : 0u;
        u32 go = (t & PLAYER_START_SEQUENCE) ? 0u : 1u; /* go=1 once countdown clears */
        s32 spd10 = (s32) (gPlayers[0].speed * 10.0f);
        netpak_debug_poke(0x52000000u | ((u32) sRaceFrames & 0x00FFFFFFu));
        netpak_debug_poke(0x53000000u | (stg << 17) | (go << 16) | (u32) t);
        netpak_debug_poke(0x54000000u | (((u32) gCurrentCourseId & 0xFF) << 16) |
                          ((u32) spd10 & 0xFFFFu));
    }
    if (gs == RACING && sRaceFrames == 240 && (t & (PLAYER_STAGING | PLAYER_START_SEQUENCE))) {
        netpak_debug_poke(0x5F000000u | (u32) t); /* STUCK: still staging at f=240 */
    }
    lastGs = gs;
    lastType = t;
}
#endif /* NET_DEBUG */

/* Dense in-race sampler, called every frame from race_logic_loop (which — unlike
 * net_race_frame up in thread5 — provably ticks on every rendered race frame).
 * Always defined so main.c can call it unconditionally; a no-op unless NET_DEBUG.
 * Its own counter, so it doesn't depend on net_race's sRaceFrames. */
void net_race_debug_tick(void) {
#if NET_DEBUG
    static u32 rf;
    static u16 lastType = 0xFFFF;
    u16 t = (u16) gPlayers[0].type;

    rf++;
    if ((t != lastType) || (rf % 8) == 0) {
        u32 stg = (t & PLAYER_STAGING) ? 1u : 0u;
        u32 go = (t & PLAYER_START_SEQUENCE) ? 0u : 1u;
        s32 spd10 = (s32) (gPlayers[0].speed * 10.0f);
        netpak_debug_poke(0x52000000u | (rf & 0x00FFFFFFu));
        netpak_debug_poke(0x53000000u | (stg << 17) | (go << 16) | (u32) t);
        netpak_debug_poke(0x54000000u | (((u32) gCurrentCourseId & 0xFF) << 16) |
                          ((u32) spd10 & 0xFFFFu));
    }
    lastType = t;
#endif
}

/* ============================ LOCKSTEP PROTOTYPE ============================
 * Input-transport layer. Every frame each console broadcasts its controller
 * state (tagged with player index + frame, with redundancy so a single lost
 * datagram is recovered from the next packet), and buffers all players' inputs
 * by (player, frame). Determinism is verified, so feeding the identical input
 * set into the sim on every console produces the identical world. This
 * increment builds + validates the transport (both consoles receive each
 * other's inputs, aligned by frame); the sim rewire — drive the native 2-4p VS
 * karts from this buffer, single-viewport render, input-delay gate — follows. */
#define NET_LOCKSTEP 1

/* Test-only: inject artificial inbound packet loss to exercise the stall gate on a
 * loss-free LAN. Drops a short burst (< LS_REDUN, so the input still arrives in a
 * later redundant copy and the stall RECOVERS) periodically, phase-offset per node
 * so the loss is asymmetric (one console stalls while the other doesn't — the real
 * jitter case). Product = 0. */
#define NET_LOCKSTEP_LOSS 0

/* Test-only: force every item box to grant a known item (even slots: lightning,
 * odd slots: star), so the scripted autodrive exercises item interactions —
 * lightning shrinking every OTHER kart, star-powered collisions tumbling the
 * victim — visibly on all consoles. Deterministic: itemOverride is shared sim
 * state and every console writes the identical values before the sim each frame.
 * Product = 0. */
#define NET_ITEM_TEST 0

#if NET_LOCKSTEP
extern s32 net_menu_node_id(void);
extern s32 net_menu_player_count(void);
extern bool net_menu_online_active(void);
extern u16 gRandomSeed16;

#define LS_TAG   0x4C /* 'L' */
#define LS_RING  64   /* frames of input history buffered */
#define LS_REDUN 6    /* frames of redundancy per packet (covers ch0 loss) */
#define LS_DELAY 2    /* input delay (frames) */

typedef struct {
    u16  button;
    s8   stickX;
    s8   stickY;
    bool have;
    u32  frame; /* the exact frame this slot's input is for — guards ring ALIASING:
                 * without it, once the ring wraps every slot reads have=true forever
                 * and a fast console barrels ahead applying inputs from frame f±64k.
                 * Invisible with identical scripted inputs (aliased value == value);
                 * instantly divergent with real differing inputs. */
} LsInput;
static LsInput sLsInput[NET_MAX_SLOTS][LS_RING]; /* [player][frame % LS_RING] */
static u16     sLsPrevBtn[NET_MAX_SLOTS]; /* last APPLIED buttons per player (edge derivation) */
static s32     sLsMyPlayer = -1;
static u32     sLsFrame;
static u16     sLsSimSeed;   /* Path B: private sim RNG state (render can't drift it) */
static bool    sLsRngActive; /* true while an online lockstep race is running */
static bool    sLsStall;     /* stall gate: true when a needed input is missing this frame */
static u32     sLsStallCount; /* diagnostic: total stalled render-frames this race */

/* ---- Race-entry CPU-block sync (start-state agreement v2) -----------------
 * Zeroing the block at race entry lobotomized the REAL CPU karts (slots np..7
 * when np < 8): course-load initializes tables inside the block, and the zero
 * ran after. Zeroing pre-load kept CPUs alive but menu frames between launch
 * and load re-accumulated per-console residue (diverged ~333). v2: the HOST
 * broadcasts ITS block at race entry over reliable ch1; joiners hold the gate
 * until applied. Init values arrive valid, residue becomes SHARED (harmless —
 * agreement is all determinism needs). */
#define LSBLK_TAG   0x42 /* 'B' */
#define LSBLK_CHUNK 900
typedef struct {
    u8  tag;
    u8  seq;
    u16 offset;
    u16 len;
    u16 total;
    u8  data[LSBLK_CHUNK];
} LsBlkMsg;
static bool sBlkApplied;

/* LIVE DESYNC DETECTOR: consoles broadcast (frame, sim-hash) every 16 frames
 * on ch0 (tag 0x48 'H'); each console checks peers' hashes against its own
 * ring. First mismatch latches sLsDesync — rendered as a flashing red square
 * (top-left) so a HUMAN race reports divergence the moment it happens instead
 * of through 'the other kart is driving through walls' symptom reports. */
#define LS_HASHRING 64
static u32  sLsHashFrame[LS_HASHRING];
static u32  sLsHashVal[LS_HASHRING];
static u32  sLsHashLastDf; /* dedupe: store/broadcast each df once (first eval) */
static u32  sLsDesyncFrame; /* first mismatching frame (0 = none) */
static bool sLsDesync;
typedef struct {
    u8  tag;   /* 0x48 */
    u8  pad;
    u16 pad2;
    u32 frame;
    u32 hash;
} LsHashMsg; /* 12 bytes */
#define LSHASH_TAG 0x48
static u32  sBlkGot;
static u32  sBlkSendPos;

/* ---- Player-drop handling -------------------------------------------------
 * A leaver would otherwise stall everyone forever (the gate needs ALL inputs).
 * Per-console timeouts would desync (different drop frames), so the drop is an
 * AGREED event: the lowest-id surviving node (the arbiter) broadcasts a DROP
 * message carrying the leaver's last known inputs and their last frame L.
 * Everyone fills the leaver's ring through L, skips them in the gate for frames
 * > L, and converts their kart to CPU when simulating frame > L — deterministic
 * on every console (MK64's CPU AI is proven deterministic across consoles). The
 * first DROP accepted for a player wins; repeats are ignored (the arbiter
 * rebroadcasts every tick while stalled, covering ch0 loss). Known v1 edge: if
 * the arbiter itself dies immediately after sending to only SOME peers, the
 * next arbiter may compute a different L -> split. Acceptable for now. */
#define LSDROP_TAG    0x44 /* 'D' */
#define NET_PAUSE_SYNC 1 /* freeze the lockstep timeline while paused (the fix
                          * for the pause->red-square desync); 0 = old behavior
                          * for A/B proof runs */
static u32 sLsPauseTicks; /* pause-menu input debounce (see pause gate) */
static bool sLsWasPaused;  /* set while in the pause gate; a 1->0 edge = WE resumed */
static u32 sLsResumeTx;    /* broadcast LSRESUME this many more ticks */
#define LSHOLD_TAG 0x49
#define LSRESUME_TAG 0x4A /* networked unpause: one player's resume releases all */ /* "I'm paused, don't drop me": resets the peer's stall clock */
#define LS_DROP_TICKS 240  /* stalled render-ticks on one frame before peer-table check */
#define LS_DROP_HARD  900  /* stalled ticks -> drop even if the relay still lists them */
typedef struct {
    u8  tag;       /* LSDROP_TAG */
    u8  player;    /* who is being dropped */
    u8  count;     /* inputs included (up to LS_REDUN, ending at lastFrame) */
    u8  pad;
    u16 lastFrame; /* leaver's last available input frame L */
    u16 pad2;
    struct {
        u16 button;
        s8  stickX;
        s8  stickY;
    } in[LS_REDUN];
} LsDropMsg;
static bool sLsDropped[NET_MAX_SLOTS];
static u32  sLsDropLast[NET_MAX_SLOTS]; /* L — last frame the leaver's input applies */
static bool sLsDropNever[NET_MAX_SLOTS]; /* dropped WITHOUT ever sending an input
    (left the lobby before the race): L=0 must not lock frame 0 — a ghost seat
    froze every console at the very first frame (title-card/black-screen hang) */
static u32  sLsStallTicks;              /* consecutive stalled ticks on the same frame */
static u32  sLsStallFrame;              /* the df we've been stalled on */

/* Local chase-cam: run MK64's REAL camera-follow for the local kart during render,
 * isolated from the sim by rendering against a COPY of the camera-path block +
 * camera1 + D_80152300 and restoring the sim's copies after. A persistent local
 * camera context carries the local camera's own state across frames.
 *
 * KNOWN LIMITATION (4p): the follow correctly targets the local kart at 2p, but at
 * 4p the retargeted cameras on nodes 2,3 mis-follow (they track slot 1) — MK64's
 * follow keys off internal per-slot camera state, not the Player* we pass, and the
 * naive fixes (camera->playerId, index) didn't move it. Determinism IS preserved at
 * 4p in this version; only the camera target is wrong. (A geometric re-anchor
 * variant targets correctly at any count but broke determinism at 4p — render
 * mutates sim-adjacent state we couldn't fully isolate — so it's not used.) */
#define LS_CAMBLK_MAX 0x1A00 /* >= the cpu_vehicles_camera_path.c block (~0x1548) */
static u8      sCamSimBlk[LS_CAMBLK_MAX]; /* sim's block, saved across the local follow */
static u8      sCamLocBlk[LS_CAMBLK_MAX]; /* local camera's persistent block context */
static Camera  sCamSim1, sCamLoc1;        /* sim camera1 / local camera1 */
static u16     sCamSimD300[4], sCamLocD300[4];
static bool    sCamLocInit;               /* local context seeded yet? (reset per race) */
static s32     sCamActive;                /* local slot in effect this frame (0 = none) */

typedef struct {
    u8  tag;       /* LS_TAG */
    u8  player;    /* sender player index (== node id) */
    u8  count;     /* frames included in in[] */
    u8  pad;
    u16 baseFrame; /* frame of in[0] */
    u16 pad2;
    struct {
        u16 button;
        s8  stickX;
        s8  stickY;
    } in[LS_REDUN];
} LsPacket;

static void net_lockstep_reset(void) {
    bzero(sLsInput, sizeof(sLsInput));
    bzero(sLsPrevBtn, sizeof(sLsPrevBtn));
    bzero(sLsDropped, sizeof(sLsDropped));
    bzero(sLsDropLast, sizeof(sLsDropLast));
    bzero(sLsDropNever, sizeof(sLsDropNever));
    sLsStallTicks = 0;
    sLsStallFrame = 0;
    sLsFrame = 0;
    sLsMyPlayer = net_menu_node_id();
    sLsSimSeed = 0x1234; /* shared, identical on every console -> synced sim RNG */

    /* NOTE: the CPU-AI block zeroing that used to live here moved to
     * net_lockstep_prerace_clear() — zeroing at race entry ran AFTER the game
     * had initialized the real CPU karts (slots np..7 when np < 8), leaving
     * them lobotomized: frozen at the start line, and star-contact with such a
     * zombie kart hung the sim on every console (deterministically). Clearing
     * BEFORE course load keeps the start-state agreement AND lets the game's
     * own init run on top. */

    sCamLocInit = false; /* fresh local-camera context each race */
    sLsStall = false;
    sLsStallCount = 0;
    sBlkApplied = false;
    sBlkGot = 0;
    sBlkSendPos = 0;
    sLsDesync = false;
    sLsDesyncFrame = 0;
    sLsHashLastDf = 0xFFFFFFFFu;
    {
        /* seed the ring with an impossible frame, NOT zero: a zeroed slot
         * claims "frame 0, hash 0", so a peer's real frame-0 hash arriving
         * before we computed frame 0 ourselves (host finishes block-sync a
         * beat early; real network latency widens the window) latched a FALSE
         * desync at race entry — red square on a perfectly synced race. */
        s32 hi;
        for (hi = 0; hi < LS_HASHRING; hi++) {
            sLsHashFrame[hi] = 0xFFFFFFFFu;
        }
        bzero(sLsHashVal, sizeof(sLsHashVal));
    }
}

/* Apply a DROP — the ONE path used by both receivers and the arbiter, so there is
 * no arbiter-vs-receiver asymmetry by construction. Fills the leaver's ring with
 * their final inputs using the sender's FIXED index mapping
 * (in[i] = frame lastFrame-(LS_REDUN-1-i); count is informational only — the old
 * compacted-prefix decode disagreed with the sender whenever count < LS_REDUN and
 * wrote zeroed inputs onto real frames), then marks the player dropped. Idempotent:
 * repeats and echoes are ignored. */
static void net_lockstep_apply_drop(const LsDropMsg* dm) {
    s32 pl = dm->player;
    s32 i;
    if (pl < 0 || pl >= NET_MAX_SLOTS || sLsDropped[pl]) {
        return;
    }
    for (i = 0; i < LS_REDUN; i++) {
        u32 back = (u32) (LS_REDUN - 1 - i);
        u32 fr;
        LsInput* d;
        if ((u32) dm->lastFrame < back) {
            continue; /* would underflow lastFrame: frame doesn't exist (early race) */
        }
        fr = (u32) dm->lastFrame - back;
        d = &sLsInput[pl][fr % LS_RING];
        d->button = dm->in[i].button;
        d->stickX = dm->in[i].stickX;
        d->stickY = dm->in[i].stickY;
        d->have = true;
        d->frame = fr;
    }
    sLsDropped[pl] = true;
    sLsDropLast[pl] = dm->lastFrame;
    if (dm->count == 0) {
        sLsDropNever[pl] = true; /* no inputs at all: CPU from frame 0 */
    }
    netpak_debug_poke(0x58000000u | ((u32) pl << 16) | ((u32) dm->lastFrame & 0xFFFFu));
}
#endif /* NET_LOCKSTEP (module) — net_lockstep_tick below is always defined */

/* Full per-frame lockstep step — call AFTER read_controllers during an online
 * race. Converts the extra player slots (1..np-1) to human so they read a
 * controller instead of AI, captures the local input, exchanges with peers, and
 * drives every player kart from the shared buffer at (frame - LS_DELAY). Emits a
 * sim-state hash (tag 0x76/0x77) so two consoles can be checked for an identical
 * world. No stall gate yet (LAN + redundancy assumed); that comes next. */
void net_lockstep_tick(void) {
#if NET_LOCKSTEP
    static netpak_pkt_t pkt; /* 1 KB — keep off the stack */
    static s32 prevRacing;
    LsPacket p;
    s32 me, np, i, base;
    u32 f, df;
    s32 racing = (gGamestate == RACING);

    if (!netpak_present() || !net_menu_online_active() || !racing) {
        prevRacing = racing;
        sLsRngActive = false;
        return;
    }
    if (!prevRacing) {
        net_lockstep_reset(); /* fresh input timeline at race start */
    }
    prevRacing = racing;

    me = sLsMyPlayer;
    if (me < 0) {
        me = net_menu_node_id();
        sLsMyPlayer = me;
    }
    if (me < 0 || me >= NET_MAX_SLOTS) {
        return;
    }
#if NET_MENU_TEST && NET_DEMO_AUTODRIVE
#if NET_ITEM_PROBE /* OFF by default: each console grants to a DIFFERENT slot
    -> deliberately divergent -> latches the (now working) detector at ~df 860.
    This probe is what desynced every test race since it was added. */
    /* ITEM-CHAIN probe: every ~300 ticks, simulate an item-box hit for MY
     * slot (the exact call the box makes) and trace the chain:
     * 0xA8 = grant attempt (slot | window obj state), 0xA9 = window's
     * currentItem later, 0xAA = player's currentItemCopy. Identical calls on
     * every console (each probes its own slot at the same tick), so the sim
     * stays deterministic ONLY IF the grant path is deterministic — which is
     * itself under test (0x7D fires if not). */
    {
        extern void func_8007ABFC(s32, s32);
        extern s32 gItemWindowObjectByPlayerId[];
        static u32 sIbTick;
        if (gGamestate == RACING && me >= 0 && me < 4) {
            sIbTick++;
            if (sIbTick > 600 && (sIbTick % 300) == 0) {
                s32 w = gItemWindowObjectByPlayerId[me];
                netpak_debug_poke(0xA8000000u | ((u32) me << 20) | ((u32) gObjectList[w].state & 0xFFFFu));
                func_8007ABFC(me, 0);
            }
            if (sIbTick > 600 && (sIbTick % 300) == 60) {
                s32 w = gItemWindowObjectByPlayerId[me];
                netpak_debug_poke(0xA9000000u | ((u32) me << 20) |
                                  ((u32) ((ItemWindowObjects*) &gObjectList[w])->currentItem & 0xFFu));
                netpak_debug_poke(0xAA000000u | ((u32) me << 20) | ((u32) gPlayers[me].currentItemCopy & 0xFFu));
            }
        }
    }
#endif /* NET_ITEM_PROBE */

    /* DETECTOR SELF-TEST (NET_DETECTOR_SELFTEST=1 only): node 1 deliberately
     * poisons its own sim at tick 2500 (speed nudge). The detector MUST latch
     * (0x7D) on both consoles within a second — this caught the gate
     * miscompile that silently blinded the detector for several versions.
     * Leave OFF for determinism campaigns (it desyncs every race by design). */
#if NET_DETECTOR_SELFTEST
    {
        static u32 sDsTick;
        if (gGamestate == RACING) {
            sDsTick++;
            if (me == 1 && sDsTick == 2500) {
                gPlayers[1].speed += 5.0f;
                netpak_debug_poke(0xCD000001u);
            }
        }
    }
#endif

    /* course-id witness: which course did THIS console load? (0xCC poke) */
    {
        extern s16 gCurrentCourseId;
        static s32 sCcLast = -1;
        if (gGamestate == RACING && gCurrentCourseId != sCcLast) {
            extern s32 gCCSelection;
            sCcLast = gCurrentCourseId;
            netpak_debug_poke(0xCC000000u | (((u32) gCCSelection & 0xFu) << 16) |
                              ((u32) gCurrentCourseId & 0xFFu));
        }
    }

    /* POST-RACE ADVANCE: once the local player has finished, pulse A so the
     * ceremony/results screens advance (lets the harness photograph the
     * standings board, where online names replace character names). */
    {
        static u32 sPrTick;
        if (gGamestate == RACING && playerHUD[0].raceCompleteBool != 0) {
            sPrTick++;
            if (sPrTick > 240 && (sPrTick % 60) == 0) {
                gControllers[0].buttonPressed |= A_BUTTON;
            }
        } else {
            sPrTick = 0;
        }
    }

    /* PAUSE regression test, REAL path: at tick 900 the host's captured ring
     * input carries a one-frame START press, so the vanilla pause loop
     * (func_8028F970, inside the gated sim) pauses EVERY console at the same
     * logical frame. While paused, each console pulses START on its local pad
     * so the real pause-menu resume path runs — like both users unpausing.
     * Pokes: 0xA5 = injection, 0xA6 = paused (|df), 0xA7 = unpaused. Detector
     * (0x7D) must stay green throughout. */
    {
        static u32 sPjTick, sPjPaused;
        sPjTick++;
        if (me == 1 && sPjTick == 900) { /* the JOINER pauses: exercises the
                 pause-owner clamp (owner index 2 reads a frozen ring slot) */
            gControllers[0].button |= START_BUTTON; /* captured + broadcast below */
            netpak_debug_poke(0xA5000900u);
        }
        if (gIsGamePaused != 0) {
            if (sPjPaused++ == 0) {
                netpak_debug_poke(0xA6000000u | (sLsFrame & 0xFFFFFFu));
            }
            if (me == 1 && sPjPaused > 90 && (sPjPaused % 20) == 0) {
                gControllers[0].buttonPressed |= START_BUTTON; /* menu resume —
                    PAUSER ONLY: the peer must resume via LSRESUME broadcast */
            }
        } else if (sPjPaused != 0) {
            netpak_debug_poke(0xA7000000u | (sLsFrame & 0xFFFFFFu));
            sPjPaused = 0;
        }
    }
#endif

    np = net_menu_player_count();
    if (np > NET_MAX_SLOTS) {
        np = NET_MAX_SLOTS;
    }

    /* Make the non-local player slots human-controlled (they read a controller
     * instead of running AI). Identical on every console -> identical sim. Keep
     * the staging/countdown bits so the race intro still plays out. DROPPED
     * players convert the other way: their kart becomes a CPU bot exactly when
     * the sim passes their last input frame L — same logical frame on every
     * console, and the CPU AI is deterministic, so the sim stays identical. */
    for (i = 0; i < np; i++) {
        extern s16 D_801633F8[12];
        if (!sLsDropped[i]) {
            gPlayers[i].type = (gPlayers[i].type & ~(u32) PLAYER_CPU) | PLAYER_HUMAN;
        }
        /* (v20: ALL 8 karts, not just humans — CPUs kept the visible-to-
         * camera1 downgrade, and camera1 is the HOST's view, so on a
         * JOINER's screen distant CPU karts ran the rail-mover and visibly
         * drove through the ground. Offline no player can ever SEE an
         * off-screen kart, so the cheat was invisible by construction;
         * a joiner's independent view breaks that assumption.) */
        /* FULL PHYSICS ALWAYS for every online player's kart. The per-frame
         * dispatcher (player_controller.c ~540) runs real kart physics —
         * including TERRAIN COLLISION — only for karts visible to camera1,
         * and downgrades the rest to control_cpu_movement, the rail-follower
         * that never touches the collision mesh. camera1 is the sim camera
         * (player 0's), so a joiner's kart got real physics only while near
         * the host's kart: everywhere else it drove through walls and floors
         * (identically on every console — detector green). Proven by per-kart
         * collision counters: kart 0 = 32 tests/16 frames, karts 1-7 = 0.
         * This flag is the engine's own override (split-screen humans are
         * simply always visible on their own screen); deterministic because
         * it's set from the same roster on every console. */
        D_801633F8[i] = 1;
    }
    {
        extern s16 D_801633F8[12];
        for (i = np; i < 8; i++) {
            D_801633F8[i] = 1; /* CPU karts too — see note above */
        }
        /* dropped slots: type is set in the READY branch as a pure function of the
         * logical frame df (CPU iff df > L) — deciding it here, at tick time, raced
         * against DROP-message arrival: a receiver whose gate unblocked in the same
         * tick the DROP arrived simulated L+1 with the kart still HUMAN while the
         * arbiter simulated it as CPU (the exact L+1 arbiter-only divergence). */
    }

#if NET_ITEM_TEST
    /* Force every box to grant a known item (shared sim state, identical values
     * written on every console pre-sim -> deterministic). ACTOR-spawning items
     * this pass: bananas (dropped on the road) + green shells (fired) — verifies
     * item actors exist identically in every sim; lightning/star are instant
     * effects and never spawn an actor. */
    for (i = 0; i < np; i++) {
        playerHUD[i].itemOverride = (i & 1) ? ITEM_GREEN_SHELL : ITEM_BANANA;
    }
#endif

    f = sLsFrame;

    /* Race-entry block sync: host streams its CPU-AI block (one ch1 chunk per
     * peer per tick); joiners hold the gate until fully applied. */
    if (!sBlkApplied) {
        u32 blkTotal = (u32) (LS_CPU_STATE_END - unk_cpu_vehicles_camera_path_pad);
        if (me == 0) {
            if (sBlkSendPos < blkTotal) {
                LsBlkMsg bm;
                u32 len = blkTotal - sBlkSendPos;
                if (len > LSBLK_CHUNK) {
                    len = LSBLK_CHUNK;
                }
                bm.tag = LSBLK_TAG;
                bm.seq = (u8) (sBlkSendPos / LSBLK_CHUNK);
                bm.offset = (u16) sBlkSendPos;
                bm.len = (u16) len;
                bm.total = (u16) blkTotal;
                memcpy(bm.data, unk_cpu_vehicles_camera_path_pad + sBlkSendPos, len);
                for (i = 1; i < np; i++) {
                    netpak_send((u8) i, 1, &bm, (u16) (8 + len));
                }
                sBlkSendPos += len;
            }
            if (sBlkSendPos >= blkTotal) {
                sBlkApplied = true; /* the host's own block IS the reference */
            }
        }
    }

    /* Path B — isolated sim RNG. Load the sim's private RNG state into the shared
     * LFSR before the sim runs this frame; net_lockstep_rng_save() (called from
     * race_logic_loop after the sim, before render) snapshots it back. Render and
     * menu code still draw from gRandomSeed16 but can no longer perturb the sim
     * stream, so both consoles consume RNG identically. Seeded to a shared
     * constant at race start (net_lockstep_reset); a host-broadcast base can add
     * per-race variety later. */
    gRandomSeed16 = sLsSimSeed;
    sLsRngActive = true;

    /* capture local input (port 0 = this console's player index `me`). Capture ONCE
     * per frame: if we're continuing a stall (sLsStall still set from last tick, f
     * hasn't advanced) the input for frame f is already committed + broadcast to the
     * peer, so re-reading the controller here would rewrite an input the peer may be
     * about to consume. Only capture on a fresh frame. */
    if (!sLsStall && gIsGamePaused == 0) {
        LsInput* s = &sLsInput[me][f % LS_RING];
        s->button = gControllers[0].button;
        s->stickX = (s8) gControllers[0].rawStickX;
        s->stickY = (s8) gControllers[0].rawStickY;
        s->have = true;
        s->frame = f;
    }

    /* broadcast my last LS_REDUN frames (redundancy covers a dropped datagram) */
    base = (s32) f - (LS_REDUN - 1);
    if (base < 0) {
        base = 0;
    }
    p.tag = LS_TAG;
    p.player = (u8) me;
    p.count = (u8) (f - (u32) base + 1);
    p.pad = 0;
    p.baseFrame = (u16) base;
    p.pad2 = 0;
    for (i = 0; i < (s32) p.count; i++) {
        LsInput* s = &sLsInput[me][(base + i) % LS_RING];
        p.in[i].button = s->button;
        p.in[i].stickX = s->stickX;
        p.in[i].stickY = s->stickY;
    }
    netpak_send(NETPAK_BROADCAST, 0, &p, sizeof(p));

    /* ingest remote inputs (stored at the SENDER's frame index) */
#if NET_LOCKSTEP_LOSS
    {
        /* drop ALL inbound this tick during a 3-of-41-tick burst, phase-offset by
         * node id so the two consoles lose different windows (asymmetric jitter). */
        static u32 sLossTick;
        sLossTick++;
        if (((sLossTick + (u32) me * 17u) % 41u) < 3u) {
            while (netpak_recv(&pkt) == 0) {
                /* discard */
            }
        }
    }
#endif
    while (netpak_recv(&pkt) == 0) {
        if (pkt.ch == 0 && pkt.len >= (u16) sizeof(LsHashMsg) && pkt.data[0] == LSHASH_TAG) {
            /* live desync check: compare the peer's (frame, hash) to ours */
            const LsHashMsg* hm = (const LsHashMsg*) pkt.data;
#if NET_MENU_TEST
            { /* pipeline bisect: E9=arrived, EA=frame-match-value-same,
                 EB=frame-mismatch (ring slot holds another frame) */
                static u32 rxDbg;
                if ((rxDbg++ & 31) == 0) {
                    netpak_debug_poke(0xE9000000u | (hm->frame & 0xFFFFFFu));
                    if (sLsHashFrame[hm->frame % LS_HASHRING] == hm->frame) {
                        if (sLsHashVal[hm->frame % LS_HASHRING] == hm->hash) {
                            netpak_debug_poke(0xEA000000u | (hm->frame & 0xFFFFFFu));
                        }
                    } else {
                        netpak_debug_poke(0xEB000000u |
                                          (sLsHashFrame[hm->frame % LS_HASHRING] & 0xFFFFFFu));
                    }
                }
            }
#endif
            if (!sLsDesync && sLsHashFrame[hm->frame % LS_HASHRING] == hm->frame &&
                sLsHashVal[hm->frame % LS_HASHRING] != hm->hash) {
                sLsDesync = true;
                sLsDesyncFrame = hm->frame;
                netpak_debug_poke(0x7D000000u | (hm->frame & 0xFFFFFFu));
            }
            continue;
        }
        if (pkt.ch == 0 && pkt.data[0] == LSRESUME_TAG) {
            if (gIsGamePaused != 0) {
                gIsGamePaused = 0; /* networked resume */
            }
            continue;
        }
        if (pkt.ch == 0 && pkt.data[0] == LSHOLD_TAG) {
            /* a peer is sitting in the pause menu: keep waiting for them instead
             * of escalating the stall into a DROP (which would CPU-convert a
             * player who is merely paused) */
            sLsStallTicks = 0;
            continue;
        }
        if (pkt.ch == 0 && pkt.len >= 8 && pkt.data[0] == LS_TAG) {
            LsPacket* rp = (LsPacket*) pkt.data;
            s32 pl = rp->player;
            s32 c = rp->count;
            if (pl >= 0 && pl < NET_MAX_SLOTS && pl != me) {
                if (c > LS_REDUN) {
                    c = LS_REDUN;
                }
                for (i = 0; i < c; i++) {
                    u32 fr = (u32) rp->baseFrame + (u32) i;
                    LsInput* d = &sLsInput[pl][fr % LS_RING];
                    d->button = rp->in[i].button;
                    d->stickX = rp->in[i].stickX;
                    d->stickY = rp->in[i].stickY;
                    d->have = true;
                    d->frame = fr;
                }
            }
        } else if (pkt.ch == 1 && pkt.len >= 8 && pkt.data[0] == LSBLK_TAG) {
            LsBlkMsg* bm = (LsBlkMsg*) pkt.data;
            u32 blkTotal = (u32) (LS_CPU_STATE_END - unk_cpu_vehicles_camera_path_pad);
            if (!sBlkApplied && bm->total == blkTotal && (u32) bm->offset + bm->len <= blkTotal) {
                memcpy(unk_cpu_vehicles_camera_path_pad + bm->offset, bm->data, bm->len);
                sBlkGot += bm->len;
                if (sBlkGot >= blkTotal) {
                    sBlkApplied = true;
                }
            }
        } else if (pkt.ch == 0 && pkt.len >= 8 && pkt.data[0] == LSDROP_TAG) {
            /* arbiter says player X is gone: adopt X's last inputs + last frame L.
             * First DROP accepted wins; repeats for an already-dropped player are
             * ignored so the arbiter can rebroadcast safely. */
            net_lockstep_apply_drop((const LsDropMsg*) pkt.data);
        }
    }

    /* DROP echo: every console that applied a drop rebroadcasts it periodically
     * while its ring still holds the leaver's final inputs, so a console that
     * missed the arbiter's original (unreliable) DROP still converges instead of
     * stalling forever as a non-arbiter. Receivers dedupe via sLsDropped. */
    {
        static u32 echoTick;
        echoTick++;
        if ((echoTick % 32) == 0) {
            for (i = 0; i < np; i++) {
                u32 L2 = sLsDropLast[i];
                if (sLsDropped[i] && f >= LS_DELAY && (f - LS_DELAY) - L2 < 48u) {
                    LsDropMsg dm2;
                    s32 k, n3 = 0;
                    dm2.tag = LSDROP_TAG;
                    dm2.player = (u8) i;
                    dm2.pad = 0;
                    dm2.pad2 = 0;
                    dm2.lastFrame = (u16) L2;
                    for (k = LS_REDUN - 1; k >= 0; k--) {
                        u32 fr = L2 - (u32) (LS_REDUN - 1 - k);
                        LsInput* s = &sLsInput[i][fr % LS_RING];
                        if (fr <= L2 && s->have && s->frame == fr) {
                            dm2.in[k].button = s->button;
                            dm2.in[k].stickX = s->stickX;
                            dm2.in[k].stickY = s->stickY;
                            n3++;
                        } else {
                            dm2.in[k].button = 0;
                            dm2.in[k].stickX = 0;
                            dm2.in[k].stickY = 0;
                        }
                    }
                    dm2.count = (u8) n3;
                    netpak_send(NETPAK_BROADCAST, 0, &dm2, sizeof(dm2));
                }
            }
        }
    }

    /* NETWORKED PAUSE. While the local player sits in the pause menu the sim is
     * halted (race_logic_loop's gIsGamePaused gate), so the lockstep timeline
     * must halt with it: no new frame is captured (gate above) and the frame
     * counter must not advance — otherwise inputs are consumed for frames this
     * console never simulated, splitting the sims the moment anyone pauses (the
     * on-device red-square desync). Every other console blocks on its stall
     * gate within LS_DELAY frames, so the whole race freezes together, exactly
     * like split-screen pause. A periodic HOLD keeps the drop protocol from
     * CPU-converting us; ingest above still ran, so nothing backs up. QUIT from
     * the pause menu leaves the race and the normal drop protocol takes over. */
#if NET_PAUSE_SYNC
    if (gIsGamePaused != 0) {
        static u32 holdTick;
        /* The vanilla pause menu reads the PAUSER's controller
         * (gIsGamePaused-1). Online, that can be a ring-fed slot frozen by
         * the pause itself -> menu dead on EVERY console. Clamp the owner to
         * slot 0 so each console's menu runs on its own local pad. */
        if (gIsGamePaused != 1) {
            gIsGamePaused = 1;
        }
        /* debounce: the press that opened the pause can leak a stale edge
         * into the menu on the very next frames ("pauses then instantly
         * unpauses"). Swallow local menu input for the first quarter second. */
        if (sLsPauseTicks < 15) {
            sLsPauseTicks++;
            gControllers[0].buttonPressed = 0;
        }
        sLsWasPaused = true;
        if ((holdTick++ & 31) == 0) {
            u32 hold = ((u32) LSHOLD_TAG << 24) | (u32) me;
            netpak_send(NETPAK_BROADCAST, 0, &hold, sizeof(hold));
        }
        sLsRngActive = false; /* pause-menu draws may roll gRandomSeed16; don't
                               * let rng_save snapshot that into the sim seed */
        return;
    }
    sLsPauseTicks = 0; /* not paused: re-arm the pause-menu debounce */
    if (sLsWasPaused) {
        /* WE just resumed (local menu): release everyone else too — leaving
         * the peers paused stranded the resumed console at the stall gate,
         * which looked exactly like a hang ("unpause only unpauses locally") */
        sLsWasPaused = false;
        sLsResumeTx = 3;
    }
    if (sLsResumeTx != 0) {
        u32 rm = ((u32) LSRESUME_TAG << 24) | (u32) me;
        sLsResumeTx--;
        netpak_send(NETPAK_BROADCAST, 0, &rm, sizeof(rm));
    }
#endif

    /* STALL GATE. Simulate logical frame df only when EVERY player's input for it
     * has arrived; otherwise freeze (race_logic_loop skips the sim while
     * net_lockstep_stalled()) rather than applying a mismatched/stale input, which
     * is what desyncs. The frame doesn't advance, and we keep broadcasting above so
     * the late input is delivered; when it arrives we resume. Both consoles only
     * ever simulate a given df with the identical complete input set, so the
     * logical-frame sequence is identical regardless of how long either stalled. */
    df = (f >= LS_DELAY) ? (f - LS_DELAY) : 0;
    {
        bool ready = true;
        s32 missing = -1;
        bool missMask[NET_MAX_SLOTS];
        if (!sBlkApplied) {
            ready = false; /* hold the sim until the host's block is adopted */
        }
        for (i = 0; i < np; i++) {
            LsInput* s = &sLsInput[i][df % LS_RING];
            missMask[i] = false;
            if (sLsDropped[i] && (sLsDropNever[i] || df > sLsDropLast[i])) {
                continue; /* dropped player: no input needed past their last frame L */
            }
            /* the slot must hold the input for EXACTLY df — have alone aliases once
             * the ring wraps (input from df±64k would silently pass) */
            if (!s->have || s->frame != df) {
                ready = false;
                missMask[i] = true;
                if (missing < 0) {
                    missing = i;
                }
            }
        }

        if (!ready) {
            sLsStall = true;
            sLsStallCount++;
            netpak_debug_poke(0x68000000u | (sLsStallCount & 0xFFFFFFu)); /* stall count */

            /* ---- leaver detection + arbitration (runs only while stalled) ----
             * Track how long we've been stuck on this same frame. Past the soft
             * threshold, ask the relay's peer table whether the missing player is
             * still in the room; past the hard threshold, assume gone regardless.
             * Only the ARBITER — the lowest node id still present — broadcasts the
             * DROP (every tick while stalled; receivers dedupe), so every console
             * applies the SAME last-frame L and inputs. */
            if (df == sLsStallFrame) {
                sLsStallTicks++;
            } else {
                sLsStallFrame = df;
                sLsStallTicks = 1;
            }

            /* PEER INPUT RELAY: while stalled on X, rebroadcast X's last-known
             * inputs in X's name (ingested via the normal LS_TAG path). Two jobs:
             * (1) recovers losses beyond X's own redundancy window; (2) before a
             * DROP, converges every console's X-horizon to the MAXIMUM anyone
             * holds — otherwise the arbiter can pick an L below what faster
             * consoles already SIMULATED with real inputs, splitting the sim at
             * the boundary (observed: arbiter one kart-frame ahead from L+1). */
            if (sLsStallTicks >= 30 && (sLsStallTicks % 8) == 0) {
                s32 mj;
                for (mj = 0; mj < np; mj++) {
                    LsPacket rp2;
                    s32 n2 = 0;
                    u32 H, base2;
                    if (!missMask[mj] || sLsDropped[mj]) {
                        continue;
                    }
                    H = df; /* find my highest consecutive frame of mj (H-1 = horizon) */
                    while (H > 0) {
                        LsInput* s = &sLsInput[mj][(H - 1) % LS_RING];
                        if (s->have && s->frame == H - 1) {
                            break;
                        }
                        H--;
                    }
                    if (H == 0) {
                        continue;
                    }
                    base2 = (H >= LS_REDUN) ? (H - LS_REDUN) : 0;
                    rp2.tag = LS_TAG;
                    rp2.player = (u8) mj; /* relayed in the missing player's name */
                    rp2.pad = 0;
                    rp2.baseFrame = (u16) base2;
                    rp2.pad2 = 0;
                    for (i = 0; (u32) i < H - base2 && i < LS_REDUN; i++) {
                        LsInput* s = &sLsInput[mj][(base2 + (u32) i) % LS_RING];
                        if (s->have && s->frame == base2 + (u32) i) {
                            rp2.in[i].button = s->button;
                            rp2.in[i].stickX = s->stickX;
                            rp2.in[i].stickY = s->stickY;
                            n2 = i + 1;
                        } else {
                            break; /* gap: send only the consecutive prefix */
                        }
                    }
                    if (n2 > 0) {
                        rp2.count = (u8) n2;
                        netpak_send(NETPAK_BROADCAST, 0, &rp2, sizeof(rp2));
                        netpak_debug_poke(0x57000000u | ((u32) mj << 20) | (H & 0xFFFFFu)); /* relay TX */
                    }
                }
            }

            if (missing >= 0 && !sLsDropped[missing] && sLsStallTicks >= LS_DROP_TICKS) {
                netpak_peer_t peers[8];
                s32 npeers = netpak_peers(peers, 8);
                bool gone = (sLsStallTicks >= LS_DROP_HARD);
                bool amArbiter = true;
                if (npeers < 0 && sLsStallTicks < LS_DROP_HARD) {
                    amArbiter = false; /* peer table unavailable: don't self-elect
                                        * (multiple blind arbiters would each pick
                                        * their own L); hard timeout overrides for
                                        * liveness */
                }
                if (npeers >= 0) {
                    bool present = false;
                    for (i = 0; i < npeers; i++) {
                        if ((s32) peers[i].node_id == missing) {
                            present = true;
                        }
                        /* a lower-id node that is still present outranks me */
                        if ((s32) peers[i].node_id < me && (s32) peers[i].node_id != missing) {
                            amArbiter = false;
                        }
                    }
                    if (!present) {
                        gone = true;
                    }
                }
                if (gone && amArbiter) {
                    /* my highest consecutive frame of the leaver's inputs = L */
                    LsDropMsg dm;
                    u32 L = df; /* stalled at df means we have everything below df */
                    while (L > 0) {
                        LsInput* s = &sLsInput[missing][(L - 1) % LS_RING];
                        if (s->have && s->frame == (L - 1)) {
                            break; /* L-1 present -> L-1 is the last frame */
                        }
                        L--;
                    }
                    if (L == 0) {
                        L = 1; /* degenerate: no inputs at all -> drop from the start */
                    }
                    L -= 1; /* L = last PRESENT frame */
                    dm.tag = LSDROP_TAG;
                    dm.player = (u8) missing;
                    dm.pad = 0;
                    dm.pad2 = 0;
                    dm.lastFrame = (u16) L;
                    dm.count = 0;
                    for (i = LS_REDUN - 1; i >= 0; i--) {
                        u32 fr = L - (u32) (LS_REDUN - 1 - i);
                        LsInput* s = &sLsInput[missing][fr % LS_RING];
                        if (fr <= L && s->have && s->frame == fr) {
                            dm.in[i].button = s->button;
                            dm.in[i].stickX = s->stickX;
                            dm.in[i].stickY = s->stickY;
                            dm.count++;
                        } else {
                            dm.in[i].button = 0;
                            dm.in[i].stickX = 0;
                            dm.in[i].stickY = 0;
                        }
                    }
                            netpak_send(NETPAK_BROADCAST, 0, &dm, sizeof(dm));
                    net_lockstep_apply_drop(&dm); /* genuinely the same path as receivers */
                }
            }
        } else {
            sLsStallTicks = 0;
            sLsStall = false;

            /* Dropped karts: type is a pure function of the logical frame — CPU
             * from the first frame past their last input L, HUMAN through L. Same
             * value on every console at the same df, regardless of when the DROP
             * message arrived (the one-tick-skew fix). */
            for (i = 0; i < np; i++) {
                if (sLsDropped[i]) {
                    if (sLsDropNever[i] || df > sLsDropLast[i]) {
                        gPlayers[i].type = (gPlayers[i].type & ~(u32) PLAYER_HUMAN) | PLAYER_CPU;
                    } else {
                        gPlayers[i].type = (gPlayers[i].type & ~(u32) PLAYER_CPU) | PLAYER_HUMAN;
                    }
                }
            }

            /* Particle-pool neutralization: the four per-kart particle pools
             * (Player+0x258..0xD98, drift dust/sparks) are updated by the RENDER
             * path at render rate — confirmed diverging across consoles at ~303
             * (10x stride-0x48 diffs) ahead of the 310 kart split. Too big to
             * snapshot (23KB would starve the heap), so zero them pre-sim every
             * ready frame: deterministic on all consoles; render still draws
             * fresh single-frame particles. */
            for (i = 0; i < NUM_PLAYERS; i++) {
                bzero(gPlayers[i].particlePool0, 0xB40);
            }

            /* drive every player kart from the delayed frame's inputs. Button edges
             * derive from the last APPLIED buttons (sLsPrevBtn), not a ring lookup
             * of df-1 — that slot may already be overwritten by frame df-1+64. The
             * applied sequence is the same on every console, so edges match. */
            for (i = 0; i < np; i++) {
                static const LsInput kNeutral = { 0, 0, 0, false, 0 };
                const LsInput* s = &sLsInput[i][df % LS_RING];
                u16 cur;
                u16 prev = sLsPrevBtn[i];
                if (sLsDropped[i] && (sLsDropNever[i] || df > sLsDropLast[i])) {
                    s = &kNeutral; /* dropped: neutral input (kart is CPU anyway) — the
                                    * ring slot holds stale per-console data */
                }
                cur = s->button;
                gControllers[i].button = cur;
                gControllers[i].buttonPressed = (u16) (cur & ~prev);
                gControllers[i].buttonDepressed = (u16) (~cur & prev);
                gControllers[i].rawStickX = s->stickX;
                gControllers[i].rawStickY = s->stickY;
                sLsPrevBtn[i] = cur;
            }

            /* validation A: hash the applied input SET for df (tag 0x74/0x75). */
            {
                u32 hi = 2166136261u;
                for (i = 0; i < np; i++) {
                    LsInput* s = &sLsInput[i][df % LS_RING];
                    if (sLsDropped[i] && (sLsDropNever[i] || df > sLsDropLast[i])) {
                        continue; /* stale per-console ring data — not part of the set */
                    }
                    hi ^= ((u32) s->button << 16) ^ ((u32) (u8) s->stickX << 8) ^ ((u32) (u8) s->stickY) ^ (u32) i;
                    hi *= 16777619u;
                }
                netpak_debug_poke(0x74000000u | (df & 0xFFFFFFu));
                netpak_debug_poke(0x75000000u | (hi & 0xFFFFFFu));
            }

            /* validation B: hash all 8 karts' sim state (tag 0x76 frame, 0x77 hash)
             * — two consoles running the same inputs must produce the identical
             * hash. Keyed by df (the logical frame), so stalls don't misalign it. */
            {
                u32 h = 2166136261u;
                s32 pi, fi;
                for (pi = 0; pi < NUM_PLAYERS; pi++) {
                    Player* pl = &gPlayers[pi];
                    u32 fld[8];
                    memcpy(&fld[0], &pl->pos[0], 4);
                    memcpy(&fld[1], &pl->pos[1], 4);
                    memcpy(&fld[2], &pl->pos[2], 4);
                    fld[3] = ((u32) (u16) pl->rotation[0] << 16) | (u16) pl->rotation[1];
                    fld[4] = ((u32) (u16) pl->rotation[2] << 16) | (u16) pl->lapCount;
                    memcpy(&fld[5], &pl->speed, 4);
                    fld[6] = pl->effects;
                    fld[7] = ((u32) pl->type << 16) | (u16) pl->characterId; /* char
                        sync in the hash: unsynced characters = different physics
                        = silent divergence; now it fails the campaign instead */
                    for (fi = 0; fi < 8; fi++) {
                        h ^= fld[fi];
                        h *= 16777619u;
                    }
                }
                netpak_debug_poke(0x76000000u | (df & 0xFFFFFFu));
                netpak_debug_poke(0x77000000u | (h & 0xFFFFFFu));

#if NET_MENU_TEST
                /* frame-0 forensics: per-kart state hash (0xB0+k) and the
                 * char/type table (0xC0+k) at the FIRST simulated frame, so a
                 * race-entry divergence names its kart + shows whether the
                 * character roster itself split (barrier fail-forward flake). */
                if (df == 0 || (df >= 896 && df <= 960 && (df & 7) == 0)) {
                    for (pi = 0; pi < NUM_PLAYERS; pi++) {
                        Player* pl = &gPlayers[pi];
                        u32 kh = 2166136261u;
                        u32 fld[8];
                        memcpy(&fld[0], &pl->pos[0], 4);
                        memcpy(&fld[1], &pl->pos[1], 4);
                        memcpy(&fld[2], &pl->pos[2], 4);
                        fld[3] = ((u32) (u16) pl->rotation[0] << 16) | (u16) pl->rotation[1];
                        fld[4] = ((u32) (u16) pl->rotation[2] << 16) | (u16) pl->lapCount;
                        memcpy(&fld[5], &pl->speed, 4);
                        fld[6] = pl->effects;
                        fld[7] = ((u32) pl->type << 16) | (u16) pl->characterId;
                        for (fi = 0; fi < 8; fi++) {
                            kh ^= fld[fi];
                            kh *= 16777619u;
                        }
                        /* tag 0xB8, kart in bits 21-23 (0xB0+k collided with
                         * the legacy speed poke at 0xB0 -> garbage diffs) */
                        netpak_debug_poke(0xB8000000u | ((u32) pi << 21) | (kh & 0x1FFFFFu));
                        netpak_debug_poke(((0xC0u + (u32) pi) << 24) |
                                          (((u32) pl->type & 0xFFFFu) << 8) | ((u32) pl->characterId & 0xFFu));
                    }
                }
#endif

                /* live desync detector: remember + periodically broadcast.
                 * ONLY once f clears the delay ramp: while f < LS_DELAY the
                 * clamp maps SEVERAL ticks to df=0, each simulating a frame,
                 * so "hash of frame 0" has three different values in flight.
                 * A peer's copy arriving one tick late (guaranteed on a real
                 * network) then compares against a different repeat -> false
                 * red square at race entry, 100% reproducible on-device. */
                /* Store/broadcast each df ONCE — its FIRST evaluation. The
                 * delay ramp evaluates df=0 several times with evolving state;
                 * both consoles execute the identical ramp, so first-eval
                 * hashes are comparable. (Replaces an `f >= LS_DELAY` gate
                 * that the optimizer miscompiled into a dead branch — the
                 * detector was blind for several versions; proven by
                 * disassembly and the poison self-test.) */
                if (df != sLsHashLastDf) {
                sLsHashLastDf = df;
#if NET_MENU_TEST
                if (df < 40 || (df & 63) == 5) {
                    netpak_debug_poke(0xF6000000u | (df & 0xFFFFFFu)); /* store ran */
                }
#endif
                sLsHashFrame[df % LS_HASHRING] = df;
                sLsHashVal[df % LS_HASHRING] = h;
                if ((df & 15) == 0) {
                    LsHashMsg hm;
                    hm.tag = LSHASH_TAG;
                    hm.pad = 0;
                    hm.pad2 = 0;
                    hm.frame = df;
                    hm.hash = h;
                    netpak_send(NETPAK_BROADCAST, 0, &hm, sizeof(hm));
#if NET_MENU_TEST
                    netpak_debug_poke(0xE8000000u | (df & 0xFFFFFFu)); /* hash SENT */
#endif
                }
                }
            }

            sLsFrame++;
        }
    }
#endif /* NET_LOCKSTEP */
}

/* Pre-race start-state agreement: zero the CPU-AI state block (all of
 * cpu_vehicles_camera_path.c's per-CPU behaviour/path/steering/item-strategy
 * bss) BEFORE the course loads. The block carries menu-history residue that
 * differs host vs joiner; its exact value never mattered — only that every
 * console agrees. Clearing pre-load lets the game's own course/spawn init run
 * on top, so the real CPU karts (slots np..7 when fewer than 8 humans) stay
 * fully functional. Called from net_menu_start_race on every console. */
void net_lockstep_prerace_clear(void) {
#if NET_LOCKSTEP
    u8* start = unk_cpu_vehicles_camera_path_pad;
    u8* end = LS_CPU_STATE_END;
    bzero(start, (u32) (end - start));
    /* cameras 2-4 carry per-console menu residue; slot 1's lakitu/window
     * update (v20) reads camera1[1], so they must agree at race entry */
    bzero(&camera1[1], (u32) (sizeof(Camera) * 3));
    /* playerHUD[1..3] too: the item-window state machine (func_8007B34C,
     * driven for online slots since v20) steps slideItemBoxX/Y and timers in
     * these entries, which 1P mode never initializes — menu residue differed
     * per console, so the first item grant's roulette ran with different
     * timing and forked the shared RNG stream (the reproducible df~863
     * red square, on-device and in-harness). */
    {
        extern hud_player playerHUD[];
        bzero(&playerHUD[1], (u32) (sizeof(hud_player) * 3));
    }
#endif
}

/* Stall-gate query for race_logic_loop: true when net_lockstep_tick could not
 * assemble the full input set for the frame to simulate, so the sim must be held
 * this render-frame (both consoles freeze in sync). Always false outside lockstep. */
bool net_lockstep_stalled(void) {
#if NET_LOCKSTEP
    return sLsStall;
#else
    return false;
#endif
}

/* Path B Hook B: snapshot the sim's private RNG state after the per-frame sim work
 * and before rendering, so render (which also advances gRandomSeed16) can't drift
 * the sim stream. Paired with the load at the top of net_lockstep_tick. No-op
 * unless a lockstep race is active. Call from race_logic_loop between sim and
 * render. */
/* Background/cloud scroll must follow the LOCAL view. course_update_clouds
 * (pre-render, func_80059D00) reads camera1, which at that point still holds
 * the SIM camera (player 0's) — a joiner's background visibly rotated with
 * the HOST's movement. Swap in last frame's local camera around just this
 * call. Cloud scroll is pure per-view render state: per-console divergence
 * here is the point, and no RNG is consumed. */
void net_course_update_clouds_screen0(void) {
    extern void course_update_clouds(s32);
#if NET_LOCKSTEP
    if (netpak_present() && net_menu_online_active() && net_lockstep_local_slot() != 0 && sCamLocInit) {
        Camera save = *camera1;
        *camera1 = sCamLoc1;
        course_update_clouds(0);
        *camera1 = save;
        return;
    }
#endif
    course_update_clouds(0);
}

/* Drive the item-window state machine (and lakitu) for every online player.
 * Vanilla 1P only runs func_8007A910(0): grants for other slots armed a
 * window whose roulette never advanced — boxes broke, no item appeared.
 * Slot 1 gets the full driver (window + lakitu rescue: both objects exist in
 * 1P); slots 2-3 get the window machine only (no lakitu objects for them).
 * Runs in the RNG-protected pre-render span, identically on every console. */
void net_online_drive_item_windows(void) {
#if NET_LOCKSTEP
    extern void func_8007A910(s32);
    if (!netpak_present() || !net_menu_online_active()) {
        return;
    }
    if (net_menu_player_count() >= 2) {
        func_8007A910(1); /* slot 1: lakitu rescue + reverse detection (the
                           * item window is driven from the SIM since v20) */
    }
#endif
}

void net_lockstep_rng_save(void) {
#if NET_LOCKSTEP
    extern u16 gRandomSeed16;
    if (sLsRngActive) {
        sLsSimSeed = gRandomSeed16;
    }
#endif
}

/* THE PARKED-KART FIX: 1P-mode Grand Prix runs the HUMAN drive handler
 * (throttle / brake / steering routing) for PLAYER 0 ONLY —
 * handle_a_press_for_all_players_during_race() early-returns after
 * (gPlayerOne, gControllerOne, 0). Slots 1..7, which lockstep types HUMAN,
 * got per-player physics but no input routing and no CPU AI: they rolled to
 * the grid during staging and sat parked forever. So every console only ever
 * DROVE kart 0 — the host's — which read as "someone else controls the kart
 * I'm looking at" on joiners, and left each joiner's own kart dead on the
 * grid. Run the real handler for every other live human slot, fed by its
 * ring-driven controller (the drive loop already fills gControllers[i]).
 * Types and controller contents are identical on every console, so the added
 * calls are deterministic. The handler self-gates on HUMAN && !CPU, so
 * dropped players (converted to CPU bots) are skipped automatically.
 * Call from race_logic_loop's sim block, right after the game's own
 * handle_a_press_for_all_players_during_race(). */
void net_lockstep_drive_humans(void) {
#if NET_LOCKSTEP
    extern void handle_a_press_for_player_during_race(Player*, struct Controller*, s8);
    extern s32 net_menu_player_count(void);
    s32 i;
    s32 np;
    if (!netpak_present() || !net_menu_online_active() || gGamestate != RACING) {
        return;
    }
    np = net_menu_player_count();
    if (np > NET_MAX_SLOTS) {
        np = NET_MAX_SLOTS;
    }
    for (i = 1; i < np; i++) {
        handle_a_press_for_player_during_race(&gPlayers[i], &gControllers[i], (s8) i);
    }
#endif
}

/* Camera retarget (render-only): which slot the local 1P viewport should follow.
 * In an online lockstep race every console runs the same 8-kart sim but each
 * player OWNS a different slot (= node id), so the local camera must track the
 * local kart, not slot 0. Returns 0 everywhere else (offline / snapshot mode /
 * host node 0), so existing behavior is unchanged. Determinism-safe: only the
 * camera struct depends on this, never gPlayers. */
s32 net_lockstep_local_slot(void) {
#if NET_LOCKSTEP
    extern bool net_menu_online_active(void);
    extern s32 net_menu_node_id(void);
    if (netpak_present() && net_menu_online_active()) {
        /* Use the SAME slot this console drives its input into (sLsMyPlayer, cached
         * once at race entry), not a fresh net_menu_node_id() — the latter can read
         * back inconsistently at render time (observed at 4p: several consoles got 0
         * and all followed slot 0). Tying the camera to the drive-slot guarantees
         * each console views its own kart. Fall back to a fresh id pre-cache. */
        s32 s = (sLsMyPlayer >= 0) ? sLsMyPlayer : net_menu_node_id();
        if (s > 0 && s < NUM_PLAYERS) {
            return s;
        }
    }
#endif
    return 0;
}

/* Robust local chase-cam, part 1 (call from race_logic_loop AFTER the sim, BEFORE
 * render_player_one_1p_screen). Saves the sim's camera state (camera1 + camera-path
 * block + D_80152300), swaps in the LOCAL player's persistent camera context, and
 * runs MK64's real camera-follow for the local kart — so the local view gets every
 * genuine reaction (spinout/hit shake, wall bounce, drift lean, Lakitu rescue) for
 * the LOCAL kart, not slot 0's. net_lockstep_cam_pop() then restores the sim state,
 * so the shared sim never sees the swap (determinism preserved). No-op offline/host. */
/* Render-write isolation, FULL-STRUCT: the render path writes a dozen+ scattered
 * Player fields at RENDER rate (unk_002 bitfield, unk_048/0CC/0D4/050[screenId],
 * animFrame/GroupSelector, tyreSpeed, unk_206, unk_DA4, unk_DB4, even effects) —
 * render count differs per console, so any of them leaking into the sim is a
 * schedule lottery (the 310/865 divergences). Snapshot ALL karts before render,
 * restore after: the sim only ever sees its own deterministic writes. */
typedef struct { /* every field the render path writes (audited); ~120B/kart */
    u16   unk_002;
    Vec4s unk_048, unk_050, unk_0CC, unk_0D4;
    s16   slopeAccel, unk_206, unk_DA4;
    s32   tyreSpeed;
    u16   animFrameSelector[4], animGroupSelector[4];
    u32   effects;
    struct UnkPlayerInner unk_DB4;
} RenderSave;
static RenderSave sPlySave[NET_MAX_SLOTS];
static u8 sCamArrSave[0x4E0]; /* camera zoom/height/c-button arrays (main.c bss,
                               * D_801645D0..+0x4E0): written by the joiner-side
                               * chase-cam follow at RENDER rate, read by the sim's
                               * own camera follow inside the sim loop */
static bool sDb4Active;
extern u16 D_801645D0[];
#define RSAVE_CP(dst, src, pi) do { \
    (dst).unk_002 = (src)[pi].unk_002;   memcpy(&(dst).unk_048, &(src)[pi].unk_048, sizeof(Vec4s)); \
    memcpy(&(dst).unk_050, &(src)[pi].unk_050, sizeof(Vec4s)); memcpy(&(dst).unk_0CC, &(src)[pi].unk_0CC, sizeof(Vec4s)); \
    memcpy(&(dst).unk_0D4, &(src)[pi].unk_0D4, sizeof(Vec4s)); (dst).slopeAccel = (src)[pi].slopeAccel; \
    (dst).unk_206 = (src)[pi].unk_206;   (dst).unk_DA4 = (src)[pi].unk_DA4; \
    (dst).tyreSpeed = (src)[pi].tyreSpeed; (dst).effects = (src)[pi].effects; \
    memcpy((dst).animFrameSelector, (src)[pi].animFrameSelector, 8); \
    memcpy((dst).animGroupSelector, (src)[pi].animGroupSelector, 8); \
    (dst).unk_DB4 = (src)[pi].unk_DB4; } while (0)
#define RSAVE_RS(dst, pi, src) do { \
    (dst)[pi].unk_002 = (src).unk_002;   memcpy(&(dst)[pi].unk_048, &(src).unk_048, sizeof(Vec4s)); \
    memcpy(&(dst)[pi].unk_050, &(src).unk_050, sizeof(Vec4s)); memcpy(&(dst)[pi].unk_0CC, &(src).unk_0CC, sizeof(Vec4s)); \
    memcpy(&(dst)[pi].unk_0D4, &(src).unk_0D4, sizeof(Vec4s)); (dst)[pi].slopeAccel = (src).slopeAccel; \
    (dst)[pi].unk_206 = (src).unk_206;   (dst)[pi].unk_DA4 = (src).unk_DA4; \
    (dst)[pi].tyreSpeed = (src).tyreSpeed; (dst)[pi].effects = (src).effects; \
    memcpy((dst)[pi].animFrameSelector, (src).animFrameSelector, 8); \
    memcpy((dst)[pi].animGroupSelector, (src).animGroupSelector, 8); \
    (dst)[pi].unk_DB4 = (src).unk_DB4; } while (0)

void net_lockstep_cam_push(void) {
#if NET_LOCKSTEP
    s32 ls = net_lockstep_local_slot();
    u32 blk;
    s32 pi;

    /* RENDER-WRITE ISOLATION (all consoles, host included): the render mutates
     * Player.unk_DB4 (camera-bounce/wobble decay: camera.c follow + the kart
     * wobble in render_player.c) at RENDER rate, which differs per console —
     * a schedule-dependent leak into sim-owned structs. The fields are zero
     * except briefly after bumps, so the leak fires as a rare binary lottery
     * (the frame-310 divergence, same two alternate hashes every occurrence).
     * Snapshot every kart's region before render; pop restores. */
#if NET_RENDER_ISOLATION
    if (netpak_present() && net_menu_online_active() && gGamestate == RACING) {
        for (pi = 0; pi < NET_MAX_SLOTS; pi++) {
            RSAVE_CP(sPlySave[pi], gPlayers, pi);
        }
        memcpy(sCamArrSave, D_801645D0, sizeof(sCamArrSave));
        sDb4Active = true;
    }
#endif

    sCamActive = 0;
    if (ls == 0) {
        return; /* host / offline: slot 0 is already this console's kart */
    }
    blk = (u32) (LS_CPU_STATE_END - unk_cpu_vehicles_camera_path_pad);
    if (blk > LS_CAMBLK_MAX) {
        return; /* safety: never overflow the save buffers */
    }
    sCamActive = ls;

    /* save the sim's camera state (must be byte-identical again after pop) */
    memcpy(sCamSimBlk, unk_cpu_vehicles_camera_path_pad, blk);
    sCamSim1 = *camera1;
    memcpy(sCamSimD300, D_80152300, sizeof(sCamSimD300));

    /* first render of a race: seed the persistent CAMERA state from the sim */
    if (!sCamLocInit) {
        sCamLoc1 = sCamSim1;
        memcpy(sCamLocD300, sCamSimD300, sizeof(sCamLocD300));
        sCamLocInit = true;
    }

    /* Render against the LOCAL camera (persistent across frames) but a FRESH
     * copy of the camera-path/course block, re-seeded from the sim EVERY
     * frame. The old once-per-race block copy went stale: the renderer reads
     * course-position bookkeeping out of this block, and stale data drew the
     * WRONG COURSE CHUNKS around the joiner's kart — geometry where there is
     * none and none where there is ("drives through the ground and walls",
     * proven render-side by the live desync detector staying green). The
     * follow's cross-frame continuity lives in camera1 + D_80152300, which
     * stay persistent; the block writes it makes during the render are
     * discarded by the next re-seed. Isolation from the sim is unchanged —
     * pop still restores the sim's block bytes (a per-console camera follow
     * writing into sim-read CPU-AI data was the original 4p desync). */
    memcpy(sCamLocBlk, sCamSimBlk, blk);
    memcpy(unk_cpu_vehicles_camera_path_pad, sCamLocBlk, blk);
    *camera1 = sCamLoc1;
    memcpy(D_80152300, sCamLocD300, sizeof(sCamLocD300));
    camera1->playerId = (s16) ls;

    /* ROOT-CAUSE FIX: the intro→chase camera-mode transition (func_8001F87C) is
     * edge-triggered — it fires only on the exact frame the shared frame-counter
     * D_80164A2C hits 60, and the SIM's own follow consumes that edge. The local
     * context therefore stays in intro mode 8 forever (positioner ignores which
     * kart we pass — this is why joiners tracked slot 1 at any player count).
     * Do the transition ourselves: once the local kart is racing (staging bits
     * clear), force chase mode 1 and snap the camera heading to the local kart,
     * exactly what F87C does at the edge. */
    if ((D_80152300[0] == 8) && !(gPlayers[ls].type & (PLAYER_STAGING | PLAYER_START_SEQUENCE))) {
        D_80152300[0] = 1;
        camera1->rot[1] = gPlayers[ls].rotation[1];
        camera1->unk_2C = gPlayers[ls].rotation[1];
    }
    /* index stays 0: it selects the per-SCREEN camera tuning state (chase distance/
     * height/zoom arrays), which 1P mode only maintains for screen 0 — passing ls
     * left those zeroed, putting the camera on the ground at the kart's tail. The
     * kart to follow is carried by the Player* + camera->playerId, not the index. */
    func_8001EE98(&gPlayers[ls], camera1, 0);

    /* Course-SEGMENT culling: the renderer picks the visible course chunk from
     * wrapper->player's track section (render_courses.c uses
     * get_track_section_id(player->collision.meshIndexZX)), and that player is
     * hardwired to gPlayers[0] — so a camera far from slot 0 renders slot 0's
     * chunk and void everywhere else ("karts floating off-track"). Point the
     * renderer at the LOCAL kart for this render; pop restores it. */
    D_800DC5EC->player = &gPlayers[ls];
#endif
}

/* Local chase-cam, part 2 (call AFTER render_player_one_1p_screen): persist the local
 * camera context, then restore the sim's camera state so the next sim frame is
 * bit-identical. Pairs with cam_push. */
static u8  sDiagCamMode;   /* LOCAL view camera health, sampled in cam_pop */
static u8  sDiagCamPid;    /*   BEFORE the sim camera is restored */
static u16 sDiagCamDist;

void net_lockstep_cam_pop(void) {
#if NET_LOCKSTEP
    extern Camera* gCopyCamera[4];
    u32 blk;
    s32 pi;

    /* AUDIO LISTENER: the sound engine pans/attenuates against
     * gCopyCamera[0], which normally aliases camera1 — the SIM camera, i.e.
     * the HOST's view. On a joiner, re-point it at our persistent local
     * camera so engine/doppler/pan follow OUR kart ("the joiner seems to be
     * listening to the host's audio"). Menus reset the pointer to camera1
     * (func_800C2474), so refresh every frame during the race. */
    if (net_lockstep_local_slot() != 0 && sCamLocInit) {
        gCopyCamera[0] = &sCamLoc1;
    }

#if NET_MENU_TEST || NET_DIAG
    { /* sample the LOCAL view camera before restoring the sim's (0xDF poke
       * reads these — sampling after the restore measured the HOST's camera
       * from the joiner's kart: always pegged at max) */
        s32 dls = net_lockstep_local_slot();
        f32 dx = camera1->pos[0] - gPlayers[dls].pos[0];
        f32 dz = camera1->pos[2] - gPlayers[dls].pos[2];
        u32 dd = (u32) ((dx < 0.0f ? -dx : dx) + (dz < 0.0f ? -dz : dz));
        sDiagCamMode = (u8) D_80152300[0];
        sDiagCamPid = (u8) (camera1->playerId & 0xF);
        sDiagCamDist = (u16) (dd > 0xFFF ? 0xFFF : dd);
    }
#endif

#if NET_RENDER_ISOLATION
    if (sDb4Active) {
        { /* TEMP #34: render-list count + kart visibility bits, post-render */
            extern s32 gPlayersToRenderCount;
            static u32 rlDbg;
            if ((rlDbg++ & 15) == 0 || gPlayersToRenderCount == 0) {
                netpak_debug_poke(0xDD000000u |
                                  ((u32) (gPlayers[1].unk_002 & 0xFF) << 16) |
                                  ((u32) (gPlayers[0].unk_002 & 0xFF) << 8) |
                                  ((u32) gPlayersToRenderCount & 0xFF));
            }
        }
        for (pi = 0; pi < NET_MAX_SLOTS; pi++) {
            RSAVE_RS(gPlayers, pi, sPlySave[pi]);
        }
        memcpy(D_801645D0, sCamArrSave, sizeof(sCamArrSave));
        sDb4Active = false;
    }
#endif

    if (sCamActive == 0) {
        return;
    }
    blk = (u32) (LS_CPU_STATE_END - unk_cpu_vehicles_camera_path_pad);
    if (blk > LS_CAMBLK_MAX) {
        blk = LS_CAMBLK_MAX;
    }

    /* persist the local camera's evolved state for next frame */
    memcpy(sCamLocBlk, unk_cpu_vehicles_camera_path_pad, blk);
    sCamLoc1 = *camera1;
    memcpy(sCamLocD300, D_80152300, sizeof(sCamLocD300));

    /* restore the sim's camera state -> determinism preserved */
    memcpy(unk_cpu_vehicles_camera_path_pad, sCamSimBlk, blk);
    *camera1 = sCamSim1;
    memcpy(D_80152300, sCamSimD300, sizeof(sCamSimD300));
    D_800DC5EC->player = &gPlayers[0]; /* restore the renderer's player (segment culling) */

    sCamActive = 0;
#endif
}

s16 gNetCullSection; /* written by render_courses.c: last course chunk drawn */
#if NET_MENU_TEST && NET_DEMO_AUTODRIVE
const s32 gNetRaceLaps = 1; /* test: bots finish quickly -> post-race flow reachable */
#else
const s32 gNetRaceLaps = 3; /* product: vanilla */
#endif
u16 gNetColCnt[8];   /* written by collision.c: surface-collision tests per kart */

/* TEMP #31 render diag: which course chunk the renderer chose vs the section
 * the LOCAL kart is actually in. Persistent mismatch = the joiner's 'drives
 * through walls' world-misdraw, now measurable in the harness. */
void net_render_cull_diag(void) {
#if NET_LOCKSTEP && (NET_MENU_TEST || NET_DIAG)
    extern s16 get_track_section_id(u16);
    static u32 cdDbg;
    s32 ls = net_lockstep_local_slot();
    if (!netpak_present() || !net_menu_online_active() || gGamestate != RACING) {
        return;
    }
    if ((cdDbg++ & 15) == 0) {
        s32 kartSec = get_track_section_id(gPlayers[ls].collision.meshIndexZX);
        if (ls == 0) { /* host: no push/pop — sample the live camera here */
            f32 dx = camera1->pos[0] - gPlayers[0].pos[0];
            f32 dz = camera1->pos[2] - gPlayers[0].pos[2];
            u32 dd = (u32) ((dx < 0.0f ? -dx : dx) + (dz < 0.0f ? -dz : dz));
            sDiagCamMode = (u8) D_80152300[0];
            sDiagCamPid = (u8) (camera1->playerId & 0xF);
            sDiagCamDist = (u16) (dd > 0xFFF ? 0xFFF : dd);
        }
        /* 0xDE: drawn course chunk vs the section the kart is really in */
        netpak_debug_poke(0xDE000000u | ((u32) (ls & 0xF) << 20) |
                          (((u32) gNetCullSection & 0xFF) << 8) | ((u32) kartSec & 0xFF));
        /* 0xDF: LOCAL view camera health — mode (8=intro, 1=chase), followed
         * playerId, XZ manhattan distance to the local kart (joiner values
         * sampled in cam_pop before the sim camera is restored). */
        netpak_debug_poke(0xDF000000u | (((u32) sDiagCamMode & 0xFFu) << 16) |
                          ((u32) (sDiagCamPid & 0xFu) << 12) | ((u32) sDiagCamDist & 0xFFFu));
        /* local kart trail (kartplot.py-compatible tags) */
        netpak_debug_poke(((0x80u + (u32) ls) << 24) | (u16) (s16) gPlayers[ls].pos[0]);
        netpak_debug_poke(((0x90u + (u32) ls) << 24) | (u16) (s16) gPlayers[ls].pos[2]);
        { /* 0xE0+k: surface-collision tests per kart since last poke — which
           * karts does the engine actually collision-test against the track? */
            extern u16 gNetColCnt[8];
            s32 ck;
            for (ck = 0; ck < 8; ck++) {
                netpak_debug_poke(((0xE0u + (u32) ck) << 24) | gNetColCnt[ck]);
                gNetColCnt[ck] = 0;
            }
        }
    }
#endif
}

/* DESYNC indicator (ships in ALL online builds): once the live detector has
 * latched, flash a red square top-left of the race view every other half
 * second. If a player sees it, the two consoles' simulations have split and
 * everything after is untrustworthy — report the moment it appeared. */
void net_lockstep_desync_indicator(void) {
#if NET_LOCKSTEP
    extern Gfx* gDisplayListHead;
    if (!sLsDesync || gGamestate != RACING) {
        return;
    }
    if (sRaceFrames & 16) {
        return; /* flash */
    }
    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    gDPSetFillColor(gDisplayListHead++,
                    (GPACK_RGBA5551(255, 0, 0, 1) << 16) | GPACK_RGBA5551(255, 0, 0, 1));
    gDPFillRectangle(gDisplayListHead++, 12, 12, 26, 26);
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
#endif
}

/* Test-build input overlay: draw the autodrive's injected inputs (stick bar +
 * A/B lights) in the lower-left of the race view. Called from race_logic_loop
 * right after cam_pop, while the master display list is still open. No-op in
 * human/product builds (NET_DEMO_AUTODRIVE off) and outside online races. */
void net_autodrive_overlay(void) {
#if NET_DEMO_AUTODRIVE
    extern Gfx* gDisplayListHead;
    s32 cx;
    if (!netpak_present() || !net_menu_online_active() || gGamestate != RACING) {
        return;
    }
    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    /* stick bar: dark track, white center tick, yellow marker at stick pos */
    gDPSetFillColor(gDisplayListHead++,
                    (GPACK_RGBA5551(40, 40, 40, 1) << 16) | GPACK_RGBA5551(40, 40, 40, 1));
    gDPFillRectangle(gDisplayListHead++, 16, 208, 116, 216);
    gDPPipeSync(gDisplayListHead++);
    gDPSetFillColor(gDisplayListHead++,
                    (GPACK_RGBA5551(255, 255, 255, 1) << 16) | GPACK_RGBA5551(255, 255, 255, 1));
    gDPFillRectangle(gDisplayListHead++, 65, 206, 67, 218);
    gDPPipeSync(gDisplayListHead++);
    cx = 66 + ((s32) sAdOverlayStick * 48) / 80;
    gDPSetFillColor(gDisplayListHead++,
                    (GPACK_RGBA5551(255, 220, 0, 1) << 16) | GPACK_RGBA5551(255, 220, 0, 1));
    gDPFillRectangle(gDisplayListHead++, cx - 2, 207, cx + 2, 217);
    gDPPipeSync(gDisplayListHead++);
    /* A light (green when held), B light (red when held) */
    gDPSetFillColor(gDisplayListHead++, (sAdOverlayBtn & A_BUTTON)
                    ? ((GPACK_RGBA5551(0, 230, 0, 1) << 16) | GPACK_RGBA5551(0, 230, 0, 1))
                    : ((GPACK_RGBA5551(20, 60, 20, 1) << 16) | GPACK_RGBA5551(20, 60, 20, 1)));
    gDPFillRectangle(gDisplayListHead++, 122, 206, 134, 218);
    gDPPipeSync(gDisplayListHead++);
    gDPSetFillColor(gDisplayListHead++, (sAdOverlayBtn & B_BUTTON)
                    ? ((GPACK_RGBA5551(230, 30, 30, 1) << 16) | GPACK_RGBA5551(230, 30, 30, 1))
                    : ((GPACK_RGBA5551(60, 20, 20, 1) << 16) | GPACK_RGBA5551(60, 20, 20, 1)));
    gDPFillRectangle(gDisplayListHead++, 138, 206, 150, 218);
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
#endif
}

extern bool net_menu_online_active(void);
extern u16 gRandomSeed16; /* gate: netcode only in online races */

void net_race_frame(void) {
    u32 epoch;

    if (!netpak_present()) {
        return;
    }

    /* Offline races (GP / VS / Time Trials) run with pure local input — the
     * in-race netcode only ever engages for an ONLINE race. */
    if (!net_menu_online_active()) {
        return;
    }

    /* A savestate/reset/reconnect invalidates everyone's continuity: drop all
     * puppet state and let fresh snapshots re-establish it (README §5). */
    epoch = netpak_epoch();
    if (epoch != sNetEpoch) {
        sNetEpoch = epoch;
        net_race_reset();
    }

    /* Reset replication when entering a race so slot mapping starts clean. */
    if (gGamestate == RACING && sPrevGamestate != RACING) {
        net_race_reset();
        sRaceFrames = 0;
#if NET_LOCKSTEP
        net_lockstep_reset();
#endif
    }
    sPrevGamestate = gGamestate;

#if NET_DEBUG
    net_race_debug_state();
#endif

    if (gGamestate != RACING) {
        return;
    }
    sRaceFrames++;

#if NET_DETERMINISM_TEST
    /* Emit (frame, state-hash) and do NO networking, so the two instances are
     * fully independent identical sims. Compare the hash streams by frame. */
    {
        u32 h = 2166136261u; /* FNV-1a over all 8 karts' sim-relevant fields */
        s32 pi, fi;
        for (pi = 0; pi < NUM_PLAYERS; pi++) {
            Player* p = &gPlayers[pi];
            u32 f[8];
            memcpy(&f[0], &p->pos[0], 4);
            memcpy(&f[1], &p->pos[1], 4);
            memcpy(&f[2], &p->pos[2], 4);
            f[3] = ((u32)(u16) p->rotation[0] << 16) | (u16) p->rotation[1];
            f[4] = ((u32)(u16) p->rotation[2] << 16) | (u16) p->lapCount;
            memcpy(&f[5], &p->speed, 4);
            f[6] = p->effects;
            f[7] = (u32) p->type;
            for (fi = 0; fi < 8; fi++) {
                h ^= f[fi];
                h *= 16777619u;
            }
        }
        netpak_debug_poke(0x76000000u | ((u32) sRaceFrames & 0xFFFFFFu));
        netpak_debug_poke(0x77000000u | (h & 0xFFFFFFu));
    }
    return;
#endif

#if NET_LOCKSTEP
    return; /* lockstep runs its own tick after read_controllers (main.c) */
#endif

    net_race_rx();
    net_race_apply_characters();
    net_race_scan_hits();
    net_race_interpolate();
    net_race_tx();
    net_race_events_tx();

#if NET_DEBUG
    /* Heartbeat: proves net_race_frame ticks in-race, and reports how many peer
     * puppet slots are live (tag 0x56 = (frame<<8)|puppetCount). */
    {
        s32 pc = 0, i;
        for (i = 1; i < NET_MAX_SLOTS; i++) {
            if (sPeerUsed[i]) {
                pc++;
            }
        }
        netpak_debug_poke(0x56000000u | (((u32) sRaceFrames & 0xFFFF) << 8) | ((u32) pc & 0xFF));
    }
#endif
}

/* Returns true exactly once, when the device is connected and it's time to
 * jump into the networked race. Latches so it never fires twice. The caller
 * (main.c) then performs the actual menu-bypass race launch.
 *
 * Readiness: device present, past the boot grace, LINK_UP, and either in a
 * relay SESSION (real multiplayer) or LINK_UP held long enough that we infer
 * loopback (single-instance self-test). */
bool net_race_autostart_check(void) {
    u32 status;
    u32 now;

    if (sAutoStarted || !netpak_present()) {
        return false;
    }
    if (gGamestate == RACING) {
        return false; /* already racing */
    }
    if (gGlobalTimer < NET_AUTOSTART_BOOT_FRAMES) {
        return false; /* still booting */
    }

    status = netpak_status();
    if (!(status & NETPAK_STATUS_LINK_UP)) {
        sLinkUpUs = 0; /* link not up yet; (re)arm the loopback timer */
        return false;
    }

    now = net_now_us();
    if (sLinkUpUs == 0) {
        sLinkUpUs = now;
    }
    if ((status & NETPAK_STATUS_SESSION) ||
        (now - sLinkUpUs > NET_AUTOSTART_LOOPBACK_US)) {
        sAutoStarted = true;
        return true;
    }
    return false;
}

bool net_race_apply_puppet(Player* player, s32 playerId) {
    RemoteSlot* r;

    if (!netpak_present() || !net_menu_online_active()) {
        return false; /* offline race: leave every kart to local simulation */
    }
    if (playerId <= 0 || playerId >= NUM_PLAYERS) {
        return false; /* never puppet the local slot 0 */
    }
    r = &sRemote[playerId];
    if (!r->valid) {
        return false; /* no snapshot yet — leave the slot as a normal CPU */
    }

    /* A remote-driven puppet is a fully active racer. Clear the staging/countdown
     * flags it would otherwise keep forever: apply_puppet skips local simulation,
     * so the puppet never runs the code that clears them. Any kart left flagged
     * PLAYER_STAGING stalls the GP intro camera — func_8001F87C waits for all 8
     * karts past staging before handing over to the racing camera, so a stuck
     * puppet freezes the view at the start line (the reported bug). */
    player->type &= ~(PLAYER_STAGING | PLAYER_START_SEQUENCE);

    /* oldPos before we move it, so per-frame deltas the renderer/anim reads
     * stay sane. */
    player->oldPos[0] = player->pos[0];
    player->oldPos[1] = player->pos[1];
    player->oldPos[2] = player->pos[2];

    /* Apply the frame's interpolated transform (computed in net_race_frame). */
    player->pos[0] = r->renderPos[0];
    player->pos[1] = r->renderPos[1];
    player->pos[2] = r->renderPos[2];
    player->rotation[1] = r->renderYaw; /* yaw drives the billboard sprite */
    player->speed = r->speed;
    player->currentSpeed = r->speed;

    /* Replicate the remote kart's visual state so opponents show their star
     * sparkle, mushroom/mini-turbo boost, spinout, and squish/shrink instead of
     * looking plain. Display-only: the puppet skips local sim, so these flags
     * just feed render_player (STAR_EFFECT sparkle, SQUISH scale, etc.). */
    player->effects = r->effects;

    /* characterId is applied once per change in net_race_apply_characters()
     * (called from net_race_frame), which also reloads the body palette — doing
     * it here (per subtick) would re-DMA the palette needlessly. */

    return true; /* caller skips local simulation for this slot */
}
