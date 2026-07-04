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
extern Gfx* gDisplayListHead;    /* main.h — for the barrier popup's fill box */

/* Room-code alphabet (netpak-spec §5.1): 32 symbols, no 0/O or 1/I. */
static const char kCodeAlphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
#define CODE_LEN 6

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
typedef struct {
    u8 tag;    /* OLMSG_TAG */
    u8 type;   /* OLMSG_* */
    u8 course; /* course id to load (OLMSG_START) */
    u8 pad;
} OnlineMsg; /* 4 bytes */

enum OnlineMenuState {
    OM_MAIN,       /* HOST GAME / JOIN GAME */
    OM_HOSTING,    /* created a room; showing code, waiting for players */
    OM_JOIN_ENTRY, /* dialing in a code */
    OM_JOINED      /* joined a room; waiting for the host to start */
};

static s32  sState;
static s32  sSel;             /* OM_MAIN cursor: 0 = HOST, 1 = JOIN */
static char sCode[8];         /* room code (create result or join entry) */
static s32  sEntryPos;        /* OM_JOIN_ENTRY cursor 0..CODE_LEN-1 */
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

/* Configure a full-screen 1-local-player VERSUS race (opponents = network
 * puppets in the CPU slots) and drop into the game's real character select. */
static void net_menu_start_race(void) {
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
    gCCSelection = CC_100;
    gPlaceItemBoxes = 1;
    gIsMirrorMode = 0;
    gDemoMode = DEMO_MODE_INACTIVE; /* human drives slot 0, not AI */
    gDemoUseController = 0;
    gCurrentCourseId = sOnlineCourse;
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

/* Record the start-barrier role at race start (see the barrier block above). */
static void net_online_barrier_arm_host(void) {
    s32 i;
    s32 n = netpak_peers(sPeers, 8); /* fresh roster so we wait for the right set */
    if (n >= 0) {
        sPeerCount = n;
    }
    sIsHost = true;
    sBarrierN = (sPeerCount < 8) ? sPeerCount : 8;
    for (i = 0; i < sBarrierN; i++) {
        sBarrierPeer[i] = sPeers[i].node_id;
        sBarrierReady[i] = false;
    }
    sBarrierTick = 0;
    sBarrierWaiting = false;
}
static void net_online_barrier_arm_joiner(u8 hostNode) {
    sIsHost = false;
    sHostNode = hostNode;
    sBarrierTick = 0;
    sBarrierWaiting = false;
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
            if (sIsHost && mtype == OLMSG_READY) {
                for (i = 0; i < sBarrierN; i++) {
                    if (sBarrierPeer[i] == pkt.src) {
                        sBarrierReady[i] = true;
                    }
                }
            } else if (!sIsHost && mtype == OLMSG_GO) {
                sBarrierWaiting = false;
                return true; /* joiner launches on the host's GO */
            }
        }
    }

    sBarrierWaiting = true;

    if (sIsHost) {
        bool all = true;
        for (i = 0; i < sBarrierN; i++) {
            if (!sBarrierReady[i]) {
                all = false;
            }
        }
        if (all) { /* every joiner reported ready — release the whole room */
            OnlineMsg m;
            m.tag = OLMSG_TAG;
            m.type = OLMSG_GO;
            m.course = 0;
            m.pad = 0;
            online_msg_send_all(&m);
            sBarrierWaiting = false;
            return true;
        }
    } else if ((sBarrierTick++ % 12) == 0) { /* joiner: keep telling the host we're ready */
        OnlineMsg m;
        m.tag = OLMSG_TAG;
        m.type = OLMSG_READY;
        m.course = 0;
        m.pad = 0;
        netpak_send(sHostNode, 1, &m, sizeof(m));
    }
    return false;
}

/* Popup drawn over the map screen while waiting at the start barrier. A dark
 * fill panel sits behind the text so it's readable over the busy map preview. */
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
}

