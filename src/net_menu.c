/**
 * net_menu.c — NetPak64 online menu (host / join / code entry).
 *
 * Drives the NETWORK_VS_MENU screen: pick HOST or JOIN, create a room and show
 * its code, or dial in a 6-char code to join. Rendered in the stock menu style
 * (print_text over the menu background, menu sounds). menus.c and menu_items.c
 * delegate here (net_menu_update / net_menu_render).
 */
#include <ultra64.h>
#include "common_structs.h"
#include "defines.h"
#include <sounds.h>
#include "menus.h"          /* is_screen_being_faded */
#include "menu_items.h"     /* print_text*, set_text_color, func_online_fade */
#include "audio/external.h" /* play_sound2 */
#include "netpak.h"
#include "net_menu.h"

/* Test/repro: allow a JOINed peer to START (fixed-room two-instance headless
 * pairing). 0 for the real host-authoritative product build. */
#define NET_MENU_JOINED_CAN_START 0

/* Race-config globals set when the host starts (declared in main.h / menus.h /
 * code_800029B0.h; externed here to avoid heavy includes). */
extern s32 gModeSelection;
extern s32 gScreenModeSelection;
extern s32 gCCSelection;
extern s32 gPlayerCountSelection1;
extern u16 gDemoMode;
extern s32 gIsMirrorMode;
extern s16 gPlaceItemBoxes;
extern s8  gPlayerCount;         /* menus.h */
extern s8  gDemoUseController;   /* menus.h */
extern s16 gCurrentCourseId;     /* course.h — the track the race loads */
extern s8  gCharacterSelections[4]; /* menus.c — this console's character pick */
extern Gfx* gDisplayListHead;    /* main.h — for the barrier popup's fill box */

/* Room-code alphabet (netpak-spec §5.1): 32 symbols, no 0/O or 1/I. */
static const char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define CODE_LEN 6

/* Player-name alphabet: leading space = "blank" slot (trimmed on save), then
 * the full font set that reads well at roster size. Names are what the other
 * players see in the lobby (relay identity, netpak_set_name). */
