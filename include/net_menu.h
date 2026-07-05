/**
 * net_menu.h — NetPak64 online menu screen (NETWORK_VS_MENU).
 *
 * Owns the host/join/lobby state machine and its rendering, so the vanilla menu
 * files just delegate: menus.c network_vs_menu_act() -> net_menu_update(), and
 * menu_items.c func_80094A64() -> net_menu_render(). Styled like the stock menus
 * (menu font via print_text, menu fades/sounds).
 */
#ifndef NET_MENU_H
#define NET_MENU_H

#include <ultra64.h>
#include "common_structs.h"

/** Build number shown on the title screen ("JSUPPE VERSION N") so a stale ROM
 *  is obvious at a glance. BUMP THIS every time a new ROM is handed out. */
#define NETPAK_ROM_VERSION 8

/** Enter the online screen fresh (called when NETWORK_VS_MENU is set up). */
void net_menu_reset(void);

/** Per-frame input + logic (from network_vs_menu_act, player 1 only). */
void net_menu_update(struct Controller* controller);

/** Per-frame draw during the menu render pass (from func_80094A64). */
void net_menu_render(void);

/** Draw "JSUPPE VERSION N" on the title screen, under the copyright line
 * (from func_80094A64's START_MENU case). */
void net_menu_render_version(void);

/** True while an online race is being set up — course_select_menu_act then
 * auto-drives the real CUP->COURSE->OK->launch flow with the course pre-locked,
 * instead of the human picking. */
bool net_menu_online_pending(void);

/** Host-locked course id for the pending online race, or -1 when not pending. */
s32 net_menu_online_course(void);

/** True only while an ONLINE race is in progress. The in-race netcode gates on
 * this so it never affects an offline race. */
bool net_menu_online_active(void);

/** This console's session node id (== its player index for lockstep), or -1 if
 * not in a session. Node 0 is the room creator/host. */
s32 net_menu_node_id(void);

/** Number of players in the room this frame (self + peers), from the roster. */
s32 net_menu_player_count(void);

/** Clear the pending flag once the OK-confirm launch has been fired, so a later
 * offline race isn't auto-advanced. */
void net_menu_online_clear(void);

/** Start-barrier handshake, called each frame while a player holds at the map
 * screen after picking a driver. Returns true once the whole room is ready and
 * the race should launch (host: all peers ready; joiner: GO received). */
bool net_online_barrier_ready(void);

/** Draw the "waiting for all players to start" popup over the map screen while
 * the barrier is waiting (no-op otherwise). Called from the render pass. */
void net_online_barrier_render(void);

#endif /* NET_MENU_H */