void net_menu_reset(void) {
    s32 i;
    sState = OM_MAIN;
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

void net_menu_update(struct Controller* controller) {
    u16 btn = controller->buttonPressed | controller->stickPressed;

    if (is_screen_being_faded()) {
        return;
    }

    switch (sState) {
        case OM_MAIN:
            if (btn & (U_JPAD | D_JPAD)) {
                sSel ^= 1;
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & B_BUTTON) {
                func_online_fade(); /* leave the online screen -> main menu */
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (btn & A_BUTTON) {
                if (sSel == 0) { /* HOST: open a room (needs the relay up) */
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
                } else if (sHavePreset) { /* JOIN: code known — join straight away */
                    if (netpak_present() && (netpak_status() & NETPAK_STATUS_LINK_UP)) {
                        sLastErr = 0;
                        sNodeId = netpak_session_join(sCode);
                        if (sNodeId >= 0) {
                            sState = OM_JOINED;
                            play_sound2(SOUND_MENU_OK_CLICKED);
                        } else {
                            sLastErr = -sNodeId;
                            play_sound2(SOUND_MENU_GO_BACK);
                        }
                    }
                } else { /* JOIN: no code yet — go to manual entry */
                    net_menu_reset();
                    sState = OM_JOIN_ENTRY;
                    play_sound2(SOUND_MENU_SELECT);
                }
            }
            break;

        case OM_HOSTING: {
            s32 n;
            if ((sPeerPoll++ % 15) == 0) { /* refresh the roster ~2x/sec */
                n = netpak_peers(sPeers, 8);
                if (n >= 0) {
                    sPeerCount = n;
                }
            }
            if (btn & R_JPAD) {
                sCourseSel = (sCourseSel + 1) % NUM_ONLINE_COURSES;
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & L_JPAD) {
                sCourseSel = (sCourseSel + NUM_ONLINE_COURSES - 1) % NUM_ONLINE_COURSES;
                play_sound2(SOUND_MENU_CURSOR_MOVE);
            }
            if (btn & B_BUTTON) {
                netpak_session_leave();
                sState = OM_MAIN;
                sPeerCount = 0;
                play_sound2(SOUND_MENU_GO_BACK);
            } else if (btn & START_BUTTON) {
                OnlineMsg m;
                net_online_barrier_arm_host(); /* refresh roster, wait for these peers */
                sOnlineCourse = kCourses[sCourseSel].id;
                m.tag = OLMSG_TAG;
                m.type = OLMSG_START;
                m.course = sOnlineCourse;
                m.pad = 0;
                online_msg_send_all(&m);   /* tell the room which track to load */
                net_menu_start_race();     /* -> character select -> locked course -> race */
            }
            break;
        }

        case OM_JOINED: {
            static netpak_pkt_t pkt; /* 1 KB — keep off the stack */
            s32 n;
            if ((sPeerPoll++ % 15) == 0) { /* refresh the roster ~2x/sec */
                n = netpak_peers(sPeers, 8);
                if (n >= 0) {
                    sPeerCount = n;
                }
            }
            /* Wait for the host's START: drain ch1 for a lobby control message. */
            while (netpak_recv(&pkt) == 0) {
                if (pkt.ch == 1 && pkt.len >= (u16)sizeof(OnlineMsg) &&
                    pkt.data[0] == OLMSG_TAG && pkt.data[1] == OLMSG_START) {
                    sOnlineCourse = pkt.data[2];
                    net_online_barrier_arm_joiner(pkt.src); /* host node id from START */
                    net_menu_start_race(); /* same track as the host */
                    return;
                }
            }
#if NET_MENU_JOINED_CAN_START
            /* Test/repro only: the room creator (node 0) self-starts and hosts
             * the barrier; other nodes fall through and start on its START above.
             * Lets two fixed-room instances run a full synced start headlessly. */
            if ((btn & START_BUTTON) && sNodeId == 0) {
                OnlineMsg m;
                net_online_barrier_arm_host();
                sOnlineCourse = kCourses[sCourseSel].id;
                m.tag = OLMSG_TAG;
                m.type = OLMSG_START;
                m.course = sOnlineCourse;
                m.pad = 0;
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
                        sState = OM_JOINED;
                        play_sound2(SOUND_MENU_OK_CLICKED);
                    } else {
                        sLastErr = -sNodeId;
                        play_sound2(SOUND_MENU_GO_BACK);
                    }
                }
            }
            break;
    }
}

/* draw one option, highlighted (cycling color) if selected, else plain yellow */
static void draw_option(s32 x, s32 y, char* text, bool selected) {
    set_text_color(selected ? TEXT_BLUE_GREEN_RED_CYCLE_1 : TEXT_YELLOW);
    print_text1_center_mode_1(x, y, text, 0, 0.9f, 0.9f);
}

void net_menu_render(void) {
    set_text_color(TEXT_BLUE_GREEN_RED_CYCLE_1);
    print_text1_center_mode_1(0xA0, 0x30, "ONLINE", 0, 1.2f, 1.2f);

    if (!netpak_present()) {
        set_text_color(TEXT_RED);
        print_text1_center_mode_1(0xA0, 0x78, "NO NETPAK DEVICE", 0, 0.8f, 0.8f);
        set_text_color(TEXT_YELLOW);
        print_text1_center_mode_1(0xA0, 0xC4, "B  BACK", 0, 0.7f, 0.7f);
        return;
    }

    switch (sState) {
        case OM_MAIN:
            draw_option(0xA0, 0x70, "HOST GAME", sSel == 0);
            draw_option(0xA0, 0x88, "JOIN GAME", sSel == 1);
            if (sLastErr) {
                set_text_color(TEXT_RED);
                print_text1_center_mode_1(0xA0, 0xA8, "CONNECTION FAILED", 0, 0.7f, 0.7f);
            }
            set_text_color(TEXT_YELLOW);
            print_text1_center_mode_1(0xA0, 0xC4, "B  BACK", 0, 0.7f, 0.7f);
            break;

        case OM_HOSTING:
        case OM_JOINED: {
            s32 i;
            s32 y;
            set_text_color(TEXT_GREEN);
            print_text1_center_mode_1(0xA0, 0x48, "ROOM CODE", 0, 0.7f, 0.7f);
            set_text_color(TEXT_BLUE_GREEN_RED_CYCLE_1);
            print_text1_center_mode_1(0xA0, 0x5C, sCode, 4, 1.1f, 1.1f);

            set_text_color(TEXT_GREEN);
            print_text1_center_mode_1(0xA0, 0x78, "PLAYERS", 0, 0.7f, 0.7f);
            set_text_color(TEXT_YELLOW);
            print_text1_center_mode_1(0xA0, 0x88, "YOU", 0, 0.7f, 0.7f); /* self */
            y = 0x94;
            for (i = 0; i < sPeerCount && i < 5; i++) {
                print_text1_center_mode_1(0xA0, y, sPeers[i].name, 0, 0.7f, 0.7f);
                y += 0xC;
            }

            if (sState == OM_HOSTING) { /* host chooses the track for the room */
                set_text_color(TEXT_GREEN);
                print_text1_center_mode_1(0xA0, 0xB4, "COURSE", 0, 0.7f, 0.7f);
                set_text_color(TEXT_BLUE_GREEN_RED_CYCLE_1);
                print_text1_center_mode_1(0xA0, 0xC2, (char*)kCourses[sCourseSel].name, 0, 0.8f, 0.8f);
                set_text_color(TEXT_YELLOW);
                print_text1_center_mode_1(0xA0, 0xD4, "L/R COURSE  START BEGIN", 0, 0.55f, 0.55f);
            } else { /* joiner waits for the host to pick + start */
                set_text_color(TEXT_YELLOW);
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
    }
}