static const char kNameAlphabet[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-.";
#define NAME_LEN 8

/* The 16 selectable race courses, in the stock cup order (Mushroom / Flower /
 * Star / Special). The host cycles this list in the lobby; the chosen course id
 * is locked and broadcast to every peer so the whole room loads the same track.
 * Course ids per include/course.h. */
typedef struct {
    u8          id;
    const char* name;
} OnlineCourse;
static const OnlineCourse kCourses[] = {
    { 0x08, "LUIGI RACEWAY" },     /* COURSE_LUIGI_RACEWAY   */
    { 0x09, "MOO MOO FARM" },      /* COURSE_MOO_MOO_FARM    */
    { 0x06, "KOOPA BEACH" },       /* COURSE_KOOPA_BEACH     */
    { 0x0B, "KALIMARI DESERT" },   /* COURSE_KALAMARI_DESERT */
    { 0x0A, "TOADS TURNPIKE" },    /* COURSE_TOADS_TURNPIKE  */
    { 0x05, "FRAPPE SNOWLAND" },   /* COURSE_FRAPPE_SNOWLAND */
    { 0x01, "CHOCO MOUNTAIN" },    /* COURSE_CHOCO_MOUNTAIN  */
    { 0x00, "MARIO RACEWAY" },     /* COURSE_MARIO_RACEWAY   */
    { 0x0E, "WARIO STADIUM" },     /* COURSE_WARIO_STADIUM   */
    { 0x0C, "SHERBET LAND" },      /* COURSE_SHERBET_LAND    */
    { 0x07, "ROYAL RACEWAY" },     /* COURSE_ROYAL_RACEWAY   */
    { 0x02, "BOWSERS CASTLE" },    /* COURSE_BOWSER_CASTLE   */
    { 0x12, "DK JUNGLE PKWY" },    /* COURSE_DK_JUNGLE       */
    { 0x04, "YOSHI VALLEY" },      /* COURSE_YOSHI_VALLEY    */
    { 0x03, "BANSHEE BOARDWALK" }, /* COURSE_BANSHEE_BOARDWALK */
    { 0x0D, "RAINBOW ROAD" },      /* COURSE_RAINBOW_ROAD    */
};
#define NUM_ONLINE_COURSES ((s32)(sizeof(kCourses) / sizeof(kCourses[0])))

/* ch1 lobby control message: the host tells every peer to start the race on a
 * specific course. Reliable + ordered (spec §10), distinct tag from net_race's
 * in-race NetEvent ('E') so the two never alias. */
#define OLMSG_TAG   0x53 /* 'S' */
#define OLMSG_START 1 /* host -> peers: leave the lobby, load this course */
#define OLMSG_READY 2 /* player -> host: driver picked, I'm at the start barrier */
#define OLMSG_GO    3 /* host -> peers: everyone's ready, launch the race now */
#define OLMSG_COURSE 4 /* host -> peers: course picked on the REAL course-select
                        * screen (v32: course chosen AFTER char select) */
typedef struct {
    u8 tag;      /* OLMSG_TAG */
    u8 type;     /* OLMSG_* */
    u8 course;   /* course id to load (OLMSG_START) */
    u8 pad;
    u8 chars[8]; /* READY: [0] = sender's character pick.
                  * GO:    full per-slot character table (CHARACTER SYNC —
                  * characters have different physics, so unsynced picks made
                  * every console simulate a DIFFERENT race). */
} OnlineMsg; /* 12 bytes */

/* ROM-version cross-check: every lobby control message carries the sender's
 * NETPAK_ROM_VERSION in OnlineMsg.pad (v12 and older always sent 0). Any
 * mismatch means the two consoles run DIFFERENT sims — a guaranteed desync —
 * so it latches here and renders as a persistent warning. */
static u8 sVerMismatch;  /* the offending peer's version + 1 (0 = none seen) */
bool net_menu_version_mismatch(u8* peerVer) {
    if (sVerMismatch != 0 && peerVer != NULL) {
        *peerVer = (u8) (sVerMismatch - 1);
    }
    return sVerMismatch != 0;
}
static void net_menu_check_ver(u8 pad) {
    if (pad != (u8) NETPAK_ROM_VERSION) {
        sVerMismatch = (u8) (pad + 1);
    }
}

enum OnlineMenuState {
    OM_MAIN,       /* HOST GAME / JOIN GAME / NAME */
    OM_HOSTING,    /* created a room; showing code, waiting for players */
    OM_JOIN_ENTRY, /* dialing in a code */
    OM_JOINED,     /* joined a room; waiting for the host to start */
    OM_NAME_ENTRY  /* editing the player name */
};
#define OM_MAIN_OPTIONS 3 /* HOST / JOIN / NAME */

static s32  sState;
static s32  sSel;             /* OM_MAIN cursor: 0 = HOST, 1 = JOIN, 2 = NAME */
static char sCode[8];         /* room code (create result or join entry) */
static s32  sCcSel = CC_100;  /* host's engine-class pick (50/100/150cc) */
static s32  sOnlineCc = CC_100; /* class this race runs at (synced via START) */
static s32  sEntryPos;        /* OM_JOIN_ENTRY cursor 0..CODE_LEN-1 */
static char sName[NAME_LEN + 1]; /* player name, space-padded while editing */
static s32  sNamePos;         /* OM_NAME_ENTRY cursor 0..NAME_LEN-1 */

static void name_load(void);
static s32  sNodeId;          /* our node id after create/join */
static s32  sLastErr;         /* last session errno (0 = ok) for display */

/* Lobby roster (peers other than us), refreshed periodically from the device
 * (netpak_peers reads the device-local peer table — no relay round-trip). */
static netpak_peer_t sPeers[8];
static s32           sPeerCount;
static u32           sPeerPoll;

/* Host's course selection (index into kCourses). */
static s32  sCourseSel;
/* One-shot: set when a race is starting online, carrying the locked course id.
 * course_select_menu_act consumes it (via net_menu_take_online_course) to lock
 * the track and skip the manual course pick, so the whole room lands together. */
static bool sOnlineArmed;
static u8   sOnlineCourse;
/* True when a launch room code was supplied (ares NP64_ROOM): the join code is
 * pre-filled and code entry is skipped, so host + joiner pair on the same code
 * without typing it. */
static bool sHavePreset;

/* True from the moment an ONLINE race launches until the player leaves the
 * session. The netcode (snapshot / lockstep) gates on this so it NEVER touches
 * an offline race — offline GP/VS/Time-Trials run with pure local input. */
static bool sOnlineActive;

/* --- Synchronized start barrier ------------------------------------------- */
/* After the lobby START, every player picks a driver, then holds at the map
 * screen ("WAITING FOR ALL PLAYERS TO START") until everyone is ready, then all
 * launch together. The host coordinates: joiners send OLMSG_READY to the host
 * (whose node id they learn from the START packet's source, so this works even
 * if the peer roster is incomplete); the host, once every peer is ready, sends
 * OLMSG_GO and launches, and the joiners launch on GO. */
static bool sIsHost;              /* set at start: am I the room's start-coordinator? */
static u8   sHostNode;            /* joiner: host node id, learned from START src */
static u8   sBarrierPeer[8];      /* host: peers we must hear READY from */
static bool sBarrierReady[8];     /* host: which of those have reported ready */
static s32  sBarrierN;            /* host: number of peers to wait for */
static u32  sBarrierTick;         /* rebroadcast pacing */
static bool sBarrierWaiting;      /* true while holding at the barrier (drives popup) */
static u32  sBarrierWaitTicks;    /* frames spent waiting at the barrier (timeout) */
static u32  sHostGonePolls;       /* lobby: consecutive roster polls with the host absent */
static u8   sBarrierPick[8];      /* host: character pick reported by each barrier peer */
static u8   sSyncChars[8];        /* per-slot character table for the coming race */
static bool sSyncCharsValid;      /* set once the table is agreed (GO) */
#define BARRIER_TIMEOUT_TICKS 1800 /* ~30 s: fail FORWARD (launch; in-race drop                                     * arbitration turns absentees into bots) */
#define LOBBY_HOST_GONE_POLLS 6    /* ~3 s of roster polls: fall BACK to the menu                                     * (no course chosen yet, nothing to launch) */

/* Configure a full-screen 1-local-player VERSUS race (opponents = network
 * puppets in the CPU slots) and drop into the game's real character select. */
extern void net_lockstep_prerace_clear(void); /* net_race.c — start-state agreement */

static s32 sOnlineDelay; /* lockstep input delay for the coming race (v40):
                          * host sizes it from the worst lobby RTT
                          * (net_menu_pick_delay); every console adopts the
                          * same value from START chars[1]. 0 until set;
                          * net_lockstep_set_delay clamps to 2..8. */

static void net_menu_start_race(void) {
    extern void net_lockstep_set_delay(s32 d);
    net_lockstep_set_delay(sOnlineDelay); /* v40: RTT-sized input delay, same
                                           * value on every console (START
                                           * chars[1]); 0/garbage clamps to 2 */
    net_lockstep_prerace_clear(); /* zero CPU-AI residue BEFORE the course loads */
    /* Engine substrate = GRAND_PRIX 1-player: it spawns the full 8-kart grid
     * (1 human in slot 0 + 7 CPU in slots 1-7), and the netcode overrides those
     * CPU slots with remote puppets. 1-player VERSUS has NO spawn path in the
     * engine (spawn_players.c SCREEN_MODE_1P handles only GP + TIME_TRIALS), so
     * the kart never gets placed on the grid and can't drive. Presented to the
     * player as "ONLINE" in the menus regardless of the underlying mode. */
    gModeSelection = GRAND_PRIX;
    gScreenModeSelection = SCREEN_MODE_1P;
    gPlayerCount = 1;
    gPlayerCountSelection1 = 1;
    gCCSelection = sOnlineCc; /* host-picked class, synced via START (CPU
                               * speeds depend on it -> must match on every
                               * console or the shared sim forks) */
    gPlaceItemBoxes = 1;
    gIsMirrorMode = 0;
    gDemoMode = DEMO_MODE_INACTIVE; /* human drives slot 0, not AI */
    gDemoUseController = 0;
    gCurrentCourseId = (sOnlineCourse != 0xFF) ? sOnlineCourse : 0; /* placeholder
        until the course-select screen decides (v32) */
    sOnlineArmed = true;   /* course_select_menu_act auto-locks this course */
    sOnlineActive = true;  /* gate the in-race netcode to this online race only */
    func_online_start_fade(); /* -> CHARACTER_SELECT_MENU */
    play_sound2(SOUND_MENU_OK_CLICKED);
}

/* True while an online race is in progress (netcode gate). Set when an online
 * race launches, cleared when leaving the session / re-entering the online
 * screen — so the in-race netcode never runs during an offline race. */
bool net_menu_online_active(void) {
    return sOnlineActive;
}

/* True while an online race is being set up: course_select_menu_act auto-drives
 * the real CUP->COURSE->OK->launch flow (course pre-locked) instead of the human
 * picking. Persistent (not one-shot) so it can drive across several frames; the
 * caller clears it via net_menu_online_clear() when it fires the launch. */
bool net_menu_online_pending(void) {
    return sOnlineArmed;
}

/* Host-locked course id for the pending online race (valid while pending). */
s32 net_menu_online_course(void) {
    return sOnlineArmed ? (s32)sOnlineCourse : -1;
}

/* This console's node id == its lockstep player index. */
s32 net_menu_node_id(void) {
    return sNodeId;
}

/* Players in the room this frame (self + peers). */
s32 net_menu_player_count(void) {
    return sPeerCount + 1;
}

/* Called by course_select_menu_act once it has fired the OK-confirm launch, so a
 * later offline race isn't auto-advanced. */
void net_menu_online_clear(void) {
    sOnlineArmed = false;
}

/* Reliable-broadcast a lobby control message to every known peer (ch1 is
 * unicast-only per spec §11, so send one copy per peer). */
static void online_msg_send_all(const OnlineMsg* m) {
    s32 i;
    for (i = 0; i < sPeerCount && i < 8; i++) {
        netpak_send(sPeers[i].node_id, 1, m, sizeof(*m));
    }
}

/* Build the shared per-slot character table from the picks the host knows:
 * humans get their reported picks; remaining slots are filled with the lowest
 * character ids not taken by a human (dup human picks allowed, like vanilla
 * VS). Deterministic given the same inputs, and only the HOST's copy matters —
 * it is broadcast in GO and applied verbatim everywhere. */
static void build_sync_chars(void) {
    s32 i;
    s32 c;
    bool used[8] = { false, false, false, false, false, false, false, false };
    for (i = 0; i < 8; i++) {
        sSyncChars[i] = 0xFF;
    }
    if (sNodeId >= 0 && sNodeId < 8) {
        sSyncChars[sNodeId] = (u8) gCharacterSelections[0];
        used[sSyncChars[sNodeId] & 7] = true;
    }
    for (i = 0; i < sBarrierN; i++) {
        if (sBarrierPick[i] != 0xFF && sBarrierPeer[i] < 8) {
            sSyncChars[sBarrierPeer[i]] = sBarrierPick[i];
            used[sBarrierPick[i] & 7] = true;
        }
    }
    c = 0;
    for (i = 0; i < 8; i++) {
        if (sSyncChars[i] == 0xFF) {
            while (c < 8 && used[c]) {
                c++;
            }
            sSyncChars[i] = (u8) ((c < 8) ? c : (i & 7));
            if (c < 8) {
                used[c] = true;
            }
        }
    }
    sSyncCharsValid = true;
}

/* Spawn hook: hand the agreed character table to spawn_players_gp_one_player.
 * Returns true (and fills the game's tables) only for an online race with an
 * agreed table — the caller then skips its RNG-consuming random roster, which
 * would otherwise consume the shared sim RNG differently per console. */
bool net_menu_take_synced_chars(s8* humanChar, s16* cpuChars) {
    s32 i;
    if (!sOnlineActive || !sSyncCharsValid) {
        return false;
    }
    *humanChar = (s8) sSyncChars[0];
    for (i = 0; i < 7; i++) {
        cpuChars[i] = (s16) sSyncChars[i + 1];
    }
    return true;
}

/* Record the start-barrier role at race start (see the barrier block above). */
static void net_online_barrier_arm_host(void) {
    s32 i;
    s32 n = netpak_peers(sPeers, 8); /* fresh roster so we wait for the right set */
    if (n >= 0) {
        sPeerCount = n;
    }
    sIsHost = true;
    sSyncCharsValid = false;
    for (i = 0; i < 8; i++) {
        sBarrierPick[i] = 0xFF;
    }
    sBarrierN = (sPeerCount < 8) ? sPeerCount : 8;
    for (i = 0; i < sBarrierN; i++) {
        sBarrierPeer[i] = sPeers[i].node_id;
        sBarrierReady[i] = false;
    }
    sBarrierTick = 0;
    sBarrierWaiting = false;
    sBarrierWaitTicks = 0;
}
static void net_online_barrier_arm_joiner(u8 hostNode) {
    sIsHost = false;
    sHostNode = hostNode;
    sBarrierTick = 0;
    sBarrierWaiting = false;
    sBarrierWaitTicks = 0;
}

/* Called every frame while a player holds at the map screen after picking a
 * driver. Runs the ready/go handshake and returns true once it's time to launch
 * (host: all peers ready; joiner: GO received). Sets sBarrierWaiting for the
 * "waiting for all players" popup. */
bool net_online_barrier_ready(void) {
    static netpak_pkt_t pkt; /* 1 KB — keep off the stack */
    s32 i;

    /* Drain ch1: host collects READY, joiner watches for GO. */
    while (netpak_recv(&pkt) == 0) {
        if (pkt.ch == 1 && pkt.len >= (u16)sizeof(OnlineMsg) && pkt.data[0] == OLMSG_TAG) {
            u8 mtype = pkt.data[1];
            net_menu_check_ver(pkt.data[3]);
            if (sIsHost && mtype == OLMSG_READY) {
                for (i = 0; i < sBarrierN; i++) {
                    if (sBarrierPeer[i] == pkt.src) {
                        sBarrierReady[i] = true;
                        if (pkt.len >= 12) {
                            sBarrierPick[i] = pkt.data[4]; /* chars[0] = their pick */
                        }
                    }
                }
            } else if (!sIsHost && mtype == OLMSG_GO) {
                if (pkt.len >= 12) { /* adopt the host's character table */
                    for (i = 0; i < 8; i++) {
                        sSyncChars[i] = pkt.data[4 + i];
                    }
                    sSyncCharsValid = true;
                }
                sBarrierWaiting = false;
                return true; /* joiner launches on the host's GO */
            }
        }
    }

    sBarrierWaiting = true;

    /* Peer/host loss at the barrier: fail FORWARD. A departed player must not
     * freeze the start popup forever — the in-race drop arbitration (proven for
     * both players and the host) turns absentees into CPU bots once racing. */
    sBarrierWaitTicks++;
    if ((sBarrierWaitTicks % 30) == 0) { /* re-check the room roster ~2x/sec */
        netpak_peer_t roster[8];
        s32 nr = netpak_peers(roster, 8);
        if (nr >= 0) {
            if (sIsHost) {
                /* any barrier peer no longer in the room counts as ready */
                for (i = 0; i < sBarrierN; i++) {
                    s32 j;
                    bool present = false;
                    for (j = 0; j < nr; j++) {
                        if (roster[j].node_id == sBarrierPeer[i]) {
                            present = true;
                        }
                    }
                    if (!present && !sBarrierReady[i]) {
                        sBarrierReady[i] = true;
                        netpak_debug_poke(0xF3000000u | sBarrierPeer[i]); /* peer left at barrier */
                    }
                }
                /* LATECOMERS: with explicit join (v4+), a player can confirm the
                 * room code after the host pressed START and miss the one-shot
                 * OLMSG_START. Adopt anyone now in the room into the barrier set
                 * and re-send START (with the course) so they catch up — the
                 * whole room still launches together. Peers already past the
                 * lobby drain and ignore the duplicate START. */
                for (i = 0; i < nr && sBarrierN < 8; i++) {
                    s32 j;
                    bool known = false;
                    for (j = 0; j < sBarrierN; j++) {
                        if (sBarrierPeer[j] == roster[i].node_id) {
                            known = true;
                        }
                    }
                    if (!known) {
                        sBarrierPeer[sBarrierN] = roster[i].node_id;
                        sBarrierReady[sBarrierN] = false;
                        sBarrierN++;
                        netpak_debug_poke(0xF4000000u | roster[i].node_id); /* latecomer adopted */
                    }
                }
                {
                    OnlineMsg m;
                    s32 ci2;
                    m.tag = OLMSG_TAG;
                    m.type = OLMSG_START;
                    m.course = sOnlineCourse;
                    m.pad = (u8) NETPAK_ROM_VERSION; /* version cross-check */
                    for (ci2 = 0; ci2 < 8; ci2++) {
                        m.chars[ci2] = 0;
                    }
                    m.chars[0] = (u8) sOnlineCc; /* class rides with every START */
                    m.chars[1] = (u8) sOnlineDelay; /* same pick as the original
                                                     * START — latecomers must
                                                     * adopt the identical delay */
                    for (i = 0; i < nr; i++) {
                        netpak_send(roster[i].node_id, 1, &m, sizeof(m));
                    }
                }
            } else {
                bool hostPresent = false;
                for (i = 0; i < nr; i++) {
                    if (roster[i].node_id == sHostNode) {
                        hostPresent = true;
                    }
                }
                if (!hostPresent) {
                    netpak_debug_poke(0xF2000000u | sHostNode); /* host left at barrier */
                    sBarrierWaiting = false;
                    return true; /* launch; the host becomes a bot in-race */
                }
            }
        }
    }
    if (sBarrierWaitTicks > BARRIER_TIMEOUT_TICKS) {
        netpak_debug_poke(0xF2000000u | 0xFFu); /* barrier timeout */
        build_sync_chars(); /* best-effort table from the picks we know */
        sBarrierWaiting = false;
        return true; /* launch with whoever is coming; drops handle the rest */
    }

    if (sIsHost) {
        bool all = true;
        for (i = 0; i < sBarrierN; i++) {
            if (!sBarrierReady[i]) {
                all = false;
            }
        }
        if (all) { /* every joiner reported ready — release the whole room */
            OnlineMsg m;
            s32 k;
            build_sync_chars(); /* freeze the character table for this race */
            m.tag = OLMSG_TAG;
            m.type = OLMSG_GO;
            m.course = 0;
            m.pad = (u8) NETPAK_ROM_VERSION; /* version cross-check */
            for (k = 0; k < 8; k++) {
                m.chars[k] = sSyncChars[k];
            }
            online_msg_send_all(&m);
            sBarrierWaiting = false;
            return true;
        }
    } else if ((sBarrierTick++ % 12) == 0) { /* joiner: keep telling the host we're ready */
        OnlineMsg m;
        s32 k;
        m.tag = OLMSG_TAG;
        m.type = OLMSG_READY;
        m.course = 0;
        m.pad = (u8) NETPAK_ROM_VERSION; /* version cross-check */
        for (k = 0; k < 8; k++) {
            m.chars[k] = 0xFF;
        }
        m.chars[0] = (u8) gCharacterSelections[0]; /* my pick, for the host's table */
        netpak_send(sHostNode, 1, &m, sizeof(m));
    }
    return false;
}

/* Popup drawn over the map screen while waiting at the start barrier. A dark
 * fill panel sits behind the text so it's readable over the busy map preview. */
static bool sCourseWait; /* joiner parked at the course screen, host picking */

void net_menu_set_coursewait(bool on) {
    sCourseWait = on;
}

/* Popup over the course-select screen while the HOST browses: joiners are
 * input-locked there (v32), so say why. Same styling as the start barrier. */
void net_online_coursewait_render(void) {
    if (!sCourseWait) {
        return;
    }
    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    gDPSetFillColor(gDisplayListHead++,
                    (GPACK_RGBA5551(0, 0, 0, 1) << 16) | GPACK_RGBA5551(0, 0, 0, 1));
    gDPFillRectangle(gDisplayListHead++, 66, 90, 254, 168);
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);

    set_text_color(TEXT_GREEN);
    print_text1_center_mode_1(0xA0, 0x66, "WAITING FOR", 0, 1.0f, 1.0f);
    print_text1_center_mode_1(0xA0, 0x7C, "HOST", 0, 1.0f, 1.0f);
    set_text_color(TEXT_YELLOW);
    print_text1_center_mode_1(0xA0, 0x9A, "CHOOSING COURSE", 0, 0.75f, 0.8f);
}

void net_online_barrier_render(void) {
    if (!sBarrierWaiting) {
        return;
    }

    /* Black panel (FILL cycle), then restore 1CYCLE for the text that follows. */
    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    gDPSetFillColor(gDisplayListHead++,
                    (GPACK_RGBA5551(0, 0, 0, 1) << 16) | GPACK_RGBA5551(0, 0, 0, 1));
    gDPFillRectangle(gDisplayListHead++, 66, 90, 254, 168);
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);

    set_text_color(TEXT_GREEN);
    print_text1_center_mode_1(0xA0, 0x66, "WAITING FOR", 0, 1.0f, 1.0f);
    print_text1_center_mode_1(0xA0, 0x7C, "ALL PLAYERS", 0, 1.0f, 1.0f);
    set_text_color(TEXT_YELLOW);
    print_text1_center_mode_1(0xA0, 0x9A, "TO START", 0, 0.9f, 0.9f);
    if (sVerMismatch != 0) {
        set_text_color(TEXT_RED);
        print_text1_center_mode_1(0xA0, 0xA8, "VERSION MISMATCH", 0, 0.6f, 0.6f);
    }
}

/* Name of the ONLINE player in kart slot `slot`, or NULL when offline / not
 * a human seat (CPU fill). slot == node id in lockstep races. Used by the
 * post-race standings so rows read JSUPPE / ALICE instead of MARIO / LUIGI. */
const char* net_menu_slot_name(s32 slot) {
    s32 i;
    if (!net_menu_online_active() || slot < 0 || slot >= net_menu_player_count()) {
        return NULL;
    }
    if (slot == sNodeId) {
        return (sName[0] != '\0' && sName[0] != ' ') ? sName : NULL;
    }
    for (i = 0; i < sPeerCount; i++) {
        if (sPeers[i].node_id == (u8) slot && sPeers[i].name[0] != '\0') {
            return sPeers[i].name;
        }
    }
    return NULL;
}

bool net_menu_is_host(void) {
    return sIsHost;
}

/* Host: broadcast the course chosen on the course-select screen (repeat-safe;
 * joiners latch the first one). */
void net_menu_send_course(s32 courseId) {
    OnlineMsg m;
    s32 ci;
    sOnlineCourse = (u8) courseId;
    m.tag = OLMSG_TAG;
    m.type = OLMSG_COURSE;
    m.course = (u8) courseId;
    m.pad = (u8) NETPAK_ROM_VERSION;
    for (ci = 0; ci < 8; ci++) {
        m.chars[ci] = 0;
    }
    m.chars[0] = (u8) sOnlineCc;
    online_msg_send_all(&m);
}

/* Joiner waiting at the course screen: poll ch1 for the host's course pick.
 * Returns true once known (sOnlineCourse valid). */
bool net_menu_poll_course(void) {
    static netpak_pkt_t pkt;
    if (sOnlineCourse != 0xFF) {
        return true;
    }
    while (netpak_recv(&pkt) == 0) {
        if (pkt.ch == 1 && pkt.len >= (u16) sizeof(OnlineMsg) && pkt.data[0] == OLMSG_TAG &&
            pkt.data[1] == OLMSG_COURSE) {
            net_menu_check_ver(pkt.data[3]);
            sOnlineCourse = pkt.data[2];
            return true;
        }
    }
    return false;
}

/* --- Lobby ping (v33) --------------------------------------------------
 * True player-to-player round trip (via the relay) measured in the lobby:
 * PING (0x4D) carries our microsecond clock; the peer echoes PONG (0x4E)
 * with the timestamp untouched; delta/1000 = ms, smoothed 50/50. Lobby
 * states only — never during lockstep. */
#define NETPING_TAG 0x4D
#define NETPONG_TAG 0x4E
static s16 sPingMs[8];   /* -1 = no measurement yet */
static u32 sPingTick;
static u8  sLobbyCc = 0xFF; /* class as learned from the HOST's pings (joiners) */

/* Host-side: smallest input delay the slowest link can hide. One-way latency
 * ~ worstPing/2, +10ms jitter allowance, ceil'd into 17ms frames, +1 frame of
 * pipeline slack; floor 2 = the v39 fixed delay (loopback/LAN stays at 2). */
static s32 net_menu_pick_delay(void) {
    s32 i;
    s32 worst = 0;
    s32 d;
    for (i = 0; i < 8; i++) {
        if (sPingMs[i] > worst) {
            worst = (s32) sPingMs[i];
        }
    }
    d = 1 + (worst / 2 + 10 + 16) / 17;
    return (d < 2) ? 2 : d; /* upper clamp to 8 happens at adopt */
}

static u32 ping_now_us(void) {
    extern u32 net_time_us(void);
    return net_time_us();
}

/* Single lobby packet drain: sends pings, answers pings, records pongs, and
 * REPORTS (not eats!) a lobby START. Returns true when a START arrived; the
 * message is copied into *startOut and startSrc. Everything else in the
 * lobby is stale race-era noise and is dropped. */
static bool net_menu_lobby_drain(OnlineMsg* startOut, u8* startSrc) {
    static netpak_pkt_t pkt;
    s32 i;
    bool gotStart = false;
    if ((sPingTick++ % 20) == 0) { /* ~3x/sec per peer */
        for (i = 0; i < sPeerCount; i++) {
            u32 m[2];
            if (sPeers[i].node_id == (u8) sNodeId) {
                continue;
            }
            m[0] = ((u32) NETPING_TAG << 24) | (((u32) sCcSel & 0xFFu) << 8) |
                   (u32) (sNodeId & 0xFF);
            m[1] = ping_now_us();
            netpak_send(sPeers[i].node_id, 0, m, sizeof(m));
            netpak_debug_poke(0xFB000000u | sPeers[i].node_id); /* ping sent */
        }
    }
    while (netpak_recv(&pkt) == 0) {
        if (pkt.ch == 0 && pkt.len >= 8 && pkt.data[0] == NETPING_TAG) {
            u32 r[2]; /* echo, timestamp untouched, straight back */
            r[0] = ((u32) NETPONG_TAG << 24) | (u32) (sNodeId & 0xFF);
            r[1] = ((u32*) pkt.data)[1];
            netpak_send(pkt.src, 0, r, sizeof(r));
            netpak_debug_poke(0xFC000000u | pkt.src); /* ping echoed */
            if (pkt.src == 0) { /* the host's ping tells us the class */
                sLobbyCc = pkt.data[2]; /* byte 1 of the BE word = bits 8-15 */
            }
        } else if (pkt.ch == 0 && pkt.len >= 8 && pkt.data[0] == NETPONG_TAG) {
            u32 dt = ping_now_us() - ((u32*) pkt.data)[1];
            s32 ms = (s32) (dt / 1000u);
            s32 slot = pkt.src & 7;
            if (ms > 999) {
                ms = 999;
            }
            sPingMs[slot] = (sPingMs[slot] < 0) ? (s16) ms
                          : (s16) ((sPingMs[slot] + ms) / 2);
            netpak_debug_poke(0xFD000000u | ((u32) slot << 16) | ((u32) ms & 0xFFFFu)); /* measured */
        } else if (pkt.ch == 1 && pkt.len >= (u16) sizeof(OnlineMsg) &&
                   pkt.data[0] == OLMSG_TAG && pkt.data[1] == OLMSG_START && startOut != NULL) {
            memcpy(startOut, pkt.data, sizeof(OnlineMsg));
            *startSrc = pkt.src;
            gotStart = true;
        }
    }
    return gotStart;
}

void net_menu_reset(void) {
    s32 i;
    sState = OM_MAIN;
    sVerMismatch = 0; /* fresh session, fresh cross-check */
    { s32 pi_; for (pi_ = 0; pi_ < 8; pi_++) { sPingMs[pi_] = -1; } }
    sLobbyCc = 0xFF;
    /* class picked on the game-select sub-menu (v27) seeds the lobby; the
     * lobby U/D still allows changing it before START */
    sCcSel = (gCCSelection >= CC_50 && gCCSelection <= CC_150) ? gCCSelection : CC_100;
    sSel = 0;
    sEntryPos = 0;
    sNodeId = -1;
    sLastErr = 0;
    sPeerCount = 0;
    sPeerPoll = 0;
    sCourseSel = 0;
    sOnlineArmed = false;
    sOnlineActive = false;
    for (i = 0; i < CODE_LEN; i++) {
        sCode[i] = kCodeAlphabet[0];
    }
    sCode[CODE_LEN] = '\0';
    sNamePos = 0;
    name_load();

    /* Pre-fill from the launch room code (ares NP64_ROOM), if any. */
    {
        char preset[8];
        netpak_get_room_code(preset);
        sHavePreset = (preset[0] != '\0');
        if (sHavePreset) {
            for (i = 0; i < CODE_LEN && preset[i] != '\0'; i++) {
                sCode[i] = preset[i];
            }
            sCode[CODE_LEN] = '\0';
        }
    }
}

/* index of char c in the alphabet, or 0 if not found */
static s32 code_index(char c) {
    s32 i;
    for (i = 0; i < (s32)(sizeof(kCodeAlphabet) - 1); i++) {
        if (kCodeAlphabet[i] == c) {
            return i;
        }
    }
    return 0;
}

static void code_cycle(s32 pos, s32 delta) {
    s32 n = (s32)(sizeof(kCodeAlphabet) - 1);
    s32 i = (code_index(sCode[pos]) + delta + n) % n;
    sCode[pos] = kCodeAlphabet[i];
}

/* index of char c in the name alphabet, or 0 (blank) if not found */
static s32 name_index(char c) {
    s32 i;
    for (i = 0; i < (s32)(sizeof(kNameAlphabet) - 1); i++) {
        if (kNameAlphabet[i] == c) {
            return i;
        }
    }
    return 0;
}

static void name_cycle(s32 pos, s32 delta) {
    s32 n = (s32)(sizeof(kNameAlphabet) - 1);
    s32 i = (name_index(sName[pos]) + delta + n) % n;
    sName[pos] = kNameAlphabet[i];
}

/* Pull the current name from the device into the edit buffer, normalized to
 * the editable alphabet (lowercase folded to uppercase, anything else blank)
 * and space-padded to NAME_LEN. */
static void name_load(void) {
    char raw[16];
    s32 i;
    netpak_get_name(raw);
    for (i = 0; i < NAME_LEN; i++) {
        char c = raw[i];
        if (c >= 'a' && c <= 'z') {
            c = (char) (c - 'a' + 'A');
        }
        if (c == '\0' || name_index(c) == 0) {
            c = ' ';
        }
        sName[i] = c;
    }
    sName[NAME_LEN] = '\0';
}

/* Commit the edit buffer: trim the space padding and push the name to the
 * device (relay + room see it immediately; the emulator persists it).
 * Returns false for an all-blank name (nothing sent). */
static bool name_save(void) {
    char out[16];
    s32 i;
    s32 end = NAME_LEN;
    while (end > 0 && sName[end - 1] == ' ') {
        end--;
    }
    if (end == 0) {
        return false;
    }
    for (i = 0; i < end; i++) {
        out[i] = sName[i];
    }
    out[end] = '\0';
    return netpak_set_name(out) == 0;
}

void net_menu_update(struct Controller* controller) {
    u16 btn = controller->buttonPressed | controller->stickPressed;

    if (is_screen_being_faded()) {
        return;
    }

    switch (sState) {
        case OM_MAIN:
            if (btn & U_JPAD) {
                sSel = (sSel + OM_MAIN_OPTIONS - 1) % OM_MAIN_OPTIONS;
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & D_JPAD) {
                sSel = (sSel + 1) % OM_MAIN_OPTIONS;
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & B_BUTTON) {
                func_online_fade(); /* leave the online screen -> main menu */
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (btn & A_BUTTON) {
                if (sSel == 2) { /* NAME: edit the player name */
                    sNamePos = 0;
                    name_load();
                    sState = OM_NAME_ENTRY;
                    play_sound2(SOUND_MENU_SELECT);
                } else if (sSel == 0) { /* HOST: open a room (needs the relay up) */
                    if (netpak_present() && (netpak_status() & NETPAK_STATUS_LINK_UP)) {
                        sLastErr = 0;
                        /* With a launch code, join-or-create that exact room so
                         * the joiner can use the same code; else create a fresh
                         * random room and show its code. */
                        sNodeId = sHavePreset ? netpak_session_join(sCode)
                                              : netpak_session_create(sCode);
                        if (sNodeId >= 0) {
                            sState = OM_HOSTING;
                            play_sound2(SOUND_MENU_OK_CLICKED);
                        } else {
                            sLastErr = -sNodeId;
                            play_sound2(SOUND_MENU_GO_BACK);
                        }
                    }
                } else { /* JOIN: always dial the code in (pre-filled from the
                          * launch code when one was given) — joining a room is
                          * an explicit act, never automatic. */
                    sEntryPos = 0;
                    sState = OM_JOIN_ENTRY;
                    play_sound2(SOUND_MENU_SELECT);
                }
            }
            break;

        case OM_HOSTING: {
            s32 n;
            net_menu_lobby_drain(NULL, NULL);
            if ((sPeerPoll++ % 15) == 0) { /* refresh the roster ~2x/sec */
                n = netpak_peers(sPeers, 8);
                if (n >= 0) {
                    sPeerCount = n;
                }
            }
            if (btn & B_BUTTON) {
                netpak_session_leave();
                sState = OM_MAIN;
                sPeerCount = 0;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (btn & START_BUTTON) {
                OnlineMsg m;
                s32 ci;
                net_online_barrier_arm_host(); /* refresh roster, wait for these peers */
                sOnlineCourse = 0xFF; /* v32: course picked on the REAL course-select screen */
                sOnlineCc = sCcSel;
                m.tag = OLMSG_TAG;
                m.type = OLMSG_START;
                m.course = sOnlineCourse;
                m.pad = (u8) NETPAK_ROM_VERSION; /* version cross-check */
                for (ci = 0; ci < 8; ci++) {
                    m.chars[ci] = 0;
                }
                m.chars[0] = (u8) sOnlineCc; /* engine class rides with the course */
                sOnlineDelay = net_menu_pick_delay(); /* size once, from lobby RTTs */
                m.chars[1] = (u8) sOnlineDelay; /* every console adopts this (v40) */
                online_msg_send_all(&m);   /* tell the room which track to load */
                net_menu_start_race();     /* -> character select -> locked course -> race */
            }
            break;
        }

        case OM_JOINED: {
            static OnlineMsg startMsg;
            u8 startSrc = 0;
            bool gotStart;
            s32 n;
            gotStart = net_menu_lobby_drain(&startMsg, &startSrc);
            if ((sPeerPoll++ % 15) == 0) { /* refresh the roster ~2x/sec */
                n = netpak_peers(sPeers, 8);
                if (n >= 0) {
                    sPeerCount = n;
                    /* Host-loss in the lobby (before START): the room creator is
                     * node 0. No course has been chosen yet, so there is nothing
                     * to launch — fall BACK to the online menu. */
                    if (sNodeId != 0) {
                        bool hostPresent = false;
                        s32 hi;
                        for (hi = 0; hi < n; hi++) {
                            if (sPeers[hi].node_id == 0) {
                                hostPresent = true;
                            }
                        }
                        if (!hostPresent) {
                            sHostGonePolls++;
                        } else {
                            sHostGonePolls = 0;
                        }
                        if (sHostGonePolls >= LOBBY_HOST_GONE_POLLS) {
                            netpak_debug_poke(0xF1000000u); /* host left the lobby */
                            sHostGonePolls = 0;
                            netpak_session_leave();
                            sPeerCount = 0;
                            sState = OM_MAIN;
                            play_sound2(SOUND_MENU_GO_BACK);
                            break;
                        }
                    }
                }
            }
            /* The host's START arrives via the shared drain above. */
            if (gotStart) {
                net_menu_check_ver(startMsg.pad);
                sOnlineCourse = startMsg.course; /* 0xFF = host picks on the course screen */
                sOnlineCc = (startMsg.chars[0] <= CC_150) ? startMsg.chars[0] : CC_100;
                /* adopt the host's input-delay pick; old hosts send 0 -> floor 2 */
                sOnlineDelay = (startMsg.chars[1] >= 2 && startMsg.chars[1] <= 8)
                             ? (s32) startMsg.chars[1] : 2;
                net_online_barrier_arm_joiner(startSrc); /* host node id from START */
                net_menu_start_race(); /* same track as the host */
                return;
            }
#if NET_MENU_JOINED_CAN_START
            /* Test/repro only: the room creator (node 0) self-starts and hosts
             * the barrier; other nodes fall through and start on its START above.
             * Lets two fixed-room instances run a full synced start headlessly. */
            if ((btn & START_BUTTON) && sNodeId == 0) {
                OnlineMsg m;
                s32 ci;
                net_online_barrier_arm_host();
                sOnlineCourse = kCourses[sCourseSel].id;
                sOnlineCc = sCcSel;
                m.tag = OLMSG_TAG;
                m.type = OLMSG_START;
                m.course = sOnlineCourse;
                m.pad = (u8) NETPAK_ROM_VERSION; /* version cross-check */
                for (ci = 0; ci < 8; ci++) {
                    m.chars[ci] = 0; /* was UNINITIALIZED: receivers read
                                      * chars[0] as the engine class since the
                                      * CC feature -> random per-boot garbage */
                }
                m.chars[0] = (u8) sOnlineCc;
                sOnlineDelay = net_menu_pick_delay();
                m.chars[1] = (u8) sOnlineDelay;
                online_msg_send_all(&m);
                net_menu_start_race();
                return;
            }
#endif
            if (btn & B_BUTTON) {
                netpak_session_leave();
                sState = OM_MAIN;
                sPeerCount = 0;
                play_sound2(SOUND_MENU_GO_BACK);
            }
            break;
        }

        case OM_JOIN_ENTRY:
            if (btn & U_JPAD) {
                code_cycle(sEntryPos, +1);
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & D_JPAD) {
                code_cycle(sEntryPos, -1);
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & R_JPAD) {
                if (sEntryPos < CODE_LEN - 1) {
                    sEntryPos++;
                    play_sound2(SOUND_MENU_CURSOR_MOVE);
                }
            }
            if (btn & L_JPAD) {
                if (sEntryPos > 0) {
                    sEntryPos--;
                    play_sound2(SOUND_MENU_CURSOR_MOVE);
                }
            }
            if (btn & B_BUTTON) {
                sState = OM_MAIN;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (btn & A_BUTTON) { /* confirm the code -> join */
                if (netpak_present() && (netpak_status() & NETPAK_STATUS_LINK_UP)) {
                    sLastErr = 0;
                    sNodeId = netpak_session_join(sCode);
                    if (sNodeId >= 0) {
                        /* Join-or-create: if the room didn't exist, the relay
                         * created it and we're node 0 — the de-facto host. Give
                         * that player the HOST lobby (course picker + START)
                         * instead of parking them at "WAITING FOR HOST" forever
                         * (two players who both picked JOIN with a shared launch
                         * code used to deadlock in guest lobbies). */
                        sState = (sNodeId == 0) ? OM_HOSTING : OM_JOINED;
                        play_sound2(SOUND_MENU_OK_CLICKED);
                    } else {
                        sLastErr = -sNodeId;
                        play_sound2(SOUND_MENU_GO_BACK);
                    }
                }
            }
            break;

        case OM_NAME_ENTRY:
            if (btn & U_JPAD) {
                name_cycle(sNamePos, +1);
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & D_JPAD) {
                name_cycle(sNamePos, -1);
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & R_JPAD) {
                if (sNamePos < NAME_LEN - 1) {
                    sNamePos++;
                    play_sound2(SOUND_MENU_CURSOR_MOVE);
                }
            }
            if (btn & L_JPAD) {
                if (sNamePos > 0) {
                    sNamePos--;
                    play_sound2(SOUND_MENU_CURSOR_MOVE);
                }
            }
            if (btn & B_BUTTON) { /* cancel: discard edits */
                name_load();
                sState = OM_MAIN;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (btn & A_BUTTON) { /* confirm -> SET_IDENTITY */
                if (name_save()) {
                    name_load(); /* re-read: the device copy is the truth */
                    sState = OM_MAIN;
                    play_sound2(SOUND_MENU_OK_CLICKED);
                } else {
                    play_sound2(SOUND_MENU_GO_BACK); /* all blank / cmd failed */
                }
            }
            break;
    }
}

/* Title-screen build stamp, under the copyright line: makes a stale ROM obvious
 * at a glance. Rendered from func_80094A64's START_MENU case each frame. */
void net_menu_render_version(void) {
    char text[24];
    s32 n = 0;
    s32 v = NETPAK_ROM_VERSION;
    const char* label = "JSUPPE VERSION ";
    while (label[n] != '\0') {
        text[n] = label[n];
        n++;
    }
    if (v >= 100) {
        text[n++] = (char) ('0' + (v / 100) % 10);
    }
    if (v >= 10) {
        text[n++] = (char) ('0' + (v / 10) % 10);
    }
    text[n++] = (char) ('0' + v % 10);
    text[n] = '\0';
    set_text_color(TEXT_YELLOW);
    print_text1_center_mode_1(0xA0, 0xE4, text, 0, 0.6f, 0.6f);
}

/* draw one option, highlighted (cycling color) if selected, else plain yellow */
static void draw_option(s32 x, s32 y, char* text, bool selected) {
    set_text_color(selected ? TEXT_BLUE_GREEN_RED_CYCLE_1 : TEXT_YELLOW);
    print_text1_center_mode_1(x, y, text, 0, 0.9f, 0.9f);
}

/* Relay status as a compact signal-bars icon (top-right, all online screens).
 * The old centered text line crowded the screen and overlapped the lobby
 * roster. Three ascending bars, filled bottom-up with connection progress:
 *   red blinking short bar          = connecting to relay (driver retries forever)
 *   two green bars  + grey stub     = relay link up, not in a room
 *   three green bars                = link up + seated in a room
 * Unlit bars render dark grey so the glyph always reads as a signal meter. */
static void net_menu_relay_icon(void) {
    static u32 tick;
    u32 st = netpak_status();
    s32 lit;      /* how many bars are on */
    s32 i;
    u32 on, off;
    tick++;
    if (!(st & NETPAK_STATUS_LINK_UP)) {
        if (tick & 16) {
            return; /* blink while connecting */
        }
        lit = 1;
        on = (GPACK_RGBA5551(255, 40, 40, 1) << 16) | GPACK_RGBA5551(255, 40, 40, 1);
    } else {
        lit = (st & NETPAK_STATUS_SESSION) ? 3 : 2;
        on = (GPACK_RGBA5551(40, 220, 40, 1) << 16) | GPACK_RGBA5551(40, 220, 40, 1);
    }
    off = (GPACK_RGBA5551(80, 80, 80, 1) << 16) | GPACK_RGBA5551(80, 80, 80, 1);

    gDPPipeSync(gDisplayListHead++);
    gDPSetRenderMode(gDisplayListHead++, G_RM_OPA_SURF, G_RM_OPA_SURF2);
    gDPSetCycleType(gDisplayListHead++, G_CYC_FILL);
    for (i = 0; i < 3; i++) {
        s32 x = 288 + i * 6;            /* 4px bars, 2px gaps */
        s32 h = 4 + i * 3;              /* heights 4/7/10, shared baseline y=26 */
        gDPSetFillColor(gDisplayListHead++, (i < lit) ? on : off);
        gDPFillRectangle(gDisplayListHead++, x, 26 - h, x + 4, 26);
    }
    gDPPipeSync(gDisplayListHead++);
    gDPSetCycleType(gDisplayListHead++, G_CYC_1CYCLE);
}

void net_menu_render(void) {
    set_text_color(TEXT_BLUE_GREEN_RED_CYCLE_1);
    print_text1_center_mode_1(0xA0, 0x30, "ONLINE", 0, 1.2f, 1.2f);
    net_menu_relay_icon();
    set_text_color(TEXT_YELLOW);

    if (!netpak_present()) {
        set_text_color(TEXT_RED);
        print_text1_center_mode_1(0xA0, 0x78, "NO NETPAK DEVICE", 0, 0.8f, 0.8f);
        set_text_color(TEXT_YELLOW);
        print_text1_center_mode_1(0xA0, 0xC4, "B  BACK", 0, 0.7f, 0.7f);
        return;
    }

    switch (sState) {
        case OM_MAIN: {
            char nameRow[24];
            s32 i;
            s32 n = 0;
            const char* label = "NAME  ";
            while (label[n] != '\0') {
                nameRow[n] = label[n];
                n++;
            }
            {
                s32 end = NAME_LEN;
                while (end > 0 && sName[end - 1] == ' ') {
                    end--;
                }
                for (i = 0; i < end; i++) {
                    nameRow[n++] = sName[i];
                }
                if (end == 0) { /* nothing set yet: show a placeholder */
                    nameRow[n++] = '-';
                }
            }
            nameRow[n] = '\0';

            draw_option(0xA0, 0x68, "HOST GAME", sSel == 0);
            draw_option(0xA0, 0x80, "JOIN GAME", sSel == 1);
            draw_option(0xA0, 0x98, nameRow, sSel == 2);
            if (sLastErr) {
                set_text_color(TEXT_RED);
                print_text1_center_mode_1(0xA0, 0xB0, "CONNECTION FAILED", 0, 0.7f, 0.7f);
            }
            set_text_color(TEXT_YELLOW);
            print_text1_center_mode_1(0xA0, 0xC4, "B  BACK", 0, 0.7f, 0.7f);
            break;
        }

        case OM_HOSTING:
        case OM_JOINED: {
            s32 i;
            s32 y;
            set_text_color(TEXT_GREEN);
            if (sVerMismatch != 0) {
                set_text_color(TEXT_RED);
                print_text1_center_mode_1(0xA0, 0x3E, "VERSION MISMATCH  UPDATE ROMS", 0, 0.55f, 0.55f);
            }
            print_text1_center_mode_1(0xA0, 0x48, "ROOM CODE", 0, 0.7f, 0.7f);
            set_text_color(TEXT_BLUE_GREEN_RED_CYCLE_1);
            print_text1_center_mode_1(0xA0, 0x5C, sCode, 4, 1.1f, 1.1f);

            set_text_color(TEXT_GREEN);
            print_text1_center_mode_1(0xA0, 0x78, "PLAYERS", 0, 0.7f, 0.7f);
            set_text_color(TEXT_YELLOW);
            { /* self: show the actual player name (falls back to YOU) */
                char self[NAME_LEN + 1];
                s32 end = NAME_LEN;
                while (end > 0 && sName[end - 1] == ' ') {
                    end--;
                }
                for (i = 0; i < end; i++) {
                    self[i] = sName[i];
                }
                self[end] = '\0';
                print_text1_center_mode_1(0xA0, 0x88, end ? self : "YOU", 0, 0.7f, 0.7f);
            }
            y = 0x94;
            for (i = 0; i < sPeerCount && i < 5; i++) {
                print_text1_center_mode_1(0xA0, y, sPeers[i].name, 0, 0.7f, 0.7f);
                if (sPeers[i].node_id != (u8) sNodeId && sPingMs[sPeers[i].node_id & 7] >= 0) {
                    char pingTxt[8];
                    s32 pv = sPingMs[sPeers[i].node_id & 7];
                    s32 pn = 0;
                    if (pv >= 100) { pingTxt[pn++] = (char) ('0' + pv / 100); }
                    if (pv >= 10) { pingTxt[pn++] = (char) ('0' + (pv / 10) % 10); }
                    pingTxt[pn++] = (char) ('0' + pv % 10);
                    pingTxt[pn++] = 'M';
                    pingTxt[pn++] = 'S';
                    pingTxt[pn] = '\0';
                    set_text_color(pv < 60 ? TEXT_GREEN : (pv < 120 ? TEXT_YELLOW : TEXT_RED));
                    print_text_mode_1(0xE4, y, pingTxt, 0, 0.5f, 0.5f);
                    set_text_color(TEXT_YELLOW);
                }
                y += 0xC;
            }

            if (sState == OM_HOSTING) { /* host chooses the track for the room */
                set_text_color(TEXT_GREEN);
                set_text_color(TEXT_BLUE);
                print_text1_center_mode_1(0xA0, 0xB6,
                    sCcSel == CC_50 ? "50CC" : (sCcSel == CC_150 ? "150CC" : "100CC"), 0, 0.7f, 0.7f);
                set_text_color(TEXT_YELLOW);
                print_text1_center_mode_1(0xA0, 0xC8, "START  BEGIN", 0, 0.7f, 0.7f);
                print_text1_center_mode_1(0xA0, 0xD8, "COURSE IS PICKED ON THE NEXT SCREEN", 0, 0.45f, 0.5f);
            } else { /* joiner waits for the host to pick + start */
                set_text_color(TEXT_YELLOW);
                if (sLobbyCc <= CC_150) {
                    set_text_color(TEXT_BLUE);
                    print_text1_center_mode_1(0xA0, 0xB0,
                        sLobbyCc == CC_50 ? "50CC" : (sLobbyCc == CC_150 ? "150CC" : "100CC"), 0, 0.7f, 0.7f);
                    set_text_color(TEXT_YELLOW);
                }
                print_text1_center_mode_1(0xA0, 0xC0, "WAITING FOR HOST", 0, 0.7f, 0.7f);
                print_text1_center_mode_1(0xA0, 0xD4, "B  CANCEL", 0, 0.6f, 0.6f);
            }
            break;
        }

        case OM_JOIN_ENTRY: {
            s32 i;
            char ch[2];
            ch[1] = '\0';
            set_text_color(TEXT_GREEN);
            print_text1_center_mode_1(0xA0, 0x64, "ENTER CODE", 0, 0.8f, 0.8f);
            for (i = 0; i < CODE_LEN; i++) {
                ch[0] = sCode[i];
                set_text_color(i == sEntryPos ? TEXT_BLUE_GREEN_RED_CYCLE_1 : TEXT_YELLOW);
                /* CODE_LEN chars centered at x=0xA0, 0x14 px pitch */
                print_text_mode_1(0x7C + i * 0x14, 0x82, ch, 0, 1.2f, 1.2f);
            }
            set_text_color(TEXT_YELLOW);
            print_text1_center_mode_1(0xA0, 0xAC, "STICK  SET   A  JOIN", 0, 0.6f, 0.6f);
            print_text1_center_mode_1(0xA0, 0xC4, "B  BACK", 0, 0.7f, 0.7f);
            break;
        }

        case OM_NAME_ENTRY: {
            s32 i;
            char ch[2];
            ch[1] = '\0';
            set_text_color(TEXT_GREEN);
            print_text1_center_mode_1(0xA0, 0x64, "ENTER NAME", 0, 0.8f, 0.8f);
            for (i = 0; i < NAME_LEN; i++) {
                /* blank slots stay blank ('.' is a real name character now);
                 * the cursor shows a dash caret when it sits on a blank */
                if (sName[i] == ' ') {
                    ch[0] = (i == sNamePos) ? '-' : ' ';
                } else {
                    ch[0] = sName[i];
                }
                set_text_color(i == sNamePos ? TEXT_BLUE_GREEN_RED_CYCLE_1 : TEXT_YELLOW);
                /* NAME_LEN chars centered at x=0xA0, 0x14 px pitch */
                print_text_mode_1(0x50 + i * 0x14, 0x82, ch, 0, 1.2f, 1.2f);
            }
            set_text_color(TEXT_YELLOW);
            print_text1_center_mode_1(0xA0, 0xAC, "STICK  SET   A  OK", 0, 0.6f, 0.6f);
            print_text1_center_mode_1(0xA0, 0xC4, "B  CANCEL", 0, 0.7f, 0.7f);
            break;
        }
    }
}
